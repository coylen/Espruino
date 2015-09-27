/*
5 * network_esp8266_board.c

 *
 *  Created on: Aug 29, 2015
 *      Author: kolban
 */

/**
 * This file contains the implementation of the ESP8266_BOARD network interfaces at the TCP/IP
 * level.
 *
 * Design notes
 * ------------
 * We maintain an array of socketData structures.  The number of such structures is defined in the
 * MAX_SOCKETS define.  The private variable that contains the array is called "socketArray".
 * Each one of these array instances represents a possible socket that we can use.
 *
 * Within the code, this allows us to reference a socket instance by an integer.  For example,
 * socket 0 is the 1st instance in the array.
 *
 * Associated with the array are accessor functions:
 *
 * o getNextFreeSocket - Return the next free socket or -1 if there are no free sockets.
 * o getSocketData - Get the socketData structure corresponding to the socket integer.
 * o resetSocket - Reset the state of the socket and clean it up if needed.
 * o releaseSocket - Release the socket and return it to the pool of unused sockets.
 *
 */
// ESP8266 specific includes
#include <c_types.h>
#include <user_interface.h>
#include <mem.h>
#include <osapi.h>
#include <espconn.h>
#include <espmissingincludes.h>

#define _GCC_WRAP_STDINT_H
typedef long long int64_t;

#include "network_esp8266.h"
#include "esp8266_board_utils.h"

/**
 * \brief The maximum number of concurrently open sockets we support.
 * We should probably pair this with the ESP8266 concept of the maximum number of sockets
 * that an ESP8266 instance can also support.
 */
#define MAX_SOCKETS (10)

#define LOG os_printf

static struct socketData *getSocketData(int s);

static int  getServerSocketByLocalPort(unsigned short port);
static void setSocketInError(int socketId, char *msg, int code);
static void dumpEspConn(struct espconn *pEspConn);
static int  getNextFreeSocket();
static void doClose(int socketId);
static void releaseSocket(int socketId);
static void resetSocket(int sckt);

static void esp8266_callback_connectCB_inbound(void *arg);
static void esp8266_callback_connectCB_outbound(void *arg);
static void esp8266_callback_disconnectCB(void *arg);
static void esp8266_callback_sentCB(void *arg);
static void esp8266_callback_writeFinishedCB(void *arg);
static void esp8266_callback_recvCB(void *arg, char *pData, unsigned short len);
static void esp8266_callback_reconnectCB(void *arg, sint8 err);

/**
 * \brief A data structure that represents a memory buffer.
 * A memory buffer is an object that represents a sized piece of memory.  Given a
 * memory buffer object, we know how big it is and can set or get data from it.
 */
struct memoryBuffer {
	/**
	 * \brief The size of data associated with this buffer.
	 */
	size_t length;

	/**
	 * \brief A pointer to the memory associated with this buffer.  This should be
	 * NULL if `length` is 0.
	 */
	uint8 *buf;
};

static uint8 *memoryBuffer_read(struct memoryBuffer *pMemoryBuffer, size_t readSize);
static uint8 *memoryBuffer_append(struct memoryBuffer *pMemoryBuffer, uint8 *pNewData, size_t length);
static void   memoryBuffer_delete(struct memoryBuffer *pMemoryBuffer);
static int    memoryBuffer_getSize(struct memoryBuffer *pMemoryBuffer);

/**
 * \brief The potential states for a socket.
 * See the socket state diagram.
 */
enum SOCKET_STATE {
	SOCKET_STATE_UNUSED,        //!< The socket is unused
	SOCKET_STATE_IDLE,          //!< The socket is idle
	SOCKET_STATE_CONNECTING,    //!< The socket is connecting
	SOCKET_STATE_TRANSMITTING,  //!< The socket is transmitting
	SOCKET_STATE_CLOSING,       //!< The socket is closing
	SOCKET_STATE_ERROR          //!< The socket is in error
};

/**
 * void initSocketData()
 * int getNextFreeSocket()
 * void releaseSocket(int s)
 * struct socketData *getSocketData(int s)
 */



/**
 * Here are some notes on the accepted list algorithms.
 *
 * tail - Where old entries are removed
 * head - Where new entries are added
 *
 * When tail = head - the list is empty
 *
 * When a new entry is added:
 * error: mod(head+1) == tail  // List full
 * *head = new entry
 * head = mod(head+1)
 *
 * When an old entry is removed
 * error: tail == head // List empty
 * retrieved entry = *tail
 * tail = mod(tail+1)
 *
 *
 *  0  1  2  3  4  5  6  7  8
 * [ ][ ][ ][ ][ ][ ][ ][ ][ ]
 */

/**
 * \brief The maximum number of accepted sockets that can be remembered.
 */
#define MAX_ACCEPTED_SOCKETS (10)

/**
 * \brief The core socket structure.
 * The structure is initialized by resetSocket.
 */
struct socketData {
	int		socketId;			//!< The id of THIS socket.
	enum SOCKET_STATE state;	//!< What is the socket state?
	bool    isConnected;		//!< Is this socket connected?
	bool    isServer;			//!< Are we a server?
	bool	shouldClose;		//!< Should this socket close when it can?

	struct  espconn *pEspconn;	//!< The ESPConn structure.

	struct  memoryBuffer txMemoryBuffer; //!< Data to be transmitted.
	uint8  *currentTx;			//!< Data currently being transmitted.
	uint8  *rxBuf;				//!< Data received (inbound).
	size_t  rxBufLen;			//!< The length of data in the buffer ready for consumption.

	char   *errorMsg;           //!< Error message.
	int     errorCode;          //!< Error code.

	/**
	 * \brief A list of accepted sockets.
	 * This array contains the storage of a list of sockets that have been accepted by this
	 * server socket but have not yet been delivered to Espruino.  A `head` and `tail`
	 * pair of indices are also associated.
	 */
	int     acceptedSockets[MAX_ACCEPTED_SOCKETS];

	/**
	 * \brief The head of the list of accepted sockets.
	 * The index into `acceptedSockets` where the next accepted socket will be placed.
	 */
	uint8   acceptedSocketsHead;

	/**
	 * \brief The tail of the list of accepted sockets.
	 * The index into `acceptedSockets` where the next accepted socket will be retrieved.
	 */
	uint8   acceptedSocketsTail;

};


/**
 * \brief An array of socket data structures.
 */
static struct socketData socketArray[MAX_SOCKETS];


/**
 * \brief An array of `esp_tcp` data structures.
 */
static esp_tcp tcpArray[MAX_SOCKETS];


/**
 * \brief An array of `struct espconn` data structures.
 */
static struct espconn espconnArray[MAX_SOCKETS];


/**
 * \brief Write the details of a socket to the debug log.
 * The data associated with the socket is dumped to the debug log.
 */
void esp8266_dumpSocket(
		int socketId //!< The ID of the socket data structure to be logged.
	) {
	struct socketData *pSocketData = getSocketData(socketId);
	LOG("Dump of socket=%d\n", socketId);
	LOG(" - isConnected=%d", pSocketData->isConnected);
	LOG(", isServer=%d", pSocketData->isServer);
	LOG(", acceptedSockets=[");
	int s=pSocketData->acceptedSocketsTail;
	while(s != pSocketData->acceptedSocketsHead) {
		LOG(" %d", pSocketData->acceptedSockets[s]);
		s = (s+1)%MAX_ACCEPTED_SOCKETS;
	}
	LOG("]");
	LOG(", rxBufLen=%d", pSocketData->rxBufLen);
	LOG(", tx length=%d", memoryBuffer_getSize(&(pSocketData->txMemoryBuffer)));
	LOG(", currentTx=0x%d", (int)pSocketData->currentTx);
	char *stateMsg;
	switch(pSocketData->state) {
	case SOCKET_STATE_CLOSING:
		stateMsg = "SOCKET_STATE_CLOSING";
		break;
	case SOCKET_STATE_CONNECTING:
		stateMsg = "SOCKET_STATE_CONNECTING";
		break;
	case SOCKET_STATE_ERROR:
		stateMsg = "SOCKET_STATE_ERROR";
		break;
	case SOCKET_STATE_IDLE:
		stateMsg = "SOCKET_STATE_IDLE";
		break;
	case SOCKET_STATE_TRANSMITTING:
		stateMsg = "SOCKET_STATE_TRANSMITTING";
		break;
	case SOCKET_STATE_UNUSED:
		stateMsg = "SOCKET_STATE_UNUSED";
		break;
	default:
		stateMsg = "Unexpected state!!";
		break;
	}
	LOG(", state=%s", stateMsg);
	LOG(", errorCode=%d", pSocketData->errorCode);

	// Print the errorMsg if it has anything to say
	if (pSocketData->errorMsg != NULL && strlen(pSocketData->errorMsg) > 0) {
		LOG(", errorMsg=\"%s\"", pSocketData->errorMsg);
	}

	LOG("\n");
} // End of dumpSocket


/**
 * \brief Dump a struct espconn (for debugging purposes).
 */
static void dumpEspConn(
		struct espconn *pEspConn //!<
	) {
	char ipString[20];
	LOG("Dump of espconn: 0x%x\n", (int)pEspConn);
	if (pEspConn == NULL) {
		return;
	}
	switch(pEspConn->type) {
	case ESPCONN_TCP:
		LOG(" - type = TCP\n");
		LOG("   - local address    = %d.%d.%d.%d [%d]\n",
				pEspConn->proto.tcp->local_ip[0],
				pEspConn->proto.tcp->local_ip[1],
				pEspConn->proto.tcp->local_ip[2],
				pEspConn->proto.tcp->local_ip[3],
				pEspConn->proto.tcp->local_port);
		LOG("   - remote address   = %d.%d.%d.%d [%d]\n",
				pEspConn->proto.tcp->remote_ip[0],
				pEspConn->proto.tcp->remote_ip[1],
				pEspConn->proto.tcp->remote_ip[2],
				pEspConn->proto.tcp->remote_ip[3],
				pEspConn->proto.tcp->remote_port);
		break;
	case ESPCONN_UDP:
		LOG(" - type = UDP\n");
		LOG("   - local_port  = %d\n", pEspConn->proto.udp->local_port);
		LOG("   - local_ip    = %d.%d.%d.%d\n",
				pEspConn->proto.tcp->local_ip[0],
				pEspConn->proto.tcp->local_ip[1],
				pEspConn->proto.tcp->local_ip[2],
				pEspConn->proto.tcp->local_ip[3]);
		LOG("   - remote_port = %d\n", pEspConn->proto.udp->remote_port);
		LOG("   - remote_ip   = %d.%d.%d.%d\n",
				pEspConn->proto.tcp->remote_ip[0],
				pEspConn->proto.tcp->remote_ip[1],
				pEspConn->proto.tcp->remote_ip[2],
				pEspConn->proto.tcp->remote_ip[3]);
		break;
	default:
		LOG(" - type = Unknown!! 0x%x\n", pEspConn->type);
	}
	switch(pEspConn->state) {
	case ESPCONN_NONE:
		LOG(" - state=NONE");
		break;
	case ESPCONN_WAIT:
		LOG(" - state=WAIT");
		break;
	case ESPCONN_LISTEN:
		LOG(" - state=LISTEN");
		break;
	case ESPCONN_CONNECT:
		LOG(" - state=CONNECT");
		break;
	case ESPCONN_WRITE:
		LOG(" - state=WRITE");
		break;
	case ESPCONN_READ:
		LOG(" - state=READ");
		break;
	case ESPCONN_CLOSE:
		LOG(" - state=CLOSE");
		break;
	default:
		LOG(" - state=unknown!!");
		break;
	}
	LOG(", link_cnt=%d", pEspConn->link_cnt);
	LOG(", reverse=0x%x\n", (unsigned int)pEspConn->reverse);
} // End of dumpEspConn


/**
 * \brief Get the next free socket.
 * Look for the first free socket in the array of sockets and return the first one
 * that is available after first flagging it as now in use.  If no available
 * socket can be found, return -1.
 */
static int getNextFreeSocket() {
	for (int i=0; i<MAX_SOCKETS; i++) {
		if (socketArray[i].state == SOCKET_STATE_UNUSED) {
			return i;
		}
	} // End of for each socket.
	return(-1);
} // End of getNextFreeSocket


/**
 * \brief Retrieve the socketData for the given socket index.
 */
static struct socketData *getSocketData(int s) {
	assert(s >=0 && s<MAX_SOCKETS);
	return &socketArray[s];
} // End of getSocketData


/**
 * \brief Find the server socket that is bound to the given local port.
 * \return The socket id of the socket listening on the given port or -1 if there is no
 * server socket that matches.
 */
static int getServerSocketByLocalPort(
		unsigned short port //!< The port number on which a server socket is listening.
	) {
	// Loop through each of the sockets in the socket array looking for a socket
	// that is inuse, a server and has a local_port of the passed in port number.
	int socketArrayIndex;
	for (socketArrayIndex=0; socketArrayIndex<MAX_SOCKETS; socketArrayIndex++) {
		struct socketData *pSocketData = socketArray;
		if (pSocketData->state != SOCKET_STATE_UNUSED &&
			pSocketData->isServer == true &&
			pSocketData->pEspconn->proto.tcp->local_port == port)
		{
			return socketArrayIndex;
		}
		pSocketData++;
	} // End of for each socket
	return -1;
} // End of getServerSocketByLocalPort


/**
 * \brief Reset the socket to its clean and unused state.
 */
static void resetSocket(
		int sckt //!<
	) {
	struct socketData *pSocketData = getSocketData(sckt);

	memoryBuffer_delete(&pSocketData->txMemoryBuffer);

	pSocketData->pEspconn       = &espconnArray[sckt];
	pSocketData->state			= SOCKET_STATE_UNUSED;
	pSocketData->rxBuf          = NULL;
	pSocketData->rxBufLen       = 0;
	pSocketData->isServer       = false;
	pSocketData->isConnected    = false;
	pSocketData->shouldClose    = false;
	pSocketData->socketId       = sckt;
	pSocketData->errorMsg       = "";
	pSocketData->errorCode      = 0;

	pSocketData->acceptedSocketsHead = 0; // Set the head to 0
	pSocketData->acceptedSocketsTail = 0; // Set the tail to 9.

	struct espconn *pEspconn    = pSocketData->pEspconn;
	pEspconn->type              = ESPCONN_TCP;
	pEspconn->state             = ESPCONN_NONE;
	pEspconn->proto.tcp         = &tcpArray[sckt];
	pEspconn->reverse           = NULL;


	espconn_regist_connectcb(pEspconn, esp8266_callback_connectCB_outbound);
	espconn_regist_disconcb(pEspconn, esp8266_callback_disconnectCB);
	espconn_regist_reconcb(pEspconn, esp8266_callback_reconnectCB);
	espconn_regist_sentcb(pEspconn, esp8266_callback_sentCB);
	espconn_regist_recvcb(pEspconn, esp8266_callback_recvCB);
	espconn_regist_write_finish(pEspconn, esp8266_callback_writeFinishedCB);
} // End of resetSocket


/**
 * \brief Release the socket and return it to the free pool.
 */
static void releaseSocket(
		int socketId //!< The socket id of the socket to be released.
	) {
	os_printf("> releaseSocket: %d\n", socketId);
	assert(socketId >=0 && socketId<MAX_SOCKETS);
	esp8266_dumpSocket(socketId);

	struct socketData *pSocketData = getSocketData(socketId);
	assert(pSocketData->state != SOCKET_STATE_UNUSED);

	if (memoryBuffer_getSize(&pSocketData->txMemoryBuffer) > 0) {
		os_printf(" - Oh oh ... attempt to close socket while the TX memoryBuffer is not empty!\n");
	}
	if (pSocketData->rxBuf != NULL || pSocketData->rxBufLen != 0) {
		os_printf(" - Oh oh ... attempt to close socket while the rxBuffer is not empty!\n");
	}
	resetSocket(socketId);
	os_printf("< releaseSocket\n");
} // End of releaseSocket


/**
 * \brief Initialize the ESP8266_BOARD environment.
 * Walk through each of the sockets and initialize each one.
 */
void netInit_esp8266_board() {
	for (int socketArrayIndex=0; socketArrayIndex<MAX_SOCKETS; socketArrayIndex++) {
		resetSocket(socketArrayIndex);
	} // End of for each socket
} // netInit_esp8266_board


/**
 * \brief Perform an actual closure of the socket by calling the ESP8266 disconnect API.
 * This is broken out into its own function because this can happen in
 * a number of possible places.
 */
static void doClose(
		int socketId //!< The socket id to be closed.
	) {
	struct socketData *pSocketData = getSocketData(socketId);

	if (pSocketData->state != SOCKET_STATE_CLOSING) {
		int rc = espconn_disconnect(pSocketData->pEspconn);
		pSocketData->state = SOCKET_STATE_CLOSING;

		if (rc != 0) {
			os_printf("espconn_disconnect: rc=%d\n", rc);
			setSocketInError(socketId, "espconn_disconnect", rc);
		}
	} else {
		releaseSocket(socketId);
	}
} // End of doClose


/**
 * \brief Set the given socket as being in error supplying a message and a code.
 * The socket state is placed in `SOCKET_STATE_ERROR`.
 */
static void setSocketInError(
		int socketId, //!< The socket id that is being flagged as in error.
		char *msg,    //!< A message to associate with the error.
		int code      //!< A low level error code.
	) {
	struct socketData *pSocketData = getSocketData(socketId);
	pSocketData->state     = SOCKET_STATE_ERROR;
	pSocketData->errorMsg  = msg;
	pSocketData->errorCode = code;
} // End of setSocketInError

/**
 * \brief Callback function registered to the ESP8266 environment that is
 * invoked when a new inbound connection has been formed.
 * A new connection
 * can occur when the ESP8266 makes a call out to a partner (in that
 * case the ESP8266 is acting as a client) or a new connection can
 * occur when a partner calls into a listening ESP8266.  In that case
 * the ESP8266 is acting as a server.
 */
static void esp8266_callback_connectCB_inbound(
		void *arg //!<
	) {
	os_printf(">> connectCB_inbound\n");
	struct espconn *pEspconn = (struct espconn *)arg;
	assert(pEspconn != NULL);

	dumpEspConn(pEspconn);

	int s = getServerSocketByLocalPort(pEspconn->proto.tcp->local_port);
	assert(s != -1);
	struct socketData *pSocketData = getSocketData(s);
	assert(pSocketData != NULL);

	esp8266_dumpSocket(pSocketData->socketId);

	os_printf("** new client has connected to us **\n");

	if ((pSocketData->acceptedSocketsHead + 1) % MAX_ACCEPTED_SOCKETS == pSocketData->acceptedSocketsTail) {
		os_printf("WARNING!! - Discarding inbound client because we have too many accepted clients.\n");
		os_printf("<< connectCB_inbound\n");
		return;
	}

	int clientSocket = getNextFreeSocket();
	if (clientSocket < 0) {
		os_printf("!!! Ran out of sockets !!!\n");
		return;
	}
	struct socketData *pClientSocketData = getSocketData(clientSocket);
	assert(pClientSocketData != NULL);
	pClientSocketData->pEspconn          = pEspconn;
	pClientSocketData->pEspconn->reverse = pClientSocketData;
	pClientSocketData->isServer          = false;
	pClientSocketData->isConnected       = true;
	pClientSocketData->socketId          = clientSocket;
	pClientSocketData->state             = SOCKET_STATE_IDLE;

	pSocketData->acceptedSockets[pSocketData->acceptedSocketsHead] = clientSocket;
	pSocketData->acceptedSocketsHead = (pSocketData->acceptedSocketsHead + 1) % MAX_ACCEPTED_SOCKETS;

	os_printf("<< connectCB_inbound\n");
} // End of esp8266_callback_connectCB_inbound

/**
 * \brief Callback function registered to the ESP8266 environment that is
 * invoked when a new outbound connection has been formed.
 */
static void esp8266_callback_connectCB_outbound(
		void *arg //!< A pointer to a `struct espconn`.
	) {
	os_printf(">> connectCB_outbound\n");
	struct espconn *pEspconn = (struct espconn *)arg;
	assert(pEspconn != NULL);

	dumpEspConn(pEspconn);

	struct socketData *pSocketData = (struct socketData *)pEspconn->reverse;
	assert(pSocketData != NULL);

	esp8266_dumpSocket(pSocketData->socketId);

	// Flag the socket as connected to a partner.
	pSocketData->isConnected = true;

	assert(pSocketData->state == SOCKET_STATE_CONNECTING);
	if (pSocketData->shouldClose) {
		doClose(pSocketData->socketId);
	} else {
		pSocketData->state = SOCKET_STATE_IDLE;
	}
	os_printf("<< connectCB_outbound\n");
} // End of esp8266_callback_connectCB_outbound


/**
 * \brief Callback function registered to the ESP8266 environment that is
 * Invoked when a previous connection has been disconnected.
 */
static void esp8266_callback_disconnectCB(
		void *arg //!< A pointer to a `struct espconn`.
	) {
	struct espconn *pEspconn = (struct espconn *)arg;
	struct socketData *pSocketData = (struct socketData *)pEspconn->reverse;
	assert(pSocketData != NULL);
	assert(pSocketData->state != SOCKET_STATE_UNUSED);

	os_printf(">> disconnectCB\n");
	dumpEspConn(pEspconn);
	esp8266_dumpSocket(pSocketData->socketId);

	// If the socket state is SOCKET_STATE_CLOSING then that means we can release the socket.  The reason
	// for this is that the last thing the user did was request an explicit socket close.
	if (pSocketData->state == SOCKET_STATE_CLOSING) {
		releaseSocket(pSocketData->socketId);
	} else {
		pSocketData->state       = SOCKET_STATE_CLOSING;
		pSocketData->isConnected = false;
	}
	os_printf("<< disconnectCB\n");
} // End of disconnectCB


/**
 *
 */
static void esp8266_callback_writeFinishedCB(
		void *arg //!< A pointer to a `struct espconn`.
	) {
	os_printf(">> writeFinishedCB\n");
	struct espconn *pEspconn = (struct espconn *)arg;
	struct socketData *pSocketData = (struct socketData *)pEspconn->reverse;
	if (pSocketData->currentTx != NULL) {
		os_free(pSocketData->currentTx);
		pSocketData->currentTx = NULL;
	}
	os_printf("<< writeFinishedCB\n");
} // End of writeFinishedCB


/**
 * \brief Error handler callback.
 * Although this is called reconnect by Espressif, this is really an error handler
 * routine.  It will be called when an error is detected.
 */
static void esp8266_callback_reconnectCB(
		void *arg, //!< A pointer to a `struct espconn`.
		sint8 err  //!< The error code.
	) {
	os_printf(">> reconnectCB:  Error code is: %d - %s\n", err, esp8266_errorToString(err));
	os_printf("<< reconnectCB");
} // End of reconnectCB


/**
 * \brief Callback function registered to the ESP8266 environment that is
 * invoked when a send operation has been completed.
 */
static void esp8266_callback_sentCB(
		void *arg //!< A pointer to a `struct espconn`.
	) {
	os_printf(">> sendCB\n");
	struct espconn *pEspconn = (struct espconn *)arg;
	struct socketData *pSocketData = (struct socketData *)pEspconn->reverse;

	assert(pSocketData != NULL);
	assert(pSocketData->state == SOCKET_STATE_TRANSMITTING);

	// We have transmitted the data ... which means that the data that was in the transmission
	// buffer can be released.
	if (pSocketData->currentTx != NULL) {
		os_free(pSocketData->currentTx);
		pSocketData->currentTx = NULL;
	}

	if (pSocketData->shouldClose) {
		doClose(pSocketData->socketId);
	} else {
		pSocketData->state = SOCKET_STATE_IDLE;
	}
	os_printf("<< sendCB\n");
} // End of sentCB


/**
 * \brief ESP8266 callback function that is invoked when new data has arrived over
 * the TCP/IP connection.
 */
static void esp8266_callback_recvCB(
		void *arg,         //!< A pointer to a `struct espconn`.
		char *pData,       //!< A pointer to data received over the socket.
		unsigned short len //!< The length of the data.
	) {
	struct espconn *pEspconn = (struct espconn *)arg;
	struct socketData *pSocketData = (struct socketData *)pEspconn->reverse;

	assert(pSocketData != NULL);
	assert(pSocketData->state != SOCKET_STATE_UNUSED);

	os_printf(">> recvCB for socket=%d, length=%d\n", pSocketData->socketId, len);

	// If we don't have any existing unconsumed data then malloc some storage and
	// copy the received data into that storage.
	if (pSocketData->rxBufLen == 0) {
		pSocketData->rxBuf = (void *)os_malloc(len);
		memcpy(pSocketData->rxBuf, pData, len);
		pSocketData->rxBufLen = len;
	} else {
// Allocate a new buffer big enough for the original data and the new data
// Copy the original data to the start of the new buffer ...
// Copy the new new data to the offset into the new buffer just after
//   the original data.
// Release the original data.
// Update the socket data.
		uint8 *pNewBuf = (uint8 *)os_malloc(len + pSocketData->rxBufLen);
		memcpy(pNewBuf, pSocketData->rxBuf, pSocketData->rxBufLen);
		memcpy(pNewBuf + pSocketData->rxBufLen, pData, len);
		os_free(pSocketData->rxBuf);
		pSocketData->rxBuf = pNewBuf;
		pSocketData->rxBufLen += len;
	} // End of new data allocated.
	dumpEspConn(pEspconn);
	os_printf("<< recvCB\n");

} // End of recvCB


// -------------------------------------------------

/**
 * \brief Define the implementation functions for the logical network functions.
 */
void netSetCallbacks_esp8266_board(
		JsNetwork *net //!< The Network we are going to use.
	) {
	  net->idle          = net_ESP8266_BOARD_idle;
	  net->checkError    = net_ESP8266_BOARD_checkError;
	  net->createsocket  = net_ESP8266_BOARD_createSocket;
	  net->closesocket   = net_ESP8266_BOARD_closeSocket;
	  net->accept        = net_ESP8266_BOARD_accept;
	  net->gethostbyname = net_ESP8266_BOARD_gethostbyname;
	  net->recv          = net_ESP8266_BOARD_recv;
	  net->send          = net_ESP8266_BOARD_send;
} // End of netSetCallbacks_esp8266_board


/**
 * \brief Determine if there is a new client connection on the server socket.
 * This function is called to poll to see if the serverSckt has a new
 * accepted connection (socket) and, if it does, return it else return -1 to indicate
 * that there was no new accepted socket.
 */
int net_ESP8266_BOARD_accept(
		JsNetwork *net, //!< The Network we are going to use to create the socket.
		int serverSckt  //!< The socket that we are checking to see if there is a new client connection.
	) {
	//os_printf("> net_ESP8266_BOARD_accept\n");
	struct socketData *pSocketData = getSocketData(serverSckt);
	assert(pSocketData->state != SOCKET_STATE_UNUSED);
	assert(pSocketData->isServer == true);

	// If the list is empty, return.
	if (pSocketData->acceptedSocketsHead == pSocketData->acceptedSocketsTail) {
		// Return -1 if there is no new client socket for this server.
		return -1;
	}

	// Return the 1st socket id that is in the list of accepted sockets.  We also update the
	// list to indicate that it has been read.
	int acceptedSocketId = pSocketData->acceptedSockets[pSocketData->acceptedSocketsTail];
	pSocketData->acceptedSocketsTail = (pSocketData->acceptedSocketsTail + 1) % MAX_ACCEPTED_SOCKETS;

	os_printf("> net_ESP8266_BOARD_accept: Accepted a new socket, socketId=%d\n", acceptedSocketId);
	return acceptedSocketId;
} // End of net_ESP8266_BOARD_accept


/**
 * \brief Receive data from the network device.
 * Returns the number of bytes received which may be 0 and -1 if there was an error.
 */
int net_ESP8266_BOARD_recv(
		JsNetwork *net, //!< The Network we are going to use to create the socket.
		int sckt,       //!< The socket from which we are to receive data.
		void *buf,      //!< The storage buffer into which we will receive data.
		size_t len      //!< The length of the buffer.
	) {
	assert(sckt >=0 && sckt<MAX_SOCKETS);
	struct socketData *pSocketData = getSocketData(sckt);
	assert(pSocketData->state != SOCKET_STATE_UNUSED);

	// If there is no data in the receive buffer, then all we need do is return
	// 0 bytes as the length of data moved.
	if (pSocketData->rxBufLen == 0) {
		if (pSocketData->state == SOCKET_STATE_CLOSING) {
			return -1;
		}
		return 0;
	}

	// If the receive buffer contains data and is it is able to fit in the buffer
	// passed into us then we can copy all the data and the receive buffer will be clear.
	if (pSocketData->rxBufLen <= len) {
		memcpy(buf, pSocketData->rxBuf, pSocketData->rxBufLen);
		int retLen = pSocketData->rxBufLen;
		os_free(pSocketData->rxBuf);
		pSocketData->rxBufLen = 0;
		pSocketData->rxBuf = NULL;
		return retLen;
	}

	// If we are here, then we have more data in the receive buffer than is available
	// to be returned in this request for data.  So we have to copy the amount of data
	// that is allowed to be returned and then strip that from the beginning of the
	// receive buffer.

	// First we copy the data we are going to return.
	memcpy(buf, pSocketData->rxBuf, len);

	// Next we allocate a new buffer and copy in the data we are not returning.
	uint8 *pNewBuf = (uint8 *)os_malloc(pSocketData->rxBufLen-len);
	memcpy(pNewBuf, pSocketData->rxBuf + len, pSocketData->rxBufLen-len);

	// Now we juggle pointers and release the original RX buffer now that we have a new
	// one.  It is likely that this algorithm can be significantly improved since there
	// is a period of time where we might actuall have TWO copies of the data.
	uint8 *pTemp = pSocketData->rxBuf;
	pSocketData->rxBuf = pNewBuf;
	pSocketData->rxBufLen = pSocketData->rxBufLen-len;
	os_free(pTemp);

	return len;
} // End of net_ESP8266_BOARD_recv.


/**
 * \brief Send data to the partner.
 * The return is the number of bytes actually transmitted which may also be
 * 0 to indicate no bytes sent or -1 to indicate an error.  For the ESP8266 implementation we
 * will return 0 if the socket is not connected or we are in the `SOCKET_STATE_TRANSMITTING`
 * state.
 */
int net_ESP8266_BOARD_send(
		JsNetwork *net,  //!< The Network we are going to use to create the socket.
		int sckt,        //!< The socket over which we will send data.
		const void *buf, //!< The buffer containing the data to be sent.
		size_t len       //!< The length of data in the buffer to send.
	) {
	assert(sckt >=0 && sckt<MAX_SOCKETS);
	os_printf("> net_ESP8266_BOARD_send: Request to send data to socket %d of size %d: ", sckt, len);

	struct socketData *pSocketData = getSocketData(sckt);
	assert(pSocketData->state != SOCKET_STATE_UNUSED);

	// If we are not connected, then we can't currently send data.
	if (pSocketData->isConnected == false) {
		os_printf(" - Not connected\n");
		return 0;
	}

	// If we are currently sending data, we can't send more.
	if (pSocketData->state == SOCKET_STATE_TRANSMITTING) {
		os_printf(" - Currently transmitting\n");
		return 0;
	}

	// Log the content of the data we are sening.
	esp8266_board_writeString(buf, len);

	os_printf("\n");

	assert(pSocketData->state == SOCKET_STATE_IDLE);

	pSocketData->state = SOCKET_STATE_TRANSMITTING;

	// Copy the data that was passed to us to a private area.  We do this because we must not
	// assume that the data passed in will be available after this function returns.  It may have
	// been passed in on the stack.
	assert(pSocketData->currentTx == NULL);
	pSocketData->currentTx = (uint8_t *)os_malloc(len);
	memcpy(pSocketData->currentTx, buf, len);

	// Send the data over the ESP8266 SDK.
	int rc = espconn_send(pSocketData->pEspconn, pSocketData->currentTx, len);
	if (rc < 0) {
		setSocketInError(sckt, "espconn_send", rc);
		os_free(pSocketData->currentTx);
		pSocketData->currentTx = NULL;
		return -1;
	}

	esp8266_dumpSocket(sckt);
	os_printf("< net_ESP8266_BOARD_send\n");
	return len;
} // End of net_ESP8266_BOARD_send


/**
 * \brief Perform idle processing.
 * There is the possibility that we may wish to perform logic when we are idle.  For the
 * ESP8266 there is no specific idle network processing needed.
 */
void net_ESP8266_BOARD_idle(
		JsNetwork *net //!< The Network we are part of.
	) {
	// Don't echo here because it is called continuously
	//os_printf("> net_ESP8266_BOARD_idle\n");
} // End of net_ESP8266_BOARD_idle


/**
 * \brief Check for errors.
 * Returns true if there are NO errors.
 */
bool net_ESP8266_BOARD_checkError(
		JsNetwork *net //!< The Network we are going to use to create the socket.
	) {
	//os_printf("> net_ESP8266_BOARD_checkError\n");
	return true;
} // End of net_ESP8266_BOARD_checkError


/**
 * \brief Create a new socket.
 * if `ipAddress == 0`, creates a server otherwise creates a client (and automatically connects). Returns >=0 on success.
 */
int net_ESP8266_BOARD_createSocket(
		JsNetwork *net,     //!< The Network we are going to use to create the socket.
		uint32_t ipAddress, //!< The address of the partner of the socket or 0 if we are to be a server.
		unsigned short port //!< The port number that the partner is listening upon.
	) {
	os_printf("> net_ESP8266_BOARD_createSocket: host: %d.%d.%d.%d, port:%d \n", ((char *)(&ipAddress))[0], ((char *)(&ipAddress))[1], ((char *)(&ipAddress))[2], ((char *)(&ipAddress))[3], port);

	bool isServer = (ipAddress == 0);

	int sckt = getNextFreeSocket();
	if (sckt < 0) { // No free socket
		os_printf("< net_ESP8266_BOARD_createSocket: No free sockets\n");
		return -1;
	}

	struct socketData *pSocketData = getSocketData(sckt);
	struct espconn *pEspconn = pSocketData->pEspconn;

	espconn_regist_disconcb(pEspconn, esp8266_callback_disconnectCB);
	espconn_regist_reconcb(pEspconn, esp8266_callback_reconnectCB);
	espconn_regist_sentcb(pEspconn, esp8266_callback_sentCB);
	espconn_regist_recvcb(pEspconn, esp8266_callback_recvCB);
	espconn_regist_write_finish(pEspconn, esp8266_callback_writeFinishedCB);

	struct ip_info ipconfig;
	wifi_get_ip_info(STATION_IF, &ipconfig); // Get the local IP address
	os_memcpy(pEspconn->proto.tcp->local_ip, &ipconfig.ip, 4);
	pEspconn->type      = ESPCONN_TCP;
	pEspconn->state     = ESPCONN_NONE;
	pEspconn->proto.tcp = &tcpArray[sckt];
	pEspconn->reverse   = pSocketData;

	// If ipAddress != 0 then make a client connection
	if (isServer == false) {
		pSocketData->state = SOCKET_STATE_CONNECTING;
		pSocketData->isServer = false;
		pEspconn->proto.tcp->remote_port = port;
		pEspconn->proto.tcp->local_port  = espconn_port();

		*(uint32 *)(pEspconn->proto.tcp->remote_ip) = ipAddress;

		// Ensure that we have flagged this socket as NOT connected
		pSocketData->isConnected = false;

		espconn_regist_connectcb(pEspconn, esp8266_callback_connectCB_outbound);

		// Make a call to espconn_connect.
		int rc = espconn_connect(pEspconn);
		if (rc != 0) {
			os_printf("Err: net_ESP8266_BOARD_createSocket -> espconn_connect returned: %d.  Using local port: %d\n", rc, pEspconn->proto.tcp->local_port);
			setSocketInError(sckt, "espconn_connect", rc);
		}
		/*
		os_printf("Checking to see if we are connected ...\n");
		uint32 clock= system_get_time();
		while(pSocketData->isConnected == false) {
			system_soft_wdt_feed();
			if ((system_get_time() - clock) > 10000000) {
				os_printf("clock %d\n", (unit32)clock);
				clock = system_get_time();
			}
		}
		*/
	}
	// If the ipAddress IS 0 ... then we are a server.
	else
	{
		pSocketData->state = SOCKET_STATE_IDLE;
		pSocketData->isServer = true;
		// We are going to set ourselves up as a server
		pEspconn->proto.tcp->local_port = port;

		espconn_regist_connectcb(pEspconn, esp8266_callback_connectCB_inbound);

		// Make a call to espconn_accept
		int rc = espconn_accept(pEspconn);
		if (rc != 0) {
			os_printf("Err: net_ESP8266_BOARD_createSocket -> espconn_accept returned: %d.  Using local port: %d\n", rc, pEspconn->proto.tcp->local_port);
			setSocketInError(sckt, "espconn_accept", rc);
		}
	} // End of

	dumpEspConn(pEspconn);
	os_printf("< net_ESP8266_BOARD_createSocket, socket=%d\n", sckt);
	return sckt;
} // End of net_ESP8266_BOARD_createSocket


/**
 * \brief Close a socket.
 */
void net_ESP8266_BOARD_closeSocket(
		JsNetwork *net, //!< The Network we are going to use to create the socket.
		int socketId    //!< The socket to be closed.
	) {
	os_printf("> net_ESP8266_BOARD_closeSocket, socket=%d\n", socketId);

	assert(socketId >=0 && socketId<MAX_SOCKETS);

	struct socketData *pSocketData = getSocketData(socketId);

	assert(pSocketData != NULL);
	assert(pSocketData->state != SOCKET_STATE_UNUSED);  // Shouldn't be closing an unused socket.

	dumpEspConn(pSocketData->pEspconn);
	esp8266_dumpSocket(socketId);

	// How we close the socket is a function of what kind of socket it is.
	if (pSocketData->isServer == true) {
		int rc = espconn_delete(pSocketData->pEspconn);
		if (rc != 0) {
			os_printf("espconn_delete: rc=%d\n", rc);
		}
	} // End this is a server socket
	else
	{
		if (pSocketData->state == SOCKET_STATE_IDLE || pSocketData->state == SOCKET_STATE_CLOSING) {
			doClose(socketId);
		} else {
			pSocketData->shouldClose = true;
		}
	} // End this is a client socket
} // End of net_ESP8266_BOARD_closeSocket


/**
 * \brief Get an IP address from a name.
 * Sets 'outIp' to 0 on failure.
 */
void net_ESP8266_BOARD_gethostbyname(
		JsNetwork *net, //!< The Network we are going to use to create the socket.
		char *hostName, //!< The string representing the hostname we wish to lookup.
		uint32_t *outIp //!< The address into which the resolved IP address will be stored.
	) {
	os_printf("> net_ESP8266_BOARD_gethostbyname\n");
	*outIp = 0x00000000;
} // End of net_ESP8266_BOARD_gethostbyname


// ----------------------------------------------------------------

/**
 * The following section is all about a logical concept called a memoryBuffer.  This is an
 * abstract data type that contains data in memory.  The operations we can perform upon it
 * are:

 * o memoryBuffer_append  - Append data to the end of the memory buffer.
 * o memoryBuffer_read    - Read a fixed number of bytes from the memory buffer.
 * o memoryBuffer_delete  - Delete the memory buffer.  No further operations should be performed
 *                          against it.
 * o memoryBuffer_getSize - Get the size of data contained in the memory buffer.
 */

/**
 * \brief Delete all content of the memory buffer.
 */
static void memoryBuffer_delete(
		struct memoryBuffer *pMemoryBuffer //!<
	) {
	if (pMemoryBuffer->length > 0) {
		os_free(pMemoryBuffer->buf);
		pMemoryBuffer->buf = NULL;
		pMemoryBuffer->length = 0;
	}
} // End of memoryBuffer_delete


/**
 * \brief Append new data to the end of the existing memory buffer.
 */
static uint8 *memoryBuffer_append(
		struct memoryBuffer *pMemoryBuffer, //!<
		uint8 *pNewData,                    //!<
		size_t length                       //!<
	) {
	assert(pMemoryBuffer != NULL);

	if (length == 0) {
		return pMemoryBuffer->buf;
	}

	assert(pNewData != NULL);

	// Handle the memory buffer being empty.
	if (pMemoryBuffer->length == 0) {
		pMemoryBuffer->buf = (uint8 *)os_malloc(length);
		if (pMemoryBuffer->buf == NULL) { // Out of memory
			jsError("malloc failed at memoryBuffer_append trying to allocate %d", length);
		} else {
			memcpy(pMemoryBuffer->buf, pNewData, length);
			pMemoryBuffer->length = length;
		}
	} else {
		// The memory buffer was not empty, so we append data.
		int newSize = pMemoryBuffer->length + length;
		uint8 *resizedStorage = (uint8 *)os_realloc(pMemoryBuffer->buf, newSize);
		if (resizedStorage != NULL) {
			pMemoryBuffer->buf = resizedStorage;
			memcpy(pMemoryBuffer->buf + length, pNewData, length);
			pMemoryBuffer->length = newSize;
		}
	}
	return pMemoryBuffer->buf;
} // End of memoryBuffer_append


/**
 * \brief Return how much data is stored in the memory buffer.
 */
static int memoryBuffer_getSize(
		struct memoryBuffer *pMemoryBuffer //!<
	) {
	assert(pMemoryBuffer != NULL);
	return pMemoryBuffer->length;
} // End of memoryBuffer_getSize


/**
 * \brief Read data from the memory buffer of an exact size.
 * The data that is returned
 * should be released with an os_free() call.
 */
static uint8 *memoryBuffer_read(
		struct memoryBuffer *pMemoryBuffer, //!<
		size_t readSize                     //!<
	) {
	assert(pMemoryBuffer != NULL);
	assert((pMemoryBuffer->length > 0 && pMemoryBuffer->buf != NULL) || pMemoryBuffer->length == 0);

	// Check that we are NOT trying to read more data than we actually have available to us.
	assert(readSize > pMemoryBuffer->length);

	// Handle the case where we are trying to read 0 bytes.
	if (readSize == 0) {
		return NULL;
	}

	// If the size of data we are willing to read is EXACTLY the size of the buffer we
	// have, then simply return a pointer to the buffer and we are done.
	if (readSize == pMemoryBuffer->length) {
		uint8 *pTemp = pMemoryBuffer->buf;
		pMemoryBuffer->buf = NULL;
		pMemoryBuffer->length = 0;
		return pTemp;
	}

	// We can assert that size < memory buffer length.
	//
	// Here we have determined that we wish to consume LESS data that we have available.
	// That means we have to split our data into parts.

	// First we build the data that we will return and copy in the memory buffer data.
	uint8 *pTemp = (uint8 *)os_malloc(readSize);
	if (pTemp == NULL) { // Out of memory
		jsError("malloc failed at memoryBuffer_append trying to allocate %d", readSize);
		return NULL;
	}
	os_memcpy(pTemp, pMemoryBuffer->buf, readSize);

	// Now we create a memory buffer to hold the remaining data that was not
	// returned.
	int newSize = pMemoryBuffer->length - readSize;
	uint8 *pTemp2 = (uint8 *)os_malloc(newSize);
	os_memcpy(pTemp2, pMemoryBuffer->buf + readSize, newSize);
	os_free(pMemoryBuffer->buf);
	pMemoryBuffer->buf = pTemp2;
	pMemoryBuffer->length = newSize;
	return pTemp;
} // End of memoryBuffer_read
// End of file
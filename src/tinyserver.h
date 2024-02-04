#ifndef TINYSERVER_H
//===========================================================================
// tinyserver.h
//
// Framework for building server applications. The building blocks of the
// code are platform-agnostic, thread-safe, and are made simple so that
// programmers can add server capabilities quickly to existing applications,
// or building new ones without being hoggled by the server part.
//
// The blueprint for building with it is:
//   1) Call InitServer().
//   2) Call AddListeningSocket() for each protocol and port to listen on.
//   3) Create at least one IO thread with an io loop.
//   4) Start a listening loop (can be on the main process thread, or a
//      thread specifically for it).
//
// The listening loop:
//   1) Call ListenForConnections().
//   2) Create a new ts_io object upon connection established.
//   3) Call AcceptConn() with the returned ts_listen object and the created
//      ts_io object. The result will be posted to the IO thread.
//   4) Repeat from step #1.
//
// The io loop:
//   1) Call WaitOnIoQueue().
//   2) Check the received ts_io object for connection status [.Status].
//      If the status is Status_Aborted, call DisconnectSocket; if it is
//      Status_Error, call TerminateConn. The ts_io object can be reused
//      for further AcceptConn calls.
//   3) If Status_Connected, perform RecvPacket, SendPacket, or SendFile.
//      Each operation will be posted again to WaitOnIoQueue, and may or may
//      not complete upon dequeue. Check [.BytesReceived] how much IO was
//      performed; adjust [.IoBuffer] and [.IoSize] to post again if needed.
//   4) Repeat from #1.
//===========================================================================
#define TINYSERVER_H

#include "tinybase-platform.h"
#include "tinybase-queues.h"


//==============================
// Structs and defines
//==============================

typedef enum ts_protocol
{
    Proto_TCPIP4    = 0,
    Proto_UDPIP4    = 1,
    Proto_TCPIP6    = 2,
    Proto_UDPIP6    = 3
} ts_protocol;

typedef enum ts_status
{
    Status_None,
    Status_Disconnected,
    Status_Connected,
    Status_Aborted,
    Status_Error
} ts_status;

typedef enum ts_op
{
    Op_None,
    Op_AcceptConn,
    Op_CreateConn,
    Op_DisconnectSocket,
    Op_TerminateConn,
    Op_RecvPacket,
    Op_SendPacket,
    Op_SendFile,
    Op_SendToIoQueue
} ts_op;

#define MAX_SOCKADDR_SIZE 28 // Enough for the largest sockaddr struct.

typedef struct ts_sockaddr
{
    u8 Addr[MAX_SOCKADDR_SIZE];
    u32 Size;
} ts_sockaddr;

typedef struct ts_listen
{
    file Socket;
    file Event;
    ts_protocol Protocol;
    i32 SockAddrSize;
} ts_listen;

#if defined(TT_WINDOWS)
# define TS_INTERNAL_DATA_SIZE 48 // See tinyserver-win32.c for more info.
#elif defined(TT_LINUX)
# define TS_INTERNAL_DATA_SIZE 4  // See tinyserver-epoll.c for more info.
#endif

typedef struct ts_io
{
    file Socket;
    usz BytesTransferred;
    ts_status Status;
    ts_op Operation;
    
    union
    {
        u8* IoBuffer;
        file IoFile;
    };
    u32 IoSize;
    u8 InternalData[TS_INTERNAL_DATA_SIZE];
} ts_io;


//==============================
// Setup
//==============================

external bool InitServer(void);

/* Must be called only once, before anything else. Sets up platform-dependent parts,
|  as well as initializing working buffers.
|--- Return: true if successful, false if not. */

external void CloseServer(void);

/* Performs server cleanup and freeing of resources.
--- Returns: nothing. */

external ts_sockaddr CreateSockAddr(char* IpAddress, u16 Port, ts_protocol Protocol);

/* Creates and populates a sockaddr struct for the desired [Protocol], with a
 |  specified [IpAddress] and [Port]. Meant to be passed as-is to CreateConn().
|--- Return: filled ts_sockaddr struct. */

external bool AddListeningSocket(ts_protocol Protocol, u16 Port);

/* Creates a new socket for the defined [Protocol], binds it to the specified [Port]
 |  number and sets it up for listening. The socket is added to an internal structure
 |  that keeps track of listening sockets.
|--- Return: true if successful, false if not. */


//==============================
// Async events
//==============================

ts_listen ListenForConnections(void);

/* Polls the added listening sockets to check for connections. Waits indefinitely
 |  until any of the sockets gets signaled. If more than one socket gets signaled at
 |  the same time, returns the first one, and the next ones upon subsequent calls to
 |  this function.
|--- Return: ts_listen struct, to be passed to AcceptConn(). If this function fails,
|            the [.Socket] member will be of value INVALID_FILE. */

ts_io* WaitOnIoQueue(void);

/* Waits indefinitely on the IO queue until an event completes. If more than one
 |  event gets dequeued at the same time, returns the first one, and the next ones
 |  upon subsequent calls to this function.
|--- Return: pointer to the ts_io connection returned, with the field [.Status]
|            indicating if the connection is still standing or has been aborted, and
 |            [.BytesTransferred] updated to that of the latest transaction. */

bool SendToIoQueue(ts_io* Conn);

/* Sends the socket in [Conn] back to the [IoQueue]. No bytes are transferred
 |  during the operation.
|--- Return: true if successful, false if not. */


//==============================
// Socket IO
//==============================

bool (*AcceptConn)(ts_listen Listening, ts_io* Conn);

/* Accepts an incoming connection on [Listening], assigns it to [Conn.Socket],
 |  and binds that socket to the IO queue. Optionally, the user can assign a
 |  memory buffer to [.IoBuffer] and its size to [.IoSize], and a read will be
 |  posted immediately upon establishing a connection. If left blank, no such read
 |  will be performed.
|--- Return: true if successful, false if not. */

bool (*CreateConn)(ts_io* Conn, ts_sockaddr SockAddr);

/* Creates a new connection on the socket in [Conn], binding it to the address at
|  [SockAddr], created with CreateSockAddr(). Optionally, the user can assign a
 |  data buffer to [.IoBuffer] and its size to [.IoSize], and a send will be
 |  posted immediately upon establishing a connection. If left blank, no such send
 |  will be performed.
|--- Return: true if successful, false if not. */

bool (*DisconnectSocket)(ts_io* Conn);

/* Gracefully disconnects the socket in [Conn]. May leave the socket open for new
 |  connections, depending on the implementation. Check socket handle upon return to
 |  see if it's still valid.
|--- Return: true if successful, false if not. */

bool (*TerminateConn)(ts_io* Conn);

/* Forcefully disconnects the socket in [Conn]. Also guaranteed to close the socket.
 |  Should only be used upon network error.
|--- Return: true if successful, false if not. */

bool (*SendPacket)(ts_io* Conn);

/* Sends data to the socket in [Conn]. The user must assign the data to [.IoBuffer]
|  and the number of bytes to send to [.IoSize] beforehand. The operation happens
 |  asynchronously, and its completion status, as well as number of bytes
 |  transmitted, is gotten by calling WaitOnIoQueue().
 |--- Return: true if successful, false if not. */

bool (*SendFile)(ts_io* Conn);

/* Sends a file to the socket in [Conn]. The user must assign the file handle to
 |  [.IoFile] and the number of bytes to send to [.IoSize] by the user. The operation
 |  happens asynchronously, and its completion status, as well as number of bytes
 |  transmitted, is gotten by calling WaitOnIoQueue().
 |--- Return: true if successful, false if not. */

bool (*RecvPacket)(ts_io* Conn);

/* Reads data from the socket in [Conn]. The user must assign a memory buffer to
 |  [.IoBuffer] and the buffer size to [.IoSize] beforehand. The operation happens
 |  asynchronously, and its completion status, as well as number of bytes
 |  transmitted, is gotten by calling WaitOnIoQueue().
 |--- Return: true if successful, false if not. */


#if !defined(TINYSERVER_STATIC_LINKING)
# if defined(TT_WINDOWS)
#  include "tinyserver-win32.c"
# elif defined(TT_LINUX)
#  if LINUX_VERSION_CODE < KERNEL_VERSION(5, 1, 21)
#   include "tinyserver-epoll.c"
#  else
#   include "tinyserver-epoll.c" // TODO: change to iouring when it's implemented.
#  endif //LINUX_VERSION_CODE
# endif //TT_WINDOWS
#endif //TINYSERVER_STATIC_LINKING

#endif //TINYSERVER_H
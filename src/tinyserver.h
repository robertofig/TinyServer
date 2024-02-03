#ifndef TINYSERVER_H
//===========================================================================
// tinyserver.h
//
// Framework for building server applications. The building blocks of the
// code are platform-agnostic, thread-safe, and are made simple so that
// programmers can add server capabilities quickly to existing applications,
// or building new ones without being hoggled by the server part.
//
// The blueprint for building with it is to call InitServer(), then use the
// functions on the "Setup" section to prepare listening sockets and create
// IO threads. The IO is performed with the "Socket IO" group of functions,
// and IO events are captured with the "Async events" section.
//===========================================================================
#define TINYSERVER_H

#include "tinybase-platform.h"
#include "tinybase-queues.h"


//==============================
// Structs and defines
//==============================

#define MAX_SOCKADDR_SIZE 28

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
    Op_RecvPacket,
    Op_SendPacket,
    Op_SendFile,
    Op_SendToIoQueue
} ts_op;

typedef struct ts_sockaddr
{
    u8 Addr[MAX_SOCKADDR_SIZE]; // Enough for the largest sockaddr struct.
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
# define TS_INTERNAL_DATA_SIZE 40 // See tinyserver-win32.c for more info.
#elif defined(TT_LINUX)
# define TS_INTERNAL_DATA_SIZE 8  // See tinyserver-epoll.c for more info.
#endif

typedef struct ts_io
{
    file Socket;
    usz BytesTransferred;
    ts_status Status;
    ts_op Operation;
    
    u8* IoBuffer;
    u32 IoBufferSize;
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

external file OpenNewSocket(ts_protocol Protocol);

/* Creates a new socket for the defined [Protocol].
 |--- Return: valid socket if successful, INVALID_FILE if not. */

external bool CloseSocket(ts_io* Conn);

/* Closes the socket, taking down any pending connection as well (forced shutdown).
|--- Return: true if successful, false if not. */

external bool BindFileToIoQueue(file File, _opt void* RelatedData);

/* Binds a [File] (socket or regular file) to the IO queue, so IO operations on it
 |  can be dequeued later by called WaitOnIoQueue(). [RelatedData] is an optional
|  parameter where a pointer can be passed to be dequeued with the handle.
|--- Return: true if successful, false if not. */

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

bool (*AcceptConn)(ts_listen Listening, ts_io* Conn, void* Buffer, u32 BufferSize);

/* Accepts an incoming connection on [Listening], assigns it to the socket in [Conn],
 |  and binds that socket to the IO queue. If a socket has not been created yet, the
 |  function creates a new one and assigns it to [Conn]. [Buffer] and [BufferSize] are
 |  used for receiving the first incoming packet of the connection.
|--- Return: true if successful, false if not. */

bool (*CreateConn)(ts_io* Conn, ts_sockaddr SockAddr, void* Buffer, u32 BufferSize);

/* Creates a new connection on the socket in [Conn], binding it to the address at
|  [SockAddr]. This must have been created with CreateSockAddr(). [Buffer] and
 |  [BufferSize] are used for sending the first packet of data right after connection.
|--- Return: true if successful, false if not. */

bool (*DisconnectSocket)(ts_io* Conn);

/* Gracefully disconnects the socket in [Conn]. May leave the socket open for new
 |  connections, depending on the implementation. Check socket handle upon return to
 |  see if it's still valid.
|--- Return: true if successful, false if not. */

bool (*SendPacket)(ts_io* Conn, void* Buffer, u32 BufferSize);

/* Sends a [Buffer] of [BufferSize] to the socket in [Conn]. The operation happens
|  asynchronously, and its completion status, as well as number of bytes transmitted,
 |  is gotten by calling WaitOnIoQueue().
 |--- Return: true if successful, false if not. */

bool (*SendFile)(ts_io* Conn, file File);

// TODO: escrever doc & funcoes!!!

bool (*RecvPacket)(ts_io* Conn, void* Buffer, u32 BufferSize);

/* Receives a list of packets into [Buffer] of [BufferSize] from the socket in [Conn].
 |  The operation happens asynchronously, and its completion status, as well as number
 |  of bytes transmitted, is gotten by calling WaitOnIoQueue().
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
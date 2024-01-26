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

#include "tinyserver-internal.h"


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

typedef enum ts_sock_opt
{
    SockOpt_Broadcast,
    SockOpt_DontLinger,
    SockOpt_Linger,
    SockOpt_UpdateContext
} ts_sock_opt;

#define TS_MAX_SOCKADDR_SIZE 28 // Covers all possible sockaddr lengths.

typedef struct ts_listen
{
    file Socket;
    file Event;
    ts_protocol Protocol;
    u8 SockAddr[TS_MAX_SOCKADDR_SIZE];
    i32 SockAddrSize;
} ts_listen;

typedef struct ts_accept
{
    file Socket;
    ts_protocol Protocol;
} ts_accept;

#if defined(TT_WINDOWS)
# define TS_INTERNAL_DATA_SIZE sizeof(OVERLAPPED)
#elif defined(TT_LINUX)
# define TS_INTERNAL_DATA_SIZE 4 // ts_work_type (see tinyserver-epoll.c for more info).
#endif

typedef struct ts_io
{
    struct ts_io* volatile Next;
    
    file Socket;
    usz BytesTransferred;
    
    u8* IoBuffer;
    u32 IoBufferSize;
    u8 InternalData[TS_INTERNAL_DATA_SIZE];
} ts_io;


//==============================
// Setup
//==============================

external bool InitServer(void);

/* Must be called only once, before anything else. What it does depends on the platform.
|--- Return: true if successful, false if not. */

external file OpenNewSocket(ts_protocol Protocol);

/* Creates a new socket for the defined [Protocol].
 |--- Return: valid socket if successful, INVALID_FILE if not. */

external bool AddListeningSocket(ts_protocol Protocol, u16 Port);

/* Creates a new socket for the defined [Protocol], binds it to the specified [Port] number
|  and sets it up for listening. The socket is added to an internal structure that keeps
|  track of listening sockets. Must have setup [IoQueue] prior to calling this.
|--- Return: true if successful, false if not. */

external void CloseServer(void);

// TODO: Documentation.


//==============================
// Async events
//==============================

ts_accept (*ListenForConnections)(void);

/* Polls the added listening sockets to check for connections. Waits indefinitely until any
|  of the sockets gets signaled. If more than one socket gets signaled at the same time,
|  returns the first one, and the next ones upon subsequent calls to this function.
|--- Return: ts_accept info of the pending connection. */

ts_io* (*WaitOnIoQueue)(void);

/* Waits on the IO queue for event completion, indefinitely until any event get dequeued.
|  If more than one event completes at the same time, returns the first one, and the next
|  ones upon subsequent calls to this function.
|--- Return: pointer to the ts_io connection returned, with the field [.BytesTransferred]
|            updated to that of the latest transaction, or NULL in case of failure. */

bool (*SendToIoQueue)(ts_io* Conn);

/* Sends the socket in [Conn] back to the [IoQueue]. No bytes are transferred during the
|  operation.
|--- Return: true if successful, false if not. */


//==============================
// Socket IO
//==============================

bool (*AcceptConn)(ts_listen Listening, ts_io* Conn, void* Buffer, u32 BufferSize);

/* Accepts an incoming connection on [Listening], assigns it to the socket in [Conn], and
|  binds that socket to the IO queue. If a socket has not been created yet, the function
 |  creates a new one and assigns it to [Conn]. [Buffer] and [BufferSize] are used for
 |  receiving the first incoming packet of the connection.
|--- Return: true if successful, false if not. */

bool (*CreateConn)(ts_io* Conn, void* DstSockAddr, u32 DstSockAddrSize, void* Buffer,
                   u32 BufferSize);

/* Creates a new connection on the socket in [Conn], binding it to the address at
|  [DstSockAddr]. This must be a sockaddr structure, of size [DstSockAddrSize]. [Buffer] and
|  [BufferSize] are used for sending the first packet of data right after connection.
|--- Return: true if successful, false if not. */

bool (*TerminateConn)(ts_io* Conn);

/* Gracefully disconnects the socket in [Conn] and closes it.
|--- Return: true if successful, false if not. */

bool (*DisconnectSocket)(ts_io* Conn);

/* Gracefully disconnects the socket in [Conn]. May leave the socket open for new connections,
|  depending on the implementation. Check socket handle upon return to see if it's still valid.
|--- Return: true if successful, false if not. */

bool (*SendPackets)(ts_io* Conn, void* Buffer, u32 BufferSize);

/* Sends a [Buffer] of [BufferSize] to the socket in [Conn]. The operation happens
|  asynchronously, and its completion status, as well as number of bytes transmitted, is
|  gotten by calling WaitOnIoQueue().
 |--- Return: true if successful, false if not. */

bool (*RecvPackets)(ts_io* Conn, void* Buffer, u32 BufferSize);

/* Receives a list of packets into [Buffer] of [BufferSize] from the socket in [Conn]. The
 |  operation happens asynchronously, and its completion status, as well as number of bytes
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
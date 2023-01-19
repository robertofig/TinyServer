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

typedef enum ts_protocol
{
    Proto_TCPIP4    = 0,
    Proto_UDPIP4    = 1,
    Proto_TCPIP6    = 2,
    Proto_UDPIP6    = 3,
    Proto_Bluetooth = 4
} ts_protocol;

#if defined(TT_WINDOWS)
#define IO_QUEUE_DATA_SIZE 8 // HANDLE to IoCP on Windows.
#else
// Other platforms.
#endif

#define MAX_SOCKADDR_SIZE 30 // Size of SOCKADDR_BTH.

typedef struct ts_listen
{
    usz Socket;
    usz Event;
    ts_protocol Protocol;
    u8 SockAddr[MAX_SOCKADDR_SIZE];
    i32 SockAddrSize;
} ts_listen;

typedef enum ts_event
{
    Event_None,
    Event_Accept,
    Event_Connect,
    Event_Read,
    Event_Write,
    Event_Close,
    Event_Unknown
} ts_event;

typedef struct ts_io_queue
{
    u32 NumThreads;
    u8 Data[IO_QUEUE_DATA_SIZE];
} ts_io_queue;

typedef struct ts_io
{
    struct ts_io* volatile Next;
    
    async Async;
    usz Socket;
    usz BytesTransferred;
} ts_io;

//==============================
// Setup
//==============================

external bool InitServer(void);

/* Must be called only once, before anything else. What it does depends
 |  on the platform.
|--- Return: 1 if successful, 0 if not. */

external file OpenNewSocket(ts_protocol Protocol);

/* Creates a new socket for the defined [Protocol].
 |--- Return: file handle for the socket, or INVALID_FILE if failure. */

external ts_io_queue SetupIoQueue(u32 NumThreads);

/* Prepares async IO queue for [NumThreads] number of threads.
 |--- Return: IO queue data, or empty object if failure. */

external file SetupConnectionSocket(ts_protocol Protocol, u16 Port, ts_io_queue IoQueue);

/* Creates a new socket for the defined [Protocol], and binds it to the
|  specified [Port] number. Must have setup [IoQueue] prior to call this.
  |--- Return: socket ready for connection, or INVALID_FILE if failure. */

external ts_listen SetupListeningSocket(ts_protocol Protocol, u16 Port, ts_io_queue IoQueue);

/* Creates a new socket for the defined [Protocol], binds it to the
|  specified [Port] number and sets it up for listening. Must have setup
 |  [IoQueue] prior to call this.
|--- Return: socket ready for listening, or INVALID_FILE if faulure. */


//==============================
// Async events
//==============================

external ts_event ListenForEvents(usz* Sockets, usz* Events, usz NumEvents);

/* Pass a list of [Sockets] and wait on a list of [Events] to be signaled.
|  Waits indefinitely until any of the events gets signaled. [NumEvents]
|  determines how many [Sockets] and [Events] there are.
|--- Return: the first event signaled. */

external u32 WaitOnIoQueue(ts_io_queue IoQueue, ts_io** Conn);

/* Waits on an [IoQueue] until any IO completes. [Conn] pointer is filled
 |  with the socket info that was used to start the IO.
|--- Return: number of bytes transferred on the operation, or 0 if failure. */

external bool SendToIoQueue(ts_io_queue IoQueue, ts_io* Conn);

/* Sends the socket in [Conn] back to the [IoQueue]. No bytes are
 |  transferred during the operation.
|--- Return: true if successful, false if not. */

external bool AddFileToIoQueue(file File, ts_io_queue IoQueue);

/* Adds a [File] to the [IoQueue], so IO on it can be completed
 |  asynchorously. [File] can be a file or a socket. Once a file is added to
 |  a queue, it stays there until the queue is closed. If [File] had already
|  been added to another queue when this function is called, it fails.
|--- Return: true if successful, false if not. */


//==============================
// Socket IO
//==============================

bool (*AcceptConn)(ts_listen Listening, ts_io* Conn, void* Buffer, u32 BufferSize, ts_io_queue IoQueue);

/* Accepts an incoming connection on [Listening], and assigns it to the
 |  socket in [Conn]. If a socket has not been created yet, the function
|  creates a new one and assigns it to [Conn]. [Buffer] and [BufferSize]
|  are used for receiving the first incoming packet of the connection.
|--- Return: true if successful, false if not. */

bool (*CreateConn)(ts_io* Conn, void* DstSockAddr, u32 DstSockAddrSize, void* Buffer, u32 BufferSize);

/* Creates a new connection on the socket in [Conn], binding it to the
 |  address at [DstSockAddr]. This must be a sockaddr structure, of size
|  [DstSockAddrSize]. [Buffer] and [BufferSize] are used for sending
|  the first packet of data right after connection.
|--- Return: true if successful, false if not. */

bool (*DisconnectSocket)(ts_io* Conn);

/* Gracefully disconnects the socket in [Conn]. May leave the socket open
 |  for new connections, depending on the implementation. Check socket
|  handle upon return to see if it's still valid.
|--- Return: true if successful, false if not. */

bool (*TerminateConn)(ts_io* Conn);

/* Gracefully disconnects the socket in [Conn] and closes it.
|--- Return: true if successful, false if not. */

bool (*SendPackets)(ts_io* Conn, void** Buffers, u32* BuffersSize, usz NumBuffers);

/* Sends a list of [Buffers] of [BufferSize] to the socket in [Conn].
 |  The number of buffers is indicated in [NumBuffers]. The buffers are
|  sent in the same order as they are laid out in the list.
 |--- Return: true if successful, false if not. */

external bool RecvPackets(ts_io* Conn, void** Buffers, u32* BuffersSize, usz NumBuffers);

/* Receives a list of packets into [Buffers] of [BufferSize] from the
 |  socket in [Conn]. The number of buffers is indicated in [NumBuffers].
 |  The buffers are received in the same order as they are laid out in
 |  the list.
 |--- Return: true if successful, false if not. */


#if !defined(TINYSERVER_STATIC_LINKING)
#if defined(TT_WINDOWS)
#include "tinyserver-win32.c"
#endif //TT_WINDOWS
#endif //TINYSERVER_STATIC_LINKING

#endif //TINYSERVER_H
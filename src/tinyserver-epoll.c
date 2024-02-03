#include <arpa/inet.h>
#include <netinet/in.h>
#include <semaphore.h>
#include <sys/epoll.h>
#include <sys/sendfile.h>
#include <sys/socket.h>

#include "tinyserver-internal.h"


//==============================
// Forward declarations
//==============================

internal bool _AcceptConn(ts_listen, ts_io*, void*, u32);
internal bool _CreateConn(ts_io*, ts_sockaddr, void*, u32);
internal bool _DisconnectSocket(ts_io*);
internal bool _RecvPacket(ts_io*, void*, u32);
internal bool _SendPacket(ts_io*, void*, u32);


//==============================
// Internal (Auxiliary)
//==============================

// This is what [.InternalData] member of ts_io translates to.
typedef struct ts_internal
{
    int EventType; // Bitmask with the events returned by epoll.
    file File; // Used on SendFile().
} ts_internal;

internal file
CreateIoQueue(void)
{
    file Result = INVALID_FILE;
    
    struct rlimit Limit = {0};
    if (getrlimit(RLIMIT_NOFILE, &Limit) == 0)
    {
        int MaxSockCount = Limit.rlim_cur;
        int EPoll = epoll_create(MaxSockCount);
        if (EPoll != -1)
        {
            Result = (file)EPoll;
        }
    }
    
    return Result;
}

internal bool
BindFileToQueue(file File, file Queue, _opt void* RelatedData)
{
    int EPoll = (int)Queue;
    struct epoll_event Event = {0};
    Event.events = EPOLLIN|EPOLLOUT|EPOLLRDHUP|EPOLLET;
    Event.data.ptr = RelatedData;
    int Result = epoll_ctl(EPoll, EPOLL_CTL_ADD, File, &Event);
    return (Result == 0);
}

internal bool
PushToWorkQueue(ts_io* Conn)
{
    ts_server_info* ServerInfo = (ts_server_info*)gServerArena.Base;
    if (!MPMCRingBufferPush(&ServerInfo->WorkQueue, (void*)Conn))
    {
        // TODO: Could not insert in ring buffer. Expand buffer size?
    }
    sem_post((sem_t*)&ServerInfo->WorkSemaphore);
    return true;
}

internal ts_io*
PopFromWorkQueue(void)
{
    ts_server_info* ServerInfo = (ts_server_info*)gServerArena.Base;
    sem_wait((sem_t*)&ServerInfo->WorkSemaphore);
    ts_io* Conn = (ts_io*)MPMCRingBufferPop(&ServerInfo->WorkQueue);
    return Conn;
}

THREAD_PROC(IoEventProc)
{
    ts_io* Result = NULL;
    ts_server_info* ServerInfo = (ts_server_info*)gServerArena.Base;
    struct epoll_event* IoEvents = (struct epoll_event*)ServerInfo->IoEvents;
    
    while (true)
    {
        int EventCount = epoll_wait(ServerInfo->IoQueue, IoEvents, MAX_DEQUEUE, -1);
        if (EventCount < 0)
        {
            continue; // An error occurred, try again.
        }
        
        for (int Idx = 0; Idx < EventCount; Idx++)
        {
            struct epoll_event Event = IoEvents[Idx];
            ts_io* Conn = (ts_io*)Event.data.ptr;
            ts_internal* Internal = (ts_internal*)Conn->InternalData;
            IoEvent->EventType = Event.events;
        }
    }
    
    return 0;
}


//==============================
// Setup
//==============================

#define TS_ARENA_SIZE Kilobyte(1)
#define TS_RINGBUF_SIZE Megabyte(1)

external bool
InitServer(void)
{
    InitBuffersArch();
    LoadSystemInfo();
    
    AcceptConn = _AcceptConn;
    CreateConn = _CreateConn;
    DisconnectSocket = _DisconnectSocket;
    RecvPacket = _RecvPacket;
    SendPacket = _SendPacket;
    
    gServerArena = GetMemory(TS_ARENA_SIZE, 0, MEM_WRITE);
    if (!gServerArena.Base)
    {
        return false;
    }
    ts_server_info* ServerInfo = PushStruct(&gServerArena, ts_server_info);
    
    ServerInfo->AcceptQueue = CreateIoQueue();
    ServerInfo->IoQueue = CreateIoQueue();
    ServerInfo->AcceptEvents = ServerInfo->IoEvents = (u8*)&ServerInfo[1];
    ServerInfo->CurrentAcceptIdx = USZ_MAX;
    
    thread IoEventThread = ThreadCreate(IoEventProc, 0, false);
    if (!IoEventThread.Handle)
    {
        return false;
    }
    ServerInfo->IoEventThread = IoEventThread;
    ServerInfo->IoEvents = PushArray(&gServerArena, MAX_DEQUEUE,
                                     struct epoll_event);
    
    buffer WorkQueueMem = GetMemory(TS_RINGBUF_SIZE, 0, MEM_WRITE);
    if (!WorkQueueMem.Base)
    {
        return false;
    }
    usz NumElements = WorkQueueMem.Size / sizeof(void*);
    void** WorkQueueStart = PushArray(&WorkQueueMem, NumElements, void**);
    ServerInfo->WorkQueue = InitMPMCRingBuffer(WorkQueueStart, WorkQueueMem.Size);
    ServerInfo->WorkQueueMem = WorkQueueMem;
    sem_init((sem_t*)&ServerInfo->WorkSemaphore, 0, 0);
    
    return true;
}

external void
CloseServer(void)
{
    if (gServerArena.Base)
    {
        ts_server_info* ServerInfo = (ts_server_info*)gServerArena.Base;
        ThreadKill(&ServerInfo->IoEventThread);
        FreeMemory(&ServerInfo->WorkQueueMem);
        sem_destroy((sem_t*)&ServerInfo->WorkSemaphore);
        CloseFileHandle(ServerInfo->AcceptQueue);
        CloseFileHandle(ServerInfo->IoQueue);
        FreeMemory(&gServerArena);
    }
}

external file
OpenNewSocket(ts_protocol Protocol)
{
    int AF, Type = SOCK_CLOEXEC|SOCK_NONBLOCK, Proto;
    switch (Protocol)
    {
        case Proto_TCPIP4:
        { AF = AF_INET; Type |= SOCK_STREAM; Proto = IPPROTO_TCP; } break;
        case Proto_TCPIP6:
        { AF = AF_INET6; Type |= SOCK_STREAM; Proto = IPPROTO_TCP; } break;
        case Proto_UDPIP4:
        { AF = AF_INET; Type |= SOCK_DGRAM; Proto = IPPROTO_UDP; } break;
        case Proto_UDPIP6:
        { AF = AF_INET6; Type |= SOCK_DGRAM; Proto = IPPROTO_UDP; } break;
        default:
        { AF = 0; Type = 0; Proto = 0; }
    }
    
    int NewSocket = socket(AF, Type, Proto);
    file Result = (NewSocket == -1) ? INVALID_FILE : (file)NewSocket;
    return Result;
}

external bool
CloseSocket(ts_io* Conn)
{
    if (close(Conn->Socket) == 0)
    {
        Conn->Status = Status_None;
        return true;
    }
    return false;
}

external bool
BindFileToIoQueue(file File, _opt void* RelatedData)
{
    ts_server_info* ServerInfo = (ts_server_info*)gServerArena.Base;
    return BindFileToQueue(File, ServerInfo->IoQueue, RelatedData);
}

external ts_sockaddr
CreateSockAddr(char* IpAddress, u16 Port, ts_protocol Protocol)
{
    ts_sockaddr Result = {0};
    if (Protocol == Proto_TCPIP4 || Protocol == Proto_UDPIP4)
    {
        struct sockaddr_in* Addr = (struct sockaddr_in*)Result.Addr;
        Addr->sin_family = AF_INET;
        Addr->sin_port = FlipEndian16(Port);
        inet_pton(AF_INET, IpAddress, &Addr->sin_addr);
        Result.Size = sizeof(struct sockaddr_in);
    }
    else if (Protocol == Proto_TCPIP6 || Protocol == Proto_UDPIP6)
    {
        struct sockaddr_in6* Addr = (struct sockaddr_in6*)Result.Addr;
        Addr->sin6_family = AF_INET6;
        Addr->sin6_port = FlipEndian16(Port);
        inet_pton(AF_INET6, IpAddress, &Addr->sin6_addr);
        Result.Size = sizeof(struct sockaddr_in6);
    }
    return Result;
}

external bool
AddListeningSocket(ts_protocol Protocol, u16 Port)
{
    bool Result = false;
    
    file Socket = OpenNewSocket(Protocol);
    if (Socket != INVALID_FILE)
    {
        u8 ListenAddr[sizeof(struct sockaddr_in6)] = {0};
        socklen_t ListenAddrSize = 0;
        
        switch (Protocol)
        {
            case Proto_TCPIP4:
            case Proto_UDPIP4:
            {
                struct sockaddr_in* Addr = (struct sockaddr_in*)ListenAddr;
                Addr->sin_family = AF_INET;
                Addr->sin_port = FlipEndian16(Port);
                Addr->sin_addr.s_addr = INADDR_ANY;
                ListenAddrSize = (socklen_t)sizeof(struct sockaddr_in);
            } break;
            
            case Proto_TCPIP6:
            case Proto_UDPIP6:
            {
                struct sockaddr_in6* Addr = (struct sockaddr_in6*)ListenAddr;
                Addr->sin6_family = AF_INET6;
                Addr->sin6_port = FlipEndian16(Port);
                Addr->sin6_addr = in6addr_any;
                ListenAddrSize = (socklen_t)sizeof(struct sockaddr_in6);
            } break;
        }
        
        setsockopt(Socket, SOL_SOCKET, SO_REUSEADDR, &(int){1}, sizeof(int));
        setsockopt(Socket, SOL_SOCKET, SO_REUSEPORT, &(int){1}, sizeof(int));
        if (bind((int)Socket, (struct sockaddr*)ListenAddr, ListenAddrSize) == 0
            && listen((int)Socket, SOMAXCONN) == 0)
        {
            ts_server_info* ServerInfo = (ts_server_info*)gServerArena.Base;
            
            ts_listen* Listen = PushStruct(&gServerArena, ts_listen);
            Listen->Socket = Socket;
            Listen->Protocol = Protocol;
            Listen->SockAddrSize = (u32)ListenAddrSize;
            
            if (BindFileToQueue(Socket, ServerInfo->AcceptQueue, Listen))
            {
                ServerInfo->ListenCount++;
                ServerInfo->AcceptEvents += sizeof(ts_listen);
                ServerInfo->IoEvents += sizeof(ts_listen) + sizeof(struct epoll_event);
                
                Result = true;
            }
            else
            {
                gServerArena.WriteCur -= sizeof(ts_listen);
            }
        }
        else
        {
            close((int)Socket);
        }
    }
    
    return Result;
}


//==============================
// Async events
//==============================

external ts_listen
ListenForConnections(void)
{
    ts_server_info* ServerInfo = (ts_server_info*)gServerArena.Base;
    struct epoll_event* EventList = (struct epoll_event*)ServerInfo->AcceptEvents;
    
    // First time calling this function it runs epoll_wait() and gets a list of
    // sockets with pending accepts; it then returns the first one. Subsequent
    // calls will advance on the list, continuing where the previous call stopped.
    // After all sockets are checked we have to execute the function again.
    
    if (ServerInfo->CurrentAcceptIdx == USZ_MAX)
    {
        int EventCount = epoll_wait(ServerInfo->AcceptQueue, EventList,
                                    ServerInfo->ListenCount, -1);
        if (EventCount < 0)
        {
            // This is the only codepath that exits ListenForEvents() on error.
            ts_listen ErrorResult = {0};
            ErrorResult.Socket = INVALID_FILE;
            return ErrorResult;
        }
        ServerInfo->CurrentAcceptIdx = 0;
        ServerInfo->MaxAcceptIdx = EventCount;
    }
    
    while (ServerInfo->CurrentAcceptIdx < ServerInfo->MaxAcceptIdx)
    {
        struct epoll_event Event = EventList[ServerInfo->CurrentAcceptIdx++];
        if (Event.events & EPOLLIN)
        {
            ts_listen Listen = *(ts_listen*)Event.data.ptr;
            return Listen;
        }
    }
    
    // If it got here, there is no sockets left to check, meaning we need to call
    // epoll_wait() again. Set CurrentAcceptIdx to default and try again.
    
    ServerInfo->CurrentAcceptIdx = USZ_MAX;
    return ListenForConnections();
}

external ts_io*
WaitOnIoQueue(void)
{
    // This call will block until there is work to be dequeued.
    ts_io* Conn = PopFromWorkQueue();
    ts_internal Internal = *(ts_internal*)Conn->InternalData;
    
    if (IoEvent.EventType & EPOLLERR)
    {
        Conn->BytesTransferred = 0;
        Conn->Status = Status_Error;
    }
    
    else if (IoEvent.EventType & EPOLLRDHUP
             || IoEvent.EventType & EPOLLHUP)
    {
        Conn->BytesTransferred = 0;
        Conn->Status = Status_Aborted;
    }
    
    else if (IoEvent.EventType & EPOLLIN
             && (Conn->Operation == Op_RecvPacket
                 || Conn->Operation == Op_AcceptConn))
    {
        ssize_t BytesTransferred = recv(Conn->Socket, Conn->IoBuffer,
                                        Conn->IoBufferSize, MSG_DONTWAIT);
        if (BytesTransferred == -1)
        {
            BytesTransferred = 0;
            Conn->Status = Status_Error;
        }
        Conn->BytesTransferred = (usz)BytesTransferred;
    }
    
    else if (IoEvent.EventType & EPOLLOUT
             && (Conn->Operation == Op_SendPacket
                 || Conn->Operation == Op_CreateConn))
    {
        ssize_t BytesTransferred = send(Conn->Socket, Conn->IoBuffer,
                                        Conn->IoBufferSize, MSG_DONTWAIT);
        if (BytesTransferred == -1)
        {
            BytesTransferred = 0;
            Conn->Status = Status_Error;
        }
        Conn->BytesTransferred = (usz)BytesTransferred;
    }
    
    else if (IoEvent.EventType & EPOLLOUT
             && Conn->Operation == Op_SendFile)
    {
        ssize_t BytesTransferred = sendfile(Conn->Socket, Internal->File, NULL,
                                            Conn->IoBufferSize);
        if (BytesTransferred == -1)
        {
            BytesTransferred = 0;
            Conn->Status = Status_Error;
        }
        Conn->BytesTransferred = (usz)BytesTransferred;
    }
    
    return Conn;
}

external bool
SendToIoQueue(ts_io* Conn)
{
    Conn->Operation = Op_SendToIoQueue;
    return PushToWorkQueue(Conn);
}


//==============================
// Socket IO
//==============================

internal bool
_AcceptConn(ts_listen Listening, ts_io* Conn, void* Buffer, u32 BufferSize)
{
    u8 RemoteSockAddr[MAX_SOCKADDR_SIZE] = {0};
    i32 RemoteSockAddrSize;
    
    Conn->Operation = Op_AcceptConn;
    int Socket = accept4(Listening.Socket, (struct sockaddr*)RemoteSockAddr,
                         (socklen_t*)&RemoteSockAddrSize, O_NONBLOCK);
    if (Socket >= 0
        && BindFileToIoQueue(Socket, Conn))
    {
        Conn->Socket = (file)Socket;
        Conn->Status = Status_Connected;
        
        u32 TotalAddrSize = RemoteSockAddrSize + 0x10;
        u8* AddrBuffer = (u8*)Buffer + BufferSize - TotalAddrSize;
        CopyData(AddrBuffer, RemoteSockAddrSize, RemoteSockAddr, RemoteSockAddrSize);
        u32 ReadBufferSize = BufferSize - TotalAddrSize;
        
        Conn->IoBuffer = Buffer;
        Conn->IoBufferSize = ReadBufferSize;
        return PushToWorkQueue(Conn);
    }
    
    close(Socket);
    Conn->Socket = USZ_MAX;
    return false;
}

internal bool
_CreateConn(ts_io* Conn, ts_sockaddr SockAddr, void* Buffer, u32 BufferSize)
{
    Conn->Operation = Op_CreateConn;
    if (connect(Conn->Socket, (const struct sockaddr*)SockAddr.Addr,
                (socklen_t)SockAddr.Size) == 0)
    {
        Conn->Status = Status_Connected;
        Conn->IoBuffer = Buffer;
        Conn->IoBufferSize = BufferSize;
        return PushToWorkQueue(Conn);
    }
    return false;
}

internal bool
_DisconnectSocket(ts_io* Conn)
{
    Conn->Operation = Op_DisconnectSocket;
    if (shutdown(Conn->Socket, SHUT_RDWR) == 0)
    {
        ts_server_info* ServerInfo = (ts_server_info*)gServerArena.Base;
        epoll_ctl(ServerInfo->IoQueue, EPOLL_CTL_DEL, Conn->Socket, 0);
        return CloseSocket(Conn);
    }
    return false;
}

internal bool
_RecvPacket(ts_io* Conn, void* Buffer, u32 BufferSize)
{
    Conn->Operation = Op_RecvPacket;
    Conn->IoBuffer = Buffer;
    Conn->IoBufferSize = BufferSize;
    return PushToWorkQueue(Conn);
}

internal bool
_SendPacket(ts_io* Conn, void* Buffer, u32 BufferSize)
{
    Conn->Operation = Op_SendPacket;
    Conn->IoBuffer = Buffer;
    Conn->IoBufferSize = BufferSize;
    return PushToWorkQueue(Conn);
}

internal bool
_SendFile(ts_io* Conn, file File)
{
    // TODO: permit file offset?
    
    Conn->Operation = Op_SendFile;
    
    ts_internal* Internal = (ts_internal*)Conn->InternalData;
    if (Conn->IoBuffer == NULL)
    {
        Conn->IoBufferSize = FileSizeOf(File);
        Conn->IoBuffer = 1; // Value used just so this codepath won't run again.
        Internal->File = File;
    }
    else
    {
        // Uses the last BytesTransferred to know how much to advance the pointer.
        Conn->IoBufferSize -= Conn->BytesTransferred;
    }
    
    return PushToWorkQueue(Conn);
}
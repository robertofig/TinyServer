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

internal bool _AcceptConn(ts_listen, ts_io*);
internal bool _CreateConn(ts_io*, ts_sockaddr);
internal bool _DisconnectSocket(ts_io*, int);
internal bool _TerminateConn(ts_io*);
internal bool _RecvData(ts_io*);
internal bool _SendData(ts_io*);
internal bool _SendFile(ts_io*);


//==============================
// Internal (Auxiliary)
//==============================

// This is what [.InternalData] member of ts_io translates to.
typedef struct ts_internal
{
    int EventType; // Bitmask with the events returned by epoll.
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

internal file
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

internal bool
CloseSocket(ts_io* Conn)
{
    if (close(Conn->Socket) == 0)
    {
        Conn->Socket = INVALID_FILE;
        return true;
    }
    return false;
}


//==============================
// Internal (Work queue)
//==============================

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
            Internal->EventType = Event.events;
            
            PushToWorkQueue(Conn);
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
    TerminateConn = _TerminateConn;
    RecvData = _RecvData;
    SendData = _SendData;
    SendFile = _SendFile;
    
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
    
    thread IoEventThread = InitThread(IoEventProc, 0, false);
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
        KillThread(&ServerInfo->IoEventThread);
        FreeMemory(&ServerInfo->WorkQueueMem);
        sem_destroy((sem_t*)&ServerInfo->WorkSemaphore);
        CloseFileHandle(ServerInfo->AcceptQueue);
        CloseFileHandle(ServerInfo->IoQueue);
        FreeMemory(&gServerArena);
    }
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
        
        const int Value = 1;
        setsockopt(Socket, SOL_SOCKET, SO_REUSEADDR, (const void*)&Value, sizeof(int));
        setsockopt(Socket, SOL_SOCKET, SO_REUSEPORT, (const void*)&Value, sizeof(int));
        if (bind((int)Socket, (struct sockaddr*)ListenAddr, ListenAddrSize) == 0
            && listen((int)Socket, SOMAXCONN) == 0)
        {
            ts_server_info* ServerInfo = (ts_server_info*)gServerArena.Base;
            
            ts_listen* Listen = PushStruct(&gServerArena, ts_listen);
            ServerInfo->ListenCount++;
            
            Listen->Socket = Socket;
            Listen->Protocol = Protocol;
            Listen->SockAddrSize = (u32)ListenAddrSize;
            
            struct epoll_event Event = {0};
            Event.events = EPOLLIN | EPOLLET;
            Event.data.ptr = (void*)Listen;
            if (epoll_ctl(ServerInfo->AcceptQueue, EPOLL_CTL_ADD, Socket, &Event) == 0)
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
    
    if (Internal.EventType & EPOLLERR)
    {
        Conn->BytesTransferred = 0;
        Conn->Status = Status_Error;
    }
    
    else if (Internal.EventType & EPOLLHUP) // || Internal.EventType & EPOLLRDHUP)
    {
        Conn->BytesTransferred = 0;
        Conn->Status = Status_Aborted;
    }
    
    else
    {
        if (Conn->Operation == Op_RecvData)
        {
            ssize_t BytesTransferred = recv(Conn->Socket, Conn->IoBuffer,
                                            Conn->IoSize, MSG_DONTWAIT);
            if (BytesTransferred == -1)
            {
                BytesTransferred = 0;
                Conn->Status = Status_Error;
            }
            else if (BytesTransferred == 0)
            {
                Conn->Status = Status_Aborted;
            }
            Conn->BytesTransferred = (usz)BytesTransferred;
        }
        
        else if (Conn->Operation == Op_SendData)
        {
            ssize_t BytesTransferred = send(Conn->Socket, Conn->IoBuffer,
                                            Conn->IoSize, MSG_DONTWAIT);
            if (BytesTransferred == -1)
            {
                BytesTransferred = 0;
                Conn->Status = Status_Error;
            }
            Conn->BytesTransferred = (usz)BytesTransferred;
        }
        
        else if (Conn->Operation == Op_SendFile)
        {
            ssize_t BytesTransferred = sendfile(Conn->Socket, Conn->IoFile, NULL,
                                                Conn->IoSize);
            if (BytesTransferred == -1)
            {
                BytesTransferred = 0;
                Conn->Status = Status_Error;
            }
            Conn->BytesTransferred = (usz)BytesTransferred;
        }
        
        // For other operations, just return the dequeued ts_io.
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
_AcceptConn(ts_listen Listening, ts_io* Conn)
{
    Conn->Operation = Op_AcceptConn;
    ts_server_info* ServerInfo = (ts_server_info*)gServerArena.Base;
    
    u8 RemoteSockAddr[MAX_SOCKADDR_SIZE] = {0};
    i32 RemoteSockAddrSize;
    int Socket = accept4(Listening.Socket, (struct sockaddr*)RemoteSockAddr,
                         (socklen_t*)&RemoteSockAddrSize, O_NONBLOCK);
    if (Socket >= 0)
    {
        Conn->Socket = (file)Socket;
        Conn->Status = Status_Connected;
        
        struct epoll_event Event = {0};
        if (epoll_ctl(ServerInfo->IoQueue, EPOLL_CTL_ADD, Socket, &Event) == 0)
        {
            if (Conn->IoBuffer)
            {
                u32 TotalAddrSize = RemoteSockAddrSize + 0x10;
                u8* AddrBuffer = (u8*)Conn->IoBuffer + Conn->IoSize - TotalAddrSize;
                CopyData(AddrBuffer, RemoteSockAddrSize, RemoteSockAddr, RemoteSockAddrSize);
                Conn->IoSize -= TotalAddrSize;
                return RecvData(Conn); // Wait for first package.
            }
            else
            {
                return PushToWorkQueue(Conn); // Just dequeue as accepted.
            }
        }
        else
        {
            close(Socket);
        }
    }
    
    Conn->Socket = INVALID_FILE;
    Conn->Status = Status_Error;
    return false;
}


internal bool
_CreateConn(ts_io* Conn, ts_sockaddr SockAddr)
{
    Conn->Operation = Op_CreateConn;
    ts_server_info* ServerInfo = (ts_server_info*)gServerArena.Base;
    
    struct epoll_event Event = {0};
    if (connect(Conn->Socket, (const struct sockaddr*)SockAddr.Addr,
                (socklen_t)SockAddr.Size) == 0
        && epoll_ctl(ServerInfo->IoQueue, EPOLL_CTL_ADD, (int)Conn->Socket, &Event) == 0)
    {
        Conn->Status = Status_Connected;
        if (Conn->IoBuffer)
        {
            return SendData(Conn); // Send first package.
        }
        else
        {
            return PushToWorkQueue(Conn); // Just dequeue as connected.
        }
    }
    
    Conn->Status = Status_Error;
    return false;
}

internal bool
_DisconnectSocket(ts_io* Conn, int Type)
{
    Conn->Operation = Op_DisconnectSocket;
    if (shutdown(Conn->Socket, Type) == 0)
    {
        if (How == TS_DISCONNECT_BOTH)
        {
            Conn->Status = Status_Disconnected;
            ts_server_info* ServerInfo = (ts_server_info*)gServerArena.Base;
            epoll_ctl(ServerInfo->IoQueue, EPOLL_CTL_DEL, Conn->Socket, 0);
            return CloseSocket(Conn);
        }
        else
        {
            Conn->Status = Status_Simplex;
            return true;
        }
    }
    return false;
}

internal bool
_TerminateConn(ts_io* Conn)
{
    Conn->Operation = Op_TerminateConn;
    Conn->Status = Status_None;
    return CloseSocket(Conn);
}

internal bool
_SendData(ts_io* Conn)
{
    Conn->Operation = Op_SendData;
    ts_server_info* ServerInfo = (ts_server_info*)gServerArena.Base;
    
    struct epoll_event Event;
    Event.data.ptr = (void*)Conn;
    Event.events = EPOLLOUT | EPOLLRDHUP | EPOLLET | EPOLLONESHOT;
    return (epoll_ctl(ServerInfo->IoQueue, EPOLL_CTL_MOD, Conn->Socket, &Event) == 0);
}

internal bool
_SendFile(ts_io* Conn)
{
    Conn->Operation = Op_SendFile;
    ts_server_info* ServerInfo = (ts_server_info*)gServerArena.Base;
    
    struct epoll_event Event;
    Event.data.ptr = (void*)Conn;
    Event.events= EPOLLOUT | EPOLLRDHUP | EPOLLET | EPOLLONESHOT;
    return (epoll_ctl(ServerInfo->IoQueue, EPOLL_CTL_MOD, Conn->Socket, &Event) == 0);
}

internal bool
_RecvData(ts_io* Conn)
{
    Conn->Operation = Op_RecvData;
    ts_server_info* ServerInfo = (ts_server_info*)gServerArena.Base;
    
    struct epoll_event Event;
    Event.data.ptr = (void*)Conn;
    Event.events= EPOLLIN | EPOLLRDHUP | EPOLLET | EPOLLONESHOT;
    return (epoll_ctl(ServerInfo->IoQueue, EPOLL_CTL_MOD, Conn->Socket, &Event) == 0);
}

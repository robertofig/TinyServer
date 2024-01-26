#include <arpa/inet.h>
#include <netinet/in.h>
#include <semaphore.h>
#include <sys/epoll.h>
#include <sys/socket.h>

#include "tinyserver-internal.h"


//==============================
// Forward declarations
//==============================

internal ts_accept _ListenForConnections(void);
internal ts_io* _WaitOnIoQueue(void);
internal bool _SendToIoQueue(ts_io*);

internal bool _AcceptConn(ts_listen, ts_io*, void*, u32);
internal bool _CreateConn(ts_io*, void*, u32, void*, u32);
internal bool _TerminateConn(ts_io*);
internal bool _DisconnectSocket(ts_io*);
internal bool _RecvPackets(ts_io*, void*, u32);
internal bool _SendPackets(ts_io*, void*, u32);


//==============================
// Internal
//==============================

enum ts_work_type
{
    WorkType_None,
    WorkType_RecvPacket,
    WorkType_SendPacket,
    WorkType_SendToIoQueue
};

internal file
CreateIoQueue(void)
{
    file Result = INVALID_FILE;
    
    struct rlimit Limit = {0};
    if (getrlimit(RLIMIT_NOFILE, &Limit))
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
BindFileToIoQueue(file File, file IoQueue, _opt void* RelatedData)
{
    int EPoll = (int)IoQueue;
    struct epoll_event Event = {0};
    Event.events = EPOLLIN|EPOLLRDHUP|EPOLLET|EPOLLONESHOT;
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
    struct epoll_event* EventList = (struct epoll_event*)ServerInfo->IoEvents;
    
    while (true)
    {
        if (ServerInfo->CurrentIoIdx == USZ_MAX)
        {
            int EventCount = epoll_wait(ServerInfo->IoQueue, EventList, TS_MAX_DEQUEUE, -1);
            if (EventCount < 0)
            {
                continue; // An error occurred, try again.
            }
            ServerInfo->CurrentIoIdx = 0;
            ServerInfo->MaxIoIdx = EventCount;
        }
        
        while (ServerInfo->CurrentIoIdx < ServerInfo->MaxIoIdx)
        {
            struct epoll_event Event = EventList[ServerInfo->CurrentIoIdx++];
            ts_io* Conn = (ts_io*)Event.data.ptr;
            enum ts_work_type* WorkType = (enum ts_work_type*)Conn->InternalData;
            
            if (Event.events & EPOLLIN) WorkType = WorkType_RecvPacket;
            else if (Event.events & EPOLLOUT) WorkType = WorkType_SendPacket;
            // TODO: Add more event handlers.
            
            PushToWorkQueue(Conn);
        }
        
        ServerInfo->CurrentIoIdx == USZ_MAX;
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
    
    ListenForConnections = _ListenForConnections;
    WaitOnIoQueue = _WaitOnIoQueue;
    SendToIoQueue = _SendToIoQueue;
    
    AcceptConn = _AcceptConn;
    CreateConn = _CreateConn;
    TerminateConn = _TerminateConn;
    DisconnectSocket = _TerminateConn;
    RecvPackets = _RecvPackets;
    SendPackets = _SendPackets;
    
    gServerArena = GetMemory(TS_ARENA_SIZE, 0, MEM_WRITE);
    if (!gServerArena.Base)
    {
        return false;
    }
    ts_server_info* ServerInfo = PushStruct(&gServerArena, ts_server_info);
    
    ServerInfo->AcceptQueue = CreateIoQueue();
    ServerInfo->IoQueue = CreateIoQueue();
    ServerInfo->AcceptEvents = ServerInfo->IoEvents = (u8*)&ServerInfo[1];
    ServerInfo->CurrentAcceptIdx = ServerInfo->CurrentIoIdx = USZ_MAX;
    
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
    
    thread IoEventThread = ThreadCreate(IoEventProc, 0, false);
    if (!IoEventThread.Handle)
    {
        return false;
    }
    ServerInfo->IoEventThread = IoEventThread;
    
    return true;
}

external void
CloseServer(void)
{
    if (gServerArena.Base)
    {
        ts_server_info* ServerInfo = (ts_server_info*)gServerArena.Base;
        ThreadClose(&ServerInfo->IoEventThread);
        FreeMemory(&ServerInfo->WorkQueueMem);
        sem_destroy((sem_t*)&ServerInfo->WorkSemaphore);
        
        // TODO: close AcceptQueue e IoQueue em funcao propria.
    }
}

external file
OpenNewSocket(ts_protocol Protocol)
{
    int AF, Type = SOCK_CLOEXEC|SOCK_NONBLOCK, Proto;
    switch (Protocol)
    {
        case Proto_TCPIP4: { AF = AF_INET; Type |= SOCK_STREAM; Proto = IPPROTO_TCP; } break;
        case Proto_TCPIP6: { AF = AF_INET6; Type |= SOCK_STREAM; Proto = IPPROTO_TCP; } break;
        case Proto_UDPIP4: { AF = AF_INET; Type |= SOCK_DGRAM; Proto = IPPROTO_UDP; } break;
        case Proto_UDPIP6: { AF = AF_INET6; Type |= SOCK_DGRAM; Proto = IPPROTO_UDP; } break;
        default: { AF = 0; Type = 0; Proto = 0; }
    }
    
    file Result = (file)socket(AF, Type, Proto);
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
        
        if (bind((int)Socket, (struct sockaddr*)ListenAddr, ListenAddrSize) == 0
            && listen((int)Socket, SOMAXCONN) == 0)
        {
            ts_server_info* ServerInfo = (ts_server_info*)gServerArena.Base;
            
            ts_listen* Listen = PushStruct(&gServerArena, ts_listen);
            Listen->Socket = Socket;
            Listen->Protocol = Protocol;
            CopyData(Listen->SockAddr, sizeof(Listen->SockAddr), ListenAddr, ListenAddrSize);
            Listen->SockAddrSize = (u32)ListenAddrSize;
            
            if (BindFileToIoQueue(Socket, ServerInfo->AcceptQueue, Listen))
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

internal ts_accept
_ListenForConnections(void)
{
    ts_accept Result = {0};
    
    ts_server_info* ServerInfo = (ts_server_info*)gServerArena.Base;
    struct epoll_event* EventList = (struct epoll_event*)ServerInfo->AcceptEvents;
    
    // First time calling this function it runs epoll_wait() and gets a list of sockets with
    // pending accepts; it then returns the first one. Subsequent calls will advance on the
    // list, continuing where the previous call stopped. After all sockets are checked we have
    // to execute the function again.
    
    if (ServerInfo->CurrentAcceptIdx == USZ_MAX)
    {
        int EventCount = epoll_wait(ServerInfo->AcceptQueue, EventList, ServerInfo->ListenCount, -1);
        if (EventCount < 0)
        {
            // This is the only codepath that exits ListenForEvents() on error.
            return Result;
        }
        ServerInfo->CurrentAcceptIdx = 0;
        ServerInfo->MaxAcceptIdx = EventCount;
    }
    
    while (ServerInfo->CurrentAcceptIdx < ServerInfo->MaxAcceptIdx)
    {
        struct epoll_event Event = EventList[ServerInfo->CurrentAcceptIdx++];
        if (Event.events & EPOLLIN)
        {
            ts_listen* Listen = (ts_listen*)Event.data.ptr;
            Result.Socket = Listen->Socket;
            Result.Protocol = Listen->Protocol;
            return Result;
        }
    }
    
    // If it got here, there is no sockets left to check, meaning we need to call
    // epoll_wait() again. Set CurrentAcceptIdx to default and try again.
    
    ServerInfo->CurrentAcceptIdx == USZ_MAX;
    return ListenForConnections();
}

internal ts_io*
_WaitOnIoQueue(void)
{
    // This call will block until there is work to be dequeued.
    ts_io* Conn = PopFromWorkQueue();
    
    enum ts_work_type WorkType = *(enum ts_work_type*)Conn->InternalData;
    switch(WorkType)
    {
        case WorkType_RecvPacket:
        {
            // OBS: Use MSG_WAITALL instead?
            ssize_t BytesTransferred = recv(Conn->Socket, Conn->IoBuffer,
                                            Conn->IoBufferSize, MSG_DONTWAIT);
            if (BytesTransferred >= 0)
            {
                Conn->BytesTransferred = (usz)BytesTransferred;
            }
            else if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                Conn->BytesTransferred = 0;
            }
            else
            {
                Conn->BytesTransferred = USZ_MAX;
            }
        } break;
        
        case WorkType_SendPacket:
        {
            // OBS: Use MSG_WAITALL instead?
            ssize_t BytesTransferred = send(Conn->Socket, Conn->IoBuffer,
                                            Conn->IoBufferSize, MSG_DONTWAIT);
            if (BytesTransferred >= 0)
            {
                Conn->BytesTransferred = (usz)BytesTransferred;
            }
            else if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                Conn->BytesTransferred = 0;
            }
            else
            {
                Conn->BytesTransferred = USZ_MAX;
            }
        } break;
        
        case WorkType_SendToIoQueue:
        {
            // Do nothing?
        } break;
    }
    
    return Conn;
}

internal bool
_SendToIoQueue(ts_io* Conn)
{
    *(enum ts_work_type*)Conn->InternalData = WorkType_SendToIoQueue;
    return PushToWorkQueue(Conn);
}


//==============================
// Socket IO
//==============================

internal bool
_AcceptConn(ts_listen Listening, ts_io* Conn, void* Buffer, u32 BufferSize)
{
    ts_server_info* ServerInfo = (ts_server_info*)gServerArena.Base;
    
    u8 RemoteSockAddr[TS_MAX_SOCKADDR_SIZE] = {0};
    i32 RemoteSockAddrSize;
    
    int Socket = accept4(Listening.Socket, (struct sockaddr*)RemoteSockAddr,
                         (socklen_t*)&RemoteSockAddrSize, O_NONBLOCK);
    if (Socket >= 0)
    {
        Conn->Socket = (file)Socket;
        if (BindFileToIoQueue(Conn->Socket, ServerInfo->IoQueue, Conn))
        {
            u32 TotalAddrSize = RemoteSockAddrSize + 0x10;
            u8* AddrBuffer = (u8*)Buffer + BufferSize - TotalAddrSize;
            CopyData(AddrBuffer, RemoteSockAddrSize, RemoteSockAddr, RemoteSockAddrSize);
            u32 ReadBufferSize = BufferSize - TotalAddrSize;
            
            return RecvPackets(Conn, Buffer, ReadBufferSize);
        }
        
        close(Conn->Socket);
        Conn->Socket = USZ_MAX;
    }
    return false;
}

internal bool
_CreateConn(ts_io* Conn, void* DstSockAddr, u32 DstSockAddrSize, void* Buffer, u32 BufferSize)
{
    if (connect(Conn->Socket, (const struct sockaddr*)DstSockAddr,
                (socklen_t)DstSockAddrSize) == 0)
    {
        return SendPackets(Conn, Buffer, BufferSize);
    }
    return false;
}

internal bool
_TerminateConn(ts_io* Conn)
{
    shutdown(Conn->Socket, SHUT_RDWR);
    close(Conn->Socket);
    Conn->Socket = USZ_MAX;
    return true;
}

internal bool
_RecvPackets(ts_io* Conn, void* Buffer, u32 BufferSize)
{
    Conn->IoBuffer = Buffer;
    Conn->IoBufferSize = BufferSize;
    *(enum ts_work_type*)Conn->InternalData = WorkType_RecvPacket;
    return PushToWorkQueue(Conn);
}

internal bool
_SendPackets(ts_io* Conn, void* Buffer, u32 BufferSize)
{
    Conn->IoBuffer = Buffer;
    Conn->IoBufferSize = BufferSize;
    *(enum ts_work_type*)Conn->InternalData = WorkType_SendPacket;
    return PushToWorkQueue(Conn);
}

#include <arpa/inet.h>
#include <linux/io_uring.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>

struct ts_ioring_info
{
    file Ring;
    struct io_uring_sqe* SQEArray;
    
    u8* SHead;
    u8* STail;
    u8* SArray;
    u32 SRingMask;
    
    u8* CHead;
    u8* CTail;
    u8* CQEs;
    u32 CRingMask;
};

//==============================
// Setup
//==============================

external b32
InitServer(void)
{
    InitBuffersArch();
    LoadSystemInfo();
}

external file
OpenNewSocket(ts_protocol Protocol)
{
    int AF, Type, Proto;
    switch (Protocol)
    {
        case Proto_TCPIP4: { AF = AF_INET; Type = SOCK_STREAM; Proto = IPPROTO_TCP; } break;
        case Proto_TCPIP6: { AF = AF_INET6; Type = SOCK_STREAM; Proto = IPPROTO_TCP; } break;
        case Proto_UDPIP4: { AF = AF_INET; Type = SOCK_DGRAM; Proto = IPPROTO_UDP; } break;
        case Proto_UDPIP6: { AF = AF_INET6; Type = SOCK_DGRAM; Proto = IPPROTO_UDP; } break;
        case Proto_Bluetooth: { AF = AF_BLUETOOTH; Type = SOCK_STREAM; Proto = BTPROTO_RFCOMM; } break;
        default: { AF = 0; Type = 0; Proto = 0; }
    }
    Type |= SOCK_CLOEXEC|SOCK_NONBLOCK;
    
    file Result = (file)socket(AF, Type, Proto);
    return Result;
}

#define TS_IORING_LEN 4096

external b32
SetupIoQueue(ts_io_queue* IoQueue, _opt u32 NumThreads)
{
    struct io_uring_params Params = {0};
    Params.flags = IORING_SETUP_SQPOLL;
    Params.sq_thread_idle = 2000; // 2 seconds.
#ifdef IORING_SETUP_SUBMIT_ALL
    Params.flags |= IORING_SETUP_SUBMIT_ALL;
#endif
    
    int Ring = syscall(SYS_io_uring_setup, TS_IORING_LEN, &Params);
    if (Ring != -1)
    {
        size_t SRingSize = Params.sq_off.array + Params.sq_entries * sizeof(u32);
        size_t CRingSize = Params.cq_off.cqes + Params.cq_entries * sizeof(struct io_uring_cqe);
        size_t SQESize = Params.sq_entries * sizeof(struct io_uring_sqe);
        
        int Prot = PROT_READ|PROT_WRITE;
        int Map = MAP_SHARED|MAP_POPULATE;
        void* SRing = 0, *CRing = 0, *SQE = 0;
        if ((SRing = mmap(0, SRingSize, Prot, Map, Ring, IORING_OFF_SQ_RING)) != MAP_FAILED
            && (CRing = mmap(0, CRingSize, Prot, Map, Ring, IORING_OFF_CQ_RING)) != MAP_FAILED
            && (SQE = mmap(0, SQESize, Prot, Map, Ring, IORING_OFF_SQES)) != MAP_FAILED)
        {
            IoQueue->NumThreads = NumThreads;
            struct ts_ioring_info* Info = (struct ts_ioring_info*)IoQueue->Data;
            
            Info->SQEArray = (struct io_uring_sqe*)SQE;
            Info->SHead = (u8*)SRing + Params.sq_off.head;
            Info->STail = (u8*)SRing + Params.sq_off.tail;
            Info->SArray = (u8*)SRing + Params.sq_off.array;
            Info->SRingMask = Params.sq_off.ring_mask;
            Info->CHead = (u8*)CRing + Params.cq_off.head;
            Info->CTail = (u8*)CRing + Params.cq_off.tail;
            Info->CQEs = (u8*)CRing + Params.cq_off.cqes;
            Info->CRingMask = Params.cq_off.ring_mask;
            
            return 1;
        }
        close(Ring);
    }
    return 0;
}

external ts_listen
SetupListeningSocket(ts_protocol Protocol, u16 Port, _opt ts_io_queue* IoQueue)
{
    ts_listen Result = {0};
    Result.Socket = INVALID_FILE;
    
    file Socket = OpenNewSocket(Protocol);
    if (Socket != INVALID_FILE)
    {
        u8 ListeningAddr[sizeof(struct sockaddr_in6)] = {0};
        socklen_t ListeningAddrSize = 0;
        
        switch (Protocol)
        {
            case Proto_TCPIP4:
            case Proto_UDPIP4:
            {
                struct sockaddr_in* Addr = (struct sockaddr_in*)ListeningAddr;
                Addr->sin_family = AF_INET;
                Addr->sin_port = FlipEndian16(Port);
                Addr->sin_addr.s_addr = INADDR_ANY;
                ListeningAddrSize = (socklen_t)sizeof(struct sockaddr_in);
            } break;
            
            case Proto_TCPIP6:
            case Proto_UDPIP6:
            {
                struct sockaddr_in6* Addr = (struct sockaddr_in6*)ListeningAddr;
                Addr->sin6_family = AF_INET6;
                Addr->sin6_port = FlipEndian16(Port);
                Addr->sin6_addr = in6addr_any;
                ListeningAddrSize = (socklen_t)sizeof(struct sockaddr_in6);
            } break;
            
            case Proto_Bluetooth:
            {
                // TODO: Fill Bluetooth struct.
            } break;
        }
        
        struct epoll_event Event = { EPOLLIN|EPOLLET, (int)Socket };
        if (bind((int)Socket, (struct sockaddr*)ListeningAddr, ListeningAddrSize) == 0
            && listen((int)Socket, SOMAXCONN) == 0
            && epoll_ctl(EPoll, EPOLL_CTL_ADD, (int)Socket, &Event) == 0
            && (!IoQueue || AddFileToIoQueue(Socket, &IoQueue))) // IoQueue is optional.
        {
            Result.Socket = Socket;
            Result.Protocol = Protocol;
            CopyData(Result.SockAddr, sizeof(Result.SockAddr), ListeningAddr, ListeningAddrSize);
            Result.SockAddrSize = (u32)ListeningAddrSize;
        }
        else
        {
            epoll_ctl(EPoll, EPOLL_CTL_DEL, (int)Socket, 0);
            close((int)Socket);
        }
    }
    
    return Result;
}


//==============================
// Async events
//==============================

external usz
MaxNumberOfEvents(void)
{
    struct rlimit Limit = {0};
    usz Result = (getrlimit(RLIMIT_NOFILE, &Limit) == 0) ? Limit.rlim_cur : 0;
    return Result;
}

// The following struct is for internal use only!!
struct ts_event_info
{
    file* Socket;
    b32 IsListening; // (0) io, (1) listening.
};

external b32
InitEventList(ts_event_list* List, usz NumEvents)
{
    // Memory layout of list is: [NumEvents] counts of ts_event_info, followed
    // by [NumEvents] counts of pollfd structs, with both arrays linked by index..
    // The socket info is duplicated because we need to return a socket pointer,
    // but poll() requires the fd descriptor itself in a pollfd struct. To
    // avoid having to recreate the pollfd array every time, we create it here.
    
    usz MemSize = NumEvents * (sizeof(struct ts_event_info) + sizeof(struct pollfd));
    List->Events = GetMemory(MemSize, 0, MEM_READ|MEM_WRITE);
    if (List->Events.Mem)
    {
        List->MaxEvents = NumEvents;
        List->_IterCurrentEvent = USZ_MAX;
        return 1;
    }
    return 0;
}

external void
DestroyEventList(ts_event_list* List)
{
    FreeMemory(&List->Events);
    List->EventCount = 0;
    List->MaxEvents = 0;
    List->_IterCurrentEvent = USZ_MAX;
}

external b32
AddEvent(ts_event_list* List, file* Socket, i32 Events)
{
    if (List->EventCount < List->MaxEvents)
    {
        short NetEvents = 0;
        if (Events & Event_Accept) NetEvents |= POLLIN;
        if (Events & Event_Read) NetEvents |= POLLIN;
        if (Events & Event_Write) NetEvents |= POLLOUT;
        if (Events & Event_Close) NetEvents |= POLLRDHUP|POLLHUP;
        
        struct ts_event_info* InfoArr = (struct ts_event_info*)List->Events.Mem;
        struct pollfd* PollArr = (struct pollfd*)(InfoArr + List->MaxEvents);
        
        InfoArr[List->EventCount].Socket = Socket;
        InfoArr[List->EventCount].IsListening = (Events & Event_Accept);
        PollArr[List->EventCount].fd = *Socket;
        PollArr[List->EventCount].events = NetEvents;
        
        return 1;
    }
    return 0;
}

external b32
PopEvent(ts_event_list* List, ts_event Event)
{
    struct ts_event_info* InfoArr = (struct ts_event_info*)List->Events.Mem;
    struct pollfd* PollArr = (struct pollfd*)(InfoArr + List->MaxEvents);
    
    PollArr[Event.Idx] = PollArr[--List->EventCount];
    InfoArr[Event.Idx] = InfoArr[List->EventCount];
    
    return 1;
}


external ts_event
ListenForEvents(ts_event_list* List)
{
    ts_event Result = {0};
    
    struct ts_event_info* InfoArr = (struct ts_event_info*)List->Events.Mem;
    struct pollfd* PollArr = (struct pollfd*)(InfoArr + List->MaxEvents);
    
    // The loop below is not meant to run forever. Event processing happens in two steps:
    // 1) call polling function, 2) test each socket to see if signaled. ListenForEvents()
    // bundles them both in one call. First time called it will execute step #1 and #2,
    // with #2 looping over sockets and returns on first found signaled. Subsequent calls
    // will only execute #2, continuing where the previous call stopped. After all events
    // have been found we have to execute #1 again, hence the outer loop.
    
    while (1)
    {
        if (List->_IterCurrentEvent == USZ_MAX)
        {
            int Signaled = poll(PollArr, List->EventCount, -1);
            if (Signaled < 0)
            {
                // This is the only codepath that exits ListenForEvents() on error.
                break;
            }
            List->_IterCurrentEvent = 0;
            List->_IterEventCount = Signaled;
        }
        
        while (List->_IterEventCount)
        {
            usz CurrentIdx = List->_IterCurrentEvent++;
            struct ts_event_info Info = InfoArr[CurrentIdx];
            struct pollfd Poll = PollArr[CurrentIdx];
            
            if (Poll.revents & POLLIN)
            {
                Result.Type |= (Info.IsListening) ? Event_Accept : Event_Read;
            }
            if (Poll.revents & POLLOUT)
            {
                Result.Type |= Event_Write;
            }
            if (Poll.revents & POLLHUP || Poll.revents & POLLRDHUP || Poll.revents & POLLERR)
            {
                Result.Type != Event_Close;
            }
            
            if (Result.Type != 0)
            {
                Result.Socket = Info.Socket;
                Result.Idx = CurrentIdx;
                return Result;;
            }
        }
        
        // No sockets left to check, do this to force another call to polling.
        List->_IterCurrentEvent = USZ_MAX;
    }
    
    // It will only get here if an error in WSAWaitForMultipleEvents() happened.
    return Result;
}

external u32
WaitOnIoQueue(ts_io_queue* IoQueue, ts_io** Conn)
{
}

external bool
SendToIoQueue(ts_io_queue* IoQueue, ts_io* Conn)
{
}

external bool
AddFileToIoQueue(file File, ts_io_queue* IoQueue)
{
}

//==============================
// Socket IO
//==============================

external bool
RecvPackets(ts_io* Conn, void** Buffers, u32* BuffersSize, usz NumBuffers)
{
    WSABUF Elements[128] = {0};
    for (usz ElemIdx = 0; ElemIdx < NumBuffers; ElemIdx++)
    {
        Elements[ElemIdx].len = BuffersSize[ElemIdx];
        Elements[ElemIdx].buf = (char*)Buffers[ElemIdx];
    }
    
    WSAOVERLAPPED* Overlapped = (WSAOVERLAPPED*)Conn->Async.Data;
    ClearMemory(Overlapped, sizeof(WSAOVERLAPPED));
    DWORD Flags = 0;
    bool Result = WSARecv((SOCKET)Conn->Socket, Elements, NumBuffers, NULL, &Flags, Overlapped, NULL) == 0;
    if (!Result) Result = (WSAGetLastError() == WSA_IO_PENDING);
    return Result;
}

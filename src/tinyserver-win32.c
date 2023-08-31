#include <ws2tcpip.h>
#include <ws2bth.h>
#include <mswsock.h>

#pragma comment(lib, "ws2_32.lib")

//==============================
// Function pointers
//==============================

bool (*_AcceptEx)(SOCKET, SOCKET, void*, DWORD, DWORD, DWORD, DWORD*, OVERLAPPED*) = 0;
bool (*_ConnectEx)(SOCKET, SOCKADDR*, int, void*, DWORD, DWORD*, OVERLAPPED*) = 0;
bool (*_DisconnectEx)(SOCKET, OVERLAPPED*, DWORD, DWORD) = 0;
bool (*_TransmitPackets)(SOCKET, TRANSMIT_PACKETS_ELEMENT*, DWORD, DWORD, OVERLAPPED*, DWORD) = 0;

internal bool _AcceptConnSimple(ts_listen, ts_io*, void*, u32, ts_io_queue);
internal bool _AcceptConnEx(ts_listen, ts_io*, void*, u32, ts_io_queue);
internal bool _CreateConnSimple(ts_io*, void*, u32, void*, u32);
internal bool _CreateConnEx(ts_io*, void*, u32, void*, u32);
internal bool _DisconnectSocketSimple(ts_io*);
internal bool _DisconnectSocketEx(ts_io*);
internal bool _SendPacketsSimple(ts_io*, void**, u32*, usz);
internal bool _SendPacketsEx(ts_io*, void**, u32*, usz);

//==============================
// Server configs
//==============================

external b32
InitServer(void)
{
    InitBuffersArch();
    LoadSystemInfo();
    
    WSADATA WSAData;
    if (WSAStartup(0x202, &WSAData) != 0)
    {
        return 0;
    }
    
    // It does not matter which protocol is called here, this socket is just for
    // loading the extension functions.
    SOCKET IoctlSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (IoctlSocket == INVALID_SOCKET)
    {
        return 0;
    }
    
    DWORD BytesReturned = 0;
    
    // AcceptEx()
    GUID GuidAcceptEx = WSAID_ACCEPTEX;
    WSAIoctl(IoctlSocket, SIO_GET_EXTENSION_FUNCTION_POINTER, &GuidAcceptEx, sizeof(GUID),
             &_AcceptEx, sizeof(_AcceptEx), &BytesReturned, NULL, NULL);
    AcceptConn = (_AcceptEx) ? _AcceptConnEx : _AcceptConnSimple;
    
    // ConnectEx()
    GUID GuidConnectEx = WSAID_CONNECTEX;
    WSAIoctl(IoctlSocket, SIO_GET_EXTENSION_FUNCTION_POINTER, &GuidConnectEx, sizeof(GUID),
             &_ConnectEx, sizeof(_ConnectEx), &BytesReturned, NULL, NULL);
    CreateConn = (_ConnectEx) ? _CreateConnEx : _CreateConnSimple;
    
    // DisconnectEx()
    GUID GuidDisconnectEx = WSAID_DISCONNECTEX;
    WSAIoctl(IoctlSocket, SIO_GET_EXTENSION_FUNCTION_POINTER, &GuidDisconnectEx, sizeof(GUID),
             &_DisconnectEx, sizeof(_DisconnectEx), &BytesReturned, NULL, NULL);
    DisconnectSocket = (_DisconnectEx) ? _DisconnectSocketEx : _DisconnectSocketSimple;
    TerminateConn = _DisconnectSocketSimple;
    
    // TransmitPackets()
    GUID GuidTransmitPackets = WSAID_TRANSMITPACKETS;
    WSAIoctl(IoctlSocket, SIO_GET_EXTENSION_FUNCTION_POINTER, &GuidTransmitPackets, sizeof(GUID),
             &_TransmitPackets, sizeof(_TransmitPackets), &BytesReturned, NULL, NULL);
    SendPackets = (_TransmitPackets && gSysInfo.OSVersion[0] == 'S') ? _SendPacketsEx : _SendPacketsSimple;
    
    closesocket(IoctlSocket);
    return 1;
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
        case Proto_Bluetooth: { AF = AF_BTH; Type = SOCK_STREAM; Proto = BTHPROTO_RFCOMM; } break;
        default: { AF = 0; Type = 0; Proto = 0; }
    }
    
    file Result = (file)WSASocketA(AF, Type, Proto, NULL, 0, WSA_FLAG_OVERLAPPED);
    return Result;
}

#if 0
external bool
SetSocketToBroadcast(file Socket)
{
    char Val = 1;
    int Result = setsockopt((SOCKET)Socket, SOL_SOCKET, SO_BROADCAST, &Val, 1);
    return Result != SOCKET_ERROR;
}

external bool
BindClientToServer(file Client, file Listening)
{
    int Result = setsockopt((SOCKET)Client, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT, (char*)&Listening, sizeof(Listening));
    return Result != SOCKET_ERROR;
}
#endif

external b32
SetupIoQueue(ts_io_queue* IoQueue, _opt u32 NumThreads)
{
    HANDLE IoCP = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, (ULONG_PTR)NULL, NumThreads);
    if (IoCP != NULL)
    {
        IoQueue->NumThreads = NumThreads;
        CopyData(IoQueue->Data, sizeof(IoQueue->Data), &IoCP, sizeof(IoCP));
        return 1;
    }
    return 0;
}

external ts_listen
SetupListeningSocket(ts_protocol Protocol, u16 Port, ts_io_queue* IoQueue)
{
    ts_listen Result = {0};
    Result.Socket = INVALID_FILE;
    
    file Socket = OpenNewSocket(Protocol);
    if (Socket != INVALID_FILE)
    {
        u8 ListeningAddr[sizeof(SOCKADDR_BTH)] = {0};
        i32 ListeningAddrSize = 0;
        
        switch (Protocol)
        {
            case Proto_TCPIP4:
            case Proto_UDPIP4:
            {
                SOCKADDR_IN* Addr = (SOCKADDR_IN*)ListeningAddr;
                Addr->sin_family = (ADDRESS_FAMILY)AF_INET;
                Addr->sin_port = FlipEndian16(Port);
                Addr->sin_addr.s_addr = INADDR_ANY;
                ListeningAddrSize = sizeof(SOCKADDR_IN);
            } break;
            
            case Proto_TCPIP6:
            case Proto_UDPIP6:
            {
                SOCKADDR_IN6* Addr = (SOCKADDR_IN6*)ListeningAddr;
                Addr->sin6_family = AF_INET6;
                Addr->sin6_port = FlipEndian16(Port);
                Addr->sin6_addr = in6addr_any;
                ListeningAddrSize = sizeof(SOCKADDR_IN6);
            } break;
            
            case Proto_Bluetooth:
            {
                // TODO: Fill Bluetooth struct.
            } break;
        }
        
        HANDLE IoCP = INVALID_HANDLE_VALUE;
        WSAEVENT Event = 0;
        if (bind((SOCKET)Socket, (SOCKADDR*)ListeningAddr, ListeningAddrSize) == 0
            && listen((SOCKET)Socket, SOMAXCONN) == 0
            && (Event = WSACreateEvent()) != INVALID_HANDLE_VALUE
            && WSAEventSelect((SOCKET)Socket, Event, FD_ACCEPT) == 0
            && AddFileToIoQueue(Socket, IoQueue))
        {
            Result.Socket = Socket;
            Result.Event = (file)Event;
            Result.Protocol = Protocol;
            CopyData(Result.SockAddr, sizeof(Result.SockAddr), ListeningAddr, ListeningAddrSize);
            Result.SockAddrSize = (u32)ListeningAddrSize;
        }
        else
        {
            closesocket((SOCKET)Socket);
            WSACloseHandle(Event);
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
    return (usz)WSA_MAXIMUM_WAIT_EVENTS;
}

external b32
InitEventList(ts_event_list* List, usz NumEvents)
{
    // Memory layout of list is: [NumEvent] counts of SOCKET pointers,
    // followed by [NumEvents] counts of WSAEVENT objects. Separating them
    // into two arrays, any index must tie a SOCKET pointer to its WSAEVENT.
    // This is because WSAWaitOnMultipleEvents() requires a contiguous
    // array of WSAEVENT objects.
    
    usz MemSize = NumEvents * (sizeof(SOCKET*) + sizeof(WSAEVENT));
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
    file** SocketList = (file**)List->Events.Mem;
    WSAEVENT* EventList = (WSAEVENT*)(SocketList + List->MaxEvents);
    
    for (usz Idx = 0; Idx < List->EventCount; Idx++)
    {
        WSACloseEvent(EventList[Idx]);
    }
    FreeMemory(&List->Events);
}

external b32
AddEvent(ts_event_list* List, file* Socket, i32 Events)
{
    if (List->EventCount < List->MaxEvents)
    {
        WSAEVENT Event = WSACreateEvent();
        long NetEvents = 0;
        if (Events & Event_Accept) NetEvents |= FD_ACCEPT;
        if (Events & Event_Connect) NetEvents |= FD_CONNECT;
        if (Events & Event_Read) NetEvents |= FD_READ;
        if (Events & Event_Write) NetEvents |= FD_WRITE;
        if (Events & Event_Close) NetEvents |= FD_CLOSE;
        
        if (WSAEventSelect(*Socket, Event, NetEvents) == 0)
        {
            file** SocketList = (file**)List->Events.Mem;
            WSAEVENT* EventList = (WSAEVENT*)(SocketList + List->MaxEvents);
            SocketList[List->EventCount] = Socket;
            EventList[List->EventCount++] = Event;
            return 1;
        }
    }
    return 0;
}

external b32
PopEvent(ts_event_list* List, ts_event Event)
{
    file** SocketList = (file**)List->Events.Mem;
    WSAEVENT* EventList = (WSAEVENT*)(SocketList + List->MaxEvents);
    
    if (WSACloseEvent(EventList[Event.Idx]))
    {
        EventList[Event.Idx] = EventList[--List->EventCount];
        SocketList[Event.Idx] = SocketList[List->EventCount];
        return 1;
    }
    return 0;
}

external ts_event
ListenForEvents(ts_event_list* List)
{
    ts_event Result = {0};
    
    file** SocketList = (file**)List->Events.Mem;
    WSAEVENT* EventList = (WSAEVENT*)(SocketList + List->MaxEvents);
    
    // The loop below is not meant to run forever. Event processing happens in two steps:
    // 1) call polling function, 2) test each socket to see if signaled. ListenForEvents()
    // bundles them both in one call. First time called it will execute step #1 and #2,
    // with #2 looping over sockets and returns on first found signaled. Subsequent calls
    // will only execute #2, continuing where the previous call stopped. After all sockets
    // are checked we have to execute #1 again, hence the outer loop.
    
    while (1)
    {
        if (List->_IterCurrentEvent == USZ_MAX)
        {
            DWORD Signaled = WSAWaitForMultipleEvents(List->EventCount, EventList, FALSE, WSA_INFINITE, FALSE);
            if (Signaled == WSA_WAIT_FAILED)
            {
                // This is the only codepath that exits ListenForEvents() on error.
                break;
            }
            List->_IterCurrentEvent = Signaled - WSA_WAIT_EVENT_0;
        }
        
        while (List->_IterCurrentEvent < List->EventCount)
        {
            file* Socket = SocketList[List->_IterCurrentEvent];
            WSAEVENT Event = EventList[List->_IterCurrentEvent++];
            
            WSANETWORKEVENTS EventType;
            if (WSAEnumNetworkEvents(*(SOCKET*)Socket, (WSAEVENT)Event, &EventType) == 0)
            {
                if (EventType.lNetworkEvents == FD_ACCEPT
                    && EventType.iErrorCode[FD_ACCEPT_BIT] == 0)
                {
                    Result.Type = Event_Accept;
                }
                else if (EventType.lNetworkEvents == FD_READ
                         && EventType.iErrorCode[FD_READ_BIT] == 0)
                {
                    Result.Type = Event_Read;
                }
                else if (EventType.lNetworkEvents == FD_WRITE
                         && EventType.iErrorCode[FD_WRITE_BIT] == 0)
                {
                    Result.Type = Event_Write;
                }
                else if (EventType.lNetworkEvents == FD_CLOSE
                         && EventType.iErrorCode[FD_CLOSE_BIT] == 0)
                {
                    Result.Type = Event_Close;
                }
                else
                {
                    Result.Type = Event_Unknown;
                }
                
                Result.Socket = Socket;
                Result.Idx = List->_IterCurrentEvent-1;
                WSAResetEvent(Event);
                
                return Result;
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
    HANDLE IoCP = *(HANDLE*)IoQueue->Data;
    DWORD BytesTransferred;
    ULONG_PTR CompletionKey;
    OVERLAPPED* Overlapped;
    GetQueuedCompletionStatus(IoCP, &BytesTransferred, &CompletionKey, &Overlapped, INFINITE);
    
    *Conn = (ts_io*)((u8*)Overlapped - sizeof(void*));
    return (u32)BytesTransferred;
}

external bool
SendToIoQueue(ts_io_queue* IoQueue, ts_io* Conn)
{
    HANDLE IoCP = *(HANDLE*)IoQueue->Data;
    ULONG_PTR CompletionKey = 0;
    OVERLAPPED* Overlapped = (OVERLAPPED*)Conn->Async.Data;
    bool Result = PostQueuedCompletionStatus(IoCP, 0, CompletionKey, Overlapped);
    return Result;
}

external bool
AddFileToIoQueue(file File, ts_io_queue* IoQueue)
{
    HANDLE IoCP = *(HANDLE*)IoQueue->Data;
    HANDLE Verify = CreateIoCompletionPort((HANDLE)File, IoCP, (ULONG_PTR)NULL, 0);
    return Verify == IoCP;
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

internal bool
_AcceptConnSimple(ts_listen Listening, ts_io* Conn, void* Buffer, u32 BufferSize, ts_io_queue IoQueue)
{
    u8 RemoteSockAddr[MAX_SOCKADDR_SIZE] = {0};
    i32 RemoteSockAddrSize;
    
    SOCKET Socket = WSAAccept((SOCKET)Listening.Socket, (SOCKADDR*)RemoteSockAddr, &RemoteSockAddrSize, NULL, (DWORD_PTR)NULL);
    if (Socket != INVALID_SOCKET)
    {
        Conn->Socket = (file)Socket;
        if (!AddFileToIoQueue(Conn->Socket, &IoQueue))
        {
            int Error = GetLastError();
            closesocket(Conn->Socket);
            Conn->Socket = (usz)INVALID_HANDLE_VALUE;
            return false;
        }
        
        u32 TotalAddrSize = RemoteSockAddrSize + 0x10;
        u8* AddrBuffer = (u8*)Buffer + BufferSize - TotalAddrSize;
        CopyData(AddrBuffer, RemoteSockAddrSize, RemoteSockAddr, RemoteSockAddrSize);
        u32 ReadBufferSize = BufferSize - TotalAddrSize;
        
        return RecvPackets(Conn, &Buffer, &ReadBufferSize, 1);
    }
    return false;
}

internal bool
_AcceptConnEx(ts_listen Listening, ts_io* Conn, void* Buffer, u32 BufferSize, ts_io_queue IoQueue)
{
    DWORD LocalAddrSize = 0x10 + Listening.SockAddrSize;
    DWORD RemoteAddrSize = 0x10 + Listening.SockAddrSize;
    
    DWORD InDataSize = BufferSize - LocalAddrSize - RemoteAddrSize;
    DWORD BytesRecv = 0;
    
    // If Conn->Socket is not initialized, initializes it.
    if (Conn->Socket == INVALID_SOCKET)
    {
        Conn->Socket = OpenNewSocket(Listening.Protocol);
        if (!AddFileToIoQueue(Conn->Socket, &IoQueue))
        {
            int Error = GetLastError();
            closesocket(Conn->Socket);
            Conn->Socket = (usz)INVALID_HANDLE_VALUE;
            return false;
        }
    }
    
    WSAOVERLAPPED* Overlapped = (WSAOVERLAPPED*)Conn->Async.Data;
    ClearMemory(Overlapped, sizeof(WSAOVERLAPPED));
    bool Result = _AcceptEx((SOCKET)Listening.Socket, (SOCKET)Conn->Socket, Buffer, InDataSize, LocalAddrSize, RemoteAddrSize, &BytesRecv, Overlapped) == TRUE;
    if (!Result) Result = (WSAGetLastError() == WSA_IO_PENDING);
    return Result;
}

internal bool
_CreateConnSimple(ts_io* Conn, void* DstSockAddr, u32 DstSockAddrSize, void* Buffer, u32 BufferSize)
{
    if (connect((SOCKET)Conn->Socket, (SOCKADDR*)DstSockAddr, (int)DstSockAddrSize) == 0)
    {
        return SendPackets(Conn, &Buffer, &BufferSize, 1);
    }
    return false;
}

internal bool
_CreateConnEx(ts_io* Conn, void* DstSockAddr, u32 DstSockAddrSize, void* Buffer, u32 BufferSize)
{
    WSAOVERLAPPED* Overlapped = (WSAOVERLAPPED*)Conn->Async.Data;
    ClearMemory(Overlapped, sizeof(WSAOVERLAPPED));
    DWORD BytesSent = 0;
    bool Result = _ConnectEx((SOCKET)Conn->Socket, (SOCKADDR*)DstSockAddr, DstSockAddrSize, Buffer, BufferSize, &BytesSent, Overlapped) == TRUE;
    if (!Result) Result = (WSAGetLastError() == WSA_IO_PENDING);
    return Result;
}

internal bool
_DisconnectSocketSimple(ts_io* Conn)
{
    shutdown((SOCKET)Conn->Socket, SD_BOTH);
    closesocket((SOCKET)Conn->Socket);
    Conn->Socket = INVALID_SOCKET;
    return true;
}

internal bool
_DisconnectSocketEx(ts_io* Conn)
{
    WSAOVERLAPPED* Overlapped = (WSAOVERLAPPED*)Conn->Async.Data;
    ClearMemory(Overlapped, sizeof(WSAOVERLAPPED));
    bool Result = _DisconnectEx((SOCKET)Conn->Socket, Overlapped, TF_REUSE_SOCKET, 0) == TRUE;
    if (!Result) Result = (WSAGetLastError() == WSA_IO_PENDING);
    return Result;
}

internal bool
_SendPacketsSimple(ts_io* Conn, void** Buffers, u32* BuffersSize, usz NumBuffers)
{
    WSABUF Elements[128] = {0};
    for (usz ElemIdx = 0; ElemIdx < NumBuffers; ElemIdx++)
    {
        Elements[ElemIdx].len = BuffersSize[ElemIdx];
        Elements[ElemIdx].buf = (char*)Buffers[ElemIdx];
    }
    
    WSAOVERLAPPED* Overlapped = (WSAOVERLAPPED*)Conn->Async.Data;
    ClearMemory(Overlapped, sizeof(WSAOVERLAPPED));
    bool Result = WSASend((SOCKET)Conn->Socket, Elements, NumBuffers, NULL, 0, Overlapped, NULL) == 0;
    if (!Result) Result = (WSAGetLastError() == WSA_IO_PENDING);
    return Result;
}

internal bool
_SendPacketsEx(ts_io* Conn, void** Buffers, u32* BuffersSize, usz NumBuffers)
{
    TRANSMIT_PACKETS_ELEMENT Elements[128] = {0};
    for (usz ElemIdx = 0; ElemIdx < NumBuffers; ElemIdx++)
    {
        Elements[ElemIdx].dwElFlags = TP_ELEMENT_MEMORY;
        Elements[ElemIdx].cLength = BuffersSize[ElemIdx];
        Elements[ElemIdx].pBuffer = Buffers[ElemIdx];
    }
    
    WSAOVERLAPPED* Overlapped = (WSAOVERLAPPED*)Conn->Async.Data;
    ClearMemory(Overlapped, sizeof(WSAOVERLAPPED));
    bool Result = _TransmitPackets((SOCKET)Conn->Socket, Elements, NumBuffers, 0, Overlapped, TF_USE_KERNEL_APC) == TRUE;
    if (!Result) Result = (WSAGetLastError() == WSA_IO_PENDING);
    return Result;
}

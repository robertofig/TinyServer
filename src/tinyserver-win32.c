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

external bool
InitServer(void)
{
    InitBuffersArch();
    LoadSystemInfo();
    
    WSADATA WSAData;
    if (WSAStartup(0x202, &WSAData) != 0)
    {
        return false;
    }
    
    // It does not matter which protocol is called here, this socket is just for
    // loading the extension functions.
    SOCKET IoctlSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (IoctlSocket == INVALID_SOCKET)
    {
        return false;
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
    
    // TransmitPackets()
    GUID GuidTransmitPackets = WSAID_TRANSMITPACKETS;
    WSAIoctl(IoctlSocket, SIO_GET_EXTENSION_FUNCTION_POINTER, &GuidTransmitPackets, sizeof(GUID),
             &_TransmitPackets, sizeof(_TransmitPackets), &BytesReturned, NULL, NULL);
    SendPackets = (_TransmitPackets && gSysInfo.OSVersion[0] == 'S') ? _SendPacketsEx : _SendPacketsSimple;
    
    closesocket(IoctlSocket);
    return true;
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
    
    file Socket = (file)WSASocketA(AF, Type, Proto, NULL, 0, WSA_FLAG_OVERLAPPED);
    return Socket;
}

external ts_io_queue
SetupIoQueue(u32 NumThreads)
{
    ts_io_queue Result = {0};
    HANDLE IoCP = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, (ULONG_PTR)NULL, NumThreads);
    if (IoCP != NULL)
    {
        Result.NumThreads = NumThreads;
        CopyData(Result.Data, sizeof(Result.Data), &IoCP, sizeof(IoCP));
    }
    return Result;
}

external ts_listen
SetupListeningSocket(ts_protocol Protocol, u16 Port, ts_io_queue IoQueue)
{
    ts_listen Result = {0};
    
    SOCKET Socket = OpenNewSocket(Protocol);
    if (Socket != INVALID_SOCKET)
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
        if (bind(Socket, (SOCKADDR*)ListeningAddr, ListeningAddrSize) == 0
            && listen(Socket, SOMAXCONN) == 0
            && (Event = WSACreateEvent()) != WSA_INVALID_EVENT
            && WSAEventSelect(Socket, Event, FD_ACCEPT) == 0
            && AddFileToIoQueue(Socket, IoQueue))
        {
            Result.Socket = (usz)Socket;
            Result.Event = (usz)Event;
            Result.Protocol = Protocol;
            CopyData(Result.SockAddr, sizeof(Result.SockAddr), ListeningAddr, ListeningAddrSize);
            Result.SockAddrSize = (u32)ListeningAddrSize;
            
        }
        else
        {
            int Error = GetLastError();
            closesocket(Socket);
        }
    }
    
    return Result;
}

external file
SetupConnectionSocket(ts_protocol Protocol, u16 Port, ts_io_queue IoQueue)
{
    file Result = INVALID_SOCKET;
    
    SOCKET Socket = OpenNewSocket(Protocol);
    if (Socket != INVALID_SOCKET)
    {
        u8 ConnectionAddr[sizeof(SOCKADDR_BTH)] = {0};
        i32 ConnectionAddrSize = 0;
        
        switch (Protocol)
        {
            case Proto_TCPIP4:
            case Proto_UDPIP4:
            {
                SOCKADDR_IN* Addr = (SOCKADDR_IN*)ConnectionAddr;
                Addr->sin_family = (ADDRESS_FAMILY)AF_INET;
                Addr->sin_port = FlipEndian16(Port);
                Addr->sin_addr.s_addr = INADDR_ANY;
                ConnectionAddrSize = sizeof(SOCKADDR_IN);
            } break;
            
            case Proto_TCPIP6:
            case Proto_UDPIP6:
            {
                SOCKADDR_IN6* Addr = (SOCKADDR_IN6*)ConnectionAddr;
                Addr->sin6_family = AF_INET6;
                Addr->sin6_port = FlipEndian16(Port);
                Addr->sin6_addr = in6addr_any;
                ConnectionAddrSize = sizeof(SOCKADDR_IN6);
            } break;
            
            case Proto_Bluetooth:
            {
                // TODO: Fill Bluetooth struct.
            } break;
        }
        
        if (bind(Socket, (SOCKADDR*)ConnectionAddr, ConnectionAddrSize) == 0
            && AddFileToIoQueue(Socket, IoQueue))
        {
            Result = (file)Socket;
        }
        else
        {
            closesocket(Socket);
        }
    }
    
    return Result;
}

external ts_event
ListenForEvents(usz* Sockets, usz* Events, usz NumEvents)
{
    ts_event Result = Event_None;
    
    DWORD Signaled = WSAWaitForMultipleEvents(NumEvents, (WSAEVENT*)Events, FALSE, WSA_INFINITE, FALSE);
    if (Signaled != WSA_WAIT_FAILED)
    {
        u32 Idx = Signaled - WSA_WAIT_EVENT_0;
        
        SOCKET Socket = (SOCKET)Sockets[Idx];
        WSAEVENT Event = (WSAEVENT)Events[Idx];
        WSANETWORKEVENTS EventType;
        if (WSAEnumNetworkEvents(Socket, Event, &EventType) == 0)
        {
            switch (EventType.lNetworkEvents)
            {
                case FD_ACCEPT: Result = Event_Accept; break;
                case FD_CONNECT: Result = Event_Connect; break;
                case FD_READ: Result = Event_Read; break;
                case FD_WRITE: Result = Event_Write; break;
                case FD_CLOSE: Result = Event_Close; break;
                default: Result = Event_Unknown;
            }
        }
        WSAResetEvent(Event);
    }
    
    return Result;
}

external u32
WaitOnIoQueue(ts_io_queue IoQueue, ts_io** Conn)
{
    HANDLE IoCP = *(HANDLE*)IoQueue.Data;
    DWORD BytesTransferred;
    ULONG_PTR CompletionKey;
    OVERLAPPED* Overlapped;
    GetQueuedCompletionStatus(IoCP, &BytesTransferred, &CompletionKey, &Overlapped, INFINITE);
    
    *Conn = (ts_io*)((u8*)Overlapped - sizeof(void*));
    return (u32)BytesTransferred;
}

external bool
SendToIoQueue(ts_io_queue IoQueue, ts_io* Conn)
{
    HANDLE IoCP = *(HANDLE*)IoQueue.Data;
    ULONG_PTR CompletionKey = 0;
    OVERLAPPED* Overlapped = (OVERLAPPED*)Conn->Async.Data;
    bool Result = PostQueuedCompletionStatus(IoCP, 0, CompletionKey, Overlapped);
    return Result;
}

external bool
AddFileToIoQueue(file File, ts_io_queue IoQueue)
{
    HANDLE IoCP = *(HANDLE*)IoQueue.Data;
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
        if (AddFileToIoQueue(Conn->Socket, IoQueue))
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
        if (!AddFileToIoQueue(Conn->Socket, IoQueue))
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
    bool Result = shutdown((SOCKET)Conn->Socket, SD_BOTH) == 0;
    return Result;
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

#include <ws2tcpip.h>
#include <ws2bth.h>
#include <winternl.h>
#include <mswsock.h>

#include "tinyserver-internal.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "ntdll.lib")

thread_local OVERLAPPED_ENTRY IoEvents[MAX_DEQUEUE] = {0};
thread_local ULONG IoEventCount = 0;
thread_local ULONG CurrentIoEventIdx = -1;


//==============================
// Function pointers
//==============================

bool (*TSAcceptEx)(SOCKET, SOCKET, void*, DWORD, DWORD, DWORD, DWORD*, OVERLAPPED*);
bool (*TSConnectEx)(SOCKET, SOCKADDR*, int, void*, DWORD, DWORD*, OVERLAPPED*);
bool (*TSDisconnectEx)(SOCKET, OVERLAPPED*, DWORD, DWORD);
bool (*TSTransmitPackets)(SOCKET, TRANSMIT_PACKETS_ELEMENT*, DWORD, DWORD, OVERLAPPED*, DWORD);

internal bool _AcceptConnSimple(ts_listen, ts_io*, void*, u32);
internal bool _AcceptConnEx(ts_listen, ts_io*, void*, u32);
internal bool _CreateConnSimple(ts_io*, void*, u32, void*, u32);
internal bool _CreateConnEx(ts_io*, void*, u32, void*, u32);
internal bool _DisconnectSocketSimple(ts_io*);
internal bool _DisconnectSocketEx(ts_io*);
internal bool _SendPacketsSimple(ts_io*, void*, u32);
internal bool _SendPacketsEx(ts_io*, void*, u32);
internal bool _RecvPackets(ts_io*, void*, u32);


//==============================
// Internal (Auxiliary)
//==============================

internal file
CreateIoQueue(void)
{
    HANDLE IoCP = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, (ULONG_PTR)NULL, 0);
    file Result = (IoCP != NULL) ? (file)IoCP : INVALID_FILE;
    return Result;
}


//==============================
// Setup
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
             &TSAcceptEx, sizeof(TSAcceptEx), &BytesReturned, NULL, NULL);
    AcceptConn = (TSAcceptEx) ? _AcceptConnEx : _AcceptConnSimple;
    
    // ConnectEx()
    GUID GuidConnectEx = WSAID_CONNECTEX;
    WSAIoctl(IoctlSocket, SIO_GET_EXTENSION_FUNCTION_POINTER, &GuidConnectEx, sizeof(GUID),
             &TSConnectEx, sizeof(TSConnectEx), &BytesReturned, NULL, NULL);
    CreateConn = (TSConnectEx) ? _CreateConnEx : _CreateConnSimple;
    
    // DisconnectEx()
    GUID GuidDisconnectEx = WSAID_DISCONNECTEX;
    WSAIoctl(IoctlSocket, SIO_GET_EXTENSION_FUNCTION_POINTER, &GuidDisconnectEx, sizeof(GUID),
             &TSDisconnectEx, sizeof(TSDisconnectEx), &BytesReturned, NULL, NULL);
    DisconnectSocket = (TSDisconnectEx) ? _DisconnectSocketEx : _DisconnectSocketSimple;
    
    // TransmitPackets()
    GUID GuidTransmitPackets = WSAID_TRANSMITPACKETS;
    WSAIoctl(IoctlSocket, SIO_GET_EXTENSION_FUNCTION_POINTER, &GuidTransmitPackets,
             sizeof(GUID), &TSTransmitPackets, sizeof(TSTransmitPackets), &BytesReturned,
             NULL, NULL);
    SendPackets = ((TSTransmitPackets && gSysInfo.OSVersion[0] == 'S')
                   ? _SendPacketsEx
                   : _SendPacketsSimple);
    
    RecvPackets = _RecvPackets;
    
    closesocket(IoctlSocket);
    
    gServerArena = GetMemory(INIT_ARENA_SIZE, 0, MEM_READ|MEM_WRITE);
    if (!gServerArena.Base)
    {
        return false;
    }
    ts_server_info* ServerInfo = PushStruct(gServerArena, ts_server_info);
    ServerInfo->IoQueue = CreateIoQueue();
    ServerInfo->CurrentAcceptIdx = ServerInfo->CurrentIoIdx = USZ_MAX;
    
    return true;
}

external void
CloseServer(void)
{
    if (gServerArena.Base)
    {
        ts_server_info* ServerInfo = (ts_server_info*)gServerArena.Base;
        
        // TODO: close IoQueue em funcao propria.
        
        FreeMemory(&gServerArena);
    }
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
        default: { AF = 0; Type = 0; Proto = 0; }
    }
    
    file Result = (file)WSASocketA(AF, Type, Proto, NULL, 0, WSA_FLAG_OVERLAPPED);
    return Result;
}

external bool
CloseSocket(ts_io* Conn)
{
    if (closesocket(Conn->Socket) == 0)
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
    HANDLE IoCP = (HANDLE)ServerInfo->IoQueue;
    HANDLE Verify = CreateIoCompletionPort((HANDLE)File, IoCP, (ULONG_PTR)RelatedData, 0);
    return (Verify == IoCP);
}

external ts_sockaddr
CreateSockAddr(char* IpAddress, u16 Port, ts_protocol Protocol)
{
    ts_sockaddr Result = {0};
    if (Protocol == Proto_TCPIP4 || Protocol == Proto_UDPIP4)
    {
        SOCKADDR_IN* Addr = (SOCKADDR_IN*)Result.Addr;
        Addr->sin_family = (ADDRESS_FAMILY)AF_INET;
        Addr->sin_port = FlipEndian16(Port);
        inet_pton(AF_INET, IpAddress, &Addr->sin_addr);
        Result.Size = sizeof(SOCKADDR_IN);
    }
    else if (Protocol == Proto_TCPIP6 || Protocol == Proto_UDPIP6)
    {
        SOCKADDR_IN6* Addr = (SOCKADDR_IN6*)Result.Addr;
        Addr->sin6_family = (ADDRESS_FAMILY)AF_INET6;
        Addr->sin6_port = FlipEndian16(Port);
        inet_pton(AF_INET6, IpAddress, &Addr->sin6_addr);
        Result.Size = sizeof(SOCKADDR_IN6);
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
        u8 ListenAddr[sizeof(SOCKADDR_IN6)] = {0};
        i32 ListenAddrSize = 0;
        
        switch (Protocol)
        {
            case Proto_TCPIP4:
            case Proto_UDPIP4:
            {
                SOCKADDR_IN* Addr = (SOCKADDR_IN*)ListenAddr;
                Addr->sin_family = (ADDRESS_FAMILY)AF_INET;
                Addr->sin_port = FlipEndian16(Port);
                Addr->sin_addr.s_addr = INADDR_ANY;
                ListenAddrSize = sizeof(SOCKADDR_IN);
            } break;
            
            case Proto_TCPIP6:
            case Proto_UDPIP6:
            {
                SOCKADDR_IN6* Addr = (SOCKADDR_IN6*)ListenAddr;
                Addr->sin6_family = AF_INET6;
                Addr->sin6_port = FlipEndian16(Port);
                Addr->sin6_addr = in6addr_any;
                ListenAddrSize = sizeof(SOCKADDR_IN6);
            } break;
        }
        
        WSAEVENT Event = 0;
        if (bind((SOCKET)Socket, (SOCKADDR*)ListenAddr, ListenAddrSize) == 0
            && listen((SOCKET)Socket, SOMAXCONN) == 0
            && (Event = WSACreateEvent()) != INVALID_HANDLE_VALUE
            && WSAEventSelect((SOCKET)Socket, Event, FD_ACCEPT) == 0)
        {
            ts_server_info* ServerInfo = (ts_server_info*)gServerArena.Base;
            
            ts_listen* Listen = PushStruct(gServerArena, ts_listen);
            ServerInfo->SockCount++;
            
            Listen->Socket = Socket;
            Listen->Event = (file)Event;
            Listen->Protocol = Protocol;
            Listen.SockAddrSize = (u32)ListenAddrSize;
            
            Result = BindFileToIoQueue(Socket, Listen);
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

external ts_listen
ListenForConnections(void)
{
    ts_server_info* ServerInfo = (ts_server_info*)gServerArena.Base;
    ts_listen* ListenList = (ts_listen*)&ServerInfo[1];
    
    // WSAWaitForMultipleEvents() expects an array of WSAEVENT objects. The code
    // block below runs only the first time this is called, setting up the array
    // in memory and appending the listening sockets to the IoQueue.
    
    if (!ServerInfo->ReadyToPoll)
    {
        file* EventPtr = PushArray(gServerArena, ServerInfo->ListenCount, file);
        for (usz Idx = 0; Idx < ServerInfo->ListenCount; Idx++)
        {
            EventPtr[Idx] = ListenList[Idx].Event;
        }
        ServerInfo->AcceptPtr = (u8*)EventPtr;
        ServerInfo->ReadyToPoll = true;
    }
    
    WSAEVENT* EventList = (WSAEVENT*)ServerInfo->AcceptPtr;
    
    // Event processing happens in two steps: 1) call polling function, 2) test
    // each socket to see if signaled. ListenForConnections() bundles them both
    // in one call. First time called it will execute step #1 and #2, with #2
    // looping over sockets and returns on first found signaled. Subsequent calls
    // will only execute #2, continuing where the previous call stopped. After all
    // sockets are checked we have to execute #1 again.
    
    if (ServerInfo->CurrentAcceptIdx == USZ_MAX)
    {
        DWORD Signaled = WSAWaitForMultipleEvents(ServerInfo->ListenCount, EventList,
                                                  FALSE, WSA_INFINITE, FALSE);
        if (Signaled == WSA_WAIT_FAILED)
        {
            // This is the only codepath that exits ListenForConnections() on error.
            ts_listen EmptyError = {0};
            return EmptyError;
        }
        ServerInfo->CurrentAcceptIdx = Signaled - WSA_WAIT_EVENT_0;
    }
    
    while (ServerInfo->CurrentAcceptIdx < ServerInfo->ListenCount)
    {
        ts_listen Listen = ListenList[ListenList->CurrentAcceptIdx];
        WSAEVENT Event = EventList[ListenList->CurrentAcceptIdx++];
        
        WSANETWORKEVENTS EventType;
        if (WSAEnumNetworkEvents((SOCKET)Listen.Socket, Event, &EventType) == 0
            && EventType.lNetworkEvents == FD_ACCEPT
            && EventType.iErrorCode[FD_ACCEPT_BIT] == 0)
        {
            WSAResetEvent(Event);
            return Listen;
        }
    }
    
    // If it got here, there is no sockets left to check, meaning we need to call
    // WSAWaitForMultipleEvents() again. Reset [CurrentAcceptIdx] to default.
    
    ServerInfo->CurrentAcceptIdx == USZ_MAX;
    return ListenForConnections();
}

external ts_io*
WaitOnIoQueue(void)
{
    ts_server_info* ServerInfo = (ts_server_info*)gServerArena.Base;
    
    // First time calling this function it runs GetQueuedCompletionStatusEx() and
    // gets a list of sockets with completed IOs; it then returns the first one.
    // Subsequent calls will advance on the list, continuing where the previous call
    // stopped. After all sockets are checked we have to execute the function again.
    
    if (CurrentIoEventIdx == -1)
    {
        HANDLE IoCP = (HANDLE)ServerInfo->IoQueue;
        GetQueuedCompletionStatusEx(IoCP, IoEvents, MAX_DEQUEUE, &IoEventCount,
                                    INFINITE, FALSE);
        CurrentIoEventIdx = 0;
    }
    
    while (CurrentIoEventIdx < IoEventCount)
    {
        OVERLAPPED_ENTRY Event = IoEvents[CurrentIoEventIdx++];
        if (Event.lpOverlapped->Internal != STATUS_PENDING)
        {
            ts_io* Conn = (ts_io*)Event.lpCompletionKey;
            Conn->BytesTransferred = (usz)Event.dwNumberOfBytesTransferred;
            OVERLAPPED* Overlapped = (OVERLAPPED*)Conn->InternalData;
            if (RtlNtStatusToDosError(Overlapped->Internal) != ERROR_SUCCESS)
            {
                Conn->Status = Status_Error;
            }
            return Conn;
        }
    }
    
    // If it got here, there is no sockets left to check, meaning we need to call
    // GetQueuedCompletionStatusEx() again. Reset [CurrentIoEventIdx] to default.
    
    CurrentIoEventIdx = -1;
    return WaitOnIoQueue();
}

external bool
SendToIoQueue(ts_io* Conn)
{
    HANDLE IoCP = (HANDLE)IoQueue;
    ULONG_PTR CompletionKey = 0;
    OVERLAPPED* Overlapped = (OVERLAPPED*)Conn->InternalData;
    BOOL Result = PostQueuedCompletionStatus(IoCP, 0, CompletionKey, Overlapped);
    return Result;
}

//==============================
// Socket IO
//==============================

internal bool
_AcceptConnSimple(ts_listen Listening, ts_io* Conn, void* Buffer, u32 BufferSize)
{
    ts_server_info* ServerInfo = (ts_server_info*)gServerArena.Base;
    
    u8 RemoteSockAddr[MAX_SOCKADDR_SIZE] = {0};
    i32 RemoteSockAddrSize;
    
    SOCKET Socket = WSAAccept((SOCKET)Listening.Socket, (SOCKADDR*)RemoteSockAddr,
                              &RemoteSockAddrSize, NULL, (DWORD_PTR)NULL);
    if (Socket != INVALID_SOCKET
        && BindFileToIoQueue(Conn->Socket, ServerInfo->IoQueue, Conn))
    {
        Conn->Socket = (file)Socket;
        Conn->Status = Status_Connected;
        
        u32 TotalAddrSize = RemoteSockAddrSize + 0x10;
        u8* AddrBuffer = (u8*)Buffer + BufferSize - TotalAddrSize;
        CopyData(AddrBuffer, RemoteSockAddrSize, RemoteSockAddr, RemoteSockAddrSize);
        u32 ReadBufferSize = BufferSize - TotalAddrSize;
        
        return RecvPackets(Conn, &Buffer, &ReadBufferSize, 1);
    }
    
    closesocket(Socket);
    Conn->Socket = (usz)INVALID_HANDLE_VALUE;
    return false;
}

internal bool
_AcceptConnEx(ts_listen Listening, ts_io* Conn, void* Buffer, u32 BufferSize, file IoQueue)
{
    ts_server_info* ServerInfo = (ts_server_info*)gServerArena.Base;
    
    DWORD LocalAddrSize = 0x10 + Listening.SockAddrSize;
    DWORD RemoteAddrSize = 0x10 + Listening.SockAddrSize;
    
    DWORD InDataSize = BufferSize - LocalAddrSize - RemoteAddrSize;
    DWORD BytesRecv = 0;
    
    // If Conn->Socket is not initialized, initializes it.
    if (Conn->Socket == INVALID_SOCKET)
    {
        file Socket = OpenNewSocket(Listening.Protocol);
        if (!BindFileToIoQueue((SOCKET)Socket, ServerInfo->IoQueue, Conn))
        {
            closesocket((SOCKET)Socket);
            return false;
        }
        Conn->Socket = Socket;
    }
    
    WSAOVERLAPPED* Overlapped = (WSAOVERLAPPED*)Conn->InternalData;
    ClearMemory(Overlapped, sizeof(WSAOVERLAPPED));
    BOOL Result = TSAcceptEx((SOCKET)Listening.Socket, (SOCKET)Conn->Socket, Buffer,
                             InDataSize, LocalAddrSize, RemoteAddrSize, &BytesRecv,
                             Overlapped);
    if ((Result == TRUE) || (WSAGetLastError() == WSA_IO_PENDING))
    {
        Conn->Status = Status_Connected;
        return true;
    }
    return false;
}

internal bool
_CreateConnSimple(ts_io* Conn, void* DstSockAddr, u32 DstSockAddrSize, void* Buffer,
                  u32 BufferSize)
{
    if (connect((SOCKET)Conn->Socket, (SOCKADDR*)DstSockAddr, (int)DstSockAddrSize) == 0)
    {
        Conn->Status = Status_Connected;
        return SendPackets(Conn, &Buffer, &BufferSize, 1);
    }
    return false;
}

internal bool
_CreateConnEx(ts_io* Conn, void* DstSockAddr, u32 DstSockAddrSize, void* Buffer,
              u32 BufferSize)
{
    WSAOVERLAPPED* Overlapped = (WSAOVERLAPPED*)Conn->InternalData;
    ClearMemory(Overlapped, sizeof(WSAOVERLAPPED));
    DWORD BytesSent = 0;
    BOOL Result = TSConnectEx((SOCKET)Conn->Socket, (SOCKADDR*)DstSockAddr, DstSockAddrSize,
                              Buffer, BufferSize, &BytesSent, Overlapped);
    if ((Result == TRUE) || (WSAGetLastError() == WSA_IO_PENDING))
    {
        Conn->Status = Status_Connected;
        return true;
    }
    return false;
}

internal bool
_DisconnectSocketSimple(ts_io* Conn)
{
    if (shutdown((SOCKET)Conn->Socket, SD_BOTH) == 0)
    {
        Conn->Status = Status_Disconnected;
        return CloseSocket(Conn);
    }
    return false;
}

internal bool
_DisconnectSocketEx(ts_io* Conn)
{
    WSAOVERLAPPED* Overlapped = (WSAOVERLAPPED*)Conn->InternalData;
    ClearMemory(Overlapped, sizeof(WSAOVERLAPPED));
    BOOL Result = TSDisconnectEx((SOCKET)Conn->Socket, Overlapped, TF_REUSE_SOCKET, 0);
    if ((Result == TRUE) || (WSAGetLastError() == WSA_IO_PENDING))
    {
        Conn->Status = Status_Disconnected;
        return true;
    }
    return false;
}

internal bool
_SendPacketsSimple(ts_io* Conn, void* Buffer, u32 BufferSize)
{
    WSABUF Element = {0};
    Element.len = BufferSize;
    Element.buf = Buffer;
    
    WSAOVERLAPPED* Overlapped = (WSAOVERLAPPED*)Conn->InternalData;
    ClearMemory(Overlapped, sizeof(WSAOVERLAPPED));
    int Result = WSASend((SOCKET)Conn->Socket, Element, 1, NULL, 0, Overlapped, NULL);
    return ((Result == 0) || (WSAGetLastError() == WSA_IO_PENDING));
}

internal bool
_SendPacketsEx(ts_io* Conn, void* Buffer, u32 BufferSize)
{
    TRANSMIT_PACKETS_ELEMENT Element = {0};
    Element.dwElFlags = TP_ELEMENT_MEMORY;
    Element.cLength = BufferSize;
    Element.pBuffer = Buffer;
    
    WSAOVERLAPPED* Overlapped = (WSAOVERLAPPED*)Conn->InternalData;
    ClearMemory(Overlapped, sizeof(WSAOVERLAPPED));
    BOOL Result = TSTransmitPackets((SOCKET)Conn->Socket, Element, 1, 0, Overlapped,
                                    TF_USE_KERNEL_APC);
    return ((Result == TRUE) || (WSAGetLastError() == WSA_IO_PENDING));
}

internal bool
_RecvPackets(ts_io* Conn, void* Buffer, u32 BufferSize)
{
    WSABUF Element = {0};
    Element.len = BufferSize;
    Element.buf = Buffer;
    
    WSAOVERLAPPED* Overlapped = (WSAOVERLAPPED*)Conn->InternalData;
    ClearMemory(Overlapped, sizeof(WSAOVERLAPPED));
    DWORD Flags = 0;
    int Result = WSARecv((SOCKET)Conn->Socket, Element, 1, NULL, &Flags, Overlapped, NULL);
    return ((Result == 0) || (WSAGetLastError() == WSA_IO_PENDING));
}

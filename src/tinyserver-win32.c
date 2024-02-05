#include <ws2tcpip.h>
#include <ws2bth.h>
#include <winternl.h>
#include <mswsock.h>

#include "tinyserver-internal.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "ntdll.lib")

//==============================
// Function pointers
//==============================

bool (*TSAcceptEx)(SOCKET, SOCKET, void*, DWORD, DWORD, DWORD, DWORD*, OVERLAPPED*);
bool (*TSConnectEx)(SOCKET, SOCKADDR*, int, void*, DWORD, DWORD*, OVERLAPPED*);
bool (*TSDisconnectEx)(SOCKET, OVERLAPPED*, DWORD, DWORD);
bool (*TSTransmitPackets)(SOCKET, TRANSMIT_PACKETS_ELEMENT*, DWORD, DWORD, OVERLAPPED*, DWORD);
bool (*TSTransmitFile)(SOCKET, HANDLE, DWORD, DWORD, LPOVERLAPPED, LPTRANSMIT_FILE_BUFFERS, DWORD);

internal bool _AcceptConnSimple(ts_listen, ts_io*);
internal bool _AcceptConnEx(ts_listen, ts_io*);
internal bool _CreateConnSimple(ts_io*, ts_sockaddr);
internal bool _CreateConnEx(ts_io*, ts_sockaddr);
internal bool _DisconnectSocketSimple(ts_io*);
internal bool _DisconnectSocketEx(ts_io*);
internal bool _SendDataSimple(ts_io*);
internal bool _SendDataEx(ts_io*);
internal bool _SendFileSimple(ts_io*);
internal bool _SendFileEx(ts_io*);
internal bool _RecvData(ts_io*);


//==============================
// Internal (Auxiliary)
//==============================

thread_local OVERLAPPED_ENTRY tIoEvents[MAX_DEQUEUE] = {0};
thread_local ULONG tIoEventCount = 0;
thread_local ULONG tCurrentIoEventIdx = -1;

// This is what [.InternalData] member of ts_io translates to.
typedef struct ts_internal
{
    WSAOVERLAPPED Overlapped;
    u8* Base; // Used on Simple version of SendFile.
    u32 FileSize; // Used on Simple version of SendFile.
    u32 SendOffset; // Used on Simple version of SendFile.
} ts_internal;

internal file
CreateIoQueue(void)
{
    HANDLE IoCP = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, (ULONG_PTR)NULL, 0);
    file Result = (IoCP != NULL) ? (file)IoCP : INVALID_FILE;
    return Result;
}

internal file
OpenNewSocket(ts_protocol Protocol)
{
    int AF, Type, Proto;
    switch (Protocol)
    {
        case Proto_TCPIP4:
        { AF = AF_INET; Type = SOCK_STREAM; Proto = IPPROTO_TCP; } break;
        case Proto_TCPIP6:
        { AF = AF_INET6; Type = SOCK_STREAM; Proto = IPPROTO_TCP; } break;
        case Proto_UDPIP4:
        { AF = AF_INET; Type = SOCK_DGRAM; Proto = IPPROTO_UDP; } break;
        case Proto_UDPIP6:
        { AF = AF_INET6; Type = SOCK_DGRAM; Proto = IPPROTO_UDP; } break;
        default:
        { AF = 0; Type = 0; Proto = 0; }
    }
    
    SOCKET NewSocket = WSASocketA(AF, Type, Proto, NULL, 0, WSA_FLAG_OVERLAPPED);
    file Result = (NewSocket == INVALID_SOCKET) ? INVALID_FILE : (file)NewSocket;
    return Result;
}

internal bool
CloseSocket(ts_io* Conn)
{
    if (closesocket(Conn->Socket) == 0)
    {
        Conn->Status = Status_None;
        return true;
    }
    return false;
}

internal bool
BindFileToIoQueue(file File, _opt void* RelatedData)
{
    ts_server_info* ServerInfo = (ts_server_info*)gServerArena.Base;
    HANDLE IoCP = (HANDLE)ServerInfo->IoQueue;
    HANDLE Verify = CreateIoCompletionPort((HANDLE)File, IoCP, (ULONG_PTR)RelatedData, 0);
    return (Verify == IoCP);
}


//==============================
// Setup
//==============================

#define TS_ARENA_SIZE Kilobyte(1)

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
    WSAIoctl(IoctlSocket, SIO_GET_EXTENSION_FUNCTION_POINTER, &GuidAcceptEx,
             sizeof(GUID), &TSAcceptEx, sizeof(TSAcceptEx), &BytesReturned, NULL, NULL);
    AcceptConn = (TSAcceptEx) ? _AcceptConnEx : _AcceptConnSimple;
    
    // ConnectEx()
    GUID GuidConnectEx = WSAID_CONNECTEX;
    WSAIoctl(IoctlSocket, SIO_GET_EXTENSION_FUNCTION_POINTER, &GuidConnectEx,
             sizeof(GUID), &TSConnectEx, sizeof(TSConnectEx), &BytesReturned, NULL, NULL);
    CreateConn = (TSConnectEx) ? _CreateConnEx : _CreateConnSimple;
    
    // DisconnectEx()
    GUID GuidDisconnectEx = WSAID_DISCONNECTEX;
    WSAIoctl(IoctlSocket, SIO_GET_EXTENSION_FUNCTION_POINTER, &GuidDisconnectEx,
             sizeof(GUID), &TSDisconnectEx, sizeof(TSDisconnectEx), &BytesReturned,
             NULL, NULL);
    DisconnectSocket = (TSDisconnectEx) ? _DisconnectSocketEx : _DisconnectSocketSimple;
    
    // TransmitPackets()
    GUID GuidTransmitPackets = WSAID_TRANSMITPACKETS;
    WSAIoctl(IoctlSocket, SIO_GET_EXTENSION_FUNCTION_POINTER, &GuidTransmitPackets,
             sizeof(GUID), &TSTransmitPackets, sizeof(TSTransmitPackets), &BytesReturned,
             NULL, NULL);
    SendData = ((TSTransmitPackets && gSysInfo.OSVersion[0] == 'S')
                ? _SendDataEx
                : _SendDataSimple);
    
    // TransmitFile()
    GUID GuidTransmitFile = WSAID_TRANSMITFILE;
    WSAIoctl(IoctlSocket, SIO_GET_EXTENSION_FUNCTION_POINTER, &GuidTransmitFile, 
             sizeof(GUID), &TSTransmitFile, sizeof(TSTransmitFile), &BytesReturned,
             NULL, NULL);
    SendFile = ((TSTransmitFile && gSysInfo.OSVersion[0] == 'S')
                ? _SendFileEx
                : _SendFileSimple);
    
    closesocket(IoctlSocket);
    
    RecvData = _RecvData;
    
    // Sets up the internal auxiliary memory arena.
    
    gServerArena = GetMemory(TS_ARENA_SIZE, 0, MEM_READ|MEM_WRITE);
    if (!gServerArena.Base)
    {
        return false;
    }
    ts_server_info* ServerInfo = PushStruct(&gServerArena, ts_server_info);
    ServerInfo->IoQueue = CreateIoQueue();
    ServerInfo->CurrentAcceptIdx = USZ_MAX;
    
    return true;
}

external void
CloseServer(void)
{
    if (gServerArena.Base)
    {
        ts_server_info* ServerInfo = (ts_server_info*)gServerArena.Base;
        FreeMemory(&gServerArena);
    }
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
            
            ts_listen* Listen = PushStruct(&gServerArena, ts_listen);
            ServerInfo->ListenCount++;
            
            Listen->Socket = Socket;
            Listen->Event = (file)Event;
            Listen->Protocol = Protocol;
            Listen->SockAddrSize = (u32)ListenAddrSize;
            
            Result = BindFileToIoQueue(Socket, Listen);
        }
        else
        {
            closesocket((SOCKET)Socket);
            CloseHandle(Event);
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
        file* EventPtr = PushArray(&gServerArena, ServerInfo->ListenCount, file);
        for (usz Idx = 0; Idx < ServerInfo->ListenCount; Idx++)
        {
            EventPtr[Idx] = ListenList[Idx].Event;
        }
        ServerInfo->AcceptEvents = (u8*)EventPtr;
        ServerInfo->ReadyToPoll = true;
    }
    
    WSAEVENT* EventList = (WSAEVENT*)ServerInfo->AcceptEvents;
    
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
            ts_listen ErrorResult = {0};
            ErrorResult.Socket = INVALID_FILE;
            return ErrorResult;
        }
        ServerInfo->CurrentAcceptIdx = Signaled - WSA_WAIT_EVENT_0;
    }
    
    while (ServerInfo->CurrentAcceptIdx < ServerInfo->ListenCount)
    {
        ts_listen Listen = ListenList[ServerInfo->CurrentAcceptIdx];
        WSAEVENT Event = EventList[ServerInfo->CurrentAcceptIdx++];
        
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
    
    ServerInfo->CurrentAcceptIdx = USZ_MAX;
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
    
    if (tCurrentIoEventIdx == -1)
    {
        HANDLE IoCP = (HANDLE)ServerInfo->IoQueue;
        GetQueuedCompletionStatusEx(IoCP, tIoEvents, MAX_DEQUEUE, &tIoEventCount,
                                    INFINITE, FALSE);
        tCurrentIoEventIdx = 0;
    }
    
    while (tCurrentIoEventIdx < tIoEventCount)
    {
        OVERLAPPED_ENTRY Event = tIoEvents[tCurrentIoEventIdx++];
        if (Event.lpOverlapped->Internal != STATUS_PENDING)
        {
            ts_io* Conn = (ts_io*)Event.lpOverlapped;
            Conn->BytesTransferred = (usz)Event.dwNumberOfBytesTransferred;
            ts_internal* Internal = (ts_internal*)Conn->InternalData;
            
            if (RtlNtStatusToDosError(Internal->Overlapped.Internal) == ERROR_SUCCESS)
            {
                if (Conn->Operation == Op_RecvData
                    || Conn->Operation == Op_AcceptConn)
                {
                    // If the socket is in TCP connection and received 0 bytes,
                    // that indicates that the peer has shut down the connection.
                    
                    if (Conn->BytesTransferred == 0
                        && Conn->Status == Status_Connected)
                    {
                        Conn->Status = Status_Aborted;
                    }
                }
                else if (Conn->Operation == Op_SendFile)
                {
                    // Check to see if the entire send operation completed. If it
                    // did, and the function used is _SendFileSimple, release the
                    // file read memory..
                    
                    if (Conn->BytesTransferred == Conn->IoSize
                        && SendFile == _SendFileSimple)
                    {
                        buffer FileBuffer = Buffer((ts_internal*)Internal->Base, 0, 0);
                        FreeMemory(&FileBuffer);
                    }
                }
                else if (Conn->Operation == Op_None)
                {
                    // Not meant to get here, assumed to be an error.
                    Conn->Status = Status_Error;
                }
            }
            else
            {
                Conn->Status = Status_Error;
            }
            
            return Conn;
        }
    }
    
    // If it got here, there is no sockets left to check, meaning we need to call
    // GetQueuedCompletionStatusEx() again. Reset [CurrentIoEventIdx] to default.
    
    tCurrentIoEventIdx = -1;
    return WaitOnIoQueue();
}

external bool
SendToIoQueue(ts_io* Conn)
{
    Conn->Operation = Op_SendToIoQueue;
    ts_server_info* ServerInfo = (ts_server_info*)gServerArena.Base;
    
    HANDLE IoCP = (HANDLE)ServerInfo->IoQueue;
    ULONG_PTR CompletionKey = (ULONG_PTR)Conn;
    ts_internal* Internal = (ts_internal*)Conn->InternalData;
    BOOL Result = PostQueuedCompletionStatus(IoCP, 0, CompletionKey,
                                             &Internal->Overlapped);
    return Result;
}

//==============================
// Socket IO
//==============================

internal bool
_AcceptConnSimple(ts_listen Listening, ts_io* Conn)
{
    Conn->Operation = Op_AcceptConn;
    ts_server_info* ServerInfo = (ts_server_info*)gServerArena.Base;
    
    u8 RemoteSockAddr[MAX_SOCKADDR_SIZE] = {0};
    i32 RemoteSockAddrSize;
    
    SOCKET Socket = WSAAccept((SOCKET)Listening.Socket, (SOCKADDR*)RemoteSockAddr,
                              &RemoteSockAddrSize, NULL, (DWORD_PTR)NULL);
    if (Socket != INVALID_SOCKET
        && BindFileToIoQueue(Conn->Socket, Conn))
    {
        Conn->Socket = (file)Socket;
        Conn->Status = Status_Connected;
        
        if (Conn->IoBuffer)
        {
            u32 TotalAddrSize = RemoteSockAddrSize + 0x10;
            u8* AddrBuffer = (u8*)Conn->IoBuffer + Conn->IoSize - TotalAddrSize;
            CopyData(AddrBuffer, RemoteSockAddrSize, RemoteSockAddr, RemoteSockAddrSize);
            Conn->IoSize -= TotalAddrSize;
            
            WSABUF Element = {0};
            Element.len = Conn->IoSize;
            Element.buf = (CHAR*)Conn->IoBuffer;
            
            ts_internal* Internal = (ts_internal*)Conn->InternalData;
            memset(&Internal->Overlapped, 0, sizeof(WSAOVERLAPPED));
            DWORD Flags = 0;
            int Result = WSARecv((SOCKET)Conn->Socket, &Element, 1, NULL, &Flags,
                                 &Internal->Overlapped, NULL);
            return ((Result == 0) || (WSAGetLastError() == WSA_IO_PENDING));
        }
        return SendToIoQueue(Conn);
    }
    
    closesocket(Socket);
    Conn->Socket = INVALID_FILE;
    Conn->Status = Status_Error;
    return false;
}

internal bool
_AcceptConnEx(ts_listen Listening, ts_io* Conn)
{
    Conn->Operation = Op_AcceptConn;
    ts_server_info* ServerInfo = (ts_server_info*)gServerArena.Base;
    
    file Socket = OpenNewSocket(Listening.Protocol);
    if (Socket != INVALID_FILE
        && BindFileToIoQueue((SOCKET)Socket, Conn))
    {
        Conn->Socket = Socket;
        Conn->Status = Status_Connected;
        
        DWORD LocalAddrSize = 0x10 + Listening.SockAddrSize;
        DWORD RemoteAddrSize = 0x10 + Listening.SockAddrSize;
        usz CombinedAddrSize = LocalAddrSize + RemoteAddrSize;
        Conn->IoSize -= CombinedAddrSize;
        
        ts_internal* Internal = (ts_internal*)Conn->InternalData;
        memset(&Internal->Overlapped, 0, sizeof(WSAOVERLAPPED));
        DWORD BytesRecv = 0;
        BOOL Result = TSAcceptEx((SOCKET)Listening.Socket, (SOCKET)Conn->Socket,
                                 Conn->IoBuffer, Conn->IoSize, LocalAddrSize,
                                 RemoteAddrSize, &BytesRecv, &Internal->Overlapped);
        if ((Result == TRUE) || (WSAGetLastError() == WSA_IO_PENDING))
        {
            // Setting it here is not ideal, but the listening socket is necessary.
            setsockopt((SOCKET)Conn->Socket, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT,
                       (char*)&Listening.Socket, sizeof(Listening.Socket));
            return true;
        }
    }
    
    closesocket((SOCKET)Socket);
    Conn->Socket = INVALID_FILE;
    Conn->Status = Status_Error;
    return false;
}

internal bool
_CreateConnSimple(ts_io* Conn, ts_sockaddr SockAddr)
{
    Conn->Operation = Op_CreateConn;
    if (connect((SOCKET)Conn->Socket, (SOCKADDR*)SockAddr.Addr, (int)SockAddr.Size) == 0)
    {
        Conn->Status = Status_Connected;
        if (Conn->IoBuffer)
        {
            return SendData(Conn);
        }
        return SendToIoQueue(Conn);
    }
    Conn->Status = Status_Error;
    return false;
}

internal bool
_CreateConnEx(ts_io* Conn, ts_sockaddr SockAddr)
{
    Conn->Operation = Op_CreateConn;
    ts_internal* Internal = (ts_internal*)Conn->InternalData;
    memset(&Internal->Overlapped, 0, sizeof(WSAOVERLAPPED));
    
    Conn->Status = Status_Connected;
    DWORD BytesSent = 0;
    BOOL Result = TSConnectEx((SOCKET)Conn->Socket, (SOCKADDR*)SockAddr.Addr,
                              SockAddr.Size, Conn->IoBuffer, Conn->IoSize,
                              &BytesSent, &Internal->Overlapped);
    if ((Result == TRUE) || (WSAGetLastError() == WSA_IO_PENDING))
    {
        return true;
    }
    Conn->Status = Status_Error;
    return false;
}

internal bool
_DisconnectSocketSimple(ts_io* Conn)
{
    Conn->Operation = Op_DisconnectSocket;
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
    Conn->Operation = Op_DisconnectSocket;
    ts_internal* Internal = (ts_internal*)Conn->InternalData;
    memset(&Internal->Overlapped, 0, sizeof(WSAOVERLAPPED));
    
    ts_status PrevStatus = Conn->Status;
    Conn->Status = Status_Disconnected;
    BOOL Result = TSDisconnectEx((SOCKET)Conn->Socket, &Internal->Overlapped,
                                 TF_REUSE_SOCKET, 0);
    if ((Result == TRUE) || (WSAGetLastError() == WSA_IO_PENDING))
    {
        return true;
    }
    Conn->Status = PrevStatus;
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
_SendDataSimple(ts_io* Conn)
{
    Conn->Operation = Op_SendData;
    
    WSABUF Element = {0};
    Element.len = Conn->IoSize;
    Element.buf = (CHAR*)Conn->IoBuffer;
    
    ts_internal* Internal = (ts_internal*)Conn->InternalData;
    memset(&Internal->Overlapped, 0, sizeof(WSAOVERLAPPED));
    int Result = WSASend((SOCKET)Conn->Socket, &Element, 1, NULL, 0,
                         &Internal->Overlapped, NULL);
    return ((Result == 0) || (WSAGetLastError() == WSA_IO_PENDING));
}

internal bool
_SendDataEx(ts_io* Conn)
{
    Conn->Operation = Op_SendData;
    
    TRANSMIT_PACKETS_ELEMENT Element = {0};
    Element.dwElFlags = TP_ELEMENT_MEMORY;
    Element.cLength = Conn->IoSize;
    Element.pBuffer = Conn->IoBuffer;
    
    ts_internal* Internal = (ts_internal*)Conn->InternalData;
    memset(&Internal->Overlapped, 0, sizeof(WSAOVERLAPPED));
    BOOL Result = TSTransmitPackets((SOCKET)Conn->Socket, &Element, 1, 0,
                                    &Internal->Overlapped, TF_USE_KERNEL_APC);
    return ((Result == TRUE) || (WSAGetLastError() == WSA_IO_PENDING));
}

internal bool
_SendFileSimple(ts_io* Conn)
{
    Conn->Operation = Op_SendFile;
    
    // This version of the function "fakes" a file transfer by creating a buffer,
    // reading the file into it, and then sending it instead. To know how much
    // to advance the buffer by, it saves the full filesize and subtracts the
    // [.IoBufferize] from it every new call.
    
    ts_internal* Internal = (ts_internal*)Conn->InternalData;
    WSABUF Element = {0};
    if (Internal->Base == NULL)
    {
        buffer ReadMem = GetMemory(Conn->IoSize, 0, MEM_WRITE);
        if (!ReadFromFile(Conn->IoFile, &ReadMem, Conn->IoSize, 0))
        {
            return false;
        }
        Internal->Base = ReadMem.Base;
        Element.buf = (CHAR*)Internal->Base;
        Element.len = Internal->FileSize = ReadMem.WriteCur;
    }
    else
    {
        u32 Offset = Internal->FileSize - Conn->IoSize;
        Internal->Base += Offset;
        Element.buf = (CHAR*)Internal->Base;
        Element.len = Internal->FileSize -= Offset;
    }
    
    memset(&Internal->Overlapped, 0, sizeof(WSAOVERLAPPED));
    int Result = WSASend((SOCKET)Conn->Socket, &Element, 1, NULL, 0,
                         &Internal->Overlapped, NULL);
    return ((Result == 0) || (WSAGetLastError() == WSA_IO_PENDING));
}

internal bool
_SendFileEx(ts_io* Conn)
{
    Conn->Operation = Op_SendFile;
    
    ts_internal* Internal = (ts_internal*)Conn->InternalData;
    memset(&Internal->Overlapped, 0, sizeof(WSAOVERLAPPED));
    BOOL Result = TSTransmitFile((SOCKET)Conn->Socket, (HANDLE)Conn->IoFile,
                                 Conn->IoSize, 0, &Internal->Overlapped, NULL,
                                 TF_USE_KERNEL_APC);
    return ((Result == 0) || (WSAGetLastError() == WSA_IO_PENDING));
}

internal bool
_RecvData(ts_io* Conn)
{
    Conn->Operation = Op_RecvData;
    
    WSABUF Element = {0};
    Element.len = Conn->IoSize;
    Element.buf = (CHAR*)Conn->IoBuffer;
    
    ts_internal* Internal = (ts_internal*)Conn->InternalData;
    memset(&Internal->Overlapped, 0, sizeof(WSAOVERLAPPED));
    DWORD Flags = 0;
    int Result = WSARecv((SOCKET)Conn->Socket, &Element, 1, NULL, &Flags,
                         &Internal->Overlapped, NULL);
    return ((Result == 0) || (WSAGetLastError() == WSA_IO_PENDING));
}

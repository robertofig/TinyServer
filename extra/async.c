#define _GNU_SOURCE

#include <tinybase-platform.h>
#include <stdio.h>

#include <linux/io_uring.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>


#define Assert(Exp) do { \
if(!(Exp)) { \
fprintf(stderr, "Err [line %d] - %s\n", __LINE__, strerror(errno)); \
return -1; \
}} while (0);

#define ReadBarrier() __asm__ __volatile__("":::"memory")
#define WriteBarrier() __asm__ __volatile__("":::"memory")

static const char Reply[] = \
"HTTP/1.1 200 OK\r\n"
"Server: My server\r\n" \
"Connection: keep-alive\r\n" \
"Content-Type: text/html\r\n" \
"Content-Length: 72\r\n" \
"\r\n" \
"<!DOCTYPE html><html><body><a href=\"index2.html\">Click</a></body></html>";

static const char Err404[] = \
"HTTP/1.1 404 Not Found\r\n" \
"Server: My server\r\n" \
"Connection: keep-alive\r\n" \
"\r\n";

enum html_stage
{
    Stage_Accept,
    Stage_Read,
    Stage_Send
};

struct ts_ioring_info
{
    file Ring;
    struct io_uring_sqe* SQEArray;
    
    u32* SHead;
    u32* STail;
    u32* SArray;
    u32 SRingMask;
    
    u32* CHead;
    u32* CTail;
    struct io_uring_cqe* CQEs;
    u32 CRingMask;
};

global enum html_stage gStage = Stage_Accept;
global u8 gData[4096] = {0};

#define TS_IORING_LEN 4096

static b32
SetupIoQueue(struct ts_ioring_info* Info)
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
        u8* SRing = 0, *CRing = 0, *SQE = 0;
        if ((SRing = (u8*)mmap(0, SRingSize, Prot, Map, Ring, IORING_OFF_SQ_RING)) != MAP_FAILED
            && (CRing = (u8*)mmap(0, CRingSize, Prot, Map, Ring, IORING_OFF_CQ_RING))
            != MAP_FAILED
            && (SQE = (u8*)mmap(0, SQESize, Prot, Map, Ring, IORING_OFF_SQES)) != MAP_FAILED)
        {
            Info->Ring = Ring;
            Info->SQEArray = (struct io_uring_sqe*)SQE;
            Info->SHead = (u32*)(SRing + Params.sq_off.head);
            Info->STail = (u32*)(SRing + Params.sq_off.tail);
            Info->SArray = (u32*)(SRing + Params.sq_off.array);
            Info->SRingMask = Params.sq_off.ring_mask;
            Info->CHead = (u32*)(CRing + Params.cq_off.head);
            Info->CTail = (u32*)(CRing + Params.cq_off.tail);
            Info->CQEs = (struct io_uring_cqe*)(CRing + Params.cq_off.cqes);
            Info->CRingMask = Params.cq_off.ring_mask;
            
            return 1;
        }
        close(Ring);
    }
    return 0;
}

static b32
SubmitToSQE(int Socket, u8 OpCode, u8* Data, struct ts_ioring_info* Info)
{
    u32 Tail = Info->STail[0];
    ReadBarrier();
    u32 Idx = Tail & Info->SRingMask;
    
    struct io_uring_sqe* SQE = Info->SQEArray + Idx;
    SQE->fd = Socket;
    SQE->flags = 0;
    //SQE->ioprio = IORING_RECVSEND_POLL_FIRST;
    SQE->opcode = OpCode;
    SQE->len = *(u32*)Data;
    SQE->addr = (u64)&Data[4];
    SQE->msg_flags = MSG_DONTWAIT;
    SQE->user_data = (unsigned long long)OpCode;
    
    Info->SArray[Idx] = Idx;
    Tail++;
    
    Info->STail[0] = Tail;
    WriteBarrier();
    
    return 1;
}

static b32
ReadFromCQE(struct ts_ioring_info* Info)
{
    int Ret = syscall(SYS_io_uring_enter, Info->Ring, 0, 1, IORING_ENTER_GETEVENTS, 0);
    Assert(Ret != -1);
    fprintf(stdout, "CQE: %d IOs dequeued.\n", Ret);
    
    u32 Head = Info->CHead[0];
    ReadBarrier();
    
    while (Head != Info->CTail[0])
    {
        struct io_uring_cqe* CQE = &Info->CQEs[Head & Info->CRingMask];
        u8 OpCode = (u8)CQE->user_data;
        i32 BytesTransferred = (i32)CQE->res;
        
        fprintf(stdout, "\t- %s (%d): %d bytes.\n", (OpCode == IORING_OP_RECV) ? "Recv" : (OpCode == IORING_OP_SEND) ? "Send" : "Unknown", OpCode, BytesTransferred);
        
        Head++;
    }
    
    return 1;
}

THREAD_PROC(IoThread)
{
    struct ts_ioring_info* Info = (struct ts_ioring_info*)Arg;
    for (;;)
    {
        if (ReadFromCQE(Info))
        {
            fprintf(stdout, "Buffer:\n%s\n", gData + 4);
            sleep(1000000);
        }
    }
}

int main()
{
    int Listen = socket(AF_INET, SOCK_STREAM|SOCK_CLOEXEC|SOCK_NONBLOCK, IPPROTO_TCP);
    Assert(Listen != -1);
    
    struct sockaddr_in Addr = {
        .sin_family = AF_INET,
        .sin_port = htons(40000),
        .sin_addr.s_addr = INADDR_ANY
    };
    Assert(bind(Listen, (struct sockaddr*)&Addr, sizeof(Addr)) == 0);
    Assert(listen(Listen, SOMAXCONN) == 0);
    
    int Poll = epoll_create1(EPOLL_CLOEXEC);
    Assert(Poll != -1);
    
    struct epoll_event Event = {
        .events = EPOLLIN,
        .data.fd = Listen
    };
    Assert(epoll_ctl(Poll, EPOLL_CTL_ADD, Listen, &Event) == 0);
    fprintf(stdout, "EPoll created.\n");
    
    struct ts_ioring_info Info = {0};
    Assert(SetupIoQueue(&Info));
    
    pthread_attr_t Attr = {0};
    pthread_t Thread = 0;
    Assert(!pthread_attr_init(&Attr));
    Assert(!pthread_attr_setdetachstate(&Attr, PTHREAD_CREATE_DETACHED));
    Assert(!pthread_attr_setguardsize(&Attr, gSysInfo.PageSize));
    Assert(!pthread_attr_setstacksize(&Attr, Megabyte(2)));
    Assert(!pthread_create(&Thread, &Attr, IoThread, (void*)&Info));
    
    *(u32*)gData = 4092;
    
    struct epoll_event RetEvent[10] = {0};
    while (1)
    {
        int EventCount = epoll_wait(Poll, RetEvent, 10, -1);
        Assert(EventCount != -1);
        fprintf(stdout, "Event caught.\n");
        
        for (int n = 0; n < EventCount; n++)
        {
            if (RetEvent[n].data.fd == Listen)
            {
                struct sockaddr_in RetAddr = {0};
                socklen_t RetAddrSize = 0;
                int Conn = accept4(Listen, &RetAddr, &RetAddrSize, SOCK_NONBLOCK|SOCK_CLOEXEC);
                Assert(Conn != -1);
                fprintf(stdout, "Conn created on fd %d.\n", Conn);
                
                SubmitToSQE(Conn, IORING_OP_READ, gData, &Info);
            }
            else
            {
                fprintf(stdout, "Unknown event %d on fd %d\n", RetEvent[n].events, RetEvent[n].data.fd);
                return -1;
            }
        }
    }
    
    return 0;
}
#include <fcntl.h>
#include <linux/fs.h>
#include <linux/io_uring.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>

#define QUEUE_DEPTH 1
#define BLOCK_SZ 1024

#define ReadBarrier() __asm__ __volatile__("":::"memory")
#define WriteBarrier() __asm__ __volatile__("":::"memory")

typedef unsigned char u8;
typedef unsigned int u32;
typedef unsigned long long u64;

#define Assert(Exp, Msg) do { \
if (!(Exp)) { \
fprintf(stderr, "Error [%d]: %s\n", __LINE__, (Msg)); \
return 0; \
}} while(0);

struct app_io_sq_ring
{
    u32* Head;
    u32* Tail;
    u32* RingMask;
    u32* RingEntries;
    u32* Flags;
    u32* Array;
};

struct app_io_cq_ring
{
    u32* Head;
    u32* Tail;
    u32* RingMask;
    u32* RingEntries;
    struct io_uring_cqe* CQEs;
};

struct submitter
{
    int Ring;
    struct app_io_sq_ring SqRing;
    struct io_uring_sqe* SQEs;
    struct app_io_cq_ring CqRing;
};

static int
AppSetupUring(struct submitter* Submit)
{
    struct app_io_sq_ring* SRing = &Submit->SqRing;
    struct app_io_cq_ring* CRing = &Submit->CqRing;
    
    struct io_uring_params IoParams = {0};
    Submit->Ring = syscall(SYS_io_uring_setup, QUEUE_DEPTH, &IoParams);
    Assert(Submit->Ring != -1, "io_uring_setup");
    
    int SRingSize = IoParams.sq_off.array + IoParams.sq_entries * sizeof(u32);
    int CRingSize = IoParams.cq_off.cqes + IoParams.cq_entries * sizeof(struct io_uring_cqe);
    
    if (IoParams.features & IORING_FEAT_SINGLE_MMAP)
    {
        if (CRingSize > SRingSize) SRingSize = CRingSize;
        CRingSize = SRingSize;
    }
    
    int Prot = PROT_READ|PROT_WRITE;
    int Map = MAP_SHARED|MAP_POPULATE;
    void* SPtr = mmap(0, SRingSize, Prot, Map, Submit->Ring, IORING_OFF_SQ_RING);
    Assert(SPtr != MAP_FAILED, "mmap");
    
    void* CPtr = ((IoParams.features & IORING_FEAT_SINGLE_MMAP)
                  ? SPtr
                  : mmap(0, CRingSize, Prot, Map, Submit->Ring, IORING_OFF_CQ_RING));
    Assert(CPtr != MAP_FAILED, "mmap");
    
    SRing->Head = SPtr + IoParams.sq_off.head;
    SRing->Tail = SPtr + IoParams.sq_off.tail;
    SRing->RingMask = SPtr + IoParams.sq_off.ring_mask;
    SRing->RingEntries = SPtr + IoParams.sq_off.ring_entries;
    SRing->Flags = SPtr + IoParams.sq_off.flags;
    SRing->Array = SPtr + IoParams.sq_off.array;
    
    Submit->SQEs = mmap(0, IoParams.sq_entries * sizeof(struct io_uring_sqe), Prot, Map, Submit->Ring, IORING_OFF_SQES);
    Assert(Submit->SQEs != MAP_FAILED, "mmap");
    
    CRing->Head = CPtr + IoParams.cq_off.head;
    CRing->Tail = CPtr + IoParams.cq_off.tail;
    CRing->RingMask = CPtr + IoParams.cq_off.ring_mask;
    CRing->RingEntries = CPtr + IoParams.cq_off.ring_entries;
    CRing->CQEs = CPtr + IoParams.cq_off.cqes;
    
    return 1;
}

static int
SubmitToSQE(int Socket, u8 OpCode, u8* Data, struct submitter* Submit)
{
    struct app_io_sq_ring* SRing = &Submit->SqRing;
    u32 Tail = SRing->Tail[0];
    ReadBarrier();
    u32 Idx = Tail & *Submit->SqRing.RingMask;
    
    struct io_uring_sqe* SQE = &Submit->SQEs[Idx];
    SQE->fd = Socket;
    SQE->flags = 0;
    SQE->opcode = OpCode;
    
    if (OpCode == IORING_OP_ACCEPT)
    {
        SQE->accept_flags = SOCK_NONBLOCK|SOCK_CLOEXEC;
        SQE->addr = (u64)(Data + sizeof(socklen_t));
        SQE->addr2 = (u64)Data;
    }
    else if (OpCode == IORING_OP_RECV || OpCode == IORING_OP_SEND)
    {
        u32* Buffer = (u32*)Data;
        SQE->len = Buffer[0];
        SQE->addr = (u64)&Buffer[1];
        SQE->msg_flags = MSG_DONTWAIT;
    }
    SQE->user_data = (unsigned long long)OpCode;
    
    SRing->Array[Idx] = Idx;
    Tail++;
    
    if (SRing->Tail[0] != Tail)
    {
        SRing->Tail[0] = Tail;
        WriteBarrier();
    }
    Assert(syscall(SYS_io_uring_enter, Submit->Ring, 1, 0, 0) != -1, "sqe");
    
    return 1;
}

static int
ReadFromCQE(struct submitter* Submit)
{
    struct app_io_cq_ring* CRing = &Submit->CqRing;
    u32 Head = CRing->Head[0];
    
    Assert(syscall(SYS_io_uring_enter, Submit->Ring, 0, 1, IORING_ENTER_GETEVENTS) != -1, "cqe");
    
    struct io_uring_cqe* CQE = &CRing->CQEs[Head & *Submit->CqRing.RingMask];
    u8 OpCode = (u8)CQE->user_data;
    fprintf(stdout, "Completed: %s\n", (OpCode == IORING_OP_ACCEPT) ? "Accept"
            : (OpCode == IORING_OP_RECV) ? "Recv"
            : (OpCode == IORING_OP_SEND) ? "Send" : "Unknown");
    
    CRing->Head[0] = Head;
    WriteBarrier();
    
    return CQE->res;
}

int main(int Argc, char** Argv)
{
    struct submitter* Submit = (struct submitter*)calloc(sizeof(*Submit), 1);
    Assert(AppSetupUring(Submit), "app_setup_uring");
    
    int Listen = socket(AF_INET, SOCK_STREAM|SOCK_CLOEXEC|SOCK_NONBLOCK, IPPROTO_TCP);
    Assert(Listen != -1, "socket");
    struct sockaddr_in InAddr = {0};
    InAddr.sin_family = AF_INET;
    InAddr.sin_port = htons(40000);
    InAddr.sin_addr.s_addr = INADDR_ANY;
    Assert(bind(Listen, (struct sockaddr*)&InAddr, sizeof(InAddr)) == 0, "bind");
    Assert(listen(Listen, SOMAXCONN) == 0, "listen");
    
    u8 AddrData[128] = {0};
    Assert(SubmitToSQE(Listen, IORING_OP_ACCEPT, AddrData, Submit), "submitting");
    int Conn = ReadFromCQE(Submit);
    Assert(Conn != -1, "accept");
    socklen_t* AddrLen = (socklen_t*)AddrData;
    struct sockaddr_in* Addr = (struct sockaddr_in*)&AddrLen[1];
    fprintf(stdout, "Conn: %d\nFamily: %s\nPort: %d\nSize: %d\n", Conn, (Addr->sin_family == AF_INET)
            ? "AF_INET" : "Unknown", ntohs(Addr->sin_port), AddrLen[0]);
    
    char* InBuffer = (char*)calloc(4096, 1);
    *((u32*)InBuffer) = 4096;
    Assert(SubmitToSQE(Conn, IORING_OP_RECV, InBuffer, Submit), "submitting");
    int BytesRecv = ReadFromCQE(Submit);
    Assert(Conn != -1, "recv");
    fprintf(stdout, "BytesRecv: %d\nContent:\n%s\n", BytesRecv, InBuffer+4);
    
    return 0;
}
#include <linux/io_uring.h>

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

internal b32
IoURing_SetupIoQueue(_opt u32 NumThreads)
{
# define TS_IORING_LEN 4096
    
    struct io_uring_params Params = {0};
    Params.flags = IORING_SETUP_SQPOLL;
    Params.sq_thread_idle = 2000; // 2 seconds.
# ifdef IORING_SETUP_SUBMIT_ALL
    Params.flags |= IORING_SETUP_SUBMIT_ALL;
# endif
    
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

#ifndef TINYSERVER_INTERNAL_H
#define TINYSERVER_INTERNAL_H

#define TS_MAX_DEQUEUE 64

typedef struct ts_server_info
{
    usz ListenCount;
    bool ReadyToPoll;  // Only relevant on Windows.
    file AcceptQueue; // Only relevant on Linux.
    u8* AcceptEvents;
    usz CurrentAcceptIdx;
    usz MaxAcceptIdx;
    
    thread IoEventThread; // Only relevant on epoll.
    usz ClientCount;
    file IoQueue;
    u8* IoEvents;
    usz CurrentIoIdx;
    usz MaxIoIdx;
    
    mpmc_ringbuf WorkQueue; // Only relevant on epoll.
    buffer WorkQueueMem;    // Only relevant on epoll.
    u8 WorkSemaphore[4];    // Only relevant on epoll.
} ts_server_info;

global buffer gServerArena;

#endif //TINYSERVER_INTERNAL_H

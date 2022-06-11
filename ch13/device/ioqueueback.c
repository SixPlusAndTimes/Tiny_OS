#include "ioqueue.h"
#include "interrupt.h"
#include "global.h"
#include "debug.h"

//初始化io队列ioq
void ioqueue_init( struct ioqueue* ioq) {
    lock_init(&ioq->lock);
    ioq->producer = ioq->consumer = NULL;//生产者消费者都置空
    ioq->head = ioq->tail = 0; //队列的初始状态的队首和队尾指针都是0
}

//此函数接受一个参数pos，功能是返回pos在缓冲区中的下一个位置
static int32_t next_pos( int32_t pos ) {
    return ( pos + 1 ) % bufsize;
 }

 //判断队列是否已满,缓冲区的大小是64字节，但是最大容量却是63字节
 bool ioq_full( struct ioqueue* ioq ) {
     ASSERT( intr_get_status() == INTR_OFF );
     return next_pos(ioq->head) == ioq->tail;
 }

 //判断队列是否已经空
 bool ioq_empty( struct ioqueue* ioq ) {
     ASSERT( intr_get_status() == INTR_OFF );
     return ioq->head == ioq->tail;
 }
 
 //使当前生产者或者消费者在此缓冲区上等待
 static void ioq_wait( struct task_struct** waiter ) {
     ASSERT( *waiter == NULL && waiter != NULL);
     *waiter = running_thread();
     thread_block(TASK_BLOCKED);
 }

 //唤醒waiter
 static void wakeup( struct taskt_struct** waiter ) {
     ASSERT(*waiter != NULL);
     thread_unblock(*waiter);
     *waiter = NULL;
 }

//  消费者从ioq队列中取一个字符
char ioq_getchar( struct ioqueue* ioq ) {
    ASSERT( intr_get_status == INTR_OFF );

    while( ioq_empty(ioq) ) {
        lock_acquire(&ioq->lock);// 为什么拿着锁睡眠？？---为了与其他消费者互斥
        ioq_wait(&ioq->consumer);// 将当前线程的PCB添加到 consumer中，表示有消费者在等待，并阻塞当前线程
        lock_release(&ioq->lock);
    }

    char byte = ioq->buf[ioq->tail]; // 从缓冲区的末尾取出一个字节
    ioq->tail = next_pos(ioq->tail); // 将环形队列的队尾指针向后移一格

    if( ioq->producer != NULL ) {
        wakeup(&ioq->producer); // 唤醒生产者
    }

    return byte;
}

// 生产者往ioq队列中写入一个字符byte
void ioq_putchar( struct ioqueue* ioq, char byte ) {
    ASSERT( intr_get_status() == INTR_OFF );

    while( ioq_full(ioq) ) {
        lock_acquire(&ioq->lock);
        ioq_wait(&ioq->producer);
        lock_release(&ioq->lock);
    }

    ioq->buf[ioq->head] = byte;
    ioq->head = next_pos(ioq->head);

    if( ioq->consumer != NULL ) {
        wakeup(&ioq->consumer);
    }
}
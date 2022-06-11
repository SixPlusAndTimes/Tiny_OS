#include "sync.h"
#include "list.h"
#include "global.h"
#include "debug.h"
#include "interrupt.h"
#include "print.h"

//初始化信号量
void sema_init(struct semaphore* psema, uint8_t value) {
    psema->value = value;//信号量的初值一般为1
    list_init(&psema->waiters);
}

//初始化锁
void lock_init(struct lock* plock) {
    plock->holder = NULL;
    plock->holder_repeat_nr = 0;
    sema_init(&plock->semaphore, 1);//信号量初值为1
}

// 信号量down操作
void sema_down(struct semaphore* psema) {
    //关中断保证原子操作 ！！！
    enum intr_status old_status = intr_disable();

    while(psema->value == 0) {
        ASSERT(!elem_find(&psema->waiters, &running_thread()->general_tag));
        //当前线程的generaltag不应该在信号量的waiters中
        if(elem_find(&psema->waiters, &running_thread()->general_tag)) {
            PANIC("sema_down: thread blocked has been in waiters_list\n");
        }
        list_append(&psema->waiters, &running_thread()->general_tag);//将本线程的general-tag加入到等待队列中
        thread_block(TASK_BLOCKED); //当前线程将自己阻塞
    }

    // 若value为1，表示本线程拿到了锁，执行临界区代码
    psema->value--;//现在是关中断的状态，所以该操作在单核cpu上具有原子性
    ASSERT(psema->value == 0);

    intr_set_status(old_status);
}

//信号量的up操作
void sema_up(struct semaphore* psema) {
    //关中断保证原子操作
    enum intr_status old_status = intr_disable();

    ASSERT(psema->value == 0);

    //如果信号量的等待队列不为空，就唤醒一个
    if(!list_empty(&psema->waiters)) {
        struct task_struct* thread_blocked = elem2entry(struct task_struct, general_tag, list_pop(&psema->waiters));
        thread_unblock(thread_blocked);//唤醒这个线程，即将这个线程的generaltag放在thread_ready_list中
    }

    psema->value++;
    ASSERT(psema->value == 1);
    //恢复之前的中断状态
    intr_set_status(old_status);
} 

//获取锁
void lock_acquire(struct lock* plock) {
    //如果申请锁的线程不是当前获得锁的线程，那么执行sema_down操作
    if(plock->holder != running_thread()){
        sema_down(&plock->semaphore);
        plock->holder = running_thread();//被唤醒后，将锁的获得者该成自己
        ASSERT(plock->holder_repeat_nr == 0);//获得该锁后，该锁的older_repeat_nr应该为0 ，否则就是出了bug
        plock->holder_repeat_nr = 1;
    }else {
        //否则表示申请锁的线程已经得到锁了，不执行信号量操作，而是将holder_repeat_nr加以
        plock->holder_repeat_nr++;
    }
}

// 释放锁
void lock_release(struct lock* plock) {
    //确保释放锁的线程是获得该锁的线程
    ASSERT(plock->holder == running_thread());
    //如果holder_repeat_nr大于一，表示该线程曾经重复获取该锁，只需将该属性减1然后返回即可
    if(plock->holder_repeat_nr > 1) {
        plock->holder_repeat_nr--;
        return;
    }
    ASSERT(plock->holder_repeat_nr == 1);

    plock->holder = NULL;//注意这个操作必须在sema_up操作之前！！
    plock->holder_repeat_nr = 0;
    sema_up(&plock->semaphore);
}
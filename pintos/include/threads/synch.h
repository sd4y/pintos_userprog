#ifndef THREADS_SYNCH_H
#define THREADS_SYNCH_H

#include <list.h>
#include <stdbool.h>

/* A counting semaphore. */
struct semaphore {
	unsigned value;             /* Current value. */
	struct list waiters;        /* 대기 중인 스레드들의 리스트. */
};

void sema_init (struct semaphore *, unsigned value);
void sema_down (struct semaphore *);
bool sema_try_down (struct semaphore *);
void sema_up (struct semaphore *);
void sema_self_test (void);

struct lock {
	struct thread *holder;      /* 락을 소유한 스레드. */
	struct semaphore semaphore; /* 접근을 제어하는 이진 세마포어. */
	struct list_elem elem;      /* 스레드의 보유 락 목록을 위한 리스트 요소. */
};

void lock_init (struct lock *);
void lock_acquire (struct lock *);
bool lock_try_acquire (struct lock *);
void lock_release (struct lock *);
bool lock_held_by_current_thread (const struct lock *);

/* Condition variable. */
struct condition {
	struct list waiters;        /* List of waiting threads. */
};

void cond_init (struct condition *);
void cond_wait (struct condition *, struct lock *);
void cond_signal (struct condition *, struct lock *);
void cond_broadcast (struct condition *, struct lock *);

/* 최적화 장벽.
 *
 * 컴파일러는 최적화 장벽을 기준으로 연산의 순서를 변경하지 않습니다.
 * 자세한 내용은 참조 가이드의 "Optimization Barriers"를 참고하세요.*/
#define barrier() asm volatile ("" : : : "memory")

#endif /* threads/synch.h */

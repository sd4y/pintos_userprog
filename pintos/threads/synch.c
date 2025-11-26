/* This file is derived from source code for the Nachos
   instructional operating system.  The Nachos copyright notice
   is reproduced in full below. */

/* Copyright (c) 1992-1996 The Regents of the University of California.
   All rights reserved.

   Permission to use, copy, modify, and distribute this software
   and its documentation for any purpose, without fee, and
   without written agreement is hereby granted, provided that the
   above copyright notice and the following two paragraphs appear
   in all copies of this software.

   IN NO EVENT SHALL THE UNIVERSITY OF CALIFORNIA BE LIABLE TO
   ANY PARTY FOR DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR
   CONSEQUENTIAL DAMAGES ARISING OUT OF THE USE OF THIS SOFTWARE
   AND ITS DOCUMENTATION, EVEN IF THE UNIVERSITY OF CALIFORNIA
   HAS BEEN ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

   THE UNIVERSITY OF CALIFORNIA SPECIFICALLY DISCLAIMS ANY
   WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
   WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
   PURPOSE.  THE SOFTWARE PROVIDED HEREUNDER IS ON AN "AS IS"
   BASIS, AND THE UNIVERSITY OF CALIFORNIA HAS NO OBLIGATION TO
   PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR
   MODIFICATIONS.
   */

#include "threads/synch.h"
#include <stdio.h>
#include <string.h>
#include "threads/interrupt.h"
#include "threads/thread.h"

/* 세마포어 SEMA를 VALUE로 초기화합니다. 세마포어는 
   음이 아닌 정수값과 이를 조작하는 두 개의 원자적 연산자로 구성됩니다:

   - down 또는 "P": 값이 양수가 될 때까지 기다린 후
   이를 감소시킵니다.

   - up 또는 "V": 값을 증가시킵니다 (그리고 대기 중인 스레드가 
   있다면 그 중 하나를 깨웁니다). */
void
sema_init (struct semaphore *sema, unsigned value) {
	ASSERT (sema != NULL);

	sema->value = value;
	list_init (&sema->waiters);
}

/* 세마포어에 대한 Down 또는 "P" 연산입니다. SEMA의 값이
   양수가 될 때까지 기다린 후 원자적으로 이를 감소시킵니다.

   이 함수는 sleep 상태가 될 수 있으므로, 인터럽트 핸들러 내에서
   호출되어서는 안 됩니다. 인터럽트가 비활성화된 상태에서 호출될 수 있지만,
   sleep 상태가 되면 다음 스케줄된 스레드가 인터럽트를 다시 활성화할 것입니다.
   이것이 sema_down 함수입니다. */
void
sema_down (struct semaphore *sema) {
	enum intr_level old_level;

	ASSERT (sema != NULL);
	ASSERT (!intr_context ());

	old_level = intr_disable ();
	while (sema->value == 0) {
		list_insert_ordered(&sema->waiters, &thread_current()->elem, priority_cmp, NULL);
		thread_block ();
	}
	sema->value--;
	intr_set_level (old_level);
}

/* 세마포어가 이미 0이 아닌 경우에만 수행되는 Down 또는 "P" 연산입니다.
   세마포어가 감소되면 true를 반환하고, 그렇지 않으면 false를 반환합니다.

   이 함수는 인터럽트 핸들러에서 호출될 수 있습니다. */
bool
sema_try_down (struct semaphore *sema) {
	enum intr_level old_level;
	bool success;

	ASSERT (sema != NULL);

	old_level = intr_disable ();
	if (sema->value > 0)
	{
		sema->value--;
		success = true;
	}
	else
		success = false;
	intr_set_level (old_level);

	return success;
}

/* 세마포어에 대한 Up 또는 "V" 연산입니다. SEMA의 값을 증가시키고
   SEMA를 기다리는 스레드들 중 하나가 있다면 이를 깨웁니다.

   이 함수는 인터럽트 핸들러에서 호출될 수 있습니다. */
void
sema_up (struct semaphore *sema) {
	enum intr_level old_level;
	bool should_yield = false;

	ASSERT (sema != NULL);

	old_level = intr_disable ();
	if (!list_empty (&sema->waiters)){
		list_sort(&sema->waiters, priority_cmp, NULL);
		struct thread *unblocked = list_entry (list_pop_front (&sema->waiters), struct thread, elem);
		thread_unblock (unblocked);
		/* 깨운 스레드의 우선순위가 현재 스레드보다 높으면 양보 예약 */
		if (unblocked->priority > thread_get_priority())
			should_yield = true;
	}
	sema->value++;
	intr_set_level (old_level);
	
	if (should_yield && !intr_context())
		thread_yield();
}

static void sema_test_helper (void *sema_);

/* 세마포어의 동작을 테스트하는 함수로, 두 스레드 사이에서
	 제어가 "핑퐁"처럼 오가는 것을 확인합니다. 동작을 확인하려면
	 printf() 호출을 삽입하세요. */
void
sema_self_test (void) {
	struct semaphore sema[2];
	int i;

	printf ("Testing semaphores...");
	sema_init (&sema[0], 0);
	sema_init (&sema[1], 0);
	thread_create ("sema-test", PRI_DEFAULT, sema_test_helper, &sema);
	for (i = 0; i < 10; i++)
	{
		sema_up (&sema[0]);
		sema_down (&sema[1]);
	}
	printf ("done.\n");
}

/* Thread function used by sema_self_test(). */
static void
sema_test_helper (void *sema_) {
	struct semaphore *sema = sema_;
	int i;

	for (i = 0; i < 10; i++)
	{
		sema_down (&sema[0]);
		sema_up (&sema[1]);
	}
}

/* LOCK을 초기화합니다. 락은 주어진 시간에 최대 하나의 스레드만이
   보유할 수 있습니다. 우리의 락은 "재귀적"이지 않습니다. 즉,
   현재 락을 보유하고 있는 스레드가 같은 락을 획득하려고 하면
   에러가 발생합니다.

   락은 초기값이 1인 세마포어의 특수한 형태입니다. 락과 이러한
   세마포어의 차이점은 두 가지입니다. 첫째, 세마포어는 1보다 큰
   값을 가질 수 있지만, 락은 한 번에 하나의 스레드만이 소유할 수
   있습니다. 둘째, 세마포어는 소유자가 없어서 한 스레드가
   세마포어를 "down"하고 다른 스레드가 "up"할 수 있지만, 락은
   동일한 스레드가 획득과 해제를 모두 수행해야 합니다. 이러한
   제한이 부담스럽다면, 락 대신 세마포어를 사용하는 것이 
   좋은 신호입니다. */
void
lock_init (struct lock *lock) {
	ASSERT (lock != NULL);

	lock->holder = NULL;
	sema_init (&lock->semaphore, 1);
	lock->elem.prev = NULL;
	lock->elem.next = NULL;
}

/* LOCK을 획득합니다. 필요하다면 사용할 수 있을 때까지 sleep합니다.
	 현재 스레드가 이미 락을 보유하고 있으면 안 됩니다.

	 이 함수는 sleep 상태가 될 수 있으므로, 인터럽트 핸들러 내에서
	 호출되어서는 안 됩니다. 인터럽트가 비활성화된 상태에서 호출될 수 있지만,
	 sleep이 필요하면 인터럽트가 다시 활성화됩니다. */
void
lock_acquire (struct lock *lock) {
	ASSERT (lock != NULL);
	ASSERT (!intr_context ());
	ASSERT (!lock_held_by_current_thread (lock));

	struct thread *curr = thread_current();
	
	if (lock->holder != NULL) {
		/* 현재 스레드가 이 락을 기다리고 있음을 표시 */
		curr->waiting_lock = lock;
		
		/* 중첩 기부: 체인을 따라가며 우선순위 전파 */
		struct thread *holder = lock->holder;
		int donate_priority = curr->priority;
		
		for (int depth = 0; depth < 8 && holder != NULL; depth++) {
			/* 이미 더 높은 우선순위를 가지고 있으면 중단 */
			if (holder->priority >= donate_priority)
				break;
			
			/* 우선순위 기부 */
			holder->priority = donate_priority;
			
			/* holder가 다른 락을 기다리고 있으면 체인 계속 */
			if (holder->waiting_lock == NULL)
				break;
			
			holder = holder->waiting_lock->holder;
		}
	}

	sema_down (&lock->semaphore);
	
	/* 락 획득 완료 - waiting_lock 초기화 */
	curr->waiting_lock = NULL;
	lock->holder = curr;
	list_push_back(&curr->holding_list, &lock->elem);
}

/* LOCK을 획득 시도하며 성공하면 true, 실패하면 false를 반환합니다.
	 현재 스레드가 이미 락을 보유하고 있으면 안 됩니다.

	 이 함수는 sleep하지 않으므로, 인터럽트 핸들러 내에서 호출될 수 있습니다. */
bool
lock_try_acquire (struct lock *lock) {
	bool success;

	ASSERT (lock != NULL);
	ASSERT (!lock_held_by_current_thread (lock));

	success = sema_try_down (&lock->semaphore);
	if (success)
		lock->holder = thread_current ();
	return success;
}



/* 현재 스레드가 소유하고 있어야만 LOCK을 해제합니다.
	 이것이 lock_release 함수입니다.

	 인터럽트 핸들러는 락을 획득할 수 없으므로,
	 인터럽트 핸들러 내에서 락을 해제하려고 하는 것은 의미가 없습니다. */
void
lock_release (struct lock *lock) {
	ASSERT (lock != NULL);
	ASSERT (lock_held_by_current_thread (lock));
	
	struct thread *holder = lock->holder;
	
	/* 보유 락 목록에서 제거 */
	list_remove(&lock->elem);
	
	/* 우선순위 재계산 */
	refresh_priority(holder);
	
	lock->holder = NULL;
	sema_up (&lock->semaphore);
}

/* 현재 스레드가 LOCK을 보유하고 있으면 true를 반환하고,
	 그렇지 않으면 false를 반환합니다. (다른 스레드가 락을 보유하고 있는지
	 검사하는 것은 경쟁 상태가 발생할 수 있습니다.) */
bool
lock_held_by_current_thread (const struct lock *lock) {
	ASSERT (lock != NULL);

	return lock->holder == thread_current ();
}

/* One semaphore in a list. */
struct semaphore_elem {
	struct list_elem elem;              /* List element. */
	struct semaphore semaphore;         /* This semaphore. */
};

/* semaphore_elem의 우선순위를 비교하는 함수 */
static bool
sema_elem_priority_cmp(const struct list_elem *a, const struct list_elem *b, void *aux UNUSED) {
	struct semaphore_elem *sa = list_entry(a, struct semaphore_elem, elem);
	struct semaphore_elem *sb = list_entry(b, struct semaphore_elem, elem);
	
	struct list_elem *ta_elem = list_front(&sa->semaphore.waiters);
	struct list_elem *tb_elem = list_front(&sb->semaphore.waiters);
	
	struct thread *ta = list_entry(ta_elem, struct thread, elem);
	struct thread *tb = list_entry(tb_elem, struct thread, elem);
	
	return ta->priority > tb->priority;
}

/* 조건 변수 COND를 초기화합니다. 조건 변수는 한 코드가
   조건을 신호로 보내고 협력하는 코드가 그 신호를 받아
   이에 따라 행동할 수 있게 합니다. */
void
cond_init (struct condition *cond) {
	ASSERT (cond != NULL);

	list_init (&cond->waiters);
}

/* 원자적으로 LOCK을 해제하고 다른 코드에 의해 COND가 신호될 때까지
   기다립니다. COND가 신호되면, 반환하기 전에 LOCK을 다시 획득합니다.
   이 함수를 호출하기 전에 LOCK이 보유되어 있어야 합니다.

   이 함수로 구현된 모니터는 "Hoare" 방식이 아닌 "Mesa" 방식입니다.
   즉, 신호를 보내고 받는 것이 원자적 연산이 아닙니다. 따라서,
   일반적으로 호출자는 대기가 완료된 후 조건을 다시 확인해야 하며,
   필요한 경우 다시 대기해야 합니다.

   주어진 조건 변수는 단 하나의 락과만 연관되지만, 하나의 락은
   여러 개의 조건 변수와 연관될 수 있습니다. 즉, 락에서 조건
   변수로의 일대다 매핑이 존재합니다.

   이 함수는 sleep 상태가 될 수 있으므로, 인터럽트 핸들러 내에서
   호출되어서는 안 됩니다. 인터럽트가 비활성화된 상태에서 호출될
   수 있지만, sleep이 필요한 경우 인터럽트가 다시 활성화됩니다. */
void
cond_wait (struct condition *cond, struct lock *lock) {
	struct semaphore_elem waiter;

	ASSERT (cond != NULL);
	ASSERT (lock != NULL);
	ASSERT (!intr_context ());
	ASSERT (lock_held_by_current_thread (lock));

	sema_init (&waiter.semaphore, 0);
	list_push_back(&cond->waiters, &waiter.elem);
	lock_release (lock);
	sema_down (&waiter.semaphore);
	lock_acquire (lock);
}

/* COND를 기다리는 스레드가 있다면(LOCK으로 보호됨), 이 함수는
   그 중 하나에게 대기 상태에서 깨어나라는 신호를 보냅니다.
   이 함수를 호출하기 전에 LOCK이 보유되어 있어야 합니다.

   인터럽트 핸들러는 락을 획득할 수 없으므로, 인터럽트 핸들러
   내에서 조건 변수에 신호를 보내는 것은 의미가 없습니다. */
void
cond_signal (struct condition *cond, struct lock *lock UNUSED) {
	ASSERT (cond != NULL);
	ASSERT (lock != NULL);
	ASSERT (!intr_context ());
	ASSERT (lock_held_by_current_thread (lock));

	if (!list_empty (&cond->waiters)) {
		list_sort(&cond->waiters, sema_elem_priority_cmp, NULL);
		sema_up (&list_entry (list_pop_front (&cond->waiters),
					struct semaphore_elem, elem)->semaphore);
	}
}

/* COND를 기다리는 모든 스레드를 깨웁니다(LOCK으로 보호됨).
   이 함수를 호출하기 전에 LOCK이 보유되어 있어야 합니다.

   인터럽트 핸들러는 락을 획득할 수 없으므로, 인터럽트 핸들러
   내에서 조건 변수에 신호를 보내는 것은 의미가 없습니다. */
void
cond_broadcast (struct condition *cond, struct lock *lock) {
	ASSERT (cond != NULL);
	ASSERT (lock != NULL);

	while (!list_empty (&cond->waiters))
		cond_signal (cond, lock);
}

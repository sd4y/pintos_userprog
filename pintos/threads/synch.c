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

/* SEMA를 VALUE로 초기화한다. 세마포어는 음이 아닌 정수 값과
	 이를 조작하는 두 개의 원자적 연산을 가진다.

	 - down 또는 "P": 값이 양수가 될 때까지 기다린 후 값을
		 1 감소시킨다.

	 - up 또는 "V": 값을 1 증가시키고(대기 중인 스레드가 있으면)
		 하나의 스레드를 깨운다. */
void
sema_init (struct semaphore *sema, unsigned value) {
	ASSERT (sema != NULL);

	sema->value = value;
	list_init (&sema->waiters);
}

/* 세마포어에 대한 Down 또는 "P" 연산이다. SEMA의 값이 양수가
	 될 때까지 대기한 뒤 원자적으로 값을 1 감소시킨다.

	 이 함수는 잠들 수 있으므로 인터럽트 핸들러 내에서 호출하면
	 안 된다. 인터럽트가 비활성화된 상태에서 호출될 수는 있으나,
	 함수가 잠들면 다음 스케줄된 스레드가 인터럽트를 다시 켤 수
	 있다. (sema_down 함수) */
void
sema_down (struct semaphore *sema) {
	enum intr_level old_level;

	ASSERT (sema != NULL);
	ASSERT (!intr_context ());

	old_level = intr_disable ();
	while (sema->value == 0) {
		list_insert_ordered (&sema->waiters, &thread_current ()->elem, thread_priority_compare, NULL);
		thread_block ();
	}
	sema->value--;
	intr_set_level (old_level);
}

/* 세마포어에 대한 Down 또는 "P" 연산이지만 세마포어 값이 0이
	 아닌 경우에만 수행한다. 값을 감소시켰다면 true를 반환하고,
	 그렇지 않으면 false를 반환한다.

	 이 함수는 인터럽트 핸들러에서 호출될 수 있다. */
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

/* 세마포어에 대한 Up 또는 "V" 연산이다. SEMA의 값을 증가시키고,
	 대기 중인 스레드가 있으면 그 중 하나를 깨운다.

	 이 함수는 인터럽트 핸들러에서 호출될 수 있다. */
void
sema_up (struct semaphore *sema) {
	enum intr_level old_level;
	ASSERT (sema != NULL);
	old_level = intr_disable ();

	if (!list_empty (&sema->waiters)) {
		// down에서 ordered를 해서 이거 안해도 될 줄 았았다.. 그래서 엄청 해맸다..
		list_sort (&sema->waiters, thread_priority_compare, NULL);
		thread_unblock (list_entry (list_pop_front (&sema->waiters), struct thread, elem));
	}

	sema->value++;
	thread_preempt();
	intr_set_level (old_level);
}

static void sema_test_helper (void *sema_);

/* 두 스레드 사이에서 제어를 주고받는 방식으로 세마포어를 테스트
	 한다. 진행 상황을 보려면 printf()를 삽입해라. */
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

/* sema_self_test()에서 사용하는 스레드 함수. */
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

/* LOCK을 초기화한다. 락은 동시에 최대 하나의 스레드만 소유할
	 수 있다. 이 구현의 락은 재귀적(recursive)이지 않다. 즉, 이미
	 락을 소유한 스레드가 동일한 락을 다시 획득하려고 하면 오류다.

	 락은 초기값이 1인 세마포어의 특수 형태이다. 락과 세마포어의
	 차이는 두 가지다. 첫째, 세마포어는 값이 1보다 클 수 있으나
	 락은 한 번에 하나의 스레드만 소유한다. 둘째, 세마포어는 소유자
	 개념이 없어서 한 스레드가 down하고 다른 스레드가 up할 수 있지만,
	 락은 동일한 스레드가 획득(acquire)하고 해제(release)해야 한다.
	 이러한 제약이 부담스럽다면 락 대신 세마포어를 사용하는 것이
	 적절하다. */
void
lock_init (struct lock *lock) {
	ASSERT (lock != NULL);

	lock->holder = NULL;
	sema_init (&lock->semaphore, 1);
}

/* LOCK을 획득한다. 필요하면 사용 가능해질 때까지 잠긴다. 현재
	 스레드가 이미 락을 소유하고 있어서는 안 된다.

	 이 함수는 잠들 수 있으므로 인터럽트 핸들러 내에서 호출하면
	 안 된다. 인터럽트가 비활성화된 상태에서 호출될 수는 있으나,
	 잠들게 되면 인터럽트는 다시 켜질 수 있다. */
// 락 얻어버리기
void
lock_acquire (struct lock *lock) {
	ASSERT (lock != NULL);
	ASSERT (!intr_context ());
	ASSERT (!lock_held_by_current_thread (lock));

	// 모시.. 락소유자가 NULL타라
	if (lock->holder != NULL) {
		enum intr_level old_level = intr_disable ();
		struct thread *curr = thread_current ();
		curr->waiting_lock = lock;

		// 뒤로 붙혀서 순서대로 탐색할게?
		list_push_back (&lock->holder->donation_list, &curr->donation_elem);
		thread_donate_priority (curr);
		intr_set_level (old_level);
	}

	sema_down (&lock->semaphore);
	thread_current ()->waiting_lock = NULL;
	lock->holder = thread_current ();
}

/* LOCK을 획득하려 시도하고 성공하면 true, 실패하면 false를 반환
	 한다. 현재 스레드가 이미 락을 소유하고 있어서는 안 된다.

	 이 함수는 잠들지 않으므로 인터럽트 핸들러 내에서 호출할 수
	 있다. */
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

/* LOCK을 해제한다. LOCK은 반드시 현재 스레드가 소유하고 있어야
	 한다. (lock_release 함수)

	 인터럽트 핸들러는 락을 획득할 수 없으므로, 인터럽트 핸들러
	 내에서 락을 해제하려는 시도는 의미가 없다. */
// 라꾸 카이죠!!! 
void
lock_release (struct lock *lock) {
	ASSERT (lock != NULL);
	ASSERT (lock_held_by_current_thread (lock));

	enum intr_level old_level = intr_disable ();

	// 헤제 했으니 도네 삭제
	thread_remove_donations (thread_current (), lock);
	// 삭제했으면 읍데이트
	thread_update_priority (thread_current ());
	// 이거 아까 얘기함..
	lock->holder = NULL;

	intr_set_level (old_level);
	sema_up (&lock->semaphore);
}

/* 현재 스레드가 LOCK을 소유하고 있으면 true, 그렇지 않으면 false를
	 반환한다. (다른 스레드가 락을 소유하고 있는지 확인하는 것은
	 레이스 컨디션이 발생할 수 있다.) */
bool
lock_held_by_current_thread (const struct lock *lock) {
	ASSERT (lock != NULL);

	return lock->holder == thread_current ();
}

/* 리스트 안의 하나의 세마포어 요소. */
struct semaphore_elem {
	struct list_elem elem;              /* List element. */
	struct semaphore semaphore;         /* This semaphore. */
};

bool
sema_priority_compare (const struct list_elem *a, const struct list_elem *b, void *aux UNUSED) {
	struct semaphore_elem *sa = list_entry (a, struct semaphore_elem, elem);
	struct semaphore_elem *sb = list_entry (b, struct semaphore_elem, elem);

	if (list_empty (&sa->semaphore.waiters))
		return false;
	if (list_empty (&sb->semaphore.waiters))
		return true;

	return thread_priority_compare (list_front (&sa->semaphore.waiters), list_front (&sb->semaphore.waiters), NULL);
}

/* 조건 변수 COND를 초기화한다. 조건 변수는 한 쪽 코드가 상태를
	 신호하고 다른 쪽 코드가 그 신호를 받아 동작하도록 한다. */
void
cond_init (struct condition *cond) {
	ASSERT (cond != NULL);

	list_init (&cond->waiters);
}

/* LOCK을 원자적으로 해제한 뒤 다른 코드가 COND를 신호할 때까지
	 기다린다. COND가 신호되면 반환 전에 LOCK을 다시 획득한다.
	 이 함수를 호출하기 전에는 LOCK을 소유하고 있어야 한다.

	 이 함수가 구현하는 모니터는 "Mesa" 스타일이며 "Hoare" 스타일이
	 아니다. 즉, 신호의 전송과 수신이 원자적 연산이 아니다. 따라서
	 일반적으로 대기 후에는 조건을 다시 확인하고 필요하면 다시
	 기다려야 한다.

	 특정 조건 변수는 하나의 락과만 연관되지만, 하나의 락은 여러
	 조건 변수와 연관될 수 있다. 즉 락과 조건 변수는 일대다 관계이다.

	 이 함수는 잠들 수 있으므로 인터럽트 핸들러 내에서 호출하면 안
	 된다. 인터럽트가 비활성화된 상태에서 호출될 수는 있으나, 잠들
	 경우 인터럽트는 다시 켜질 수 있다. */
void
cond_wait (struct condition *cond, struct lock *lock) {
	struct semaphore_elem waiter;

	ASSERT (cond != NULL);
	ASSERT (lock != NULL);
	ASSERT (!intr_context ());
	ASSERT (lock_held_by_current_thread (lock));

	sema_init (&waiter.semaphore, 0);
	// list_push_back (&cond->waiters, &waiter.elem);
	list_insert_ordered(&cond->waiters, &waiter.elem, sema_priority_compare, NULL);
	lock_release (lock);
	sema_down (&waiter.semaphore);
	lock_acquire (lock);
}

/* 만약 COND에 대해 대기 중인 스레드가 있으면(LOCK으로 보호됨)
	 이 함수는 그들 중 하나에게 신호를 보내 대기에서 깨어나게 한다.
	 이 함수를 호출하기 전에 LOCK을 소유하고 있어야 한다.

	 인터럽트 핸들러는 락을 획득할 수 없으므로, 인터럽트 핸들러
	 내에서 조건 변수에 신호를 보내려는 시도는 의미가 없다. */
void
cond_signal (struct condition *cond, struct lock *lock UNUSED) {
	ASSERT (cond != NULL);
	ASSERT (lock != NULL);
	ASSERT (!intr_context ());
	ASSERT (lock_held_by_current_thread (lock));

	if (!list_empty (&cond->waiters)) {
		struct list_elem *e = list_min (&cond->waiters, sema_priority_compare, NULL);
		list_remove(e);
		sema_up (&list_entry (e, struct semaphore_elem, elem)->semaphore);
	}
}

/* COND에 대해 대기 중인 모든 스레드를 깨운다(LOCK으로 보호됨).
	 이 함수를 호출하기 전에 LOCK을 소유하고 있어야 한다.

	 인터럽트 핸들러는 락을 획득할 수 없으므로, 이 함수 내에서
	 조건 변수에 신호를 보내려는 시도는 의미가 없다. */
void
cond_broadcast (struct condition *cond, struct lock *lock) {
	ASSERT (cond != NULL);
	ASSERT (lock != NULL);

	while (!list_empty (&cond->waiters))
		cond_signal (cond, lock);
}

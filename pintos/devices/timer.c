#include "devices/timer.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include "threads/interrupt.h"
#include "threads/io.h"
#include "threads/synch.h"
#include "threads/thread.h"

/* 8254 타이머 칩의 하드웨어 세부 사항은 [8254]를 참고하세요. */

#if TIMER_FREQ < 19
#error 8254 timer requires TIMER_FREQ >= 19
#endif
#if TIMER_FREQ > 1000
#error TIMER_FREQ <= 1000 recommended
#endif

extern struct list sleep_list;
/* OS가 부팅된 이후의 타이머 틱 수. */
static int64_t ticks;

/* 타이머 틱당 루프 횟수.
	timer_calibrate()에 의해 초기화됩니다. */
static unsigned loops_per_tick;

static intr_handler_func timer_interrupt;
static bool too_many_loops (unsigned loops);
static void busy_wait (int64_t loops);
static void real_time_sleep (int64_t num, int32_t denom);
extern void thread_sleep(int64_t _tick);

/* 8254 프로그래머블 인터벌 타이머(PIT)를 설정하여
	PIT_FREQ 초당 인터럽트를 발생시키고, 해당 인터럽트를 등록합니다. */
void
timer_init (void) {
		/* 8254 입력 주파수를 TIMER_FREQ로 나눈 값(가장 가까운 값으로 반올림). */
	uint16_t count = (1193180 + TIMER_FREQ / 2) / TIMER_FREQ;

	outb (0x43, 0x34);    /* CW: counter 0, LSB then MSB, mode 2, binary. */
	outb (0x40, count & 0xff);
	outb (0x40, count >> 8);

	intr_register_ext (0x20, timer_interrupt, "8254 Timer");
}

/* 짧은 지연을 구현하기 위해 loops_per_tick을 보정합니다. */
void
timer_calibrate (void) {
	unsigned high_bit, test_bit;

	ASSERT (intr_get_level () == INTR_ON);
	printf ("Calibrating timer...  ");

		/* loops_per_tick을 한 타이머 틱보다 작은 가장 큰 2의 거듭제곱으로 근사합니다. */
	loops_per_tick = 1u << 10;
	while (!too_many_loops (loops_per_tick << 1)) {
		loops_per_tick <<= 1;
		ASSERT (loops_per_tick != 0);
	}

		/* loops_per_tick의 다음 8비트를 더 정밀하게 조정합니다. */
	high_bit = loops_per_tick;
	for (test_bit = high_bit >> 1; test_bit != high_bit >> 10; test_bit >>= 1)
		if (!too_many_loops (high_bit | test_bit))
			loops_per_tick |= test_bit;

	printf ("%'"PRIu64" loops/s.\n", (uint64_t) loops_per_tick * TIMER_FREQ);
}

/* OS가 부팅된 이후의 타이머 틱 수를 반환합니다. */
int64_t
timer_ticks (void) {
	enum intr_level old_level = intr_disable ();
	int64_t t = ticks;
	intr_set_level (old_level);
	barrier ();
	return t;
}

/* THEN(이전 값) 이후 경과한 타이머 틱 수를 반환합니다.
	THEN은 timer_ticks()가 반환한 값이어야 합니다. */
int64_t
timer_elapsed (int64_t then) {
	return timer_ticks () - then;
}

/* 약 TICKS 타이머 틱 동안 실행을 중단합니다. */
void
timer_sleep (int64_t _tick) {
	int64_t start = timer_ticks ();

	ASSERT (intr_get_level () == INTR_ON);
	thread_sleep(start + _tick);
}

/* 약 MS 밀리초 동안 실행을 중단합니다. */
void
timer_msleep (int64_t ms) {
	real_time_sleep (ms, 1000);
}

/* 약 US 마이크로초 동안 실행을 중단합니다. */
void
timer_usleep (int64_t us) {
	real_time_sleep (us, 1000 * 1000);
}

/* 약 NS 나노초 동안 실행을 중단합니다. */
void
timer_nsleep (int64_t ns) {
	real_time_sleep (ns, 1000 * 1000 * 1000);
}

/* 타이머 통계를 출력합니다. */
void
timer_print_stats (void) {
	printf ("Timer: %"PRId64" ticks\n", timer_ticks ());
}

/* 타이머 인터럽트 핸들러. */
static void
timer_interrupt (struct intr_frame *args UNUSED) {
	ticks++;
	thread_tick ();

	while (!list_empty(&sleep_list)){
		struct list_elem *e = list_front(&sleep_list);
		struct thread* pthread = list_entry(e, struct thread, elem);
	
		if (pthread->m_tick <= ticks){
			thread_wakeUp(pthread);
		}
		else{
			break;
		}
	}
}

/* LOOPS 반복이 한 타이머 틱 이상 대기하면 true를 반환하고, 그렇지 않으면 false를 반환합니다. */
static bool
too_many_loops (unsigned loops) {
		/* 타이머 틱을 기다립니다. */
	int64_t start = ticks;
	while (ticks == start)
		barrier ();

		/* LOOPS만큼 루프를 실행합니다. */
	start = ticks;
	busy_wait (loops);

		/* 틱 카운트가 변경되었다면, 반복 시간이 너무 길었던 것입니다. */
	barrier ();
	return start != ticks;
}

/* 짧은 지연을 구현하기 위해 LOOPS만큼 단순 루프를 반복합니다.

	NO_INLINE으로 표시된 이유는 코드 정렬이 타이밍에 큰 영향을 줄 수 있기 때문입니다.
	이 함수가 서로 다른 위치에서 다르게 인라인되면 결과를 예측하기 어려워집니다. */
static void NO_INLINE
busy_wait (int64_t loops) {
	while (loops-- > 0)
		barrier ();
}

/* 약 NUM/DENOM 초 동안 대기합니다. */
static void
real_time_sleep (int64_t num, int32_t denom) {
	  /* NUM/DENOM 초를 타이머 틱으로 변환합니다(내림).

		  (NUM / DENOM) 초
		  ---------------------- = NUM * TIMER_FREQ / DENOM 틱
		  1초 / TIMER_FREQ 틱
	  */
	int64_t ticks = num * TIMER_FREQ / denom;

	ASSERT (intr_get_level () == INTR_ON);
	if (ticks > 0) {
		 /* 최소 한 번의 전체 타이머 틱을 기다리는 경우입니다.
			 timer_sleep()을 사용하면 CPU를 다른 프로세스에 양보할 수 있습니다. */
		timer_sleep (ticks);
	} else {
		 /* 그렇지 않은 경우, 더 정확한 서브-틱 타이밍을 위해 busy-wait 루프를 사용합니다.
			 오버플로우 가능성을 피하기 위해 분자와 분모를 1000으로 나눕니다. */
		ASSERT (denom % 1000 == 0);
		busy_wait (loops_per_tick * num / 1000 * TIMER_FREQ / (denom / 1000));
	}
}

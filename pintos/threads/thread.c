#include "threads/thread.h"
#include <debug.h>
#include <stddef.h>
#include <random.h>
#include <stdio.h>
#include <string.h>
#include "threads/flags.h"
#include "threads/interrupt.h"
#include "threads/intr-stubs.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "intrinsic.h"
#ifdef USERPROG
#include "userprog/process.h"
#endif

/* struct thread의 `magic' 멤버를 위한 랜덤 값.
   스택 오버플로우를 감지하는 데 사용됩니다. 자세한 내용은
   thread.h 상단의 큰 주석을 참조하세요. */
#define THREAD_MAGIC 0xcd6abf4b

/* 기본 스레드를 위한 랜덤 값
   이 값을 수정하지 마세요. */
#define THREAD_BASIC 0xd42df210

/* THREAD_READY 상태의 프로세스 목록, 즉
   실행할 준비가 되었지만 실제로는 실행되지 않는 프로세스들. */
static struct list ready_list;

/* THREAD_BLOCKED 상태의 프로세스 목록, 즉
   잠들어 있고 깨어나기를 기다리는 프로세스들. */
static struct list sleep_list;

/* Idle 스레드. */
static struct thread *idle_thread;

/* 초기 스레드, init.c:main()을 실행하는 스레드. */
static struct thread *initial_thread;

/* allocate_tid()에서 사용하는 락. */
static struct lock tid_lock;

/* 스레드 파괴 요청 */
static struct list destruction_req;

/* 통계. */
static long long idle_ticks;    /* Idle 상태에서 보낸 타이머 틱 수. */
static long long kernel_ticks;  /* 커널 스레드의 타이머 틱 수. */
static long long user_ticks;    /* 사용자 프로그램의 타이머 틱 수. */

/* 스케줄링. */
#define TIME_SLICE 4            /* 각 스레드에 할당할 타이머 틱 수. */
static unsigned thread_ticks;   /* 마지막 yield 이후의 타이머 틱 수. */

/* false(기본값)이면 라운드 로빈 스케줄러를 사용합니다.
   true이면 다단계 피드백 큐 스케줄러를 사용합니다.
   커널 명령줄 옵션 "-o mlfqs"로 제어됩니다. */
bool thread_mlfqs;

static void kernel_thread (thread_func *, void *aux);

static void idle (void *aux UNUSED);
static struct thread *next_thread_to_run (void);
static void init_thread (struct thread *, const char *name, int priority);
static void do_schedule(int status);
static void schedule (void);
static tid_t allocate_tid (void);

/* T가 유효한 스레드를 가리키는 것처럼 보이면 true를 반환합니다. */
#define is_thread(t) ((t) != NULL && (t)->magic == THREAD_MAGIC)

/* 실행 중인 스레드를 반환합니다.
 * CPU의 스택 포인터 `rsp'를 읽은 다음
 * 페이지의 시작 부분으로 내림합니다. `struct thread'는
 * 항상 페이지의 시작 부분에 있고 스택 포인터는
 * 중간 어딘가에 있으므로, 이것이 현재 스레드를 찾습니다. */
#define running_thread() ((struct thread *) (pg_round_down (rrsp ())))


// thread_start를 위한 전역 디스크립터 테이블.
// gdt는 thread_init 이후에 설정되므로,
// 먼저 임시 gdt를 설정해야 합니다.
static uint64_t gdt[3] = { 0, 0x00af9a000000ffff, 0x00cf92000000ffff };

struct child_info *
find_child_info(struct thread *parent, tid_t child_tid) {
	for (struct list_elem *e = list_begin(&parent->child_list);
         e != list_end(&parent->child_list);
         e = list_next(e)) {

        struct child_info *ci = list_entry(e, struct child_info, elem);
        if (ci->tid == child_tid)
            return ci;
    }
    return NULL;
}

bool
thread_sleep_compare (const struct list_elem *a, const struct list_elem *b, void *aux UNUSED) {
	const struct thread *ta = list_entry (a, struct thread, elem);
	const struct thread *tb = list_entry (b, struct thread, elem);
	return ta->sleep_until < tb->sleep_until;
}

bool
thread_priority_compare (const struct list_elem *a, const struct list_elem *b, void *aux UNUSED) {
	const struct thread *ta = list_entry (a, struct thread, elem);
	const struct thread *tb = list_entry (b, struct thread, elem);
	return ta->priority > tb->priority;
}

static bool
thread_donation_priority_less (const struct list_elem *a,
		const struct list_elem *b, void *aux UNUSED) {
	const struct thread *ta = list_entry (a, struct thread, donation_elem);
	const struct thread *tb = list_entry (b, struct thread, donation_elem);
	return ta->priority < tb->priority;
}

// 스레드의 우선순위를 재갱신합니다
void
thread_update_priority (struct thread *t) {
	ASSERT (t != NULL);

	int max_priority = t->default_priority;

	if (!list_empty (&t->donation_list)) {
		struct list_elem *max_elem = list_max (&t->donation_list, thread_donation_priority_less, NULL);
		struct thread *donor = list_entry (max_elem, struct thread, donation_elem);

		if (donor->priority > max_priority)
			max_priority = donor->priority;
	}

	t->priority = max_priority;
}

// 특정 락과 관련된 우선순위 기부 제거
void
thread_remove_donations (struct thread *t, struct lock *lock) {
	ASSERT (t != NULL);

	struct list_elem *e = list_begin (&t->donation_list);

	while (e != list_end (&t->donation_list)) {
		struct thread *donor = list_entry (e, struct thread, donation_elem);
		struct list_elem *next = list_next (e);

		if (donor->waiting_lock == lock)
			list_remove (e);

		e = next;
	}
}

// 우선순위 기부를 수행합니다!
void
thread_donate_priority (struct thread *t) {
	// 락을 계속 위로 전달해야 합니다..
	while (t->waiting_lock != NULL) {
		struct thread *holder = t->waiting_lock->holder;

		if (holder == NULL)
			break;

		// 우선순위를 갱신합니다!
		thread_update_priority (holder);
		t = holder;
	}
}

/* 현재 실행 중인 코드를 스레드로 변환하여 스레딩 시스템을 초기화합니다.
   이것은 일반적으로 작동할 수 없으며, 이 경우에만 가능한 이유는
   loader.S가 스택의 바닥을 페이지 경계에 배치하도록 주의했기 때문입니다.

   또한 실행 큐와 tid 락을 초기화합니다.

   이 함수를 호출한 후, thread_create()로 스레드를 생성하기 전에
   페이지 할당자를 초기화해야 합니다.

   이 함수가 완료될 때까지 thread_current()를 호출하는 것은 안전하지 않습니다. */
void
thread_init (void) {
	ASSERT (intr_get_level () == INTR_OFF);

	/* 커널을 위한 임시 gdt를 다시 로드합니다
	 * 이 gdt는 사용자 컨텍스트를 포함하지 않습니다.
	 * 커널은 gdt_init()에서 사용자 컨텍스트를 포함하여 gdt를 재구축합니다. */
	struct desc_ptr gdt_ds = {
		.size = sizeof (gdt) - 1,
		.address = (uint64_t) gdt
	};
	lgdt (&gdt_ds);

	/* 전역 스레드 컨텍스트 초기화 */
	lock_init (&tid_lock);
	list_init (&ready_list);
	list_init (&sleep_list);
	list_init (&destruction_req);

	/* 실행 중인 스레드를 위한 스레드 구조체를 설정합니다. */
	initial_thread = running_thread ();
	init_thread (initial_thread, "main", PRI_DEFAULT);
	initial_thread->status = THREAD_RUNNING;
	initial_thread->tid = allocate_tid ();
}

/* 인터럽트를 활성화하여 선점형 스레드 스케줄링을 시작합니다.
   또한 유휴 스레드를 생성합니다. */
void
thread_start (void) {
	/* Idle 스레드를 생성합니다. */
	struct semaphore idle_started;
	sema_init (&idle_started, 0);
	thread_create ("idle", PRI_MIN, idle, &idle_started);

	/* 선점형 스레드 스케줄링을 시작합니다. */
	intr_enable ();

	/* Idle 스레드가 idle_thread를 초기화할 때까지 대기합니다. */
	sema_down (&idle_started);
}

/* 각 타이머 틱마다 타이머 인터럽트 핸들러에 의해 호출됩니다.
   따라서 이 함수는 외부 인터럽트 컨텍스트에서 실행됩니다. */
void
thread_tick (void) {
	struct thread *t = thread_current ();

	/* 통계를 업데이트합니다. */
	if (t == idle_thread)
		idle_ticks++;
#ifdef USERPROG
	else if (t->pml4 != NULL)
		user_ticks++;
#endif
	else
		kernel_ticks++;

	/* 선점을 강제합니다. */
	if (++thread_ticks >= TIME_SLICE)
		intr_yield_on_return ();
}

/* 스레드 통계를 출력합니다. */
void
thread_print_stats (void) {
	printf ("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",
			idle_ticks, kernel_ticks, user_ticks);
}

/* 주어진 초기 PRIORITY로 NAME이라는 새 커널 스레드를 생성하고,
   AUX를 인수로 전달하여 FUNCTION을 실행하며,
   준비 큐에 추가합니다. 새 스레드의 스레드 식별자를 반환하거나,
   생성에 실패하면 TID_ERROR를 반환합니다.

   thread_start()가 호출되었다면, 새 스레드는 thread_create()가
   반환되기 전에 스케줄링될 수 있습니다. 심지어 thread_create()가
   반환되기 전에 종료될 수도 있습니다. 반대로, 원래 스레드는
   새 스레드가 스케줄링되기 전에 얼마든지 실행될 수 있습니다.
   순서를 보장해야 한다면 세마포어나 다른 형태의 동기화를 사용하세요.

   제공된 코드는 새 스레드의 `priority' 멤버를 PRIORITY로 설정하지만,
   실제 우선순위 스케줄링은 구현되지 않았습니다.
   우선순위 스케줄링은 문제 1-3의 목표입니다. */
tid_t
thread_create (const char *name, int priority,
		thread_func *function, void *aux) {
	struct thread *t;
	tid_t tid;



	ASSERT (function != NULL);

	/* 스레드를 할당합니다. */
	t = palloc_get_page (PAL_ZERO);
	if (t == NULL)
		return TID_ERROR;

	/* 스레드를 초기화합니다. */
	init_thread (t, name, priority);
	tid = t->tid = allocate_tid ();

	/* 스케줄링되면 kernel_thread를 호출합니다.
	 * 참고) rdi는 첫 번째 인수이고, rsi는 두 번째 인수입니다. */
	t->tf.rip = (uintptr_t) kernel_thread;
	t->tf.R.rdi = (uint64_t) function;
	t->tf.R.rsi = (uint64_t) aux;
	t->tf.ds = SEL_KDSEG;
	t->tf.es = SEL_KDSEG;
	t->tf.ss = SEL_KDSEG;
	t->tf.cs = SEL_KCSEG; 
	t->tf.eflags = FLAG_IF;

	/* 실행 큐에 추가합니다. */
	thread_unblock (t);

	thread_preempt ();

	return tid;
}

/* 현재 스레드를 잠들게 합니다. thread_unblock()에 의해
   깨어날 때까지 다시 스케줄링되지 않습니다.

   이 함수는 인터럽트가 꺼진 상태에서 호출되어야 합니다.
   일반적으로 synch.h의 동기화 기본 요소 중 하나를 사용하는 것이
   더 좋은 방법입니다. */
void
thread_block (void) {
	ASSERT (!intr_context ());
	ASSERT (intr_get_level () == INTR_OFF);
	thread_current ()->status = THREAD_BLOCKED;
	schedule ();
}

/* 차단된 스레드 T를 실행 준비 상태로 전환합니다.
   T가 차단되지 않은 경우 오류입니다. (thread_yield()를 사용하여
   실행 중인 스레드를 준비 상태로 만드세요.)

   이 함수는 실행 중인 스레드를 선점하지 않습니다. 이것은
   중요할 수 있습니다: 호출자가 인터럽트를 직접 비활성화한 경우,
   스레드를 원자적으로 차단 해제하고 다른 데이터를 업데이트할 수 있다고
   기대할 수 있습니다. */
void
thread_unblock (struct thread *t) {
	enum intr_level old_level;

	ASSERT (is_thread (t));

	old_level = intr_disable ();
	ASSERT (t->status == THREAD_BLOCKED);
	list_insert_ordered (&ready_list, &t->elem, thread_priority_compare, NULL);
	t->status = THREAD_READY;
	intr_set_level (old_level);
}

void
thread_sleep (int64_t ticks) {
	struct thread *cur = thread_current ();

	enum intr_level old_level = intr_disable ();

	cur->sleep_until = ticks;
	list_insert_ordered (&sleep_list, &cur->elem, thread_sleep_compare, NULL);
	thread_block ();
	intr_set_level (old_level);
}

void
thread_wake (int64_t ticks) {
	ASSERT (intr_context ());
	while (!list_empty (&sleep_list)) {
		struct list_elem *e = list_front (&sleep_list);
		struct thread *t = list_entry (e, struct thread, elem);
		if (t->sleep_until <= ticks) {
			list_remove(e);
			thread_unblock (t);
		}
		else
			break;
	}
	thread_preempt ();
}

/* 실행 중인 스레드의 이름을 반환합니다. */
const char *
thread_name (void) {
	return thread_current ()->name;
}

struct thread *
thread_current (void) {
	struct thread *t = running_thread ();

	/* T가 정말 스레드인지 확인합니다.
	   이러한 어설션 중 하나라도 발생하면 스레드가
	   스택을 오버플로우했을 수 있습니다. 각 스레드는
	   4KB 미만의 스택을 가지므로, 큰 자동 배열이나
	   적당한 재귀는 스택 오버플로우를 일으킬 수 있습니다. */
	ASSERT (is_thread (t));
	ASSERT (t->status == THREAD_RUNNING);

	return t;
}



/* 실행 중인 스레드의 tid를 반환합니다. */
tid_t
thread_tid (void) {
	return thread_current ()->tid;
}

/* 현재 스레드를 스케줄에서 제거하고 파괴합니다.
   호출자에게 절대 반환하지 않습니다. */
void
thread_exit (void) {
	ASSERT (!intr_context ());

#ifdef USERPROG
	process_exit ();
#endif

	/* 상태를 dying으로 설정하고 다른 프로세스를 스케줄링합니다.
	   schedule_tail() 호출 중에 파괴됩니다. */
	intr_disable ();
	do_schedule (THREAD_DYING);
	NOT_REACHED ();
}

/* CPU를 양보합니다. 현재 스레드는 잠들지 않으며
   스케줄러의 판단에 따라 즉시 다시 스케줄링될 수 있습니다. */
void
thread_yield (void) {
	struct thread *curr = thread_current ();
	enum intr_level old_level;

	ASSERT (!intr_context ());

	old_level = intr_disable ();
	if (curr != idle_thread)
		list_insert_ordered (&ready_list, &curr->elem, thread_priority_compare, NULL);
	do_schedule (THREAD_READY);
	intr_set_level (old_level);
}

void thread_preempt (void) {
	enum intr_level old_level = intr_disable ();

	if (list_empty (&ready_list))
		return;

	struct thread *curr = thread_current ();
	struct thread *next = list_entry (list_front (&ready_list), struct thread, elem);
	if (next->priority > curr->priority) {
		if (intr_context ())
			intr_yield_on_return ();
		else
			thread_yield ();
	}
	intr_set_level (old_level);
}

// 이 스레드의 우선순위를 new_priority로 설정합니다
void
thread_set_priority (int new_priority) {
	struct thread *curr = thread_current ();
	enum intr_level old_level = intr_disable ();
	int old_priority = curr->priority;

	curr->default_priority = new_priority;
	thread_update_priority (curr);

	intr_set_level (old_level);
  
	if (curr->priority < old_priority)
		thread_yield ();
}

/* 현재 스레드의 우선순위를 반환합니다. */
int
thread_get_priority (void) {
	return thread_current ()->priority;
}

/* 현재 스레드의 nice 값을 NICE로 설정합니다. */
void
thread_set_nice (int nice UNUSED) {
	/* TODO: 여기에 구현을 작성하세요 */
}

/* 현재 스레드의 nice 값을 반환합니다. */
int
thread_get_nice (void) {
	/* TODO: 여기에 구현을 작성하세요 */
	return 0;
}

/* 시스템 로드 평균의 100배를 반환합니다. */
int
thread_get_load_avg (void) {
	/* TODO: 여기에 구현을 작성하세요 */
	return 0;
}

/* 현재 스레드의 recent_cpu 값의 100배를 반환합니다. */
int
thread_get_recent_cpu (void) {
	/* TODO: 여기에 구현을 작성하세요 */
	return 0;
}

/* 유휴 스레드. 다른 스레드가 실행 준비가 되지 않았을 때 실행됩니다.

   유휴 스레드는 처음에 thread_start()에 의해 준비 목록에 배치됩니다.
   처음에 한 번 스케줄링되며, 이 시점에 idle_thread를 초기화하고,
   전달된 세마포어를 "up"하여 thread_start()가 계속되도록 하고,
   즉시 차단합니다. 그 후 Idle 스레드는 준비 목록에 나타나지 않습니다.
   준비 목록이 비어 있을 때 특수한 경우로 next_thread_to_run()에 의해
   반환됩니다. */
static void
idle (void *idle_started_ UNUSED) {
	struct semaphore *idle_started = idle_started_;

	idle_thread = thread_current ();
	sema_up (idle_started);

	for (;;) {
		/* 다른 스레드가 실행되도록 합니다. */
		intr_disable ();
		thread_block ();

		/* 인터럽트를 다시 활성화하고 다음 인터럽트를 기다립니다.

		   `sti' 명령은 다음 명령이 완료될 때까지 인터럽트를 비활성화하므로,
		   이 두 명령은 원자적으로 실행됩니다. 이 원자성은
		   중요합니다. 그렇지 않으면 인터럽트를 다시 활성화하고
		   다음 인터럽트가 발생할 때까지 기다리는 사이에 인터럽트가
		   처리될 수 있어, 최대 한 클럭 틱만큼의 시간을 낭비할 수 있습니다.

		   [IA32-v2a] "HLT", [IA32-v2b] "STI", [IA32-v3a]
		   7.11.1 "HLT Instruction"을 참조하세요. */
		asm volatile ("sti; hlt" : : : "memory");
	}
}

/* 커널 스레드의 기반으로 사용되는 함수. */
static void
kernel_thread (thread_func *function, void *aux) {
	ASSERT (function != NULL);

	intr_enable ();       /* 스케줄러는 인터럽트가 꺼진 상태에서 실행됩니다. */
	function (aux);       /* 스레드 함수를 실행합니다. */
	thread_exit ();       /* function()이 반환되면 스레드를 종료합니다. */
}


/* NAME이라는 차단된 스레드로 T를 기본 초기화합니다. */
static void
init_thread (struct thread *t, const char *name, int priority) {
	ASSERT (t != NULL);
	ASSERT (PRI_MIN <= priority && priority <= PRI_MAX);
	ASSERT (name != NULL);

	memset (t, 0, sizeof *t);
	t->status = THREAD_BLOCKED;
	strlcpy (t->name, name, sizeof t->name);
	t->tf.rsp = (uint64_t) t + PGSIZE - sizeof (void *);
	t->magic = THREAD_MAGIC;

	// 초기화를 빼먹지 않기!
	t->priority = priority;
	t->default_priority = priority;
	t->waiting_lock = NULL;
	list_init(&t->donation_list);
	list_init(&t->child_list);
}

/* 스케줄링할 다음 스레드를 선택하고 반환합니다.
   실행 큐가 비어 있지 않으면 실행 큐에서 스레드를 반환해야 합니다.
   (실행 중인 스레드가 계속 실행할 수 있다면 실행 큐에 있을 것입니다.)
   실행 큐가 비어 있으면 idle_thread를 반환합니다. */
static struct thread *
next_thread_to_run (void) {
	if (list_empty (&ready_list))
		return idle_thread;
	else
		return list_entry (list_pop_front (&ready_list), struct thread, elem);
}

/* iretq를 사용하여 스레드를 시작합니다 */
void
do_iret (struct intr_frame *tf) {
	__asm __volatile(
			"movq %0, %%rsp\n"
			"movq 0(%%rsp),%%r15\n"
			"movq 8(%%rsp),%%r14\n"
			"movq 16(%%rsp),%%r13\n"
			"movq 24(%%rsp),%%r12\n"
			"movq 32(%%rsp),%%r11\n"
			"movq 40(%%rsp),%%r10\n"
			"movq 48(%%rsp),%%r9\n"
			"movq 56(%%rsp),%%r8\n"
			"movq 64(%%rsp),%%rsi\n"
			"movq 72(%%rsp),%%rdi\n"
			"movq 80(%%rsp),%%rbp\n"
			"movq 88(%%rsp),%%rdx\n"
			"movq 96(%%rsp),%%rcx\n"
			"movq 104(%%rsp),%%rbx\n"
			"movq 112(%%rsp),%%rax\n"
			"addq $120,%%rsp\n"
			"movw 8(%%rsp),%%ds\n"
			"movw (%%rsp),%%es\n"
			"addq $32, %%rsp\n"
			"iretq"
			: : "g" ((uint64_t) tf) : "memory");
}

/* 새 스레드의 페이지 테이블을 활성화하여 스레드를 전환하고,
   이전 스레드가 dying 상태이면 파괴합니다.

   이 함수가 호출될 때, 우리는 방금 스레드 PREV에서 전환했고,
   새 스레드는 이미 실행 중이며 인터럽트는 여전히 비활성화되어 있습니다.

   스레드 전환이 완료될 때까지 printf()를 호출하는 것은 안전하지 않습니다.
   실제로는 printf()를 함수 끝에 추가해야 함을 의미합니다. */
static void
thread_launch (struct thread *th) {
	uint64_t tf_cur = (uint64_t) &running_thread ()->tf;
	uint64_t tf = (uint64_t) &th->tf;
	ASSERT (intr_get_level () == INTR_OFF);

	/* 주요 전환 로직.
	 * 먼저 전체 실행 컨텍스트를 intr_frame으로 복원한 다음
	 * do_iret을 호출하여 다음 스레드로 전환합니다.
	 * 전환이 완료될 때까지 여기서부터 스택을 사용하지 않아야 합니다. */
	__asm __volatile (
			/* 사용될 레지스터를 저장합니다. */
			"push %%rax\n"
			"push %%rbx\n"
			"push %%rcx\n"
			/* 입력을 한 번 가져옵니다 */
			"movq %0, %%rax\n"
			"movq %1, %%rcx\n"
			"movq %%r15, 0(%%rax)\n"
			"movq %%r14, 8(%%rax)\n"
			"movq %%r13, 16(%%rax)\n"
			"movq %%r12, 24(%%rax)\n"
			"movq %%r11, 32(%%rax)\n"
			"movq %%r10, 40(%%rax)\n"
			"movq %%r9, 48(%%rax)\n"
			"movq %%r8, 56(%%rax)\n"
			"movq %%rsi, 64(%%rax)\n"
			"movq %%rdi, 72(%%rax)\n"
			"movq %%rbp, 80(%%rax)\n"
			"movq %%rdx, 88(%%rax)\n"
			"pop %%rbx\n"              // 저장된 rcx
			"movq %%rbx, 96(%%rax)\n"
			"pop %%rbx\n"              // 저장된 rbx
			"movq %%rbx, 104(%%rax)\n"
			"pop %%rbx\n"              // 저장된 rax
			"movq %%rbx, 112(%%rax)\n"
			"addq $120, %%rax\n"
			"movw %%es, (%%rax)\n"
			"movw %%ds, 8(%%rax)\n"
			"addq $32, %%rax\n"
			"call __next\n"         // 현재 rip를 읽습니다.
			"__next:\n"
			"pop %%rbx\n"
			"addq $(out_iret -  __next), %%rbx\n"
			"movq %%rbx, 0(%%rax)\n" // rip
			"movw %%cs, 8(%%rax)\n"  // cs
			"pushfq\n"
			"popq %%rbx\n"
			"mov %%rbx, 16(%%rax)\n" // eflags
			"mov %%rsp, 24(%%rax)\n" // rsp
			"movw %%ss, 32(%%rax)\n"
			"mov %%rcx, %%rdi\n"
			"call do_iret\n"
			"out_iret:\n"
			: : "g"(tf_cur), "g" (tf) : "memory"
			);
}

/* 새 프로세스를 스케줄링합니다. 진입 시 인터럽트가 꺼져 있어야 합니다.
 * 이 함수는 현재 스레드의 상태를 status로 수정한 다음
 * 실행할 다른 스레드를 찾아 전환합니다.
 * schedule()에서 printf()를 호출하는 것은 안전하지 않습니다. */
static void
do_schedule(int status) {
	ASSERT (intr_get_level () == INTR_OFF);
	ASSERT (thread_current()->status == THREAD_RUNNING);
	while (!list_empty (&destruction_req)) {
		struct thread *victim =
			list_entry (list_pop_front (&destruction_req), struct thread, elem);
		palloc_free_page(victim);
	}
	thread_current ()->status = status;
	schedule ();
}

static void
schedule (void) {
	struct thread *curr = running_thread ();
	struct thread *next = next_thread_to_run ();

	ASSERT (intr_get_level () == INTR_OFF);
	ASSERT (curr->status != THREAD_RUNNING);
	ASSERT (is_thread (next));
	/* 실행 중으로 표시합니다. */
	next->status = THREAD_RUNNING;

	/* 새 타임 슬라이스를 시작합니다. */
	thread_ticks = 0;

#ifdef USERPROG
	/* Activate the new address space. */
	process_activate (next);
#endif

	if (curr != next) {
		/* 전환한 스레드가 dying 상태이면 그 struct thread를 파괴합니다.
		   이것은 늦게 발생해야 하므로 thread_exit()이 자신의 기반을
		   제거하지 않습니다.
		   페이지가 현재 스택에 사용되고 있기 때문에 여기서는
		   페이지 해제 요청만 큐에 넣습니다.
		   실제 파괴 로직은 schedule()의 시작 부분에서 호출됩니다. */
		if (curr && curr->status == THREAD_DYING && curr != initial_thread) {
			ASSERT (curr != next);
			list_push_back (&destruction_req, &curr->elem);
		}

		/* 스레드를 전환하기 전에 먼저 현재 실행 중인
		 * 정보를 저장합니다. */
		thread_launch (next);
	}
}

/* 새 스레드에 사용할 tid를 반환합니다. */
static tid_t
allocate_tid (void) {
	static tid_t next_tid = 1;
	tid_t tid;

	lock_acquire (&tid_lock);
	tid = next_tid++;
	lock_release (&tid_lock);

	return tid;
}

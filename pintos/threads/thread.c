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

/* struct thread의 `magic' 멤버를 위한 임의의 값.
	스택 오버플로우를 감지하는 데 사용됩니다. 자세한 내용은 thread.h 상단의
	큰 주석을 참고하세요. */
#define THREAD_MAGIC 0xcd6abf4b

/* 기본 스레드를 위한 임의의 값
	이 값은 수정하지 마세요. */
#define THREAD_BASIC 0xd42df210

/* THREAD_READY 상태에 있는 프로세스들의 리스트.
	즉, 실행할 준비는 되었지만 실제로 실행 중은 아닌 프로세스들입니다. */
static struct list ready_list;
struct list sleep_list;

/* Idle(유휴) 스레드. */
static struct thread *idle_thread;

/* 초기 스레드, init.c:main()을 실행하는 스레드. */
static struct thread *initial_thread;

/* allocate_tid()에서 사용하는 락. */
static struct lock tid_lock;

/* 스레드 파괴 요청 리스트 */
static struct list destruction_req;

/* 통계 정보. */
static long long idle_ticks;    /* idle 상태에서 소모된 타이머 틱 수. */
static long long kernel_ticks;  /* 커널 스레드에서 소모된 타이머 틱 수. */
static long long user_ticks;    /* 사용자 프로그램에서 소모된 타이머 틱 수. */

/* 스케줄링 관련. */
#define TIME_SLICE 4            /* 각 스레드에 할당되는 타이머 틱 수. */
static unsigned thread_ticks;   /* 마지막 yield 이후 경과한 타이머 틱 수. */

/* false(기본값)이면 라운드 로빈 스케줄러를 사용.
	true면 다단계 피드백 큐 스케줄러를 사용.
	커널 커맨드라인 옵션 "-o mlfqs"로 제어됩니다. */
bool thread_mlfqs;

static void kernel_thread (thread_func *, void *aux);

static void idle (void *aux UNUSED);
static struct thread *next_thread_to_run (void);
static void init_thread (struct thread *, const char *name, int priority);
static void do_schedule(int status);
static void schedule (void);
static tid_t allocate_tid (void);
void thread_sleep(int64_t ticks);
static bool list_cmp(const struct list_elem *a, const struct list_elem *b, void* aux UNUSED);

/* T가 올바른 스레드를 가리키는 것처럼 보이면 true를 반환합니다. */
#define is_thread(t) ((t) != NULL && (t)->magic == THREAD_MAGIC)

/* 현재 실행 중인 스레드를 반환합니다.
 * CPU의 스택 포인터 `rsp'를 읽고, 페이지의 시작 주소로 내림합니다.
 * `struct thread'는 항상 페이지의 시작에 위치하고, 스택 포인터는 중간에 있으므로
 * 현재 스레드를 찾을 수 있습니다. */
#define running_thread() ((struct thread *) (pg_round_down (rrsp ())))


// thread_start를 위한 글로벌 디스크립터 테이블.
// gdt가 thread_init 이후에 설정되므로, 임시 gdt를 먼저 설정해야 합니다.
static uint64_t gdt[3] = { 0, 0x00af9a000000ffff, 0x00cf92000000ffff };

/* 현재 실행 중인 코드를 스레드로 변환하여 스레딩 시스템을 초기화합니다.
	일반적으로는 불가능하지만, loader.S가 스택의 바닥을 페이지 경계에 맞추었기 때문에 가능합니다.

	실행 큐와 tid 락도 초기화합니다.

	이 함수를 호출한 후에는 thread_create()로 스레드를 생성하기 전에
	반드시 페이지 할당자를 초기화해야 합니다.

	이 함수가 끝나기 전까지는 thread_current()를 호출하면 안전하지 않습니다. */
void
thread_init (void) {
	ASSERT (intr_get_level () == INTR_OFF);

	/* 커널을 위한 임시 gdt를 다시 로드합니다.
	 * 이 gdt에는 사용자 컨텍스트가 포함되어 있지 않습니다.
	 * 커널은 gdt_init()에서 사용자 컨텍스트가 포함된 gdt를 다시 생성합니다. */
	struct desc_ptr gdt_ds = {
		.size = sizeof (gdt) - 1,
		.address = (uint64_t) gdt
	};
	lgdt (&gdt_ds); /* gdt를 로드합니다. */

	/* 글로벌 스레드 컨텍스트를 초기화합니다. */
	lock_init (&tid_lock);
	list_init (&ready_list);
	list_init (&sleep_list);
	list_init (&destruction_req);

	/* 현재 실행 중인 스레드에 대한 구조체를 설정합니다. */
	initial_thread = running_thread ();
	init_thread (initial_thread, "main", PRI_DEFAULT);
	initial_thread->status = THREAD_RUNNING;
	initial_thread->tid = allocate_tid ();
	initial_thread->waiting_lock = NULL;
}

/* 인터럽트를 활성화하여 선점형 스레드 스케줄링을 시작합니다.
	또한 idle 스레드를 생성합니다. */
void
thread_start (void) {
	/* idle 스레드를 생성합니다. */
	struct semaphore idle_started;
	sema_init (&idle_started, 0);
	thread_create ("idle", PRI_MIN, idle, &idle_started);

	/* 선점형 스레드 스케줄링을 시작합니다. */
	intr_enable ();

	/* idle 스레드가 idle_thread를 초기화할 때까지 기다립니다. */
	sema_down (&idle_started);
}

/* 타이머 인터럽트 핸들러가 매 타이머 틱마다 호출합니다.
	따라서 이 함수는 외부 인터럽트 컨텍스트에서 실행됩니다. */
void
thread_tick (void) {
	struct thread *t = thread_current ();

	/* Update statistics. */
	if (t == idle_thread)
		idle_ticks++;
#ifdef USERPROG
	else if (t->pml4 != NULL)
		user_ticks++;
#endif
	else
		kernel_ticks++;

	/* 선점(preemption)을 강제합니다. */
	if (++thread_ticks >= TIME_SLICE)
		intr_yield_on_return ();
}

/* 스레드 통계 정보를 출력합니다. */
void
thread_print_stats (void) {
	printf ("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",
			idle_ticks, kernel_ticks, user_ticks);
}

/* NAME이라는 이름과 주어진 초기 PRIORITY로 새로운 커널 스레드를 생성합니다.
	FUNCTION을 AUX 인자를 넘겨 실행하며, 준비 큐에 추가합니다.
	새 스레드의 식별자를 반환하거나, 생성에 실패하면 TID_ERROR를 반환합니다.

	thread_start()가 호출된 경우, 새 스레드는 thread_create()가 반환되기 전에
	스케줄될 수 있습니다. 심지어 thread_create()가 반환되기 전에 종료될 수도 있습니다.
	반대로, 원래의 스레드는 새 스레드가 스케줄되기 전까지 얼마든지 실행될 수 있습니다.
	순서를 보장하려면 세마포어나 다른 동기화 방법을 사용하세요.

	제공된 코드는 새 스레드의 `priority' 멤버를 PRIORITY로 설정하지만,
	실제 우선순위 스케줄링은 구현되어 있지 않습니다.
	우선순위 스케줄링은 문제 1-3의 목표입니다. */
tid_t
thread_create (const char *name, int priority,
		thread_func *function, void *aux) {
	struct thread *t;
	tid_t tid;

	ASSERT (function != NULL);

	/* Allocate thread. */
	t = palloc_get_page (PAL_ZERO);
	if (t == NULL)
		return TID_ERROR;

	/* Initialize thread. */
	init_thread (t, name, priority);
	tid = t->tid = allocate_tid ();

	/* 스케줄되면 kernel_thread를 호출합니다.
	 * 참고) rdi가 첫 번째 인자, rsi가 두 번째 인자입니다. */
	t->tf.rip = (uintptr_t) kernel_thread;
	t->tf.R.rdi = (uint64_t) function;
	t->tf.R.rsi = (uint64_t) aux;
	t->tf.ds = SEL_KDSEG;
	t->tf.es = SEL_KDSEG;
	t->tf.ss = SEL_KDSEG;
	t->tf.cs = SEL_KCSEG;
	t->tf.eflags = FLAG_IF;

	/* Add to run queue. */
	thread_unblock (t);

	/* 새 스레드의 우선순위가 현재 스레드보다 높으면 양보 */
    if (priority > thread_get_priority())
        thread_yield();

	return tid;
}

/* 현재 스레드를 sleep 상태로 만듭니다. thread_unblock()에 의해 깨워질 때까지
	다시 스케줄되지 않습니다.

	이 함수는 반드시 인터럽트가 꺼진 상태에서 호출해야 합니다.
	일반적으로 synch.h의 동기화 프리미티브를 사용하는 것이 더 좋습니다. */
void
thread_block (void) {
	ASSERT (!intr_context ());
	ASSERT (intr_get_level () == INTR_OFF);
	thread_current ()->status = THREAD_BLOCKED;
	schedule ();
}

/* BLOCKED 상태의 스레드 T를 실행 준비 상태로 전환합니다.
	T가 BLOCKED 상태가 아니면 에러입니다. (실행 중인 스레드를 준비 상태로 만들려면 thread_yield()를 사용하세요)

	이 함수는 실행 중인 스레드를 선점하지 않습니다. 호출자가 직접 인터럽트를 껐다면,
	스레드를 원자적으로 깨우고 다른 데이터를 갱신할 수 있기를 기대할 수 있으므로 중요할 수 있습니다. */
void
thread_unblock (struct thread *t) {
	enum intr_level old_level;

	ASSERT (is_thread (t));

	old_level = intr_disable ();
	ASSERT (t->status == THREAD_BLOCKED);
	list_insert_ordered (&ready_list, &t->elem, priority_cmp, NULL);
	t->status = THREAD_READY;
	intr_set_level (old_level);
}

/* 현재 실행 중인 스레드의 이름을 반환합니다. */
const char *
thread_name (void) {
	return thread_current ()->name;
}

/* 현재 실행 중인 스레드를 반환합니다.
	running_thread()에 몇 가지 검증을 추가한 것입니다.
	자세한 내용은 thread.h 상단의 큰 주석을 참고하세요. */
struct thread *
thread_current (void) {
	struct thread *t = running_thread ();

	 /* T가 실제로 스레드인지 확인합니다.
		 이 assert가 실패한다면, 스레드의 스택이 오버플로우 되었을 수 있습니다.
		 각 스레드는 4KB 미만의 스택을 가지므로, 큰 자동 배열이나 과도한 재귀는
		 스택 오버플로우를 일으킬 수 있습니다. */
	ASSERT (is_thread (t));
	ASSERT (t->status == THREAD_RUNNING);

	return t;
}

/* 현재 실행 중인 스레드의 tid를 반환합니다. */
tid_t
thread_tid (void) {
	return thread_current ()->tid;
}

/* 현재 스레드를 스케줄에서 제외하고 파괴합니다. 호출자에게 절대 반환하지 않습니다. */
void
thread_exit (void) {
	ASSERT (!intr_context ());

#ifdef USERPROG
	process_exit ();
#endif

	 /* 단순히 현재 스레드의 상태를 DYING으로 설정하고 다른 프로세스를 스케줄합니다.
		 우리는 schedule_tail() 호출 중에 파괴될 것입니다. */
	intr_disable ();
	do_schedule (THREAD_DYING);
	NOT_REACHED ();
}

/* CPU를 양보합니다. 현재 스레드는 sleep 상태가 되지 않으며,
	스케줄러의 결정에 따라 즉시 다시 스케줄될 수 있습니다. */
void
thread_yield (void) {
	struct thread *curr = thread_current ();
	enum intr_level old_level;

	ASSERT (!intr_context ());

	old_level = intr_disable ();
	if (curr != idle_thread)
		list_insert_ordered (&ready_list, &curr->elem, priority_cmp, NULL);
	do_schedule (THREAD_READY);
	intr_set_level (old_level);
}

/* 현재 스레드의 우선순위를 NEW_PRIORITY로 설정합니다. */
void
thread_set_priority (int new_priority) {
	struct thread *curr = thread_current();
	int old_priority = curr->priority;
	curr->original_priority = new_priority;

	/* 기부받은 우선순위 재계산 */
	refresh_priority(curr);

	/* 우선순위가 낮아졌거나, ready_list에 더 높은 우선순위 스레드가 있으면 양보 */
	if (curr->priority < old_priority || 
	    (!list_empty(&ready_list) && 
	     curr->priority < list_entry(list_front(&ready_list), struct thread, elem)->priority)) {
		thread_yield();
	}
}

/* 현재 스레드의 우선순위를 반환합니다. */
int
thread_get_priority (void) {
	return thread_current ()->priority;
}

/* 현재 스레드의 nice 값을 NICE로 설정합니다. */
void
thread_set_nice (int nice UNUSED) {
	/* TODO: Your implementation goes here */
}

/* 현재 스레드의 nice 값을 반환합니다. */
int
thread_get_nice (void) {
	/* TODO: Your implementation goes here */
	return 0;
}

/* 시스템 load average에 100을 곱한 값을 반환합니다. */
int
thread_get_load_avg (void) {
	/* TODO: Your implementation goes here */
	return 0;
}

/* 현재 스레드의 recent_cpu 값에 100을 곱한 값을 반환합니다. */
int
thread_get_recent_cpu (void) {
	/* TODO: Your implementation goes here */
	return 0;
}

/* Idle(유휴) 스레드. 실행할 다른 스레드가 없을 때 실행됩니다.

	idle 스레드는 thread_start()에 의해 처음에 ready 리스트에 추가됩니다.
	처음 한 번 스케줄되어 idle_thread를 초기화하고, 전달받은 세마포어를 "up"하여
	thread_start()가 계속 진행될 수 있게 한 뒤 즉시 block됩니다.
	그 이후로 idle 스레드는 ready 리스트에 나타나지 않습니다.
	ready 리스트가 비어 있을 때 next_thread_to_run()에서 특별히 반환됩니다. */
static void
idle (void *idle_started_ UNUSED) {
	struct semaphore *idle_started = idle_started_;

	idle_thread = thread_current ();
	sema_up (idle_started);

	for (;;) {
		/* Let someone else run. */
		intr_disable ();
		thread_block ();

		  /* 인터럽트를 다시 활성화하고 다음 인터럽트를 기다립니다.

			  `sti` 명령어는 다음 명령어가 완료될 때까지 인터럽트를 비활성화하므로,
			  이 두 명령어는 원자적으로 실행됩니다. 이 원자성은 중요합니다.
			  그렇지 않으면 인터럽트가 다시 활성화된 후 다음 인터럽트를 기다리는 사이에
			  인터럽트가 처리되어 한 클럭 틱만큼의 시간이 낭비될 수 있습니다.

			  자세한 내용은 [IA32-v2a] "HLT", [IA32-v2b] "STI",
			   [IA32-v3a] 7.11.1 "HLT Instruction"을 참고하세요. */
		asm volatile ("sti; hlt" : : : "memory");
	}
}

/* 커널 스레드의 기반이 되는 함수. */
static void
kernel_thread (thread_func *function, void *aux) {
	ASSERT (function != NULL);

	intr_enable ();       /* 스케줄러는 인터럽트가 꺼진 상태에서 동작합니다. */
	function (aux);       /* 스레드 함수를 실행합니다. */
	thread_exit ();       /* function()이 반환되면, 스레드를 종료합니다. */
}


/* T를 BLOCKED 상태의 NAME이라는 이름을 가진 스레드로 기본 초기화합니다. */
static void
init_thread (struct thread *t, const char *name, int priority) {
	ASSERT (t != NULL);
	ASSERT (PRI_MIN <= priority && priority <= PRI_MAX);
	ASSERT (name != NULL);

	memset (t, 0, sizeof *t);
	t->status = THREAD_BLOCKED;
	strlcpy (t->name, name, sizeof t->name);
	t->tf.rsp = (uint64_t) t + PGSIZE - sizeof (void *);
	t->priority = priority;
	t->original_priority = priority;  /* 원래 우선순위 초기화 */
	list_init(&t->holding_list);      /* 보유 락 리스트 초기화 */
	t->waiting_lock = NULL;           /* 대기 중인 락 초기화 */
	t->magic = THREAD_MAGIC;
	t->exit_status = 0;              /* 스레드 종료 상태 초기화 */
	t->wait_status = NULL;            /* 부모가 자식의 종료 상태를 기다리기 위해 사용하는 구조체 초기화 */
	list_init(&t->children);          /* 자식 프로세스 리스트 초기화 */

#ifdef USERPROG
	/* 파일 디스크립터 테이블 초기화 */
	t->fd_table = NULL;
	t->fd_table_size = 0;
	t->next_fd = 2;  /* 0: STDIN, 1: STDOUT, 2부터 일반 파일 */
	t->running_file = NULL;
#endif
}

/* 다음에 스케줄될 스레드를 선택하여 반환합니다.
	실행 큐가 비어 있지 않으면 그 중 하나를 반환해야 합니다.
	(실행 중인 스레드가 계속 실행될 수 있다면 실행 큐에 있습니다.)
	실행 큐가 비어 있으면 idle_thread를 반환합니다. */
static struct thread *
next_thread_to_run (void) {
	if (list_empty (&ready_list))
		return idle_thread;
	else
		return list_entry (list_pop_front (&ready_list), struct thread, elem);
}

/* iretq 명령어를 사용하여 스레드를 실행합니다. */
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

/* 새로운 스레드의 페이지 테이블을 활성화하여 스레드를 전환하고,
	이전 스레드가 죽는 중이라면 파괴합니다.

	이 함수가 호출될 때 이미 PREV 스레드에서 전환되었고,
	새 스레드가 실행 중이며 인터럽트는 아직 비활성화되어 있습니다.

	스레드 전환이 완료되기 전까지 printf()를 호출하면 안전하지 않습니다.
	실제로는 함수 끝부분에 printf()를 추가해야 합니다. */
static void
thread_launch (struct thread *th) {
	uint64_t tf_cur = (uint64_t) &running_thread ()->tf;
	uint64_t tf = (uint64_t) &th->tf;
	ASSERT (intr_get_level () == INTR_OFF);

	/* 주요 스위칭 로직입니다.
	 * 먼저 전체 실행 컨텍스트를 intr_frame에 복원한 후
	 * do_iret을 호출하여 다음 스레드로 전환합니다.
	 * 이 지점부터 스위칭이 완료될 때까지
	 * 스택을 사용하면 안 됩니다. */
	__asm __volatile (
			/* Store registers that will be used. */
			"push %%rax\n"
			"push %%rbx\n"
			"push %%rcx\n"
			/* Fetch input once */
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
			"pop %%rbx\n"              // Saved rcx
			"movq %%rbx, 96(%%rax)\n"
			"pop %%rbx\n"              // Saved rbx
			"movq %%rbx, 104(%%rax)\n"
			"pop %%rbx\n"              // Saved rax
			"movq %%rbx, 112(%%rax)\n"
			"addq $120, %%rax\n"
			"movw %%es, (%%rax)\n"
			"movw %%ds, 8(%%rax)\n"
			"addq $32, %%rax\n"
			"call __next\n"         // read the current rip.
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

/* 새로운 프로세스를 스케줄합니다. 진입 시 인터럽트가 꺼져 있어야 합니다.
 * 이 함수는 현재 스레드의 상태를 변경한 뒤, 실행할 다른 스레드를 찾아 전환합니다.
 * schedule() 내에서 printf()를 호출하면 안전하지 않습니다. */
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
	list_sort(&ready_list, priority_cmp, NULL);
	
	struct thread *curr = running_thread ();
	struct thread *next = next_thread_to_run ();

	ASSERT (intr_get_level () == INTR_OFF);
	ASSERT (curr->status != THREAD_RUNNING);
	ASSERT (is_thread (next));
	next->status = THREAD_RUNNING;
	thread_ticks = 0;
#ifdef USERPROG
	/* 새로운 주소 공간을 활성화합니다. */
	process_activate (next);
#endif
	if (curr != next) {
		 /* 이전 스레드가 종료 중이라면 파괴 요청 리스트에 추가합니다. */
		if (curr && curr->status == THREAD_DYING && curr != initial_thread) {
			ASSERT (curr != next);
			list_push_back (&destruction_req, &curr->elem);
		}
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

static bool list_cmp(const struct list_elem *a, const struct list_elem *b, void* aux UNUSED){
 /* sleep_list에서 tick 값을 비교하는 함수 */
 const struct thread* ta = list_entry(a, struct thread, elem);
 const struct thread* tb = list_entry(b, struct thread, elem);
 return ta->m_tick < tb->m_tick;
}

/* 스레드의 우선순위를 다시 계산 (기부 고려) */
void
refresh_priority(struct thread *t) {
	if (t == NULL)
		return;
	
	/* 기본은 원래 우선순위 */
	t->priority = t->original_priority;
	
	/* 보유한 락이 있으면, 대기 중인 스레드들의 최고 우선순위 확인 */
	if (!list_empty(&t->holding_list)) {
		int max_donated = t->original_priority;
		
		for (struct list_elem *e = list_begin(&t->holding_list);
		     e != list_end(&t->holding_list);
		     e = list_next(e)) {
			struct lock *lock = list_entry(e, struct lock, elem);
			
			if (!list_empty(&lock->semaphore.waiters)) {
				struct thread *waiter = list_entry(
					list_front(&lock->semaphore.waiters),
					struct thread, elem);
				if (waiter->priority > max_donated)
					max_donated = waiter->priority;
			}
		}
		t->priority = max_donated;
	}
}

bool priority_cmp(const struct list_elem *a, const struct list_elem *b, void* aux UNUSED){
 /* ready_list에서 우선순위 값을 비교하는 함수 */
 const struct thread* ta = list_entry(a, struct thread, elem);
 const struct thread* tb = list_entry(b, struct thread, elem);
 return ta->priority > tb->priority;
}

/* 현재 스레드를 sleep_list에 넣고 지정된 tick까지 sleep 상태로 만듭니다. */
void thread_sleep(int64_t ticks){
 enum intr_level old_level = intr_disable();
 ASSERT (!intr_context ());
 ASSERT (intr_get_level () == INTR_OFF);
 struct thread* curr = thread_current();
 curr->status = THREAD_BLOCKED;
 curr->m_tick = ticks;
 list_insert_ordered(&sleep_list, &curr->elem, list_cmp, NULL);
 schedule ();
 intr_set_level(old_level);
}

/* sleep_list에서 스레드를 깨워 ready 상태로 만듭니다. */
void thread_wakeUp(struct thread *t){
 list_remove(&t->elem);
 thread_unblock(t);
}
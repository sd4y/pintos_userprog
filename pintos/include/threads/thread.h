#ifndef THREADS_THREAD_H
#define THREADS_THREAD_H

#include <debug.h>
#include <list.h>
#include <stdint.h>
#include "threads/interrupt.h"
#include "threads/synch.h"
#ifdef VM
#include "vm/vm.h"
#endif


/* 스레드 생명주기의 상태. */
enum thread_status {
	THREAD_RUNNING,     /* 실행 중인 스레드. */
	THREAD_READY,       /* 실행 중이 아니지만 실행할 준비가 된 스레드. */
	THREAD_BLOCKED,     /* 이벤트가 발생하기를 기다리는 스레드. */
	THREAD_DYING        /* 곧 파괴될 스레드. */
};

/* 스레드 식별자 타입.
   원하는 타입으로 재정의할 수 있습니다. */
typedef int tid_t;
#define TID_ERROR ((tid_t) -1)          /* tid_t의 오류 값. */

/* 스레드 우선순위. */
#define PRI_MIN 0                       /* 최저 우선순위. */
#define PRI_DEFAULT 31                  /* 기본 우선순위. */
#define PRI_MAX 63                      /* 최고 우선순위. */

#define FD_MAX 128

/* 커널 스레드 또는 사용자 프로세스.
 *
 * 각 스레드 구조체는 자체 4KB 페이지에 저장됩니다.
 * 스레드 구조체 자체는 페이지의 맨 아래에 있습니다
 * (오프셋 0). 페이지의 나머지 부분은 스레드의 커널 스택을 위해
 * 예약되어 있으며, 페이지 상단(오프셋 4KB)에서 아래로 자랍니다.
 * 다음은 그림입니다:
 *
 *      4 kB +---------------------------------+
 *           |          커널 스택              |
 *           |                |                |
 *           |                |                |
 *           |                V                |
 *           |         아래로 자람             |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           +---------------------------------+
 *           |              magic              |
 *           |            intr_frame           |
 *           |                :                |
 *           |                :                |
 *           |               name              |
 *           |              status             |
 *      0 kB +---------------------------------+
 *
 * 이것의 결과는 두 가지입니다:
 *
 *    1. 첫째, `struct thread'가 너무 커지도록 허용되어서는 안 됩니다.
 *       그렇게 되면 커널 스택을 위한 공간이 충분하지 않습니다.
 *       기본 `struct thread'는 몇 바이트 크기입니다.
 *       아마도 1KB 미만으로 유지되어야 합니다.
 *
 *    2. 둘째, 커널 스택이 너무 커지도록 허용되어서는 안 됩니다.
 *       스택이 오버플로우되면 스레드 상태가 손상됩니다.
 *       따라서 커널 함수는 비정적 지역 변수로 큰 구조체나 배열을
 *       할당해서는 안 됩니다. 대신 malloc() 또는 palloc_get_page()를
 *       사용한 동적 할당을 사용하세요.
 *
 * 이러한 문제 중 하나의 첫 번째 증상은 아마도 thread_current()에서의
 * 어설션 실패일 것입니다. 이것은 실행 중인 스레드의 `struct thread'의
 * `magic' 멤버가 THREAD_MAGIC으로 설정되어 있는지 확인합니다.
 * 스택 오버플로우는 일반적으로 이 값을 변경하여 어설션을 트리거합니다. */
/* `elem' 멤버는 이중 목적을 가집니다. 실행 큐(thread.c)의 요소이거나
 * 세마포어 대기 목록(synch.c)의 요소일 수 있습니다. 이것이 이 두 가지
 * 방식으로 사용될 수 있는 이유는 상호 배타적이기 때문입니다:
 * 준비 상태의 스레드만 실행 큐에 있고, 차단된 상태의 스레드만
 * 세마포어 대기 목록에 있습니다. */
struct thread {
	/* thread.c에서 소유. */
	tid_t tid;                          /* 스레드 식별자. */
	enum thread_status status;          /* 스레드 상태. */
	int exit_status;                    /* 스레드 종료 상태. */
	char name[16];                      /* 이름 (디버깅 목적). */
	int64_t sleep_until; // 깨어날 틱

	int priority; // 이것은 계속 바뀔 수 있음
	int default_priority; // 원래 가졌던 우선순위

	struct lock *waiting_lock; // 내가 대기 중인 락

	struct list donation_list; // 기부를 받은 목록
	struct list_elem donation_elem; // 기부 목록 안의 요소

	struct list child_list; // 자식 스레드 목록
	struct child_info *self_ci;

	struct thread *parent;

	struct file *exec_file;
	struct file *fd_table[FD_MAX];	   // 쓰레드가 보유한 fd테이블

	/* thread.c와 synch.c 사이에서 공유. */
	struct list_elem elem;              /* 리스트 요소. */


#ifdef USERPROG
	/* userprog/process.c에서 소유. */
	uint64_t *pml4;                     /* 페이지 맵 레벨 4 */	
#endif
#ifdef VM
	/* 스레드가 소유한 전체 가상 메모리를 위한 테이블. */
	struct supplemental_page_table spt;
#endif

	/* thread.c에서 소유. */
	struct intr_frame tf;               /* 전환을 위한 정보 */
	unsigned magic;                     /* 스택 오버플로우를 감지합니다. */
};

struct child_info {
    tid_t tid;
    int exit_status;
    bool waited;
	bool exited;
    struct semaphore wait_sema;
    struct list_elem elem;
};

/* false(기본값)이면 라운드 로빈 스케줄러를 사용합니다.
   true이면 다단계 피드백 큐 스케줄러를 사용합니다.
   커널 명령줄 옵션 "-o mlfqs"로 제어됩니다. */
extern bool thread_mlfqs;

struct child_info * find_child_info(struct thread *parent, tid_t child_tid);

void thread_init (void);
void thread_start (void);

void thread_tick (void);
void thread_print_stats (void);

typedef void thread_func (void *aux);
tid_t thread_create (const char *name, int priority, thread_func *, void *);

void thread_block (void);
void thread_unblock (struct thread *);

void thread_sleep (int64_t ticks);
void thread_wake (int64_t ticks);

struct thread *thread_current (void);
tid_t thread_tid (void);
const char *thread_name (void);

void thread_exit (void) NO_RETURN;
void thread_yield (void);
void thread_preempt (void);

int thread_get_priority (void);
void thread_set_priority (int);

int thread_get_nice (void);
void thread_set_nice (int);
int thread_get_recent_cpu (void);
int thread_get_load_avg (void);

bool thread_sleep_compare (const struct list_elem *a, const struct list_elem *b, void *aux);
bool thread_priority_compare (const struct list_elem *a, const struct list_elem *b, void *aux);
void thread_update_priority (struct thread *t);
void thread_remove_donations (struct thread *t, struct lock *lock);
void thread_donate_priority (struct thread *t);

void do_iret (struct intr_frame *tf);

#endif /* threads/thread.h */

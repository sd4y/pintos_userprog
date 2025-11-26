#ifndef THREADS_THREAD_H
#define THREADS_THREAD_H

#include <debug.h>
#include <list.h>
#include <stdint.h>
#include "threads/interrupt.h"
#define USERPROG 1
#ifdef VM
#include "vm/vm.h"
#endif


/* 스레드의 생명주기 상태. */
enum thread_status {
	THREAD_RUNNING,     /* 실행 중인 스레드. */
	THREAD_READY,       /* 실행 중은 아니지만 실행 준비가 된 스레드. */
	THREAD_BLOCKED,     /* 이벤트 발생을 기다리는 중인 스레드. */
	THREAD_DYING        /* 곧 소멸될 스레드. */
};

/* 스레드 식별자 타입.
	원하는 타입으로 재정의할 수 있습니다. */
typedef int tid_t;
#define TID_ERROR ((tid_t) -1)          /* Error value for tid_t. */

/* 스레드 우선순위. */
#define PRI_MIN 0                       /* Lowest priority. */
#define PRI_DEFAULT 31                  /* Default priority. */
#define PRI_MAX 63                      /* Highest priority. */

/* 커널 스레드 또는 사용자 프로세스.
 *
 * 각 스레드 구조체는 4 kB 페이지에 저장됩니다. 스레드 구조체 자체는 페이지의 맨 아래(오프셋 0)에 위치합니다.
 * 나머지 페이지는 스레드의 커널 스택을 위해 예약되어 있으며, 커널 스택은 페이지의 맨 위(오프셋 4 kB)에서 아래로 성장합니다.
 * 아래는 그 구조를 나타낸 그림입니다:
 *
 *      4 kB +---------------------------------+
 *           |          커널 스택              |
 *           |                |                |
 *           |                |                |
 *           |                V                |
 *           |         아래로 성장함           |
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
 * 이 구조의 요점은 두 가지입니다:
 *
 *    1. 첫째, `struct thread'가 너무 커지면 안 됩니다. 너무 커지면 커널 스택을 위한 공간이 부족해집니다.
 *       기본 `struct thread'는 몇 바이트 정도의 크기이며, 1 kB를 넘지 않는 것이 좋습니다.
 *
 *    2. 둘째, 커널 스택이 너무 커지면 안 됩니다. 스택이 오버플로우되면 스레드 상태가 손상됩니다.
 *       따라서 커널 함수에서 큰 구조체나 배열을 비-static 지역 변수로 할당하지 말고,
 *       malloc()이나 palloc_get_page()를 사용하여 동적으로 할당해야 합니다.
 *
 * 이러한 문제의 첫 번째 증상은 thread_current()에서의 assertion 실패일 수 있습니다.
 * 이 함수는 실행 중인 스레드의 `struct thread`의 `magic` 멤버가 THREAD_MAGIC으로 설정되어 있는지 확인합니다.
 * 스택 오버플로우가 발생하면 이 값이 변경되어 assertion이 발생합니다. */
/* `elem` 멤버는 두 가지 용도로 사용됩니다. 실행 큐(thread.c)의 요소이거나, 세마포어 대기 리스트(synch.c)의 요소가 될 수 있습니다.
 * 이 두 가지 용도로 사용할 수 있는 이유는 상호 배타적이기 때문입니다:
 * 준비 상태(ready)의 스레드는 실행 큐에만 있고, 블록 상태(blocked)의 스레드는 세마포어 대기 리스트에만 있습니다. */
struct thread {
	/* Owned by thread.c. */
	tid_t tid;                          /* Thread identifier. */
	enum thread_status status;          /* Thread state. */
	char name[16];                      /* Name (for debugging purposes). */
	int priority;                       /* Priority. */

	/* thread.c와 synch.c에서 공유됨. */
	struct list_elem elem;              /* List element. */
	int64_t m_tick;					 /* 깨워야 할 시간(타이머 틱 단위). */
	int original_priority;         /* 기부받기 전의 원래 우선순위. */
	struct list holding_list;              /* 현재 스레드가 보유한 락 목록. */
	struct lock* waiting_lock;		  /* 현재 스레드가 기다리고 있는 락. */
	int exit_status;                   /* 스레드 종료 상태. */
	struct wait_status *wait_status; /* 부모가 자식의 종료 상태를 기다리기 위해 사용하는 구조체. */
	struct list children;             /* 자식 프로세스 리스트. */
#ifdef USERPROG
	/* Owned by userprog/process.c. */
	uint64_t *pml4;                     /* Page map level 4 */
	struct file **fd_table;            /* 파일 디스크립터 테이블 (동적 할당) */
	int fd_table_size;                 /* fd 테이블 크기 */
	int next_fd;                       /* 다음 할당할 fd 번호 */
	struct file *running_file;         /* 현재 실행 중인 파일 (deny write용) */
#endif
#ifdef VM
		/* 스레드가 소유한 전체 가상 메모리에 대한 테이블입니다. */
	struct supplemental_page_table spt;
#endif

	/* Owned by thread.c. */
	struct intr_frame tf;               /* 스레드 전환을 위한 정보 */
	unsigned magic;                     /* 스택 오버플로우 감지용 */
};

/* false(기본값)이면 라운드 로빈 스케줄러(기본 스케줄러)를 사용합니다.
	true면 다단계 피드백 큐 스케줄러를 사용합니다.
	커널 커맨드라인 옵션 "-o mlfqs"로 제어됩니다. */
extern bool thread_mlfqs;

void thread_init (void);
void thread_start (void);

void thread_tick (void);
void thread_print_stats (void);

typedef void thread_func (void *aux);
tid_t thread_create (const char *name, int priority, thread_func *, void *);

void thread_block (void);
void thread_unblock (struct thread *);

struct thread *thread_current (void);
tid_t thread_tid (void);
const char *thread_name (void);

void thread_exit (void) NO_RETURN;
void thread_yield (void);

int thread_get_priority (void);
void thread_set_priority (int);

int thread_get_nice (void);
void thread_set_nice (int);
int thread_get_recent_cpu (void);
int thread_get_load_avg (void);

void do_iret (struct intr_frame *tf);

void thread_wakeUp(struct thread *t);
void thread_sleep(int64_t ticks);
void refresh_priority(struct thread *t);

bool priority_cmp(const struct list_elem *a, const struct list_elem *b, void* aux UNUSED);
#endif /* threads/thread.h */

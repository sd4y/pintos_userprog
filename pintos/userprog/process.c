#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "userprog/gdt.h"
#include "userprog/tss.h"
#include "userprog/syscall.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/mmu.h"
#include "threads/vaddr.h"
#include "threads/synch.h"
#include "threads/malloc.h"
#include "intrinsic.h"
#ifdef VM
#include "vm/vm.h"
#endif

extern struct lock filesys_lock;  /* syscall.c에 선언된 전역 락 */

static void process_cleanup (void);
static bool load (const char *file_name, struct intr_frame *if_);
static void initd (void *f_name);
static void __do_fork (void *);
extern char* strtok_r(char* s, const char* delim, char** saveptr);

/* initd(또는 자식 시작 함수)에 전달할 정보 묶음 */
struct exec_info {
    char *file_name;
    struct wait_status *wstatus;
};

/* fork 시 부모 컨텍스트 전달용 */
struct fork_info {
    struct thread *parent;
    struct intr_frame parent_if;
    struct semaphore sema;
    bool success;
};


/* initd 및 기타 프로세스를 위한 일반 프로세스 초기화 함수. */
static void
process_init (void) {
	struct thread *current = thread_current ();
}

/* 첫 번째 유저랜드 프로그램인 "initd"를 FILE_NAME에서 로드하여 시작합니다.
 * 새 스레드는 process_create_initd()가 반환되기 전에 스케줄될 수 있으며(심지어 종료될 수도 있음)
 * initd의 스레드 id를 반환하거나, 스레드 생성에 실패하면 TID_ERROR를 반환합니다.
 * 이 함수는 반드시 한 번만 호출되어야 합니다. */
tid_t
process_create_initd (const char *file_name) {
	struct thread *parent = thread_current();
    struct exec_info *info;
    struct wait_status *wstatus;
	char *fn_copy;
	tid_t tid;

	/* FILE_NAME의 사본을 만듭니다.
	 * 그렇지 않으면 호출자와 load() 사이에 경쟁 상태가 발생할 수 있습니다. */
	fn_copy = palloc_get_page (0);
	if (fn_copy == NULL)
		return TID_ERROR;
	strlcpy (fn_copy, file_name, PGSIZE);

	wstatus = malloc (sizeof *wstatus);
    if (wstatus == NULL) {
        palloc_free_page (fn_copy);
        return TID_ERROR;
	}
	wstatus->tid = TID_ERROR;   /* 일단 초기값, thread_create 후에 채움 */
    wstatus->exit_status = 0;
    wstatus->exited = false;
    wstatus->waited = false;
    sema_init (&wstatus->sema, 0);

	 /* exec_info 할당 */
    info = malloc (sizeof *info);
    if (info == NULL) {
        free (wstatus);
        palloc_free_page (fn_copy);
        return TID_ERROR;
    }
    info->file_name = fn_copy;
    info->wstatus = wstatus;

	/* 프로그램 이름만 추출 (첫 번째 공백 전까지) */
	char thread_name[16];
	char *space = strchr(file_name, ' ');
	if (space != NULL) {
		size_t len = space - file_name;
		if (len > sizeof(thread_name) - 1)
			len = sizeof(thread_name) - 1;
		memcpy(thread_name, file_name, len);
		thread_name[len] = '\0';
	} else {
		strlcpy(thread_name, file_name, sizeof(thread_name));
	}

	/* FILE_NAME을 실행할 새 스레드를 생성합니다. */
	tid = thread_create (thread_name, PRI_DEFAULT, initd, info);
	if (tid == TID_ERROR){
		free (info);
		free (wstatus);
		palloc_free_page (fn_copy);
		return TID_ERROR;
	}
	 /* wait_status에 tid 채우고, 부모의 children 리스트에 등록 */
    wstatus->tid = tid;
    list_push_back (&parent->children, &wstatus->elem);
	return tid;
}

/* 첫 번째 사용자 프로세스를 실행하는 스레드 함수. */
static void
initd (void *aux) {
    struct exec_info *info = aux;
    struct thread *cur = thread_current();

#ifdef VM
    supplemental_page_table_init (&cur->spt);
#endif

    process_init ();

    /* 자식 입장에서 자신의 wait_status 포인터를 연결 */
    cur->wait_status = info->wstatus;

    /* file_name 문자열은 process_exec에서 사용 후 palloc_free_page로 해제됨 */
    if (process_exec (info->file_name) < 0)
        PANIC ("Fail to launch initd\n");

    /* exec_info 구조체는 이제 필요 없음 */
    free (info);

    NOT_REACHED ();
}


/* 현재 프로세스를 `name`으로 복제합니다. 새 프로세스의 스레드 id를 반환하며,
 * 스레드를 생성할 수 없으면 TID_ERROR를 반환합니다. */
tid_t
process_fork (const char *name, struct intr_frame *if_) {
	struct thread *parent = thread_current();
	struct fork_info *info;
	struct wait_status *wstatus;
	tid_t tid;

	/* fork_info 할당 */
	info = (struct fork_info *)malloc(sizeof(struct fork_info));
	if (info == NULL)
		return TID_ERROR;

	/* wait_status 생성 */
	wstatus = (struct wait_status *)malloc(sizeof(struct wait_status));
	if (wstatus == NULL) {
		free(info);
		return TID_ERROR;
	}

	/* info 초기화 */
	info->parent = parent;
	memcpy(&info->parent_if, if_, sizeof(struct intr_frame));
	sema_init(&info->sema, 0);
	info->success = false;

	/* wait_status 초기화 */
	wstatus->tid = TID_ERROR;
	wstatus->exit_status = 0;
	wstatus->exited = false;
	wstatus->waited = false;
	sema_init(&wstatus->sema, 0);

	/* 자식 스레드 생성 */
	tid = thread_create(name, PRI_DEFAULT, __do_fork, info);
	if (tid == TID_ERROR) {
		free(wstatus);
		free(info);
		return TID_ERROR;
	}

	/* wait_status에 tid 설정 및 부모의 children 리스트에 추가 */
	wstatus->tid = tid;
	list_push_back(&parent->children, &wstatus->elem);

	/* 자식이 fork 완료할 때까지 대기 */
	sema_down(&info->sema);
	bool success = info->success;
	free(info);

	/* fork 실패 시 wait_status 정리 */
	if (!success) {
		list_remove(&wstatus->elem);
		free(wstatus);
		return TID_ERROR;
	}

	return tid;
}

#ifndef VM
/* 부모의 주소 공간을 복제하기 위해 이 함수를 pml4_for_each에 전달합니다.
 * 이 코드는 프로젝트 2에서만 사용됩니다. */
static bool
duplicate_pte (uint64_t *pte, void *va, void *aux) {
	struct thread *current = thread_current ();
	struct thread *parent = (struct thread *) aux;
	void *parent_page;
	void *newpage;
	bool writable;

	/* 1. 커널 페이지는 건너뛰기 */
	if (!is_user_vaddr(va))
		return true;

	/* 2. 부모의 PML4에서 VA에 해당하는 물리 페이지를 조회합니다. */
	parent_page = pml4_get_page (parent->pml4, va);
	if (parent_page == NULL)
		return true;  /* 매핑되지 않은 페이지는 건너뛰기 */

	/* 3. 자식용 PAL_USER 페이지를 새로 할당하고 결과를 NEWPAGE에 저장합니다. */
	newpage = palloc_get_page(PAL_USER);
	if (newpage == NULL)
		return false;

	/* 4. 부모의 페이지 내용을 새 페이지로 복사하고,
	 *    부모 페이지의 쓰기 가능 여부를 확인하여 WRITABLE 값을 설정합니다. */
	memcpy(newpage, parent_page, PGSIZE);
	writable = is_writable(pte);

	/* 5. 자식의 페이지 테이블에 VA 주소로 NEWPAGE를 WRITABLE 권한과 함께 매핑합니다. */
	if (!pml4_set_page (current->pml4, va, newpage, writable)) {
		/* 6. 페이지 삽입에 실패한 경우 오류 처리를 수행합니다. */
		palloc_free_page(newpage);
		return false;
	}
	return true;
}
#endif

/* 부모의 실행 컨텍스트를 복사하는 스레드 함수입니다.
 * 힌트) parent->tf에는 프로세스의 유저랜드 컨텍스트가 저장되어 있지 않습니다.
 *       즉, process_fork의 두 번째 인자를 이 함수로 전달해야 합니다. */
static void
__do_fork (void *aux) {
	struct intr_frame if_;
	struct fork_info *info = (struct fork_info *)aux;
	struct thread *parent = info->parent;
	struct thread *current = thread_current ();
	bool succ = true;

	/* 1. CPU 컨텍스트를 로컬 스택(if_)으로 복사합니다. */
	memcpy (&if_, &info->parent_if, sizeof (struct intr_frame));
	if_.R.rax = 0;  /* 자식의 fork 반환값은 0 */

	/* 2. 페이지 테이블 복제 */
	current->pml4 = pml4_create();
	if (current->pml4 == NULL)
		goto error;

	process_activate (current);
#ifdef VM
	supplemental_page_table_init (&current->spt);
	if (!supplemental_page_table_copy (&current->spt, &parent->spt))
		goto error;
#else
	if (!pml4_for_each (parent->pml4, duplicate_pte, parent))
		goto error;
#endif

	/* 파일 디스크립터 테이블 복제 */
	if (succ && parent->fd_table != NULL) {
		fd_table_init(current);
		for (int i = 2; i < parent->fd_table_size; i++) {
			if (parent->fd_table[i] != NULL) {
				struct file *dup = file_duplicate(parent->fd_table[i]);
				if (dup == NULL) {
					succ = false;
					break;
				}
				current->fd_table[i] = dup;
			}
		}
	}

	/* wait_status 설정 */
	if (succ) {
		/* 부모의 children 리스트에서 자신의 wait_status 찾기 */
		struct list_elem *e;
		for (e = list_begin(&parent->children);
			 e != list_end(&parent->children);
			 e = list_next(e)) {
			struct wait_status *ws = list_entry(e, struct wait_status, elem);
			if (ws->tid == current->tid) {
				current->wait_status = ws;
				break;
			}
		}
	}

	process_init ();

	/* 부모에게 fork 결과 알림 */
	info->success = succ;
	sema_up(&info->sema);

	/* 마지막으로, 새로 생성된 프로세스로 전환합니다. */
	if (succ)
		do_iret (&if_);
error:
	/* fork 실패 시에도 부모에게 알림 */
	info->success = false;
	sema_up(&info->sema);
	thread_exit ();
}

void parse_args(char* cmdline, char** argv, int* argc){
	char* save_ptr;
	*argc = 0;
	for(char* token = strtok_r(cmdline, " ", &save_ptr);
		token != NULL;
		token = strtok_r(NULL, " ", &save_ptr)){
			argv[(*argc)++] = token;
			if (*argc >= 64) break;
	}
}

void setup_stack_args(const char** argv, const int argc, struct intr_frame *_if){
	uint64_t rsp = USER_STACK;
    uint64_t arg_addrs[64];

    /* 1. 문자열들을 역순으로 스택에 복사 */
    for (int i = argc - 1; i >= 0; i--) {
        size_t len = strlen(argv[i]) + 1;  // '\0' 포함
        rsp -= len;
        memcpy((void *) rsp, argv[i], len);
        arg_addrs[i] = rsp;
    }

    /* 2. 8바이트 정렬 맞추기 */
    rsp &= ~((uint64_t)0x7);  // 하위 3비트 0으로

    /* 3. argv[argc] = NULL sentinel */
    rsp -= sizeof(uint64_t);
    *(uint64_t *) rsp = 0;

    /* 4. argv[i] 포인터들 (역순으로 push) */
    for (int i = argc - 1; i >= 0; i--) {
        rsp -= sizeof(uint64_t);
        *(uint64_t *) rsp = arg_addrs[i];
    }
    uint64_t argv_start = rsp;  // &argv[0]

    /* 5. fake return address */
    rsp -= sizeof(uint64_t);
    *(uint64_t *) rsp = 0;

    /* 6. intr_frame 레지스터 세팅 */
    _if->rsp = rsp;
    _if->R.rdi = argc;       // 첫 번째 인자
    _if->R.rsi = argv_start; // 두 번째 인자
}

/* 현재 실행 컨텍스트를 f_name으로 전환합니다.
 * 실패 시 -1을 반환합니다. */
int
process_exec (void *f_name) {
	char *file_name = f_name;
	bool success;

	/* 스레드 구조체 내의 intr_frame을 사용할 수 없습니다.
	 * 현재 스레드가 다시 스케줄될 때 해당 멤버에 실행 정보가 저장되기 때문입니다. */
	struct intr_frame _if;
	_if.ds = _if.es = _if.ss = SEL_UDSEG;
	_if.cs = SEL_UCSEG;
	_if.eflags = FLAG_IF | FLAG_MBS;

	char* argv[64];
	int argc;
	parse_args(file_name, argv, &argc);

	if (argc == 0){
		palloc_free_page (file_name);
		return -1;
	}

	/* 먼저 현재 컨텍스트를 정리합니다. */
	process_cleanup ();

	/* 그리고 실행 파일을 로드합니다. */
	success = load (argv[0], &_if);

	/* 로드에 실패하면 종료합니다. */
	if (!success){
		palloc_free_page (file_name);
		return -1;
	}

	/* 스택에 인자들을 설정합니다. */
	setup_stack_args((const char**)argv, argc, &_if);

	palloc_free_page (file_name);

	/* 전환된 프로세스를 시작합니다. */
	do_iret (&_if);
	NOT_REACHED ();
}


/* 스레드 TID가 종료될 때까지 기다린 후 그 종료 상태를 반환합니다.
 * 커널에 의해 종료되었다면(예: 예외로 인해 종료) -1을 반환합니다.
 * TID가 유효하지 않거나 호출자의 자식이 아니거나, 해당 TID에 대해 이미
 * process_wait()가 성공적으로 호출된 경우 즉시 -1을 반환합니다(대기하지 않음).
 *
 * 이 함수는 과제 2-2에서 구현됩니다. 현재는 동작하지 않습니다. */
/* 현재 프로세스의 자식 중에서 child_tid에 해당하는 wait_status 찾기 */
static struct wait_status *
find_child_wait_status (struct thread *cur, tid_t child_tid) {
    struct list_elem *e;
    for (e = list_begin (&cur->children);
         e != list_end (&cur->children);
         e = list_next (e)) {
        struct wait_status *ws = list_entry (e, struct wait_status, elem);
        if (ws->tid == child_tid)
            return ws;
    }
    return NULL;
}

int
process_wait (tid_t child_tid) {
    struct thread *cur = thread_current ();
    struct wait_status *ws = find_child_wait_status (cur, child_tid);

    /* 1. 내 자식이 아니거나, 이미 정리된 경우 */
    if (ws == NULL)
        return -1;

    /* 2. 이미 wait 한 적 있으면 -1 */
    if (ws->waited)
        return -1;

    /* 3. 자식이 아직 안 죽었다면, 종료를 기다린다. */
    if (!ws->exited)
        sema_down (&ws->sema);

    /* 4. waited 플래그는 sema_down 후에 설정 (자식이 sema_up을 호출한 후) */
    ws->waited = true;

    /* 5. 자식의 종료 코드를 받아온다. */
    int status = ws->exit_status;

    /* 6. 부모의 children 리스트에서 제거하고, wait_status 해제 */
    list_remove (&ws->elem);
    free (ws);

    return status;
}


/* 프로세스를 종료합니다. 이 함수는 thread_exit()에 의해 호출됩니다. */
void
process_exit (void) {
    struct thread *cur = thread_current ();

    /* Project2: 종료 코드와 wait_status 업데이트 */
    if (cur->wait_status != NULL) {
        /* 프로세스 종료 메시지 출력 (유저 프로세스만) */
        printf("%s: exit(%d)\n", cur->name, cur->exit_status);
        
        cur->wait_status->exit_status = cur->exit_status;
        cur->wait_status->exited = true;
        
        /* 부모가 이미 wait 했거나 죽었으면 wait_status 해제 */
        if (cur->wait_status->waited) {
            free(cur->wait_status);
            cur->wait_status = NULL;
        } else {
            /* 부모가 기다리고 있을 수 있으니 깨워줍니다. */
            sema_up (&cur->wait_status->sema);
        }
    }

    /* 모든 열린 파일 디스크립터 닫기 */
    close_all_fds();
    
    /* 실행 중인 파일이 있으면 쓰기 허용 및 닫기 */
    if (cur->running_file != NULL) {
        file_allow_write(cur->running_file);
        file_close(cur->running_file);
        cur->running_file = NULL;
    }

    /* 자식 프로세스 리스트 정리 */
    while (!list_empty(&cur->children)) {
        struct list_elem *e = list_pop_front(&cur->children);
        struct wait_status *ws = list_entry(e, struct wait_status, elem);
        
        /* 자식이 이미 종료했으면 wait_status 해제 */
        if (ws->exited) {
            free(ws);
        }
        /* 아직 살아있는 자식은 고아가 되므로, 자식이 종료될 때 스스로 정리하도록 표시 */
        else {
            ws->waited = true;  /* 부모가 wait 안 하고 죽었다고 표시 */
        }
    }

    process_cleanup ();
}


/* 현재 프로세스의 자원을 해제합니다. */
static void
process_cleanup (void) {
	struct thread *curr = thread_current ();

#ifdef VM
	supplemental_page_table_kill (&curr->spt);
#endif

	uint64_t *pml4;
	/* 현재 프로세스의 페이지 디렉터리를 파괴하고 커널 전용 페이지 디렉터리로
	 * 전환합니다. */
	pml4 = curr->pml4;
	if (pml4 != NULL) {
		/* 여기서의 순서가 매우 중요합니다. 페이지 디렉터리를 전환하기 전에
		 * cur->pagedir를 NULL로 설정해야 타이머 인터럽트가 프로세스 페이지
		 * 디렉터리로 다시 전환하지 않습니다. 또한 프로세스의 페이지 디렉터리를
		 * 파괴하기 전에 기본 페이지 디렉터리를 활성화해야, 이미 해제(초기화)된
		 * 페이지 디렉터리가 활성 상태가 되는 일을 피할 수 있습니다. */
		curr->pml4 = NULL;
		pml4_activate (NULL);
		pml4_destroy (pml4);
	}
}

/* 다음(새로운) 스레드에서 사용자 코드를 실행할 수 있도록 CPU를 준비합니다.
 * 이 함수는 매번 컨텍스트 스위치 시 호출됩니다. */
void
process_activate (struct thread *next) {
	/* 스레드의 페이지 테이블을 활성화합니다. */
	pml4_activate (next->pml4);

	/* 인터럽트 처리에 사용할 스레드의 커널 스택을 설정합니다. */
	tss_update (next);
}

/* ELF 바이너리를 로드합니다. 아래 정의들은 ELF 명세서 [ELF1]에서 거의 그대로
 * 가져온 것입니다. */

/* ELF 타입들. [ELF1] 1-2 참고. */
#define EI_NIDENT 16

#define PT_NULL    0            /* 무시. */
#define PT_LOAD    1            /* 로드 가능한 세그먼트. */
#define PT_DYNAMIC 2            /* 동적 링킹 정보. */
#define PT_INTERP  3            /* 동적 로더 이름. */
#define PT_NOTE    4            /* 보조 정보. */
#define PT_SHLIB   5            /* 예약됨. */
#define PT_PHDR    6            /* 프로그램 헤더 테이블. */
#define PT_STACK   0x6474e551   /* 스택 세그먼트. */

#define PF_X 1          /* 실행 가능. */
#define PF_W 2          /* 쓰기 가능. */
#define PF_R 4          /* 읽기 가능. */

/* 실행 파일 헤더. [ELF1] 1-4부터 1-8 참고.
 * ELF 바이너리의 가장 앞부분에 위치합니다. */
struct ELF64_hdr {
	unsigned char e_ident[EI_NIDENT];
	uint16_t e_type;
	uint16_t e_machine;
	uint32_t e_version;
	uint64_t e_entry;
	uint64_t e_phoff;
	uint64_t e_shoff;
	uint32_t e_flags;
	uint16_t e_ehsize;
	uint16_t e_phentsize;
	uint16_t e_phnum;
	uint16_t e_shentsize;
	uint16_t e_shnum;
	uint16_t e_shstrndx;
};

struct ELF64_PHDR {
	uint32_t p_type;
	uint32_t p_flags;
	uint64_t p_offset;
	uint64_t p_vaddr;
	uint64_t p_paddr;
	uint64_t p_filesz;
	uint64_t p_memsz;
	uint64_t p_align;
};

/* 약어 정의 */
#define ELF ELF64_hdr
#define Phdr ELF64_PHDR

static bool setup_stack (struct intr_frame *if_);
static bool validate_segment (const struct Phdr *, struct file *);
static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes,
		bool writable);

/* FILE_NAME에서 ELF 실행 파일을 현재 스레드로 로드합니다.
 * 실행 파일의 진입 지점을 *RIP에, 초기 스택 포인터를 *RSP에 저장합니다.
 * 성공 시 true, 실패 시 false를 반환합니다. */
static bool
load (const char *file_name, struct intr_frame *if_) {
	struct thread *t = thread_current ();
	struct ELF ehdr;
	struct file *file = NULL;
	off_t file_ofs;
	bool success = false;
	int i;

	/* 페이지 디렉터리를 생성하고 활성화합니다. */
	t->pml4 = pml4_create ();
	if (t->pml4 == NULL)
		goto done;
	process_activate (thread_current ());

	/* 파일 시스템 접근 시작 - 락 획득 */
	lock_acquire(&filesys_lock);

	/* 실행 파일을 엽니다. */
	file = filesys_open (file_name);
	if (file == NULL) {
		printf ("load: %s: open failed\n", file_name);
		lock_release(&filesys_lock);
		return false;
	}
	
	/* 실행 파일 쓰기 금지 (deny write to executable) */
	file_deny_write(file);
	t->running_file = file;

	/* 실행 파일 헤더를 읽고 검증합니다. */
	if (file_read (file, &ehdr, sizeof ehdr) != sizeof ehdr
			|| memcmp (ehdr.e_ident, "\177ELF\2\1\1", 7)
			|| ehdr.e_type != 2
			|| ehdr.e_machine != 0x3E // amd64
			|| ehdr.e_version != 1
			|| ehdr.e_phentsize != sizeof (struct Phdr)
			|| ehdr.e_phnum > 1024) {
		printf ("load: %s: error loading executable\n", file_name);
		goto done;
	}

	/* Read program headers. */
	file_ofs = ehdr.e_phoff;
	for (i = 0; i < ehdr.e_phnum; i++) {
		struct Phdr phdr;

		if (file_ofs < 0 || file_ofs > file_length (file))
			goto done;
		file_seek (file, file_ofs);

		if (file_read (file, &phdr, sizeof phdr) != sizeof phdr)
			goto done;
		file_ofs += sizeof phdr;
		switch (phdr.p_type) {
			case PT_NULL:
			case PT_NOTE:
			case PT_PHDR:
			case PT_STACK:
			default:
				/* 이 세그먼트는 무시합니다. */
				break;
			case PT_DYNAMIC:
			case PT_INTERP:
			case PT_SHLIB:
				goto done;
			case PT_LOAD:
				if (validate_segment (&phdr, file)) {
					bool writable = (phdr.p_flags & PF_W) != 0;
					uint64_t file_page = phdr.p_offset & ~PGMASK;
					uint64_t mem_page = phdr.p_vaddr & ~PGMASK;
					uint64_t page_offset = phdr.p_vaddr & PGMASK;
					uint32_t read_bytes, zero_bytes;
					if (phdr.p_filesz > 0) {
						/* 일반적인 세그먼트.
						 * 디스크에서 앞부분을 읽고 나머지는 0으로 채웁니다. */
						read_bytes = page_offset + phdr.p_filesz;
						zero_bytes = (ROUND_UP (page_offset + phdr.p_memsz, PGSIZE)
								- read_bytes);
					} else {
						/* 전체가 0으로 채워지는 세그먼트.
						 * 디스크에서 아무 것도 읽지 않습니다. */
						read_bytes = 0;
						zero_bytes = ROUND_UP (page_offset + phdr.p_memsz, PGSIZE);
					}
					if (!load_segment (file, file_page, (void *) mem_page,
								read_bytes, zero_bytes, writable))
						goto done;
				}
				else
					goto done;
				break;
		}
	}

	/* 스택을 설정합니다. */
	if (!setup_stack (if_))
		goto done;

	/* 시작 주소. */
	if_->rip = ehdr.e_entry;

	success = true;

done:
	/* 파일 시스템 락 해제 (락을 잡고 있는 경우에만) */
	if (file != NULL)
		lock_release(&filesys_lock);
	
	/* 로드의 성공 여부와 관계없이 이 지점에 도달합니다. */
	/* 성공 시에는 running_file이 설정되어 있으므로 여기서 닫지 않음 */
	/* 실패 시에만 파일을 닫음 */
	if (!success && file != NULL) {
		file_allow_write(file);
		file_close(file);
		t->running_file = NULL;
	}
	return success;
}


/* PHDR가 FILE 내의 유효하고 로드 가능한 세그먼트를 기술하는지 검사합니다.
 * 맞으면 true, 아니면 false를 반환합니다. */
static bool
validate_segment (const struct Phdr *phdr, struct file *file) {
	/* p_offset과 p_vaddr는 동일한 페이지 오프셋을 가져야 합니다. */
	if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK))
		return false;

	/* p_offset은 FILE 범위 내를 가리켜야 합니다. */
	if (phdr->p_offset > (uint64_t) file_length (file))
		return false;

	/* p_memsz는 p_filesz보다 크거나 같아야 합니다. */
	if (phdr->p_memsz < phdr->p_filesz)
		return false;

	/* 세그먼트가 비어 있으면 안 됩니다. */
	if (phdr->p_memsz == 0)
		return false;

	/* 가상 메모리 영역의 시작과 끝은 모두 사용자 주소 공간 범위 내에 있어야 합니다. */
	if (!is_user_vaddr ((void *) phdr->p_vaddr))
		return false;
	if (!is_user_vaddr ((void *) (phdr->p_vaddr + phdr->p_memsz)))
		return false;

	/* 이 영역은 커널 가상 주소 공간을 가로질러 "랩 어라운드"되어서는 안 됩니다. */
	if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
		return false;

	 /* 페이지 0 매핑은 허용하지 않습니다.
		 페이지 0을 매핑하는 것은 나쁜 아이디어일 뿐 아니라,
		 이를 허용하면 사용자 코드가 시스템 콜에 널 포인터를 전달했을 때
		 memcpy() 등의 널 포인터 단언 실패로 커널 패닉이 발생할 수 있습니다. */
	if (phdr->p_vaddr < PGSIZE)
		return false;

	/* 유효합니다. */
	return true;
}

#ifndef VM
/* 이 블록의 코드는 프로젝트 2에서만 사용됩니다.
 * 프로젝트 2 전체에 대해 함수를 구현하려면 #ifndef 매크로 바깥에서 구현하세요. */

/* load() 보조 함수들. */
static bool install_page (void *upage, void *kpage, bool writable);

/* FILE의 OFS 오프셋에서 시작하는 세그먼트를 주소 UPAGE에 로드합니다.
 * 총 READ_BYTES + ZERO_BYTES 바이트의 가상 메모리가 다음과 같이 초기화됩니다:
 *
 * - OFS부터 FILE에서 READ_BYTES 바이트를 읽어 UPAGE에 씁니다.
 *
 * - UPAGE + READ_BYTES부터 ZERO_BYTES 바이트를 0으로 채웁니다.
 *
 * WRITABLE이 true이면 이 함수가 초기화한 페이지는 사용자 프로세스에서 쓰기 가능해야 하며,
 * 그렇지 않으면 읽기 전용이어야 합니다.
 *
 * 성공 시 true를 반환하고, 메모리 할당 오류나 디스크 읽기 오류 발생 시 false를 반환합니다. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes, bool writable) {
	ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
	ASSERT (pg_ofs (upage) == 0);
	ASSERT (ofs % PGSIZE == 0);

	file_seek (file, ofs);
	while (read_bytes > 0 || zero_bytes > 0) {
		/* 이 페이지를 어떻게 채울지 계산합니다.
		 * FILE에서 PAGE_READ_BYTES 바이트를 읽고 나머지 PAGE_ZERO_BYTES 바이트는 0으로 채웁니다. */
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

		/* 메모리 페이지를 할당합니다. */
		uint8_t *kpage = palloc_get_page (PAL_USER);
		if (kpage == NULL)
			return false;

		/* 이 페이지에 데이터를 로드합니다. */
		if (file_read (file, kpage, page_read_bytes) != (int) page_read_bytes) {
			palloc_free_page (kpage);
			return false;
		}
		memset (kpage + page_read_bytes, 0, page_zero_bytes);

		/* 페이지를 프로세스의 주소 공간에 매핑합니다. */
		if (!install_page (upage, kpage, writable)) {
			printf("fail\n");
			palloc_free_page (kpage);
			return false;
		}

		/* 진행 위치를 갱신합니다. */
		read_bytes -= page_read_bytes;
		zero_bytes -= page_zero_bytes;
		upage += PGSIZE;
	}
	return true;
}

/* USER_STACK에 0으로 초기화된 페이지를 매핑하여 최소한의 스택을 생성합니다. */
static bool
setup_stack (struct intr_frame *if_) {
	uint8_t *kpage;
	bool success = false;

	kpage = palloc_get_page (PAL_USER | PAL_ZERO);
	if (kpage != NULL) {
		success = install_page (((uint8_t *) USER_STACK) - PGSIZE, kpage, true);
		if (success)
			if_->rsp = USER_STACK;
		else
			palloc_free_page (kpage);
	}
	return success;
}

/* Adds a mapping from user virtual address UPAGE to kernel
 * virtual address KPAGE to the page table.
 * If WRITABLE is true, the user process may modify the page;
 * otherwise, it is read-only.
 * UPAGE must not already be mapped.
 * KPAGE should probably be a page obtained from the user pool
 * with palloc_get_page().
 * Returns true on success, false if UPAGE is already mapped or
 * if memory allocation fails. */
static bool
install_page (void *upage, void *kpage, bool writable) {
	struct thread *t = thread_current ();

	/* 해당 가상 주소에 이미 페이지가 없는지 확인한 뒤, 페이지를 매핑합니다. */
	return (pml4_get_page (t->pml4, upage) == NULL
			&& pml4_set_page (t->pml4, upage, kpage, writable));
}
#else
/* 여기부터의 코드는 프로젝트 3 이후에 사용됩니다.
 * 만약 프로젝트 2에서만 함수를 구현하려면 위의 블록에 구현하세요. */

static bool
lazy_load_segment (struct page *page, void *aux) {
	/* TODO: 파일에서 세그먼트를 로드합니다. */
	/* TODO: 이 함수는 주소 VA에서 첫 페이지 폴트가 발생했을 때 호출됩니다. */
	/* TODO: 이 함수를 호출할 때 VA는 유효합니다. */
}

/* FILE의 OFS 오프셋에서 시작하는 세그먼트를 주소 UPAGE에 로드합니다.
 * 총 READ_BYTES + ZERO_BYTES 바이트의 가상 메모리가 다음과 같이 초기화됩니다:
 *
 * - OFS부터 FILE에서 READ_BYTES 바이트를 읽어 UPAGE에 씁니다.
 *
 * - UPAGE + READ_BYTES부터 ZERO_BYTES 바이트를 0으로 채웁니다.
 *
 * WRITABLE이 true이면 이 함수가 초기화한 페이지는 사용자 프로세스에서 쓰기 가능해야 하며,
 * 그렇지 않으면 읽기 전용이어야 합니다.
 *
 * 성공 시 true를 반환하고, 메모리 할당 오류나 디스크 읽기 오류 발생 시 false를 반환합니다. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes, bool writable) {
	ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
	ASSERT (pg_ofs (upage) == 0);
	ASSERT (ofs % PGSIZE == 0);

	while (read_bytes > 0 || zero_bytes > 0) {
		/* 이 페이지를 어떻게 채울지 계산합니다.
		 * FILE에서 PAGE_READ_BYTES 바이트를 읽고 나머지 PAGE_ZERO_BYTES 바이트는 0으로 채웁니다. */
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

		/* TODO: lazy_load_segment로 정보를 전달하기 위한 aux를 설정합니다. */
		void *aux = NULL;
		if (!vm_alloc_page_with_initializer (VM_ANON, upage,
					writable, lazy_load_segment, aux))
			return false;

		/* Advance. */
		read_bytes -= page_read_bytes;
		zero_bytes -= page_zero_bytes;
		upage += PGSIZE;
	}
	return true;
}

/* USER_STACK에 스택 페이지를 생성합니다. 성공 시 true를 반환합니다. */
static bool
setup_stack (struct intr_frame *if_) {
	bool success = false;
	void *stack_bottom = (void *) (((uint8_t *) USER_STACK) - PGSIZE);

	/* TODO: stack_bottom에 스택을 매핑하고 즉시 페이지를 할당(클레임)합니다.
	 * TODO: 성공 시 rsp를 적절히 설정합니다.
	 * TODO: 해당 페이지를 스택으로 표시해야 합니다. */
	/* TODO: 여기에 코드를 작성하세요 */

	return success;
}
#endif /* VM */

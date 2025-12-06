#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <list.h>
#include <stdlib.h>
#include "userprog/gdt.h"
#include "userprog/tss.h"
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
#include "intrinsic.h"
#include "devices/timer.h"
#include "filesys/file.h"
#include "vm/vm.h"
#define VM 1
#ifdef VM
#include "vm/vm.h"
#endif

static void process_cleanup (void);
static bool load (const char *file_name, struct intr_frame *if_);
static void initd (void *f_name);
static void __do_fork (void *);

/* General process initializer for initd and other process. */
static void
process_init (void) {
	struct thread *current = thread_current ();

    if (current->fdt_table == NULL) {
        current->fdt_table = palloc_get_page(PAL_ZERO);
        if (current->fdt_table == NULL) return;
    }
}

/* Starts the first userland program, called "initd", loaded from FILE_NAME.
 * The new thread may be scheduled (and may even exit)
 * before process_create_initd() returns. Returns the initd's
 * thread id, or TID_ERROR if the thread cannot be created.
 * Notice that THIS SHOULD BE CALLED ONCE. */
tid_t
process_create_initd (const char *file_name) {
	char *fn_copy;
	tid_t tid;

	lock_init(&filesys_lock);

	/* Make a copy of FILE_NAME.
	 * Otherwise there's a race between the caller and load(). */
	fn_copy = palloc_get_page (0);
	if (fn_copy == NULL)
		return TID_ERROR;
	strlcpy (fn_copy, file_name, PGSIZE);

	char thread_name[64];
	strlcpy(thread_name, file_name, sizeof(thread_name));
	char* save_ptr;
	strtok_r(thread_name, " ", &save_ptr);

	/* Create a new thread to execute FILE_NAME. */
	tid = thread_create (thread_name, PRI_DEFAULT, initd, fn_copy);
	if (tid == TID_ERROR)
		palloc_free_page (fn_copy);
	return tid;
}
 
/* A thread function that launches first user process. */
static void
initd (void *f_name) {
#ifdef VM
	supplemental_page_table_init (&thread_current ()->spt);
#endif

	process_init ();

	if (process_exec (f_name) < 0)
		PANIC("Fail to launch initd\n");
	NOT_REACHED ();
}

// tid를 받아 해당하는 자식 스레드 구조체를 반환하는 함수
struct thread *get_child_thread(tid_t tid){
	struct thread *cur = thread_current();
	struct list_elem *e;

	// 엄마의 자식 리스트를 순회
	for (e = list_begin(&cur->child_list); e != list_end(&cur->child_list); e = list_next(e)){
		struct thread *t = list_entry(e, struct thread, child_elem);
		if (t->tid == tid) return t;
	}
	// 찾지 못했다면~
	return NULL;
}
/* Clones the current process as `name`. Returns the new process's thread id, or
 * TID_ERROR if the thread cannot be created. */
// 일단 파일 디스크립터와 가상 메모리 공간 복사해오기
tid_t
process_fork (const char *name, struct intr_frame *if_ UNUSED) {
	/* Clone current thread to new thread.*/
	struct thread *cur = thread_current();
	// 부모의 현재 유저 문맥을 스레드 구조체에 빽업해놓기
	memcpy(&cur->parent_if, if_, sizeof(struct intr_frame));
	// 자식 스레드 생성하기
	tid_t tid = thread_create (name, PRI_DEFAULT, __do_fork, cur);
	if (tid == TID_ERROR) return TID_ERROR;

	// 자식 리스트를 만들어 부모가 스레드 구조체를 찾아서 생성  완료 대기하기
	struct thread *child = get_child_thread(tid);

	// 자식이 do_fork를 할 때까지 멈춘다.
	sema_down(&child->fork_sema);
	
	if (child->exit_status == -1) {
        // 자식은 현재 process_exit의 sema_down(&free_sema)에 걸려있음.
        // 부모가 여기서 풀어주지 않으면 자식 스레드 페이지(4KB)가 영원히 해제되지 않음.
        sema_up(&child->free_sema); 
        // 자식 리스트에서도 제거
        list_remove(&child->child_elem);
        return TID_ERROR;
    }

	// 부모는 tid를 내놓고 기다린다.
	return tid;
}

#ifndef VM
/* Duplicate the parent's address space by passing this function to the
 * pml4_for_each. This is only for the project 2. */
static bool
duplicate_pte (uint64_t *pte, void *va, void *aux) {
	struct thread *current = thread_current ();
	struct thread *parent = (struct thread *) aux; // 부모의 스레드 포인터
	void *parent_page;
	void *newpage;
	bool writable;

	/* 1. TODO: If the parent_page is kernel page, then return immediately. */
	// 커널 영역 주소라면 즉시 반환
	if (is_kernel_vaddr(va)) return true;
	/* 2. Resolve VA from the parent's page map level 4. */
	// 부모의 pml4를 이용해 맞는 물리 메모리를 찾는다.
	parent_page = pml4_get_page (parent->pml4, va);
	if (parent_page == NULL) return false;
	/* 3. TODO: Allocate new PAL_USER page for the child and set result to
	 *    TODO: NEWPAGE. */
	// 자식 프로세스 페이지를 할당받기
	newpage = palloc_get_page(PAL_USER);
	if (newpage == NULL) return false;
	/* 4. TODO: Duplicate parent's page to the new page and
	 *    TODO: check whether parent's page is writable or not (set WRITABLE
	 *    TODO: according to the result). */
	// 부모 페이지 그대로 복사 & 읽기 권한이 있다면 권한도 가져오기
	memcpy(newpage, parent_page, PGSIZE);
	writable = is_writable(pte);
	/* 5. Add new page to child's page table at address VA with WRITABLE
	 *    permission. */
	// 자식 테이블에 매핑
	if (!pml4_set_page (current->pml4, va, newpage, writable)) {
		/* 6. TODO: if fail to insert page, do error handling. */
		// 실패시 자식 할당 공간 반납
		palloc_free_page(newpage);
		return false;
	}
	return true;
}
#endif

/* A thread function that copies parent's execution context.
 * Hint) parent->tf does not hold the userland context of the process.
 *       That is, you are required to pass second argument of process_fork to
 *       this function. */
// CR3 레지스터 : 현재 실행 중인 프로세스의 PML4 위치를 가리킨다.
// level4(pml4) : 큰 구역을 나눈다. -> PDP를 가리킨다.
// level3(PDP) : 그 안의 구역을 나눈다. -> PD를 가리킨다.
// level2(PD) : 더 작은 구역을 나눈다 -> PT를 가리킨다.
// level1(PT) : 진짜 구역을 가리킨다. -> 물리 프레임을 발견한다.
// Offset : 그 페이지 안에서 몇 번째 데이터인지 찾는다.
static void
__do_fork (void *aux) {
	struct intr_frame if_;
	struct thread *parent = (struct thread *) aux;
	struct thread *current = thread_current ();
	/* TODO: somehow pass the parent_if. (i.e. process_fork()'s if_) */
	struct intr_frame *parent_if;
	bool succ = true;

	// 저장해둔 부모 컨텍스트 가져오기
	parent_if = &parent->parent_if;
	// 부모의 레지스터 복사
	memcpy (&if_, parent_if, sizeof (struct intr_frame));
	if_.R.rax = 0; // 자식 프로세스의 리턴값 설정

	// 가상 메모리 공간 복사
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

	/* TODO: Your code goes here.
	 * TODO: Hint) To duplicate the file object, use `file_duplicate`
	 * TODO:       in include/filesys/file.h. Note that parent should not return
	 * TODO:       from the fork() until this function successfully duplicates
	 * TODO:       the resources of parent.*/
	// 가상 메모리 복사
	// 파일 디스크립터 테이블 복사
	// 자식 프로세스의 반환값 설정(0반환)
	// 부모에게 완료 신호 보내기
	// 자식도 페이지 테이블 할당
	process_init ();
	
	// current->fdt_table = palloc_get_page(PAL_ZERO);
	if (current->fdt_table == NULL){
		goto error;
	}
	lock_acquire(&filesys_lock);

	// 부모꺼 fdt 복사 ㄱㄱ
	current->fdt_table[0] = parent->fdt_table[0];
	current->fdt_table[1] = parent->fdt_table[1];  // fd 1도 그대로 복사 (stdout 또는 stdout marker)
	
    for (int i = 2; i < 512; i++){
        struct file *parent_file = parent->fdt_table[i];
        
        if(parent_file != NULL){
            // stdout marker 처리
            if (parent_file == (struct file *)1) {
                current->fdt_table[i] = parent_file;  // marker 그대로 복사
            } else {
                struct file *child_file = file_duplicate(parent_file);
                current->fdt_table[i] = child_file;
            }
        }
    }
	lock_release(&filesys_lock);
	// 성공했으면 엄마한테 신호보내기
	sema_up(&current->fork_sema);
	
	// 사용자 모드로 전환
	if (succ)
		do_iret (&if_);
error:
	// 실패해도 엄마는 깨워야한다.
	if (current->fdt_table != NULL){
		lock_acquire(&filesys_lock);
		for (int i = 2; i < 512; i++){
			if (current->fdt_table[i] != NULL){
				file_close(current->fdt_table[i]);
				current->fdt_table[i] = NULL;
			}
		}
		lock_release(&filesys_lock);
		palloc_free_page(current->fdt_table);
		current->fdt_table = NULL;
	}

	if (current->pml4 != NULL){
		pml4_activate(NULL);
	}

	process_cleanup();

	current->exit_status = -1;
	sema_up(&current->fork_sema);
	thread_exit ();
}

/* Switch the current execution context to the f_name.
 * Returns -1 on fail. */
// 현재 실행 컨텍스트를 f_name으로 전환
int
process_exec (void *f_name) {
	char *file_name = f_name;
	bool success;

	// f_name 으로 file_obj 추출
	//if(file_obj)
	/* We cannot use the intr_frame in the thread structure.
	 * This is because when current thread rescheduled,
	 * it stores the execution information to the member. */
	// 스레드 구조체에 있는 intr_frame을 사용할 수 없다.
	// 전환할 때 사용하는 스레드의 
	struct intr_frame _if;
	_if.ds = _if.es = _if.ss = SEL_UDSEG;
	_if.cs = SEL_UCSEG;
	_if.eflags = FLAG_IF | FLAG_MBS;

	/* We first kill the current context */
	// 먼저 현재 컨텍스트를 종료 -> pml4 파괴술
	process_cleanup();

	#ifdef VM
		supplemental_page_table_init (&thread_current()->spt);
	#endif
	/* And then load the binary */
	// 바이너리를 로드
	success = load (file_name, &_if);

	palloc_free_page(file_name);

	/* If load failed, quit. */
	// 로드에 실패하면 종료
	if (!success){
		thread_current()->exit_status = -1;
		thread_exit();
	}

	/* Start switched process. */
	// 전환된 프로세스를 시작
	do_iret (&_if);
	NOT_REACHED ();
}


/* Waits for thread TID to die and returns its exit status.  If
 * it was terminated by the kernel (i.e. killed due to an
 * exception), returns -1.  If TID is invalid or if it was not a
 * child of the calling process, or if process_wait() has already
 * been successfully called for the given TID, returns -1
 * immediately, without waiting.
 *
 * This function will be implemented in problem 2-2.  For now, it
 * does nothing. */
int
process_wait (tid_t child_tid UNUSED) { 
	// 구현 해야함 while을 통한 busy-wait로 먼저 해보고 생각해보기
	/* XXX: Hint) The pintos exit if process_wait (initd), we recommend you
	 * XXX:       to add infinite loop here before
	 * XXX:       implementing the process_wait. */
	// 자식 찾아서 가져와
	struct thread *child = get_child_thread(child_tid);

	if (child == NULL) return -1;
	// 자식이 종료될 때까지 대기
	sema_down(&child->wait_sema);
	// 자식 종료상태 가져오기
	int exit_status = child->exit_status;
	// 자식이 이제 free를 해도 된다.
	sema_up(&child->free_sema);
	// 자식 리스트에서 제거하기
	list_remove(&child->child_elem);

	return exit_status;
}

/* Exit the process. This function is called by thread_exit (). */
void
process_exit (void) {
	struct thread *curr = thread_current ();
	/* TODO: Your code goes here.
	 * TODO: Implement process termination message (see
	 * TODO: project2/process_termination.html).
	 * TODO: We recommend you to implement process resource cleanup here. */
	if (curr->pml4 != NULL){
		printf ("%s: exit(%d)\n", curr->name, curr->exit_status);
	}	
	// if (curr->running_file != NULL) file_allow_write(curr->running_file);
	
	bool lock_held = lock_held_by_current_thread(&filesys_lock);
    if (!lock_held) {
        lock_acquire(&filesys_lock);
    }
	
	if(curr->running_file!=NULL){
		file_close(curr->running_file);
		curr->running_file = NULL;
	}

	// 강제종료될 경우 정상적으로 닫히지 않은 잔존 파일들 닫아주기
	if (curr->fdt_table != NULL) {
        for (int i = 2; i < 512; i++) {
            if (curr->fdt_table[i] != NULL) {
                // 중복 검사 루프
                file_close(curr->fdt_table[i]); 
                curr->fdt_table[i] = NULL;
            }
        }
        // 테이블 자체 해제
        palloc_free_page(curr->fdt_table);
        curr->fdt_table = NULL;
    }
	
	while (!list_empty(&curr->child_list)) {
		struct list_elem *e = list_pop_front(&curr->child_list);
		struct thread *child = list_entry(e, struct thread, child_elem);
		child->parent = NULL;
		sema_up(&child->free_sema);
	}
	
	
	process_cleanup ();
	
	if (!lock_held) {
		lock_release(&filesys_lock);
	}
	// 부모에게 종료 상태 전달
	sema_up(&curr->wait_sema);
	
	sema_down(&curr->free_sema);
	
}


/* Free the current process's resources. */
static void
process_cleanup (void) {
	struct thread *curr = thread_current ();
	
	#ifdef VM
		supplemental_page_table_kill (&curr->spt);
	#endif

	uint64_t *pml4;
	/* Destroy the current process's page directory and switch back
	 * to the kernel-only page directory. */
	pml4 = curr->pml4;
	if (pml4 != NULL) {
		/* Correct ordering here is crucial.  We must set
		 * cur->pagedir to NULL before switching page directories,
		 * so that a timer interrupt can't switch back to the
		 * process page directory.  We must activate the base page
		 * directory before destroying the process's page
		 * directory, or our active page directory will be one
		 * that's been freed (and cleared). */
		curr->pml4 = NULL;
		pml4_activate (NULL);
		pml4_destroy (pml4);
	}
}

/* Sets up the CPU for running user code in the nest thread.
 * This function is called on every context switch. */
void
process_activate (struct thread *next) {
	/* Activate thread's page tables. */
	pml4_activate (next->pml4);

	/* Set thread's kernel stack for use in processing interrupts. */
	tss_update (next);
}

/* We load ELF binaries.  The following definitions are taken
 * from the ELF specification, [ELF1], more-or-less verbatim.  */

/* ELF types.  See [ELF1] 1-2. */
#define EI_NIDENT 16

#define PT_NULL    0            /* Ignore. */
#define PT_LOAD    1            /* Loadable segment. */
#define PT_DYNAMIC 2            /* Dynamic linking info. */
#define PT_INTERP  3            /* Name of dynamic loader. */
#define PT_NOTE    4            /* Auxiliary info. */
#define PT_SHLIB   5            /* Reserved. */
#define PT_PHDR    6            /* Program header table. */
#define PT_STACK   0x6474e551   /* Stack segment. */

#define PF_X 1          /* Executable. */
#define PF_W 2          /* Writable. */
#define PF_R 4          /* Readable. */

/* Executable header.  See [ELF1] 1-4 to 1-8.
 * This appears at the very beginning of an ELF binary. */
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

/* Abbreviations */
#define ELF ELF64_hdr
#define Phdr ELF64_PHDR

static bool setup_stack (struct intr_frame *if_);
static bool validate_segment (const struct Phdr *, struct file *);
static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
	uint32_t read_bytes, uint32_t zero_bytes,
	bool writable);
	
	/* Loads an ELF executable from FILE_NAME into the current thread.
	* Stores the executable's entry point into *RIP
	* and its initial stack pointer into *RSP.
	* Returns true if successful, false otherwise. */
static bool
load (const char *file_name, struct intr_frame *if_) {
	struct thread *t = thread_current ();
	struct ELF ehdr;
	struct file *file = NULL;
	off_t file_ofs;
	bool success = false;
	int i;
	
	char *file_name_copy = NULL;
	char *argv[64];
	int argc = 0;
	char *token, *bookmark;
	char* rsp = (char *)if_->rsp;
	char* stack_addr[64];
	
	int len;
	// char *argv_addr// argv 배열의 시작 주소
	int padding;
	lock_acquire(&filesys_lock);

	/* Allocate and activate page directory. */
	t->pml4 = pml4_create ();
	if (t->pml4 == NULL)
	goto done;
	process_activate (thread_current ());

	file_name_copy = palloc_get_page(PAL_ZERO);

	if (file_name_copy == NULL)
	{
		goto done;
	}
	
	strlcpy(file_name_copy, file_name, PGSIZE); // 최대 페이지 사이즈만큼 짤라서 복사
	
	// strtok_r -> 더 이상 분리할 단어가 없을때 NULL을 반환
	for (token = strtok_r(file_name_copy, " ", &bookmark);
	token != NULL;
		token = strtok_r(NULL, " ", &bookmark))
	{
		argv[argc++] = token;
	}

	file_name = argv[0];

	/* Open executable file. */
	file = filesys_open (file_name);
	if (file == NULL) {
		printf ("load: %s: open failed\n", file_name);
		goto done;
	}
	
	/* Read and verify executable header. */
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
				/* Ignore this segment. */
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
						/* Normal segment.
						 * Read initial part from disk and zero the rest. */
						read_bytes = page_offset + phdr.p_filesz;
						zero_bytes = (ROUND_UP (page_offset + phdr.p_memsz, PGSIZE)
								- read_bytes);
					} else {
						/* Entirely zero.
						 * Don't read anything from disk. */
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

	
	/* Set up stack. */
	if (!setup_stack (if_)){
		goto done;
	}
	
	/* Start address. */
	if_->rip = ehdr.e_entry;
	
	/* TODO: Your code goes here.
	* TODO: Implement argument passing (see project2/argument_passing.html). */ // argument 구현하기 1빠따로 해야할 일
	
	// 복사본 사용 -> 포인터라 real filename이 변경된다.
	for (int i = argc -1; i >= 0; i--)
	{
		len = strlen(argv[i]) + 1;
		if_->rsp -= len;
		memcpy((void*)if_->rsp, argv[i], len); // void*를 사용해도 되나?
		stack_addr[i] = (char*) if_->rsp;
	}
	
	int padding1 = (uintptr_t)if_->rsp % 8;
	if (padding1 != 0){
		if_->rsp -= padding1;
		memset((void*)if_->rsp, 0, padding1); // NULL 
	}
	
	// rsi rdi
	// if_ 구조체에 넣어주기
	// rsp 
	
	// argv[argc] = NULL;을 
	if_->rsp -= sizeof(char *);
	*((char**)if_->rsp) = NULL;
	
	// argv 주소값 배열 저장
	for (int j = argc - 1; j >= 0; j--){
		if_->rsp -= sizeof(char*);
		*((char **)if_->rsp) = stack_addr[j];
	}
	
	// 가짜 반환 주소 저장
	if_->rsp -= sizeof(void*);
	*((void**)if_->rsp) = NULL;
	
	// 새 사용자 프로세스를 위해 인터럽트 프레임을 업데이트
	if_->R.rdi = argc;	// 첫 번째 인자: argc
	if_->R.rsi = (uint64_t)if_->rsp + sizeof(void*); // argv[0]의 주소
	
	/*
	높은 주소 (USER_STACK = 0x47480000)
	+---------------------------+
	| argv[0] 문자열              | "child-read\0"
	| (stack_addr[0])           |
	+---------------------------+
	| argv[1] 문자열              | "3\0"
	| (stack_addr[1])           |
	+---------------------------+
	| padding (0~7 bytes)       | 8바이트 정렬
	+---------------------------+
	| NULL                      | argv[argc]
	+---------------------------+
	| stack_addr[1]             | argv[1] 주소
	+---------------------------+
	| stack_addr[0]             | argv[0] 주소
	+---------------------------+ ← rsi가 가리킴 (argv)
	| NULL (가짜 반환 주소)        |
	+---------------------------+ ← rsp
	낮은 주소
	*/
success = true;

done:
/* We arrive here whether the load is successful or not. */
	if (file_name_copy != NULL)
	{
		palloc_free_page(file_name_copy);
	}

	if (success) {
		t->running_file = file;
		file_deny_write(file);
	} else {
		file_close(file);
	}
	lock_release(&filesys_lock);
	return success;
}


/* Checks whether PHDR describes a valid, loadable segment in
* FILE and returns true if so, false otherwise. */
static bool
validate_segment (const struct Phdr *phdr, struct file *file) {
	/* p_offset and p_vaddr must have the same page offset. */
	if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK))
		return false;

	/* p_offset must point within FILE. */
	if (phdr->p_offset > (uint64_t) file_length (file))
		return false;

	/* p_memsz must be at least as big as p_filesz. */
	if (phdr->p_memsz < phdr->p_filesz)
		return false;

	/* The segment must not be empty. */
	if (phdr->p_memsz == 0)
		return false;

	/* The virtual memory region must both start and end within the
	   user address space range. */
	if (!is_user_vaddr ((void *) phdr->p_vaddr))
		return false;
	if (!is_user_vaddr ((void *) (phdr->p_vaddr + phdr->p_memsz)))
		return false;

	/* The region cannot "wrap around" across the kernel virtual
	   address space. */
	if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
		return false;

	/* Disallow mapping page 0.
	   Not only is it a bad idea to map page 0, but if we allowed
	   it then user code that passed a null pointer to system calls
	   could quite likely panic the kernel by way of null pointer
	   assertions in memcpy(), etc. */
	if (phdr->p_vaddr < PGSIZE)
		return false;

	/* It's okay. */
	return true;
}

#ifndef VM
/* Codes of this block will be ONLY USED DURING project 2.
 * If you want to implement the function for whole project 2, implement it
 * outside of #ifndef macro. */

/* load() helpers. */
static bool install_page (void *upage, void *kpage, bool writable);

/* Loads a segment starting at offset OFS in FILE at address
 * UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
 * memory are initialized, as follows:
 *
 * - READ_BYTES bytes at UPAGE must be read from FILE
 * starting at offset OFS.
 *
 * - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.
 *
 * The pages initialized by this function must be writable by the
 * user process if WRITABLE is true, read-only otherwise.
 *
 * Return true if successful, false if a memory allocation error
 * or disk read error occurs. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes, bool writable) {
	ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
	ASSERT (pg_ofs (upage) == 0);
	ASSERT (ofs % PGSIZE == 0);

	file_seek (file, ofs);
	while (read_bytes > 0 || zero_bytes > 0) {
		/* Do calculate how to fill this page.
		 * We will read PAGE_READ_BYTES bytes from FILE
		 * and zero the final PAGE_ZERO_BYTES bytes. */
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

		/* Get a page of memory. */
		uint8_t *kpage = palloc_get_page (PAL_USER);
		if (kpage == NULL)
			return false;

		/* Load this page. */
		if (file_read (file, kpage, page_read_bytes) != (int) page_read_bytes) {
			palloc_free_page (kpage);
			return false;
		}
		memset (kpage + page_read_bytes, 0, page_zero_bytes);

		/* Add the page to the process's address space. */
		if (!install_page (upage, kpage, writable)) {
			printf("fail\n");
			palloc_free_page (kpage);
			return false;
		}

		/* Advance. */
		read_bytes -= page_read_bytes;
		zero_bytes -= page_zero_bytes;
		upage += PGSIZE;
	}
	return true;
}

/* Create a minimal stack by mapping a zeroed page at the USER_STACK */
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

	/* Verify that there's not already a page at that virtual
	 * address, then map our page there. */
	return (pml4_get_page (t->pml4, upage) == NULL
			&& pml4_set_page (t->pml4, upage, kpage, writable));
}
#else
/* From here, codes will be used after project 3.
 * If you want to implement the function for only project 2, implement it on the
 * upper block. */


static bool
lazy_load_segment (struct page *page, void *aux) {
	/* TODO: Load the segment from the file */
	/* TODO: This called when the first page fault occurs on address VA. */
	/* TODO: VA is available when calling this function. */
	struct aux_info *new = (struct aux_info *)aux;
	
	file_seek(new->file, new->ofs);
	if(file_read(new->file, page->frame->kva, new->read_bytes) != (int) new->read_bytes){
        return false; 
    }

	memset(page->frame->kva + new->read_bytes, 0, new->zero_bytes);

	// file_close(new->file);
	free(aux);

	return true;
}

/* Loads a segment starting at offset OFS in FILE at address
 * UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
 * memory are initialized, as follows:
 *
 * - READ_BYTES bytes at UPAGE must be read from FILE
 * starting at offset OFS.
 *
 * - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.
 *
 * The pages initialized by this function must be writable by the
 * user process if WRITABLE is true, read-only otherwise.
 *
 * Return true if successful, false if a memory allocation error
 * or disk read error occurs. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes, bool writable) {
	ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
	ASSERT (pg_ofs (upage) == 0);
	ASSERT (ofs % PGSIZE == 0);

	while (read_bytes > 0 || zero_bytes > 0) {
		/* Do calculate how to fill this page.
		 * We will read PAGE_READ_BYTES bytes from FILE
		 * and zero the final PAGE_ZERO_BYTES bytes. */
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

		/* TODO: Set up aux to pass information to the lazy_load_segment. */
		struct aux_info *aux = malloc(sizeof(struct aux_info));
		if (aux == NULL) return false;

		aux->file = file_reopen(file);
		aux->ofs = ofs;
		aux->read_bytes = page_read_bytes;
		aux->zero_bytes = page_zero_bytes;

		if (aux->file == NULL) {
			free(aux);
			return false;
		}

		// 페이지 예약하기
		if (!vm_alloc_page_with_initializer (VM_ANON, upage, writable, lazy_load_segment, aux)){
			free(aux);
			return false;
		}
		
		// 변경사항 저장
		ofs += page_read_bytes;
		read_bytes -= page_read_bytes;
		zero_bytes -= page_zero_bytes;
		upage += PGSIZE;
	}
	return true;
}

/* Create a PAGE of stack at the USER_STACK. Return true on success. */
static bool
setup_stack (struct intr_frame *if_) {
	bool success = false;
	// 스택의 첫 페이지 주소 계산 -> USER_STACK 바로 아래
	void *stack_bottom = (void *) (((uint8_t *) USER_STACK) - PGSIZE);

	/* TODO: Map the stack on stack_bottom and claim the page immediately.
	 * TODO: If success, set the rsp accordingly.
	 * TODO: You should mark the page is stack. */
	// VM 시스템에 스택 페이지 등록
	if (vm_alloc_page (VM_ANON | VM_MARKER_0, stack_bottom, true)) {
		success = vm_claim_page (stack_bottom);
		if (success)
			// 스택 포인터 초기화
			if_->rsp = USER_STACK;
			// 초기 스택 포인터도 스레드 구조체에 백업
			thread_current()->stack_pointer = USER_STACK;
	}
	return success;
}
// static bool setup_stack(struct intr_frame* if_) {
//     struct page* page;
//     struct thread* cur = thread_current();
//     void* stack_bottom = (uint8_t*)USER_STACK - PGSIZE;
//     if (!vm_alloc_page_with_initializer(VM_ANON | VM_MARKER_0, stack_bottom, true, NULL, NULL))
//         return false;
//     if (!vm_claim_page(stack_bottom)) {
//         page = spt_find_page(&cur->spt, stack_bottom);
//         if (page != NULL)
//             spt_remove_page(&cur->spt, page);
//         return false;
//     }
//     if_->rsp = USER_STACK;
//     return true;
// }
#endif /* VM */
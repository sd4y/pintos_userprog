#include "userprog/syscall.h"
#include <stdio.h>
#include <stdlib.h>
#include <syscall-nr.h>
#include <string.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "intrinsic.h"
#include "userprog/process.h"
#include "filesys/filesys.h"
#include "filesys/file.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "devices/input.h"
#include "threads/init.h"

/* 파일 시스템 접근을 동기화하기 위한 전역 락 */
struct lock filesys_lock;

void syscall_entry (void);
void syscall_handler (struct intr_frame *);

/* 유저 포인터 검증 함수 */
static void check_user_ptr(const void *uaddr);
static void check_user_buffer(const void* uaddr, unsigned size, bool writable);
static void check_user_string(const char *str);

/* 파일 디스크립터 테이블 관리 함수 */
static int fd_allocate(struct file *file);
static struct file *fd_get_file(int fd);
static void fd_remove(int fd);
static void fd_table_destroy(struct thread *t);

/* 시스템 콜 구현 함수들 */
static void sys_halt(void);
static void sys_exit(int status);
static tid_t sys_fork(const char *thread_name, struct intr_frame *f);
static int sys_exec(const char *cmd_line);
static int sys_wait(tid_t pid);
static bool sys_create(const char *file, unsigned initial_size);
static bool sys_remove(const char *file);
static int sys_open(const char *file);
static int sys_filesize(int fd);
static int sys_read(int fd, void *buf, unsigned size);
static int sys_write(int fd, const void *buf, unsigned size);
static void sys_seek(int fd, unsigned position);
static unsigned sys_tell(int fd);
static void sys_close(int fd);

/* 시스템 콜.
 *
 * 과거에는 시스템 콜 서비스가 인터럽트 핸들러에 의해 처리되었습니다
 * (예: 리눅스의 int 0x80). 그러나 x86-64에서는 제조사가 시스템 콜을
 * 요청하기 위한 더 효율적인 경로인 `syscall` 명령을 제공합니다.
 *
 * `syscall` 명령은 모델 특수 레지스터(MSR)의 값을 읽어 동작합니다.
 * 자세한 내용은 매뉴얼을 참고하세요. */

#define MSR_STAR 0xc0000081         /* 세그먼트 셀렉터 MSR */
#define MSR_LSTAR 0xc0000082        /* 롱 모드 SYSCALL 대상 주소 */
#define MSR_SYSCALL_MASK 0xc0000084 /* EFLAGS 마스크 */

void
syscall_init (void) {
	lock_init(&filesys_lock);
	
	write_msr(MSR_STAR, ((uint64_t)SEL_UCSEG - 0x10) << 48  |
			((uint64_t)SEL_KCSEG) << 32);
	write_msr(MSR_LSTAR, (uint64_t) syscall_entry);

	/* 인터럽트 서비스 루틴은 syscall_entry가 유저랜드 스택을
	 * 커널 모드 스택으로 교체하기 전까지 어떤 인터럽트도 처리하면 안 됩니다.
	 * 따라서 FLAG_FL을 마스킹합니다. */
	write_msr(MSR_SYSCALL_MASK,
			FLAG_IF | FLAG_TF | FLAG_DF | FLAG_IOPL | FLAG_AC | FLAG_NT);
}

static void
sys_halt(void){
	power_off();
}

static void
sys_exit(int status){
	struct thread *curr = thread_current();
	curr->exit_status = status;

	thread_exit();
}

static tid_t
sys_fork(const char *thread_name, struct intr_frame *f){
	check_user_string(thread_name);
	return process_fork(thread_name, f);
}

static int
sys_exec(const char *cmd_line){
	check_user_string(cmd_line);
	
	/* cmd_line을 커널 공간으로 복사 (process_exec에서 페이지 해제하므로) */
	char *cmd_copy = palloc_get_page(0);
	if (cmd_copy == NULL)
		return -1;
	
	strlcpy(cmd_copy, cmd_line, PGSIZE);
	
	/* exec 실패 시 -1, 성공 시 리턴하지 않음 */
	if (process_exec(cmd_copy) < 0) {
    	/* process_exec에서 이미 페이지를 해제했음 */
    	sys_exit(-1);               // 프로세스 종료
	}
	
	NOT_REACHED();
	return -1;
}

static int
sys_wait(tid_t pid){
	return process_wait(pid);
}

static bool
sys_create(const char *file, unsigned initial_size){
	check_user_string(file);
	
	if (file == NULL || strlen(file) == 0)
		return false;
	
	lock_acquire(&filesys_lock);
	bool success = filesys_create(file, initial_size);
	lock_release(&filesys_lock);
	
	return success;
}

static bool
sys_remove(const char *file){
	check_user_string(file);
	
	lock_acquire(&filesys_lock);
	bool success = filesys_remove(file);
	lock_release(&filesys_lock);
	
	return success;
}

static int
sys_open(const char *file){
	check_user_string(file);
	
	if (file == NULL || strlen(file) == 0)
		return -1;
	
	lock_acquire(&filesys_lock);
	struct file *f = filesys_open(file);
	lock_release(&filesys_lock);
	
	if (f == NULL)
		return -1;
	
	/* 파일 디스크립터 할당 */
	int fd = fd_allocate(f);
	if (fd == -1) {
		file_close(f);
		return -1;
	}
	
	return fd;
}

static int
sys_filesize(int fd){
	struct file *f = fd_get_file(fd);
	if (f == NULL)
		return -1;
	
	lock_acquire(&filesys_lock);
	int size = file_length(f);
	lock_release(&filesys_lock);
	
	return size;
}

static int
sys_read(int fd, void *buf, unsigned size){
	check_user_buffer(buf, size, true);	

	if(fd == 0){
		/* STDIN: 키보드로부터 입력 */
		unsigned i;
		uint8_t* buffer = (uint8_t*)buf;
		for(i = 0; i < size; i++){
			buffer[i] = input_getc();
		}
		return size;
	}
	
	if (fd == 1)
		return -1;  /* STDOUT에서는 읽을 수 없음 */
	
	struct file *f = fd_get_file(fd);
	if (f == NULL)
		return -1;
	
	lock_acquire(&filesys_lock);
	int bytes_read = file_read(f, buf, size);
	lock_release(&filesys_lock);
	
	return bytes_read;
}

static int
sys_write(int fd, const void *buf, unsigned size){
	check_user_buffer(buf, size, false);

	if(fd == 1){ 
		/* STDOUT: 콘솔에 출력 */
		putbuf(buf, size);
		return size;
	}
	
	if (fd == 0)
		return 0;  /* STDIN에는 쓸 수 없음 */
	
	struct file *f = fd_get_file(fd);
	if (f == NULL)
		return -1;
	
	lock_acquire(&filesys_lock);
	int bytes_written = file_write(f, buf, size);
	lock_release(&filesys_lock);
	
	return bytes_written;
}

static void
sys_seek(int fd, unsigned position){
	struct file *f = fd_get_file(fd);
	if (f == NULL)
		return;
	
	lock_acquire(&filesys_lock);
	file_seek(f, position);
	lock_release(&filesys_lock);
}

static unsigned
sys_tell(int fd){
	struct file *f = fd_get_file(fd);
	if (f == NULL)
		return 0;
	
	lock_acquire(&filesys_lock);
	unsigned pos = file_tell(f);
	lock_release(&filesys_lock);
	
	return pos;
}

static void
sys_close(int fd){
	if (fd < 2)  /* STDIN, STDOUT은 닫을 수 없음 */
		return;
	
	struct file *f = fd_get_file(fd);
	if (f == NULL)
		return;
	
	lock_acquire(&filesys_lock);
	file_close(f);
	lock_release(&filesys_lock);
	
	fd_remove(fd);
}

/* 유저 포인터가 유효한지 검사 */
static void
check_user_ptr(const void *uaddr){
	struct thread *curr = thread_current();

	if (uaddr == NULL || !is_user_vaddr(uaddr) || pml4_get_page(curr->pml4, uaddr) == NULL) {
		sys_exit(-1);
	}
}

/* 유저 버퍼 전체가 유효한지 검사 */
static void
check_user_buffer(const void* uaddr, unsigned size, bool writable){
	const uint8_t* start = uaddr;
	const uint8_t* end = start + size;

	/* 페이지 경계마다 검사 */
	for (const uint8_t* p = start; p < end; p += PGSIZE) {
		check_user_ptr(p);
	}
	/* 마지막 바이트도 검사 */
	if(size > 0){
		check_user_ptr(end - 1);
	}
}

/* 유저 문자열이 유효한지 검사 */
static void
check_user_string(const char *str){
	check_user_ptr(str);
	
	/* 문자열 끝까지 검사 */
	const char *p = str;
	while (true) {
		check_user_ptr(p);
		if (*p == '\0')
			break;
		p++;
	}
}

/* 파일 디스크립터 테이블 초기화 */
void
fd_table_init(struct thread *t){
	if (t->fd_table != NULL)
		return;
	
	/* 초기 크기 64로 설정 */
	t->fd_table_size = 64;
	t->fd_table = (struct file **)calloc(t->fd_table_size, sizeof(struct file *));
	
	if (t->fd_table == NULL)
		PANIC("fd_table allocation failed");
	
	t->next_fd = 2;  /* 0: STDIN, 1: STDOUT */
}

/* 파일에 대한 fd 할당 */
static int
fd_allocate(struct file *file){
	struct thread *t = thread_current();
	
	/* fd 테이블이 없으면 초기화 */
	if (t->fd_table == NULL)
		fd_table_init(t);
	
	/* 사용 가능한 fd 찾기 */
	int fd = -1;
	for (int i = 2; i < t->fd_table_size; i++) {
		if (t->fd_table[i] == NULL) {
			fd = i;
			break;
		}
	}
	
	/* 빈 슬롯이 없으면 테이블 확장 */
	if (fd == -1) {
		/* 프로세스당 열 수 있는 파일 개수 제한 (64개로 감소) */
		if (t->fd_table_size >= 64)
			return -1;
			
		int new_size = t->fd_table_size * 2;
		if (new_size > 64)
			new_size = 64;
			
		struct file **new_table = (struct file **)calloc(new_size, sizeof(struct file *));
		
		if (new_table == NULL)
			return -1;
		
		/* 기존 데이터 복사 */
		memcpy(new_table, t->fd_table, t->fd_table_size * sizeof(struct file *));
		free(t->fd_table);
		
		fd = t->fd_table_size;
		t->fd_table = new_table;
		t->fd_table_size = new_size;
	}
	
	t->fd_table[fd] = file;
	return fd;
}

/* fd로부터 파일 구조체 가져오기 */
static struct file *
fd_get_file(int fd){
	struct thread *t = thread_current();
	
	if (t->fd_table == NULL || fd < 0 || fd >= t->fd_table_size)
		return NULL;
	
	return t->fd_table[fd];
}

/* fd 제거 */
static void
fd_remove(int fd){
	struct thread *t = thread_current();
	
	if (t->fd_table == NULL || fd < 2 || fd >= t->fd_table_size)
		return;
	
	t->fd_table[fd] = NULL;
}

/* fd 테이블 전체 정리 */
static void
fd_table_destroy(struct thread *t){
	if (t->fd_table == NULL)
		return;
	
	/* 모든 열린 파일 닫기 */
	/* filesys_lock을 이미 가지고 있는지 확인 (exception 처리 중일 수 있음) */
	bool need_lock = !lock_held_by_current_thread(&filesys_lock);
	
	if (need_lock)
		lock_acquire(&filesys_lock);
	
	for (int i = 2; i < t->fd_table_size; i++) {
		if (t->fd_table[i] != NULL) {
			file_close(t->fd_table[i]);
			t->fd_table[i] = NULL;
		}
	}
	
	if (need_lock)
		lock_release(&filesys_lock);
	
	/* 테이블 메모리 해제 */
	free(t->fd_table);
	t->fd_table = NULL;
	t->fd_table_size = 0;
}

/* 모든 fd 닫기 (외부에서 호출 가능) */
void
close_all_fds(void){
	fd_table_destroy(thread_current());
}

/* 메인 시스템 콜 인터페이스 */
void
syscall_handler (struct intr_frame *f) {
	int syscall_no = f->R.rax;
	
	switch (syscall_no)	{
	case SYS_HALT:
		sys_halt();
		NOT_REACHED();
		break;

	case SYS_EXIT: {
		int status = (int) f->R.rdi;
		sys_exit(status);
		NOT_REACHED();
		break;
	}

	case SYS_FORK: {
		const char *thread_name = (const char *)f->R.rdi;
		tid_t tid = sys_fork(thread_name, f);
		f->R.rax = (uint64_t)tid;
		break;
	}

	case SYS_EXEC: {
		const char *cmd_line = (const char *)f->R.rdi;
		sys_exec(cmd_line);
		NOT_REACHED();
		break;
	}

	case SYS_WAIT: {
		tid_t pid = (tid_t)f->R.rdi;
		int ret = sys_wait(pid);
		f->R.rax = (uint64_t)ret;
		break;
	}

	case SYS_CREATE: {
		const char *file = (const char *)f->R.rdi;
		unsigned initial_size = (unsigned)f->R.rsi;
		bool ret = sys_create(file, initial_size);
		f->R.rax = (uint64_t)ret;
		break;
	}

	case SYS_REMOVE: {
		const char *file = (const char *)f->R.rdi;
		bool ret = sys_remove(file);
		f->R.rax = (uint64_t)ret;
		break;
	}

	case SYS_OPEN: {
		const char *file = (const char *)f->R.rdi;
		int ret = sys_open(file);
		f->R.rax = (uint64_t)ret;
		break;
	}

	case SYS_FILESIZE: {
		int fd = (int)f->R.rdi;
		int ret = sys_filesize(fd);
		f->R.rax = (uint64_t)ret;
		break;
	}

	case SYS_READ: {
		int fd = (int)f->R.rdi;
		void *buf = (void *)f->R.rsi;
		unsigned size = (unsigned)f->R.rdx;
		int ret = sys_read(fd, buf, size);
		f->R.rax = (uint64_t)ret;
		break;
	}

	case SYS_WRITE: {
		int fd = (int)f->R.rdi;
		const void *buf = (const void *)f->R.rsi;
		unsigned size = (unsigned)f->R.rdx;
		int ret = sys_write(fd, buf, size);
		f->R.rax = (uint64_t)ret;
		break;
	}

	case SYS_SEEK: {
		int fd = (int)f->R.rdi;
		unsigned position = (unsigned)f->R.rsi;
		sys_seek(fd, position);
		break;
	}

	case SYS_TELL: {
		int fd = (int)f->R.rdi;
		unsigned ret = sys_tell(fd);
		f->R.rax = (uint64_t)ret;
		break;
	}

	case SYS_CLOSE: {
		int fd = (int)f->R.rdi;
		sys_close(fd);
		break;
	}

	default:
		printf("Unknown system call: %d\n", syscall_no);
		sys_exit(-1);
		break;
	}
}

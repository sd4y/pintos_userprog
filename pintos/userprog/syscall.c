#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "threads/synch.h"
#include "threads/palloc.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "intrinsic.h"
#include "filesys/filesys.h"

void syscall_entry (void);
void syscall_handler (struct intr_frame *);

struct lock filesys_lock;

/* 시스템 호출.
 *
 * 이전에는 시스템 호출 서비스가 인터럽트 핸들러에 의해 처리되었습니다
 * (예: 리눅스의 int 0x80). 그러나 x86-64에서는 제조업체가
 * 시스템 호출을 요청하는 효율적인 경로인 `syscall` 명령을 제공합니다.
 *
 * syscall 명령은 모델 특정 레지스터(MSR)에서 값을 읽어 작동합니다.
 * 자세한 내용은 매뉴얼을 참조하세요. */

#define MSR_STAR 0xc0000081         /* 세그먼트 선택자 msr */
#define MSR_LSTAR 0xc0000082        /* 롱 모드 SYSCALL 대상 */
#define MSR_SYSCALL_MASK 0xc0000084 /* eflags용 마스크 */

/* 초기화 함수 */

void
syscall_init (void) {
	lock_init(&filesys_lock);
	write_msr(MSR_STAR, ((uint64_t)SEL_UCSEG - 0x10) << 48  |
			((uint64_t)SEL_KCSEG) << 32);
	write_msr(MSR_LSTAR, (uint64_t) syscall_entry);

	/* 인터럽트 서비스 루틴은 syscall_entry가 사용자 스택을 커널
	 * 모드 스택으로 교환할 때까지 어떤 인터럽트도 처리하지 않아야 합니다.
	 * 따라서 FLAG_FL을 마스크했습니다. */
	write_msr(MSR_SYSCALL_MASK,
			FLAG_IF | FLAG_TF | FLAG_DF | FLAG_IOPL | FLAG_AC | FLAG_NT);
}

/* 헬퍼 함수들 */

/* Reads a byte at user virtual address UADDR.
 * UADDR must be below KERN_BASE.
 * Returns the byte value if successful, -1 if a segfault
 * occurred. */
 static int64_t
 get_user (const uint8_t *uaddr) {
	 int64_t result;
	 __asm __volatile (
	 "movabsq $done_get, %0\n"
	 "movzbq %1, %0\n"
	 "done_get:\n"
	 : "=&a" (result) : "m" (*uaddr));
	 return result;
 }
 
 /* Writes BYTE to user address UDST.
  * UDST must be below KERN_BASE.
  * Returns true if successful, false if a segfault occurred. */
 static bool
 put_user (uint8_t *udst, uint8_t byte) {
	 int64_t error_code;
	 __asm __volatile (
	 "movabsq $done_put, %0\n"
	 "movb %b2, %1\n"
	 "done_put:\n"
	 : "=&a" (error_code), "=m" (*udst) : "q" (byte));
	 return error_code != -1;
 }

 static void
 validate_user_buffer(const void *buffer, size_t length) {
	 const uint8_t *u = buffer;
	 if (u == NULL)
		 syscall_exit(-1);
	 if (length == 0)
		 return;
 
	 const uint8_t *end = u + length - 1;
	 for (const uint8_t *p = u; p <= end; ) {
		 if (get_user(p) == -1)
			 syscall_exit(-1);
		 /* 다음 페이지의 첫 주소로 점프 */
		 uintptr_t next = ( ((uintptr_t)p) & ~PGMASK ) + PGSIZE;
		 if (next > (uintptr_t)end)
			 break;
		 p = (const uint8_t *)next;
	 }
	 /* 마지막 바이트도 한 번 명시적으로 확인 */
	 if (get_user(end) == -1)
		 syscall_exit(-1);
 }

static bool
copy_user_string (char *kbuf, const char *ustr, size_t max_len)
{
	if (ustr == NULL || is_kernel_vaddr(ustr))
		return false;

	size_t i = 0;
	while (i + 1 < max_len) {
		int64_t val = get_user((const uint8_t *)ustr + i);
		if (val == -1)
			return false;

		kbuf[i] = (char)val;
		if (kbuf[i] == '\0')
			return true;
		i++;
	}

	kbuf[max_len - 1] = '\0';
	return true;
}

static struct file *
find_file_by_fd(int fd) {
	struct thread *cur = thread_current ();
	if (fd < 0 || fd >= FD_MAX)
		return NULL;
	return cur->fd_table[fd];
}

static int
fd_insert (struct file *f) {
	struct thread *t = thread_current();

    for (int fd = 2; fd < FD_MAX; fd++) {
        if (t->fd_table[fd] == NULL) {
            t->fd_table[fd] = f;
            return fd;
        }
    }
    return -1; 
}

/* 시스템 콜 구현 */

static int
syscall_read(int fd, void *buffer, unsigned size) {
	if (size == 0)
		return 0;

	if (fd == STDIN_FILENO) {
		for (unsigned i = 0; i < size; i++) {
			uint8_t c= input_getc();
			if (!(put_user((uint8_t *)buffer + i, c)))
				syscall_exit(-1);
		}
		return (int)size;
	}

	if (fd < 2 || fd >= FD_MAX)
		return -1;

	struct file *f = find_file_by_fd(fd);
	if (f == NULL)
		return -1;

	uint8_t *kbuf = palloc_get_page(0);
	if (kbuf == NULL)
		syscall_exit(-1);

	unsigned remaining = size;
	unsigned copied = 0;

	while (remaining > 0) {
		unsigned chunk = remaining > PGSIZE ? PGSIZE : remaining;
		lock_acquire(&filesys_lock);
		int n = file_read(f, kbuf, chunk);
		lock_release(&filesys_lock);

		if (n <= 0)
			break;

		for (int i = 0; i < n; i++) {
			if (!put_user((uint8_t *)buffer + copied + i, kbuf[i])) {
				palloc_free_page(kbuf);
				syscall_exit(-1);
			}
		}
		copied += n;
		remaining -= n;

		if ((unsigned)n < chunk)
    		break;
	}
	palloc_free_page(kbuf);
	return copied;
}

static int
syscall_write(int fd, const void *buffer, unsigned length) {
	validate_user_buffer(buffer, length);

	if (fd == STDOUT_FILENO) {
		putbuf(buffer, length);
		return length;
	}

	if (fd < 2 || fd >= FD_MAX)
		return -1;

	struct file *f = find_file_by_fd(fd);
	if (f == NULL)
		return -1;

	lock_acquire(&filesys_lock);
	int ret = file_write(f, buffer, length);
	lock_release(&filesys_lock);

	return ret;
}

static bool
syscall_remove(const char* filename) {
	char *kname = palloc_get_page(0);
	if (kname == NULL)
		syscall_exit(-1);
	if (copy_user_string(kname, filename, PGSIZE) == false) {
		palloc_free_page(kname);
		syscall_exit(-1);
	}

	if (kname[0] == '\0') {
		palloc_free_page(kname);
		return false;
	}

	lock_acquire(&filesys_lock);
    bool ok = filesys_remove(kname);
    lock_release(&filesys_lock);

	palloc_free_page(kname);
	
	return ok;
}

static int
syscall_open(const char* filename) {
	char *kname = palloc_get_page(0);

	if (kname == NULL)
		syscall_exit(-1);

	if (copy_user_string(kname, filename, PGSIZE) == false) {
		palloc_free_page(kname);
		syscall_exit(-1);
	}

	if (kname[0] == '\0') {
		palloc_free_page(kname);
		return -1;
	}

	lock_acquire(&filesys_lock);
	struct file *f = filesys_open(kname);
	lock_release(&filesys_lock);

	palloc_free_page(kname);

	if (f == NULL)
		return -1;

	int fd = fd_insert(f);
	
	if (fd == -1) {
		lock_acquire(&filesys_lock);
		file_close(f);
		lock_release(&filesys_lock);
	}

	return fd;
}

static void
syscall_close (int fd) {
	do_close_fd(thread_current(), fd);
}

void
syscall_exit (int status) {
	struct thread *cur = thread_current();
    cur->exit_status = status;
    thread_exit();
}


static bool
syscall_create (const char *file, unsigned initial_size) {
    char *kname = palloc_get_page(0);
    if (kname == NULL)
        syscall_exit(-1);

    if (copy_user_string(kname, file, PGSIZE) == false) {
		palloc_free_page(kname);
		syscall_exit(-1);
	}

    if (kname[0] == '\0') {
        palloc_free_page(kname);
        return false;
    }

    lock_acquire(&filesys_lock);
    bool ok = filesys_create(kname, initial_size);
    lock_release(&filesys_lock);

    palloc_free_page(kname);

    return ok;
}

static int
syscall_filesize (int fd) {
	struct file *f = find_file_by_fd(fd);
	if (f == NULL)
		return -1;

	lock_acquire(&filesys_lock);
	int length = file_length(f);
	lock_release(&filesys_lock);

	return length;
}

static void
syscall_seek (int fd, unsigned position) {
	struct file *f = find_file_by_fd(fd);
	if (f == NULL)
		return;

	lock_acquire(&filesys_lock);
	file_seek(f, position);
	lock_release(&filesys_lock);
}

static unsigned
syscall_tell (int fd) {
	struct file *f = find_file_by_fd(fd);
	if (f == NULL)
		return 0;

	lock_acquire(&filesys_lock);
	int position = file_tell(f);
	lock_release(&filesys_lock);

	return position;
}

static int
syscall_exec (const char *cmd_line) {
	char *kname = palloc_get_page(0);

	if (kname == NULL)
		syscall_exit(-1);

	if (!copy_user_string(kname, cmd_line, PGSIZE)) {
		palloc_free_page(kname);
		syscall_exit(-1);
	}

	if (kname[0] == '\0') {
		palloc_free_page(kname);
		syscall_exit(-1);
	}

	int ret = process_exec(kname);

	if (ret == -1)
		syscall_exit(-1);
	
	return ret;
}

/* 주요 시스템 호출 인터페이스 */
void
syscall_handler (struct intr_frame *f) {
	// TODO: 여기에 구현을 작성하세요.
	switch (f->R.rax) {
		case SYS_HALT:
			power_off();
			break;
		case SYS_EXIT:
			syscall_exit((int)f->R.rdi);
			break;
		case SYS_CREATE:
			f->R.rax = syscall_create((const char *)f->R.rdi, (unsigned)f->R.rsi);
			break;
		case SYS_FORK:
			f->R.rax = process_fork((const char *)f->R.rdi, f);
			break;
		case SYS_EXEC:
			f->R.rax = syscall_exec((const char *)f->R.rdi);
			break;
		case SYS_WAIT:
			f->R.rax = process_wait(f->R.rdi);
			break;
		case SYS_REMOVE:
			f->R.rax = syscall_remove(f->R.rdi);
			break;
		case SYS_FILESIZE:
			f->R.rax = syscall_filesize(f->R.rdi);
			break;
		case SYS_CLOSE:
			syscall_close(f->R.rdi);
			break;
		case SYS_READ:
			f->R.rax = syscall_read(f->R.rdi, (void *)f->R.rsi, f->R.rdx);
			break;
		case SYS_WRITE:
			f->R.rax = syscall_write(f->R.rdi, (void *)f->R.rsi, f->R.rdx);
			break;
		case SYS_OPEN:
			f->R.rax = syscall_open(f->R.rdi); 
			break;
		case SYS_SEEK:
			syscall_seek((int)f->R.rdi, (unsigned)f->R.rsi);
			break;
		case SYS_TELL:
			f->R.rax = syscall_tell((int)f->R.rdi);
			break;
		default:
			break;
	}
}
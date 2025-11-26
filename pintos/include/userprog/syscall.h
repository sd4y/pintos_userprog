#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H

struct thread;  /* Forward declaration */

void syscall_init (void);
void close_all_fds (void);  /* 모든 fd 닫기 (process_exit에서 사용) */
void fd_table_init (struct thread *t);  /* fd 테이블 초기화 (fork에서 사용) */

#endif /* userprog/syscall.h */

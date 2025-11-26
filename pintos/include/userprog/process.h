#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include "threads/thread.h"
#include "threads/synch.h"

/* 부모가 자식 프로세스를 기다리기 위해 사용하는 구조체 */
struct wait_status {
    tid_t tid;                /* 자식의 tid */
    int exit_status;          /* 자식의 종료 코드 */
    bool exited;              /* 자식이 이미 종료했는지 */
    bool waited;              /* 이 자식에 대해 이미 wait() 했는지 */
    struct semaphore sema;    /* 부모가 자식 종료를 기다릴 때 사용하는 세마포어 */
    struct list_elem elem;    /* 부모의 children 리스트에 들어가는 리스트 노드 */
};


tid_t process_create_initd (const char *file_name);
tid_t process_fork (const char *name, struct intr_frame *if_);
int process_exec (void *f_name);
int process_wait (tid_t);
void process_exit (void);
void process_activate (struct thread *next);

#endif /* userprog/process.h */

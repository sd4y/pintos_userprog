#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include "threads/thread.h"

struct child_info * find_child_info(struct thread *parent, tid_t child_tid);

bool do_close_fd(struct thread *t, int fd);
tid_t process_create_initd (const char *file_name);
tid_t process_fork (const char *name, struct intr_frame *if_);
int process_exec (void *f_name);
int process_wait (tid_t);
void process_exit (void);
void process_activate (struct thread *next);
bool load_argument (const char *file_name, struct intr_frame *if_);

#endif /* userprog/process.h */

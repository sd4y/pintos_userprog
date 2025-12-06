#ifndef VM_FILE_H
#define VM_FILE_H
#include "filesys/file.h"
#include "vm/vm.h"

struct page;
enum vm_type;

struct file_page {
	struct file *file;      // 매핑된 파일 포인터
    off_t offset;           // 파일의 몇 번째 바이트부터 읽을지
    size_t read_bytes;      // 페이지에서 읽어야 할 실제 데이터 크기
    size_t zero_bytes;      // 나머지 0으로 채울 크기
};

void vm_file_init (void);
bool file_backed_initializer (struct page *page, enum vm_type type, void *kva);
void *do_mmap(void *addr, size_t length, int writable,
		struct file *file, off_t offset);
void do_munmap (void *va);
#endif

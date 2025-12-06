/* file.c: Implementation of memory backed file object (mmaped object). */

#include "vm/vm.h"
#include <string.h>
#include "threads/vaddr.h"
#include "threads/mmu.h"
static bool file_backed_swap_in (struct page *page, void *kva);
static bool file_backed_swap_out (struct page *page);
static void file_backed_destroy (struct page *page);

/* DO NOT MODIFY this struct */
static const struct page_operations file_ops = {
	.swap_in = file_backed_swap_in,
	.swap_out = file_backed_swap_out,
	.destroy = file_backed_destroy,
	.type = VM_FILE,
};

// 파일 내용
struct mmap_info {
    struct file *file;      // 매핑된 파일 포인터
    off_t offset;           // 파일의 몇 번째 바이트부터 읽을지
    size_t read_bytes;      // 페이지에서 읽어야 할 실제 데이터 크기
    size_t zero_bytes;      // 나머지 0으로 채울 크기
};

/* The initializer of file vm */
void
vm_file_init (void) {
}

/* Initialize the file backed page */
bool
file_backed_initializer (struct page *page, enum vm_type type, void *kva) {
	/* Set up the handler */
	page->operations = &file_ops;

	// mmap 정보를 담을 구조체
	struct file_page *file_page UNUSED = &page->file;
	struct mmap_info *aux_info = (struct mmap_info *)page->uninit.aux;

	file_page->file = aux_info->file;
    file_page->offset = aux_info->offset;
    file_page->read_bytes = aux_info->read_bytes;
    file_page->zero_bytes = aux_info->zero_bytes;

    memset (kva, 0, PGSIZE);
    return true;
}

/* Swap in the page by read contents from the file. */
static bool
file_backed_swap_in (struct page *page, void *kva) {
	struct file_page *file_page UNUSED = &page->file;
	memset (kva, 0, PGSIZE);
    return true;
}

/* Swap out the page by writeback contents to the file. */
static bool
file_backed_swap_out (struct page *page) {
	struct file_page *file_page UNUSED = &page->file;
	return true;
}

/* Destory the file backed page. PAGE will be freed by the caller. */
static void
file_backed_destroy (struct page *page) {
	struct file_page *file_page UNUSED = &page->file;

	if(page->frame != NULL){
		// 수정되었는지 확인하기
		if(pml4_is_dirty(thread_current()->pml4, page->va)){
			bool lock_held = lock_held_by_current_thread(&filesys_lock);
            if (!lock_held) lock_acquire(&filesys_lock);

			// 수정되었다면 파일에 저장
			file_write_at(page->file.file, page->frame->kva, page->file.read_bytes, page->file.offset);
			if (!lock_held) lock_release(&filesys_lock);
			pml4_set_dirty(thread_current()->pml4, page->va, false);
		}
	}
	if (file_page->file != NULL) {
		bool lock_held = lock_held_by_current_thread(&filesys_lock);
        if (!lock_held) lock_acquire(&filesys_lock);
        file_close(file_page->file);
        if (!lock_held) lock_release(&filesys_lock);
	}
}

static bool lazy_load_file (struct page *page, void *aux) {
    struct mmap_info *info = (struct mmap_info *)aux;

	page->operations = &file_ops;
	bool lock_held = lock_held_by_current_thread(&filesys_lock);
    if (!lock_held) lock_acquire(&filesys_lock);
	
    // 파일 읽기 (Load)
    file_seek(info->file, info->offset);
    if (file_read(info->file, page->frame->kva, info->read_bytes) != info->read_bytes) {
		if (!lock_held) lock_release(&filesys_lock); // 실패 시 해제
		free(info);
		return false; // 읽기 실패
    }

    // 0으로 채우기
    memset(page->frame->kva + info->read_bytes, 0, info->zero_bytes);
	if (!lock_held) lock_release(&filesys_lock); // 성공 시 해제

    struct file_page *file_page = &page->file;
    file_page->file = info->file;
    file_page->offset = info->offset;
    file_page->read_bytes = info->read_bytes;
	file_page->zero_bytes = info->zero_bytes;


    free(info);
    return true;
}

/* Do the mmap */
void *
do_mmap (void *addr, size_t length, int writable, struct file *file, off_t offset) {

	if(addr == NULL) return NULL;
	if (pg_ofs(addr) != 0) return NULL;
	if (is_kernel_vaddr(addr)) return NULL;
	if ((long)length <= 0) return NULL;

	void *check_addr = addr;
    size_t check_len = length;
    while (check_len > 0) {
        if (spt_find_page(&thread_current()->spt, check_addr)) {
            return NULL; // 이미 올라가 있는 상태
        }
        
        // 스택 영역과 겹치는지도 확인 필요 (spt_find_page가 해결해줄 수도 있지만, stack_bottom 확인 필요할 수 있음)
        if (check_addr >= (void *)USER_STACK) return NULL; // Pintos 스택 시작점 근처 겹치면 거부 (대략적인 값)

        check_addr += PGSIZE;
        check_len = (check_len < PGSIZE) ? 0 : check_len - PGSIZE;
    }

	void *start_addr = addr; // 반환할 시작 주소 저장

	struct file *reopen_file = file_reopen(file); // 여기서 미리 reopen 권장
    if (reopen_file == NULL) return NULL;
    size_t file_len = file_length(reopen_file);

	while (length > 0) {
		size_t page_read_bytes = length < PGSIZE ? length : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;
		
		if (offset + page_read_bytes > file_len) {
            if (offset >= file_len) {
                page_read_bytes = 0; // 이미 파일 끝을 지남
            } else {
                page_read_bytes = file_len - offset; // 남은 만큼만 읽기
            }
            // read_bytes가 줄어든 만큼 zero_bytes를 늘려야 함 (페이지 크기는 유지)
            page_zero_bytes = PGSIZE - page_read_bytes;
        }

		struct mmap_info *info = malloc(sizeof(struct mmap_info));
		if (info == NULL) return NULL;
		info->file = file_reopen(file);
		info->offset = offset;
		info->read_bytes = page_read_bytes;
		info->zero_bytes = page_zero_bytes;

		// 페이지 예약
		if (!vm_alloc_page_with_initializer(VM_FILE, addr, writable, lazy_load_file, info)){
			file_close(info->file);
			free(info);
			return NULL;
		}

		addr += PGSIZE;
		offset += (length < PGSIZE ? length : PGSIZE); 
        length -= (length < PGSIZE ? length : PGSIZE);
	}

	file_close(reopen_file);
	return start_addr;
}

/* Do the munmap */
void
do_munmap (void *addr) {
	// addr 부터 시작해서 연속된 페이지를 찾는다.
	while(true){
		struct page* page = spt_find_page(&thread_current()->spt, addr);
		if (page == NULL) break; // 매핑된 페이지 없으면 종료

		pml4_clear_page(thread_current()->pml4, page->va);
		// 페이지 해제 (SPT 제거, 프레임 반납)
		spt_remove_page(&thread_current()->spt, page->va);
		addr += PGSIZE;
	}
}
/* file.c: Implementation of memory backed file object (mmaped object). */

#include "vm/vm.h"
#include <string.h>
#include "devices/disk.h"
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

static struct disk *swap_disk;
static struct list free_swap_slots;
static struct lock swap_lock;

struct swap_slot {
	struct list_elem elem;
	size_t slot_index;
};

/* The initializer of file vm */
void
vm_file_init (void) {
	swap_disk = disk_get(1, 1);
	list_init(&free_swap_slots);
	lock_init(&swap_lock);
	
	// 초기화 - 모든 swap slot을 프리 리스트에 추가
	size_t total_slots = disk_size(swap_disk) / (PGSIZE / DISK_SECTOR_SIZE);
	
	for (size_t i = 0; i < total_slots; i++) {
		struct swap_slot *slot = malloc(sizeof(struct swap_slot));
		slot->slot_index = i;
		list_push_back(&free_swap_slots, &slot->elem);
	}
}

static int alloc_swap_slot (void) {
	lock_acquire(&swap_lock);
	
	if (list_empty(&free_swap_slots)) {
		lock_release(&swap_lock);
		return -1;
	}
	
	struct swap_slot *slot = list_entry(list_pop_front(&free_swap_slots), struct swap_slot, elem);
	
	size_t slot_index = slot->slot_index;
	free(slot);
	
	lock_release(&swap_lock);
	return slot_index;
}

static void free_swap_slot (int slot_index) {
	struct swap_slot *slot = malloc(sizeof(struct swap_slot));
	slot->slot_index = slot_index;
	
	lock_acquire(&swap_lock);
	list_push_back(&free_swap_slots, &slot->elem);
	lock_release(&swap_lock);
}

/* Initialize the file backed page */
bool
file_backed_initializer (struct page *page, enum vm_type type, void *kva) {
	/* Set up the handler */
	page->operations = &file_ops;

	// mmap 정보를 담을 구조체
	struct file_page *file_page UNUSED = &page->file;
	struct aux_info *aux_info = (struct aux_info *)page->uninit.aux;

	file_page->file = aux_info->file;
    file_page->ofs = aux_info->ofs;
    file_page->read_bytes = aux_info->read_bytes;
    file_page->zero_bytes = aux_info->zero_bytes;

	file_page->swap_index = -1;
    return true;
}

/* Swap in the page by read contents from the file. */
static bool
file_backed_swap_in (struct page *page, void *kva) {
	struct file_page *file_page UNUSED = &page->file;
	
	bool lock_held = lock_held_by_current_thread(&filesys_lock);
    if (!lock_held) lock_acquire(&filesys_lock);

    file_seek(file_page->file, file_page->ofs);
    
    if (file_read(file_page->file, kva, file_page->read_bytes) != (int)file_page->read_bytes) {
        if (!lock_held) lock_release(&filesys_lock);
        return false;
    }
    
    memset(kva + file_page->read_bytes, 0, file_page->zero_bytes);
    
    if (!lock_held) lock_release(&filesys_lock);
    
    return true;
}

/* Swap out the page by writeback contents to the file. */
static bool
file_backed_swap_out (struct page *page) {
	struct file_page *file_page UNUSED = &page->file;
	struct thread *curr = thread_current();
	if (page->frame == NULL) {
		return false;
	}

	// Dirty 확인: 페이지가 수정되었다면 파일에 저장
    if (pml4_is_dirty(curr->pml4, page->va)) {
        bool lock_held = lock_held_by_current_thread(&filesys_lock);
        if (!lock_held) lock_acquire(&filesys_lock);
        file_write_at(file_page->file, page->frame->kva, file_page->read_bytes, file_page->ofs);
        if (!lock_held) lock_release(&filesys_lock);
        
        pml4_set_dirty(curr->pml4, page->va, false);
    }

    // 페이지 연결 끊기 (프레임 해제는 호출자가 처리함)
    pml4_clear_page(curr->pml4, page->va);
    page->frame = NULL; // 프레임과의 연결 끊기

    return true;
}

/* Destory the file backed page. PAGE will be freed by the caller. */
static void
file_backed_destroy (struct page *page) {
	struct file_page *file_page = &page->file;
    struct thread *curr = thread_current();

	// 메모리에 로드되어 있고(frame != NULL), 수정된 적이 있다면(Dirty) 파일에 저장
    if (page->frame != NULL && pml4_is_dirty(curr->pml4, page->va)) {
        bool lock_held = lock_held_by_current_thread(&filesys_lock);
        if (!lock_held) lock_acquire(&filesys_lock);

        int bytes_written = file_write_at(file_page->file, page->frame->kva, file_page->read_bytes, file_page->ofs);

        if (!lock_held) lock_release(&filesys_lock);
    }

    // 파일 닫기
    if (file_page->file != NULL) {
        bool lock_held = lock_held_by_current_thread(&filesys_lock);
        if (!lock_held) lock_acquire(&filesys_lock);
        
        file_close(file_page->file);
        
        if (!lock_held) lock_release(&filesys_lock);
    }
}

static bool lazy_load_file (struct page *page, void *aux) {
    struct aux_info *info = (struct aux_info *)aux;

	page->operations = &file_ops;
	bool lock_held = lock_held_by_current_thread(&filesys_lock);
    if (!lock_held) lock_acquire(&filesys_lock);
	
	if (info->file == NULL) {
		if (!lock_held) lock_release(&filesys_lock);
		free(info);
		return false;
	}

    // 파일 읽기 (Load)
    file_seek(info->file, info->ofs);
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
    file_page->ofs = info->ofs;
    file_page->read_bytes = info->read_bytes;
    file_page->zero_bytes = info->zero_bytes;
	file_page->swap_index = -1;  // 초기화

    free(info);
    return true;
}

/* Do the mmap */
void *
do_mmap (void *addr, size_t length, int writable, struct file *file, off_t offset) {

	if (addr == NULL || pg_round_down(addr) != addr || is_kernel_vaddr(addr) || 
        (long long)length <= 0 || offset % PGSIZE != 0) {
        return NULL;
    }
	
	if (length == 0) return NULL;

    struct file *reopen_file = file_reopen(file);
    if (reopen_file == NULL) return NULL;

    void *start_addr = addr; // 반환할 시작 주소 저장
	bool lock_held = lock_held_by_current_thread(&filesys_lock);
	if (!lock_held) lock_acquire(&filesys_lock);
    size_t file_len = file_length(reopen_file);
	if (!lock_held) lock_release(&filesys_lock);

    // length가 0이 될 때까지 반복
    while (length > 0) {
        size_t page_read_bytes = length < PGSIZE ? length : PGSIZE;
        size_t page_zero_bytes = PGSIZE - page_read_bytes;
        
        // 파일 끝을 넘어가는 경우 처리
		size_t read_bytes_real = 0;
        if (offset < file_len) {
             size_t rem = file_len - offset;
             read_bytes_real = rem < PGSIZE ? rem : PGSIZE;
        }
        size_t zero_bytes_real = PGSIZE - read_bytes_real;

        struct aux_info *info = malloc(sizeof(struct aux_info));
        if (info == NULL) {
            file_close(reopen_file);
            return NULL;
        }

		info->file = file_reopen(file); 
        info->ofs = offset;
        info->read_bytes = read_bytes_real;
        info->zero_bytes = zero_bytes_real;

        // 페이지 예약 (VM_FILE 타입)
        if (!vm_alloc_page_with_initializer(VM_FILE, addr, writable, lazy_load_file, info)){
            file_close(info->file);
            free(info);
            file_close(reopen_file);
            return NULL;
        }

        // 다음 페이지로 이동
		addr += PGSIZE;
		offset += PGSIZE;
		if (length >= PGSIZE) length -= PGSIZE;
        else length = 0;
    }
    
    // 원본 reopen_file은 이제 필요 없음 (각 페이지가 복사본 가짐)
    file_close(reopen_file);
    return start_addr;
}

/* Do the munmap */
void
do_munmap (void *addr) {
	struct thread *cur = thread_current();

    while (true) {
        struct page* page = spt_find_page(&cur->spt, addr);
        if (page == NULL) break;
        
        // spt_remove_page 내부에서 destroy 호출 -> file_backed_destroy 호출됨
        // 따라서 여기서 별도의 file_write_at을 할 필요가 없음!
        spt_remove_page(&cur->spt, page);

        addr += PGSIZE;
    }
}
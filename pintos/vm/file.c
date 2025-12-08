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
	
	// 원본 파일에서 읽기
	if(file_page->swap_index == -1){
		bool lock_held = lock_held_by_current_thread(&filesys_lock);
		if (!lock_held) lock_acquire(&filesys_lock);
		// 파일 포인터 설정 후 열기
		file_seek(file_page->file, file_page->ofs);
		if(file_read(file_page->file, kva, file_page->read_bytes) != file_page->read_bytes){
			if (!lock_held) lock_release(&filesys_lock);
			return false;
		}

		memset(kva + file_page->read_bytes, 0, file_page->zero_bytes);
		if (!lock_held) lock_release(&filesys_lock);

		return true;
	}

	// 스왑 디스크에서 읽기 (swap_out 되었던 것들)
	int slot_index = file_page->swap_index;
	if (slot_index < 0 || slot_index >= disk_size(swap_disk) / (PGSIZE / DISK_SECTOR_SIZE)) {
		return false;
	}

	disk_sector_t sector = slot_index * (PGSIZE / DISK_SECTOR_SIZE);

	for(int i = 0; i < PGSIZE / DISK_SECTOR_SIZE; i++){
		disk_read(swap_disk, sector + i, (uint8_t *)kva + i * DISK_SECTOR_SIZE);
	}

	free_swap_slot(slot_index);
	file_page->swap_index = -1;

    return true;
}

/* Swap out the page by writeback contents to the file. */
static bool
file_backed_swap_out (struct page *page) {
	struct file_page *file_page UNUSED = &page->file;

	if (page->frame == NULL) {
		return false;
	}

	// 수정되지 않았던 파일 페이지는 스왑할 필요 없음
	if(!pml4_is_dirty(thread_current()->pml4, page->va)){
		return true;
	}

	// 수정된 경우
	int slot_index = alloc_swap_slot();
	if(slot_index == -1) return false; // 스왑 공간 부족

	// 스왑 디스크에 쓰기
	uint8_t *kva = page->frame->kva;
	disk_sector_t sector = slot_index * (PGSIZE / DISK_SECTOR_SIZE);

	for (int i = 0; i < PGSIZE / DISK_SECTOR_SIZE; i++) {
		disk_write(swap_disk, sector + i, kva + i * DISK_SECTOR_SIZE);
	}

	file_page->swap_index = slot_index;

	return true;
}

/* Destory the file backed page. PAGE will be freed by the caller. */
static void
file_backed_destroy (struct page *page) {
	struct file_page *file_page UNUSED = &page->file;
	
	// 스왑 디스크에 있는 경우 슬롯 해제
	if (file_page->swap_index != -1) {
		free_swap_slot(file_page->swap_index);
	}

	if(page->frame != NULL && pml4_is_dirty(thread_current()->pml4, page->va)){
		// 수정되었는지 확인하기
		bool lock_held = lock_held_by_current_thread(&filesys_lock);
		if (!lock_held) lock_acquire(&filesys_lock);

		// 수정되었다면 파일에 저장
		file_write_at(page->file.file, page->frame->kva, page->file.read_bytes, page->file.ofs);
		if (!lock_held) lock_release(&filesys_lock);
	}

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
		if (offset >= file_len) {
			page_read_bytes = 0;
			page_zero_bytes = PGSIZE;
		} else if (offset + page_read_bytes > file_len) {
			page_read_bytes = file_len - offset;
			page_zero_bytes = PGSIZE - page_read_bytes;
		}

        struct aux_info *info = malloc(sizeof(struct aux_info));
        if (info == NULL) {
            file_close(reopen_file);
            return NULL;
        }

		info->file = file_reopen(file);
		if (info->file == NULL) {
			free(info);
			file_close(reopen_file);
			return NULL;
		}
		
		info->ofs = offset;
		info->read_bytes = page_read_bytes;
		info->zero_bytes = page_zero_bytes;

        // 페이지 예약 (VM_FILE 타입)
        if (!vm_alloc_page_with_initializer(VM_FILE, addr, writable, lazy_load_file, info)){
            file_close(info->file);
            free(info);
            file_close(reopen_file);
            return NULL;
        }

        // 다음 페이지로 이동
		addr += PGSIZE;
		offset += page_read_bytes;  /* 읽은 바이트만 증가 */
		length -= page_read_bytes;   /* length도 읽은 바이트만 감소 */
    }
    
    // 원본 reopen_file은 이제 필요 없음 (각 페이지가 복사본 가짐)
    file_close(reopen_file);
    return start_addr;
}

/* Do the munmap */
void
do_munmap (void *addr) {
	struct thread *cur = thread_current();
	struct list munmap_pages;
	list_init(&munmap_pages);

	// addr 부터 시작해서 연속된 페이지를 찾는다.
	while(true){
		struct page* page = spt_find_page(&thread_current()->spt, addr);
		if (page == NULL) break; // 매핑된 페이지 없으면 종료
		
		if (page->frame != NULL && pml4_is_dirty(cur->pml4, page->va)) {
			enum vm_type type = page->operations->type;
			if (type == VM_FILE) {
				struct file_page *file_page = &page->file;
				if (file_page->file != NULL && file_page->swap_index == -1) {
					bool lock_held = lock_held_by_current_thread(&filesys_lock);
					if (!lock_held) lock_acquire(&filesys_lock);
					
					file_write_at(file_page->file, page->frame->kva, 
								  file_page->read_bytes, file_page->ofs);
					
					if (!lock_held) lock_release(&filesys_lock);
				}
			}
		}
		
		// 파일 정리
		if (page->operations->type == VM_FILE) {
			struct file_page *file_page = &page->file;
			if (file_page->file != NULL) {
				bool lock_held = lock_held_by_current_thread(&filesys_lock);
				if (!lock_held) lock_acquire(&filesys_lock);
				
				file_close(file_page->file);
				file_page->file = NULL;
				
				if (!lock_held) lock_release(&filesys_lock);
			}
		}
		
		/* SPT에서 제거 */
		spt_remove_page(&cur->spt, page);

		/* 페이지 테이블 엔트리 제거 */
		if (pml4_get_page(cur->pml4, addr)) {
			pml4_clear_page(cur->pml4, addr);
		}

		addr += PGSIZE;
	}
}
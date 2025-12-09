/* file.c: Implementation of memory backed file object (mmaped object). */

#include "vm/vm.h"
#include "threads/vaddr.h"
#include "threads/mmu.h"
#include "userprog/process.h"
#include "filesys/filesys.h"
#include <string.h>

static bool file_backed_swap_in (struct page *page, void *kva);
static bool file_backed_swap_out (struct page *page);
static void file_backed_destroy (struct page *page);
static bool lazy_load_file (struct page *page, void *aux);

/* DO NOT MODIFY this struct */
static const struct page_operations file_ops = {
	.swap_in = file_backed_swap_in,
	.swap_out = file_backed_swap_out,
	.destroy = file_backed_destroy,
	.type = VM_FILE,
};

extern struct lazy_aux;

/* The initializer of file vm */
void
vm_file_init (void) {
}

/* Initialize the file backed page */
bool
file_backed_initializer (struct page *page, enum vm_type type, void *kva) {
	/* Set up the handler */
	page->operations = &file_ops;
	
	/* aux에서 file 정보를 복사 */
	struct lazy_aux *aux = (struct lazy_aux *) page->uninit.aux;
	if (aux != NULL) {
		page->file.file = aux->file;
		page->file.ofs = aux->ofs;
		page->file.read_bytes = aux->read_bytes;
		page->file.zero_bytes = aux->zero_bytes;
		page->file.writable = aux->writable;
	} else {
		memset (&page->file, 0, sizeof page->file);
	}
	return true;
}

/* Swap in the page by read contents from the file. */
static bool
file_backed_swap_in (struct page *page, void *kva) {
	struct file_page *file_page = &page->file;
	if (file_page->file == NULL)
		return false;

	bool need_lock = !lock_held_by_current_thread(&filesys_lock);
	if (need_lock) lock_acquire(&filesys_lock);
	
	if (file_read_at (file_page->file, kva, file_page->read_bytes,
			file_page->ofs) != (int) file_page->read_bytes) {
		if (need_lock) lock_release(&filesys_lock);
		return false;
	}

	if (need_lock) lock_release(&filesys_lock);
	memset (kva + file_page->read_bytes, 0, file_page->zero_bytes);
	return true;
}

/* Swap out the page by writeback contents to the file. */
static bool
file_backed_swap_out (struct page *page) {
	struct file_page *file_page = &page->file;
	
	if (file_page->file == NULL)
		return false;
	
	/* dirty 페이지면 파일에 write back */
	if (pml4_is_dirty (thread_current ()->pml4, page->va)) {
		bool need_lock = !lock_held_by_current_thread(&filesys_lock);
		if (need_lock) lock_acquire(&filesys_lock);
		
		file_write_at (file_page->file, page->frame->kva,
				file_page->read_bytes, file_page->ofs);
		
		if (need_lock) lock_release(&filesys_lock);
		pml4_set_dirty (thread_current ()->pml4, page->va, false);
	}
	
	/* 프레임 연결 해제 */
	pml4_clear_page (thread_current ()->pml4, page->va);
	page->frame->page = NULL;
	page->frame = NULL;
	
	return true;
}

/* Destory the file backed page. PAGE will be freed by the caller. */
static void
file_backed_destroy (struct page *page) {
	struct file_page *file_page = &page->file;
	
	/* dirty 페이지면 파일에 write back */
	if (page->frame != NULL && file_page->file != NULL) {
		if (pml4_is_dirty (thread_current ()->pml4, page->va)) {
			bool need_lock = !lock_held_by_current_thread(&filesys_lock);
			if (need_lock) lock_acquire(&filesys_lock);
			
			file_write_at (file_page->file, page->frame->kva,
					file_page->read_bytes, file_page->ofs);
			
			if (need_lock) lock_release(&filesys_lock);
		}
	}
}

/* mmap용 lazy load 콜백 */
static bool
lazy_load_file (struct page *page, void *aux) {
	struct lazy_aux *laux = (struct lazy_aux *) aux;
	
	bool need_lock = !lock_held_by_current_thread(&filesys_lock);
	if (need_lock) lock_acquire(&filesys_lock);
	
	if (file_read_at (laux->file, page->frame->kva, laux->read_bytes, laux->ofs)
			!= (int) laux->read_bytes) {
		if (need_lock) lock_release(&filesys_lock);
		free (aux);
		return false;
	}
	
	if (need_lock) lock_release(&filesys_lock);
	memset (page->frame->kva + laux->read_bytes, 0, laux->zero_bytes);
	free (aux);
	return true;
}

/* Do the mmap */
void *
do_mmap (void *addr, size_t length, int writable,
		struct file *file, off_t offset) {
	/* 파일 복제 (독립적인 position 유지) */
	struct file *mfile = file_reopen (file);
	if (mfile == NULL)
		return NULL;
	
	void *start_addr = addr;
	size_t file_len = file_length (mfile);
	
	/* 실제 매핑할 길이 결정 */
	if (length > file_len - offset)
		length = file_len - offset;
	
	size_t read_bytes = length;
	off_t ofs = offset;
	
	while (read_bytes > 0) {
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;
		
		/* lazy_aux 생성 */
		struct lazy_aux *aux = malloc (sizeof (struct lazy_aux));
		if (aux == NULL) {
			file_close (mfile);
			return NULL;
		}
		aux->file = mfile;
		aux->ofs = ofs;
		aux->read_bytes = page_read_bytes;
		aux->zero_bytes = page_zero_bytes;
		aux->writable = writable;
		
		/* VM_FILE 타입 페이지 할당 */
		if (!vm_alloc_page_with_initializer (VM_FILE, addr, writable,
					lazy_load_file, aux)) {
			free (aux);
			file_close (mfile);
			return NULL;
		}
		
		read_bytes -= page_read_bytes;
		ofs += page_read_bytes;
		addr += PGSIZE;
	}
	
	return start_addr;
}

/* Do the munmap */
void
do_munmap (void *addr) {
	struct thread *curr = thread_current ();
	struct page *page = spt_find_page (&curr->spt, addr);
	
	if (page == NULL)
		return;
	
	/* 첫 페이지의 파일 정보 저장 (모든 페이지가 같은 파일 공유) */
	struct file *file = NULL;
	if (page->operations->type == VM_FILE)
		file = page->file.file;
	else if (page->operations->type == VM_UNINIT) {
		struct lazy_aux *aux = (struct lazy_aux *) page->uninit.aux;
		if (aux != NULL)
			file = aux->file;
	}
	
	/* 연속된 mmap 페이지들을 모두 해제 */
	while (page != NULL) {
		struct page *next_page = spt_find_page (&curr->spt, addr + PGSIZE);
		
		/* 같은 mmap 영역인지 확인 */
		bool same_mapping = false;
		if (next_page != NULL) {
			if (next_page->operations->type == VM_FILE &&
					next_page->file.file == file)
				same_mapping = true;
			else if (next_page->operations->type == VM_UNINIT) {
				struct lazy_aux *aux = (struct lazy_aux *) next_page->uninit.aux;
				if (aux != NULL && aux->file == file)
					same_mapping = true;
			}
		}
		
		/* 현재 페이지 제거 */
		spt_remove_page (&curr->spt, page);
		
		addr += PGSIZE;
		page = same_mapping ? next_page : NULL;
	}
	
	/* 파일 닫기 */
	if (file != NULL)
		file_close (file);
}

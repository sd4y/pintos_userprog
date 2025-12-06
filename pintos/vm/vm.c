/* vm.c: Generic interface for virtual memory objects. */

#include "threads/malloc.h"
#include "vm/vm.h"
#include "vm/inspect.h"
#include <hash.h>
#include "threads/vaddr.h"
#include "threads/mmu.h"
#include "threads/thread.h"
#include <string.h>

struct list frame_table;
struct lock frame_table_lock;

void spt_destroy_page (struct hash_elem *e, void *aux);

/* Initializes the virtual memory subsystem by invoking each subsystem's
 * intialize codes. */
void
vm_init (void) {
	vm_anon_init ();
	vm_file_init ();

#ifdef EFILESYS  /* For project 4 */
	pagecache_init ();
#endif
	register_inspect_intr ();
	/* DO NOT MODIFY UPPER LINES. */
	/* TODO: Your code goes here. */
	list_init(&frame_table);
	lock_init(&frame_table_lock);
}

/* Get the type of the page. This function is useful if you want to know the
 * type of the page after it will be initialized.
 * This function is fully implemented now. */
enum vm_type
page_get_type (struct page *page) {
	int ty = VM_TYPE (page->operations->type);
	switch (ty) {
		case VM_UNINIT:
			return VM_TYPE (page->uninit.type);
		default:
			return ty;
	}
}

/* Helpers */
static struct frame *vm_get_victim (void);
static bool vm_do_claim_page (struct page *page);
static struct frame *vm_evict_frame (void);

/* Create the pending page object with initializer. If you want to create a
 * page, do not create it directly and make it through this function or
 * `vm_alloc_page`. */
bool
vm_alloc_page_with_initializer (enum vm_type type, void *upage, bool writable,
		vm_initializer *init, void *aux) {

	ASSERT (VM_TYPE(type) != VM_UNINIT);

	struct supplemental_page_table *spt = &thread_current ()->spt;

	if (spt_find_page (spt, upage) == NULL) {
		struct page *page = malloc (sizeof (struct page));
		if (page == NULL)
			goto err;

		bool (*page_initializer) (struct page *, enum vm_type, void *) = NULL;
		switch (VM_TYPE (type)) {
		case VM_ANON:
			page_initializer = anon_initializer;
			break;
		case VM_FILE:
			page_initializer = file_backed_initializer;
			break;
		default:
			goto err;
		}

		uninit_new (page, upage, init, type, aux, page_initializer);
		page->writable = writable;

		if (spt_insert_page (spt, page))
			return true;
		free (page);
	}
err:
	return false;
}

/* Find VA from spt and return page. On error, return NULL. */
struct page *
spt_find_page (struct supplemental_page_table *spt UNUSED, void *va UNUSED) {
	// 더미 페이지 생성
	struct page page;
	// 4kb로 정렬 -> 가상 주소를 찾아야 하기 때문
	page.va = pg_round_down(va); // 내림
	// 테이블 검색
	struct hash_elem *e = hash_find(&spt->page, &page.hash_elem);
	if(e == NULL) return NULL;
	// page 구조체로 hash_elem을 준다.
	return hash_entry(e, struct page, hash_elem);
}

/* Insert PAGE into spt with validation. */
bool
spt_insert_page (struct supplemental_page_table *spt UNUSED,
		struct page *page UNUSED) {
	int succ = false;
	succ = hash_insert(&spt->page, &page->hash_elem) == NULL;

	return succ;
}

void
spt_remove_page (struct supplemental_page_table *spt, struct page *page) {
	// hash 테이블에서 삭제하기
	struct hash_elem *e;

	e = hash_delete (&spt->page, &page->hash_elem);
	// 페이지 메모리 삭제하기
	if(e != NULL) spt_destroy_page (e, NULL);
}

/* Get the struct frame, that will be evicted. */
static struct frame *
vm_get_victim (void) {
	struct frame *victim = NULL;
	 /* TODO: The policy for eviction is up to you. */

	return victim;
}

/* Evict one page and return the corresponding frame.
 * Return NULL on error.*/
static struct frame *
vm_evict_frame (void) {
	struct frame *victim UNUSED = vm_get_victim ();
	/* TODO: swap out the victim and return the evicted frame. */

	return NULL;
}

/* palloc() and get frame. If there is no available page, evict the page
 * and return it. This always return valid address. That is, if the user pool
 * memory is full, this function evicts the frame to get the available memory
 * space.*/
static struct frame *
vm_get_frame (void) {
	void *kva = palloc_get_page (PAL_USER | PAL_ZERO);
	/* TODO: Fill this function. */
	struct frame *frame = NULL;
	
	if (kva == NULL) PANIC("todo: implement eviction");
	
	// 프레임 맴버 초기화
	frame = malloc(sizeof(struct frame));
	if (frame == NULL) {
        palloc_free_page(kva);
        PANIC ("Failed to allocate frame metadata");
    }

	frame->kva = kva;
	frame->page = NULL;

	lock_acquire (&frame_table_lock);
	list_push_back (&frame_table, &frame->frame_elem);
	lock_release (&frame_table_lock);
	ASSERT (frame != NULL);
	ASSERT (frame->page == NULL);
	return frame;
}

/* Growing the stack. */
static void
vm_stack_growth (void *addr UNUSED) {
	void *stack_bottom = pg_round_down (addr);
    // alloc만 수행하기
    vm_alloc_page(VM_ANON | VM_MARKER_0, stack_bottom, true);
}

/* Handle the fault on write_protected pag1e */
static bool
vm_handle_wp (struct page *page UNUSED) {
}

/* Return true on success */
bool
vm_try_handle_fault (struct intr_frame *f UNUSED, void *addr UNUSED,
		bool user UNUSED, bool write UNUSED, bool not_present UNUSED) {
	struct thread *cur = thread_current();
	struct supplemental_page_table *spt = &cur->spt;
	struct page *page;

	// 커널 영역 주소이거나 주소가 NULL이면 처리 불가
	if (is_kernel_vaddr(addr) || addr == NULL) {
        return false;
    }
	
	// 스탯 포인터 확인
	uintptr_t rsp;
	// if(user){
	// 	// 유저 모드 폴트 -> intr_frame에 저장된 rsp가 진짜임
	// 	rsp = f->rsp;
	// } else {
	// 	// 커널 모드 폴트 -> intr_frame의 rsp는 쓰레기 값이므로, 백업해둔 값 사용
	// 	rsp = thread_current()->stack_pointer;
	// }
	rsp = f->rsp;

	// 스택 확장 판별 / 접근 주소가 USER_STACK인가? / 접근 주소가 1MB 제한 이내인가? /
	if (addr <= (void *)USER_STACK && addr >= (void *)(USER_STACK - (1 << 20)) && addr >= (void *)(rsp - 8)){
		if (spt_find_page(spt, addr) == NULL){
			vm_stack_growth(addr);
		}
	}
	
	if (!not_present && write)
		return false;

	// 페이지 복구 및 물리 메모리 연결
	page = spt_find_page (spt, addr);

	if (page == NULL)
		return false;

	if (write && !page->writable)
		return false;

	return vm_do_claim_page (page);
}

/* Free the page.
 * DO NOT MODIFY THIS FUNCTION. */
void
vm_dealloc_page (struct page *page) {
	destroy (page);
	free (page);
}

/* Claim the page that allocate on VA. */
bool
vm_claim_page (void *va UNUSED) {
	struct page *page = NULL;
	struct thread *cur = thread_current();
	// 해당 va를 가진 페이지 구조체 찾기
	page = spt_find_page(&cur->spt, va);
	
	// 페이지가 없으면 실패
	if(page == NULL) return false;
	
	return vm_do_claim_page (page);
}

/* Claim the PAGE and set up the mmu. */
static bool
vm_do_claim_page (struct page *page) {
	// 프레임 할당 받기
	struct frame *frame = vm_get_frame ();
	struct thread *t = thread_current ();
	if (frame == NULL) return false;

	/* Set links */
	// 링크하기
	frame->page = page;
	page->frame = frame;

	/* TODO: Insert page table entry to map page's VA to frame's PA. */
	// MMU 설정하기 -> 현재 스레드, 가상주소, 커널 가상주소, 쓰기 권한
	// 매핑 실패시
	if (!pml4_set_page (thread_current ()->pml4, page->va, frame->kva, page->writable)) {
		frame->page = NULL;
		page->frame = NULL;
		palloc_free_page (frame->kva);
		free (frame);
		return false;
	}
	return swap_in (page, frame->kva);
}

// hash_hash_func
// 2개의 포인터를 넘겨야 함.
unsigned page_hash (const struct hash_elem *h, void *aux UNUSED){
	const struct page *p = hash_entry(h, struct page, hash_elem); // hash_elem을 사용해서 struct page를 찾아낸다.
	return hash_bytes (&p->va, sizeof p->va); // va를 해싱한다.1
}

// 버킷 리스트 정렬용 크기 비교 함수
bool page_less (const struct hash_elem *a, const struct hash_elem *b, void *aux UNUSED){
	const struct page *a_ = hash_entry(a, struct page, hash_elem);
	const struct page *b_ = hash_entry(b, struct page, hash_elem);
	if (a_->va < b_->va) return true;
	return false;
}

/* Initialize new supplemental page table */
void
supplemental_page_table_init (struct supplemental_page_table *spt UNUSED) {
	// 해시테이블 초기화
	hash_init (&spt->page, page_hash, page_less, NULL);
}

// 헬퍼 함수 1: UNINIT 페이지 복사
static bool
copy_uninit_page(struct supplemental_page_table *dst, 
                 struct page *src_page, void *upage, bool writable) {
    vm_initializer *init = src_page->uninit.init;
    void *aux = src_page->uninit.aux;

    if (src_page->uninit.type & VM_FILE) {
        // FILE-BACKED UNINIT
        struct aux_info *src_aux = (struct aux_info *) aux;
        struct aux_info *dst_aux = malloc(sizeof(struct aux_info));
        if (dst_aux == NULL) return false;

        dst_aux->file = file_reopen(src_aux->file);
        if (dst_aux->file == NULL) {
            free(dst_aux);
            return false;
        }
        dst_aux->ofs = src_aux->ofs;
        dst_aux->read_bytes = src_aux->read_bytes;
        dst_aux->zero_bytes = src_aux->zero_bytes;

        if (!vm_alloc_page_with_initializer(src_page->uninit.type, upage, writable, init, dst_aux)) {
            file_close(dst_aux->file);
            free(dst_aux);
            return false;
        }
    } else {
        // ANON UNINIT
        if (!vm_alloc_page_with_initializer(VM_ANON, upage, writable, NULL, NULL)) {
            return false;
        }
    }
    return true;
}

// 헬퍼 함수 2: PRESENT 페이지 복사
static bool
copy_present_page(struct supplemental_page_table *dst,
                  struct page *src_page, void *upage, bool writable) {
    // 자식에게는 VM_ANON으로 할당
    if (!vm_alloc_page_with_initializer(VM_ANON, upage, writable, NULL, NULL)) {
        return false;
    }
    if (!vm_claim_page(upage)) {
        return false;
    }

    struct page *dst_page = spt_find_page(dst, upage);
    
    if (src_page->frame == NULL) {
        return false;
    }

    if (dst_page->frame != NULL) {
        memcpy(dst_page->frame->kva, src_page->frame->kva, PGSIZE);
    } else {
        return false;
    }

    return true;
}

/* Copy supplemental page table from src to dst */
bool
supplemental_page_table_copy(struct supplemental_page_table *dst,
                             struct supplemental_page_table *src) {
    struct hash_iterator i;
    hash_first(&i, &src->page);

    while (hash_next(&i)) {
        struct hash_elem *e = hash_cur(&i);
        struct page *src_page = hash_entry(e, struct page, hash_elem);
        enum vm_type type = src_page->operations->type;
        void *upage = src_page->va;
        bool writable = src_page->writable;

        if (type == VM_UNINIT) {
            if (!copy_uninit_page(dst, src_page, upage, writable))
                return false;
        } else {
            if (!copy_present_page(dst, src_page, upage, writable))
                return false;
        }
    }
    return true;
}

// 해시 테이블의 각 요소를 삭제할 때 호출될 함수
void
spt_destroy_page (struct hash_elem *e, void *aux UNUSED) {
	struct page *page = hash_entry (e, struct page, hash_elem);
	struct thread *cur = thread_current ();
	
	// 페이지 테이블에서 매핑을 끊어주기 -> frame이 할당된 경우
	if(page->frame != NULL){
		if(cur->pml4 != NULL){
			pml4_clear_page(cur->pml4, page->va);
		}
	}

	// 페이지 구조체 자체를 메모리에서 해제
	vm_dealloc_page(page);
}

/* Free the resource hold by the supplemental page table */
void
supplemental_page_table_kill (struct supplemental_page_table *spt UNUSED) {
	/* TODO: Destroy all the supplemental_page_table hold by thread and
	 * TODO: writeback all the modified contents to the storage. */
	hash_destroy(&spt->page, spt_destroy_page);
}
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
#ifdef EFILESYS
		case VM_PAGE_CACHE:
			page_initializer = page_cache_initializer;
			break;
#endif
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
}

/* Handle the fault on write_protected page */
static bool
vm_handle_wp (struct page *page UNUSED) {
}

/* Return true on success */
bool
vm_try_handle_fault (struct intr_frame *f UNUSED, void *addr UNUSED,
		bool user UNUSED, bool write UNUSED, bool not_present UNUSED) {
	struct supplemental_page_table *spt = &thread_current ()->spt;
	struct page *page;

	if (addr == NULL || is_kernel_vaddr (addr))
		return false;

	if (!not_present)
		return false;

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

/* Copy supplemental page table from src to dst */
bool
supplemental_page_table_copy (struct supplemental_page_table *dst UNUSED, struct supplemental_page_table *src UNUSED) {
	struct hash_iterator i; // 책갈피인거임 (반복자)

	hash_first (&i, &src->page);

	while (hash_next (&i)){ // 더이상 책이 없을 때까지 계속 반복한다.
		struct hash_elem *e = hash_cur(&i);
		struct page *src_page = hash_entry(e, struct page, hash_elem);
		enum vm_type type = src_page->operations->type;
		void *upage = src_page->va;
		bool writable = src_page->writable;

		// UNINIT 페이지인 경우 (아직 물리 메모리에 안 올라온 페이지)
		if (type == VM_UNINIT) {
			// 부모의 초기화 정보를 그대로 가져와서 자식한테 주기
			vm_initializer *init =src_page->uninit.init;
			void *aux = src_page->uninit.aux;

			// aux가 파일 정보를 가지고 있다면, 상황에 따라 deep copy가 필요할 수 있다.
			// 일단 부모의 aux를 그대로 전달한다. -> 나중에 어떻게 로드해야 하는지에 대한 정보를 등록 
			if (!vm_alloc_page_with_initializer (src_page->uninit.type, upage, writable, init, aux)){
				return false;
			}
		} else { //이미 로드된 페이지인 경우
			// 자식 프로세스에도 같은 타입의 페이지 할당을 한다. -> uninit 상태로 생성된다.
			if(!vm_alloc_page_with_initializer (VM_ANON, upage, writable, NULL, NULL)){
                return false;
            }
			
			// 자식의 페이지를 즉시 물리 프레임과 매칭
			if(!vm_claim_page (upage)){ // 내부적으로 spt_find_page(dst, upage)를 수행한다.
				return false;
			}
			
			// 내용물 복사 -> 부모의 프레임 내용을 자식의 프레임으로 그대로 복사
			struct page *dst_page = spt_find_page(dst, upage);

			if (dst_page && src_page->frame) {
                memcpy (dst_page->frame->kva, src_page->frame->kva, PGSIZE);
            }
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
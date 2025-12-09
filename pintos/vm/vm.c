/* vm.c: Generic interface for virtual memory objects. */

#include "threads/malloc.h"
#include "threads/vaddr.h"
#include "threads/mmu.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "hash.h"
#include "vm/vm.h"
#include "vm/inspect.h"
#include "filesys/file.h"
#include "threads/synch.h"

extern struct lock filesys_lock;

struct list frame_table;
struct lock frame_lock;
static bool frame_system_ready;

#define STACK_MAX_BYTES      (1 << 20)
#define STACK_GUARD_BYTES    8

static bool setup_uninit_page(struct page *page, enum vm_type type,
							 void *upage, bool writable,
							 vm_initializer *init, void *aux);
static bool register_new_page(struct supplemental_page_table *spt,
							  struct page *page);
static bool try_stack_growth(struct intr_frame *f, void *addr, bool user);
static bool stack_growth_allowed(void *upage);
static bool should_grow_stack(const struct intr_frame *f, void *addr, bool user);
static bool page_write_permitted(struct page *page, bool write);
static struct frame *allocate_frame_struct(void *kva);
static void track_frame_allocation(struct frame *frame);
static bool install_page_in_pml4(struct page *page, struct frame *frame);
static void release_frame_resources(struct frame *frame);

/* 가상 메모리 하위 시스템을 초기화한다. */
void
vm_init (void) {
	vm_anon_init ();
	vm_file_init ();
#ifdef EFILESYS
	pagecache_init ();
#endif
	register_inspect_intr ();
	list_init (&frame_table);
	lock_init (&frame_lock);
	frame_system_ready = true;
}

/* 페이지 객체의 실제 타입을 반환한다. */
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

static struct frame *vm_get_victim (void);
static bool vm_do_claim_page (struct page *page);
static struct frame *vm_evict_frame (void);
static void spt_destroy_page (struct hash_elem *e, void *aux);

/* 초기화 정보를 포함한 페이지 객체를 생성한다. */
bool
vm_alloc_page_with_initializer (enum vm_type type, void *upage, bool writable,
		vm_initializer *init, void *aux) {

	ASSERT (VM_TYPE(type) != VM_UNINIT);

	struct supplemental_page_table *spt = &thread_current ()->spt;
	if (spt_find_page (spt, upage) != NULL)
		return false;

	struct page *page = malloc (sizeof *page);
	if (page == NULL)
		return false;

	if (!setup_uninit_page (page, type, upage, writable, init, aux) ||
			!spt_insert_page (spt, page)) {
		free (page);
		return false;
	}

	return true;
}

/* SPT에서 VA에 해당하는 페이지를 찾는다. */
struct page*
spt_find_page (struct supplemental_page_table *spt UNUSED, void *va UNUSED) {
	struct page page;
	struct hash_elem *elem;

	page.va = pg_round_down (va);
	elem = hash_find (&spt->page_hash, &page.e);

	return elem ? hash_entry (elem, struct page, e) : NULL;
}

/* PAGE를 SPT에 삽입한다. */
bool
spt_insert_page (struct supplemental_page_table *spt UNUSED,
		struct page *page UNUSED) {
	int succ = false;
	succ = hash_insert (&spt->page_hash, &page->e) == NULL;

	return succ;
}

/* UNINIT_PAGE를 TYPE과 INIT으로 세팅한다. */
static bool
setup_uninit_page (struct page *page, enum vm_type type, void *upage,
		bool writable, vm_initializer *init, void *aux) {
	bool (*page_initializer) (struct page *, enum vm_type, void *) = NULL;
	switch (VM_TYPE (type)) {
	case VM_ANON:
		page_initializer = anon_initializer;
		break;
	case VM_FILE:
		page_initializer = file_backed_initializer;
		break;
	default:
		return false;
	}

	uninit_new (page, upage, init, type, aux, page_initializer);
	page->writable = writable;
	return true;
}

/* SPT에서 PAGE를 제거하고 spt_destroy_page를 호출한다. */
void
spt_remove_page (struct supplemental_page_table *spt, struct page *page) {
	struct hash_elem *e;

	e = hash_delete (&spt->page_hash, &page->e);
	if (e != NULL)
		spt_destroy_page (e, NULL);
}

/* 추방 대상이 될 프레임을 선택한다 (FIFO 정책). */
static struct frame *
vm_get_victim (void) {
	lock_acquire (&frame_lock);
	if (list_empty (&frame_table)) {
		lock_release (&frame_lock);
		return NULL;
	}

	struct list_elem *e = list_pop_front (&frame_table);
	lock_release (&frame_lock);

	struct frame *victim = list_entry (e, struct frame, e);
	return victim;
}

/* 선택한 페이지를 스왑아웃하고 해당 프레임을 돌려준다. */
static struct frame *
vm_evict_frame (void) {
	struct frame *victim = vm_get_victim ();
	if (victim == NULL)
		return NULL;

	struct page *page = victim->page;
	if (page == NULL)
		return victim;

	/* VM_UNINIT 페이지는 swap_out이 없으므로 건너뜀 */
	if (page->operations->swap_out == NULL) {
		lock_acquire (&frame_lock);
		list_push_back (&frame_table, &victim->e);
		lock_release (&frame_lock);
		return vm_evict_frame ();  /* 다른 victim 찾기 */
	}

	if (!swap_out (page))
		return NULL;

	return victim;
}

/* 프레임을 확보하고 프레임 테이블에 등록한다*/
static struct frame *
vm_get_frame (void) {
	ASSERT (frame_system_ready);
	void *kva = palloc_get_page (PAL_USER);
	if (kva == NULL) {
		struct frame *frame = vm_evict_frame ();
		if (frame == NULL)
			PANIC ("Failed to evict frame");
		/* eviction 후 프레임 재사용을 위해 테이블에 다시 등록 */
		frame->page = NULL;
		track_frame_allocation (frame);
		return frame;
	}

	struct frame *frame = allocate_frame_struct (kva);
	if (frame == NULL) {
		palloc_free_page (kva);
		PANIC ("Failed to allocate frame metadata");
	}

	track_frame_allocation (frame);
	return frame;
}

/* 프레임 메타데이터 구조체를 생성한다. */
static struct frame *
allocate_frame_struct (void *kva) {
	struct frame *frame = malloc (sizeof *frame);
	if (frame == NULL)
		return NULL;

	frame->kva = kva;
	frame->page = NULL;
	return frame;
}

/* 프레임 테이블에 새 프레임을 등록한다. */
static void
track_frame_allocation (struct frame *frame) {
	lock_acquire (&frame_lock);
	list_push_back (&frame_table, &frame->e);
	lock_release (&frame_lock);
}

/* PML4에 페이지-프레임을 연결(매핑)한다. */
static bool
install_page_in_pml4 (struct page *page, struct frame *frame) {
	return pml4_set_page (thread_current ()->pml4, page->va,
					      frame->kva, page->writable);
}

/* 프레임이 점유한 리소스를 해제한다. */
static void
release_frame_resources (struct frame *frame) {
	palloc_free_page (frame->kva);
	free (frame);
}


/* 스택 확장을 위해 새로운 페이지를 확보한다. */
static bool
vm_stack_growth (void *addr) {
	void *page_bottom = pg_round_down (addr);
	if (!vm_alloc_page (VM_ANON, page_bottom, true))
		return false;
	return vm_claim_page (page_bottom);
}

/* 쓰기 보호 위반을 처리한다. */
static bool
vm_handle_wp (struct page *page UNUSED) {
}

/* 페이지 폴트를 처리한다. */
bool
vm_try_handle_fault (struct intr_frame *f UNUSED, void *addr UNUSED, bool user UNUSED, bool write UNUSED, bool not_present UNUSED) {
	if (addr == NULL || !is_user_vaddr (addr))
		return false;

	struct supplemental_page_table *spt = &thread_current ()->spt;
	struct page *page = spt_find_page (spt, addr);
	
	if (page == NULL) {
		if (!not_present)
			return false;
		return try_stack_growth (f, addr, user);
	}

	/* 페이지가 존재하는 경우 쓰기 권한 검사 */
	if (!page_write_permitted (page, write))
		return false;

	/* not_present인 경우에만 claim 시도 */
	if (not_present)
		return vm_do_claim_page (page);

	return false;  /* present 페이지에 fault = 처리 불가능한 에러 */
}

/* 쓰기 접근이 허용되는지 확인한다. */
static bool
page_write_permitted (struct page *page, bool write) {
	return !write || page->writable;
}

/* 스택 성장 요건을 만족하는지 판단하고 확장을 시도한다. */
static bool
try_stack_growth (struct intr_frame *f, void *addr, bool user) {
	if (!should_grow_stack (f, addr, user))
		return false;

	void *upage = pg_round_down (addr);
	if (!stack_growth_allowed (upage))
		return false;

	return vm_stack_growth (addr);
}

/* 스택 접근이 성장 가능한 범위인지 확인한다. */
static bool
should_grow_stack (const struct intr_frame *f, void *addr, bool user) {
	uintptr_t stack_pointer = user ? f->rsp : thread_current()->user_rsp;
	return addr >= stack_pointer - STACK_GUARD_BYTES && addr < USER_STACK;
}

/* 최대 스택 크기 제한을 넘지 않는지 확인한다. */
static bool
stack_growth_allowed (void *upage) {
	uintptr_t distance = USER_STACK - (uintptr_t) upage;
	return distance <= STACK_MAX_BYTES;
}

/* 페이지 메모리를 해제한다. */
void
vm_dealloc_page (struct page *page) {
	destroy (page);
	free (page);
}

/* 주어진 VA에 대응하는 페이지를 할당받는다. */
bool
vm_claim_page (void *va) {
	struct page *page = spt_find_page (&thread_current ()->spt, va);
	if (page == NULL)
		return false;

	return vm_do_claim_page (page);
}

/* 페이지를 프레임에 연결하고 PML4에 등록한다. */
static bool
vm_do_claim_page (struct page *page) {
	struct frame *frame = vm_get_frame ();

	frame->page = page;
	page->frame = frame;

	if (!install_page_in_pml4 (page, frame)) {
		frame->page = NULL;
		page->frame = NULL;
		release_frame_resources (frame);
		return false;
	}

	return swap_in (page, frame->kva);
}

/* 페이지 해시 테이블 정렬을 위한 비교 함수. */
bool 
page_less (const struct hash_elem *a, const struct hash_elem *b, void *aux) {
	struct page *page_a = hash_entry(a, struct page, e);
	struct page *page_b = hash_entry(b, struct page, e);

	return page_a->va < page_b->va;
}

/* 페이지 해시 테이블에서 사용할 해시 함수. */
uint64_t
page_hash (const struct hash_elem *e, void *aux) {
	struct page *page = hash_entry(e, struct page, e);
	return hash_bytes(&page->va, sizeof(page->va));
}

/* 새로운 SPT를 초기화한다. */
void
supplemental_page_table_init (struct supplemental_page_table *spt UNUSED) {
	hash_init(&spt->page_hash, page_hash, page_less, NULL);
}

/* SPT 복사에 필요한 헬퍼 선언 */
static bool copy_page_entry(struct supplemental_page_table *dst,
                            struct page *src_page);
static bool copy_uninit_page_entry(struct supplemental_page_table *dst,
                                   struct page *src_page);
static bool copy_normal_page_entry(struct supplemental_page_table *dst,
                                   struct page *src_page,
                                   enum vm_type type);
static bool clone_frame_if_present(struct supplemental_page_table *dst,
                                   struct page *src_page);

/* TODO: 필요하면 aux deep copy용 헬퍼를 구현해라.
   예: static void *dup_lazy_aux(void *aux); */

/* 전체 SPT를 다른 테이블로 복제한다. */
bool
supplemental_page_table_copy(struct supplemental_page_table *dst,
                             struct supplemental_page_table *src)
{
    struct hash_iterator iter;
    hash_first(&iter, &src->page_hash);

    while (hash_next(&iter)) {
        struct page *src_page = hash_entry(hash_cur(&iter), struct page, e);

        if (!copy_page_entry(dst, src_page)) {
            supplemental_page_table_kill(dst);
            return false;
        }
    }

    return true;
}

/* 단일 페이지 엔트리를 복제한다. */
static bool
copy_page_entry(struct supplemental_page_table *dst, struct page *src_page)
{
    enum vm_type type = page_get_type(src_page);

    if (VM_TYPE(type) == VM_UNINIT) {
        if (!copy_uninit_page_entry(dst, src_page))
            return false;
    } else {
        if (!copy_normal_page_entry(dst, src_page, type))
            return false;
    }

    if (!clone_frame_if_present(dst, src_page))
        return false;

    return true;
}

/* UNINIT 페이지 메타데이터를 복제한다. */
static bool
copy_uninit_page_entry(struct supplemental_page_table *dst,
                       struct page *src_page)
{
    struct uninit_page *u = &src_page->uninit;
    void *va = src_page->va;

	/* aux 깊은 복사 */
	void *new_aux = NULL;
	if (u->aux != NULL) {
		struct lazy_aux *orig_aux = (struct lazy_aux *) u->aux;
		struct lazy_aux *dup_aux = malloc(sizeof(struct lazy_aux));
		if (dup_aux == NULL)
			return false;
		
		/* 구조체 전체 복사 */
		memcpy(dup_aux, orig_aux, sizeof(struct lazy_aux));
		
		/* file 포인터는 file_reopen으로 독립적인 파일 객체 생성 */
		if (orig_aux->file != NULL) {
			lock_acquire(&filesys_lock);
			dup_aux->file = file_reopen(orig_aux->file);
			lock_release(&filesys_lock);
			
			if (dup_aux->file == NULL) {
				free(dup_aux);
				return false;
			}
		}
		new_aux = dup_aux;
	}

	return vm_alloc_page_with_initializer(u->type, va, src_page->writable,
										  u->init, new_aux);
}

/* 확정된 타입의 페이지 엔트리를 복제한다. */
static bool
copy_normal_page_entry(struct supplemental_page_table *dst,
					   struct page *src_page,
					   enum vm_type type) {
    return vm_alloc_page(type, src_page->va, src_page->writable);
}

/* 이미 로드된 페이지라면 프레임 내용을 그대로 복제한다. */
static bool
clone_frame_if_present(struct supplemental_page_table *dst, struct page *src_page) {

    if (src_page->frame == NULL) {
        if (src_page->operations->type == VM_ANON && src_page->anon.swap_slot != BITMAP_ERROR) {
            if (!vm_do_claim_page (src_page)) return false;
        } else {
            return true;
        }
    }

    struct page *dst_page = spt_find_page(dst, src_page->va);
    if (dst_page == NULL)
        return false; 

    if (!vm_do_claim_page(dst_page))
        return false;

    memcpy(dst_page->frame->kva, src_page->frame->kva, PGSIZE);

    return true;
}


/* SPT가 보유한 모든 리소스를 해제한다. */
void
supplemental_page_table_kill (struct supplemental_page_table *spt UNUSED) {
	hash_destroy (&spt->page_hash, spt_destroy_page);
}

/* pml4에서 PAGE제거 하고 PAGE 메모리 해제 */
static void
spt_destroy_page (struct hash_elem *e, void *aux UNUSED) {
	struct page *page = hash_entry (e, struct page, e);
	struct thread *curr = thread_current ();

	/* destroy를 먼저 호출해서 dirty page write back 처리 */
	destroy (page);

	if (page->frame != NULL) {
		pml4_clear_page (curr->pml4, page->va);
		lock_acquire (&frame_lock);
		list_remove (&page->frame->e);
		lock_release (&frame_lock);
		palloc_free_page (page->frame->kva);
		free (page->frame);
		page->frame = NULL;
	}
	free (page);
}

/* 지연 로딩에 필요한 파일 정보를 복제한다. */
void*
set_lazy_aux(struct file* file, off_t ofs, size_t read_bytes, size_t zero_bytes, bool writable) {
	struct lazy_aux *lazy_aux = malloc(sizeof(struct lazy_aux));
	lazy_aux->file = file_reopen(file);
	lazy_aux->ofs = ofs;
	lazy_aux->read_bytes = read_bytes;
	lazy_aux->zero_bytes = zero_bytes;
	lazy_aux->writable = writable;

	return lazy_aux;
}

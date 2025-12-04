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

struct list frame_table;
struct lock frame_lock;
static bool frame_system_ready;

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
	list_init (&frame_table);
	lock_init (&frame_lock);
	frame_system_ready = true;
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
static void spt_destroy_page (struct hash_elem *e, void *aux);

/* Create the pending page object with initializer. If you want to create a
 * page, do not create it directly and make it through this function or
 * `vm_alloc_page`. 
 * 페이지 생성 함수*/
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
struct page*
spt_find_page (struct supplemental_page_table *spt UNUSED, void *va UNUSED) {
	struct page page;
	struct hash_elem *elem;

	page.va = pg_round_down (va);
	elem = hash_find (&spt->page_hash, &page.e);

	return elem ? hash_entry (elem, struct page, e) : NULL;
}

/* Insert PAGE into spt with validation. */
bool
spt_insert_page (struct supplemental_page_table *spt UNUSED,
		struct page *page UNUSED) {
	int succ = false;
	succ = hash_insert (&spt->page_hash, &page->e) == NULL;

	return succ;
}

void
spt_remove_page (struct supplemental_page_table *spt, struct page *page) {
	struct hash_elem *e;

	e = hash_delete (&spt->page_hash, &page->e);
	if (e != NULL)
		spt_destroy_page (e, NULL);
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
	struct frame *frame = NULL;
	ASSERT (frame_system_ready);
	void *kva = palloc_get_page (PAL_USER);

if (kva == NULL)
        PANIC("todo: implement eviction");
	else {
		frame = malloc (sizeof *frame);
		if (frame == NULL) {
			palloc_free_page (kva);
			PANIC ("Failed to allocate frame metadata");
		}
		frame->kva = kva;
		frame->page = NULL;

		lock_acquire (&frame_lock);
		list_push_back (&frame_table, &frame->e);
		lock_release (&frame_lock);
	}

	ASSERT (frame != NULL);
	ASSERT (frame->page == NULL);
	return frame;
}


/* Growing the stack. */
static void
vm_stack_growth (void *addr UNUSED) {
	void *page_bottom = pg_round_down(addr);
	if (!vm_alloc_page(VM_ANON, page_bottom, true))
		return;
	vm_claim_page(page_bottom);
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
	void *upage = pg_round_down (addr);

	if (addr == NULL || is_kernel_vaddr (addr))
		return false;

	if (!not_present)
		return false;

	page = spt_find_page (spt, addr);
	if (page == NULL){
		if (!user)
			return false;
		if (addr >= f->rsp - 8 && addr < USER_STACK){
			if ((USER_STACK - (uintptr_t) upage) > (1 << 20))
				return false;

			vm_stack_growth (addr);
			return true;
		}
		return false;
	}

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
vm_claim_page (void *va) {
	struct page *page = spt_find_page (&thread_current ()->spt, va);
	if (page == NULL)
		return false;

	return vm_do_claim_page (page);
}

/* Claim the PAGE and set up the mmu. */
static bool
vm_do_claim_page (struct page *page) {
	struct frame *frame = vm_get_frame ();

	/* Set links */
	frame->page = page;
	page->frame = frame;

	/* Map user page to the allocated frame. */
	if (!pml4_set_page (thread_current ()->pml4, page->va, frame->kva,
		page->writable)) {
		frame->page = NULL;
		page->frame = NULL;
		palloc_free_page (frame->kva);
		free (frame);
		return false;
	}

	return swap_in (page, frame->kva);
}

bool 
page_less (const struct hash_elem *a, const struct hash_elem *b, void *aux) {
	struct page *page_a = hash_entry(a, struct page, e);
	struct page *page_b = hash_entry(b, struct page, e);

	return page_a->va < page_b->va;
}

uint64_t
page_hash (const struct hash_elem *e, void *aux) {
	struct page *page = hash_entry(e, struct page, e);
	return hash_bytes(&page->va, sizeof(page->va));
}

/* Initialize new supplemental page table */
void
supplemental_page_table_init (struct supplemental_page_table *spt UNUSED) {
	hash_init(&spt->page_hash, page_hash, page_less, NULL);
}

/* Copy supplemental page table from src to dst */
bool
supplemental_page_table_copy (struct supplemental_page_table *dst UNUSED,
		struct supplemental_page_table *src UNUSED) {
	struct hash_iterator iter;
	hash_first (&iter, &src->page_hash);

	while (hash_next (&iter)) {
		struct page *src_page = hash_entry (hash_cur (&iter), struct page, e);
		enum vm_type type = page_get_type (src_page);
		bool ok = false;

		if (type == VM_UNINIT) {
			struct uninit_page *u = &src_page->uninit;
			ok = vm_alloc_page_with_initializer (u->type, src_page->va,
				src_page->writable, u->init, u->aux);
		} else {
			ok = vm_alloc_page (type, src_page->va, src_page->writable);
		}

		if (!ok)
			goto err;

		/* If the source page is already in memory, clone its contents. */
		if (src_page->frame != NULL) {
			struct page *dst_page = spt_find_page (dst, src_page->va);
			if (dst_page == NULL || !vm_do_claim_page (dst_page))
				goto err;
			memcpy (dst_page->frame->kva, src_page->frame->kva, PGSIZE);
		}
	}
	return true;
err:
	supplemental_page_table_kill (dst);
	return false;
}

/* Free the resource hold by the supplemental page table */
void
supplemental_page_table_kill (struct supplemental_page_table *spt UNUSED) {
	hash_destroy (&spt->page_hash, spt_destroy_page);
}

static void
spt_destroy_page (struct hash_elem *e, void *aux UNUSED) {
	struct page *page = hash_entry (e, struct page, e);
	struct thread *curr = thread_current ();

	if (page->frame != NULL) {
		if (pml4_is_dirty (curr->pml4, page->va))
			swap_out (page);
		pml4_clear_page (curr->pml4, page->va);
	}
	vm_dealloc_page (page);
}

void*
set_lazy_aux(struct file* file, off_t ofs, size_t read_bytes, size_t zero_bytes,
		bool writable) {
	struct lazy_aux *lazy_aux = malloc(sizeof(struct lazy_aux));
	lazy_aux->file = file_reopen(file);
	lazy_aux->ofs = ofs;
	lazy_aux->read_bytes = read_bytes;
	lazy_aux->zero_bytes = zero_bytes;
	lazy_aux->writable = writable;

	return lazy_aux;
}

/* anon.c: Implementation of page for non-disk image (a.k.a. anonymous page). */

#include "vm/vm.h"
#include "devices/disk.h"
#include "threads/vaddr.h"
#include "threads/synch.h"
#include "bitmap.h"
#include <string.h>

#define SECTORS_PER_PAGE (PGSIZE / DISK_SECTOR_SIZE)

/* DO NOT MODIFY BELOW LINE */
static struct disk *swap_disk;
static struct bitmap *swap_table;
static struct lock swap_lock;

static bool anon_swap_in (struct page *page, void *kva);
static bool anon_swap_out (struct page *page);
static void anon_destroy (struct page *page);

/* DO NOT MODIFY this struct */
static const struct page_operations anon_ops = {
	.swap_in = anon_swap_in,
	.swap_out = anon_swap_out,
	.destroy = anon_destroy,
	.type = VM_ANON,
};

/* 스왑 디스크와 스왑 테이블(비트맵)을 초기화한다. */
void
vm_anon_init (void) {
	swap_disk = disk_get (1, 1);
	if (swap_disk == NULL)
		PANIC ("No swap disk found");

	size_t swap_slot_cnt = disk_size (swap_disk) / SECTORS_PER_PAGE;
	swap_table = bitmap_create (swap_slot_cnt);
	if (swap_table == NULL)
		PANIC ("Failed to create swap table bitmap");

	lock_init (&swap_lock);
}

/* 익명 페이지 초기화; 스왑 슬롯은 아직 미할당. */
bool
anon_initializer (struct page *page, enum vm_type type, void *kva) {
	page->operations = &anon_ops;
	struct anon_page *anon_page = &page->anon;
	anon_page->swap_slot = BITMAP_ERROR;

	memset (kva, 0, PGSIZE);
	return true;
}

/* 스왑 디스크에서 페이지 내용을 읽어 kva에 복원한다. */
static bool
anon_swap_in (struct page *page, void *kva) {
	struct anon_page *anon_page = &page->anon;
	size_t slot_idx = anon_page->swap_slot;

	if (slot_idx == BITMAP_ERROR) {
		memset (kva, 0, PGSIZE);
		return true;
	}

	disk_sector_t start_sector = slot_idx * SECTORS_PER_PAGE;
	for (size_t i = 0; i < SECTORS_PER_PAGE; i++)
		disk_read (swap_disk, start_sector + i, kva + i * DISK_SECTOR_SIZE);

	lock_acquire (&swap_lock);
	bitmap_reset (swap_table, slot_idx);
	lock_release (&swap_lock);

	anon_page->swap_slot = BITMAP_ERROR;
	return true;
}

/* 페이지 내용을 스왑 디스크에 쓰고 슬롯 인덱스를 저장한다. */
static bool
anon_swap_out (struct page *page) {
	struct anon_page *anon_page = &page->anon;

	lock_acquire (&swap_lock);
	size_t slot_idx = bitmap_scan_and_flip (swap_table, 0, 1, false);
	lock_release (&swap_lock);

	if (slot_idx == BITMAP_ERROR)
		return false;

	disk_sector_t start_sector = slot_idx * SECTORS_PER_PAGE;
	void *kva = page->frame->kva;
	for (size_t i = 0; i < SECTORS_PER_PAGE; i++)
		disk_write (swap_disk, start_sector + i, kva + i * DISK_SECTOR_SIZE);

	anon_page->swap_slot = slot_idx;

	page->frame->page = NULL;
	page->frame = NULL;
	pml4_clear_page (thread_current ()->pml4, page->va);

	return true;
}

/* 페이지 파괴 시 스왑 슬롯 반환. */
static void
anon_destroy (struct page *page) {
	struct anon_page *anon_page = &page->anon;

	if (anon_page->swap_slot != BITMAP_ERROR) {
		lock_acquire (&swap_lock);
		bitmap_reset (swap_table, anon_page->swap_slot);
		lock_release (&swap_lock);
	}
}

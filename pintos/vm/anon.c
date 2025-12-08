/* anon.c: Implementation of page for non-disk image (a.k.a. anonymous page). */

#include "vm/vm.h"
#include "devices/disk.h"
#include "threads/vaddr.h"
#include <string.h>

/* DO NOT MODIFY BELOW LINE */
static struct disk *swap_disk;
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

static struct list free_swap_list;
static struct lock swap_lock;

struct swap_slot {
	struct list_elem elem;
	size_t slot_index;
};

/* Initialize the data for anonymous pages */
void
vm_anon_init (void) {
	/* TODO: Set up the swap_disk. */
	swap_disk = disk_get (1, 1);
	list_init (&free_swap_list);
	lock_init (&swap_lock);

	size_t total_slots = disk_size(swap_disk)/ (PGSIZE / DISK_SECTOR_SIZE);

	for (size_t i = 0; i < total_slots; i++){
		struct swap_slot *slot = malloc(sizeof (struct swap_slot));
		if (slot == NULL) {
			PANIC("슬롯 부족");
		}
		slot->slot_index = i;
		list_push_back(&free_swap_list, &slot->elem);
	}
}

// 스왑 디스크에서 사용 가능한 slot 할당
static int alloc_swap_slot (void) {
	lock_acquire(&swap_lock);
	
	if (list_empty(&free_swap_list)) {
		lock_release(&swap_lock);
		return -1;  // 스왑 공간 부족
	}
	
	struct swap_slot *slot = list_entry(list_pop_front(&free_swap_list),struct swap_slot, elem);
	
	size_t slot_index = slot->slot_index;
	free(slot);
	
	lock_release(&swap_lock);
	return slot_index;
}

// 사용한 slot 프리 리스트에 반환
static void free_swap_slot (int slot_index) {
	struct swap_slot *slot = malloc(sizeof(struct swap_slot));
	slot->slot_index = slot_index;
	
	lock_acquire(&swap_lock);
	list_push_back(&free_swap_list, &slot->elem);
	lock_release(&swap_lock);
}

/* Initialize the file mapping */
bool
anon_initializer (struct page *page, enum vm_type type, void *kva) {
	/* Set up the handler */
	page->operations = &anon_ops;
	struct anon_page *anon_page UNUSED = &page->anon;
    /* Stack pages have no initializer, so provide zero-filled memory. */
	// 초기화 시 스왑 인덱스는 -1
    anon_page->swap_index = -1; // 아직 스왑 아웃 안함

    return true;
}

/* Swap in the page by read contents from the swap disk. */
static bool
anon_swap_in (struct page *page, void *kva) {
	struct anon_page *anon_page UNUSED = &page->anon;
    
	if((anon_page->swap_index == -1)){
		memset(kva, 0, PGSIZE);
		return true;
	}

	int slot_index = anon_page->swap_index;

	if (slot_index < 0 || slot_index >= disk_size(swap_disk) / (PGSIZE / DISK_SECTOR_SIZE)) {
		return false;
	}

	disk_sector_t sector = slot_index * (PGSIZE / DISK_SECTOR_SIZE);
	
	for(int i = 0; i < PGSIZE / DISK_SECTOR_SIZE; i++){
		disk_read(swap_disk, sector + i, (uint8_t *)kva + i * DISK_SECTOR_SIZE);
	}

	free_swap_slot(slot_index);
	anon_page->swap_index = -1;
	
    return true;
}

/* Swap out the page by writing contents to the swap disk. */
static bool
anon_swap_out (struct page *page) {
	struct anon_page *anon_page UNUSED = &page->anon;

	if (page->frame == NULL) {
		return false;
	}
	// 사용 가능한 스왑 슬롯 할당
	int slot_index = alloc_swap_slot();
	if(slot_index == -1) return false;

	// 메모리 내용을 디스크에 쓰기
	uint8_t *kva = page->frame->kva;
	disk_sector_t sector = slot_index * (PGSIZE / DISK_SECTOR_SIZE);

	for (int i = 0; i < PGSIZE / DISK_SECTOR_SIZE; i++) {
		disk_write(swap_disk, sector + i, (uint8_t *)kva + i * DISK_SECTOR_SIZE);
	}

	anon_page->swap_index = slot_index;
    return true;
}

/* Destroy the anonymous page. PAGE will be freed by the caller. */
static void
anon_destroy (struct page *page) {
	struct anon_page *anon_page UNUSED = &page->anon;
		
	// 스왑 디스크에 있는 경우 슬롯 해제
	if (anon_page->swap_index != -1) {
		free_swap_slot(anon_page->swap_index);
			anon_page->swap_index = -1;  // 중복 해제 방지
	}
}

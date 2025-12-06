#ifndef VM_ANON_H
#define VM_ANON_H
#include "vm/vm.h"
struct page;
enum vm_type;

struct anon_page {
    // 스왑 영역에서의 위치 인덱스
    // 메모리에 있을때는 -1
    // 스왑 아웃 되면 해당 슬록 번호를 가진다.
    int swap_index;
};

void vm_anon_init (void);
bool anon_initializer (struct page *page, enum vm_type type, void *kva);

#endif

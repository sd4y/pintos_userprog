#ifndef __LIB_KERNEL_LIST_H
#define __LIB_KERNEL_LIST_H

/* 이중 연결 리스트.
 *
 * 이 이중 연결 리스트 구현은 동적 메모리 할당을 필요로 하지 않습니다.
 * 대신, 리스트의 요소가 될 수 있는 각 구조체는 struct list_elem 멤버를 포함해야 합니다.
 * 모든 리스트 함수는 이러한 `struct list_elem`을 대상으로 동작합니다.
 * list_entry 매크로를 사용하면 struct list_elem에서 이를 포함하는 구조체 객체로 변환할 수 있습니다.

 * 예를 들어, `struct foo`로 이루어진 리스트가 필요하다고 가정하면,
 * `struct foo`는 다음과 같이 `struct list_elem` 멤버를 포함해야 합니다:

 * struct foo {
 *   struct list_elem elem;
 *   int bar;
 *   ...다른 멤버들...
 * };

 * 그리고 `struct foo` 리스트는 다음과 같이 선언 및 초기화할 수 있습니다:

 * struct list foo_list;

 * list_init (&foo_list);

 * 반복(iteration)은 struct list_elem에서 이를 포함하는 구조체로 변환이 필요한 대표적인 상황입니다.
 * foo_list를 사용하는 예시는 다음과 같습니다:

 * struct list_elem *e;

 * for (e = list_begin (&foo_list); e != list_end (&foo_list);
 *      e = list_next (e)) {
 *   struct foo *f = list_entry (e, struct foo, elem);
 *   ...f로 작업 수행...
 * }

 * 실제 리스트 사용 예시는 소스 곳곳에서 볼 수 있습니다.
 * 예를 들어, threads 디렉토리의 malloc.c, palloc.c, thread.c 등이 리스트를 사용합니다.

 * 이 리스트의 인터페이스는 C++ STL의 list<> 템플릿에서 영감을 받았습니다.
 * list<>에 익숙하다면 쉽게 사용할 수 있습니다.
 * 하지만 이 리스트는 타입 체크를 전혀 하지 않으며, 다른 올바름 검증도 거의 하지 않습니다.
 * 실수하면 문제가 발생할 수 있습니다.

 * 리스트 용어 정리:

 * - "front": 리스트의 첫 번째 요소. 리스트가 비어 있으면 정의되지 않음. list_front()가 반환.

 * - "back": 리스트의 마지막 요소. 리스트가 비어 있으면 정의되지 않음. list_back()이 반환.

 * - "tail": 리스트의 마지막 요소 바로 뒤에 있는 요소(가상적). 리스트가 비어 있어도 잘 정의됨.
 *   list_end()가 반환. front에서 back으로 반복할 때 종료 지점으로 사용.

 * - "beginning": 리스트가 비어 있지 않으면 front, 비어 있으면 tail. list_begin()이 반환.
 *   front에서 back으로 반복할 때 시작 지점으로 사용.

 * - "head": 리스트의 첫 번째 요소 바로 앞에 있는 요소(가상적). 리스트가 비어 있어도 잘 정의됨.
 *   list_rend()가 반환. back에서 front로 반복할 때 종료 지점으로 사용.

 * - "reverse beginning": 리스트가 비어 있지 않으면 back, 비어 있으면 head. list_rbegin()이 반환.
 *   back에서 front로 반복할 때 시작 지점으로 사용.
 *
 * - "interior element": head나 tail이 아닌 실제 리스트 요소. 빈 리스트에는 interior element가 없음.*/

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* List element. */
struct list_elem {
	struct list_elem *prev;     /* Previous list element. */
	struct list_elem *next;     /* Next list element. */
};

/* List. */
struct list {
	struct list_elem head;      /* List head. */
	struct list_elem tail;      /* List tail. */
};

/* 리스트 요소 LIST_ELEM의 포인터를 LIST_ELEM이 포함된 구조체의 포인터로 변환합니다.
   외부 구조체의 이름(STRUCT)과 리스트 요소 멤버 이름(MEMBER)을 지정해야 합니다.
   예시는 파일 상단의 큰 주석을 참고하세요. */
#define list_entry(LIST_ELEM, STRUCT, MEMBER)           \
	((STRUCT *) ((uint8_t *) &(LIST_ELEM)->next     \
		- offsetof (STRUCT, MEMBER.next)))

void list_init (struct list *);

/* List traversal. */
struct list_elem *list_begin (struct list *);
struct list_elem *list_next (struct list_elem *);
struct list_elem *list_end (struct list *);

struct list_elem *list_rbegin (struct list *);
struct list_elem *list_prev (struct list_elem *);
struct list_elem *list_rend (struct list *);

struct list_elem *list_head (struct list *);
struct list_elem *list_tail (struct list *);

/* List insertion. */
void list_insert (struct list_elem *, struct list_elem *);
void list_splice (struct list_elem *before,
		struct list_elem *first, struct list_elem *last);
void list_push_front (struct list *, struct list_elem *);
void list_push_back (struct list *, struct list_elem *);

/* List removal. */
struct list_elem *list_remove (struct list_elem *);
struct list_elem *list_pop_front (struct list *);
struct list_elem *list_pop_back (struct list *);

/* List elements. */
struct list_elem *list_front (struct list *);
struct list_elem *list_back (struct list *);

/* List properties. */
size_t list_size (struct list *);
bool list_empty (struct list *);

/* Miscellaneous. */
void list_reverse (struct list *);

/* 두 리스트 요소 A와 B의 값을 비교합니다.
   보조 데이터 AUX를 받아서, A가 B보다 작으면 true를 반환하고
   그렇지 않으면 false를 반환합니다. */
typedef bool list_less_func (const struct list_elem *a,
                             const struct list_elem *b,
                             void *aux);

/* 정렬된 요소를 가진 리스트에 대한 연산. */
void list_sort (struct list *,
                list_less_func *, void *aux);
void list_insert_ordered (struct list *, struct list_elem *,
                          list_less_func *, void *aux);
void list_unique (struct list *, struct list *duplicates,
                  list_less_func *, void *aux);

/* Max and min. */
struct list_elem *list_max (struct list *, list_less_func *, void *aux);
struct list_elem *list_min (struct list *, list_less_func *, void *aux);

#endif /* lib/kernel/list.h */

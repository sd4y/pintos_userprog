#include "list.h"
#include "../debug.h"

/* 이중 연결 리스트는 두 개의 헤더 요소를 가집니다: 첫 번째 요소 바로 앞의 "head"와 마지막 요소 바로 뒤의 "tail"입니다.
	앞쪽 헤더의 `prev` 링크와 뒤쪽 헤더의 `next` 링크는 null입니다.
	나머지 두 링크는 리스트의 내부 요소를 통해 서로를 가리킵니다.

	빈 리스트는 다음과 같이 생겼습니다:

	+------+     +------+
	<---| head |<--->| tail |--->
	+------+     +------+

	두 개의 요소가 있는 리스트는 다음과 같습니다:

	+------+     +-------+     +-------+     +------+
	<---| head |<--->|   1   |<--->|   2   |<--->| tail |<--->
	+------+     +-------+     +-------+     +------+

	이러한 대칭 구조 덕분에 리스트 처리에서 많은 특수 케이스가 사라집니다.
	예를 들어, list_remove()를 보면 조건문 없이 포인터 두 개만 할당하면 됩니다.
	헤더 요소가 없었다면 훨씬 복잡해졌을 것입니다.

	(각 헤더 요소에서 실제로 하나의 포인터만 사용되므로, 두 헤더를 하나로 합칠 수도 있지만
	두 개의 요소를 사용하면 일부 연산에서 체크를 할 수 있어 더 유용합니다.) */

static bool is_sorted (struct list_elem *a, struct list_elem *b,
		list_less_func *less, void *aux) UNUSED;

/* ELEM이 head이면 true, 아니면 false를 반환합니다. */
static inline bool
is_head (struct list_elem *elem) {
	return elem != NULL && elem->prev == NULL && elem->next != NULL;
}

/* ELEM이 내부 요소면 true, 아니면 false를 반환합니다. */
static inline bool
is_interior (struct list_elem *elem) {
	return elem != NULL && elem->prev != NULL && elem->next != NULL;
}

/* ELEM이 tail이면 true, 아니면 false를 반환합니다. */
static inline bool
is_tail (struct list_elem *elem) {
	return elem != NULL && elem->prev != NULL && elem->next == NULL;
}

/* LIST를 빈 리스트로 초기화합니다. */
void
list_init (struct list *list) {
	ASSERT (list != NULL);
	list->head.prev = NULL;
	list->head.next = &list->tail;
	list->tail.prev = &list->head;
	list->tail.next = NULL;
}

/* LIST의 시작 요소를 반환합니다. */
struct list_elem *
list_begin (struct list *list) {
	ASSERT (list != NULL);
	return list->head.next;
}

/* ELEM의 다음 요소를 반환합니다.
	ELEM이 리스트의 마지막 요소라면 리스트의 tail을 반환합니다.
	ELEM이 tail인 경우 결과는 정의되지 않습니다. */
struct list_elem *
list_next (struct list_elem *elem) {
	ASSERT (is_head (elem) || is_interior (elem));
	return elem->next;
}

/* Returns LIST's tail.

	list_end()는 리스트를 앞에서 뒤로 순회할 때 자주 사용됩니다.
	예시는 list.h 상단의 큰 주석을 참고하세요. */
struct list_elem *
list_end (struct list *list) {
	ASSERT (list != NULL);
	return &list->tail;
}

/* LIST의 역방향 시작 요소를 반환합니다.
	리스트를 뒤에서 앞으로 순회할 때 사용됩니다. */
struct list_elem *
list_rbegin (struct list *list) {
	ASSERT (list != NULL);
	return list->tail.prev;
}

/* ELEM의 이전 요소를 반환합니다.
	ELEM이 리스트의 첫 번째 요소라면 리스트의 head를 반환합니다.
	ELEM이 head인 경우 결과는 정의되지 않습니다. */
struct list_elem *
list_prev (struct list_elem *elem) {
	ASSERT (is_interior (elem) || is_tail (elem));
	return elem->prev;
}

/* LIST의 head를 반환합니다.

	list_rend()는 리스트를 뒤에서 앞으로 순회할 때 자주 사용됩니다.
	사용 예시는 list.h 상단의 예시를 참고하세요:

	for (e = list_rbegin(&foo_list); e != list_rend(&foo_list);
		  e = list_prev(e))
	{
		 struct foo *f = list_entry(e, struct foo, elem);
		 ...f로 무언가를 수행...
	}
*/
struct list_elem *
list_rend (struct list *list) {
	ASSERT (list != NULL);
	return &list->head;
}

/* Return's LIST's head.

	list_head()는 리스트를 순회하는 다른 방식에
	사용할 수 있습니다, e.g.:

   e = list_head (&list);
   while ((e = list_next (e)) != list_end (&list))
   {
   ...
   }
   */
struct list_elem *
list_head (struct list *list) {
	ASSERT (list != NULL);
	return &list->head;
}

/* Return's LIST's tail. */
struct list_elem *
list_tail (struct list *list) {
	ASSERT (list != NULL);
	return &list->tail;
}

/* ELEM을 BEFORE 바로 앞에 삽입합니다. BEFORE는 내부 요소이거나 tail일 수 있습니다.
	후자의 경우는 list_push_back()과 동일합니다. */
void
list_insert (struct list_elem *before, struct list_elem *elem) {
	ASSERT (is_interior (before) || is_tail (before));
	ASSERT (elem != NULL);

	elem->prev = before->prev;
	elem->next = before;
	before->prev->next = elem;
	before->prev = elem;
}

/* FIRST부터 LAST(마지막은 제외)까지의 요소를 현재 리스트에서 제거한 뒤,
	BEFORE 바로 앞에 삽입합니다. BEFORE는 내부 요소이거나 tail일 수 있습니다. */
void
list_splice (struct list_elem *before,
		struct list_elem *first, struct list_elem *last) {
	ASSERT (is_interior (before) || is_tail (before));
	if (first == last)
		return;
	last = list_prev (last);

	ASSERT (is_interior (first));
	ASSERT (is_interior (last));

	/* FIRST부터 LAST까지의 구간을 현재 리스트에서 깔끔하게 제거합니다. */
	first->prev->next = last->next;
	last->next->prev = first->prev;

	/* FIRST부터 LAST까지의 구간을 새로운 리스트에 연결합니다. */
	first->prev = before->prev;
	last->next = before;
	before->prev->next = first;
	before->prev = last;
}

/* ELEM을 LIST의 맨 앞에 삽입하여, LIST의 첫 번째 요소가 되게 합니다. */
void
list_push_front (struct list *list, struct list_elem *elem) {
	list_insert (list_begin (list), elem);
}

/* ELEM을 LIST의 맨 뒤에 삽입하여, LIST의 마지막 요소가 되게 합니다. */
void
list_push_back (struct list *list, struct list_elem *elem) {
	list_insert (list_end (list), elem);
}

/* ELEM을 리스트에서 제거하고, 그 다음 요소를 반환합니다.
   ELEM이 리스트에 속하지 않은 경우 동작은 정의되지 않습니다.

   ELEM을 리스트에서 제거한 후에는 ELEM을 리스트의 요소로 취급하면 안 됩니다.
   특히, 제거 후 list_next()나 list_prev()를 ELEM에 사용하면 정의되지 않은 동작이 발생합니다.
   따라서 리스트의 모든 요소를 제거하는 단순 반복문은 올바르지 않습니다:

 ** 이렇게 하면 안 됩니다 **
 for (e = list_begin(&list); e != list_end(&list); e = list_next(e)) {
	 ...e로 무언가를 수행...
	 list_remove(e);
 }
 ** 이렇게 하면 안 됩니다 **

 리스트의 요소를 올바르게 제거하는 방법은 다음과 같습니다:

 for (e = list_begin(&list); e != list_end(&list); e = list_remove(e)) {
	 ...e로 무언가를 수행...
 }

 만약 리스트의 요소를 free()해야 한다면 더 신중해야 합니다. 다음과 같은 방법이 있습니다:

 while (!list_empty(&list)) {
	 struct list_elem *e = list_pop_front(&list);
	 ...e로 무언가를 수행...
 }
*/
struct list_elem *
list_remove (struct list_elem *elem) {
	ASSERT (is_interior (elem));
	elem->prev->next = elem->next;
	elem->next->prev = elem->prev;
	return elem->next;
}

/* LIST의 맨 앞 요소를 제거하고 반환합니다.
	제거 전에 LIST가 비어 있으면 동작은 정의되지 않습니다. */
struct list_elem *
list_pop_front (struct list *list) {
	struct list_elem *front = list_front (list);
	list_remove (front);
	return front;
}

/* LIST의 맨 뒤 요소를 제거하고 반환합니다.
	제거 전에 LIST가 비어 있으면 동작은 정의되지 않습니다. */
struct list_elem *
list_pop_back (struct list *list) {
	struct list_elem *back = list_back (list);
	list_remove (back);
	return back;
}

/* LIST의 맨 앞 요소를 반환합니다.
	LIST가 비어 있으면 동작은 정의되지 않습니다. */
struct list_elem *
list_front (struct list *list) {
	ASSERT (!list_empty (list));
	return list->head.next;
}

/* LIST의 마지막(뒤쪽) 원소를 반환한다.
   LIST가 비어 있는 경우, 동작은 정의되지 않는다. */
struct list_elem *
list_back (struct list *list) {
	ASSERT (!list_empty (list));
	return list->tail.prev;
}

/* LIST에 포함된 요소의 개수를 반환합니다.
	요소 개수에 대해 O(n) 시간에 동작합니다. */
size_t
list_size (struct list *list) {
	struct list_elem *e;
	size_t cnt = 0;

	for (e = list_begin (list); e != list_end (list); e = list_next (e))
		cnt++;
	return cnt;
}

/* LIST가 비어 있으면 true를, 그렇지 않으면 false를 반환합니다. */
bool
list_empty (struct list *list) {
	return list_begin (list) == list_end (list);
}

/* A와 B가 가리키는 `struct list_elem *`를 서로 교환합니다. */
static void
swap (struct list_elem **a, struct list_elem **b) {
	struct list_elem *t = *a;
	*a = *b;
	*b = t;
}

/* LIST의 순서를 반대로 뒤집습니다. */
void
list_reverse (struct list *list) {
	if (!list_empty (list)) {
		struct list_elem *e;

		for (e = list_begin (list); e != list_end (list); e = e->prev)
			swap (&e->prev, &e->next);
		swap (&list->head.next, &list->tail.prev);
		swap (&list->head.next->prev, &list->tail.prev->next);
	}
}

/* 리스트의 원소 A부터 B 직전까지의 구간이,
   보조 데이터 AUX와 비교 함수 LESS에 따라
   올바른 순서로 정렬되어 있을 때만 true를 반환한다. */
static bool
is_sorted (struct list_elem *a, struct list_elem *b,
		list_less_func *less, void *aux) {
	if (a != b)
		while ((a = list_next (a)) != b)
			if (less (a, list_prev (a), aux))
				return false;
	return true;
}

/* 리스트의 구간 A에서 시작하여 B를 넘지 않는 범위 내에서,
   비교 함수 LESS와 보조 데이터 AUX를 기준으로
   **비감소(nondecreasing) 순서**로 정렬된 연속된 구간(run)을 찾는다.
   찾은 구간의 (끝 바로 다음 원소, 즉) 'exclusive end'를 반환한다.

   A부터 B 바로 앞까지의 구간은 반드시 비어 있지 않아야 한다.*/
static struct list_elem *
find_end_of_run (struct list_elem *a, struct list_elem *b,
		list_less_func *less, void *aux) {
	ASSERT (a != NULL);
	ASSERT (b != NULL);
	ASSERT (less != NULL);
	ASSERT (a != b);

	do {
		a = list_next (a);
	} while (a != b && !less (a, list_prev (a), aux));
	return a;
}

/* 구간 [A0, A1B0) 와 [A1B0, B1) 를 병합하여,
   끝이 B1(직전까지)인 하나의 결합된 구간을 만든다.
   두 입력 구간은 모두 비어 있지 않아야 하며,
   비교 함수 LESS와 보조 데이터 AUX를 기준으로
   비감소(nondecreasing) 순서로 정렬되어 있어야 한다.
   결과로 만들어지는 출력 구간도 동일한 기준으로 정렬된다. */
static void
inplace_merge (struct list_elem *a0, struct list_elem *a1b0,
		struct list_elem *b1,
		list_less_func *less, void *aux) {
	ASSERT (a0 != NULL);
	ASSERT (a1b0 != NULL);
	ASSERT (b1 != NULL);
	ASSERT (less != NULL);
	ASSERT (is_sorted (a0, a1b0, less, aux));
	ASSERT (is_sorted (a1b0, b1, less, aux));

	while (a0 != a1b0 && a1b0 != b1)
		if (!less (a1b0, a0, aux))
			a0 = list_next (a0);
		else {
			a1b0 = list_next (a1b0);
			list_splice (a0, list_prev (a1b0), a1b0);
		}
}

/* LIST를 비교 함수 LESS와 보조 데이터 AUX에 따라 정렬합니다.
	O(n lg n) 시간과 O(1) 공간에서 동작하는 자연 반복 병합 정렬(natural iterative merge sort)을 사용합니다. */
void
list_sort (struct list *list, list_less_func *less, void *aux) {
	size_t output_run_cnt;        /* Number of runs output in current pass. */

	ASSERT (list != NULL);
	ASSERT (less != NULL);

	  /* 리스트를 여러 번 순회하며, 인접한 비감소 구간(run)을 병합합니다.
		  단 하나의 구간만 남을 때까지 반복합니다. */
	do {
		struct list_elem *a0;     /* 첫 번째 구간(run)의 시작. */
		struct list_elem *a1b0;   /* 첫 번째 구간의 끝, 두 번째 구간의 시작. */
		struct list_elem *b1;     /* 두 번째 구간의 끝. */

		output_run_cnt = 0;
		for (a0 = list_begin (list); a0 != list_end (list); a0 = b1) {
			/* Each iteration produces one output run. */
			output_run_cnt++;

				/* 인접한 두 개의 비감소 구간(run)을 찾습니다
					A0...A1B0 및 A1B0...B1. */
			a1b0 = find_end_of_run (a0, list_end (list), less, aux);
			if (a1b0 == list_end (list))
				break;
			b1 = find_end_of_run (a1b0, list_end (list), less, aux);

			/* Merge the runs. */
			inplace_merge (a0, a1b0, b1, less, aux);
		}
	}
	while (output_run_cnt > 1);

	ASSERT (is_sorted (list_begin (list), list_end (list), less, aux));
}

/* LIST가 비교 함수 LESS와 보조 데이터 AUX에 따라 정렬되어 있다고 가정하고,
	ELEM을 적절한 위치에 삽입합니다.
	평균적으로 O(n) 시간에 동작합니다. */
void
list_insert_ordered (struct list *list, struct list_elem *elem,
		list_less_func *less, void *aux) {
	struct list_elem *e;

	ASSERT (list != NULL);
	ASSERT (elem != NULL);
	ASSERT (less != NULL);

	for (e = list_begin (list); e != list_end (list); e = list_next (e))
		if (less (elem, e, aux))
			break;
	return list_insert (e, elem);
}

/* LIST를 순회하며, 비교 함수 LESS와 보조 데이터 AUX에 따라 인접한 중복 원소 중 첫 번째만 남기고 나머지는 제거합니다.
	DUPLICATES가 NULL이 아니면, 제거된 원소들을 DUPLICATES 리스트에 추가합니다. */
void
list_unique (struct list *list, struct list *duplicates,
		list_less_func *less, void *aux) {
	struct list_elem *elem, *next;

	ASSERT (list != NULL);
	ASSERT (less != NULL);
	if (list_empty (list))
		return;

	elem = list_begin (list);
	while ((next = list_next (elem)) != list_end (list))
		if (!less (elem, next, aux) && !less (next, elem, aux)) {
			list_remove (next);
			if (duplicates != NULL)
				list_push_back (duplicates, next);
		} else
			elem = next;
}

/* LIST에서 비교 함수 LESS와 보조 데이터 AUX에 따라 가장 큰 값을 가진 원소를 반환합니다.
	최대값이 여러 개라면 리스트에서 더 앞에 있는 원소를 반환합니다.
	리스트가 비어 있으면 tail을 반환합니다. */
struct list_elem *
list_max (struct list *list, list_less_func *less, void *aux) {
	struct list_elem *max = list_begin (list);
	if (max != list_end (list)) {
		struct list_elem *e;

		for (e = list_next (max); e != list_end (list); e = list_next (e))
			if (less (max, e, aux))
				max = e;
	}
	return max;
}

/* LIST에서 비교 함수 LESS와 보조 데이터 AUX에 따라 가장 작은 값을 가진 원소를 반환합니다.
	최소값이 여러 개라면 리스트에서 더 앞에 있는 원소를 반환합니다.
	리스트가 비어 있으면 tail을 반환합니다. */
struct list_elem *
list_min (struct list *list, list_less_func *less, void *aux) {
	struct list_elem *min = list_begin (list);
	if (min != list_end (list)) {
		struct list_elem *e;

		for (e = list_next (min); e != list_end (list); e = list_next (e))
			if (less (e, min, aux))
				min = e;
	}
	return min;
}

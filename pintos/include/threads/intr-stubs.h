#ifndef THREADS_INTR_STUBS_H
#define THREADS_INTR_STUBS_H

/* 인터럽트 스텁.
 *
 * intr-stubs.S에 있는 작은 코드 조각들로, 256가지 x86 인터럽트 각각에 대해 하나씩 존재합니다.
 * 각 스텁은 스택을 약간 조작한 뒤 intr_entry()로 점프합니다.
 * 자세한 내용은 intr-stubs.S를 참고하세요.
 *
 * 이 배열은 각 인터럽트 스텁의 진입점(엔트리 포인트)을 가리키며,
 * intr_init()에서 쉽게 찾을 수 있도록 합니다. */
typedef void intr_stub_func (void);
extern intr_stub_func *intr_stubs[256];

#endif /* threads/intr-stubs.h */

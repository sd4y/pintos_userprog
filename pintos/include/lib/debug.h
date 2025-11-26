#ifndef __LIB_DEBUG_H
#define __LIB_DEBUG_H

/* GCC는 함수, 함수 매개변수(파라미터) 등에 "속성(attribute)"을
 * 추가하여 그것들의 속성을 명시할 수 있게 해줍니다.
 * 자세한 내용은 GCC 매뉴얼을 참조하세요. */
#define UNUSED __attribute__ ((unused))
#define NO_RETURN __attribute__ ((noreturn))
#define NO_INLINE __attribute__ ((noinline))
#define PRINTF_FORMAT(FMT, FIRST) __attribute__ ((format (printf, FMT, FIRST)))

/* 소스 파일 이름, 줄(라인) 번호, 함수 이름,
 * 그리고 사용자 지정 메시지를 출력하며 OS를 중단시킵니다. */
#define PANIC(...) debug_panic (__FILE__, __LINE__, __func__, __VA_ARGS__)

void debug_panic (const char *file, int line, const char *function,
		const char *message, ...) PRINTF_FORMAT (4, 5) NO_RETURN;
void debug_backtrace (void);

#endif



/* 이 부분은 헤더 가드(header guard) 바깥에 있습니다.
 * 이는 NDEBUG의 설정을 다르게 하여 debug.h가
 * 여러 번 포함(include)될 수 있도록 하기 위함입니다. */
#undef ASSERT
#undef NOT_REACHED

#ifndef NDEBUG
#define ASSERT(CONDITION)                                       \
	if ((CONDITION)) { } else {                             \
		PANIC ("assertion `%s' failed.", #CONDITION);   \
	}
#define NOT_REACHED() PANIC ("executed an unreachable statement");
#else
#define ASSERT(CONDITION) ((void) 0)
#define NOT_REACHED() for (;;)
#endif /* lib/debug.h */

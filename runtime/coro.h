/* Minimal coroutine switching for the HLE thread scheduler: ucontext on POSIX, fibers on Windows. */
#ifndef CORO_H
#define CORO_H

#include <stddef.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

typedef struct { void *fiber; int owned; } Coro;

static VOID CALLBACK coro_entry(LPVOID p) { ((void (*)(void))p)(); }

/* The calling thread must be able to switch fibers before the first switch. */
static inline void coro_thread_init(void) { if (!IsThreadAFiber()) ConvertThreadToFiber(NULL); }
static inline void coro_destroy(Coro *c) {
    if (c->fiber && c->owned && c->fiber != GetCurrentFiber()) DeleteFiber(c->fiber);
    c->fiber = NULL;
    c->owned = 0;
}
static inline void coro_create(Coro *c, void *stack, size_t stack_size, void (*fn)(void)) {
    (void)stack;
    coro_destroy(c);
    c->fiber = CreateFiber(stack_size, coro_entry, (LPVOID)fn);
    c->owned = 1;
}
static inline void coro_switch(Coro *from, Coro *to) {
    from->fiber = GetCurrentFiber();
    SwitchToFiber(to->fiber);
}
static inline void coro_jump(Coro *to) { SwitchToFiber(to->fiber); }

#else
#if defined(__APPLE__) && !defined(_XOPEN_SOURCE)
#define _XOPEN_SOURCE 600
#endif
#include <ucontext.h>

typedef struct { ucontext_t uc; } Coro;

static inline void coro_thread_init(void) {}
static inline void coro_destroy(Coro *c) { (void)c; }
static inline void coro_create(Coro *c, void *stack, size_t stack_size, void (*fn)(void)) {
    getcontext(&c->uc);
    c->uc.uc_stack.ss_sp = stack;
    c->uc.uc_stack.ss_size = stack_size;
    c->uc.uc_link = NULL;
    makecontext(&c->uc, fn, 0);
}
static inline void coro_switch(Coro *from, Coro *to) { swapcontext(&from->uc, &to->uc); }
static inline void coro_jump(Coro *to) { setcontext(&to->uc); }
#endif

#endif

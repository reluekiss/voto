#ifndef COROUTINE_H_
#define COROUTINE_H_
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <poll.h>
#include <unistd.h>
#include <sys/mman.h>
// # What is a Coroutine?
//
// Coroutine is a lightweight user space thread with its own stack that can
// suspend its execution and switch to another coroutine (see coroutine_yield()
// function). Coroutines do not run in parallel but rather cooperatively switch
// between each other whenever they feel like it.
//
// Coroutines are useful in cases when all your program does majority of the
// time is waiting on IO. So with coroutines you have an opportunity to do
// coroutine_yield() and go do something else. It is not useful to split up
// heavy CPU computations because they all going to be executed on a single
// thread. Use proper threads for that (pthreads on POSIX).
//
// Good use cases for coroutines are usually Network Applications and UI.
// Anything with a slow Async IO.
//
// # How does it work?
//
// Each coroutine has its own separate call stack. Every time a new coroutine is
// created with coroutine_go() a new call stack is allocated in dynamic memory.
// The library manages a global array of coroutine stacks and switches between
// them (on x86_64 literally swaps out the value of the RSP register) on every
// coroutine_yield(), coroutine_sleep_read(), or coroutine_sleep_write().

// Switch to the next coroutine. The execution will continue starting from
// coroutine_yield() (or any other flavor of it like coroutine_sleep_read() or
// coroutine_sleep_write) call of another coroutine.
void coroutine_yield(void);

// Create a new coroutine. This function does not automatically switch to the
// new coroutine, but continues executing in the current coroutine. The
// execution of the new coroutine will start as soon as the scheduler gets to it
// handling the chains of coroutine_yield()-s.
void coroutine_go(void (*f)(void*), void *arg);

// The id of the current coroutine.
size_t coroutine_id(void);

// How many coroutines are currently alive. Could be used by the main coroutine
// to wait until all the "child" coroutines have died. It may also continue from
// the call of coroutine_sleep_read() and coroutine_sleep_write() if the
// corresponding coroutine was woken up.
size_t coroutine_alive(void);

// Put the current coroutine to sleep until the non-blocking socket `fd` has
// avaliable data to read. Trying to read from fd after coroutine_sleep_read()
// may still cause EAGAIN, if the coroutine was woken up by coroutine_wake_up
// before the socket became available for reading. You may treat this function
// as a flavor of coroutine_yield().
void coroutine_sleep_read(int fd);

// Put the current coroutine to sleep until the non-blocking socket `fd` is
// ready to accept data to write. Trying to write to fd after
// coroutine_sleep_write() may still cause EAGAIN, if the coroutine was woken up
// by coroutine_wake_up before the socket became available for writing. You may
// treat this function as a flavor of coroutine_yield().
void coroutine_sleep_write(int fd);

// Wake up coroutine by id if it is currently sleeping due to
// coroutine_sleep_read() or coroutine_sleep_write() calls.
void coroutine_wake_up(size_t id);

// TODO: implement sleeping by timeout
// TODO: add timeouts to coroutine_sleep_read() and coroutine_sleep_write()

#ifdef COROUTINE_IMPLEMENTATION
// just use undef and redef this for custom values
#define STACK_CAPACITY (1024*getpagesize())

// Initial capacity of a dynamic array
#ifndef DA_INIT_CAP
#define DA_INIT_CAP 256
#endif

// Append an item to a dynamic array
#define da_append(da, item)                                                          \
    do {                                                                             \
        if ((da)->count >= (da)->capacity) {                                         \
            (da)->capacity = (da)->capacity == 0 ? DA_INIT_CAP : (da)->capacity*2;   \
            (da)->items = realloc((da)->items, (da)->capacity*sizeof(*(da)->items)); \
            assert((da)->items != NULL && "Buy more RAM lol");                       \
        }                                                                            \
                                                                                     \
        (da)->items[(da)->count++] = (item);                                         \
    } while (0)

#define da_remove_unordered(da, i)                   \
    do {                                             \
        size_t j = (i);                              \
        assert(j < (da)->count);                     \
        (da)->items[j] = (da)->items[--(da)->count]; \
    } while(0)

#define UNUSED(x) (void)(x)
#define TODO(message) do { fprintf(stderr, "%s:%d: TODO: %s\n", __FILE__, __LINE__, message); abort(); } while(0)
#define UNREACHABLE(message) do { fprintf(stderr, "%s:%d: UNREACHABLE: %s\n", __FILE__, __LINE__, message); abort(); } while(0)

typedef struct {
    void *rsp;
    void *stack_base;
} Context;

typedef struct {
    Context *items;
    size_t count;
    size_t capacity;
} Contexts;

typedef struct {
    size_t *items;
    size_t count;
    size_t capacity;
} Indices;

typedef struct {
    struct pollfd *items;
    size_t count;
    size_t capacity;
} Polls;

typedef enum {
    SM_NONE = 0,
    SM_READ,
    SM_WRITE,
} Sleep_Mode;


// TODO: coroutines library probably does not work well in multithreaded environment
static size_t current     = 0;
static Indices active     = {0};
static Indices dead       = {0};
static Contexts contexts  = {0};
static Indices asleep     = {0};
static Polls polls        = {0};

#if defined(__x86_64__)
// Linux x86_64 call convention
// %rdi, %rsi, %rdx, %rcx, %r8, and %r9

void __attribute__((naked)) coroutine_yield(void)
{
    asm(
    "    pushq %rdi\n"
    "    pushq %rbp\n"
    "    pushq %rbx\n"
    "    pushq %r12\n"
    "    pushq %r13\n"
    "    pushq %r14\n"
    "    pushq %r15\n"
    "    movq %rsp, %rdi\n"     // rsp
    "    movq $0, %rsi\n"       // sm = SM_NONE
    "    jmp coroutine_switch_context\n");
}

void __attribute__((naked)) coroutine_sleep_read(int fd)
{
    asm(
    "    pushq %rdi\n"
    "    pushq %rbp\n"
    "    pushq %rbx\n"
    "    pushq %r12\n"
    "    pushq %r13\n"
    "    pushq %r14\n"
    "    pushq %r15\n"
    "    movq %rdi, %rdx\n"     // fd
    "    movq %rsp, %rdi\n"     // rsp
    "    movq $1, %rsi\n"       // sm = SM_READ
    "    jmp coroutine_switch_context\n");
}

void __attribute__((naked)) coroutine_sleep_write(int fd)
{
    asm(
    "    pushq %rdi\n"
    "    pushq %rbp\n"
    "    pushq %rbx\n"
    "    pushq %r12\n"
    "    pushq %r13\n"
    "    pushq %r14\n"
    "    pushq %r15\n"
    "    movq %rdi, %rdx\n"     // fd
    "    movq %rsp, %rdi\n"     // rsp
    "    movq $2, %rsi\n"       // sm = SM_WRITE
    "    jmp coroutine_switch_context\n");
}

void __attribute__((naked)) coroutine_restore_context(void *rsp)
{
    asm(
    "    movq %rdi, %rsp\n"
    "    popq %r15\n"
    "    popq %r14\n"
    "    popq %r13\n"
    "    popq %r12\n"
    "    popq %rbx\n"
    "    popq %rbp\n"
    "    popq %rdi\n"
    "    ret\n");
}

#elif defined(__aarch64__)
// AArch64 calling convention
// x0-x7: argument/return registers (caller-saved)
// x9-x15: temporary registers (caller-saved)  
// x19-x28: callee-saved registers
// x29: frame pointer (FP)
// x30: link register (LR)

void __attribute__((naked)) coroutine_yield(void)
{
    asm(
    "sub sp, sp, #240\n"        
    "stp q8, q9, [sp, #0]\n"    
    "stp q10, q11, [sp, #32]\n" 
    "stp q12, q13, [sp, #64]\n" 
    "stp q14, q15, [sp, #96]\n" 
    "stp x19, x20, [sp, #128]\n"
    "stp x21, x22, [sp, #144]\n"
    "stp x23, x24, [sp, #160]\n"
    "stp x25, x26, [sp, #176]\n"
    "stp x27, x28, [sp, #192]\n"
    "stp x29, x30, [sp, #208]\n"
    "mov x1, x30\n"             
    "str x30, [sp, #224]\n"     
    "str x0, [sp, #232]\n"
    "mov x0, sp\n"
    "mov x1, #0\n"
    "b coroutine_switch_context\n"
    );
}

void __attribute__((naked)) coroutine_sleep_read(int fd)
{
    asm(
    "sub sp, sp, #240\n"        
    "stp q8, q9, [sp, #0]\n"    
    "stp q10, q11, [sp, #32]\n" 
    "stp q12, q13, [sp, #64]\n" 
    "stp q14, q15, [sp, #96]\n" 
    "stp x19, x20, [sp, #128]\n"
    "stp x21, x22, [sp, #144]\n"
    "stp x23, x24, [sp, #160]\n"
    "stp x25, x26, [sp, #176]\n"
    "stp x27, x28, [sp, #192]\n"
    "stp x29, x30, [sp, #208]\n"
    "mov x1, x30\n"             
    "str x30, [sp, #224]\n"     
    "str x0, [sp, #232]\n"
    "mov x2, x0\n"
    "mov x0, sp\n"
    "mov x1, #1\n"
    "b coroutine_switch_context\n"
    );
}

void __attribute__((naked)) coroutine_sleep_write(int fd)
{
    asm(
    "sub sp, sp, #240\n"        
    "stp q8, q9, [sp, #0]\n"    
    "stp q10, q11, [sp, #32]\n" 
    "stp q12, q13, [sp, #64]\n" 
    "stp q14, q15, [sp, #96]\n" 
    "stp x19, x20, [sp, #128]\n"
    "stp x21, x22, [sp, #144]\n"
    "stp x23, x24, [sp, #160]\n"
    "stp x25, x26, [sp, #176]\n"
    "stp x27, x28, [sp, #192]\n"
    "stp x29, x30, [sp, #208]\n"
    "mov x1, x30\n"             
    "str x30, [sp, #224]\n"     
    "str x0, [sp, #232]\n"
    "mov x2, x0\n" 
    "mov x0, sp\n"
    "mov x1, #2\n"
    "b coroutine_switch_context\n"
    );
}

void __attribute__((naked)) coroutine_restore_context(void *rsp)
{
    asm(
    "mov sp, x0\n"              
    "ldp q8, q9, [sp, #0]\n"    
    "ldp q10, q11, [sp, #32]\n" 
    "ldp q12, q13, [sp, #64]\n" 
    "ldp q14, q15, [sp, #96]\n" 
    "ldp x19, x20, [sp, #128]\n"
    "ldp x21, x22, [sp, #144]\n"
    "ldp x23, x24, [sp, #160]\n"
    "ldp x25, x26, [sp, #176]\n"
    "ldp x27, x28, [sp, #192]\n"
    "ldp x29, x30, [sp, #208]\n"
    "mov x1, x30\n"             
    "ldr x30, [sp, #224]\n"     
    "ldr x0, [sp, #232]\n"      
    "add sp, sp, #240\n"        
    "ret x1\n"
    );
}

#else
    #error "Unsupported architecture. Only x86_64 and aarch64 are supported."
#endif

void coroutine_switch_context(void *rsp, Sleep_Mode sm, int fd)
{
    contexts.items[active.items[current]].rsp = rsp;

    switch (sm) {
    case SM_NONE: current += 1; break;
    case SM_READ: {
        da_append(&asleep, active.items[current]);
        struct pollfd pfd = {.fd = fd, .events = POLLRDNORM,};
        da_append(&polls, pfd);
        da_remove_unordered(&active, current);
    } break;

    case SM_WRITE: {
        da_append(&asleep, active.items[current]);
        struct pollfd pfd = {.fd = fd, .events = POLLWRNORM,};
        da_append(&polls, pfd);
        da_remove_unordered(&active, current);
    } break;

    default: UNREACHABLE("coroutine_switch_context");
    }

    if (polls.count > 0) {
        int timeout = active.count == 0 ? -1 : 0;
        int result = poll(polls.items, polls.count, timeout);
        if (result < 0) TODO("poll");

        for (size_t i = 0; i < polls.count;) {
            if (polls.items[i].revents) {
                size_t id = asleep.items[i];
                da_remove_unordered(&polls, i);
                da_remove_unordered(&asleep, i);
                da_append(&active, id);
            } else {
                ++i;
            }
        }
    }

    assert(active.count > 0);
    current %= active.count;
    coroutine_restore_context(contexts.items[active.items[current]].rsp);
}

// No need to call this manually
__attribute__((constructor))
void coroutine_init(void)
{
    if (contexts.count != 0) return;
    da_append(&contexts, (Context){0});
    da_append(&active, 0);
}

void coroutine__finish_current(void)
{
    if (active.items[current] == 0) {
        UNREACHABLE("Main Coroutine with id == 0 should never reach this place");
    }

    da_append(&dead, active.items[current]);
    da_remove_unordered(&active, current);

    if (polls.count > 0) {
        int timeout = active.count == 0 ? -1 : 0;
        int result = poll(polls.items, polls.count, timeout);
        if (result < 0) TODO("poll");

        for (size_t i = 0; i < polls.count;) {
            if (polls.items[i].revents) {
                size_t id = asleep.items[i];
                da_remove_unordered(&polls, i);
                da_remove_unordered(&asleep, i);
                da_append(&active, id);
            } else {
                ++i;
            }
        }
    }

    assert(active.count > 0);
    current %= active.count;
    coroutine_restore_context(contexts.items[active.items[current]].rsp);
}

void coroutine_go(void (*f)(void*), void *arg)
{
    size_t id;
    if (dead.count > 0) {
        id = dead.items[--dead.count];
    } else {
        da_append(&contexts, ((Context){0}));
        id = contexts.count-1;
        contexts.items[id].stack_base = mmap(NULL, STACK_CAPACITY, PROT_WRITE|PROT_READ, MAP_PRIVATE|MAP_STACK|MAP_ANONYMOUS|MAP_GROWSDOWN, -1, 0);
        assert(contexts.items[id].stack_base != MAP_FAILED);
    }

    void **rsp = (void**)((char*)contexts.items[id].stack_base + STACK_CAPACITY);
    
    // stack setup
#if defined(__x86_64__)
    *(--rsp) = coroutine__finish_current;
    *(--rsp) = f;
    *(--rsp) = arg; // push rdi
    *(--rsp) = 0;   // push rbp
    *(--rsp) = 0;   // push rbx
    *(--rsp) = 0;   // push r12
    *(--rsp) = 0;   // push r13
    *(--rsp) = 0;   // push r14
    *(--rsp) = 0;   // push r15
#elif defined(__aarch64__)
    *(--rsp) = arg;
    *(--rsp) = coroutine__finish_current;
    *(--rsp) = f; // push r0
    *(--rsp) = 0; // push r29
    *(--rsp) = 0; // push r28
    *(--rsp) = 0; // push r27
    *(--rsp) = 0; // push r26
    *(--rsp) = 0; // push r25
    *(--rsp) = 0; // push r24
    *(--rsp) = 0; // push r23
    *(--rsp) = 0; // push r22
    *(--rsp) = 0; // push r21
    *(--rsp) = 0; // push r20
    *(--rsp) = 0; // push r19
    *(--rsp) = 0; // push v15
    *(--rsp) = 0;
    *(--rsp) = 0; // push v14
    *(--rsp) = 0;
    *(--rsp) = 0; // push v13
    *(--rsp) = 0;
    *(--rsp) = 0; // push v12
    *(--rsp) = 0;
    *(--rsp) = 0; // push v11
    *(--rsp) = 0;
    *(--rsp) = 0; // push v10
    *(--rsp) = 0;
    *(--rsp) = 0; // push v09
    *(--rsp) = 0;
    *(--rsp) = 0; // push v08
    *(--rsp) = 0;
#endif
    
    contexts.items[id].rsp = rsp;
    da_append(&active, id);
}

size_t coroutine_id(void)
{
    return active.items[current];
}

size_t coroutine_alive(void)
{
    return active.count;
}

void coroutine_wake_up(size_t id)
{
    // @speed coroutine_wake_up is linear
    for (size_t i = 0; i < asleep.count; ++i) {
        if (asleep.items[i] == id) {
            da_remove_unordered(&asleep, id);
            da_remove_unordered(&polls, id);
            da_append(&active, id);
            return;
        }
    }
}

#endif // COROUTINE_IMPLEMENTATION

#endif // COROUTINE_H_

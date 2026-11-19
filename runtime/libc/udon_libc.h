// Declarations for the freestanding libc that llvm-udon links in.
//
// The driver force-includes this file, so a program compiled with cc-udon.sh
// needs no #include at all. It is deliberately not a copy of any hosted header:
// it declares only what runtime/libc/libc.c implements, so a call to something
// unimplemented fails at compile time rather than at run time.
//
// Nothing here may assume a hosted environment. There is no errno, no locale,
// no stdio stream and no process to exit from.

#ifndef UDON_LIBC_H
#define UDON_LIBC_H

typedef unsigned int size_t;
typedef int ptrdiff_t;

#ifndef NULL
#define NULL ((void *)0)
#endif

void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);

size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
char *strcpy(char *dst, const char *src);
char *strncpy(char *dst, const char *src, size_t n);
int atoi(const char *s);

int abs(int v);
long labs(long v);

void exit(int status);

// The heap is the region between the globals image and the shadow stack, and
// its size is `-udon-heap-bytes`. `malloc` is a bump allocator: nothing is ever
// reclaimed, which is the only honest answer when there is no way to run a
// collector inside a Udon behaviour.
void *malloc(size_t n);
void *calloc(size_t n, size_t size);
void *realloc(void *p, size_t n);
void free(void *p);

#endif

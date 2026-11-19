// Freestanding libc for llvm-udon.
//
// Compiled by the same clang that compiles the program and linked in as IR, so
// these need no Udon extern and no hand-written .ll. Only capabilities the
// target genuinely lacks (the host's math, logging, event dispatch) are externs.
//
// No headers: this file is self-contained, so it compiles with -ffreestanding
// and needs no sysroot. Every function here is also one clang would otherwise
// synthesise a call to (a struct copy, an array initialiser), which is why the
// build passes -fno-builtin: without it clang may lower part of memset into a
// call to memset.

typedef unsigned int u32;
typedef unsigned long long u64;

// A pointer is a byte offset into `__mem`, so the word size matches the target's
// 32-bit pointers rather than the host's.
typedef unsigned int size_t;
typedef int ptrdiff_t;

#define NULL ((void *)0)

// ---------------------------------------------------------------------------
// String and memory
// ---------------------------------------------------------------------------

void *memcpy(void *dst, const void *src, size_t n) {
  unsigned char *d = (unsigned char *)dst;
  const unsigned char *s = (const unsigned char *)src;
  // `__mem` is a 32-bit word array: a `load i32, align 4`/`store i32, align 4`
  // pair at a word-aligned address is two array externs, while the byte loop
  // below costs a window read, shift, mask and store for every single byte.
  // When both ends are 4-aligned, copy the bulk in words and only the tail in
  // bytes. The pointer is a byte offset, so `(u32)ptr & 3` is its alignment.
  if ((((u32)d | (u32)s) & 3u) == 0u) {
    u32 *dw = (u32 *)d;
    const u32 *sw = (const u32 *)s;
    size_t words = n >> 2;
    while (words--)
      *dw++ = *sw++;
    size_t rem = n & 3u;
    d = (unsigned char *)dw;
    s = (const unsigned char *)sw;
    while (rem--)
      *d++ = *s++;
  } else {
    while (n--)
      *d++ = *s++;
  }
  return dst;
}

// memmove differs from memcpy only when the ranges overlap and dst > src, where
// a forward copy would re-read bytes it has already written. Copying backwards
// in that one case is what makes overlap safe.
void *memmove(void *dst, const void *src, size_t n) {
  unsigned char *d = (unsigned char *)dst;
  const unsigned char *s = (const unsigned char *)src;
  if (d == s || n == 0)
    return dst;
  if (d < s) {
    memcpy(dst, src, n);
    return dst;
  }
  // d > s: copy from the tail backwards. Word-copy when both ends are aligned.
  if ((((u32)d | (u32)s) & 3u) == 0u) {
    u32 *dw = (u32 *)d + (n >> 2);
    const u32 *sw = (const u32 *)s + (n >> 2);
    size_t words = n >> 2;
    while (words--)
      *--dw = *--sw;
    size_t rem = n & 3u;
    d = (unsigned char *)d + (n & ~3u);
    s = (const unsigned char *)s + (n & ~3u);
    while (rem--)
      *--d = *--s;
  } else {
    d += n;
    s += n;
    while (n--)
      *--d = *--s;
  }
  return dst;
}

void *memset(void *dst, int c, size_t n) {
  unsigned char *d = (unsigned char *)dst;
  unsigned char v = (unsigned char)c;
  // Word fill when the destination is 4-aligned: broadcast the byte into a
  // 32-bit pattern and store words.
  if (((u32)d & 3u) == 0u) {
    u32 pat = (u32)v * 0x01010101u;
    u32 *dw = (u32 *)d;
    size_t words = n >> 2;
    while (words--)
      *dw++ = pat;
    size_t rem = n & 3u;
    d = (unsigned char *)dw;
    while (rem--)
      *d++ = v;
  } else {
    while (n--)
      *d++ = v;
  }
  return dst;
}

int memcmp(const void *a, const void *b, size_t n) {
  const unsigned char *p = (const unsigned char *)a;
  const unsigned char *q = (const unsigned char *)b;
  while (n--) {
    if (*p != *q)
      return (int)*p - (int)*q;
    p++;
    q++;
  }
  return 0;
}

size_t strlen(const char *s) {
  const char *p = s;
  while (*p)
    p++;
  return (size_t)(p - s);
}

int strcmp(const char *a, const char *b) {
  while (*a && *a == *b) {
    a++;
    b++;
  }
  return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
  while (n && *a && *a == *b) {
    a++;
    b++;
    n--;
  }
  if (n == 0)
    return 0;
  return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

char *strcpy(char *dst, const char *src) {
  char *d = dst;
  while ((*d++ = *src++))
    ;
  return dst;
}

char *strncpy(char *dst, const char *src, size_t n) {
  char *d = dst;
  while (n && *src) {
    *d++ = *src++;
    n--;
  }
  while (n--)
    *d++ = '\0';
  return dst;
}

int abs(int v) { return v < 0 ? -v : v; }

long labs(long v) { return v < 0 ? -v : v; }

// Parsed by hand: strtol would need errno, and nothing here reports errors.
int atoi(const char *s) {
  int sign = 1;
  int value = 0;
  while (*s == ' ' || *s == '\t' || *s == '\n')
    s++;
  if (*s == '-') {
    sign = -1;
    s++;
  } else if (*s == '+') {
    s++;
  }
  while (*s >= '0' && *s <= '9')
    value = value * 10 + (*s++ - '0');
  return sign * value;
}

// ---------------------------------------------------------------------------
// Exit
// ---------------------------------------------------------------------------

// There is no process to exit from: the Udon event simply returns.
void exit(int status) { (void)status; }

// ---------------------------------------------------------------------------
// Heap
// ---------------------------------------------------------------------------

// The writer owns the memory map, so it fills these two in: the heap is the
// region between the globals image and the shadow stack, and only the writer
// knows where that is. A program that never calls malloc leaves them zero, and
// malloc then fails rather than handing out an address outside the heap.
//
// These are real `__mem` globals, not slots: malloc mutates them, and a data
// image value cannot be written to.
unsigned __udon_heap_ptr;
unsigned __udon_heap_end;

#define HEAP_ALIGN 8u
#define HEAP_HEADER 8u

static unsigned heap_align(unsigned v) {
  return (v + (HEAP_ALIGN - 1)) & ~(HEAP_ALIGN - 1);
}

// A bump allocator with a size header. Nothing is ever reclaimed: this target
// has no way to run a collector, and every allocation a Udon behaviour makes
// lives as long as the behaviour does. The header exists so realloc can copy
// the right number of bytes.
void *malloc(size_t n) {
  if (n == 0)
    return NULL;
  unsigned base = heap_align(__udon_heap_ptr);
  unsigned body = base + HEAP_HEADER;
  unsigned end = heap_align(body + (unsigned)n);
  // `end < body` catches the wrap that a huge request would otherwise hide.
  if (end < body || end > __udon_heap_end)
    return NULL;
  *(unsigned *)base = (unsigned)n;
  __udon_heap_ptr = end;
  return (void *)body;
}

void free(void *p) { (void)p; }

void *calloc(size_t n, size_t size) {
  unsigned total = (unsigned)(n * size);
  if (size != 0 && total / (unsigned)size != (unsigned)n)
    return NULL;
  void *p = malloc(total);
  if (!p)
    return NULL;
  memset(p, 0, total);
  return p;
}

void *realloc(void *p, size_t n) {
  if (!p)
    return malloc(n);
  if (n == 0) {
    free(p);
    return NULL;
  }
  unsigned old = *(unsigned *)((unsigned)p - HEAP_HEADER);
  void *q = malloc(n);
  if (!q)
    return NULL;
  memcpy(q, p, old < (unsigned)n ? old : (unsigned)n);
  return q;
}

// ---------------------------------------------------------------------------
// Math the host provides
// ---------------------------------------------------------------------------

// sqrtf and sqrt are lowered to the host's SystemMath.Sqrt by the writer: it is
// the one square root available, and re-deriving one here would only be slower
// and less accurate. See UDONWriterCalls.cpp.
//
// frexp has no host equivalent (.NET has no Math.Frexp), so it is built from
// multiplications. Loop counts are small for normal values; a denormal takes
// more iterations, which is correct if not fast.
double frexp(double x, int *exp) {
  int e = 0;
  if (x == 0.0 || x != x) {
    *exp = 0;
    return x;
  }
  double ax = x < 0.0 ? -x : x;
  while (ax >= 1.0) {
    ax *= 0.5;
    x *= 0.5;
    e++;
  }
  while (ax < 0.5) {
    ax *= 2.0;
    x *= 2.0;
    e--;
  }
  *exp = e;
  return x;
}

// ---------------------------------------------------------------------------
// Host facilities that do not exist
// ---------------------------------------------------------------------------

// There is no process, no filesystem and no console inside a Udon behaviour, so
// these are stubs rather than implementations. They exist because libraries
// compiled for a hosted target reference them on debug and asset-loading paths;
// the data a real program needs (a ROM, say) is supplied by the embedding
// behaviour instead of being read from disk.
//
// Every one is variadic where the C library's is, so a call site passing more
// arguments than the definition declares still links: the writer gives each
// passed argument a slot and the stub ignores them.

int printf(const char *format, ...) {
  (void)format;
  return 0;
}

int sprintf(char *dst, const char *format, ...) {
  (void)format;
  if (dst)
    dst[0] = 0;
  return 0;
}

int fprintf(void *stream, const char *format, ...) {
  (void)stream;
  (void)format;
  return 0;
}

int fputs(const char *s, void *stream) {
  (void)s;
  (void)stream;
  return 0;
}

int fputc(int c, void *stream) {
  (void)stream;
  return c;
}

void perror(const char *s) { (void)s; }

void *fopen(const char *path, const char *mode) {
  (void)path;
  (void)mode;
  return NULL;
}

int fclose(void *stream) {
  (void)stream;
  return 0;
}

size_t fread(void *buf, size_t size, size_t count, void *stream) {
  (void)buf;
  (void)size;
  (void)count;
  (void)stream;
  return 0;
}

size_t fwrite(const void *buf, size_t size, size_t count, void *stream) {
  (void)buf;
  (void)size;
  (void)count;
  (void)stream;
  return 0;
}

int fseek(void *stream, long offset, int whence) {
  (void)stream;
  (void)offset;
  (void)whence;
  return -1;
}

long ftell(void *stream) {
  (void)stream;
  return -1;
}

int feof(void *stream) {
  (void)stream;
  return 1;
}

void *opendir(const char *path) {
  (void)path;
  return NULL;
}

void *readdir(void *dir) {
  (void)dir;
  return NULL;
}

int closedir(void *dir) {
  (void)dir;
  return 0;
}

int open(const char *path, int flags, ...) {
  (void)path;
  (void)flags;
  return -1;
}

int close(int fd) {
  (void)fd;
  return -1;
}

// The Microsoft CRT's process-wide file plumbing, referenced by libsm64's
// audio-file loader. None of it can work here.
void *__acrt_iob_func(unsigned index) {
  (void)index;
  return NULL;
}

int _fileno(void *stream) {
  (void)stream;
  return -1;
}

int _setmode(int fd, int mode) {
  (void)fd;
  (void)mode;
  return -1;
}

int _mkdir(const char *path) {
  (void)path;
  return -1;
}

int _stat64(const char *path, void *buf) {
  (void)path;
  (void)buf;
  return -1;
}

int _utime64(const char *path, void *times) {
  (void)path;
  (void)times;
  return -1;
}

// `assert` failure. Reporting is the host's job, so this only stops the
// activation: there is nothing to abort.
void _assert(const char *expr, const char *file, unsigned line) {
  (void)expr;
  (void)file;
  (void)line;
}

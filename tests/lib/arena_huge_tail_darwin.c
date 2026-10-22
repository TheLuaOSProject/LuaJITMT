/* Interpose only the arena translation unit; production objects stay intact. */
#include <stdlib.h>
#include <sys/mman.h>
void *__wrap_mmap(void *, size_t, int, int, int, off_t);
int __wrap_munmap(void *, size_t);
void *__wrap_calloc(size_t, size_t);
void __wrap_free(void *);
#define mmap __wrap_mmap
#define munmap __wrap_munmap
#define calloc __wrap_calloc
#define free __wrap_free
#include "../../src/lj_arena.c"

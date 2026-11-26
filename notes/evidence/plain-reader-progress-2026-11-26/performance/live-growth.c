#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include "lj_obj.h"
#include "lj_arena.h"
#include "lj_prng.h"
static double now(void) { struct timespec t; assert(clock_gettime(CLOCK_MONOTONIC,&t)==0); return (double)t.tv_sec+(double)t.tv_nsec*1e-9; }
int main(int argc,char **argv) {
  TGAlloc alloc; PRNGState rs; size_t i,n; unsigned char **ptr; double t,elapsed;
  assert(argc==2); n=(size_t)strtoul(argv[1],NULL,10)*(LJ_ARENA_CELLS-LJ_AFIRST_CELL)/60;
  ptr=(unsigned char **)malloc(n*sizeof(*ptr)); assert(ptr);
  lj_prng_seed_fixed(&rs); lj_arena_alloc_init(&alloc); t=now();
  for(i=0;i<n;i++) {ptr[i]=(unsigned char *)lj_arena_alloc(&alloc,&rs,960,0); assert(ptr[i]); ptr[i][0]=(unsigned char)i;}
  elapsed=now()-t;
  for(i=0;i<n;i++)assert(ptr[i][0]==(unsigned char)i);
  printf("%zu,%.9f,%.3f\n",n,elapsed,elapsed*1e9/(double)n);
  lj_arena_alloc_fini(&alloc); free(ptr); return 0;
}

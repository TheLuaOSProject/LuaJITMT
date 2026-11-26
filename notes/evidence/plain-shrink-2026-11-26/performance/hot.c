#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "lj_obj.h"
#include "lj_arena.h"
#include "lj_prng.h"
static double stamp(clockid_t clock)
{
  struct timespec t;assert(clock_gettime(clock,&t)==0);
  return (double)t.tv_sec+(double)t.tv_nsec*1e-9;
}
int main(int argc,char **argv)
{
  TGAlloc alloc; PRNGState rs;size_t size,n,i;double c,w;
  assert(argc==3);size=(size_t)strtoul(argv[1],NULL,10);n=(size_t)strtoul(argv[2],NULL,10);
  assert(size&&size<=LJ_HUGE_THRESHOLD&&n);
  lj_prng_seed_fixed(&rs);lj_arena_alloc_init(&alloc);
  for(i=0;i<5000;i++) {void *p=lj_arena_alloc(&alloc,&rs,size,0);assert(p);lj_arena_free(&alloc,p,size);}
  c=stamp(CLOCK_PROCESS_CPUTIME_ID);w=stamp(CLOCK_MONOTONIC);
  for(i=0;i<n;i++) {
    unsigned char *p=(unsigned char *)lj_arena_alloc(&alloc,&rs,size,0);assert(p);
    p[0]=(unsigned char)i;p[size-1]=(unsigned char)(i+1);
    lj_arena_free(&alloc,p,size);
  }
  c=stamp(CLOCK_PROCESS_CPUTIME_ID)-c;w=stamp(CLOCK_MONOTONIC)-w;
  printf("%zu,%zu,%.9f,%.9f,%.3f,%u\n",size,n,c,w,c*1e9/(double)n,lj_arena_alloc_owned_count_acq(&alloc,LJ_ARENAK_PLAIN));
  lj_arena_alloc_fini(&alloc);return 0;
}

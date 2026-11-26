#define main bin_capacity_fixture_main
#include "t-arena-bin-capacity.c"
#undef main
#include <time.h>
static double stamp(clockid_t clock)
{
  struct timespec t; assert(clock_gettime(clock,&t)==0);
  return (double)t.tv_sec+(double)t.tv_nsec*1e-9;
}
int main(int argc, char **argv)
{
  Workload w; Census free_before, freed; unsigned char **again,*seen;
  uint32_t count,flags,i,arenas,reused=0; double c,wall,fc,fw,rc,rw;
  assert(argc==3);count=(uint32_t)strtoul(argv[1],NULL,10);
  flags=atoi(argv[2]) ? LJ_AF_TRAVERSABLE : 0;
  assert(count && count<=CAPACITY_RUNS);alarm(60);
  init_workload(&w,flags,count);allocate_blocks(&w);
  free_before=census(&w,NULL);arenas=lj_arena_alloc_owned_count_acq(&w.alloc,w.kind);
  c=stamp(CLOCK_PROCESS_CPUTIME_ID);wall=stamp(CLOCK_MONOTONIC);
  for(i=0;i<count;i++)lj_arena_free(&w.alloc,w.blocks[i],BLOCK_BYTES);
  fc=stamp(CLOCK_PROCESS_CPUTIME_ID)-c;fw=stamp(CLOCK_MONOTONIC)-wall;
  freed=census(&w,NULL);
  again=(unsigned char **)calloc(count,sizeof(*again));seen=(unsigned char *)calloc(count,1);
  assert(again&&seen);
  c=stamp(CLOCK_PROCESS_CPUTIME_ID);wall=stamp(CLOCK_MONOTONIC);
  for(i=0;i<count;i++) {
    again[i]=(unsigned char *)lj_arena_alloc(&w.alloc,&w.prng,BLOCK_BYTES,flags);
    assert(again[i]);memset(again[i],pattern(i+103u),BLOCK_BYTES);
  }
  rc=stamp(CLOCK_PROCESS_CPUTIME_ID)-c;rw=stamp(CLOCK_MONOTONIC)-wall;
  for(i=0;i<count;i++) {
    uintptr_t key=(uintptr_t)again[i],*found=(uintptr_t *)bsearch(&key,w.sorted,count,sizeof(key),compare_address);
    if(found) {size_t index=(size_t)(found-w.sorted);assert(!seen[index]);seen[index]=1;reused++;}
    check_bytes(again[i],BLOCK_BYTES,pattern(i+103u));
  }
  printf("%u,%u,%.9f,%.9f,%.9f,%.9f,%u,%u,%u,%llu,%llu,%u\n",
    count,flags!=0,fc,fw,rc,rw,arenas,lj_arena_alloc_owned_count_acq(&w.alloc,w.kind),freed.targets,
    (unsigned long long)freed.cells,(unsigned long long)(free_before.cells+(uint64_t)count*lj_arena_ncells(BLOCK_BYTES)),reused);
  free(again);free(seen);fini_workload(&w);alarm(0);return 0;
}

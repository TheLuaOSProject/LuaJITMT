#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "lj_obj.h"
#include "lj_arena.h"
#include "lj_prng.h"
static double stamp(clockid_t clock)
{
  struct timespec t;assert(clock_gettime(clock,&t)==0);
  return (double)t.tv_sec+(double)t.tv_nsec*1e-9;
}
static void emit(TGAlloc *alloc,const char *stage,size_t logical,double c,double w)
{
  GCArena *a;size_t cells=0,starts=0,freecells=0,bumpcells=0,nodes=0;unsigned arenas=0,bin;char ack[16];
  for(a=alloc->owned[LJ_ARENAK_PLAIN];a;a=lj_arena_next_acq(a)) {
    uint32_t cell;arenas++;
    for(cell=LJ_AFIRST_CELL;cell<LJ_ARENA_CELLS;cell++) {
      uint32_t end;
      if(!lj_arena_bm_get(a->block,cell))continue;
      starts++;end=cell+1;
      while(end<LJ_ARENA_CELLS&&!lj_arena_bm_get(a->block,end)&&!lj_arena_bm_get(a->mark,end))end++;
      if(alloc->bump[LJ_ARENAK_PLAIN].a==a&&alloc->bump[LJ_ARENAK_PLAIN].cell>cell&&alloc->bump[LJ_ARENAK_PLAIN].cell<end)end=alloc->bump[LJ_ARENAK_PLAIN].cell;
      cells+=end-cell;
    }
  }
  for(bin=0;bin<LJ_ALLOC_NBINS;bin++) {
    LJArenaFreeRun *r;
    for(r=alloc->bins[LJ_ARENAK_PLAIN][bin];r;r=r->next) { assert(++nodes<1000000);freecells+=r->len; }
  }
  if(alloc->bump[LJ_ARENAK_PLAIN].a)bumpcells=alloc->bump[LJ_ARENAK_PLAIN].end-alloc->bump[LJ_ARENAK_PLAIN].cell;
  printf("%s,%zu,%zu,%zu,%u,%zu,%zu,%.9f,%.9f\n",stage,logical,cells*LJ_CELL_SIZE,starts,arenas,freecells*LJ_CELL_SIZE,bumpcells*LJ_CELL_SIZE,c,w);fflush(stdout);
  assert(fgets(ack,sizeof(ack),stdin)&&!strcmp(ack,"go\n"));
}
int main(int argc,char **argv)
{
  TGAlloc alloc;PRNGState rs;unsigned char **p,**q;size_t n,size,nq,i;double c,w;
  assert(argc==3);n=(size_t)strtoul(argv[1],NULL,10);size=(size_t)strtoul(argv[2],NULL,10);
  assert(n&&size>=4096&&size<=LJ_HUGE_THRESHOLD&&size%1024==0);
  nq=n*(size/1024-1);p=(unsigned char **)calloc(n,sizeof(*p));q=(unsigned char **)calloc(nq,sizeof(*q));assert(p&&q);
  lj_prng_seed_fixed(&rs);lj_arena_alloc_init(&alloc);
  puts("stage,logical_bytes,physical_allocated_bytes,block_starts,arenas,bin_free_bytes,bump_free_bytes,work_cpu,work_wall");fflush(stdout);
  c=stamp(CLOCK_PROCESS_CPUTIME_ID);w=stamp(CLOCK_MONOTONIC);
  for(i=0;i<n;i++){p[i]=(unsigned char *)lj_arena_alloc(&alloc,&rs,size,0);assert(p[i]);memset(p[i],0xa6,size);}
  c=stamp(CLOCK_PROCESS_CPUTIME_ID)-c;w=stamp(CLOCK_MONOTONIC)-w;emit(&alloc,"original",n*size,c,w);
  c=stamp(CLOCK_PROCESS_CPUTIME_ID);w=stamp(CLOCK_MONOTONIC);
  for(i=0;i<n;i++){void *next=lj_arena_realloc(&alloc,&rs,p[i],size,16,0);assert(next==p[i]);assert(p[i][0]==0xa6&&p[i][15]==0xa6);}
  c=stamp(CLOCK_PROCESS_CPUTIME_ID)-c;w=stamp(CLOCK_MONOTONIC)-w;emit(&alloc,"shrunk",n*16,c,w);
  c=stamp(CLOCK_PROCESS_CPUTIME_ID);w=stamp(CLOCK_MONOTONIC);
  for(i=0;i<nq;i++){q[i]=(unsigned char *)lj_arena_alloc(&alloc,&rs,1024,0);assert(q[i]);memset(q[i],0x5d,1024);}
  c=stamp(CLOCK_PROCESS_CPUTIME_ID)-c;w=stamp(CLOCK_MONOTONIC)-w;emit(&alloc,"refilled",n*16+nq*1024,c,w);
  for(i=0;i<n;i++)assert(p[i][0]==0xa6&&p[i][15]==0xa6);
  for(i=0;i<nq;i++)assert(q[i][0]==0x5d&&q[i][1023]==0x5d);
  c=stamp(CLOCK_PROCESS_CPUTIME_ID);w=stamp(CLOCK_MONOTONIC);
  for(i=0;i<n;i++)lj_arena_free(&alloc,p[i],16);
  for(i=0;i<nq;i++)lj_arena_free(&alloc,q[i],1024);
  c=stamp(CLOCK_PROCESS_CPUTIME_ID)-c;w=stamp(CLOCK_MONOTONIC)-w;emit(&alloc,"all_freed",0,c,w);
  lj_arena_alloc_rebuild_free_kind(&alloc,LJ_ARENAK_PLAIN);emit(&alloc,"rebuilt",0,0,0);
  lj_arena_alloc_fini(&alloc);free(p);free(q);return 0;
}

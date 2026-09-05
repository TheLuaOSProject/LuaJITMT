#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include "lj_obj.h"
#include "lj_arena.h"
#include "lj_prng.h"

struct Watch {void *base,*wide;size_t size;unsigned active,unmaps;};
static struct Watch watched[512];static unsigned nwatch,auto_watch,fail_map,mapcalls,calloc_calls,free_calls;
static struct Watch *watch_base(void *base,size_t size) {
 struct Watch *w;assert(nwatch<512);w=&watched[nwatch++];w->base=base;w->size=size;w->wide=(char *)base+size-16;w->active=1;return w;
}
static struct Watch *watch(void *p,size_t size) {return watch_base(lj_arena_of(p),lj_arena_huge_mapsize(size));}
extern void *__real_mmap64(void *,size_t,int,int,int,off64_t);
extern int __real_munmap(void *,size_t);
extern void *__real_calloc(size_t,size_t);
extern void __real_free(void *);
void *__wrap_mmap64(void *p,size_t n,int prot,int flags,int fd,off64_t off) {
 void *r;mapcalls++;if(fail_map){errno=ENOMEM;return MAP_FAILED;}
 r=__real_mmap64(p,n,prot,flags,fd,off);
 if(auto_watch && r!=MAP_FAILED) {
  uintptr_t a=((uintptr_t)r+LJ_ARENA_MASK)&~(uintptr_t)LJ_ARENA_MASK;
  assert(n>LJ_ARENA_SIZE);watch_base((void *)a,n-LJ_ARENA_SIZE);
 }
 return r;
}
void *__wrap_mmap(void *p,size_t n,int prot,int flags,int fd,off_t off) {return __wrap_mmap64(p,n,prot,flags,fd,(off64_t)off);}
int __wrap_munmap(void *p,size_t n) {
 unsigned i;
 for(i=0;i<nwatch;i++)if(watched[i].active && p==watched[i].base) {
  assert(n==watched[i].size);watched[i].active=0;watched[i].unmaps++;
 }
 return __real_munmap(p,n);
}
void *__wrap_calloc(size_t n,size_t s){calloc_calls++;return __real_calloc(n,s);}
void __wrap_free(void *p){unsigned i;free_calls++;for(i=0;i<nwatch;i++)assert(!watched[i].active || p!=watched[i].wide);__real_free(p);}
static size_t want_size(size_t s) {
 const size_t max=~(size_t)LJ_ARENA_MASK;
 if(s<=LJ_HUGE_THRESHOLD || s>max-sizeof(GCAhdr)-16)return 0;
 return (s+sizeof(GCAhdr)+16+LJ_ARENA_MASK)&~(size_t)LJ_ARENA_MASK;
}
static la_u128 wide_seed(void *p) {
 LJGC2TabWideStamp *w=lj_arena_gc2_wide_acq(p);la_u128 old,next={UINT64_C(0x1122334400000017),UINT64_C(0x8877665544332211)};
 assert(w);old=lj_arena_gc2_wide_snapshot(w);assert(la_cas128(&w->proof,&old,next));
 la_store64_rel(&lj_arena_gc2_stamp_acq(p)->state,UINT32_MAX);return next;
}
static void wide_is(void *p,la_u128 expected) {
 la_u128 actual=lj_arena_gc2_wide_snapshot(lj_arena_gc2_wide_acq(p));assert(actual.lo==expected.lo && actual.hi==expected.hi);
}
static void bytes_are(void *p,size_t n,unsigned char c) {size_t i;for(i=0;i<n;i++)assert(((unsigned char *)p)[i]==c);}
static void delete_map(HugeTab *ht,void *p) {LJHugeInfo hi;assert(lj_arena_hugetab_delete(ht,p,&hi)==1);lj_arena_huge_unmap(p,hi.size);}
static void payload_tail_probe(PRNGState *rs) {
 size_t size=65393;void *p=lj_arena_huge_map(rs,size,LJ_AF_TRAVERSABLE);la_u128 proof;
 assert(p);watch(p,size);proof=wide_seed(p);memset(p,0xa7,size);
 /* Causal negative: the old mapping length lets the final user byte overwrite W. */
 wide_is(p,proof);bytes_are(p,size,0xa7);lj_arena_huge_unmap(p,size);
 puts("final advertised payload byte leaves W intact");
}
static void geometry(PRNGState *rs) {
 const int deltas[]={-17,-16,-15,-1,0,1,15,16,17};unsigned k,j,kind;
 const size_t max=~(size_t)LJ_ARENA_MASK;
 assert(sizeof(GCAhdr)==128 && sizeof(LJGC2TabStamp)==16 && offsetof(LJGC2TabStamp,token)==8);
 assert(lj_arena_huge_mapsize(LJ_HUGE_THRESHOLD)==0);
 assert(lj_arena_huge_mapsize(LJ_HUGE_THRESHOLD+1)==want_size(LJ_HUGE_THRESHOLD+1));
 assert(lj_arena_huge_mapsize(max-144)==max);
 for(k=0;k<144;k++)assert(lj_arena_huge_mapsize(max-k)==0);
 assert(lj_arena_huge_mapsize(SIZE_MAX)==0);
 errno=EDOM;assert(lj_arena_huge_map(rs,max-144,LJ_AF_TRAVERSABLE)==NULL);assert(errno==EDOM);
 for(k=1;k<=3;k++)for(j=0;j<sizeof(deltas)/sizeof(deltas[0]);j++)for(kind=0;kind<2;kind++) {
  size_t size=(size_t)k*LJ_ARENA_SIZE-sizeof(GCAhdr)-16+deltas[j],m=lj_arena_huge_mapsize(size);
  void *p;GCArena *a;struct Watch *watcher;unsigned calls=calloc_calls,frees=free_calls;LJGC2TabWideStamp *w;
  assert(m==want_size(size));errno=EDOM;p=lj_arena_huge_map(rs,size,kind?LJ_AF_TRAVERSABLE:0);assert(p && errno==EDOM);
  watcher=watch(p,size);a=lj_arena_of(p);w=lj_arena_gc2_wide_acq(p);
  assert((char *)p==(char *)a+128);assert(a->hdr.live_cells==(uint32_t)(m>>LJ_CELL_SHIFT));
  if(kind)assert((void *)w==(char *)a+m-16 && ((uintptr_t)w&15)==0 && (char *)p+size<=(char *)w);else assert(!w);
  memset(p,0xa7,size);bytes_are(p,size,0xa7);
  if(w){la_u128 zero={0,0};wide_is(p,zero);(void)wide_seed(p);bytes_are(p,size,0xa7);}
  lj_arena_huge_unmap(p,size);assert(errno==EDOM);assert(watcher->unmaps==1);
  assert(calloc_calls==calls && free_calls==frees);
 }
 puts("checked physical geometry, full payload bytes, zero W, plain NULL and zero calloc/free");
}
static void bounds_and_zero(PRNGState *rs) {
 HugeTab ht={0};void *p,*base=NULL;size_t s=20000,m=lj_arena_huge_mapsize(s);LJHugeReader r={0};LJHugeInfo hi;
 LJGC2TabWideStamp *w;unsigned char resident=0;long page=sysconf(_SC_PAGESIZE);void *ends[4];unsigned i;
 assert(lj_arena_hugetab_init(&ht,3));p=lj_arena_huge_map(rs,s,LJ_AF_TRAVERSABLE);assert(p);watch(p,s);w=lj_arena_gc2_wide_acq(p);
 assert(mincore((void *)((uintptr_t)w&~((uintptr_t)page-1)),(size_t)page,&resident)==0);assert((resident&1)==0);
 assert(lj_arena_hugetab_insert(&ht,p,s,LJ_HUGEF_TRAVERSABLE)==1);assert(lj_arena_hugetab_publish_interior_cdata(&ht,p));
 assert(lj_arena_hugetab_reader_range_acquire(&ht,(char *)p+s-1,&base,&r,&hi)==1);assert(base==p && hi.size==s);
 assert(lj_arena_hugetab_reader_covers(&r,(char *)p+s-1));assert(!lj_arena_hugetab_reader_covers(&r,(char *)p+s));
 assert(lj_arena_hugetab_reader_covers_range(&r,(char *)p+s,0));assert(!lj_arena_hugetab_reader_covers_range(&r,(char *)p+s-1,2));
 assert(lj_arena_hugetab_reader_release(&r,NULL)==LJ_ARENA_HUGE_READER_RELEASED);
 ends[0]=(char *)p+s;ends[1]=(char *)p+s+1;ends[2]=w;ends[3]=(char *)lj_arena_of(p)+m-1;
 for(i=0;i<4;i++) {
  base=(void *)1;assert(!lj_arena_hugetab_range_lookup(&ht,ends[i],&base,&hi));
  base=(void *)1;assert(!lj_arena_hugetab_cdata_range_lookup(&ht,ends[i],&base,&hi));
  base=(void *)1;assert(lj_arena_hugetab_reader_range_acquire(&ht,ends[i],&base,&r,&hi)==LJ_ARENA_HUGE_READER_MISSING);assert(!r.h);
  assert(lj_arena_hugetab_reader_cdata_range_acquire(&ht,ends[i],&base,&r,&hi)==LJ_ARENA_HUGE_READER_MISSING);assert(!r.h);
 }
 delete_map(&ht,p);lj_arena_hugetab_fini(&ht);puts("logical bounds exclude padding and W; initial tail page untouched");
}
static void direct_resize(PRNGState *rs) {
 TGAlloc alloc;void *p,*q;LJGC2TabWideStamp *w;la_u128 proof,zero={0,0};struct Watch *old;size_t s=65000;
 lj_arena_alloc_init(&alloc);p=lj_arena_alloc(&alloc,rs,s,LJ_AF_TRAVERSABLE);assert(p);old=watch(p,s);memset(p,0x69,s);proof=wide_seed(p);w=lj_arena_gc2_wide_acq(p);
 q=lj_arena_realloc(&alloc,rs,p,s,65392,LJ_AF_TRAVERSABLE);assert(q==p && lj_arena_gc2_wide_acq(q)==w);wide_is(q,proof);memset(q,0x69,65392);
 q=lj_arena_realloc(&alloc,rs,p,65392,65200,LJ_AF_TRAVERSABLE);assert(q==p);wide_is(q,proof);
 q=lj_arena_realloc(&alloc,rs,p,65200,65408,LJ_AF_TRAVERSABLE);assert(q && q!=p);watch(q,65408);assert(old->unmaps==1);bytes_are(q,65200,0x69);wide_is(q,zero);
 memset(q,0x79,65408);(void)wide_seed(q);bytes_are(q,65408,0x79);lj_arena_free(&alloc,q,65408);lj_arena_alloc_fini(&alloc);
 puts("private direct same-extent W stationary; crossing tail boundary moves/copies only payload");
}
static void shared_resize_and_reader(PRNGState *rs) {
 HugeTab ht={0};TGAlloc alloc;LJArenaAllocD ad;void *p,*q;LJHugeReader r={0};LJHugeInfo hi;la_u128 proof;struct Watch *old;
 assert(lj_arena_hugetab_init(&ht,4));lj_arena_alloc_init(&alloc);lj_arena_allocd_init(&ad,&alloc,rs,0);lj_arena_allocd_sethugetab(&ad,&ht);
 p=lj_arena_allocd_alloc(&ad,65000,LJ_AF_TRAVERSABLE);assert(p);old=watch(p,65000);assert(lj_arena_allocd_publish_gco(&ad,p));proof=wide_seed(p);
 assert(lj_arena_hugetab_reader_acquire(&ht,p,&r,&hi)==1);assert(lj_arena_allocf(&ad,p,65000,65392)==NULL);assert(lj_arena_allocf(&ad,p,65000,65408)==NULL);wide_is(p,proof);
 assert(lj_arena_hugetab_lookup(&ht,p,&hi) && hi.size==65000 && hi.readers==1 && !(hi.flags&LJ_HUGEF_BUSY));assert(!old->unmaps);
 assert(lj_arena_hugetab_reader_release(&r,NULL)==LJ_ARENA_HUGE_READER_RELEASED);assert(lj_arena_allocf(&ad,p,65000,0)==NULL);assert(old->unmaps==1);
 p=lj_arena_allocd_alloc(&ad,65000,0);assert(p);old=watch(p,65000);memset(p,0x41,65000);
 assert(lj_arena_allocf(&ad,p,65000,65392)==p);assert(lj_arena_hugetab_lookup(&ht,p,&hi) && hi.size==65392);memset(p,0x41,65392);
 assert(lj_arena_hugetab_reader_acquire(&ht,p,&r,&hi)==1);assert(lj_arena_allocf(&ad,p,65392,SIZE_MAX)==NULL);assert(lj_arena_hugetab_lookup(&ht,p,&hi) && hi.size==65392 && hi.readers==1 && !(hi.flags&LJ_HUGEF_BUSY));
 q=lj_arena_allocf(&ad,p,65392,65200);assert(q && q!=p);watch(q,65200);bytes_are(q,65200,0x41);bytes_are(p,65392,0x41);assert(!old->unmaps);
 assert(lj_arena_hugetab_lookup(&ht,p,&hi) && hi.size==65392 && (hi.flags&LJ_HUGEF_DEFER_FREE));assert(lj_arena_hugetab_reader_release(&r,&hi)==LJ_ARENA_HUGE_READER_HANDOFF);assert(!old->unmaps);delete_map(&ht,p);assert(old->unmaps==1);
 p=q;old=&watched[nwatch-1];q=lj_arena_allocf(&ad,p,65200,65408);assert(q && q!=p);watch(q,65408);assert(old->unmaps==1);bytes_are(q,65200,0x41);assert(lj_arena_gc2_wide_acq(q)==NULL);assert(lj_arena_allocf(&ad,q,65408,0)==NULL);
 lj_arena_alloc_fini(&alloc);lj_arena_hugetab_fini(&ht);puts("published traversable resize refuses; plain readers preserve old logical extent through deferred unmap");
}
static void failures_transfer(PRNGState *rs) {
 HugeTab src={0},dst={0};TGAlloc alloc;LJArenaAllocD ad;void *p,*q;struct Watch *old;unsigned count,calls;la_u128 proof;
 calls=mapcalls;fail_map=1;errno=ERANGE;assert(lj_arena_huge_map(rs,65393,LJ_AF_TRAVERSABLE)==NULL);assert(errno==ERANGE && mapcalls==calls+1);fail_map=0;
 lj_arena_test_gc2_sidecar_fail_alloc(1);assert(lj_arena_map(rs,LJ_AF_TRAVERSABLE)==NULL);p=lj_arena_huge_map(rs,65393,LJ_AF_TRAVERSABLE);assert(p);watch(p,65393);lj_arena_huge_unmap(p,65393);lj_arena_test_gc2_sidecar_fail_alloc(0);
 assert(lj_arena_hugetab_init(&src,0));assert(lj_arena_hugetab_init(&dst,1));lj_arena_alloc_init(&alloc);lj_arena_allocd_init(&ad,&alloc,rs,0);lj_arena_allocd_sethugetab(&ad,&src);
 p=lj_arena_allocd_alloc(&ad,65393,0);assert(p);old=watch(p,65393);memset(p,0x31,65393);
 count=nwatch;auto_watch=1;q=lj_arena_allocd_alloc(&ad,65408,LJ_AF_TRAVERSABLE);auto_watch=0;assert(!q);assert(nwatch==count+1 && watched[count].unmaps==1);assert(!old->unmaps);
 count=nwatch;auto_watch=1;q=lj_arena_allocf(&ad,p,65393,130945);auto_watch=0;assert(!q);assert(nwatch==count+1 && watched[count].unmaps==1);assert(!old->unmaps);bytes_are(p,65393,0x31);
 assert(lj_arena_allocf(&ad,p,65393,0)==NULL);assert(old->unmaps==1);
 p=lj_arena_huge_map(rs,65393,LJ_AF_TRAVERSABLE);assert(p);old=watch(p,65393);proof=wide_seed(p);assert(lj_arena_hugetab_insert(&src,p,65393,LJ_HUGEF_TRAVERSABLE|LJ_HUGEF_READY));assert(lj_arena_hugetab_transfer(&dst,&src,77));wide_is(p,proof);assert(!old->unmaps && !lj_arena_hugetab_lookup(&src,p,NULL) && lj_arena_hugetab_lookup(&dst,p,NULL));
 assert(lj_arena_hugetab_fini_all(&dst)==1);assert(old->unmaps==1);lj_arena_hugetab_fini(&src);lj_arena_alloc_fini(&alloc);puts("map/sidecar/insertion/replacement failures, transfer and terminal whole-map cleanup verified");
}
int main(int argc,char **argv) {
 PRNGState rs;unsigned i;const char *mode=argc>1?argv[1]:"all";alarm(40);setvbuf(stdout,NULL,_IOLBF,0);lj_prng_seed_fixed(&rs);
 if(!strcmp(mode,"all") || !strcmp(mode,"geometry"))geometry(&rs);
 if(!strcmp(mode,"all") || !strcmp(mode,"payload"))payload_tail_probe(&rs);
 if(!strcmp(mode,"all") || !strcmp(mode,"bounds"))bounds_and_zero(&rs);
 if(!strcmp(mode,"all") || !strcmp(mode,"resize")){direct_resize(&rs);shared_resize_and_reader(&rs);}
 if(!strcmp(mode,"all") || !strcmp(mode,"failures"))failures_transfer(&rs);
 for(i=0;i<nwatch;i++)assert(!watched[i].active && watched[i].unmaps==1);
 puts("Huge-tail targeted controls passed");return 0;
}

#define _GNU_SOURCE
#include <assert.h>
#include <inttypes.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "lj_obj.h"
#include "lj_arena.h"
#include "lj_prng.h"
#include "lj_gc2.h"
#include "lj_tg.h"
static void *ptrs[4096],*walloc[8192];static unsigned wused;
static uint64_t maps,unmaps,mapbytes,unmapbytes,callocs,frees,wcallocs,wfrees;
static int count_on;
extern void *__real_mmap64(void *,size_t,int,int,int,off64_t);
extern int __real_munmap(void *,size_t);
extern void *__real_calloc(size_t,size_t);
extern void __real_free(void *);
void *__wrap_mmap64(void *p,size_t n,int prot,int flags,int fd,off64_t off) {if(count_on){maps++;mapbytes+=n;}return __real_mmap64(p,n,prot,flags,fd,off);}
void *__wrap_mmap(void *p,size_t n,int prot,int flags,int fd,off_t off) {return __wrap_mmap64(p,n,prot,flags,fd,(off64_t)off);}
int __wrap_munmap(void *p,size_t n) {if(count_on){unmaps++;unmapbytes+=n;}return __real_munmap(p,n);}
void *__wrap_calloc(size_t n,size_t s) {void *p=__real_calloc(n,s);if(count_on){callocs++;if(n==1 && s==16 && p){assert(wused<8192);walloc[wused++]=p;wcallocs++;}}return p;}
void __wrap_free(void *p) {unsigned i;if(count_on){frees++;for(i=0;i<wused;i++)if(walloc[i]==p && p){wfrees++;walloc[i]=NULL;break;}}__real_free(p);}
static double now(void) {struct timespec t;assert(clock_gettime(CLOCK_PROCESS_CPUTIME_ID,&t)==0);return t.tv_sec+t.tv_nsec*1e-9;}
static void sample(const char *stage,global_State *g) {
 FILE *f;char line[256];size_t rss=0,priv=0,vsize=0;struct rusage r;struct mallinfo2 mi=mallinfo2();GC2StatsSnapshot st;
 memset(&st,0,sizeof(st));if(g)lj_gc2_stats_snapshot(g,&st);
 f=fopen("/proc/self/smaps_rollup","r");assert(f);while(fgets(line,sizeof(line),f)){(void)sscanf(line,"Rss: %zu kB",&rss);(void)sscanf(line,"Private_Dirty: %zu kB",&priv);}fclose(f);
 f=fopen("/proc/self/status","r");assert(f);while(fgets(line,sizeof(line),f))(void)sscanf(line,"VmSize: %zu kB",&vsize);fclose(f);assert(getrusage(RUSAGE_SELF,&r)==0);
 printf("{\"type\":\"sample\",\"stage\":\"%s\",\"rss_kb\":%zu,\"private_dirty_kb\":%zu,\"vsize_kb\":%zu,\"minor_faults\":%ld,\"major_faults\":%ld,\"malloc_arena\":%zu,\"malloc_used\":%zu,\"malloc_mmaps\":%zu,\"malloc_mmap_bytes\":%zu,\"mmap_calls\":%"PRIu64",\"mmap_requested_bytes\":%"PRIu64",\"munmap_calls\":%"PRIu64",\"munmap_requested_bytes\":%"PRIu64",\"calloc_calls\":%"PRIu64",\"free_calls\":%"PRIu64",\"calloc_1_16\":%"PRIu64",\"free_1_16\":%"PRIu64",\"gc_total\":%"PRIu64",\"gc_live\":%"PRIu64",\"phase\":%u,\"recovery\":%"PRIu64",\"cycles\":%"PRIu64"}\n",stage,rss,priv,vsize,r.ru_minflt,r.ru_majflt,mi.arena,mi.uordblks,mi.hblks,mi.hblkhd,maps,mapbytes,unmaps,unmapbytes,callocs,frees,wcallocs,wfrees,(uint64_t)st.total_bytes,st.live_estimate,st.phase,st.recovery_items,st.cycle_starts);fflush(stdout);
}
static void standalone(size_t size,unsigned n,unsigned kind,const char *touch) {
 PRNGState rs;unsigned i;double start,dt,freedt;size_t m=lj_arena_huge_mapsize(size);uint32_t flags=kind?LJ_AF_TRAVERSABLE:0;
 assert(n<=4096 && m);lj_prng_seed_fixed(&rs);sample("before",NULL);count_on=1;start=now();
 for(i=0;i<n;i++) {
  void *p=lj_arena_huge_map(&rs,size,flags);LJGC2TabWideStamp *w;assert(p);ptrs[i]=p;w=lj_arena_gc2_wide_acq(p);assert((w!=NULL)==(kind!=0));
#ifdef TAIL_W
  if(w)assert((void *)w==(char *)lj_arena_of(p)+m-16);
#endif
  if(strcmp(touch,"untouched"))memset(p,0xa7,size);
  if(!strcmp(touch,"wide")) {la_u128 old={0,0},next={1,7};assert(w);assert(la_cas128(&w->proof,&old,next));la_store64_rel(&lj_arena_gc2_stamp_acq(p)->state,UINT32_MAX);}
 }
 dt=now()-start;count_on=0;sample("retained",NULL);
 for(i=0;i<n;i++)if(strcmp(touch,"untouched")){unsigned char *p=(unsigned char *)ptrs[i];assert(p[0]==0xa7 && p[size-1]==0xa7);}
 printf("{\"type\":\"allocation\",\"workload\":\"standalone\",\"size\":%zu,\"mapsize\":%zu,\"n\":%u,\"kind\":%u,\"touch\":\"%s\",\"reserved_bytes\":%zu,\"cpu_seconds\":%.9f,\"ns_per_alloc\":%.4f}\n",size,m,n,kind,touch,m*n,dt,dt/n*1e9);
 count_on=1;start=now();for(i=0;i<n;i++)lj_arena_huge_unmap(ptrs[i],size);freedt=now()-start;count_on=0;
 printf("{\"type\":\"free\",\"cpu_seconds\":%.9f,\"ns_per_free\":%.4f}\n",freedt,freedt/n*1e9);sample("freed",NULL);
}
static void runtime(size_t size,unsigned n) {
 lua_State *L=luaL_newstate();global_State *g;unsigned i;double start,dt;size_t len=size-sizeof(GCudata),m=lj_arena_huge_mapsize(size);
 assert(L && size>sizeof(GCudata));luaL_openlibs(L);g=G(L);lua_createtable(L,n,0);assert(lua_gc(L,LUA_GCCOLLECT,0)==0);sample("before",g);
 count_on=1;start=now();for(i=1;i<=n;i++) {void *d=lua_newuserdata(L,len);GCudata *u=udataV(L->top-1);LJHugeInfo hi;assert(d && sizeudata(u)==size && lj_arena_ishuge(lj_arena_of(u)));assert(lj_arena_hugetab_lookup(&g->main_tg->huge,u,&hi) && hi.size==size);memset(d,0xa7,len);lua_rawseti(L,1,(int)i);}dt=now()-start;count_on=0;sample("retained",g);
 for(i=1;i<=n;i++) {unsigned char *d;lua_rawgeti(L,1,(int)i);d=(unsigned char *)lua_touserdata(L,-1);assert(d && d[0]==0xa7 && d[len-1]==0xa7);lua_pop(L,1);}
 printf("{\"type\":\"allocation\",\"workload\":\"runtime_userdata\",\"size\":%zu,\"user_bytes\":%zu,\"mapsize\":%zu,\"n\":%u,\"reserved_userdata_mapping_bytes\":%zu,\"cpu_seconds\":%.9f,\"ns_per_alloc\":%.4f}\n",size,len,m,n,m*n,dt,dt/n*1e9);
 count_on=1;assert(lua_gc(L,LUA_GCCOLLECT,0)==0);count_on=0;assert(gc2_phase_acq(g)==LJ_GC2_IDLE && !gc2_recovery_items_acq(g));sample("retained_collected",g);
 count_on=1;lua_settop(L,0);assert(lua_gc(L,LUA_GCCOLLECT,0)==0);assert(lua_gc(L,LUA_GCCOLLECT,0)==0);count_on=0;assert(gc2_phase_acq(g)==LJ_GC2_IDLE && !gc2_recovery_items_acq(g) && !lj_gc2_activation_reclaim_veto(g));sample("released_collected",g);
 count_on=1;lua_close(L);count_on=0;sample("closed",NULL);
}
int main(int argc,char **argv) {size_t size;unsigned n;assert(argc>=4);alarm(60);setvbuf(stdout,NULL,_IOLBF,0);size=(size_t)strtoull(argv[2],NULL,10);n=(unsigned)strtoul(argv[3],NULL,10);if(!strcmp(argv[1],"runtime"))runtime(size,n);else {assert(argc==6);standalone(size,n,(unsigned)strtoul(argv[4],NULL,10),argv[5]);}return 0;}

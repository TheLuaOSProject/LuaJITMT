#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "lj_obj.h"
#include "lj_arena.h"
#include "lj_gc2.h"
#include "lj_tg.h"
static FILE *report;
static unsigned pop(uint64_t x) { return (unsigned)__builtin_popcountll(x); }
static int census(lua_State *L)
{
 const char *stage=luaL_checkstring(L,1); int round=(int)luaL_checkinteger(L,2);
 TGAlloc *alloc=&L2TG(L)->alloc;
 unsigned k,list,bin;
 for(k=0;k<LJ_ARENA_NKINDS;k++) {
  GCArena *heads[5]={alloc->owned[k],alloc->needsweep[k],alloc->quarantine[k],alloc->reclaimed[k],k==0?alloc->empty_reclaimed:NULL};
  const char *names[5]={"owned","needsweep","quarantine","reclaimed","empty_reclaimed"};
  for(list=0;list<5;list++) {
   uint64_t count=0,blocks=0,marks=0,late=0,both=0,allocated=0,latecells=0,live=0,remoteclosed=0,remotesealed=0,remotepending=0,remoteactors=0;
   uint64_t flags[256]={0}; GCArena *a;
   for(a=heads[list];a;a=lj_arena_next_acq(a)) {
    unsigned wi,prev=0,prevblock=0,prevlate=0;uint64_t remote=lj_arena_remote_active_acq(a);
    assert(++count<1000000); flags[lj_arena_flags_acq(a)&255]++;
    live+=la_load32_acq(&a->hdr.live_cells);remoteclosed+=!!(remote&LJ_ARENA_REMOTE_CLOSED);remotesealed+=!!(remote&LJ_ARENA_REMOTE_SEALED);remotepending+=!!(remote&LJ_ARENA_REMOTE_PENDING);remoteactors+=remote&LJ_ARENA_REMOTE_COUNT_MASK;
    for(wi=LJ_AFIRST_CELL>>6;wi<LJ_ARENA_WORDS;wi++) {
     uint64_t mask=~UINT64_C(0),b,m,t,bits;
     if(wi==(LJ_AFIRST_CELL>>6))mask<<=(LJ_AFIRST_CELL&63);
     b=la_load64_acq(&a->block[wi])&mask;m=la_load64_acq(&a->mark[wi])&mask;t=la_load64_acq(&a->late[wi])&mask;
     blocks+=pop(b);marks+=pop(m);late+=pop(t);both+=pop(b&m);bits=b|m;
     while(bits) {
      unsigned bit=__builtin_ctzll(bits),cell=(wi<<6)+bit;
      if(prevblock){allocated+=cell-prev;if(prevlate)latecells+=cell-prev;}
      prev=cell;prevblock=(unsigned)((b>>bit)&1);prevlate=(unsigned)((t>>bit)&1);bits&=bits-1;
     }
    }
    if(prevblock){allocated+=LJ_ARENA_CELLS-prev;if(prevlate)latecells+=LJ_ARENA_CELLS-prev;}
   }
   fprintf(report,"{\"type\":\"arenas\",\"stage\":\"%s\",\"round\":%d,\"kind\":%u,\"list\":\"%s\",\"arenas\":%llu,\"block_starts\":%llu,\"mark_starts\":%llu,\"marked_block_starts\":%llu,\"late_starts\":%llu,\"allocated_extent_cells\":%llu,\"late_extent_cells\":%llu,\"live_cells\":%llu,\"remote_closed\":%llu,\"remote_sealed\":%llu,\"remote_pending\":%llu,\"remote_actors\":%llu,\"flags\":[",stage,round,k,names[list],(unsigned long long)count,(unsigned long long)blocks,(unsigned long long)marks,(unsigned long long)both,(unsigned long long)late,(unsigned long long)allocated,(unsigned long long)latecells,(unsigned long long)live,(unsigned long long)remoteclosed,(unsigned long long)remotesealed,(unsigned long long)remotepending,(unsigned long long)remoteactors);
   { unsigned i,comma=0;for(i=0;i<256;i++)if(flags[i])fprintf(report,"%s[%u,%llu]",comma++?",":"",i,(unsigned long long)flags[i]); }fputs("]}\n",report);
  }
  fprintf(report,"{\"type\":\"allocator\",\"stage\":\"%s\",\"round\":%d,\"kind\":%u,\"binmask\":%u,\"bump_cells\":%u,\"free_noinsert\":%u}\n",stage,round,k,alloc->binmask[k],alloc->bump[k].a?alloc->bump[k].end-alloc->bump[k].cell:0,alloc->free_noinsert);
  if(k!=LJ_ARENAK_PLAIN)continue;
  for(bin=0;bin<LJ_ALLOC_NBINS;bin++) {
   LJArenaFreeRun *run;uint64_t hist[LJ_ARENA_CELLS+1]={0},nodes=0,small=0,fitting=0,freecells=0,bad=0;
   for(run=alloc->bins[k][bin];run;run=run->next) {
    uintptr_t addr=(uintptr_t)run;uint32_t len,start;
    if(++nodes>1000000||!checkptrGC(run)||addr<LJ_ARENA_SIZE||(addr&(LJ_CELL_SIZE-1))||(addr&LJ_ARENA_MASK)<(LJ_AFIRST_CELL<<LJ_CELL_SHIFT)){bad++;break;}
    start=run->start;len=run->len;
    if(start<LJ_AFIRST_CELL||start>=LJ_ARENA_CELLS||len==0||len>LJ_ARENA_CELLS-start||lj_arena_cellof(run)!=start){bad++;break;}
    hist[len]++;freecells+=len;if(len<259)small++;else fitting++;
    if(run->next==run){bad++;break;}
   }
   if(nodes){unsigned i,comma=0;fprintf(report,"{\"type\":\"bin\",\"stage\":\"%s\",\"round\":%d,\"kind\":%u,\"bin\":%u,\"nodes\":%llu,\"below259\":%llu,\"fit259\":%llu,\"free_cells\":%llu,\"bad\":%llu,\"histogram\":[",stage,round,k,bin,(unsigned long long)nodes,(unsigned long long)small,(unsigned long long)fitting,(unsigned long long)freecells,(unsigned long long)bad);for(i=1;i<=LJ_ARENA_CELLS;i++)if(hist[i])fprintf(report,"%s[%u,%llu]",comma++?",":"",i,(unsigned long long)hist[i]);fputs("]}\n",report);}
  }
 }
 fflush(report);return 0;
}
int main(int argc,char **argv)
{
 lua_State *L;int i,status;clock_t before;struct timespec t0,t1;char ack[32];
 if(argc!=7)return 2;report=fopen(getenv("CENSUS_OUT"),"w");assert(report);
 L=luaL_newstate();luaL_openlibs(L);lua_pushcfunction(L,census);lua_setglobal(L,"bin_census");lua_newtable(L);
 for(i=1;i<=5;i++){lua_pushstring(L,argv[i+1]);lua_rawseti(L,-2,i);}lua_setglobal(L,"arg");
 status=luaL_loadfile(L,argv[1])||lua_pcall(L,0,0,0);if(status){fprintf(stderr,"%s\n",lua_tostring(L,-1));return 3;}
 before=clock();clock_gettime(CLOCK_MONOTONIC,&t0);lua_close(L);clock_gettime(CLOCK_MONOTONIC,&t1);
 printf("@closed,%.9f,%.9f\n",(double)(clock()-before)/CLOCKS_PER_SEC,(double)(t1.tv_sec-t0.tv_sec)+(t1.tv_nsec-t0.tv_nsec)*1e-9);fflush(stdout);assert(fgets(ack,sizeof(ack),stdin));fclose(report);return 0;
}

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include "lua.h"
#include "lauxlib.h"
#include "lj_obj.h"
#include "lj_arena.h"
#include "lj_gc2.h"
#include "lj_prng.h"
#include "lj_tg.h"
static int fail_maps;
static unsigned refused;
void *__real_mmap(void *, size_t, int, int, int, off_t);
void *__wrap_mmap(void *p, size_t n, int prot, int flags, int fd, off_t offset)
{
  if (fail_maps) { refused++; errno = ENOMEM; return MAP_FAILED; }
  return __real_mmap(p,n,prot,flags,fd,offset);
}
void *__real_mmap64(void *, size_t, int, int, int, off_t);
void *__wrap_mmap64(void *p, size_t n, int prot, int flags, int fd, off_t offset)
{
  if (fail_maps) { refused++; errno = ENOMEM; return MAP_FAILED; }
  return __real_mmap64(p,n,prot,flags,fd,offset);
}
int main(int argc, char **argv)
{
  lua_State *L = luaL_newstate();
  TGAlloc alloc; PRNGState rs; LJGC2Lease lease;
  unsigned char *p, *guard; void *result; size_t i; int same;
  assert(L && argc == 2);
  lua_gc(L,LUA_GCSTOP,0);lj_prng_seed_fixed(&rs);lj_arena_alloc_init(&alloc);
  lj_arena_alloc_owner_rel(&alloc,lj_tg_tid_acq(L2TG(L)));
  lj_arena_alloc_owner_tg_rel(&alloc,L2TG(L));
  lj_arena_alloc_set_registry(&alloc,(HugeTab *)gc2_small_arena_tab_acq(G(L)));
  p=(unsigned char *)lj_arena_alloc(&alloc,&rs,4096,0);
  guard=(unsigned char *)lj_arena_alloc(&alloc,&rs,64,0);assert(p&&guard);
  memset(p,0xa6,4096);memset(guard,0x5d,64);
  if (!strcmp(argv[1],"oom")) {
    LJArenaBump *b=&alloc.bump[LJ_ARENAK_PLAIN];
    while(b->cell<b->end) {
      size_t n=(size_t)(b->end-b->cell)*LJ_CELL_SIZE;
      if(n>LJ_HUGE_THRESHOLD)n=LJ_HUGE_THRESHOLD;
      assert(lj_arena_alloc(&alloc,&rs,n,0));
    }
    fail_maps=1;
  }
  assert(lj_gc2_storage_lease_try(G(L),p,4096,&lease,NULL));
  result=lj_arena_realloc(&alloc,&rs,p,4096,17,0);
  fail_maps=0;
  for(i=0;i<4096;i++)assert(p[i]==0xa6);
  for(i=0;i<64;i++)assert(guard[i]==0x5d);
  same=result==p;
  printf("{\"mode\":\"%s\",\"same_pointer\":%s,\"returned_null\":%s,\"rejected_maps\":%u,\"old_bytes_preserved\":true}\n",
    argv[1],same?"true":"false",result?"false":"true",refused);
  lj_gc2_lease_release(&lease);
  if(result)lj_arena_free(&alloc,result,17);else lj_arena_free(&alloc,p,4096);
  (void)lj_arena_remote_free_drain_force(&alloc);
  lj_arena_alloc_fini(&alloc);lua_close(L);
  return same?0:1;
}

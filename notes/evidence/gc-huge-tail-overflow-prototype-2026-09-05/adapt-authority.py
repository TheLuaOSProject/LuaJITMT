from pathlib import Path
import shutil
p=Path(__file__).parent;old=Path('/tmp/lj-dense-overflow-20260905-7tl6kcfk')
for name in ['dense-helpers.h','coalescing-adapter.c','traverse-adapter.c','wide-guards.h','t-dense-tnew.c','t-dense-fnew.c','t-dense-overflow.c']:
 shutil.copy2(old/name,p/name)
f=p/'t-dense-overflow.c';s=f.read_text()
old='''  assert(lj_arena_huge_map(&f.tg->prng, LJ_HUGE_THRESHOLD + 4096u,
                         LJ_AF_TRAVERSABLE) == NULL);'''
new='''  /* Tail W has no separate sidecar allocation to fail. Map failure and
  ** locator-insertion cleanup are exercised by the mmap64 wrapper fixture. */
  {
    size_t size = LJ_HUGE_THRESHOLD + 4096u;
    void *huge = lj_arena_huge_map(&f.tg->prng, size, LJ_AF_TRAVERSABLE);
    assert(huge && lj_arena_gc2_wide_acq(huge));
    lj_arena_huge_unmap(huge, size);
  }'''
assert old in s;s=s.replace(old,new,1);f.write_text(s)
f=p/'traverse-adapter.c';s=f.read_text();start=s.index('static void test_table_token_huge_phase_behavior(');end=s.index('static void test_table_token_huge_reclaiming_owner(',start);x=s[start:end]
old='''    void *poison;
    assert(pagesize > 0);
    poison = mmap(NULL, (size_t)pagesize, PROT_NONE,
                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(poison != MAP_FAILED);'''
new='''    void *poison = (char *)lj_arena_of(t) + pagesize;
    size_t protected_bytes = lj_arena_huge_mapsize(size + n) -
      (size_t)pagesize;
    assert(pagesize > 0);'''
assert old in x;x=x.replace(old,new,1)
old='''    /* Header-only completion cannot follow a W pointer, even in wide mode. */
    la_storeptr_rel((void **)&lj_arena_of(t)->hdr.gc2_huge_wide, poison);
    assert(lj_gc2_test_table_token_scan_one(g, t) == 1);
    la_storeptr_rel((void **)&lj_arena_of(t)->hdr.gc2_huge_wide, saved_wide);
    assert(munmap(poison, (size_t)pagesize) == 0);'''
new='''    /* Protect every later payload/padding page, including the actual W
    ** tail. The fixed header and deliberately poisoned gct stay readable. */
    assert((void *)saved_wide == (char *)lj_arena_of(t) +
      lj_arena_huge_mapsize(size + n) - sizeof(*saved_wide));
    assert(mprotect(poison, protected_bytes, PROT_NONE) == 0);
    assert(lj_gc2_test_table_token_scan_one(g, t) == 1);
    assert(mprotect(poison, protected_bytes, PROT_READ|PROT_WRITE) == 0);
    assert(lj_arena_of(t)->hdr.gc2_huge_wide == saved_wide);'''
assert old in x;x=x.replace(old,new,1);s=s[:start]+x+s[end:];f.write_text(s)

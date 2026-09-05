from pathlib import Path
p=Path(__file__).parent
f=p/'t-dense-fnew.c';s=f.read_text();(p/'t-dense-fnew-original-failure.c').write_text(s)
old='''  begin_open_fnew_mark(g, tg);
  cycle = gc2_cycle_acq(g);
  node = lj_tg_ssb_active_acq(tg);'''
assert old in s
s=s.replace(old,'''  begin_open_fnew_mark(g, tg);
  cycle = gc2_cycle_acq(g);
  /* Explicit setup control: publish carried producer work before requiring
  ** a fresh capacity boundary, matching the independently checked base. */
  (void)lj_gc2_flush_ssb(g, tg);
  node = lj_tg_ssb_active_acq(tg);''',1)
start=s.index('static void establish_persistent_fnew_root_snapshot(')
end=s.index('\ntypedef struct FNewEnvRaceCtx',start)
x=s[start:end]
x=x.replace('  uint32_t i;\n','''  uint32_t i;
  /* Explicit scheduling control, not a discarded producer buffer. */
  for (i = 0; i < 64u && !fnew_mark_work_empty(g, tg); i++) {
    lj_gc2_jit_mark_request_exit(g);
    (void)lj_gc2_worker_drain(g, 1u << 20);
  }
  assert(fnew_mark_work_empty(g, tg));
''',1)
x=x.replace('    assert(!lj_gc2_mark_complete(g, L, 1, 1));','    lj_gc2_jit_mark_request_exit(g);\n    assert(!lj_gc2_mark_complete(g, L, 1, 1));',1)
s=s[:start]+x+s[end:];(p/'t-dense-fnew-settled.c').write_text(s)
f=p/'traverse-adapter.c';s=f.read_text();(p/'traverse-adapter-before-protection.c').write_text(s)
s=s.replace('#include <string.h>','#include <string.h>\n#include <sys/mman.h>\n#include <unistd.h>',1)
start=s.index('static void test_table_token_huge_phase_behavior(');end=s.index('static void test_table_token_huge_reclaiming_owner(',start);x=s[start:end]
x=x.replace('    uint8_t gct;','''    uint8_t gct;
    LJGC2TabWideStamp *saved_wide;
    long pagesize = sysconf(_SC_PAGESIZE);
    void *poison;
    assert(pagesize > 0);
    poison = mmap(NULL, (size_t)pagesize, PROT_NONE,
                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(poison != MAP_FAILED);''',1)
x=x.replace('    assert(!lj_arena_hugetab_claim_external_free(&tg->huge, t, &hi));','''    saved_wide = lj_arena_gc2_wide_acq(t);
    assert(saved_wide != NULL);
    assert(!lj_arena_hugetab_claim_external_free(&tg->huge, t, &hi));''',1)
x=x.replace('    assert(lj_gc2_test_table_token_scan_one(g, t) == 1);\n    assert(gc2_table_token_scan_payloads_acq(g) == payload0);','''    /* Header-only completion cannot follow a W pointer, even in wide mode. */
    la_storeptr_rel((void **)&lj_arena_of(t)->hdr.gc2_huge_wide, poison);
    assert(lj_gc2_test_table_token_scan_one(g, t) == 1);
    la_storeptr_rel((void **)&lj_arena_of(t)->hdr.gc2_huge_wide, saved_wide);
    assert(munmap(poison, (size_t)pagesize) == 0);
    assert(gc2_table_token_scan_payloads_acq(g) == payload0);''',1)
s=s[:start]+x+s[end:]
start=s.index('static void test_table_token_small_free_no_body(');end=s.index('static void test_table_token_small_proof_races(',start);x=s[start:end]
x=x.replace('  uint8_t gct;','''  uint8_t gct;
  LJGC2TabWideStamp *wide;
  long pagesize = sysconf(_SC_PAGESIZE);
  void *page;
  assert(pagesize > 0);''',1)
x=x.replace('  payload0 = gc2_table_token_scan_payloads_acq(g);','''  wide = lj_arena_gc2_wide_acq(t);
  assert(wide != NULL);
  page = (void *)((uintptr_t)wide & ~((uintptr_t)pagesize - 1u));
  assert((uintptr_t)page >= (uintptr_t)lj_arena_gc2_tabstamp_acq(a) +
         offsetof(LJGC2TabStampArena, wide));
  payload0 = gc2_table_token_scan_payloads_acq(g);''',1)
x=x.replace('  assert(lj_gc2_test_table_token_scan_one(g, t) == 1);','''  /* Inline token storage stays readable while W is inaccessible. */
  assert(mprotect(page, (size_t)pagesize, PROT_NONE) == 0);
  assert(lj_gc2_test_table_token_scan_one(g, t) == 1);
  assert(mprotect(page, (size_t)pagesize, PROT_READ | PROT_WRITE) == 0);''',1)
s=s[:start]+x+s[end:];f.write_text(s)
f=p/'validate.py';s=f.read_text().replace("p/'t-dense-fnew.c'","p/'t-dense-fnew-settled.c'");f.write_text(s)

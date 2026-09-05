from pathlib import Path
import difflib
import subprocess

P = Path(__file__).resolve().parent
T = P / 'tree'
OLD = Path('/tmp/lj-dense-huge-tail-20260905-djqhqfe8')
def replace(s, old, new):
    assert s.count(old) == 1, (old, s.count(old))
    return s.replace(old, new)
def naming(s):
    return s.replace('dense_seed', 'ljt_gc2_wide_seed').replace('dense_snapshot', 'ljt_gc2_wide_snapshot').replace('dense-helpers.h', 'lib/gc2_wide_fixture_helpers.h').replace('wide-guards.h', 'lib/gc2_wide_reuse_helpers.h')

helpers = naming((OLD / 'dense-helpers.h').read_text()).replace('DENSE_HELPERS_H', 'LJT_GC2_WIDE_FIXTURE_HELPERS_H')
helpers = helpers.replace('/* Test-only namespace compression, used while the fixture owns/stops actors. */', '/* Test-only namespace compression. The caller owns private storage, holds\n** exact allocation authority, or has stopped every actor that can mutate W.\n** A sampled address or stamp alone is not permission to read a body. */')
(T / 'tests/lib/gc2_wide_fixture_helpers.h').write_text(helpers)
guards = (OLD / 'wide-guards.h').read_text()
guards = '#ifndef LJT_GC2_WIDE_REUSE_HELPERS_H\n#define LJT_GC2_WIDE_REUSE_HELPERS_H\n' + guards + '\n#endif\n'
(T / 'tests/lib/gc2_wide_reuse_helpers.h').write_text(guards)

# Extend the current fixture; no obsolete copied coalescing source is added.
s = (T / 'tests/t-gc2-sweep-table-coalescing.c').read_text()
s = replace(s, '#include "lj_tg.h"', '#include "lj_tg.h"\n#include "lualib.h"\n#include "lib/gc2_wide_fixture_helpers.h"')
assert s.count('la_store64_rel(&f.stamp->state, UINT32_MAX);') == 2
s = s.replace('la_store64_rel(&f.stamp->state, UINT32_MAX);', 'ljt_gc2_wide_seed(f.parent, UINT64_MAX, UINT32_MAX, 0, 1);')
extra = (OLD / 't-dense-overflow.c').read_text()
extra = extra[extra.index('static uint32_t exact_scan_result;'):extra.index('int main(int argc, char **argv)')]
extra = naming(extra).replace('DENSE_WRAP_CALLOC', 'LJ_TEST_WRAP_CALLOC')
s = replace(s, 'int main(int argc, char **argv)', extra + '\nint main(int argc, char **argv)')
s = replace(s, '  int huge, api, cyclic;', '  int huge, api, cyclic, wide, exact;')
s = replace(s, '  alarm(30);', '  alarm(60);')
s = replace(s, '  puts("t-gc2-sweep-table-coalescing OK:', '''  if (!strcmp(mode, "all") || !strcmp(mode, "wide")) {
    for (huge = 0; huge < 2; huge++) {
      for (wide = 0; wide < 2; wide++)
        for (exact = 0; exact < 2; exact++)
          test_old_scanner(huge, wide, exact);
      test_mode_pause(huge, LJ_GC2_TABLE_COALESCE_TEST_PRE_MODE);
      test_mode_pause(huge, LJ_GC2_TABLE_COALESCE_TEST_POST_MODE);
      test_continued_collection(huge);
    }
    test_reserved_failure();
  }
  puts("t-gc2-sweep-table-coalescing OK:''')
(T / 'tests/t-gc2-sweep-table-coalescing.c').write_text(s)

# Apply the proven focused traversal changes to the current file, but retain
# its ordinary inline token coverage. Only the protected controls force wide.
s = naming((OLD / 'traverse-adapter.c').read_text())
global_promotion = '''  if ((uint32_t)la_load64_acq(&stamp->state) != UINT32_MAX) {
    uint64_t inline_state = la_load64_acq(&stamp->state);
    ljt_gc2_wide_seed(t, 0, (uint32_t)inline_state, (uint32_t)(inline_state >> 32), 1);
  }
'''
s = replace(s, global_promotion, '')
s = replace(s, '  /* Dirty saturation must invalidate the covered scan cycle without wrapping\n  ** the persistent per-cell identity back to one. Use a private universe\n  ** because NO_RECLAIM is intentionally absorbing. */', '  /* Only exhaustion of the full era/serial namespace is terminal. It must\n  ** invalidate coverage without reusing an old authority. Use a private\n  ** universe because NO_RECLAIM is intentionally absorbing. */')
s = replace(s, '#include <sys/mman.h>\n#include <unistd.h>\n', '')
s = replace(s, '#include "lj_obj.h"', '#include "lj_obj.h"\n#if LJ_TARGET_LINUX\n#include <sys/mman.h>\n#include <unistd.h>\n#endif')
# Guard only the Linux protection probes; preserve cross-platform semantics.
blocks = [
'''    LJGC2TabWideStamp *saved_wide;
    long pagesize = sysconf(_SC_PAGESIZE);
    void *poison = (char *)lj_arena_of(t) + pagesize;
    size_t protected_bytes = lj_arena_huge_mapsize(size + n) -
      (size_t)pagesize;
    assert(pagesize > 0);
''',
'''    saved_wide = lj_arena_gc2_wide_acq(t);
    assert(saved_wide != NULL);
''',
'''    /* Protect every later payload/padding page, including the actual W
    ** tail. The fixed header and deliberately poisoned gct stay readable. */
    assert((void *)saved_wide == (char *)lj_arena_of(t) +
      lj_arena_huge_mapsize(size + n) - sizeof(*saved_wide));
    assert(mprotect(poison, protected_bytes, PROT_NONE) == 0);
''',
'''    assert(mprotect(poison, protected_bytes, PROT_READ|PROT_WRITE) == 0);
    assert(lj_arena_of(t)->hdr.gc2_huge_wide == saved_wide);
''',
'''  LJGC2TabWideStamp *wide;
  long pagesize = sysconf(_SC_PAGESIZE);
  void *page;
  assert(pagesize > 0);
''',
'''  wide = lj_arena_gc2_wide_acq(t);
  assert(wide != NULL);
  page = (void *)((uintptr_t)wide & ~((uintptr_t)pagesize - 1u));
  assert((uintptr_t)page >= (uintptr_t)lj_arena_gc2_tabstamp_acq(a) +
         offsetof(LJGC2TabStampArena, wide));
''',
'''  /* Inline token storage stays readable while W is inaccessible. */
  assert(mprotect(page, (size_t)pagesize, PROT_NONE) == 0);
''',
'''  assert(mprotect(page, (size_t)pagesize, PROT_READ | PROT_WRITE) == 0);
''']
for b in blocks:
    s = replace(s, b, '#if LJ_TARGET_LINUX\n' + b + '#endif\n')
start = s.index('static void test_table_token_huge_phase_behavior(')
end = s.index('static void test_table_token_huge_reclaiming_owner(', start)
part = s[start:end]
part = replace(part, '    stamp = table_token_test_stamp(t);', '    stamp = table_token_test_stamp(t);\n    ljt_gc2_wide_seed(t, 9u, 1u, 0, 1);')
s = s[:start] + part + s[end:]
start = s.index('static void test_table_token_small_free_no_header(') if 'static void test_table_token_small_free_no_header(' in s else s.rfind('static void ', 0, s.index('  /* Inline token storage stays readable'))
end = s.index('\nstatic ', start + 1)
part = s[start:end]
part = replace(part, '  stamp = table_token_test_stamp(t);', '  stamp = table_token_test_stamp(t);\n  ljt_gc2_wide_seed(t, 11u, 1u, 0, 1);')
s = s[:start] + part + s[end:]
# No production traversal source changed between 28de and ff2; require that
# premise explicitly instead of copying an obsolete full fixture blindly.
base_current = subprocess.check_output(['git', 'show', 'ff2a6ca0:tests/t-gc2-traverse.c'], cwd='/workspaces/lj-lockless')
assert base_current == (OLD / 'calloc/tests/t-gc2-traverse.c').read_bytes()
(T / 'tests/t-gc2-traverse.c').write_text(s)

# Extend the current TNEW fixture with its four exact high-cell cases.
s = (T / 'tests/t-x64-tnew-empty-inline.c').read_text()
s = replace(s, '#include "lib/lua_fixture_helpers.h"', '#include "lib/lua_fixture_helpers.h"\n#include "lib/gc2_wide_fixture_helpers.h"\n#include "lib/gc2_wide_reuse_helpers.h"')
extra = (OLD / 't-dense-tnew.c').read_text()
extra = naming(extra[extra.index('static void high_cell_case('):extra.index('int main(int argc, char **argv)')])
extra = extra.replace('high_cell_case', 'test_wide_cell_reuse')
s = replace(s, 'int main(void)', extra + '\nint main(void)')
s = replace(s, '  lua_close(L);\n  puts("t-x64-tnew-empty-inline OK:', '''  lua_close(L);
  test_wide_cell_reuse(1536u, 0);
  test_wide_cell_reuse(1537u, 0);
  test_wide_cell_reuse(1536u, 1);
  test_wide_cell_reuse(1537u, 1);
  puts("t-x64-tnew-empty-inline OK:''')
(T / 'tests/t-x64-tnew-empty-inline.c').write_text(s)

# Apply only the proven emitted-reuse hunks to the truthful latest FNEW fixture.
s = (T / 'tests/t-jit-fnew-bump.c').read_text()
s = replace(s, '#include "lib/lua_fixture_helpers.h"', '#include "lib/lua_fixture_helpers.h"\n#include "lib/gc2_wide_fixture_helpers.h"\n#include "lib/gc2_wide_reuse_helpers.h"\nstatic uint32_t wide_test_bump_start;')
s = replace(s, '  alloc->bump[LJ_ARENAK_TRAVERSABLE].cell = LJ_AFIRST_CELL;', '  alloc->bump[LJ_ARENAK_TRAVERSABLE].cell =\n    wide_test_bump_start ? wide_test_bump_start : LJ_AFIRST_CELL;')
s = replace(s, '  la_store64_rel(&stamp->state, state);\n  return control;', '''  la_store64_rel(&stamp->state, state);
  if (wide_test_bump_start)
    ljt_gc2_wide_seed(lj_arena_cellptr(a, cell), UINT64_C(0xfedcba9876543210),
                     (uint32_t)state, (uint32_t)(state >> 32), 1);
  return control;''')
s = replace(s, '  assert(fnstamp != NULL && uvstamp != NULL);', '  assert(fnstamp != NULL && uvstamp != NULL);\n  if (wide_test_bump_start) wide_guards_arm(stamp_a, stamp_fncell, fncells);')
s = replace(s, '  assert(la_load64_acq(&fnstamp->state) == 0);', '''  if (wide_test_bump_start) {
    wide_guards_check();
    assert(ljt_gc2_wide_snapshot(first).hi == UINT64_C(0xfedcba9876543210));
    assert(ljt_gc2_wide_snapshot(first).lo ==
      ((UINT64_C(0x13572468) << 32) | UINT64_C(0x11111111)));
    assert(ljt_gc2_wide_snapshot(firstuv).hi == UINT64_C(0xfedcba9876543210));
    assert(ljt_gc2_wide_snapshot(firstuv).lo ==
      ((UINT64_C(0x24681357) << 32) | UINT64_C(0x22222222)));
  }
  assert(la_load64_acq(&fnstamp->state) == 0);''')
extra = '''static void test_wide_pair_reuse(uint32_t cell)
{
  lua_State *L = luaL_newstate();
  assert(L != NULL);
  wide_test_bump_start = cell;
  luaL_openlibs(L);
  assert(lua_checkstack(L, 64));
  assert(lua_gc(L, LUA_GCCOLLECT, 0) == 0);
  lj_gc_threshold_store(G(L), UINT64_MAX / 2u);
  lj_gc2_hard_store(G(L), UINT64_MAX / 2u);
  lj_gc2_trigger_store(G(L), UINT64_MAX / 2u);
  test_traced_immutable_numeric_inline(L, G(L));
  assert_no_unfinished_owned_constructors(L2TG(L));
  lua_close(L);
  wide_test_bump_start = 0;
  printf("FNEW cell %u: persistent W and both exact token guards passed\\n", cell);
}

'''
s = replace(s, 'int main(void)', extra + 'int main(void)')
s = replace(s, '  lua_close(L);\n  puts("t-jit-fnew-bump OK");', '  lua_close(L);\n  test_wide_pair_reuse(1536u);\n  test_wide_pair_reuse(1537u);\n  puts("t-jit-fnew-bump OK");')
(T / 'tests/t-jit-fnew-bump.c').write_text(s)

(T / 'tests/t-arena-huge-tail.c').write_text((OLD / 't-huge-tail.c').read_text())

# Canonical registrations: current tests retain their names and gain coverage.
s = (T / 'tests/suites/m3_gc.lua').read_text()
start = s.index('    name = "m3_gc2_sweep_table_coalescing",')
end = s.index('\n  register({', start)
part = s[start:end]
part = part.replace('admitted SWEEP requests coalesce behind complete table scans', 'SWEEP coalescing, persistent overflow proof and paused publisher progress')
part = replace(part, '      local flags = gc2_test_cflags .. " -DLUA_USE_ASSERT"', '      local flags = gc2_test_cflags ..\n                    " -DLJ_ARENA_TEST_HELPERS -DLUA_USE_ASSERT"')
part = replace(part, '          cflags = flags,\n          timeout = "40s"', '''          cflags = flags .. " -DLJ_TEST_WRAP_CALLOC",
          libs = {
            "-lm", "-ldl", os.getenv("PTHREAD") or "-pthread",
            "-Wl,--wrap=calloc"
          },
          timeout = "90s"''')
s = s[:start] + part + s[end:]
(T / 'tests/suites/m3_gc.lua').write_text(s)
s = (T / 'tests/suites/m5_x64.lua').read_text()
s = replace(s, '                            build.tab_helper_opts())', '''                            build.tab_helper_opts({
                              xcflags = "-DLJ_TAB_TEST_HELPERS -DLJ_GC2_TEST_HELPERS",
                              cflags = "-DLJ_TAB_TEST_HELPERS -DLJ_GC2_TEST_HELPERS"
                            }))''')
(T / 'tests/suites/m5_x64.lua').write_text(s)
s = (T / 'tests/suites/m2_arena.lua').read_text()
s = replace(s, '  "m2_arena_hugetab",', '  "m2_arena_hugetab",\n  "m2_arena_huge_tail",')
new = '''  register({
    name = "m2_arena_huge_tail",
    description = "Huge overflow tail geometry, failure, reader and realloc lifetime",
    run = function(t)
      if jit.os ~= "Linux" then
        print("M2 Huge-tail fixture requires Linux mmap64 and linker wrappers")
        return
      end
      local flags = "-DLJ_ARENA_TEST_HELPERS -DLUA_USE_ASSERT"
      build.with_default_build_restore(t, function()
        build.clean_build(t, { quiet = true, xcflags = flags })
        build.compile_and_run_c(t, t:tmp("lj-t-arena-huge-tail"),
                               "t-arena-huge-tail.c", {
          cflags = flags,
          libs = {
            "-lm", "-ldl", os.getenv("PTHREAD") or "-pthread",
            "-Wl,--wrap=mmap", "-Wl,--wrap=mmap64", "-Wl,--wrap=munmap",
            "-Wl,--wrap=calloc", "-Wl,--wrap=free"
          },
          timeout = "60s"
        })
      end)
    end
  })

'''
needle = '  register({\n    name = "m2_arena_hugetab",'
s = replace(s, needle, new + needle)
(T / 'tests/suites/m2_arena.lua').write_text(s)
print(P)

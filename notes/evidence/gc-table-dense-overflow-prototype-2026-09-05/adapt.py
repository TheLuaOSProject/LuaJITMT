from pathlib import Path
import shutil
p=Path(__file__).parent; t=p/'strict'
old=Path('/tmp/lj-wide-stamp-corrected-20260905-cqq3p87i')
s=(t/'tests/t-gc2-sweep-table-coalescing.c').read_text()
s=s.replace('#include "lj_tg.h"','#include "lj_tg.h"\n#include "dense-helpers.h"')
s=s.replace('la_store64_rel(&f.stamp->state, UINT32_MAX);','dense_seed(f.parent, UINT64_MAX, UINT32_MAX, 0, 1);')
(p/'coalescing-adapter.c').write_text(s)
s=(t/'tests/t-gc2-traverse.c').read_text()
s=s.replace('#include "lj_tg.h"','#include "lj_tg.h"\n#include "dense-helpers.h"')
s=s.replace('''  la_store64_rel(&stamp->state,
\t\t ((uint64_t)UINT32_C(17) << 32) | (UINT32_MAX - 1u));''',
'''  dense_seed(t, UINT64_MAX, UINT32_MAX - 2u, 17u, 1);''')
s=s.replace('state = la_load64_acq(&stamp->state);','state = dense_snapshot(t).lo;')
s=s.replace('assert((uint32_t)state == UINT32_MAX);','assert((uint32_t)state == UINT32_MAX - 1u);',1)
(p/'traverse-adapter.c').write_text(s)
shutil.copy(old/'tnew-original.c',p/'tnew-original.c')
s=(old/'t-wide-tnew.c').read_text().replace('#include "wide-guards.h"','#include "wide-guards.h"\n#include "dense-helpers.h"')
s=s.replace('la_store64_rel(&s->era, UINT64_C(0xfedcba9876543210));',
'''dense_seed(lj_arena_cellptr(a, cell), UINT64_C(0xfedcba9876543210),
             2718u, gc2_cycle_acq(g), 1);''')
s=s.replace('assert(la_load64_acq(&s->era) == 0);',
'''assert(dense_snapshot(result).hi == UINT64_C(0xfedcba9876543210));
    assert((uint32_t)dense_snapshot(result).lo == 2718u);
    la_store64_rel(&s->state, UINT32_MAX - 1u);
    lj_gc2_test_table_dirty_bump(g, result);
    assert(la_load64_acq(&s->state) == UINT32_MAX);
    assert(dense_snapshot(result).hi == UINT64_C(0xfedcba9876543210));
    assert((uint32_t)dense_snapshot(result).lo == 2719u);
    assert((uint32_t)(dense_snapshot(result).lo >> 32) == 0);''')
s=s.replace('assert(la_load64_acq(&s->era) == UINT64_C(0xfedcba9876543210));',
'''assert(dense_snapshot(lj_arena_cellptr(a, cell)).hi ==
           UINT64_C(0xfedcba9876543210));''')
s=s.replace('exact full reset','inline reset/wide persistence and promotion')
(p/'t-dense-tnew.c').write_text(s)
s=(old/'t-wide-fnew.c').read_text().replace('#include "wide-guards.h"','#include "wide-guards.h"\n#include "dense-helpers.h"')
s=s.replace('la_store64_rel(&stamp->era, UINT64_C(0xfedcba9876543210));',
'''dense_seed(lj_arena_cellptr(a, cell), UINT64_C(0xfedcba9876543210),
             (uint32_t)state, (uint32_t)(state >> 32), 1);''')
s=s.replace('assert(la_load64_acq(&fnstamp->era) == 0);',
'''assert(dense_snapshot(first).hi == UINT64_C(0xfedcba9876543210));
  assert(dense_snapshot(first).lo ==
    ((UINT64_C(0x13572468) << 32) | UINT64_C(0x11111111)));''')
s=s.replace('assert(la_load64_acq(&uvstamp->era) == 0);',
'''assert(dense_snapshot(firstuv).hi == UINT64_C(0xfedcba9876543210));
  assert(dense_snapshot(firstuv).lo ==
    ((UINT64_C(0x24681357) << 32) | UINT64_C(0x22222222)));''')
s=s.replace('both full resets','both inline resets/wide persistence')
(p/'t-dense-fnew.c').write_text(s)
s=(old/'wide-guards.h').read_text()
s=s.replace('  LJGC2TabStamp *stamp;','  LJGC2TabStamp *stamp;\n  LJGC2TabWideStamp *wide;\n  uint64_t inline_state;')
s=s.replace('  la_u128 proof;','  la_u128 proof;')
s=s.replace('la_store64_rel(&s->era, UINT64_C(0x2718281800000000) + i);',
'''{
      LJGC2TabWideStamp *w = lj_arena_gc2_wide_acq(lj_arena_cellptr(a, first+i));
      la_u128 old = lj_arena_gc2_wide_snapshot(w);
      la_u128 next = { UINT64_C(0x55550000) + i,
                       UINT64_C(0x2718281800000000) + i };
      assert(la_cas128(&w->proof, &old, next));
      wide_guards[i].wide = w;
    }''')
s=s.replace('wide_guards[i].proof = lj_arena_gc2_stamp_snapshot(s);',
'''wide_guards[i].proof = lj_arena_gc2_wide_snapshot(wide_guards[i].wide);
    wide_guards[i].inline_state = la_load64_acq(&s->state);''')
s=s.replace('la_u128 p = lj_arena_gc2_stamp_snapshot(wide_guards[i].stamp);',
'''la_u128 p = lj_arena_gc2_wide_snapshot(wide_guards[i].wide);
    assert(la_load64_acq(&wide_guards[i].stamp->state) == wide_guards[i].inline_state);''')
(p/'wide-guards.h').write_text(s)
print(p)


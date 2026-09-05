from pathlib import Path
p=Path(__file__).parent; t=p/'strict'
def edit(name, pairs):
 f=t/name; s=f.read_text()
 for old,new in pairs:
  assert old in s, (name,old[:90])
  s=s.replace(old,new,1)
 f.write_text(s)
edit('src/lj_arena.h',[
 ('typedef struct LJGC2TabStampArena {\n  LJGC2TabStamp cell[LJ_ARENA_CELLS];\n} LJGC2TabStampArena;', '''/* The ordinary entry and token geometry remain unchanged. Overflow proof
** storage is reserved before the mapping is published, never on a barrier. */
typedef struct LJGC2TabWideStamp {
  la_u128 proof;  /* lo={covered_cycle32,dirty32}, hi=nonwrapping era64. */
} LJGC2TabWideStamp;

typedef struct LJGC2TabStampArena {
  LJGC2TabStamp cell[LJ_ARENA_CELLS];
  LJGC2TabWideStamp wide[LJ_ARENA_CELLS];
} LJGC2TabStampArena;

LJ_STATIC_ASSERT(sizeof(LJGC2TabStamp) == 16u);
LJ_STATIC_ASSERT(offsetof(LJGC2TabStamp, token) == 8u);

static LJ_AINLINE la_u128 lj_arena_gc2_wide_snapshot(LJGC2TabWideStamp *w)
{
  la_u128 old = { 0, 0 }, same;
  do { same = old; } while (!la_cas128(&w->proof, &old, same));
  return old;
}'''),
 ('  LJGC2TabStampArena *gc2_tabstamp;\n', '''  /* Immutable mapping kind selects this union. Small traversable mappings
  ** own the full inline+wide cell sidecar; huge mappings own one reserved W.
  ** Plain mappings leave the pointer NULL. Both arms live until final unmap. */
  union {
    LJGC2TabStampArena *gc2_tabstamp;
    LJGC2TabWideStamp *gc2_huge_wide;
  };
'''),
 ('/* Physical table-rescan ownership for one retained small mapping.', '''/* Body-proof lookup, deliberately separate from the header-only token APIs.
** Exact mapping AND readable allocation admission must already be retained.
** Wide serials persist across cell reuse; private construction resets inline
** only. The promotion invalidation precedes exposing the inline sentinel. */
static LJ_AINLINE LJGC2TabWideStamp *lj_arena_gc2_wide_acq(const void *p)
{
  GCArena *a;
  LJGC2TabStampArena *side;
  uint32_t flags, cell;
  if (!p)
    return NULL;
  a = lj_arena_of(p);
  flags = lj_arena_flags_acq(a);
  if ((flags & LJ_AF_HUGE_MAGIC) == LJ_AF_HUGE_MAGIC)
    return (LJGC2TabWideStamp *)la_loadptr_acq(
      (void *const *)&a->hdr.gc2_huge_wide);
  if (!(flags & LJ_AF_TRAVERSABLE))
    return NULL;
  cell = lj_arena_cellof(p);
  if (cell < LJ_AFIRST_CELL || cell >= LJ_ARENA_CELLS)
    return NULL;
  side = lj_arena_gc2_tabstamp_acq(a);
  return side ? &side->wide[cell] : NULL;
}

/* Physical table-rescan ownership for one retained small mapping.''')
])
edit('src/lj_arena.c',[
 ('  a->hdr.live_cells = (uint32_t)(mapsize >> LJ_CELL_SHIFT);\n  return (void *)((char *)a + sizeof(GCAhdr));', '''  a->hdr.live_cells = (uint32_t)(mapsize >> LJ_CELL_SHIFT);
  if (flags & LJ_AF_TRAVERSABLE) {
    LJGC2TabWideStamp *wide = NULL;
    if (!arena_test_gc2_sidecar_alloc_fails())
      wide = (LJGC2TabWideStamp *)calloc(1, sizeof(*wide));
    if (!wide) {
      arena_unmap_aligned(a, mapsize);
      return NULL;  /* No mapping, header or body has been published. */
    }
    la_storeptr_rel((void **)&a->hdr.gc2_huge_wide, wide);
  }
  return (void *)((char *)a + sizeof(GCAhdr));'''),
 ('''    if (lj_arena_gc2_tokens_empty_acq(a) &&
\tlj_arena_gc2_desc_mapping_clear_acq(a))
      arena_unmap_aligned((void *)a, mapsize);''', '''    if (lj_arena_gc2_tokens_empty_acq(a) &&
\tlj_arena_gc2_desc_mapping_clear_acq(a)) {
      free(la_loadptr_acq((void *const *)&a->hdr.gc2_huge_wide));
      arena_unmap_aligned((void *)a, mapsize);
    }'''),
 ('''  if (p && mapsize)
    arena_unmap_aligned((void *)lj_arena_of(p), mapsize);''', '''  if (p && mapsize) {
    GCArena *a = lj_arena_of(p);
    free(la_loadptr_acq((void *const *)&a->hdr.gc2_huge_wide));
    arena_unmap_aligned((void *)a, mapsize);
  }''')
])
edit('src/lj_gc2.h',[
 ('#define LJ_GC2_TABLE_COALESCE_TEST_MARK_ENTER\t5u', '''#define LJ_GC2_TABLE_COALESCE_TEST_MARK_ENTER\t5u
#define LJ_GC2_TABLE_COALESCE_TEST_PRE_MODE\t6u
#define LJ_GC2_TABLE_COALESCE_TEST_POST_MODE\t7u''')
])
f=t/'src/lj_gc2.c'; s=f.read_text()
start=s.index('static LJ_AINLINE uint32_t gc2_table_dirty_epoch(')
end=s.index('static LJ_AINLINE void gc2_table_dirty_bump_parent(',start)
s=s[:start]+'''/* Captured before payload loads. An old inline scan never adopts W after
** promotion; an old wide scan must match both serial and era. */
typedef struct GC2TabAuthority {
  la_u128 proof;
  uint8_t wide;
  uint8_t stamped;
} GC2TabAuthority;

static LJ_AINLINE GC2TabAuthority gc2_table_authority(global_State *g, GCtab *t)
{
  GC2TabAuthority a = { { 0, 0 }, 0, 0 };
  LJGC2TabStamp *s = gc2_table_stamp(g, t);
  if (s) {
    a.stamped = 1;
    a.proof.lo = la_load64_acq(&s->state);
    if (gc2_tabstamp_dirty(a.proof.lo) == UINT32_MAX) {
      LJGC2TabWideStamp *w = lj_arena_gc2_wide_acq(t);
      a.wide = 1;
      if (!w) {
        a.stamped = 0;
        gc2_activation_pin_no_reclaim(g);
      } else {
        a.proof = lj_arena_gc2_wide_snapshot(w);
      }
    }
  }
  return a;
}

static LJ_AINLINE int gc2_table_authority_terminal(GC2TabAuthority a)
{
  return a.wide && a.proof.hi == UINT64_MAX &&
    gc2_tabstamp_dirty(a.proof.lo) == UINT32_MAX;
}

static LJ_AINLINE int gc2_table_scan_publish(global_State *g, GCtab *t,
                                             uint32_t cycle,
                                             GC2TabAuthority captured)
{
  LJGC2TabStamp *s = gc2_table_stamp(g, t);
  uint32_t dirty = gc2_tabstamp_dirty(captured.proof.lo);
  if (!s || !captured.stamped)
    return 0;
  if (captured.wide) {
    LJGC2TabWideStamp *w = lj_arena_gc2_wide_acq(t);
    la_u128 old, next;
    if (!w || gc2_tabstamp_dirty(la_load64_acq(&s->state)) != UINT32_MAX)
      return 0;
    old = lj_arena_gc2_wide_snapshot(w);
    for (;;) {
      if (old.hi != captured.proof.hi || gc2_tabstamp_dirty(old.lo) != dirty)
        return 0;
      next.lo = gc2_tabstamp_pack(cycle, dirty);
      next.hi = old.hi;
      if (la_cas128(&w->proof, &old, next))
        return 1;
    }
  } else {
    uint64_t old = la_load64_acq(&s->state);
    for (;;) {
      uint64_t next;
      if (gc2_tabstamp_dirty(old) == UINT32_MAX ||
          gc2_tabstamp_dirty(old) != dirty)
        return 0;
      next = gc2_tabstamp_pack(cycle, dirty);
      if (la_cas64(&s->state, &old, next, LA_ACQ_REL, LA_ACQ))
        return 1;
    }
  }
}

static LJ_AINLINE void gc2_table_dirty_bump(global_State *g, GCtab *t)
{
  LJGC2TabStamp *s = gc2_table_stamp(g, t);
  uint64_t old;
  if (!s)
    return;
  old = la_load64_acq(&s->state);
  for (;;) {
    uint32_t dirty = gc2_tabstamp_dirty(old);
    if (dirty >= UINT32_MAX - 1u) {
      LJGC2TabWideStamp *w = lj_arena_gc2_wide_acq(t);
      la_u128 prior, next;
      if (!w) {
        gc2_activation_pin_no_reclaim(g);
        return;  /* Reserved-storage invariant violation, never lazy OOM. */
      }
      prior = lj_arena_gc2_wide_snapshot(w);
      for (;;) {
        uint32_t serial = gc2_tabstamp_dirty(prior.lo);
        next.hi = prior.hi;
        if (serial == UINT32_MAX) {
          if (prior.hi == UINT64_MAX) {
            next.lo = UINT32_MAX;
          } else {
            next.hi++;
            next.lo = 1;
          }
        } else {
          next.lo = (uint64_t)(serial + 1u);
        }
        if (la_cas128(&w->proof, &prior, next))
          break;
      }
      if (next.hi == UINT64_MAX && (uint32_t)next.lo == UINT32_MAX)
        gc2_activation_pin_no_reclaim(g);
      /* W is monotone across cell reuse. Clear its previous-incarnation
      ** coverage BEFORE the inline mode LP; no initializer/reset can race a
      ** scanner that observes WIDE. A competing promoter needs no owner help. */
      gc2_table_coalesce_test_at(g, t, LJ_GC2_TABLE_COALESCE_TEST_PRE_MODE);
      while (gc2_tabstamp_dirty(old) != UINT32_MAX) {
        if (la_cas64(&s->state, &old, (uint64_t)UINT32_MAX,
                     LA_ACQ_REL, LA_ACQ))
          break;
      }
      gc2_table_coalesce_test_at(g, t, LJ_GC2_TABLE_COALESCE_TEST_POST_MODE);
      return;
    }
    if (la_cas64(&s->state, &old, gc2_tabstamp_pack(0, dirty + 1u),
                 LA_ACQ_REL, LA_ACQ))
      return;
  }
}

'''+s[end:]
s=s.replace('if (gc2_table_dirty_epoch(g, t, NULL) == ~(uint32_t)0) {',
            'if (gc2_table_authority_terminal(gc2_table_authority(g, t))) {')
start=s.index('static LJ_AINLINE int gc2_table_scan_current(global_State *g, GCtab *t)\n{')
end=s.index('#if defined(LJ_GC2_TEST_HELPERS)',start)
s=s[:start]+'''static LJ_AINLINE int gc2_table_scan_current(global_State *g, GCtab *t)
{
  GC2TabAuthority a;
  uint32_t cycle;
  if (!g || !t || (cycle = gc2_cycle_acq(g)) == 0)
    return 0;
  a = gc2_table_authority(g, t);
  return a.stamped && gc2_tabstamp_cycle(a.proof.lo) == cycle;
}

static LJ_AINLINE int gc2_table_scan_coalescible(global_State *g, GCtab *t)
{
  GC2TabAuthority a;
  uint32_t cycle;
  if (!g || !t || (cycle = gc2_cycle_acq(g)) == 0)
    return 0;
  a = gc2_table_authority(g, t);
  return a.stamped && gc2_tabstamp_cycle(a.proof.lo) == cycle &&
    !gc2_table_authority_terminal(a);
}

'''+s[end:]
s=s.replace('uint32_t cycle, dirty0;\n  int stamped, tabstatus, weak_retry', 'uint32_t cycle;\n  GC2TabAuthority dirty0;\n  int stamped, tabstatus, weak_retry')
s=s.replace('dirty0 = gc2_table_dirty_epoch(g, t, &stamped);','dirty0 = gc2_table_authority(g, t);\n  stamped = dirty0.stamped;')
assert 'gc2_table_dirty_epoch' not in s
f.write_text(s)
print(p)


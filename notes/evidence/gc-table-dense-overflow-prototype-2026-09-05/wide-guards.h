/* Private FREE cells model persisted authority after the previous lifetime
** has ended. Guard entries are also FREE; their NONE generations must survive. */
typedef struct WideGuard {
  LJGC2TabStamp *stamp;
  LJGC2TabWideStamp *wide;
  uint64_t inline_state;
  la_u128 proof;
  uint64_t token;
} WideGuard;
static WideGuard wide_guards[16];
static uint32_t wide_guard_count;

static void wide_guards_arm(GCArena *a, uint32_t cell, uint32_t delta)
{
  uint32_t first = cell / 2u - 1u, i;
  assert(first >= LJ_AFIRST_CELL);
  assert(delta + 3u <= 16u);
  wide_guard_count = delta + 3u;
  for (i = 0; i < wide_guard_count; i++) {
    LJGC2TabStamp *s = lj_arena_gc2_stamp_acq(lj_arena_cellptr(a, first + i));
    LJGC2TableTokenTicket ticket;
    assert(lj_arena_lifetime_state_acq(a, first + i) == LJ_ARENA_LIFETIME_FREE);
    assert(s);
    assert(lj_gc2_table_token_refresh(&s->token, &ticket) == LJ_GC2_TABLE_TOKEN_RESULT_OK);
    assert(lj_gc2_table_token_complete(&s->token, &ticket) == LJ_GC2_TABLE_TOKEN_RESULT_OK);
    /* Low two bits zero allow the old misaddressed precheck to reach its
    ** corrupting store, including when it confuses proof with token. */
    la_store64_rel(&s->state, UINT64_C(0x3141592600000040) + 4u * i);
    {
      LJGC2TabWideStamp *w = lj_arena_gc2_wide_acq(lj_arena_cellptr(a, first+i));
      la_u128 old = lj_arena_gc2_wide_snapshot(w);
      la_u128 next = { UINT64_C(0x55550000) + i,
                       UINT64_C(0x2718281800000000) + i };
      assert(la_cas128(&w->proof, &old, next));
      wide_guards[i].wide = w;
    }
    wide_guards[i].stamp = s;
    wide_guards[i].proof = lj_arena_gc2_wide_snapshot(wide_guards[i].wide);
    wide_guards[i].inline_state = la_load64_acq(&s->state);
    wide_guards[i].token = la_load64_acq(&s->token.control);
    assert(wide_guards[i].token != 0);
  }
}

static void wide_guards_check(void)
{
  uint32_t i;
  for (i = 0; i < wide_guard_count; i++) {
    la_u128 p = lj_arena_gc2_wide_snapshot(wide_guards[i].wide);
    assert(la_load64_acq(&wide_guards[i].stamp->state) == wide_guards[i].inline_state);
    assert(la_load64_acq(&wide_guards[i].stamp->token.control) == wide_guards[i].token);
    assert(p.lo == wide_guards[i].proof.lo);
    assert(p.hi == wide_guards[i].proof.hi);
  }
}

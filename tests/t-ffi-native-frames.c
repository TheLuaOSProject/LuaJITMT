/*
** Deterministic structural tests for the dormant generic FFI native-frame
** publisher. No test frame grants GC authority or enters native/JIT state.
*/

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_ccall.h"
#include "lj_jit.h"
#include "lj_state.h"
#include "lj_tg.h"

#ifndef LJ_FFI_NATIVE_FRAME_TEST_HELPERS
#error "t-ffi-native-frames requires LJ_FFI_NATIVE_FRAME_TEST_HELPERS"
#endif

static void frame_make(LJFFINativeFrame *frame, lua_State *L, uint32_t n)
{
  uint64_t root = (uint64_t)n * 32u;
  memset(frame, 0, sizeof(*frame));
  lj_ffi_native_frame_trace_rel(frame,
    (GCtrace *)(uintptr_t)(UINT64_C(0x10000) + (uint64_t)n * 16u));
  lj_ffi_native_frame_L_rel(frame, L);
  lj_ffi_native_frame_func_rel(frame,
    (void *)(uintptr_t)(UINT64_C(0x20000) + (uint64_t)n * 16u));
  lj_ffi_native_frame_old_func_rel(frame,
    n == 0 ? NULL :
      (void *)(uintptr_t)(UINT64_C(0x30000) + (uint64_t)n * 16u));
  lj_ffi_native_frame_root_offset_rel(frame, root);
  lj_ffi_native_frame_base_offset_rel(frame, root + 2u);
  lj_ffi_native_frame_top_offset_rel(frame, root + 12u);
  lj_ffi_native_frame_jit_base_offset_rel(frame, root + 2u);
  lj_ffi_native_frame_entry_exit_epoch_rel(frame, UINT64_C(7000) + n);
  lj_ffi_native_frame_trace_no_rel(frame, n + 1u);
  lj_ffi_native_frame_old_callback_slot_rel(frame, (MSize)(40u + n));
  lj_ffi_native_frame_flags_rel(frame,
    LJ_FFI_NATIVE_FRAME_F_SYNCHRONIZED |
    LJ_FFI_NATIVE_FRAME_F_ACTIVE |
    ((n & 1u) ? LJ_FFI_NATIVE_FRAME_F_CALLBACK_SEEN : 0));
  lj_ffi_native_frame_old_stopreq_rel(frame, (uint8_t)(n & 1u));
  lj_ffi_native_frame_had_stopreq_rel(frame, (uint8_t)((n >> 1) & 1u));
}

static void frame_assert(const LJFFINativeFrame *frame, lua_State *L,
			 uint32_t n)
{
  uint64_t root = (uint64_t)n * 32u;
  assert(lj_ffi_native_frame_trace_acq(frame) ==
    (GCtrace *)(uintptr_t)(UINT64_C(0x10000) + (uint64_t)n * 16u));
  assert(lj_ffi_native_frame_L_acq(frame) == L);
  assert(lj_ffi_native_frame_func_acq(frame) ==
    (void *)(uintptr_t)(UINT64_C(0x20000) + (uint64_t)n * 16u));
  assert(lj_ffi_native_frame_old_func_acq(frame) ==
    (n == 0 ? NULL :
      (void *)(uintptr_t)(UINT64_C(0x30000) + (uint64_t)n * 16u)));
  assert(lj_ffi_native_frame_root_offset_acq(frame) == root);
  assert(lj_ffi_native_frame_base_offset_acq(frame) == root + 2u);
  assert(lj_ffi_native_frame_top_offset_acq(frame) == root + 12u);
  assert(lj_ffi_native_frame_jit_base_offset_acq(frame) == root + 2u);
  assert(lj_ffi_native_frame_entry_exit_epoch_acq(frame) ==
    UINT64_C(7000) + n);
  assert(lj_ffi_native_frame_trace_no_acq(frame) == n + 1u);
  assert(lj_ffi_native_frame_old_callback_slot_acq(frame) ==
    (MSize)(40u + n));
  assert(lj_ffi_native_frame_flags_acq(frame) ==
    (LJ_FFI_NATIVE_FRAME_F_SYNCHRONIZED |
     LJ_FFI_NATIVE_FRAME_F_ACTIVE |
     ((n & 1u) ? LJ_FFI_NATIVE_FRAME_F_CALLBACK_SEEN : 0)));
  assert(lj_ffi_native_frame_old_stopreq_acq(frame) == (uint8_t)(n & 1u));
  assert(lj_ffi_native_frame_had_stopreq_acq(frame) ==
    (uint8_t)((n >> 1) & 1u));
}

typedef struct DormantState {
  void *ffi_call_func;
  TValue *ffi_xsave_root;
  TValue *jit_base;
  MSize callback_slot;
  uint32_t ffi_xsave_baseslot;
  uint32_t ffi_xsave_nslots;
  uint32_t in_native;
  uint8_t callback_had_stopreq;
} DormantState;

static void dormant_state_save(TGState *tg, DormantState *state)
{
  state->ffi_call_func = lj_tg_ffi_call_func_acq(tg);
  state->ffi_xsave_root = (TValue *)la_loadptr_acq(
    (void *const *)&tg->ffi_xsave_root);
#if LJ_HASJIT
  state->jit_base = lj_tg_load_jit_base(tg);
#else
  state->jit_base = NULL;
#endif
  state->callback_slot = ccallback_slot_acq(&tg->cb);
  state->ffi_xsave_baseslot = la_load32_acq(&tg->ffi_xsave_baseslot);
  state->ffi_xsave_nslots = la_load32_acq(&tg->ffi_xsave_nslots);
  state->in_native = lj_tg_in_native_acq(tg);
  state->callback_had_stopreq =
    ccallback_native_had_stopreq_acq(&tg->cb);
}

static void dormant_state_assert(TGState *tg, const DormantState *state)
{
  assert(lj_tg_ffi_call_func_acq(tg) == state->ffi_call_func);
  assert(la_loadptr_acq((void *const *)&tg->ffi_xsave_root) ==
    state->ffi_xsave_root);
#if LJ_HASJIT
  assert(lj_tg_load_jit_base(tg) == state->jit_base);
#endif
  assert(ccallback_slot_acq(&tg->cb) == state->callback_slot);
  assert(la_load32_acq(&tg->ffi_xsave_baseslot) ==
    state->ffi_xsave_baseslot);
  assert(la_load32_acq(&tg->ffi_xsave_nslots) == state->ffi_xsave_nslots);
  assert(lj_tg_in_native_acq(tg) == state->in_native);
  assert(ccallback_native_had_stopreq_acq(&tg->cb) ==
    state->callback_had_stopreq);
}

static void dormant_state_restore(TGState *tg, const DormantState *state)
{
  lj_tg_ffi_call_func_rel(tg, state->ffi_call_func);
  la_storeptr_rel((void **)&tg->ffi_xsave_root, state->ffi_xsave_root);
#if LJ_HASJIT
  lj_tg_store_jit_base(tg, state->jit_base);
#endif
  ccallback_slot_rel(&tg->cb, state->callback_slot);
  la_store32_rel(&tg->ffi_xsave_baseslot, state->ffi_xsave_baseslot);
  la_store32_rel(&tg->ffi_xsave_nslots, state->ffi_xsave_nslots);
  lj_tg_in_native_store_rlx(tg, state->in_native);
  ccallback_native_had_stopreq_rel(&tg->cb,
    state->callback_had_stopreq);
}

static void dormant_state_seed(TGState *tg)
{
  lj_tg_ffi_call_func_rel(tg, (void *)(uintptr_t)UINT64_C(0x41000));
  la_storeptr_rel((void **)&tg->ffi_xsave_root,
    (void *)(uintptr_t)UINT64_C(0x42000));
#if LJ_HASJIT
  lj_tg_store_jit_base(tg,
    (TValue *)(uintptr_t)UINT64_C(0x43000));
#endif
  ccallback_slot_rel(&tg->cb, 37);
  la_store32_rel(&tg->ffi_xsave_baseslot, 41);
  la_store32_rel(&tg->ffi_xsave_nslots, 53);
  lj_tg_in_native_store_rlx(tg, 3);
  ccallback_native_had_stopreq_rel(&tg->cb, 1);
}

static void test_retry_and_invalid(TGState *tg)
{
  LJFFINativeFrameSnapshot snapshot, sentinel;
  LJFFINativeFrame frame;
  uint64_t seq = lj_ffi_native_frame_sequence_acq(tg);

  assert((seq & 1u) == 0);
  memset(&snapshot, 0xa5, sizeof(snapshot));
  sentinel = snapshot;
  la_store64_rel(&tg->ffi_native_seq, seq + 1u);
  assert(lj_ffi_native_frame_snapshot(tg, &snapshot) ==
    LJ_FFI_NATIVE_FRAME_SNAPSHOT_RETRY);
  assert(memcmp(&snapshot, &sentinel, sizeof(snapshot)) == 0);
  la_store64_rel(&tg->ffi_native_seq, seq + 2u);

  seq += 2u;
  memset(&snapshot, 0x5a, sizeof(snapshot));
  sentinel = snapshot;
  la_store64_rel(&tg->ffi_native_seq, seq + 1u);
  la_store32_rel(&tg->ffi_native_depth, LJ_FFI_NATIVE_FRAME_MAX + 1u);
  la_store64_rel(&tg->ffi_native_seq, seq + 2u);
  assert(lj_ffi_native_frame_snapshot(tg, &snapshot) ==
    LJ_FFI_NATIVE_FRAME_SNAPSHOT_INVALID);
  assert(memcmp(&snapshot, &sentinel, sizeof(snapshot)) == 0);

  seq += 2u;
  la_store64_rel(&tg->ffi_native_seq, seq + 1u);
  la_store32_rel(&tg->ffi_native_depth, 0);
  la_store64_rel(&tg->ffi_native_seq, seq + 2u);
  assert(lj_ffi_native_frame_snapshot(tg, &snapshot) ==
    LJ_FFI_NATIVE_FRAME_SNAPSHOT_EMPTY);

  /* A same-generation, in-range but malformed active frame is corruption,
  ** not a transient retry. Keep the caller's output untouched. */
  frame_make(&frame, lj_tg_load_cur_L(tg), 0);
  assert(lj_ffi_native_frame_push(tg, &frame) == 1);
  seq = lj_ffi_native_frame_sequence_acq(tg);
  la_store64_rel(&tg->ffi_native_seq, seq + 1u);
  la_fence_rel();
  lj_ffi_native_frame_flags_rel(&tg->ffi_native_frame[0], 0);
  la_store64_rel(&tg->ffi_native_seq, seq + 2u);
  memset(&snapshot, 0x3c, sizeof(snapshot));
  sentinel = snapshot;
  assert(lj_ffi_native_frame_snapshot(tg, &snapshot) ==
    LJ_FFI_NATIVE_FRAME_SNAPSHOT_INVALID);
  assert(memcmp(&snapshot, &sentinel, sizeof(snapshot)) == 0);
  la_store64_rel(&tg->ffi_native_seq, seq + 3u);
  la_fence_rel();
  lj_ffi_native_frame_flags_rel(&tg->ffi_native_frame[0],
    LJ_FFI_NATIVE_FRAME_F_SYNCHRONIZED | LJ_FFI_NATIVE_FRAME_F_ACTIVE);
  la_store64_rel(&tg->ffi_native_seq, seq + 4u);
  lj_ffi_native_frame_pop(tg, NULL);
}

static void snapshot_sequence_change(TGState *tg)
{
  uint64_t seq = lj_ffi_native_frame_sequence_acq(tg);
  assert((seq & 1u) == 0 && seq <= UINT64_MAX - 2u);
  la_store64_rel(&tg->ffi_native_seq, seq + 1u);
  la_store64_rel(&tg->ffi_native_seq, seq + 2u);
  lj_ffi_native_frame_test_set_snapshot_hook(NULL);
}

static void test_final_sequence_retry(TGState *tg)
{
  LJFFINativeFrameSnapshot snapshot;
  uint64_t seq = lj_ffi_native_frame_sequence_acq(tg);
  lj_ffi_native_frame_test_set_snapshot_hook(snapshot_sequence_change);
  assert(lj_ffi_native_frame_snapshot(tg, &snapshot) ==
    LJ_FFI_NATIVE_FRAME_SNAPSHOT_RETRY);
  assert(lj_ffi_native_frame_sequence_acq(tg) == seq + 2u);
  assert(lj_ffi_native_frame_snapshot(tg, &snapshot) ==
    LJ_FFI_NATIVE_FRAME_SNAPSHOT_EMPTY);
}

int main(void)
{
  LJFFINativeFrameSnapshot snapshot, before_overflow;
  LJFFINativeFrame frame, popped;
  DormantState dormant, original;
  lua_State *L = luaL_newstate();
  TGState *tg;
  uint64_t seq;
  uint32_t i;

  assert(L != NULL);
  tg = L2TG(L);
  assert(tg != NULL);
  assert(lj_ffi_native_frame_depth_acq(tg) == 0);
  assert(lj_ffi_native_frame_snapshot(tg, &snapshot) ==
    LJ_FFI_NATIVE_FRAME_SNAPSHOT_EMPTY);
  dormant_state_save(tg, &original);
  dormant_state_seed(tg);
  dormant_state_save(tg, &dormant);
  seq = snapshot.sequence;

  for (i = 0; i < LJ_FFI_NATIVE_FRAME_MAX; i++) {
    frame_make(&frame, L, i);
    assert(lj_ffi_native_frame_push(tg, &frame) == 1);
    seq += 2u;
    assert(lj_ffi_native_frame_sequence_acq(tg) == seq);
    assert(lj_ffi_native_frame_depth_acq(tg) == i + 1u);
    assert(lj_ffi_native_frame_snapshot(tg, &snapshot) ==
      LJ_FFI_NATIVE_FRAME_SNAPSHOT_STABLE);
    assert(snapshot.sequence == seq && snapshot.depth == i + 1u);
    frame_assert(&snapshot.frame[i], L, i);
    dormant_state_assert(tg, &dormant);
  }

  before_overflow = snapshot;
  frame_make(&frame, L, LJ_FFI_NATIVE_FRAME_MAX);
  assert(lj_ffi_native_frame_push(tg, &frame) == 0);
  errno = EDOM;
  /* Capacity is checked before the raw generated trace constant. A generated
  ** caller can exit without dereferencing this deliberate poison value, while
  ** consuming the one-shot XSAVE staging for the rejected attempt. */
  assert(lj_ffi_native_trace_enter(L, (GCtrace *)(uintptr_t)3u,
				   (void *)(uintptr_t)5u) == 0);
  assert(errno == EDOM);
  assert(lj_ffi_native_frame_sequence_acq(tg) == seq);
  assert(lj_ffi_native_frame_depth_acq(tg) == LJ_FFI_NATIVE_FRAME_MAX);
  assert(lj_ffi_native_frame_snapshot(tg, &snapshot) ==
    LJ_FFI_NATIVE_FRAME_SNAPSHOT_STABLE);
  assert(snapshot.sequence == before_overflow.sequence);
  assert(snapshot.depth == before_overflow.depth);
  for (i = 0; i < LJ_FFI_NATIVE_FRAME_MAX; i++)
    frame_assert(&snapshot.frame[i], L, i);
  dormant.ffi_xsave_root = NULL;
  dormant.ffi_xsave_baseslot = 0;
  dormant.ffi_xsave_nslots = 0;
  dormant_state_assert(tg, &dormant);

  for (i = LJ_FFI_NATIVE_FRAME_MAX; i > 0; i--) {
    memset(&popped, 0, sizeof(popped));
    lj_ffi_native_frame_pop(tg, &popped);
    frame_assert(&popped, L, i - 1u);
    seq += 2u;
    assert(lj_ffi_native_frame_sequence_acq(tg) == seq);
    assert(lj_ffi_native_frame_depth_acq(tg) == i - 1u);
    dormant_state_assert(tg, &dormant);
  }
  assert(lj_ffi_native_frame_snapshot(tg, &snapshot) ==
    LJ_FFI_NATIVE_FRAME_SNAPSHOT_EMPTY);

  test_final_sequence_retry(tg);
  test_retry_and_invalid(tg);
  dormant_state_assert(tg, &dormant);
  dormant_state_restore(tg, &original);
  dormant_state_assert(tg, &original);
  lj_ffi_native_frame_fini(tg);
  lua_close(L);
  printf("t-ffi-native-frames OK: sequenced nesting and overflow are stable\n");
  return 0;
}

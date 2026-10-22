/* Recorder lookup contract only: no synthetic trace is assembled or entered. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "lj_obj.h"
#include "lj_ir.h"
#include "lj_ircall.h"
#include "lj_iropt.h"
#include "lj_record.h"

#if !LJ_HASJIT || !defined(LJ_TRACE_TEST_HELPERS)
#error "t-jit-cell-cnew-epoch requires JIT and LJ_TRACE_TEST_HELPERS"
#endif

enum {
  OLD_BASE = REF_FIRST,
  OLD_ARG,
  OLD_CALL,
  RETURN_FRAME,
  NEW_BASE,
  NEW_ARG,
  NEW_CALL,
  LAST_CALL,
  IR_END
};

static jit_State recorder;
static IRIns ir[IR_END];

static void setir(IRRef ref, IROp op, IRType type, IRRef op1, IRRef op2)
{
  ir[ref].o = (uint8_t)op;
  ir[ref].t.irt = (uint8_t)type;
  ir[ref].op1 = (IRRef1)op1;
  ir[ref].op2 = (IRRef1)op2;
}

static void check_epoch(int frame_slots, IROp callop)
{
  jit_State *J = &recorder;
  IRRef slot, offset, oldbase = REF_BASE, newbase = REF_BASE;
  IROp otherop = callop == IR_CALLA ? IR_CALLS : IR_CALLA;
  memset(J, 0, sizeof(*J));
  memset(ir, 0, sizeof(ir));
  J->cur.ir = ir;
  J->cur.nk = REF_TRUE;
  J->irbotlim = REF_TRUE - 16;
  J->baseslot = (BCReg)(1 + LJ_FR2 + frame_slots);
  slot = tref_ref(lj_ir_kint(J, 3));
  if (frame_slots) {
    offset = tref_ref(lj_ir_kintpgc(J, frame_slots * 8));
    setir(OLD_BASE, IR_ADD, IRT_PGC, REF_BASE, offset);
    setir(NEW_BASE, IR_ADD, IRT_PGC, REF_BASE, offset);
    oldbase = OLD_BASE;
    newbase = NEW_BASE;
  }
  setir(OLD_ARG, IR_CARG, IRT_NIL, oldbase, slot);
  setir(OLD_CALL, callop, IRT_PGC, OLD_ARG,
        IRCALL_lj_func_newuvcell_forjit);
  J->chain[callop] = OLD_CALL;
  assert(tref_ref(lj_record_test_celluv_cnewref(J, 3)) == OLD_CALL);
  assert(lj_record_test_celluv_cnewref(J, 4) == 0);

  /* The same relative address names another frame after returning below the
  ** trace entry. The allocation from the previous BASE must not be selected. */
  setir(RETURN_FRAME, IR_RETF, IRT_PGC, 0, 0);
  J->chain[IR_RETF] = RETURN_FRAME;
  assert(lj_record_test_celluv_cnewref(J, 3) == 0);

  /* Structural matching must still find a fresh allocation without relying
  ** on CSE to merge the otherwise equal ADD expressions. */
  setir(NEW_ARG, IR_CARG, IRT_NIL, newbase, slot);
  setir(NEW_CALL, callop, IRT_PGC, NEW_ARG,
        IRCALL_lj_func_newuvcell_forjit);
  ir[NEW_CALL].prev = OLD_CALL;
  J->chain[callop] = NEW_CALL;
  assert(tref_ref(lj_record_test_celluv_cnewref(J, 3)) == NEW_CALL);

  /* Both allocation and side-effect call chains obey the boundary; the
  ** newest matching allocation wins regardless of which chain owns it. */
  setir(LAST_CALL, otherop, IRT_PGC, NEW_ARG,
        IRCALL_lj_func_newuvcell_forjit);
  J->chain[otherop] = LAST_CALL;
  assert(tref_ref(lj_record_test_celluv_cnewref(J, 3)) == LAST_CALL);
  ir[NEW_ARG].op2 = (IRRef1)tref_ref(lj_ir_kint(J, 4));
  assert(lj_record_test_celluv_cnewref(J, 3) == 0);
  assert(tref_ref(lj_record_test_celluv_cnewref(J, 4)) == LAST_CALL);
}

int main(void)
{
  check_epoch(0, IR_CALLA);
  check_epoch(0, IR_CALLS);
  check_epoch(4, IR_CALLA);
  check_epoch(4, IR_CALLS);
  puts("t-jit-cell-cnew-epoch OK: recorder cell lookup respects RETF");
  return 0;
}

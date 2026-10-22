/* Exercise the production pending-capture scans without executing native JIT. */
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "lj_obj.h"
#include "lj_bc.h"
#include "lj_record.h"

#if !LJ_HASJIT || !defined(LJ_TRACE_TEST_HELPERS)
#error "requires JIT and LJ_TRACE_TEST_HELPERS"
#endif

typedef struct Parent {
  GCproto pt;
  BCIns bc[4];
  GCRef kgc[1];
} Parent;

static void defined(jit_State *J, BCIns ins, BCReg slot, int want)
{
  BCIns code[2];
  code[0] = ins;
  J->pc = code;
  assert(lj_record_test_celluv_defined_before(J, code+1, slot) == want);
}

int main(void)
{
  static const BCOp stops[] = {
    BC_RET, BC_RET0, BC_RET1, BC_RETM, BC_CALLT, BC_CALLMT,
    BC_FORL, BC_ITERL, BC_LOOP, BC_UCLO
  };
  Parent parent;
  GCproto child;
  uint16_t uv = PROTO_UV_LOCAL|1;
  jit_State *J = (jit_State *)calloc(1, sizeof(*J));
  unsigned i;
  assert(J != NULL);
  memset(&parent, 0, sizeof(parent));
  memset(&child, 0, sizeof(child));
  assert((void *)proto_bc(&parent.pt) == (void *)parent.bc);
  child.gct = (uint8_t)~LJ_TPROTO;
  child.sizeuv = 1;
  proto_setcelluv(&child);
  setmref(child.uv, &uv);
  setgcref(parent.kgc[0], obj2gco(&child));
  setmref(parent.pt.k, parent.kgc+1);
  parent.pt.sizebc = 4;
  proto_sizekgc_rel(&parent.pt, 1);
  parent.bc[2] = BCINS_AD(BC_FNEW, 2, 0);
  parent.bc[3] = BCINS_AD(BC_RET0, 0, 1);
  J->pt = &parent.pt;
  J->pc = parent.bc+1;
  parent.bc[1] = BCINS_AD(BC_MOV, 0, 0);
  assert(lj_record_test_celluv_will_promote(J, 1) == 1);
  assert(lj_record_test_celluv_will_promote(J, 0) == 0);
  for (i=0; i<sizeof(stops)/sizeof(stops[0]); i++) {
    parent.bc[1] = BCINS_AD(stops[i], 0, 0);
    assert(lj_record_test_celluv_will_promote(J, 1) == 0);
  }
  defined(J, BCINS_ABC(BC_CALL, 3, 2, 1), 2, 0);
  defined(J, BCINS_ABC(BC_CALL, 3, 2, 1), 3, 1);
  defined(J, BCINS_ABC(BC_CALL, 3, 2, 1), 20, 1);
  defined(J, BCINS_ABC(BC_CALLM, 3, 0, 1), 20, 1);
  defined(J, BCINS_ABC(BC_ITERC, 5, 3, 3), 4, 0);
  defined(J, BCINS_ABC(BC_ITERC, 5, 3, 3), 20, 1);
  defined(J, BCINS_ABC(BC_ITERN, 5, 3, 3), 20, 1);
  defined(J, BCINS_ABC(BC_VARG, 3, 3, 0), 2, 0);
  defined(J, BCINS_ABC(BC_VARG, 3, 3, 0), 3, 1);
  defined(J, BCINS_ABC(BC_VARG, 3, 3, 0), 4, 1);
  defined(J, BCINS_ABC(BC_VARG, 3, 3, 0), 5, 0);
  defined(J, BCINS_ABC(BC_VARG, 3, 0, 0), 20, 1);
  defined(J, BCINS_AD(BC_FORI, 3, 0), 2, 0);
  defined(J, BCINS_AD(BC_FORI, 3, 0), 3, 1);
  defined(J, BCINS_AD(BC_FORL, 3, 0), 6, 1);
  defined(J, BCINS_AD(BC_FORL, 3, 0), 7, 0);
  defined(J, BCINS_AD(BC_ITERL, 5, 0), 4, 1);
  defined(J, BCINS_AD(BC_ISNEXT, 5, 0), 4, 1);
  defined(J, BCINS_AD(BC_ISNEXT, 5, 0), 5, 0);
  free(J);
  puts("pending capture scans: OK");
  return 0;
}

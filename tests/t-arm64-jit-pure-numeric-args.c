/*
** Native macOS ARM64 contract for exact ascending/descending ADD, ascending
** MUL/DIV, and descending DIV/SUB dynamic-accumulator pure-NUM roots.
**
** This certifies twelve intentionally narrow evolution profiles over one loop
** geometry: strict/inclusive ascending ADD, strict/inclusive ascending MUL,
** strict/inclusive ascending DIV, strict/inclusive descending DIV,
** strict/inclusive descending ADD, and strict/inclusive descending SUB, each
** with live NUM accumulator/limit parameters and either a NUM or INT
** invariant step, factor, or divisor. INT invariants are converted to NUM
** exactly once before either recurrence.
** Adjacent arithmetic, direction, and bytecode families remain fail-closed,
** while the already-admitted fixed-initializer roots stay distinct.
*/

#include <assert.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#if defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__)) && \
    defined(LUAJIT_MT_ARM64_BOOTSTRAP) && \
    defined(LUAJIT_MT_ARM64_JIT_EXPERIMENTAL) && \
    defined(LJ_TRACE_TEST_HELPERS)

#include "lj_obj.h"
#include "lj_atomic.h"
#include "lj_bc.h"
#include "lj_dispatch.h"
#include "lj_func.h"
#include "lj_gc2.h"
#include "lj_ir.h"
#include "lj_jit.h"
#include "lj_profile.h"
#include "lj_target.h"
#include "lj_tg.h"
#include "lj_thr.h"
#include "lj_trace.h"

#if !LJ_HASJIT || LJ_ARM64_JIT_ROOT_RECORDER_FAIL_CLOSED || \
    !LJ_ARM64_JIT_SIDE_RECORDER_FAIL_CLOSED || \
    !LJ_ARM64_JIT_STITCH_RECORDER_FAIL_CLOSED || \
    LJ_ARM64_JIT_LOOP_NATIVE_ENTRY_FAIL_CLOSED || \
    LJ_ARM64_JIT_JFUNCF_NATIVE_ENTRY_FAIL_CLOSED || \
    !LJ_ARM64_JIT_STITCH_NATIVE_ENTRY_FAIL_CLOSED
#error "t-arm64-jit-pure-numeric-args requires the admitted ARM64 root gates"
#endif

#if !LJ_HASPROFILE || !LJ_PROFILE_TGLOCAL
#error "t-arm64-jit-pure-numeric-args requires ARM64 TG-local profile polling"
#endif

enum {
  R_X = REF_FIRST,
  R_STEP,
  R_X_PRE,
  R_LIMIT,
  R_PRECOND,
  R_LOOP,
  R_XPOLL,
  R_X_BODY,
  R_COND,
  R_X_PHI,
  R_NOP,
  R_END
};

enum {
  X_OR_STEP_TYPE_EXIT = 0,
  LIMIT_TYPE_EXIT = 1,
  PRECOND_EXIT = 2,
  XPOLL_EXIT = 3,
  FINAL_EXIT = 4
};

typedef enum NumericArgsComparison {
  NUMERIC_ARGS_STRICT,
  NUMERIC_ARGS_INCLUSIVE
} NumericArgsComparison;

typedef enum NumericArgsStepKind {
  NUMERIC_ARGS_STEP_NUM,
  NUMERIC_ARGS_STEP_INT
} NumericArgsStepKind;

typedef enum NumericArgsEvolution {
  NUMERIC_ARGS_ADD_ASCENDING,
  NUMERIC_ARGS_MUL_ASCENDING,
  NUMERIC_ARGS_DIV_ASCENDING,
  NUMERIC_ARGS_DIV_DESCENDING,
  NUMERIC_ARGS_ADD_DESCENDING,
  NUMERIC_ARGS_SUB_DESCENDING,
  NUMERIC_ARGS_EVOLUTION_MAX
} NumericArgsEvolution;

typedef struct NumericArgsCall {
  lua_Number x;
  lua_Number limit;
  lua_Number step;
  lua_Number result;
} NumericArgsCall;

typedef struct NumericArgsProfile {
  const char *name;
  NumericArgsEvolution evolution;
  NumericArgsComparison comparison;
  BCOp bytecode_op;
  BCReg compare_a;
  BCReg compare_d;
  BCOp recurrence_bc;
  IROp recurrence_ir;
  IROp precondition_op;
  IROp body_op;
  uint32_t recurrence_mcode;
  int fcmp_limit_first;
  A64CC precondition_exit_cc;
  A64CC body_loop_cc;
  NumericArgsCall record;
  NumericArgsCall reuse;
  NumericArgsCall lifecycle;
  NumericArgsCall mutation;
  NumericArgsCall integer_x;
  NumericArgsCall integer_step;
  NumericArgsCall integer_limit;
  NumericArgsCall precondition;
} NumericArgsProfile;

static int numeric_args_is_descending(const NumericArgsProfile *profile)
{
  return profile->evolution == NUMERIC_ARGS_ADD_DESCENDING ||
	 profile->evolution == NUMERIC_ARGS_SUB_DESCENDING ||
	 profile->evolution == NUMERIC_ARGS_DIV_DESCENDING;
}

static IRRef numeric_args_ref(NumericArgsStepKind step_kind, IRRef num_ref)
{
  return (IRRef)(num_ref+
	(step_kind == NUMERIC_ARGS_STEP_INT && num_ref >= R_X_PRE));
}

static IRRef numeric_args_step_value_ref(NumericArgsStepKind step_kind)
{
  return step_kind == NUMERIC_ARGS_STEP_INT ? R_STEP+1u : R_STEP;
}

static IRRef numeric_args_end_ref(NumericArgsStepKind step_kind)
{
  return numeric_args_ref(step_kind, R_END);
}

static const NumericArgsProfile strict_profile = {
  "__arm64_pure_numeric_args", NUMERIC_ARGS_ADD_ASCENDING,
  NUMERIC_ARGS_STRICT, BC_ISGE, 3, 4, BC_ADDVV, IR_ADD,
  IR_GT, IR_LT, A64I_FADDd, 0, CC_HS, CC_LO,
  { 0.5, 20.25, 0.5, 20.5 },
  { 0.25, 1.0, 0.375, 1.0 },
  { 0.25, 20.25, 0.25, 20.25 },
  { 0.25, 20.25, 0.5, 0.0 },
  { 1.0, 20.25, 0.5, 20.5 },
  { 0.5, 20.25, 1.0, 20.5 },
  { 0.5, 20.0, 0.5, 20.0 },
  { 0.5, 0.75, 0.5, 1.0 }
};

static const NumericArgsProfile inclusive_profile = {
  "__arm64_pure_numeric_args_inclusive", NUMERIC_ARGS_ADD_ASCENDING,
  NUMERIC_ARGS_INCLUSIVE, BC_ISGT, 3, 4, BC_ADDVV, IR_ADD,
  IR_GE, IR_LE, A64I_FADDd, 0, CC_HI, CC_LS,
  { 0.5, 20.25, 0.5, 20.5 },
  { 0.25, 1.0, 0.375, 1.375 },
  { 0.25, 20.25, 0.25, 20.5 },
  { 0.25, 20.25, 0.5, 0.0 },
  { 1.0, 20.25, 0.5, 20.5 },
  { 0.5, 20.25, 1.0, 20.5 },
  { 0.5, 20.0, 0.5, 20.5 },
  { 0.5, 0.75, 0.5, 1.0 }
};

/* The MUL reuse tuple is made entirely of exact binary fractions. Replacing
** only x, limit, or factor with its recording value produces 13.5, 50.625,
** or 10 respectively instead of 5.625. These rows reach Lua through
** lua_pushnumber: a Lua source literal such as 2.0 may be INT-tagged and
** correctly exercise the separate IR_CONV/type-exit boundary instead. */
static const NumericArgsProfile mul_profile = {
  "__arm64_pure_numeric_args_mul", NUMERIC_ARGS_MUL_ASCENDING,
  NUMERIC_ARGS_STRICT, BC_ISGE, 3, 4, BC_MULVV, IR_MUL,
  IR_GT, IR_LT, A64I_FMULd, 0, CC_HS, CC_LO,
  { 0.5, 20.25, 2.0, 32.0 },
  { 0.625, 5.5, 3.0, 5.625 },
  { 0.5, 20.25, 2.0, 32.0 },
  { 0.5, 20.25, 2.0, 0.0 },
  { 1.0, 20.25, 2.0, 32.0 },
  { 0.5, 20.25, 2.0, 32.0 },
  { 0.5, 20.0, 2.0, 32.0 },
  { 15.0, 20.25, 2.0, 30.0 }
};

/* Inclusive MUL uses the same exact operands as strict MUL, but equality at
** either native guard takes one additional multiply. The reuse tuple and all
** three substitutions are exact binary fractions. */
static const NumericArgsProfile mul_inclusive_profile = {
  "__arm64_pure_numeric_args_mul_inclusive", NUMERIC_ARGS_MUL_ASCENDING,
  NUMERIC_ARGS_INCLUSIVE, BC_ISGT, 3, 4, BC_MULVV, IR_MUL,
  IR_GE, IR_LE, A64I_FMULd, 0, CC_HI, CC_LS,
  { 0.5, 20.25, 2.0, 32.0 },
  { 0.625, 5.625, 3.0, 16.875 },
  { 0.5, 20.25, 2.0, 32.0 },
  { 0.5, 20.25, 2.0, 0.0 },
  { 1.0, 20.25, 2.0, 32.0 },
  { 0.5, 20.25, 2.0, 32.0 },
  { 0.5, 20.0, 2.0, 32.0 },
  { 15.0, 20.25, 2.0, 30.0 }
};

/* DIV is noncommutative: both IR and machine code retain X/DIVISOR operand
** order. Replacing only x, limit, or divisor in the exact-binary reuse tuple
** with its recording value produces 8, 40, or 5 respectively instead of 10. */
static const NumericArgsProfile div_profile = {
  "__arm64_pure_numeric_args_div", NUMERIC_ARGS_DIV_ASCENDING,
  NUMERIC_ARGS_STRICT, BC_ISGE, 3, 4, BC_DIVVV, IR_DIV,
  IR_GT, IR_LT, A64I_FDIVd, 0, CC_HS, CC_LO,
  { 0.5, 20.25, 0.5, 32.0 },
  { 0.625, 4.5, 0.25, 10.0 },
  { 0.5, 20.25, 0.5, 32.0 },
  { 0.5, 20.25, 0.5, 0.0 },
  { 1.0, 20.25, 0.5, 32.0 },
  { 0.5, 20.25, 0.0, INFINITY },
  { 0.5, 20.0, 0.5, 32.0 },
  { 15.0, 20.25, 0.5, 30.0 }
};

/* Inclusive DIV retains the exact noncommutative X/DIVISOR ordering. The
** reuse tuple and its three single-recording-value substitutions remain
** distinct from the result of the fully live call. */
static const NumericArgsProfile div_inclusive_profile = {
  "__arm64_pure_numeric_args_div_inclusive", NUMERIC_ARGS_DIV_ASCENDING,
  NUMERIC_ARGS_INCLUSIVE, BC_ISGT, 3, 4, BC_DIVVV, IR_DIV,
  IR_GE, IR_LE, A64I_FDIVd, 0, CC_HI, CC_LS,
  { 0.5, 20.25, 0.5, 32.0 },
  { 0.625, 4.5, 0.25, 10.0 },
  { 0.5, 20.25, 0.5, 32.0 },
  { 0.5, 20.25, 0.5, 0.0 },
  { 1.0, 20.25, 0.5, 32.0 },
  { 0.5, 20.25, 0.0, INFINITY },
  { 0.5, 20.0, 0.5, 32.0 },
  { 15.0, 20.25, 0.5, 30.0 }
};

/* Descending DIV retains X/DIVISOR operand order while comparing LIMIT/X.
** Replacing only x, limit, or divisor in the reuse tuple with its recording
** value produces 0.3203125, 0.072265625, or 0.578125 respectively instead of
** 0.2890625. All values and results are exact binary fractions. */
static const NumericArgsProfile div_descending_profile = {
  "__arm64_pure_numeric_args_div_descending",
  NUMERIC_ARGS_DIV_DESCENDING,
  NUMERIC_ARGS_STRICT, BC_ISGE, 4, 3, BC_DIVVV, IR_DIV,
  IR_LT, IR_GT, A64I_FDIVd, 1, CC_HS, CC_LO,
  { 20.5, 0.25, 2.0, 0.16015625 },
  { 18.5, 0.75, 4.0, 0.2890625 },
  { 20.5, 0.25, 2.0, 0.16015625 },
  { 20.5, 0.25, 2.0, 0.0 },
  { 20.0, 0.25, 2.0, 0.15625 },
  { 20.5, 0.25, 2.0, 0.16015625 },
  { 20.5, 1.0, 2.0, 0.640625 },
  { 0.75, 0.5, 2.0, 0.375 }
};

/* Inclusive descending DIV has the same all-live exact-binary reuse proof,
** with distinct equality probes for both native guards and interpreted >=. */
static const NumericArgsProfile div_descending_inclusive_profile = {
  "__arm64_pure_numeric_args_div_descending_inclusive",
  NUMERIC_ARGS_DIV_DESCENDING,
  NUMERIC_ARGS_INCLUSIVE, BC_ISGT, 4, 3, BC_DIVVV, IR_DIV,
  IR_LE, IR_GE, A64I_FDIVd, 1, CC_HI, CC_LS,
  { 20.5, 0.25, 2.0, 0.16015625 },
  { 18.5, 0.75, 4.0, 0.2890625 },
  { 20.5, 0.25, 2.0, 0.16015625 },
  { 20.5, 0.25, 2.0, 0.0 },
  { 20.0, 0.25, 2.0, 0.15625 },
  { 20.5, 0.25, 2.0, 0.16015625 },
  { 20.5, 1.0, 2.0, 0.640625 },
  { 0.75, 0.5, 2.0, 0.375 }
};

/* The descending-ADD reuse tuple is made entirely of exact binary fractions.
** Replacing only x, limit, or step with its recording value produces -0.875,
** 0.125, or -1.0 respectively instead of -0.625. */
static const NumericArgsProfile add_descending_profile = {
  "__arm64_pure_numeric_args_add_descending",
  NUMERIC_ARGS_ADD_DESCENDING,
  NUMERIC_ARGS_STRICT, BC_ISGE, 4, 3, BC_ADDVV, IR_ADD,
  IR_LT, IR_GT, A64I_FADDd, 1, CC_HS, CC_LO,
  { 20.5, 0.25, -0.5, 0.0 },
  { 0.5, -0.625, -0.375, -0.625 },
  { 20.5, 0.25, -0.5, 0.0 },
  { 20.25, 0.25, -0.5, 0.25 },
  { 20.0, 0.25, -0.5, 0.0 },
  { 20.5, 0.25, -1.0, -0.5 },
  { 20.5, 1.0, -0.5, 1.0 },
  { 0.75, 0.5, -0.5, 0.25 }
};

/* The inclusive descending-ADD reuse tuple is made entirely of exact binary
** fractions. Replacing only x, limit, or step with its recording value
** produces -0.75, 0.125, or -1.125 respectively instead of -0.875. */
static const NumericArgsProfile add_descending_inclusive_profile = {
  "__arm64_pure_numeric_args_add_descending_inclusive",
  NUMERIC_ARGS_ADD_DESCENDING,
  NUMERIC_ARGS_INCLUSIVE, BC_ISGT, 4, 3, BC_ADDVV, IR_ADD,
  IR_LE, IR_GE, A64I_FADDd, 1, CC_HI, CC_LS,
  { 20.5, 0.25, -0.5, 0.0 },
  { 0.375, -0.625, -0.25, -0.875 },
  { 20.5, 0.25, -0.5, 0.0 },
  { 20.25, 0.25, -0.5, -0.25 },
  { 20.0, 0.25, -0.5, 0.0 },
  { 20.5, 0.25, -1.0, -0.5 },
  { 20.5, 1.0, -0.5, 0.5 },
  { 0.75, 0.5, -0.5, 0.25 }
};

static const NumericArgsProfile descending_profile = {
  "__arm64_pure_numeric_args_descending", NUMERIC_ARGS_SUB_DESCENDING,
  NUMERIC_ARGS_STRICT, BC_ISGE, 4, 3, BC_SUBVV, IR_SUB,
  IR_LT, IR_GT, A64I_FSUBd, 1, CC_HS, CC_LO,
  { 20.5, 0.25, 0.5, 0.0 },
  { 0.5, -0.625, 0.375, -0.625 },
  { 20.5, 0.25, 0.5, 0.0 },
  { 20.25, 0.25, 0.5, 0.0 },
  { 20.0, 0.25, 0.5, 0.0 },
  { 20.5, 0.25, 1.0, -0.5 },
  { 20.5, 1.0, 0.5, 1.0 },
  { 0.75, 0.5, 0.5, 0.25 }
};

/* The inclusive reuse tuple is made entirely of exact binary fractions.
** Replacing only x, limit, or step with its recording value would produce
** -0.75, 0.125, or -1.125 respectively instead of -0.875. */
static const NumericArgsProfile descending_inclusive_profile = {
  "__arm64_pure_numeric_args_descending_inclusive",
  NUMERIC_ARGS_SUB_DESCENDING,
  NUMERIC_ARGS_INCLUSIVE, BC_ISGT, 4, 3, BC_SUBVV, IR_SUB,
  IR_LE, IR_GE, A64I_FSUBd, 1, CC_HI, CC_LS,
  { 20.5, 0.25, 0.5, 0.0 },
  { 0.375, -0.625, 0.25, -0.875 },
  { 20.5, 0.25, 0.5, 0.0 },
  { 20.25, 0.25, 0.5, -0.25 },
  { 20.0, 0.25, 0.5, 0.0 },
  { 20.5, 0.25, 1.0, -0.5 },
  { 20.5, 1.0, 0.5, 0.5 },
  { 0.75, 0.5, 0.5, 0.25 }
};

typedef struct NumericArgsIntModeData {
  NumericArgsCall record;
  NumericArgsCall reuse;
  lua_Number substitute_x_result;
  lua_Number substitute_limit_result;
  lua_Number substitute_step_result;
  NumericArgsCall integer_x;
  NumericArgsCall integer_limit;
  NumericArgsCall precondition;
} NumericArgsIntModeData;

/* Comparison pairs share one INT-invariant dataset per recurrence family.
** None of these ordinary rows lands exactly on its comparison boundary. */
static const NumericArgsIntModeData int_mode_data[
  NUMERIC_ARGS_EVOLUTION_MAX] = {
  [NUMERIC_ARGS_ADD_ASCENDING] = {
    { 0.5, 20.25, 2.0, 20.5 },
    { 0.625, 5.5, 4.0, 8.625 },
    8.5, 20.625, 6.625,
    { 0.0, 20.25, 2.0, 22.0 },
    { 0.5, 20.0, 2.0, 20.5 },
    { 0.5, 1.0, 2.0, 2.5 }
  },
  [NUMERIC_ARGS_MUL_ASCENDING] = {
    { 0.5, 20.25, 2.0, 32.0 },
    { 0.625, 5.5, 3.0, 5.625 },
    13.5, 50.625, 10.0,
    { 1.0, 20.25, 2.0, 32.0 },
    { 0.5, 20.0, 2.0, 32.0 },
    { 0.75, 1.0, 2.0, 1.5 }
  },
  [NUMERIC_ARGS_DIV_ASCENDING] = {
    { -20.5, -0.25, 2.0, -0.16015625 },
    { -18.5, -0.75, 4.0, -0.2890625 },
    -0.3203125, -0.072265625, -0.578125,
    { -20.0, -0.25, 2.0, -0.15625 },
    { -20.5, -1.0, 2.0, -0.640625 },
    { -1.0, -0.75, 2.0, -0.5 }
  },
  [NUMERIC_ARGS_DIV_DESCENDING] = {
    { 20.5, 0.25, 2.0, 0.16015625 },
    { 18.5, 0.75, 4.0, 0.2890625 },
    0.3203125, 0.072265625, 0.578125,
    { 20.0, 0.25, 2.0, 0.15625 },
    { 20.5, 1.0, 2.0, 0.640625 },
    { 1.0, 0.75, 2.0, 0.5 }
  },
  [NUMERIC_ARGS_ADD_DESCENDING] = {
    { 20.5, 0.25, -2.0, -1.5 },
    { 5.5, -2.25, -6.0, -6.5 },
    -3.5, -0.5, -2.5,
    { 20.0, 0.25, -2.0, 0.0 },
    { 20.5, 1.0, -2.0, 0.5 },
    { 1.5, 1.0, -2.0, -0.5 }
  },
  [NUMERIC_ARGS_SUB_DESCENDING] = {
    { 20.5, 0.25, 2.0, -1.5 },
    { 5.5, -2.25, 6.0, -6.5 },
    -3.5, -0.5, -2.5,
    { 20.0, 0.25, 2.0, 0.0 },
    { 20.5, 1.0, 2.0, 0.5 },
    { 1.5, 1.0, 2.0, -0.5 }
  }
};

static const NumericArgsIntModeData *numeric_args_int_data(
	const NumericArgsProfile *profile)
{
  assert((unsigned)profile->evolution < NUMERIC_ARGS_EVOLUTION_MAX);
  return &int_mode_data[profile->evolution];
}

#define QNAN_BITS UINT64_C(0x7ff8000000000000)
#define PINF_BITS UINT64_C(0x7ff0000000000000)
#define NINF_BITS UINT64_C(0xfff0000000000000)
#define ZERO_BITS UINT64_C(0x0000000000000000)
#define NEGZERO_BITS UINT64_C(0x8000000000000000)
#define ONE_BITS UINT64_C(0x3ff0000000000000)
#define NEGONE_BITS UINT64_C(0xbff0000000000000)

static const IRRef expected_snaprefs[] = {
  R_X, R_LIMIT, R_PRECOND, R_LOOP, R_COND
};
static const MSize expected_mapofs[] = { 0, 2, 6, 9, 12 };
static const uint8_t expected_nent[] = { 0, 2, 1, 1, 1 };
static const uint8_t expected_nslots[] = { 5, 6, 5, 5, 5 };
static const uint8_t expected_pcpos[] = { 6, 2, 11, 6, 11 };
static const uint8_t expected_map_slots[] = { 2, 5, 2, 2, 2 };
static const IRRef expected_map_refs[] = {
  R_X_PRE, R_X_PRE, R_X_PRE, R_X_PRE, R_X_BODY
};

static void run_lua(lua_State *L, const char *chunk)
{
  int status = luaL_dostring(L, chunk);
  if (status != LUA_OK) {
    fprintf(stderr, "ARM64 dynamic-args NUM chunk failed: %s\n",
	    lua_tostring(L, -1));
    assert(status == LUA_OK);
  }
}

static GCproto *global_proto(lua_State *L, const char *name)
{
  GCfunc *fn;
  GCproto *pt;
  lua_getglobal(L, name);
  assert(lua_isfunction(L, -1));
  fn = funcV(L->top-1);
  assert(isluafunc(fn));
  pt = funcproto(fn);
  lua_pop(L, 1);
  return pt;
}

static lua_Number call_triple(lua_State *L, const char *name,
	lua_Number x, lua_Number limit, lua_Number step,
	int integer_x, int integer_limit, int integer_step)
{
  void *saved_cframe = L->cframe;
  lua_Number result;
  int status;
  lua_getglobal(L, name);
  assert(lua_isfunction(L, -1));
  if (integer_x)
    lua_pushinteger(L, (lua_Integer)x);
  else
    lua_pushnumber(L, x);
  if (integer_limit)
    lua_pushinteger(L, (lua_Integer)limit);
  else
    lua_pushnumber(L, limit);
  if (integer_step)
    lua_pushinteger(L, (lua_Integer)step);
  else
    lua_pushnumber(L, step);
  status = lua_pcall(L, 3, 1, 0);
  if (status != LUA_OK) {
    fprintf(stderr, "ARM64 dynamic-args NUM call failed: %s\n",
	    lua_tostring(L, -1));
    assert(status == LUA_OK);
  }
  assert(lua_isnumber(L, -1));
  result = lua_tonumber(L, -1);
  lua_pop(L, 1);
  assert(L->cframe == saved_cframe);
  return result;
}

typedef enum PostAdmissionRequest {
  POSTADMISSION_PROFILE,
  POSTADMISSION_STOPREQ,
  POSTADMISSION_QNAN_X,
  POSTADMISSION_PINF_X,
  POSTADMISSION_NINF_X,
  POSTADMISSION_ZERO_X,
  POSTADMISSION_NEGZERO_X,
  POSTADMISSION_QNAN_LIMIT,
  POSTADMISSION_PINF_LIMIT,
  POSTADMISSION_NINF_LIMIT,
  POSTADMISSION_ZERO_LIMIT,
  POSTADMISSION_NEGZERO_LIMIT,
  POSTADMISSION_QNAN_STEP,
  POSTADMISSION_PINF_STEP,
  POSTADMISSION_NINF_STEP,
  POSTADMISSION_ZERO_STEP,
  POSTADMISSION_NEGZERO_STEP,
  POSTADMISSION_ONE_STEP,
  POSTADMISSION_NEGONE_STEP
} PostAdmissionRequest;

typedef struct PostAdmissionPublisher {
  lua_State *L;
  global_State *g;
  TGState *tg;
  uint64_t epoch;
  lua_Number expected_value;
  PostAdmissionRequest request;
  uint32_t stop_after_mutation;
  uint32_t saw_stage;
  uint32_t saw_jit_base;
  uint32_t mutated;
  uint32_t published;
} PostAdmissionPublisher;

static void clear_stopreq(TGState *tg)
{
  (void)lj_tg_flags_and_rlx(tg,
	(uint8_t)~(TGF_STOPREQ|TGF_STOPREQ_FRESH));
}

static void wait_for_postadmission(PostAdmissionPublisher *publisher)
{
  uint32_t i;
  for (i = 0; i < 10000000u; i++) {
    if (lj_trace_test_root_entry_paused() ==
	LJ_TRACE_ROOT_ENTRY_PAUSE_POSTADMISSION) {
      la_store32_rel(&publisher->saw_stage, 1);
      return;
    }
    (void)lj_thr_retry_yield(NULL);
  }
  assert(!"ARM64 dynamic-args NUM root entry did not reach pause");
}

static void *publish_postadmission_request(void *arg)
{
  PostAdmissionPublisher *publisher = (PostAdmissionPublisher *)arg;
  global_State *g = publisher->g;
  TGState *tg = publisher->tg;
  TValue *base;

  wait_for_postadmission(publisher);
  assert(gc2_hs_epoch_acq(g) == publisher->epoch);
  assert(lj_tg_hs_epoch_ack_acq(tg) == publisher->epoch);
  assert(gc2_hs_pending_acq(g) == 0);
  assert(lj_tg_reqmask_acq(tg) == 0);
  assert(lj_tg_poll_acq(tg) == 0);
  assert(lj_tg_profile_request_acq(tg) == 0);
  base = lj_tg_load_jit_base(tg);
  if (base != NULL)
    la_store32_rel(&publisher->saw_jit_base, 1);
  assert(la_load32_acq(&publisher->saw_jit_base) == 1);
  assert(base == publisher->L->base);

  if (publisher->request == POSTADMISSION_PROFILE) {
    lj_tg_profile_request_rel(tg, 1);
  } else if (publisher->request == POSTADMISSION_STOPREQ) {
    gc2_hs_actions_rel(g, LJ_GC2_HS_STOPREQ);
    gc2_hs_pending_rel(g, 1);
    gc2_hs_epoch_rel(g, publisher->epoch+1u);
    lj_tg_reqmask_rel(tg, LJ_GC2_HS_STOPREQ);
    lj_tg_poll_rel(tg, 1);
  } else {
    TValue live;
    TValue *target;
    uint64_t replacement;
    int stop_after_mutation;
    /* Admission has finished and the owner has published this frame, but
    ** native SLOAD/FADD/FSUB/FMUL/FDIV have not run. Arguments occupy
    ** base[0..2]. */
    if (publisher->request == POSTADMISSION_QNAN_X ||
	publisher->request == POSTADMISSION_PINF_X ||
	publisher->request == POSTADMISSION_NINF_X ||
	publisher->request == POSTADMISSION_ZERO_X ||
	publisher->request == POSTADMISSION_NEGZERO_X) {
      target = &base[0];
    } else if (publisher->request == POSTADMISSION_QNAN_LIMIT ||
	publisher->request == POSTADMISSION_PINF_LIMIT ||
	publisher->request == POSTADMISSION_NINF_LIMIT ||
	publisher->request == POSTADMISSION_ZERO_LIMIT ||
	publisher->request == POSTADMISSION_NEGZERO_LIMIT) {
      target = &base[1];
    } else {
      target = &base[2];
    }
    lj_tv_load_acq(&live, target);
    assert(tvisnum(&live));
    assert(numV(&live) == publisher->expected_value);
    if (publisher->request == POSTADMISSION_QNAN_X ||
	publisher->request == POSTADMISSION_QNAN_LIMIT ||
	publisher->request == POSTADMISSION_QNAN_STEP) {
      replacement = QNAN_BITS;
    } else if (publisher->request == POSTADMISSION_NINF_X ||
	publisher->request == POSTADMISSION_NINF_LIMIT ||
	publisher->request == POSTADMISSION_NINF_STEP) {
      replacement = NINF_BITS;
    } else if (publisher->request == POSTADMISSION_NEGZERO_X ||
	publisher->request == POSTADMISSION_NEGZERO_LIMIT ||
	publisher->request == POSTADMISSION_NEGZERO_STEP) {
      replacement = NEGZERO_BITS;
    } else if (publisher->request == POSTADMISSION_ZERO_STEP ||
	publisher->request == POSTADMISSION_ZERO_LIMIT ||
	publisher->request == POSTADMISSION_ZERO_X) {
      replacement = ZERO_BITS;
    } else if (publisher->request == POSTADMISSION_NEGONE_STEP) {
      replacement = NEGONE_BITS;
    } else if (publisher->request == POSTADMISSION_ONE_STEP) {
      replacement = ONE_BITS;
    } else {
      replacement = PINF_BITS;
    }
    stop_after_mutation = publisher->stop_after_mutation != 0;
    tv_rawstore_rel(target, replacement);
    lj_tv_load_acq(&live, target);
    assert(tvisnum(&live));
    if (replacement == QNAN_BITS)
      assert(tvisnan(&live));
    else if (replacement == PINF_BITS)
      assert(isinf(numV(&live)) && numV(&live) > 0);
    else if (replacement == ZERO_BITS)
      assert(numV(&live) == 0.0 && !signbit(numV(&live)));
    else if (replacement == NEGZERO_BITS)
      assert(numV(&live) == 0.0 && signbit(numV(&live)));
    else if (replacement == ONE_BITS)
      assert(numV(&live) == 1.0);
    else if (replacement == NEGONE_BITS)
      assert(numV(&live) == -1.0);
    else
      assert(isinf(numV(&live)) && numV(&live) < 0);
    la_store32_rel(&publisher->mutated, 1);
    if (stop_after_mutation) {
      gc2_hs_actions_rel(g, LJ_GC2_HS_STOPREQ);
      gc2_hs_pending_rel(g, 1);
      gc2_hs_epoch_rel(g, publisher->epoch+1u);
      lj_tg_reqmask_rel(tg, LJ_GC2_HS_STOPREQ);
      lj_tg_poll_rel(tg, 1);
    }
  }
  la_store32_rel(&publisher->published, 1);
  lj_trace_test_root_entry_release();
  return NULL;
}

static void expect_bc_ad(const BCIns *bc, MSize pos, BCOp op,
	BCReg a, BCReg d)
{
  BCIns ins = (BCIns)la_load32_acq((const uint32_t *)&bc[pos]);
  assert(bc_op(ins) == op);
  assert(bc_a(ins) == a);
  assert(bc_d(ins) == d);
}

static void expect_proto_shape(const GCproto *pt,
	const NumericArgsProfile *profile)
{
  const BCIns *bc = proto_bc(pt);
  BCIns ins;
  assert(pt->framesize == 5 && pt->sizebc == 13 && pt->numparams == 3);
  assert(pt->sizeuv == 0 && pt->sizekn == 0 && pt->sizekgc == 0);
  assert(pt->flags == PROTO_HAS_RETURN);
  assert(pt->flags2 == PROTO2_CELLOPS);
  expect_bc_ad(bc, 0, BC_FUNCF, 5, 0);
  expect_bc_ad(bc, 1, BC_CGET, 3, 0);
  expect_bc_ad(bc, 2, BC_CGET, 4, 1);
  expect_bc_ad(bc, 3, profile->bytecode_op,
	profile->compare_a, profile->compare_d);
  ins = (BCIns)la_load32_acq((const uint32_t *)&bc[4]);
  assert(bc_op(ins) == BC_JMP && bc_a(ins) == 3 && bc_j(ins) == 6);
  ins = (BCIns)la_load32_acq((const uint32_t *)&bc[5]);
  assert(bc_op(ins) == BC_JLOOP && bc_a(ins) == 3 && bc_d(ins) == 1);
  expect_bc_ad(bc, 6, BC_CGET, 3, 0);
  expect_bc_ad(bc, 7, BC_CGET, 4, 2);
  ins = (BCIns)la_load32_acq((const uint32_t *)&bc[8]);
  assert(bc_op(ins) == profile->recurrence_bc && bc_a(ins) == 3);
  assert(bc_b(ins) == 3 && bc_c(ins) == 4);
  expect_bc_ad(bc, 9, BC_CSET, 0, 3);
  ins = (BCIns)la_load32_acq((const uint32_t *)&bc[10]);
  assert(bc_op(ins) == BC_JMP && bc_a(ins) == 3 && bc_j(ins) == -10);
  expect_bc_ad(bc, 11, BC_CGET, 3, 0);
  expect_bc_ad(bc, 12, BC_RET1, 3, 2);
}

static void expect_ir(const IRIns *ir, IRRef ref, IROp op, uint8_t type,
	IRRef op1, IRRef op2)
{
  IRIns ins = ir_load_acq(&ir[ref]);
  if (ins.o != op || ins.t.irt != type || ins.op1 != op1 || ins.op2 != op2) {
    fprintf(stderr, "dynamic args NUM IR %u got op=%u type=%u op1=%u op2=%u; "
	    "wanted op=%u type=%u op1=%u op2=%u\n",
	    (unsigned)(ref-REF_FIRST), (unsigned)ins.o,
	    (unsigned)ins.t.irt, (unsigned)ins.op1, (unsigned)ins.op2,
	    (unsigned)op, (unsigned)type, (unsigned)op1, (unsigned)op2);
  }
  assert(ins.o == op);
  assert(ins.t.irt == type);
  assert(ins.op1 == op1);
  assert(ins.op2 == op2);
}

static Reg expect_fpr(const IRIns *ir, IRRef ref)
{
  IRIns ins = ir_load_acq(&ir[ref]);
  assert(ins.r >= RID_MIN_FPR && ins.r < RID_MAX_FPR);
  assert(rset_test(RSET_FPR, ins.r));
  assert(!ra_hasspill(ins.s));
  return ins.r;
}

static Reg expect_gpr(const IRIns *ir, IRRef ref)
{
  IRIns ins = ir_load_acq(&ir[ref]);
  assert(ins.r >= RID_MIN_GPR && ins.r < RID_MAX_GPR);
  assert(rset_test(RSET_GPR, ins.r));
  assert(!ra_hasspill(ins.s));
  return ins.r;
}

static unsigned fpr_index(Reg reg)
{
  assert(reg >= RID_MIN_FPR && reg < RID_MAX_FPR);
  return (unsigned)(reg-RID_MIN_FPR);
}

static void dump_unexpected_postra(const GCtrace *T,
	NumericArgsStepKind step_kind)
{
  const IRIns *ir = trace_ir_acq(T);
  IRRef ref;
  fprintf(stderr, "dynamic args NUM post-RA nk=%u nins=%u, expected %u/%u\n",
	  (unsigned)trace_nk_acq(T), (unsigned)trace_nins_acq(T),
	  (unsigned)REF_TRUE, (unsigned)numeric_args_end_ref(step_kind));
  for (ref = REF_TRUE; ref < trace_nins_acq(T); ref++) {
    IRIns ins = ir_load_acq(&ir[ref]);
    fprintf(stderr, "post-RA ref=%u op=%u type=%u op1=%u op2=%u "
	    "r=%u s=%u prev=%u raw=%#llx\n",
	    (unsigned)ref, (unsigned)ins.o, (unsigned)ins.t.irt,
	    (unsigned)ins.op1, (unsigned)ins.op2, (unsigned)ins.r,
	    (unsigned)ins.s, (unsigned)ins.prev,
	    (unsigned long long)ins.tv.u64);
  }
}

static void expect_ir_shape(const GCtrace *T,
	const NumericArgsProfile *profile, NumericArgsStepKind step_kind)
{
  const IRIns *ir = trace_ir_acq(T);
  IRIns suffix;
  IRRef step_value = numeric_args_step_value_ref(step_kind);
  IRRef xpre_ref = numeric_args_ref(step_kind, R_X_PRE);
  IRRef limit_ref = numeric_args_ref(step_kind, R_LIMIT);
  IRRef precond_ref = numeric_args_ref(step_kind, R_PRECOND);
  IRRef loop_ref = numeric_args_ref(step_kind, R_LOOP);
  IRRef xpoll_ref = numeric_args_ref(step_kind, R_XPOLL);
  IRRef xbody_ref = numeric_args_ref(step_kind, R_X_BODY);
  IRRef cond_ref = numeric_args_ref(step_kind, R_COND);
  IRRef xphi_ref = numeric_args_ref(step_kind, R_X_PHI);
  IRRef nop_ref = numeric_args_ref(step_kind, R_NOP);
  IRRef end_ref = numeric_args_end_ref(step_kind);
  Reg x, step, xpre, limit, xbody, xphi;
  IRRef ref;

  if (trace_nk_acq(T) != REF_TRUE || trace_nins_acq(T) != end_ref)
    dump_unexpected_postra(T, step_kind);
  assert(trace_nk_acq(T) == REF_TRUE);
  assert(trace_nins_acq(T) == end_ref);
  for (ref = REF_TRUE; ref <= REF_NIL; ref++) {
    IRIns pri = ir_load_acq(&ir[ref]);
    assert(pri.o == IR_KPRI);
    assert(pri.t.irt == (uint8_t)(REF_NIL-ref));
    assert(pri.op12 == 0);
  }
  expect_ir(ir, REF_BASE, IR_BASE, IRT_PGC, 0, 0);
  expect_ir(ir, R_X, IR_SLOAD, IRT_NUM|IRT_GUARD,
	    2, IRSLOAD_TYPECHECK);
  expect_ir(ir, R_STEP, IR_SLOAD,
	    (uint8_t)((step_kind == NUMERIC_ARGS_STEP_INT ? IRT_INT : IRT_NUM)|
	    IRT_GUARD), 4, IRSLOAD_TYPECHECK);
  if (step_kind == NUMERIC_ARGS_STEP_INT)
    expect_ir(ir, step_value, IR_CONV, IRT_NUM,
	      R_STEP, IRCONV_NUM_INT);
  if (profile->evolution == NUMERIC_ARGS_SUB_DESCENDING ||
      profile->evolution == NUMERIC_ARGS_DIV_ASCENDING ||
      profile->evolution == NUMERIC_ARGS_DIV_DESCENDING) {
    expect_ir(ir, xpre_ref, profile->recurrence_ir,
	IRT_NUM|IRT_ISPHI, R_X, step_value);
  } else {
    expect_ir(ir, xpre_ref, profile->recurrence_ir,
	IRT_NUM|IRT_ISPHI, step_value, R_X);
  }
  expect_ir(ir, limit_ref, IR_SLOAD, IRT_NUM|IRT_GUARD,
	    3, IRSLOAD_TYPECHECK);
  expect_ir(ir, precond_ref, profile->precondition_op,
	IRT_NUM|IRT_GUARD, limit_ref, xpre_ref);
  expect_ir(ir, loop_ref, IR_LOOP, IRT_NIL|IRT_GUARD, 0, 0);
  expect_ir(ir, xpoll_ref, IR_XPOLL, IRT_NIL|IRT_GUARD, 1, 0);
  expect_ir(ir, xbody_ref, profile->recurrence_ir, IRT_NUM|IRT_ISPHI,
	    xpre_ref, step_value);
  expect_ir(ir, cond_ref, profile->body_op,
	IRT_NUM|IRT_GUARD, xbody_ref, limit_ref);
  expect_ir(ir, xphi_ref, IR_PHI, IRT_NUM, xpre_ref, xbody_ref);
  suffix = ir_load_acq(&ir[nop_ref]);
  assert(suffix.o == IR_NOP && suffix.t.irt == IRT_NIL);
  assert(suffix.op1 == 0 && suffix.op2 == 0 && suffix.prev == 0);
  for (ref = REF_BASE; ref <= xphi_ref; ref++) {
    IRIns ins = ir_load_acq(&ir[ref]);
    assert(!ra_hasspill(ins.s));
    assert(ins.o != IR_RENAME);
  }

  x = expect_fpr(ir, R_X);
  step = expect_fpr(ir, step_value);
  xpre = expect_fpr(ir, xpre_ref);
  limit = expect_fpr(ir, limit_ref);
  xbody = expect_fpr(ir, xbody_ref);
  xphi = expect_fpr(ir, xphi_ref);
  assert(fpr_index(x) == 2);
  assert(fpr_index(step) == 1);
  assert(fpr_index(xpre) == 15);
  assert(fpr_index(limit) == 0);
  assert(xpre == xbody && xpre == xphi);
  assert(step != xphi && limit != xphi && step != limit);
  assert(x != step);
  if (step_kind == NUMERIC_ARGS_STEP_INT)
    assert(expect_gpr(ir, R_STEP) == RID_X1);
}

static void expect_snapshot_shape(const GCtrace *T, const GCproto *pt,
	NumericArgsStepKind step_kind)
{
  const IRIns *ir = trace_ir_acq(T);
  const SnapShot *snap = trace_snap_acq(T);
  SnapEntry *snapmap = trace_snapmap_acq(T);
  Reg phireg = expect_fpr(ir,
	numeric_args_ref(step_kind, R_X_PHI));
  MSize tuple = 0;
  SnapNo snapno;

  assert(trace_nsnap_acq(T) == 5);
  assert(trace_nsnapmap_acq(T) == 15);
  for (snapno = 0; snapno < 5; snapno++) {
    MSize n;
    assert(snap_ref_acq(&snap[snapno]) ==
	   numeric_args_ref(step_kind, expected_snaprefs[snapno]));
    assert(snap_mapofs_acq(&snap[snapno]) == expected_mapofs[snapno]);
    assert(snap_nent_acq(&snap[snapno]) == expected_nent[snapno]);
    assert(snap_nslots_acq(&snap[snapno]) == expected_nslots[snapno]);
    assert(snap_topslot_acq(&snap[snapno]) == 5);
    for (n = 0; n < expected_nent[snapno]; n++) {
      MSize mapno = expected_mapofs[snapno]+n;
      SnapEntry sn = snapentry_acq(&snapmap[mapno]);
      IRIns value;
      assert(tuple < sizeof(expected_map_refs)/sizeof(expected_map_refs[0]));
      assert(sn == SNAP(expected_map_slots[tuple], 0,
		       numeric_args_ref(step_kind,
			 expected_map_refs[tuple])));
      value = ir_load_acq(&ir[snap_ref(sn)]);
      assert(value.t.irt == (IRT_NUM|IRT_ISPHI));
      assert(value.r == phireg && !ra_hasspill(value.s));
      tuple++;
    }
    assert(snap_pc_acq(&snapmap[expected_mapofs[snapno]+
	   expected_nent[snapno]]) == proto_bc(pt)+expected_pcpos[snapno]);
#if LJ_FR2
    {
      uint64_t pcbase;
      SnapEntry raw[2];
      raw[0] = snapentry_acq(&snapmap[expected_mapofs[snapno]+
		expected_nent[snapno]]);
      raw[1] = snapentry_acq(&snapmap[expected_mapofs[snapno]+
		expected_nent[snapno]+1u]);
      memcpy(&pcbase, raw, sizeof(pcbase));
      assert((uint8_t)pcbase == 0);
    }
#endif
  }
  assert(tuple == sizeof(expected_map_refs)/sizeof(expected_map_refs[0]));
}

static int32_t sign_extend_branch(uint32_t value, unsigned bits)
{
  return (int32_t)(value << (32u-bits)) >> (32u-bits);
}

static uint32_t numeric_args_int_first_word(
	const NumericArgsProfile *profile)
{
  if (profile->evolution == NUMERIC_ARGS_MUL_ASCENDING)
    return UINT32_C(0x1e62082f);
  if (profile->evolution == NUMERIC_ARGS_DIV_ASCENDING ||
      profile->evolution == NUMERIC_ARGS_DIV_DESCENDING)
    return UINT32_C(0x1e61184f);
  if (profile->evolution == NUMERIC_ARGS_SUB_DESCENDING)
    return UINT32_C(0x1e61384f);
  return UINT32_C(0x1e62282f);
}

static uint32_t numeric_args_int_body_word(
	const NumericArgsProfile *profile)
{
  if (profile->evolution == NUMERIC_ARGS_MUL_ASCENDING)
    return UINT32_C(0x1e6109ef);
  if (profile->evolution == NUMERIC_ARGS_DIV_ASCENDING ||
      profile->evolution == NUMERIC_ARGS_DIV_DESCENDING)
    return UINT32_C(0x1e6119ef);
  if (profile->evolution == NUMERIC_ARGS_SUB_DESCENDING)
    return UINT32_C(0x1e6139ef);
  return UINT32_C(0x1e6129ef);
}

static void expect_dynamic_fp_mcode(const GCtrace *T,
	const NumericArgsProfile *profile, NumericArgsStepKind step_kind)
{
  const IRIns *ir = trace_ir_acq(T);
  const MCode *mcode = trace_mcode_acq(T);
  const MCode *exitstub = trace_exitstub_acq(T);
  MSize nword = trace_szmcode_acq(T) / sizeof(MCode);
  MSize i;
  unsigned nfarith = 0, nfirstarith = 0, nbodyarith = 0;
  unsigned nopposite = 0;
  unsigned nfcmp = 0, npre = 0, nbody = 0;
  unsigned nscvtf = 0;
  unsigned xreg = fpr_index(expect_fpr(ir, R_X));
  IRRef step_value = numeric_args_step_value_ref(step_kind);
  unsigned stepreg = fpr_index(expect_fpr(ir, step_value));
  unsigned stepintreg = step_kind == NUMERIC_ARGS_STEP_INT ?
    (unsigned)(expect_gpr(ir, R_STEP)-RID_MIN_GPR) : 0;
  unsigned phireg = fpr_index(expect_fpr(ir,
	numeric_args_ref(step_kind, R_X_PHI)));
  unsigned limitreg = fpr_index(expect_fpr(ir,
	numeric_args_ref(step_kind, R_LIMIT)));
  const uint32_t farith_mask =
    ~(uint32_t)(A64F_D(31u)|A64F_N(31u)|A64F_M(31u));
  const uint32_t fcmp_mask =
    ~(uint32_t)(A64F_N(31u)|A64F_M(31u));
  const uint32_t fcvt_mask =
    ~(uint32_t)(A64F_D(31u)|A64F_N(31u));

  assert(fpr_index(expect_fpr(ir,
	numeric_args_ref(step_kind, R_X_PRE))) == phireg);
  assert(fpr_index(expect_fpr(ir,
	numeric_args_ref(step_kind, R_X_BODY))) == phireg);
  assert(stepreg != phireg && limitreg != phireg && stepreg != limitreg);
  assert(xreg != stepreg);
  assert((trace_szmcode_acq(T) & (sizeof(MCode)-1u)) == 0);
  for (i = 0; i < nword; i++) {
    uint32_t ins = mcode[i];
    if ((ins & fcvt_mask) == A64I_FCVT_F64_S32) {
      assert(step_kind == NUMERIC_ARGS_STEP_INT);
      assert((ins & 31u) == stepreg);
      assert(((ins >> 5) & 31u) == stepintreg);
      assert(ins == UINT32_C(0x1e620021));
      assert(i < trace_mcloop_acq(T)/sizeof(MCode));
      nscvtf++;
    }
    if ((ins & farith_mask) == profile->recurrence_mcode) {
      unsigned dest = ins & 31u;
      unsigned left = (ins >> 5) & 31u;
      unsigned right = (ins >> 16) & 31u;
      assert(dest == phireg);
      if (profile->evolution == NUMERIC_ARGS_SUB_DESCENDING ||
	  profile->evolution == NUMERIC_ARGS_DIV_ASCENDING ||
	  profile->evolution == NUMERIC_ARGS_DIV_DESCENDING) {
        assert(right == stepreg);
        if (xreg == phireg) {
          assert(left == phireg);
        } else if (left == xreg) {
          nfirstarith++;
        } else {
          assert(left == phireg);
          nbodyarith++;
        }
      } else if (profile->evolution == NUMERIC_ARGS_MUL_ASCENDING) {
	if (left == stepreg && right == xreg) {
	  nfirstarith++;
	} else {
	  assert(left == phireg && right == stepreg);
	  nbodyarith++;
	}
      } else {
        unsigned other;
        assert((left == stepreg) != (right == stepreg));
        other = left == stepreg ? right : left;
        if (xreg == phireg) {
          assert(other == phireg);
        } else if (other == xreg) {
          nfirstarith++;
        } else {
          assert(other == phireg);
          nbodyarith++;
        }
      }
      nfarith++;
    }
    if (((ins & farith_mask) == A64I_FADDd ||
	 (ins & farith_mask) == A64I_FSUBd ||
	 (ins & farith_mask) == A64I_FMULd ||
	 (ins & farith_mask) == A64I_FDIVd) &&
	(ins & farith_mask) != profile->recurrence_mcode)
      nopposite++;
    if ((ins & fcmp_mask) == A64I_FCMPd) {
      uint32_t branch;
      int32_t delta;
      const MCode *target;
      const MCode *pretarget = exitstub_trace_addr_(
	(MCode *)(uintptr_t)exitstub, PRECOND_EXIT);
      const MCode *bodytarget = exitstub_trace_addr_(
	(MCode *)(uintptr_t)exitstub, FINAL_EXIT);
      const MCode *looptarget = mcode+
	trace_mcloop_acq(T)/sizeof(MCode);
      unsigned left = (ins >> 5) & 31u;
      unsigned right = (ins >> 16) & 31u;
      if (profile->fcmp_limit_first)
        assert(left == limitreg && right == phireg);
      else
        assert(left == phireg && right == limitreg);
      assert(i+1u < nword);
      branch = mcode[i+1u];
      assert((branch & UINT32_C(0xff000010)) == A64I_BCC);
      delta = sign_extend_branch((branch >> 5) & 0x7ffffu, 19);
      target = &mcode[i+1u]+delta;
      if ((branch & 15u) == profile->precondition_exit_cc) {
        assert(target == pretarget);
        npre++;
      } else {
        uint32_t exit_branch;
        int32_t exit_delta;
        assert((branch & 15u) == profile->body_loop_cc);
        assert(target == looptarget);
        assert(i+2u < nword);
        exit_branch = mcode[i+2u];
        assert((exit_branch & UINT32_C(0xfc000000)) == A64I_B);
        exit_delta = sign_extend_branch(
	  exit_branch & UINT32_C(0x03ffffff), 26);
        assert(&mcode[i+2u]+exit_delta == bodytarget);
        nbody++;
      }
      nfcmp++;
    }
  }
  if (step_kind == NUMERIC_ARGS_STEP_INT) {
    MSize shift = LJ_ABI_BRANCH_TRACK ? 1u : 0u;
    uint32_t fcmp = profile->fcmp_limit_first ?
	UINT32_C(0x1e6f2000) : UINT32_C(0x1e6021e0);
    uint32_t prebranch = profile->comparison == NUMERIC_ARGS_INCLUSIVE ?
	UINT32_C(0x54000488) : UINT32_C(0x54000482);
    uint32_t bodybranch = profile->comparison == NUMERIC_ARGS_INCLUSIVE ?
	UINT32_C(0x54fffe69) : UINT32_C(0x54fffe63);
    assert(nword > shift+34u);
    assert(mcode[shift+12u] == UINT32_C(0x1e620021));
    assert(mcode[shift+13u] == numeric_args_int_first_word(profile));
    assert(mcode[shift+18u] == fcmp);
    assert(mcode[shift+19u] == prebranch);
    assert(mcode[shift+31u] == numeric_args_int_body_word(profile));
    assert(mcode[shift+32u] == fcmp);
    assert(mcode[shift+33u] == bodybranch);
    assert(mcode[shift+34u] == UINT32_C(0x14000025));
  } else if (step_kind == NUMERIC_ARGS_STEP_NUM &&
      profile->evolution == NUMERIC_ARGS_MUL_ASCENDING &&
      profile->comparison == NUMERIC_ARGS_INCLUSIVE) {
    MSize shift = LJ_ABI_BRANCH_TRACK ? 1u : 0u;
    assert(nword > shift+33u);
    assert(mcode[shift+12u] == UINT32_C(0x1e62082f));
    assert(mcode[shift+18u] == UINT32_C(0x54000488));
    assert(mcode[shift+30u] == UINT32_C(0x1e6109ef));
    assert(mcode[shift+31u] == UINT32_C(0x1e6021e0));
    assert(mcode[shift+32u] == UINT32_C(0x54fffe69));
    assert(mcode[shift+33u] == UINT32_C(0x14000025));
  } else if (step_kind == NUMERIC_ARGS_STEP_NUM &&
      profile->evolution == NUMERIC_ARGS_DIV_ASCENDING &&
      profile->comparison == NUMERIC_ARGS_INCLUSIVE) {
    MSize shift = LJ_ABI_BRANCH_TRACK ? 1u : 0u;
    assert(nword > shift+33u);
    assert(mcode[shift+12u] == UINT32_C(0x1e61184f));
    assert(mcode[shift+17u] == UINT32_C(0x1e6021e0));
    assert(mcode[shift+18u] == UINT32_C(0x54000488));
    assert(mcode[shift+30u] == UINT32_C(0x1e6119ef));
    assert(mcode[shift+31u] == UINT32_C(0x1e6021e0));
    assert(mcode[shift+32u] == UINT32_C(0x54fffe69));
    assert(mcode[shift+33u] == UINT32_C(0x14000025));
  } else if (step_kind == NUMERIC_ARGS_STEP_NUM &&
      profile->evolution == NUMERIC_ARGS_DIV_ASCENDING) {
    MSize shift = LJ_ABI_BRANCH_TRACK ? 1u : 0u;
    assert(nword > shift+33u);
    assert(mcode[shift+12u] == UINT32_C(0x1e61184f));
    assert(mcode[shift+17u] == UINT32_C(0x1e6021e0));
    assert(mcode[shift+18u] == UINT32_C(0x54000482));
    assert(mcode[shift+30u] == UINT32_C(0x1e6119ef));
    assert(mcode[shift+31u] == UINT32_C(0x1e6021e0));
    assert(mcode[shift+32u] == UINT32_C(0x54fffe63));
    assert(mcode[shift+33u] == UINT32_C(0x14000025));
  } else if (step_kind == NUMERIC_ARGS_STEP_NUM &&
      profile->evolution == NUMERIC_ARGS_DIV_DESCENDING &&
      profile->comparison == NUMERIC_ARGS_INCLUSIVE) {
    MSize shift = LJ_ABI_BRANCH_TRACK ? 1u : 0u;
    assert(nword > shift+33u);
    assert(mcode[shift+12u] == UINT32_C(0x1e61184f));
    assert(mcode[shift+17u] == UINT32_C(0x1e6f2000));
    assert(mcode[shift+18u] == UINT32_C(0x54000488));
    assert(mcode[shift+30u] == UINT32_C(0x1e6119ef));
    assert(mcode[shift+31u] == UINT32_C(0x1e6f2000));
    assert(mcode[shift+32u] == UINT32_C(0x54fffe69));
    assert(mcode[shift+33u] == UINT32_C(0x14000025));
  } else if (step_kind == NUMERIC_ARGS_STEP_NUM &&
      profile->evolution == NUMERIC_ARGS_DIV_DESCENDING) {
    MSize shift = LJ_ABI_BRANCH_TRACK ? 1u : 0u;
    assert(nword > shift+33u);
    assert(mcode[shift+12u] == UINT32_C(0x1e61184f));
    assert(mcode[shift+17u] == UINT32_C(0x1e6f2000));
    assert(mcode[shift+18u] == UINT32_C(0x54000482));
    assert(mcode[shift+30u] == UINT32_C(0x1e6119ef));
    assert(mcode[shift+31u] == UINT32_C(0x1e6f2000));
    assert(mcode[shift+32u] == UINT32_C(0x54fffe63));
    assert(mcode[shift+33u] == UINT32_C(0x14000025));
  }
  assert(nfarith == 2 && nopposite == 0);
  assert(nscvtf == (step_kind == NUMERIC_ARGS_STEP_INT ? 1u : 0u));
  if (xreg != phireg)
    assert(nfirstarith == 1 && nbodyarith == 1);
  assert(nfcmp == 2 && npre == 1 && nbody == 1);
}

static void expect_only_args_root_kind(lua_State *L, GCproto *pt,
	const NumericArgsProfile *profile, NumericArgsStepKind step_kind)
{
  jit_State *J = L2J(L);
  GCtrace *T = traceref_safe(J, 1);
  const BCIns *pc;
  BCIns patched;
  TraceNo traceno;
  uint8_t admission;

  expect_proto_shape(pt, profile);
  assert(trace_runnable_acq(T, 1));
  assert(trace_traceno_acq(T) == 1 && trace_root_acq(T) == 0);
  assert(trace_link_acq(T) == 1 && trace_linktype_acq(T) == LJ_TRLINK_LOOP);
  assert(trace_nchild_acq(T) == 0 && trace_nextside_acq(T) == 0);
  assert(trace_startpt_acq(T) == pt);
  assert(trace_topslot_acq(T) == 5);
  assert(trace_spadjust_acq(T) == 0);
  admission = la_load8_acq(&T->unused1);
  assert((admission & (TRACE_ARM64_INT_LOOP_ADMITTED |
	  TRACE_ARM64_INT_FORL_ADMITTED | TRACE_ARM64_TRUE_FUNCF_ADMITTED |
	  TRACE_ARM64_INT_SIDE_ADMITTED)) == TRACE_ARM64_INT_LOOP_ADMITTED);
  assert(trace_mcode_acq(T) != NULL && trace_szmcode_acq(T) > sizeof(MCode));
  assert(trace_mcloop_acq(T) > 0 &&
	 trace_mcloop_acq(T) < trace_szmcode_acq(T));
#if LJ_ABI_BRANCH_TRACK
  if (step_kind == NUMERIC_ARGS_STEP_NUM) {
    assert(trace_szmcode_acq(T) == 140 && trace_mcloop_acq(T) == 80);
  } else {
    assert(trace_szmcode_acq(T) == 144 && trace_mcloop_acq(T) == 84);
  }
  assert(trace_mcode_acq(T)[0] == A64I_BTI_J);
#else
  if (step_kind == NUMERIC_ARGS_STEP_NUM) {
    assert(trace_szmcode_acq(T) == 136 && trace_mcloop_acq(T) == 76);
  } else {
    assert(trace_szmcode_acq(T) == 140 && trace_mcloop_acq(T) == 80);
  }
#endif
  pc = trace_startpc_acq(T);
  if (pc != proto_bc(pt)+5u)
    fprintf(stderr, "dynamic args NUM startpc offset=%td, expected=5\n",
	    pc-proto_bc(pt));
  assert(pc == proto_bc(pt)+5u);
  assert(bc_op(trace_startins_acq(T)) == BC_LOOP);
  assert(bc_a(trace_startins_acq(T)) == 3);
  assert(bc_j(trace_startins_acq(T)) == 5);
  patched = (BCIns)la_load32_acq((const uint32_t *)pc);
  assert(bc_op(patched) == BC_JLOOP && bc_d(patched) == 1);
  assert(proto_trace_acq(pt) == 1);
  expect_ir_shape(T, profile, step_kind);
  expect_snapshot_shape(T, pt, step_kind);
  expect_dynamic_fp_mcode(T, profile, step_kind);
  for (traceno = 2; (MSize)traceno < trace_sizetrace_acq(J); traceno++)
    assert(!trace_runnable_acq(traceref_safe(J, traceno), traceno));
}

static void expect_only_args_root(lua_State *L, GCproto *pt,
	const NumericArgsProfile *profile)
{
  expect_only_args_root_kind(L, pt, profile, NUMERIC_ARGS_STEP_NUM);
}

static void expect_native_exit(ExitNo first, ExitNo last)
{
  if (lj_trace_test_first_exitno() != first ||
      lj_trace_test_last_exitno() != last)
    fprintf(stderr, "dynamic args NUM exits got first=%u last=%u calls=%u "
	    "publishes=%u; wanted first=%u last=%u\n",
	    (unsigned)lj_trace_test_first_exitno(),
	    (unsigned)lj_trace_test_last_exitno(),
	    (unsigned)lj_trace_test_exit_calls(),
	    (unsigned)lj_trace_test_root_entry_publishes(),
	    (unsigned)first, (unsigned)last);
  assert(lj_trace_test_root_entry_publishes() >= 1);
  assert(lj_trace_test_root_entry_cleanups() == 0);
  assert(lj_trace_test_exit_calls() >= 1);
  assert(lj_trace_test_first_exit_parent() == 1);
  assert(lj_trace_test_first_exitno() == first);
  assert(lj_trace_test_last_exit_parent() == 1);
  assert(lj_trace_test_last_exitno() == last);
}

static void expect_single_exit(ExitNo exitno)
{
  assert(lj_trace_test_root_entry_publishes() == 1);
  assert(lj_trace_test_root_entry_cleanups() == 0);
  assert(lj_trace_test_exit_calls() == 1);
  assert(lj_trace_test_first_exit_parent() == 1);
  assert(lj_trace_test_first_exitno() == exitno);
  assert(lj_trace_test_last_exit_parent() == 1);
  assert(lj_trace_test_last_exitno() == exitno);
}

static void expect_profile_exit_and_reentry(void)
{
  assert(lj_trace_test_root_entry_publishes() == 2);
  assert(lj_trace_test_root_entry_cleanups() == 0);
  assert(lj_trace_test_exit_calls() == 2);
  assert(lj_trace_test_first_exit_parent() == 1);
  assert(lj_trace_test_first_exitno() == XPOLL_EXIT);
  assert(lj_trace_test_last_exit_parent() == 1);
  assert(lj_trace_test_last_exitno() == FINAL_EXIT);
}

static void assert_native_idle(lua_State *L, int32_t idle_vmstate)
{
  TGState *tg = L2TG(L);
  assert(lj_tg_load_jit_base(tg) == NULL);
  assert(lj_tg_in_native_acq(tg) == 0);
  assert(lj_tg_vmstate_load_acq(tg) == idle_vmstate);
}

static void assert_publisher_done(PostAdmissionPublisher *publisher)
{
  assert(la_load32_acq(&publisher->saw_stage) == 1);
  assert(la_load32_acq(&publisher->saw_jit_base) == 1);
  assert(la_load32_acq(&publisher->published) == 1);
  assert(lj_trace_test_root_entry_paused() == 0);
}

static void test_xpoll_lifecycle(lua_State *L, GCproto *pt,
	int32_t idle_vmstate, const NumericArgsProfile *profile,
	NumericArgsStepKind step_kind, const NumericArgsCall *call)
{
  global_State *g = G(L);
  TGState *tg = L2TG(L);
  GCtrace *T = traceref_safe(L2J(L), 1);
  uint64_t epoch = gc2_hs_epoch_acq(g);
  void *saved_cframe = L->cframe;
  PostAdmissionPublisher publisher;
  pthread_t worker;
  int status;

  assert(snap_ref_acq(&trace_snap_acq(T)[XPOLL_EXIT]) ==
	 numeric_args_ref(step_kind, R_LOOP));
  assert(snap_ref_acq(&trace_snap_acq(T)[FINAL_EXIT]) ==
	 numeric_args_ref(step_kind, R_COND));
  assert(lj_tg_hs_epoch_ack_acq(tg) == epoch);
  assert(gc2_hs_leader_acq(g) == 0);
  assert(gc2_hs_pending_acq(g) == 0);
  assert(lj_tg_reqmask_acq(tg) == 0);
  assert(lj_tg_poll_acq(tg) == 0);
  assert(lj_tg_profile_request_acq(tg) == 0);
  assert((lj_tg_flags_acq(tg) &
	  (TGF_STOPREQ|TGF_STOPREQ_FRESH)) == 0);

  lj_trace_test_root_entry_reset();
  lj_trace_test_reset_exit_stats();
  lj_trace_test_root_entry_pause(LJ_TRACE_ROOT_ENTRY_PAUSE_POSTADMISSION);
  publisher = (PostAdmissionPublisher){
    .L = L, .g = g, .tg = tg, .epoch = epoch,
    .expected_value = call->x,
    .request = POSTADMISSION_PROFILE
  };
  assert(pthread_create(&worker, NULL, publish_postadmission_request,
	&publisher) == 0);
  assert(call_triple(L, profile->name,
	call->x, call->limit, call->step, 0, 0,
	step_kind == NUMERIC_ARGS_STEP_INT) == call->result);
  assert(pthread_join(worker, NULL) == 0);
  assert_publisher_done(&publisher);
  expect_profile_exit_and_reentry();
  assert(gc2_hs_epoch_acq(g) == epoch);
  assert(lj_tg_hs_epoch_ack_acq(tg) == epoch);
  assert(gc2_hs_leader_acq(g) == 0);
  assert(gc2_hs_pending_acq(g) == 0);
  assert(lj_tg_reqmask_acq(tg) == 0);
  assert(lj_tg_poll_acq(tg) == 0);
  assert(lj_tg_profile_request_acq(tg) == 0);
  assert_native_idle(L, idle_vmstate);
  assert(L->cframe == saved_cframe);
  expect_only_args_root_kind(L, pt, profile, step_kind);

  clear_stopreq(tg);
  lj_trace_test_root_entry_reset();
  lj_trace_test_reset_exit_stats();
  lj_trace_test_root_entry_pause(LJ_TRACE_ROOT_ENTRY_PAUSE_POSTADMISSION);
  publisher = (PostAdmissionPublisher){
    .L = L, .g = g, .tg = tg, .epoch = epoch,
    .expected_value = call->x,
    .request = POSTADMISSION_STOPREQ
  };
  assert(pthread_create(&worker, NULL, publish_postadmission_request,
	&publisher) == 0);
  lua_getglobal(L, profile->name);
  assert(lua_isfunction(L, -1));
  lua_pushnumber(L, call->x);
  lua_pushnumber(L, call->limit);
  if (step_kind == NUMERIC_ARGS_STEP_INT)
    lua_pushinteger(L, (lua_Integer)call->step);
  else
    lua_pushnumber(L, call->step);
  status = lua_pcall(L, 3, 1, 0);
  assert(pthread_join(worker, NULL) == 0);
  assert(status == LUA_ERRRUN);
  assert(lua_isstring(L, -1));
  assert(strstr(lua_tostring(L, -1),
		"thread interrupted: VM shutdown") != NULL);
  lua_pop(L, 1);
  assert_publisher_done(&publisher);
  expect_single_exit(XPOLL_EXIT);
  assert(gc2_hs_actions_acq(g) == LJ_GC2_HS_STOPREQ);
  assert(gc2_hs_epoch_acq(g) == epoch+1u);
  assert(lj_tg_hs_epoch_ack_acq(tg) == epoch+1u);
  assert(gc2_hs_leader_acq(g) == 0);
  assert(gc2_hs_pending_acq(g) == 0);
  assert(lj_tg_reqmask_acq(tg) == 0);
  assert(lj_tg_poll_acq(tg) == 0);
  assert(lj_tg_profile_request_acq(tg) == 0);
  assert((lj_tg_flags_acq(tg) & TGF_STOPREQ) != 0);
  assert((lj_tg_flags_acq(tg) & TGF_STOPREQ_FRESH) == 0);
  assert_native_idle(L, idle_vmstate);
  assert(L->cframe == saved_cframe);
  clear_stopreq(tg);
  assert((lj_tg_flags_acq(tg) &
	  (TGF_STOPREQ|TGF_STOPREQ_FRESH)) == 0);
  expect_only_args_root_kind(L, pt, profile, step_kind);

  lj_trace_test_root_entry_reset();
  lj_trace_test_reset_exit_stats();
  assert(call_triple(L, profile->name,
	call->x, call->limit, call->step, 0, 0,
	step_kind == NUMERIC_ARGS_STEP_INT) == call->result);
  expect_single_exit(FINAL_EXIT);
  assert_native_idle(L, idle_vmstate);
  assert(L->cframe == saved_cframe);
  expect_only_args_root_kind(L, pt, profile, step_kind);
}

typedef enum MutationResult {
  MUTATION_FINITE,
  MUTATION_QNAN,
  MUTATION_PINF,
  MUTATION_NINF,
  MUTATION_ZERO,
  MUTATION_NEGZERO
} MutationResult;

static void test_terminating_mutation_at_exit(lua_State *L, GCproto *pt,
	int32_t idle_vmstate, const NumericArgsProfile *profile,
	PostAdmissionRequest request, lua_Number expected_live,
	MutationResult result_kind, lua_Number expected_result,
	ExitNo expected_exit)
{
  global_State *g = G(L);
  TGState *tg = L2TG(L);
  uint64_t epoch = gc2_hs_epoch_acq(g);
  PostAdmissionPublisher publisher;
  pthread_t worker;
  lua_Number result;

  lj_trace_test_root_entry_reset();
  lj_trace_test_reset_exit_stats();
  lj_trace_test_root_entry_pause(LJ_TRACE_ROOT_ENTRY_PAUSE_POSTADMISSION);
  publisher = (PostAdmissionPublisher){
    .L = L, .g = g, .tg = tg, .epoch = epoch,
    .expected_value = expected_live, .request = request
  };
  assert(pthread_create(&worker, NULL, publish_postadmission_request,
	&publisher) == 0);
  result = call_triple(L, profile->name,
	profile->mutation.x, profile->mutation.limit,
	profile->mutation.step, 0, 0, 0);
  assert(pthread_join(worker, NULL) == 0);
  assert_publisher_done(&publisher);
  assert(la_load32_acq(&publisher.mutated) == 1);
  if (result_kind == MUTATION_QNAN)
    assert(isnan(result));
  else if (result_kind == MUTATION_PINF)
    assert(isinf(result) && result > 0);
  else if (result_kind == MUTATION_NINF)
    assert(isinf(result) && result < 0);
  else if (result_kind == MUTATION_ZERO)
    assert(result == 0.0 && !signbit(result));
  else if (result_kind == MUTATION_NEGZERO)
    assert(result == 0.0 && signbit(result));
  else
    assert(result == expected_result);
  expect_single_exit(expected_exit);
  assert(gc2_hs_epoch_acq(g) == epoch);
  assert(lj_tg_hs_epoch_ack_acq(tg) == epoch);
  assert(gc2_hs_pending_acq(g) == 0);
  assert(lj_tg_reqmask_acq(tg) == 0);
  assert(lj_tg_poll_acq(tg) == 0);
  assert(lj_tg_profile_request_acq(tg) == 0);
  assert_native_idle(L, idle_vmstate);
  expect_only_args_root(L, pt, profile);

  lj_trace_test_root_entry_reset();
  lj_trace_test_reset_exit_stats();
  assert(call_triple(L, profile->name,
	profile->lifecycle.x, profile->lifecycle.limit,
	profile->lifecycle.step, 0, 0, 0) == profile->lifecycle.result);
  expect_single_exit(FINAL_EXIT);
  assert_native_idle(L, idle_vmstate);
  expect_only_args_root(L, pt, profile);
}

static void test_terminating_mutation(lua_State *L, GCproto *pt,
	int32_t idle_vmstate, const NumericArgsProfile *profile,
	PostAdmissionRequest request, lua_Number expected_live,
	MutationResult result_kind, lua_Number expected_result)
{
  test_terminating_mutation_at_exit(L, pt, idle_vmstate, profile, request,
	expected_live, result_kind, expected_result, PRECOND_EXIT);
}

static void test_nonterminating_mutation_stop(lua_State *L, GCproto *pt,
	int32_t idle_vmstate, const NumericArgsProfile *profile,
	PostAdmissionRequest request, lua_Number expected_live)
{
  global_State *g = G(L);
  TGState *tg = L2TG(L);
  uint64_t epoch = gc2_hs_epoch_acq(g);
  void *saved_cframe = L->cframe;
  PostAdmissionPublisher publisher;
  pthread_t worker;
  int status;

  clear_stopreq(tg);
  lj_trace_test_root_entry_reset();
  lj_trace_test_reset_exit_stats();
  lj_trace_test_root_entry_pause(LJ_TRACE_ROOT_ENTRY_PAUSE_POSTADMISSION);
  publisher = (PostAdmissionPublisher){
    .L = L, .g = g, .tg = tg, .epoch = epoch,
    .expected_value = expected_live, .request = request,
    .stop_after_mutation = 1
  };
  assert(pthread_create(&worker, NULL, publish_postadmission_request,
	&publisher) == 0);
  lua_getglobal(L, profile->name);
  assert(lua_isfunction(L, -1));
  lua_pushnumber(L, profile->mutation.x);
  lua_pushnumber(L, profile->mutation.limit);
  lua_pushnumber(L, profile->mutation.step);
  status = lua_pcall(L, 3, 1, 0);
  assert(pthread_join(worker, NULL) == 0);
  assert(status == LUA_ERRRUN);
  assert(lua_isstring(L, -1));
  assert(strstr(lua_tostring(L, -1),
	"thread interrupted: VM shutdown") != NULL);
  lua_pop(L, 1);
  assert_publisher_done(&publisher);
  assert(la_load32_acq(&publisher.mutated) == 1);
  expect_single_exit(XPOLL_EXIT);
  assert(gc2_hs_actions_acq(g) == LJ_GC2_HS_STOPREQ);
  assert(gc2_hs_epoch_acq(g) == epoch+1u);
  assert(lj_tg_hs_epoch_ack_acq(tg) == epoch+1u);
  assert(gc2_hs_leader_acq(g) == 0);
  assert(gc2_hs_pending_acq(g) == 0);
  assert(lj_tg_reqmask_acq(tg) == 0);
  assert(lj_tg_poll_acq(tg) == 0);
  assert(lj_tg_profile_request_acq(tg) == 0);
  assert((lj_tg_flags_acq(tg) & TGF_STOPREQ) != 0);
  assert((lj_tg_flags_acq(tg) & TGF_STOPREQ_FRESH) == 0);
  assert_native_idle(L, idle_vmstate);
  assert(L->cframe == saved_cframe);
  clear_stopreq(tg);
  assert((lj_tg_flags_acq(tg) &
	  (TGF_STOPREQ|TGF_STOPREQ_FRESH)) == 0);
  expect_only_args_root(L, pt, profile);

  lj_trace_test_root_entry_reset();
  lj_trace_test_reset_exit_stats();
  assert(call_triple(L, profile->name,
	profile->lifecycle.x, profile->lifecycle.limit,
	profile->lifecycle.step, 0, 0, 0) == profile->lifecycle.result);
  expect_single_exit(FINAL_EXIT);
  assert_native_idle(L, idle_vmstate);
  assert(L->cframe == saved_cframe);
  expect_only_args_root(L, pt, profile);
}

static void expect_no_trace(lua_State *L, const char *name)
{
  jit_State *J = L2J(L);
  GCproto *pt = global_proto(L, name);
  TraceNo traceno;
  if (proto_trace_acq(pt) != 0)
    fprintf(stderr, "dynamic args NUM unexpected trace for %s\n", name);
  assert(proto_trace_acq(pt) == 0);
  for (traceno = 1; (MSize)traceno < trace_sizetrace_acq(J); traceno++)
    assert(!trace_runnable_acq(traceref_safe(J, traceno), traceno));
}

static void test_positive_and_guard_exits(const NumericArgsProfile *profile)
{
  lua_State *L = luaL_newstate();
  TGState *tg;
  GCproto *pt;
  GCtrace *T;
  const BCIns *startpc;
  BCIns startins;
  int32_t idle_vmstate;
  int i;

  assert(L != NULL);
  luaL_openlibs(L);
  tg = L2TG(L);
  idle_vmstate = lj_tg_vmstate_load_acq(tg);
  if (profile->evolution == NUMERIC_ARGS_DIV_DESCENDING &&
      profile->comparison == NUMERIC_ARGS_INCLUSIVE) {
    run_lua(L,
      "jit.flush(); jit.on(); "
      "jit.opt.start('hotloop=1','hotexit=1','maxtrace=2'); "
      "function __arm64_pure_numeric_args_div_descending_inclusive"
	"(x,limit,divisor) while x >= limit do x = x / divisor end "
	"return x end");
  } else if (profile->evolution == NUMERIC_ARGS_DIV_DESCENDING) {
    run_lua(L,
      "jit.flush(); jit.on(); "
      "jit.opt.start('hotloop=1','hotexit=1','maxtrace=2'); "
      "function __arm64_pure_numeric_args_div_descending"
	"(x,limit,divisor) while x > limit do x = x / divisor end "
	"return x end");
  } else if (profile->evolution == NUMERIC_ARGS_DIV_ASCENDING &&
      profile->comparison == NUMERIC_ARGS_INCLUSIVE) {
    run_lua(L,
      "jit.flush(); jit.on(); "
      "jit.opt.start('hotloop=1','hotexit=1','maxtrace=2'); "
      "function __arm64_pure_numeric_args_div_inclusive(x,limit,divisor) "
	"while x <= limit do x = x / divisor end return x end");
  } else if (profile->evolution == NUMERIC_ARGS_DIV_ASCENDING) {
    run_lua(L,
      "jit.flush(); jit.on(); "
      "jit.opt.start('hotloop=1','hotexit=1','maxtrace=2'); "
      "function __arm64_pure_numeric_args_div(x,limit,divisor) "
	"while x < limit do x = x / divisor end return x end");
  } else if (profile->evolution == NUMERIC_ARGS_MUL_ASCENDING &&
      profile->comparison == NUMERIC_ARGS_INCLUSIVE) {
    run_lua(L,
      "jit.flush(); jit.on(); "
      "jit.opt.start('hotloop=1','hotexit=1','maxtrace=2'); "
      "function __arm64_pure_numeric_args_mul_inclusive(x,limit,factor) "
	"while x <= limit do x = x * factor end return x end");
  } else if (profile->evolution == NUMERIC_ARGS_MUL_ASCENDING) {
    run_lua(L,
      "jit.flush(); jit.on(); "
      "jit.opt.start('hotloop=1','hotexit=1','maxtrace=2'); "
      "function __arm64_pure_numeric_args_mul(x,limit,factor) "
	"while x < limit do x = x * factor end return x end");
  } else if (profile->evolution == NUMERIC_ARGS_ADD_DESCENDING &&
      profile->comparison == NUMERIC_ARGS_INCLUSIVE) {
    run_lua(L,
      "jit.flush(); jit.on(); "
      "jit.opt.start('hotloop=1','hotexit=1','maxtrace=2'); "
      "function __arm64_pure_numeric_args_add_descending_inclusive"
	"(x,limit,step) while x>=limit do x=x+step end return x end");
  } else if (profile->evolution == NUMERIC_ARGS_ADD_DESCENDING) {
    run_lua(L,
      "jit.flush(); jit.on(); "
      "jit.opt.start('hotloop=1','hotexit=1','maxtrace=2'); "
      "function __arm64_pure_numeric_args_add_descending(x,limit,step) "
	"while x>limit do x=x+step end return x end");
  } else if (profile->evolution == NUMERIC_ARGS_SUB_DESCENDING &&
      profile->comparison == NUMERIC_ARGS_INCLUSIVE) {
    run_lua(L,
      "jit.flush(); jit.on(); "
      "jit.opt.start('hotloop=1','hotexit=1','maxtrace=2'); "
      "function __arm64_pure_numeric_args_descending_inclusive"
	"(x,limit,step) while x>=limit do x=x-step end return x end");
  } else if (profile->evolution == NUMERIC_ARGS_SUB_DESCENDING) {
    run_lua(L,
      "jit.flush(); jit.on(); "
      "jit.opt.start('hotloop=1','hotexit=1','maxtrace=2'); "
      "function __arm64_pure_numeric_args_descending(x,limit,step) "
	"while x>limit do x=x-step end return x end");
  } else if (profile->comparison == NUMERIC_ARGS_INCLUSIVE) {
    run_lua(L,
      "jit.flush(); jit.on(); "
      "jit.opt.start('hotloop=1','hotexit=1','maxtrace=2'); "
      "function __arm64_pure_numeric_args_inclusive(x,limit,step) "
	"while x<=limit do x=x+step end return x end");
  } else {
    run_lua(L,
      "jit.flush(); jit.on(); "
      "jit.opt.start('hotloop=1','hotexit=1','maxtrace=2'); "
      "function __arm64_pure_numeric_args(x,limit,step) "
	"while x<limit do x=x+step end return x end");
  }

  assert(call_triple(L, profile->name,
	profile->record.x, profile->record.limit, profile->record.step,
	0, 0, 0) == profile->record.result);
  pt = global_proto(L, profile->name);
  expect_only_args_root(L, pt, profile);
  assert_native_idle(L, idle_vmstate);

  /* The same trace must consume different accumulator, limit, and step NUMs
  ** without recording or specializing a second root. */
  lj_trace_test_root_entry_reset();
  lj_trace_test_reset_exit_stats();
  assert(call_triple(L, profile->name,
	profile->reuse.x, profile->reuse.limit, profile->reuse.step,
	0, 0, 0) == profile->reuse.result);
  expect_single_exit(FINAL_EXIT);
  expect_only_args_root(L, pt, profile);

  if (profile->evolution == NUMERIC_ARGS_DIV_DESCENDING) {
    /* Retaining one recording-time value at a time distinguishes every live
    ** argument from a constant while preserving X/DIVISOR ordering. */
    lj_trace_test_root_entry_reset();
    lj_trace_test_reset_exit_stats();
    assert(call_triple(L, profile->name,
	profile->record.x, profile->reuse.limit, profile->reuse.step,
	0, 0, 0) == 0.3203125);
    expect_single_exit(FINAL_EXIT);
    expect_only_args_root(L, pt, profile);

    lj_trace_test_root_entry_reset();
    lj_trace_test_reset_exit_stats();
    assert(call_triple(L, profile->name,
	profile->reuse.x, profile->record.limit, profile->reuse.step,
	0, 0, 0) == 0.072265625);
    expect_single_exit(FINAL_EXIT);
    expect_only_args_root(L, pt, profile);

    lj_trace_test_root_entry_reset();
    lj_trace_test_reset_exit_stats();
    assert(call_triple(L, profile->name,
	profile->reuse.x, profile->reuse.limit, profile->record.step,
	0, 0, 0) == 0.578125);
    expect_single_exit(FINAL_EXIT);
    expect_only_args_root(L, pt, profile);
  } else if (profile->evolution == NUMERIC_ARGS_DIV_ASCENDING) {
    /* Retaining one recording-time value at a time distinguishes every live
    ** argument from a constant and preserves noncommutative DIV ordering. */
    lj_trace_test_root_entry_reset();
    lj_trace_test_reset_exit_stats();
    assert(call_triple(L, profile->name,
	profile->record.x, profile->reuse.limit, profile->reuse.step,
	0, 0, 0) == 8.0);
    expect_single_exit(FINAL_EXIT);
    expect_only_args_root(L, pt, profile);

    lj_trace_test_root_entry_reset();
    lj_trace_test_reset_exit_stats();
    assert(call_triple(L, profile->name,
	profile->reuse.x, profile->record.limit, profile->reuse.step,
	0, 0, 0) == 40.0);
    expect_single_exit(FINAL_EXIT);
    expect_only_args_root(L, pt, profile);

    lj_trace_test_root_entry_reset();
    lj_trace_test_reset_exit_stats();
    assert(call_triple(L, profile->name,
	profile->reuse.x, profile->reuse.limit, profile->record.step,
	0, 0, 0) == 5.0);
    expect_single_exit(FINAL_EXIT);
    expect_only_args_root(L, pt, profile);
  } else if (profile->evolution == NUMERIC_ARGS_MUL_ASCENDING) {
    /* Each call retains one recording-time value. All three distinguish the
    ** live argument from a constant specialized into the MUL root. */
    lj_trace_test_root_entry_reset();
    lj_trace_test_reset_exit_stats();
    assert(call_triple(L, profile->name,
	profile->record.x, profile->reuse.limit, profile->reuse.step,
	0, 0, 0) == 13.5);
    expect_single_exit(FINAL_EXIT);
    expect_only_args_root(L, pt, profile);

    lj_trace_test_root_entry_reset();
    lj_trace_test_reset_exit_stats();
    assert(call_triple(L, profile->name,
	profile->reuse.x, profile->record.limit, profile->reuse.step,
	0, 0, 0) == 50.625);
    expect_single_exit(FINAL_EXIT);
    expect_only_args_root(L, pt, profile);

    lj_trace_test_root_entry_reset();
    lj_trace_test_reset_exit_stats();
    assert(call_triple(L, profile->name,
	profile->reuse.x, profile->reuse.limit, profile->record.step,
	0, 0, 0) == 10.0);
    expect_single_exit(FINAL_EXIT);
    expect_only_args_root(L, pt, profile);
  } else if (profile->evolution == NUMERIC_ARGS_ADD_DESCENDING &&
      profile->comparison == NUMERIC_ARGS_INCLUSIVE) {
    /* Each call retains one recording-time value. All three distinguish the
    ** live argument from a constant specialized into the inclusive root. */
    lj_trace_test_root_entry_reset();
    lj_trace_test_reset_exit_stats();
    assert(call_triple(L, profile->name,
	profile->record.x, profile->reuse.limit, profile->reuse.step,
	0, 0, 0) == -0.75);
    expect_single_exit(FINAL_EXIT);
    expect_only_args_root(L, pt, profile);

    lj_trace_test_root_entry_reset();
    lj_trace_test_reset_exit_stats();
    assert(call_triple(L, profile->name,
	profile->reuse.x, profile->record.limit, profile->reuse.step,
	0, 0, 0) == 0.125);
    expect_single_exit(PRECOND_EXIT);
    expect_only_args_root(L, pt, profile);

    lj_trace_test_root_entry_reset();
    lj_trace_test_reset_exit_stats();
    assert(call_triple(L, profile->name,
	profile->reuse.x, profile->reuse.limit, profile->record.step,
	0, 0, 0) == -1.125);
    expect_single_exit(FINAL_EXIT);
    expect_only_args_root(L, pt, profile);
  } else if (profile->evolution == NUMERIC_ARGS_ADD_DESCENDING) {
    /* Each call retains one recording-time value. All three distinguish the
    ** live argument from a constant specialized into the strict root. */
    lj_trace_test_root_entry_reset();
    lj_trace_test_reset_exit_stats();
    assert(call_triple(L, profile->name,
	profile->record.x, profile->reuse.limit, profile->reuse.step,
	0, 0, 0) == -0.875);
    expect_single_exit(FINAL_EXIT);
    expect_only_args_root(L, pt, profile);

    lj_trace_test_root_entry_reset();
    lj_trace_test_reset_exit_stats();
    assert(call_triple(L, profile->name,
	profile->reuse.x, profile->record.limit, profile->reuse.step,
	0, 0, 0) == 0.125);
    expect_single_exit(PRECOND_EXIT);
    expect_only_args_root(L, pt, profile);

    lj_trace_test_root_entry_reset();
    lj_trace_test_reset_exit_stats();
    assert(call_triple(L, profile->name,
	profile->reuse.x, profile->reuse.limit, profile->record.step,
	0, 0, 0) == -1.0);
    expect_single_exit(FINAL_EXIT);
    expect_only_args_root(L, pt, profile);
  }

  if (profile->evolution == NUMERIC_ARGS_DIV_DESCENDING &&
      profile->comparison == NUMERIC_ARGS_INCLUSIVE) {
    /* Equality at the body guard takes the inclusive backedge once more. */
    lj_trace_test_root_entry_reset();
    lj_trace_test_reset_exit_stats();
    assert(call_triple(L, profile->name,
	2.0, 0.5, 2.0, 0, 0, 0) == 0.25);
    expect_single_exit(FINAL_EXIT);
    expect_only_args_root(L, pt, profile);

    /* Equality after the first quotient passes the inclusive precondition. */
    lj_trace_test_root_entry_reset();
    lj_trace_test_reset_exit_stats();
    assert(call_triple(L, profile->name,
	1.0, 0.5, 2.0, 0, 0, 0) == 0.25);
    expect_single_exit(FINAL_EXIT);
    expect_only_args_root(L, pt, profile);

    /* Initial equality enters JLOOP; the first quotient falls below limit. */
    lj_trace_test_root_entry_reset();
    lj_trace_test_reset_exit_stats();
    assert(call_triple(L, profile->name,
	0.5, 0.5, 2.0, 0, 0, 0) == 0.25);
    expect_single_exit(PRECOND_EXIT);
    expect_only_args_root(L, pt, profile);
  } else if (profile->evolution == NUMERIC_ARGS_DIV_DESCENDING) {
    /* Equality at the body guard exits through the strict final snapshot. */
    lj_trace_test_root_entry_reset();
    lj_trace_test_reset_exit_stats();
    assert(call_triple(L, profile->name,
	2.0, 0.5, 2.0, 0, 0, 0) == 0.5);
    expect_single_exit(FINAL_EXIT);
    expect_only_args_root(L, pt, profile);

    /* Equality after the first quotient fails the strict precondition. */
    lj_trace_test_root_entry_reset();
    lj_trace_test_reset_exit_stats();
    assert(call_triple(L, profile->name,
	1.0, 0.5, 2.0, 0, 0, 0) == 0.5);
    expect_single_exit(PRECOND_EXIT);
    expect_only_args_root(L, pt, profile);

    /* Initial equality fails interpreted > and never enters JLOOP. */
    lj_trace_test_root_entry_reset();
    lj_trace_test_reset_exit_stats();
    assert(call_triple(L, profile->name,
	0.5, 0.5, 2.0, 0, 0, 0) == 0.5);
    assert(lj_trace_test_root_entry_publishes() == 0);
    assert(lj_trace_test_exit_calls() == 0);
    expect_only_args_root(L, pt, profile);
  } else if (profile->evolution == NUMERIC_ARGS_DIV_ASCENDING &&
      profile->comparison == NUMERIC_ARGS_INCLUSIVE) {
    /* Equality at the body guard takes the inclusive backedge once more. */
    lj_trace_test_root_entry_reset();
    lj_trace_test_reset_exit_stats();
    assert(call_triple(L, profile->name,
	0.5, 2.0, 0.5, 0, 0, 0) == 4.0);
    expect_single_exit(FINAL_EXIT);
    expect_only_args_root(L, pt, profile);

    /* Equality after the first quotient passes the inclusive precondition. */
    lj_trace_test_root_entry_reset();
    lj_trace_test_reset_exit_stats();
    assert(call_triple(L, profile->name,
	0.5, 1.0, 0.5, 0, 0, 0) == 2.0);
    expect_single_exit(FINAL_EXIT);
    expect_only_args_root(L, pt, profile);

    /* Initial equality enters JLOOP; the first quotient exceeds the limit. */
    lj_trace_test_root_entry_reset();
    lj_trace_test_reset_exit_stats();
    assert(call_triple(L, profile->name,
	1.0, 1.0, 0.5, 0, 0, 0) == 2.0);
    expect_single_exit(PRECOND_EXIT);
    expect_only_args_root(L, pt, profile);
  } else if (profile->evolution == NUMERIC_ARGS_DIV_ASCENDING) {
    /* Equality at the body guard exits through the strict final snapshot. */
    lj_trace_test_root_entry_reset();
    lj_trace_test_reset_exit_stats();
    assert(call_triple(L, profile->name,
	0.5, 2.0, 0.5, 0, 0, 0) == 2.0);
    expect_single_exit(FINAL_EXIT);
    expect_only_args_root(L, pt, profile);

    /* Equality after the first quotient fails the strict precondition. */
    lj_trace_test_root_entry_reset();
    lj_trace_test_reset_exit_stats();
    assert(call_triple(L, profile->name,
	0.5, 1.0, 0.5, 0, 0, 0) == 1.0);
    expect_single_exit(PRECOND_EXIT);
    expect_only_args_root(L, pt, profile);

    /* Initial equality fails interpreted < and never enters JLOOP. */
    lj_trace_test_root_entry_reset();
    lj_trace_test_reset_exit_stats();
    assert(call_triple(L, profile->name,
	1.0, 1.0, 0.5, 0, 0, 0) == 1.0);
    assert(lj_trace_test_root_entry_publishes() == 0);
    assert(lj_trace_test_exit_calls() == 0);
    expect_only_args_root(L, pt, profile);
  } else if (profile->evolution == NUMERIC_ARGS_MUL_ASCENDING &&
      profile->comparison == NUMERIC_ARGS_INCLUSIVE) {
    /* Equality at the body guard takes the inclusive backedge once more. */
    lj_trace_test_root_entry_reset();
    lj_trace_test_reset_exit_stats();
    assert(call_triple(L, profile->name,
	0.5, 2.0, 2.0, 0, 0, 0) == 4.0);
    expect_single_exit(FINAL_EXIT);
    expect_only_args_root(L, pt, profile);

    /* Equality after the first product passes the inclusive precondition. */
    lj_trace_test_root_entry_reset();
    lj_trace_test_reset_exit_stats();
    assert(call_triple(L, profile->name,
	0.5, 1.0, 2.0, 0, 0, 0) == 2.0);
    expect_single_exit(FINAL_EXIT);
    expect_only_args_root(L, pt, profile);

    /* Initial equality enters JLOOP; the first product exceeds the limit. */
    lj_trace_test_root_entry_reset();
    lj_trace_test_reset_exit_stats();
    assert(call_triple(L, profile->name,
	1.0, 1.0, 2.0, 0, 0, 0) == 2.0);
    expect_single_exit(PRECOND_EXIT);
    expect_only_args_root(L, pt, profile);
  } else if (profile->evolution == NUMERIC_ARGS_MUL_ASCENDING) {
    /* Exact equality at the body guard exits through the final snapshot. */
    lj_trace_test_root_entry_reset();
    lj_trace_test_reset_exit_stats();
    assert(call_triple(L, profile->name,
	0.5, 2.0, 2.0, 0, 0, 0) == 2.0);
    expect_single_exit(FINAL_EXIT);
    expect_only_args_root(L, pt, profile);

    /* Equality after the first recurrence fails the strict precondition. */
    lj_trace_test_root_entry_reset();
    lj_trace_test_reset_exit_stats();
    assert(call_triple(L, profile->name,
	0.5, 1.0, 2.0, 0, 0, 0) == 1.0);
    expect_single_exit(PRECOND_EXIT);
    expect_only_args_root(L, pt, profile);

    /* Initial equality fails interpreted < and never enters JLOOP. */
    lj_trace_test_root_entry_reset();
    lj_trace_test_reset_exit_stats();
    assert(call_triple(L, profile->name,
	1.0, 1.0, 2.0, 0, 0, 0) == 1.0);
    assert(lj_trace_test_root_entry_publishes() == 0);
    assert(lj_trace_test_exit_calls() == 0);
    expect_only_args_root(L, pt, profile);
  } else if (numeric_args_is_descending(profile) &&
      profile->comparison == NUMERIC_ARGS_INCLUSIVE) {
    const lua_Number equality_body_step =
      profile->evolution == NUMERIC_ARGS_SUB_DESCENDING ? 0.375 : -0.375;
    const lua_Number equality_first_step =
      profile->evolution == NUMERIC_ARGS_SUB_DESCENDING ? 0.5 : -0.5;
    /* Equality at the body guard takes the inclusive backedge once more. */
    lj_trace_test_root_entry_reset();
    lj_trace_test_reset_exit_stats();
    assert(call_triple(L, profile->name,
	1.0, 0.25, equality_body_step, 0, 0, 0) == -0.125);
    expect_single_exit(FINAL_EXIT);
    expect_only_args_root(L, pt, profile);

    /* Equality after the first recurrence passes the inclusive precondition. */
    lj_trace_test_root_entry_reset();
    lj_trace_test_reset_exit_stats();
    assert(call_triple(L, profile->name,
	1.0, 0.5, equality_first_step, 0, 0, 0) == 0.0);
    expect_single_exit(FINAL_EXIT);
    expect_only_args_root(L, pt, profile);

    /* Initial equality enters JLOOP; the first recurrence falls below limit. */
    lj_trace_test_root_entry_reset();
    lj_trace_test_reset_exit_stats();
    assert(call_triple(L, profile->name,
	0.5, 0.5, equality_first_step, 0, 0, 0) == 0.0);
    expect_single_exit(PRECOND_EXIT);
    expect_only_args_root(L, pt, profile);
  } else if (numeric_args_is_descending(profile)) {
    const lua_Number equality_body_step =
      profile->evolution == NUMERIC_ARGS_SUB_DESCENDING ? 0.375 : -0.375;
    const lua_Number equality_first_step =
      profile->evolution == NUMERIC_ARGS_SUB_DESCENDING ? 0.5 : -0.5;
    /* Exact equality at the body guard exits through the final snapshot. */
    lj_trace_test_root_entry_reset();
    lj_trace_test_reset_exit_stats();
    assert(call_triple(L, profile->name,
	1.0, 0.25, equality_body_step, 0, 0, 0) == 0.25);
    expect_single_exit(FINAL_EXIT);
    expect_only_args_root(L, pt, profile);

    /* Equality after the first recurrence fails the strict precondition. */
    lj_trace_test_root_entry_reset();
    lj_trace_test_reset_exit_stats();
    assert(call_triple(L, profile->name,
	1.0, 0.5, equality_first_step, 0, 0, 0) == 0.5);
    expect_single_exit(PRECOND_EXIT);
    expect_only_args_root(L, pt, profile);

    /* Initial equality fails the interpreted > and never enters JLOOP. */
    lj_trace_test_root_entry_reset();
    lj_trace_test_reset_exit_stats();
    assert(call_triple(L, profile->name,
	0.5, 0.5, equality_first_step, 0, 0, 0) == 0.5);
    assert(lj_trace_test_root_entry_publishes() == 0);
    assert(lj_trace_test_exit_calls() == 0);
    expect_only_args_root(L, pt, profile);
  } else if (profile->comparison == NUMERIC_ARGS_INCLUSIVE) {
    /* Equality must pass both the preheader and loop-body guards. */
    lj_trace_test_root_entry_reset();
    lj_trace_test_reset_exit_stats();
    assert(call_triple(L, profile->name,
	0.625, 1.0, 0.375, 0, 0, 0) == 1.375);
    expect_single_exit(FINAL_EXIT);
    expect_only_args_root(L, pt, profile);

    /* The interpreter's initial equality enters JLOOP, then the native
    ** preheader returns the first value strictly above the limit. */
    lj_trace_test_root_entry_reset();
    lj_trace_test_reset_exit_stats();
    assert(call_triple(L, profile->name,
	1.0, 1.0, 0.375, 0, 0, 0) == 1.375);
    expect_single_exit(PRECOND_EXIT);
    expect_only_args_root(L, pt, profile);
  } else {
    /* Strict ascending equality fails both the native precondition and the
    ** interpreter's initial comparison at their respective boundaries. */
    lj_trace_test_root_entry_reset();
    lj_trace_test_reset_exit_stats();
    assert(call_triple(L, profile->name,
	0.625, 1.0, 0.375, 0, 0, 0) == 1.0);
    expect_single_exit(PRECOND_EXIT);
    expect_only_args_root(L, pt, profile);

    lj_trace_test_root_entry_reset();
    lj_trace_test_reset_exit_stats();
    assert(call_triple(L, profile->name,
	1.0, 1.0, 0.375, 0, 0, 0) == 1.0);
    assert(lj_trace_test_root_entry_publishes() == 0);
    assert(lj_trace_test_exit_calls() == 0);
    expect_only_args_root(L, pt, profile);
  }

  test_xpoll_lifecycle(L, pt, idle_vmstate, profile,
	NUMERIC_ARGS_STEP_NUM, &profile->lifecycle);
  if (profile->evolution == NUMERIC_ARGS_DIV_DESCENDING) {
    test_terminating_mutation(L, pt, idle_vmstate, profile,
      POSTADMISSION_QNAN_X, profile->mutation.x, MUTATION_QNAN, 0.0);
    test_nonterminating_mutation_stop(L, pt, idle_vmstate, profile,
      POSTADMISSION_PINF_X, profile->mutation.x);
    test_terminating_mutation(L, pt, idle_vmstate, profile,
      POSTADMISSION_NINF_X, profile->mutation.x, MUTATION_NINF, 0.0);
    test_terminating_mutation(L, pt, idle_vmstate, profile,
      POSTADMISSION_ZERO_X, profile->mutation.x, MUTATION_ZERO, 0.0);
    test_terminating_mutation(L, pt, idle_vmstate, profile,
      POSTADMISSION_NEGZERO_X, profile->mutation.x, MUTATION_NEGZERO, 0.0);
    test_terminating_mutation(L, pt, idle_vmstate, profile,
      POSTADMISSION_QNAN_LIMIT, profile->mutation.limit,
      MUTATION_FINITE, 10.25);
    test_terminating_mutation(L, pt, idle_vmstate, profile,
      POSTADMISSION_PINF_LIMIT, profile->mutation.limit,
      MUTATION_FINITE, 10.25);
    test_nonterminating_mutation_stop(L, pt, idle_vmstate, profile,
      POSTADMISSION_NINF_LIMIT, profile->mutation.limit);
    if (profile->comparison == NUMERIC_ARGS_INCLUSIVE) {
      test_nonterminating_mutation_stop(L, pt, idle_vmstate, profile,
	POSTADMISSION_ZERO_LIMIT, profile->mutation.limit);
      test_nonterminating_mutation_stop(L, pt, idle_vmstate, profile,
	POSTADMISSION_NEGZERO_LIMIT, profile->mutation.limit);
    } else {
      test_terminating_mutation_at_exit(L, pt, idle_vmstate, profile,
	POSTADMISSION_ZERO_LIMIT, profile->mutation.limit,
	MUTATION_ZERO, 0.0, FINAL_EXIT);
      test_terminating_mutation_at_exit(L, pt, idle_vmstate, profile,
	POSTADMISSION_NEGZERO_LIMIT, profile->mutation.limit,
	MUTATION_ZERO, 0.0, FINAL_EXIT);
    }
    test_terminating_mutation(L, pt, idle_vmstate, profile,
      POSTADMISSION_QNAN_STEP, profile->mutation.step,
      MUTATION_QNAN, 0.0);
    test_terminating_mutation(L, pt, idle_vmstate, profile,
      POSTADMISSION_PINF_STEP, profile->mutation.step,
      MUTATION_ZERO, 0.0);
    test_terminating_mutation(L, pt, idle_vmstate, profile,
      POSTADMISSION_NINF_STEP, profile->mutation.step,
      MUTATION_NEGZERO, 0.0);
    test_nonterminating_mutation_stop(L, pt, idle_vmstate, profile,
      POSTADMISSION_ZERO_STEP, profile->mutation.step);
    test_terminating_mutation(L, pt, idle_vmstate, profile,
      POSTADMISSION_NEGZERO_STEP, profile->mutation.step,
      MUTATION_NINF, 0.0);
    test_nonterminating_mutation_stop(L, pt, idle_vmstate, profile,
      POSTADMISSION_ONE_STEP, profile->mutation.step);
    test_terminating_mutation(L, pt, idle_vmstate, profile,
      POSTADMISSION_NEGONE_STEP, profile->mutation.step,
      MUTATION_FINITE, -20.5);
  } else if (profile->evolution == NUMERIC_ARGS_DIV_ASCENDING) {
    test_terminating_mutation(L, pt, idle_vmstate, profile,
      POSTADMISSION_QNAN_X, profile->mutation.x, MUTATION_QNAN, 0.0);
    test_terminating_mutation(L, pt, idle_vmstate, profile,
      POSTADMISSION_PINF_X, profile->mutation.x, MUTATION_PINF, 0.0);
    test_nonterminating_mutation_stop(L, pt, idle_vmstate, profile,
      POSTADMISSION_NINF_X, profile->mutation.x);
    test_nonterminating_mutation_stop(L, pt, idle_vmstate, profile,
      POSTADMISSION_ZERO_X, profile->mutation.x);
    test_nonterminating_mutation_stop(L, pt, idle_vmstate, profile,
      POSTADMISSION_NEGZERO_X, profile->mutation.x);
    test_terminating_mutation(L, pt, idle_vmstate, profile,
      POSTADMISSION_QNAN_LIMIT, profile->mutation.limit,
      MUTATION_FINITE, 1.0);
    if (profile->comparison == NUMERIC_ARGS_INCLUSIVE) {
      test_nonterminating_mutation_stop(L, pt, idle_vmstate, profile,
	POSTADMISSION_PINF_LIMIT, profile->mutation.limit);
    } else {
      test_terminating_mutation_at_exit(L, pt, idle_vmstate, profile,
	POSTADMISSION_PINF_LIMIT, profile->mutation.limit,
	MUTATION_PINF, 0.0, FINAL_EXIT);
    }
    test_terminating_mutation(L, pt, idle_vmstate, profile,
      POSTADMISSION_NINF_LIMIT, profile->mutation.limit,
      MUTATION_FINITE, 1.0);
    test_terminating_mutation(L, pt, idle_vmstate, profile,
      POSTADMISSION_ZERO_LIMIT, profile->mutation.limit,
      MUTATION_FINITE, 1.0);
    test_terminating_mutation(L, pt, idle_vmstate, profile,
      POSTADMISSION_NEGZERO_LIMIT, profile->mutation.limit,
      MUTATION_FINITE, 1.0);
    test_terminating_mutation(L, pt, idle_vmstate, profile,
      POSTADMISSION_QNAN_STEP, profile->mutation.step,
      MUTATION_QNAN, 0.0);
    test_nonterminating_mutation_stop(L, pt, idle_vmstate, profile,
      POSTADMISSION_PINF_STEP, profile->mutation.step);
    test_nonterminating_mutation_stop(L, pt, idle_vmstate, profile,
      POSTADMISSION_NINF_STEP, profile->mutation.step);
    test_terminating_mutation(L, pt, idle_vmstate, profile,
      POSTADMISSION_ZERO_STEP, profile->mutation.step,
      MUTATION_PINF, 0.0);
    test_terminating_mutation_at_exit(L, pt, idle_vmstate, profile,
      POSTADMISSION_NEGZERO_STEP, profile->mutation.step,
      MUTATION_PINF, 0.0, FINAL_EXIT);
    test_nonterminating_mutation_stop(L, pt, idle_vmstate, profile,
      POSTADMISSION_ONE_STEP, profile->mutation.step);
    test_nonterminating_mutation_stop(L, pt, idle_vmstate, profile,
      POSTADMISSION_NEGONE_STEP, profile->mutation.step);
  } else if (profile->evolution == NUMERIC_ARGS_MUL_ASCENDING) {
    test_terminating_mutation(L, pt, idle_vmstate, profile,
      POSTADMISSION_QNAN_X, profile->mutation.x, MUTATION_QNAN, 0.0);
    test_terminating_mutation(L, pt, idle_vmstate, profile,
      POSTADMISSION_PINF_X, profile->mutation.x, MUTATION_PINF, 0.0);
    test_nonterminating_mutation_stop(L, pt, idle_vmstate, profile,
      POSTADMISSION_NINF_X, profile->mutation.x);
    if (profile->comparison == NUMERIC_ARGS_INCLUSIVE)
      test_nonterminating_mutation_stop(L, pt, idle_vmstate, profile,
	POSTADMISSION_ZERO_X, profile->mutation.x);
    test_terminating_mutation(L, pt, idle_vmstate, profile,
      POSTADMISSION_QNAN_LIMIT, profile->mutation.limit,
      MUTATION_FINITE, 1.0);
    if (profile->comparison == NUMERIC_ARGS_INCLUSIVE) {
      test_nonterminating_mutation_stop(L, pt, idle_vmstate, profile,
	POSTADMISSION_PINF_LIMIT, profile->mutation.limit);
    } else {
      test_terminating_mutation_at_exit(L, pt, idle_vmstate, profile,
	POSTADMISSION_PINF_LIMIT, profile->mutation.limit,
	MUTATION_PINF, 0.0, FINAL_EXIT);
    }
    test_terminating_mutation(L, pt, idle_vmstate, profile,
      POSTADMISSION_NINF_LIMIT, profile->mutation.limit,
      MUTATION_FINITE, 1.0);
    test_terminating_mutation(L, pt, idle_vmstate, profile,
      POSTADMISSION_QNAN_STEP, profile->mutation.step,
      MUTATION_QNAN, 0.0);
    test_terminating_mutation(L, pt, idle_vmstate, profile,
      POSTADMISSION_PINF_STEP, profile->mutation.step,
      MUTATION_PINF, 0.0);
    test_terminating_mutation_at_exit(L, pt, idle_vmstate, profile,
      POSTADMISSION_NINF_STEP, profile->mutation.step,
      MUTATION_PINF, 0.0, FINAL_EXIT);
    test_nonterminating_mutation_stop(L, pt, idle_vmstate, profile,
      POSTADMISSION_ZERO_STEP, profile->mutation.step);
    test_nonterminating_mutation_stop(L, pt, idle_vmstate, profile,
      POSTADMISSION_ONE_STEP, profile->mutation.step);
    if (profile->comparison == NUMERIC_ARGS_INCLUSIVE)
      test_nonterminating_mutation_stop(L, pt, idle_vmstate, profile,
	POSTADMISSION_NEGONE_STEP, profile->mutation.step);
  } else if (numeric_args_is_descending(profile)) {
    test_terminating_mutation(L, pt, idle_vmstate, profile,
      POSTADMISSION_QNAN_X, profile->mutation.x, MUTATION_QNAN, 0.0);
    test_nonterminating_mutation_stop(L, pt, idle_vmstate, profile,
      POSTADMISSION_PINF_X, profile->mutation.x);
    test_terminating_mutation(L, pt, idle_vmstate, profile,
      POSTADMISSION_NINF_X, profile->mutation.x, MUTATION_NINF, 0.0);
    test_terminating_mutation(L, pt, idle_vmstate, profile,
      POSTADMISSION_QNAN_LIMIT, profile->mutation.limit,
      MUTATION_FINITE, 19.75);
    test_terminating_mutation(L, pt, idle_vmstate, profile,
      POSTADMISSION_PINF_LIMIT, profile->mutation.limit,
      MUTATION_FINITE, 19.75);
    test_nonterminating_mutation_stop(L, pt, idle_vmstate, profile,
      POSTADMISSION_NINF_LIMIT, profile->mutation.limit);
    test_terminating_mutation(L, pt, idle_vmstate, profile,
      POSTADMISSION_QNAN_STEP, profile->mutation.step,
      MUTATION_QNAN, 0.0);
    if (profile->evolution == NUMERIC_ARGS_ADD_DESCENDING) {
      test_nonterminating_mutation_stop(L, pt, idle_vmstate, profile,
	POSTADMISSION_PINF_STEP, profile->mutation.step);
      test_terminating_mutation(L, pt, idle_vmstate, profile,
	POSTADMISSION_NINF_STEP, profile->mutation.step,
	MUTATION_NINF, 0.0);
    } else {
      test_terminating_mutation(L, pt, idle_vmstate, profile,
	POSTADMISSION_PINF_STEP, profile->mutation.step,
	MUTATION_NINF, 0.0);
      test_nonterminating_mutation_stop(L, pt, idle_vmstate, profile,
	POSTADMISSION_NINF_STEP, profile->mutation.step);
    }
  } else {
    test_terminating_mutation(L, pt, idle_vmstate, profile,
      POSTADMISSION_QNAN_X, profile->mutation.x, MUTATION_QNAN, 0.0);
    test_terminating_mutation(L, pt, idle_vmstate, profile,
      POSTADMISSION_PINF_X, profile->mutation.x, MUTATION_PINF, 0.0);
  }
  if (profile->evolution == NUMERIC_ARGS_ADD_ASCENDING &&
      profile->comparison == NUMERIC_ARGS_INCLUSIVE) {
    test_nonterminating_mutation_stop(L, pt, idle_vmstate, profile,
      POSTADMISSION_NINF_X, profile->mutation.x);
    test_terminating_mutation(L, pt, idle_vmstate, profile,
      POSTADMISSION_QNAN_LIMIT, profile->mutation.limit,
      MUTATION_FINITE, 0.75);
    test_nonterminating_mutation_stop(L, pt, idle_vmstate, profile,
      POSTADMISSION_PINF_LIMIT, profile->mutation.limit);
    test_terminating_mutation(L, pt, idle_vmstate, profile,
      POSTADMISSION_NINF_LIMIT, profile->mutation.limit,
      MUTATION_FINITE, 0.75);
    test_terminating_mutation(L, pt, idle_vmstate, profile,
      POSTADMISSION_QNAN_STEP, profile->mutation.step,
      MUTATION_QNAN, 0.0);
    test_terminating_mutation(L, pt, idle_vmstate, profile,
      POSTADMISSION_PINF_STEP, profile->mutation.step,
      MUTATION_PINF, 0.0);
    test_nonterminating_mutation_stop(L, pt, idle_vmstate, profile,
      POSTADMISSION_NINF_STEP, profile->mutation.step);
  }

  /* Repeated hot exits try to start side traces. The side recorder must stay
  ** closed for every speculative guard family. Both live x and live step
  ** share the first snapshot/type exit. */
  lj_trace_test_root_entry_reset();
  lj_trace_test_reset_exit_stats();
  for (i = 0; i < 4; i++)
    assert(call_triple(L, profile->name,
	  profile->integer_x.x, profile->integer_x.limit,
	  profile->integer_x.step, 1, 0, 0) == profile->integer_x.result);
  /* An integer x exits before the first recurrence. The interpreter updates
  ** the accumulator to NUM, so the same call can re-enter and finish native. */
  expect_native_exit(X_OR_STEP_TYPE_EXIT, FINAL_EXIT);
  expect_only_args_root(L, pt, profile);

  lj_trace_test_root_entry_reset();
  lj_trace_test_reset_exit_stats();
  for (i = 0; i < 4; i++)
    assert(call_triple(L, profile->name,
	  profile->integer_step.x, profile->integer_step.limit,
	  profile->integer_step.step, 0, 0, 1) ==
	profile->integer_step.result);
  expect_native_exit(X_OR_STEP_TYPE_EXIT, X_OR_STEP_TYPE_EXIT);
  expect_only_args_root(L, pt, profile);

  lj_trace_test_root_entry_reset();
  lj_trace_test_reset_exit_stats();
  for (i = 0; i < 4; i++)
    assert(call_triple(L, profile->name,
	  profile->integer_limit.x, profile->integer_limit.limit,
	  profile->integer_limit.step, 0, 1, 0) ==
	profile->integer_limit.result);
  expect_native_exit(LIMIT_TYPE_EXIT, LIMIT_TYPE_EXIT);
  expect_only_args_root(L, pt, profile);

  lj_trace_test_root_entry_reset();
  lj_trace_test_reset_exit_stats();
  for (i = 0; i < 4; i++)
    assert(call_triple(L, profile->name,
	  profile->precondition.x, profile->precondition.limit,
	  profile->precondition.step, 0, 0, 0) ==
	profile->precondition.result);
  expect_native_exit(PRECOND_EXIT, PRECOND_EXIT);
  expect_only_args_root(L, pt, profile);
  T = traceref_safe(L2J(L), 1);
  assert(trace_nchild_acq(T) == 0 && trace_nextside_acq(T) == 0);
  assert_native_idle(L, idle_vmstate);

  startpc = trace_startpc_acq(T);
  startins = trace_startins_acq(T);
  run_lua(L, "jit.flush()");
  assert((BCIns)la_load32_acq((const uint32_t *)startpc) == startins);
  assert(bc_op(startins) == BC_LOOP);
  assert(proto_trace_acq(pt) == 0);
  T = traceref_safe(L2J(L), 1);
  assert(T == NULL || !trace_runnable_acq(T, 1));
  assert_native_idle(L, idle_vmstate);
  lua_close(L);
}

static void load_int_step_function(lua_State *L,
	const NumericArgsProfile *profile)
{
  char chunk[512];
  const char *comparison;
  char recurrence;
  int ascending = profile->evolution == NUMERIC_ARGS_ADD_ASCENDING ||
	profile->evolution == NUMERIC_ARGS_MUL_ASCENDING ||
	profile->evolution == NUMERIC_ARGS_DIV_ASCENDING;
  int n;

  if (ascending)
    comparison = profile->comparison == NUMERIC_ARGS_INCLUSIVE ? "<=" : "<";
  else
    comparison = profile->comparison == NUMERIC_ARGS_INCLUSIVE ? ">=" : ">";
  if (profile->evolution == NUMERIC_ARGS_ADD_ASCENDING ||
      profile->evolution == NUMERIC_ARGS_ADD_DESCENDING)
    recurrence = '+';
  else if (profile->evolution == NUMERIC_ARGS_SUB_DESCENDING)
    recurrence = '-';
  else if (profile->evolution == NUMERIC_ARGS_MUL_ASCENDING)
    recurrence = '*';
  else
    recurrence = '/';
  n = snprintf(chunk, sizeof(chunk),
    "jit.flush(); jit.on(); "
    "jit.opt.start('hotloop=1','hotexit=1','maxtrace=2'); "
    "function %s(x,limit,step) while x%s limit do x=x%cstep end "
    "return x end", profile->name, comparison, recurrence);
  assert(n > 0 && (size_t)n < sizeof(chunk));
  run_lua(L, chunk);
}

static void numeric_args_int_equality(const NumericArgsProfile *profile,
	NumericArgsCall *body, NumericArgsCall *first,
	NumericArgsCall *initial)
{
  lua_Number body_after, first_after;
  switch (profile->evolution) {
  case NUMERIC_ARGS_ADD_ASCENDING:
    *body = (NumericArgsCall){ 0.5, 4.5, 2.0, 4.5 };
    *first = (NumericArgsCall){ 0.5, 2.5, 2.0, 2.5 };
    body_after = 6.5;
    first_after = 4.5;
    break;
  case NUMERIC_ARGS_MUL_ASCENDING:
    *body = (NumericArgsCall){ 0.5, 2.0, 2.0, 2.0 };
    *first = (NumericArgsCall){ 0.5, 1.0, 2.0, 1.0 };
    body_after = 4.0;
    first_after = 2.0;
    break;
  case NUMERIC_ARGS_DIV_ASCENDING:
    *body = (NumericArgsCall){ -2.0, -0.5, 2.0, -0.5 };
    *first = (NumericArgsCall){ -1.0, -0.5, 2.0, -0.5 };
    body_after = first_after = -0.25;
    break;
  case NUMERIC_ARGS_DIV_DESCENDING:
    *body = (NumericArgsCall){ 2.0, 0.5, 2.0, 0.5 };
    *first = (NumericArgsCall){ 1.0, 0.5, 2.0, 0.5 };
    body_after = first_after = 0.25;
    break;
  case NUMERIC_ARGS_ADD_DESCENDING:
    *body = (NumericArgsCall){ 4.5, 0.5, -2.0, 0.5 };
    *first = (NumericArgsCall){ 2.5, 0.5, -2.0, 0.5 };
    body_after = first_after = -1.5;
    break;
  case NUMERIC_ARGS_SUB_DESCENDING:
    *body = (NumericArgsCall){ 4.5, 0.5, 2.0, 0.5 };
    *first = (NumericArgsCall){ 2.5, 0.5, 2.0, 0.5 };
    body_after = first_after = -1.5;
    break;
  default:
    assert(!"bad INT-step equality profile");
    return;
  }
  *initial = (NumericArgsCall){
    first->limit, first->limit, first->step, first->limit
  };
  if (profile->comparison == NUMERIC_ARGS_INCLUSIVE) {
    body->result = body_after;
    first->result = initial->result = first_after;
  }
}

static void expect_int_step_call(lua_State *L, GCproto *pt,
	const NumericArgsProfile *profile, const NumericArgsCall *call,
	ExitNo exitno)
{
  lua_Number result;
  lj_trace_test_root_entry_reset();
  lj_trace_test_reset_exit_stats();
  result = call_triple(L, profile->name, call->x, call->limit, call->step,
	0, 0, 1);
  if (result != call->result)
    fprintf(stderr, "INT-step %s call %.17g %.17g %.17g got %.17g, "
	"wanted %.17g\n", profile->name, call->x, call->limit,
	call->step, result, call->result);
  assert(result == call->result);
  expect_single_exit(exitno);
  expect_only_args_root_kind(L, pt, profile, NUMERIC_ARGS_STEP_INT);
}

static void test_int_step_nonterminating_stop(lua_State *L, GCproto *pt,
	int32_t idle_vmstate, const NumericArgsProfile *profile,
	const NumericArgsIntModeData *data, int32_t step)
{
  global_State *g = G(L);
  TGState *tg = L2TG(L);
  uint64_t epoch = gc2_hs_epoch_acq(g);
  void *saved_cframe = L->cframe;
  PostAdmissionPublisher publisher;
  pthread_t worker;
  int status;

  clear_stopreq(tg);
  lj_trace_test_root_entry_reset();
  lj_trace_test_reset_exit_stats();
  lj_trace_test_root_entry_pause(LJ_TRACE_ROOT_ENTRY_PAUSE_POSTADMISSION);
  publisher = (PostAdmissionPublisher){
    .L = L, .g = g, .tg = tg, .epoch = epoch,
    .expected_value = data->record.x,
    .request = POSTADMISSION_STOPREQ
  };
  assert(pthread_create(&worker, NULL, publish_postadmission_request,
	&publisher) == 0);
  lua_getglobal(L, profile->name);
  assert(lua_isfunction(L, -1));
  lua_pushnumber(L, data->record.x);
  lua_pushnumber(L, data->record.limit);
  lua_pushinteger(L, (lua_Integer)step);
  status = lua_pcall(L, 3, 1, 0);
  assert(pthread_join(worker, NULL) == 0);
  assert(status == LUA_ERRRUN);
  assert(lua_isstring(L, -1));
  assert(strstr(lua_tostring(L, -1),
	"thread interrupted: VM shutdown") != NULL);
  lua_pop(L, 1);
  assert_publisher_done(&publisher);
  expect_single_exit(XPOLL_EXIT);
  assert(gc2_hs_actions_acq(g) == LJ_GC2_HS_STOPREQ);
  assert(gc2_hs_epoch_acq(g) == epoch+1u);
  assert(lj_tg_hs_epoch_ack_acq(tg) == epoch+1u);
  assert(gc2_hs_leader_acq(g) == 0);
  assert(gc2_hs_pending_acq(g) == 0);
  assert(lj_tg_reqmask_acq(tg) == 0);
  assert(lj_tg_poll_acq(tg) == 0);
  assert(lj_tg_profile_request_acq(tg) == 0);
  assert((lj_tg_flags_acq(tg) & TGF_STOPREQ) != 0);
  assert((lj_tg_flags_acq(tg) & TGF_STOPREQ_FRESH) == 0);
  assert_native_idle(L, idle_vmstate);
  assert(L->cframe == saved_cframe);
  clear_stopreq(tg);
  assert((lj_tg_flags_acq(tg) &
	  (TGF_STOPREQ|TGF_STOPREQ_FRESH)) == 0);
  expect_only_args_root_kind(L, pt, profile, NUMERIC_ARGS_STEP_INT);
}

static void test_int_step_terminating(lua_State *L, GCproto *pt,
	const NumericArgsProfile *profile,
	const NumericArgsIntModeData *data, int32_t step,
	lua_Number result, ExitNo exitno)
{
  NumericArgsCall call = data->record;
  call.step = (lua_Number)step;
  call.result = result;
  expect_int_step_call(L, pt, profile, &call, exitno);
}

static void test_int_step_extremes(lua_State *L, GCproto *pt,
	int32_t idle_vmstate, const NumericArgsProfile *profile,
	const NumericArgsIntModeData *data)
{
  switch (profile->evolution) {
  case NUMERIC_ARGS_ADD_ASCENDING:
    test_int_step_nonterminating_stop(L, pt, idle_vmstate, profile,
	data, INT32_MIN);
    test_int_step_terminating(L, pt, profile, data, INT32_MAX,
	data->record.x+(lua_Number)INT32_MAX, PRECOND_EXIT);
    test_int_step_nonterminating_stop(L, pt, idle_vmstate, profile, data, 0);
    test_int_step_terminating(L, pt, profile, data, 1, 20.5, FINAL_EXIT);
    test_int_step_nonterminating_stop(L, pt, idle_vmstate, profile, data, -1);
    break;
  case NUMERIC_ARGS_MUL_ASCENDING:
    test_int_step_terminating(L, pt, profile, data, INT32_MIN,
	data->record.x*(lua_Number)INT32_MIN*(lua_Number)INT32_MIN,
	FINAL_EXIT);
    test_int_step_terminating(L, pt, profile, data, INT32_MAX,
	data->record.x*(lua_Number)INT32_MAX, PRECOND_EXIT);
    test_int_step_nonterminating_stop(L, pt, idle_vmstate, profile, data, 0);
    test_int_step_nonterminating_stop(L, pt, idle_vmstate, profile, data, 1);
    test_int_step_nonterminating_stop(L, pt, idle_vmstate, profile, data, -1);
    break;
  case NUMERIC_ARGS_DIV_ASCENDING:
    /* SCVTF maps integer zero to +0.0. Negative X reaches -infinity and
    ** remains in the ascending loop for both comparisons. */
    test_int_step_terminating(L, pt, profile, data, INT32_MIN,
	data->record.x/(lua_Number)INT32_MIN, PRECOND_EXIT);
    test_int_step_terminating(L, pt, profile, data, INT32_MAX,
	data->record.x/(lua_Number)INT32_MAX, PRECOND_EXIT);
    test_int_step_nonterminating_stop(L, pt, idle_vmstate, profile, data, 0);
    test_int_step_nonterminating_stop(L, pt, idle_vmstate, profile, data, 1);
    test_int_step_terminating(L, pt, profile, data, -1,
	-data->record.x, PRECOND_EXIT);
    break;
  case NUMERIC_ARGS_ADD_DESCENDING:
    test_int_step_terminating(L, pt, profile, data, INT32_MIN,
	data->record.x+(lua_Number)INT32_MIN, PRECOND_EXIT);
    test_int_step_nonterminating_stop(L, pt, idle_vmstate, profile,
	data, INT32_MAX);
    test_int_step_nonterminating_stop(L, pt, idle_vmstate, profile, data, 0);
    test_int_step_nonterminating_stop(L, pt, idle_vmstate, profile, data, 1);
    test_int_step_terminating(L, pt, profile, data, -1, -0.5, FINAL_EXIT);
    break;
  case NUMERIC_ARGS_SUB_DESCENDING:
    test_int_step_nonterminating_stop(L, pt, idle_vmstate, profile,
	data, INT32_MIN);
    test_int_step_terminating(L, pt, profile, data, INT32_MAX,
	data->record.x-(lua_Number)INT32_MAX, PRECOND_EXIT);
    test_int_step_nonterminating_stop(L, pt, idle_vmstate, profile, data, 0);
    test_int_step_terminating(L, pt, profile, data, 1, -0.5, FINAL_EXIT);
    test_int_step_nonterminating_stop(L, pt, idle_vmstate, profile, data, -1);
    break;
  case NUMERIC_ARGS_DIV_DESCENDING:
    /* The same +0.0 divisor maps positive X to the +infinity fixed point. */
    test_int_step_terminating(L, pt, profile, data, INT32_MIN,
	data->record.x/(lua_Number)INT32_MIN, PRECOND_EXIT);
    test_int_step_terminating(L, pt, profile, data, INT32_MAX,
	data->record.x/(lua_Number)INT32_MAX, PRECOND_EXIT);
    test_int_step_nonterminating_stop(L, pt, idle_vmstate, profile, data, 0);
    test_int_step_nonterminating_stop(L, pt, idle_vmstate, profile, data, 1);
    test_int_step_terminating(L, pt, profile, data, -1,
	-data->record.x, PRECOND_EXIT);
    break;
  default:
    assert(!"bad INT-step extreme profile");
  }
}

static NumericArgsCall numeric_args_int_x_negative(
	const NumericArgsProfile *profile)
{
  switch (profile->evolution) {
  case NUMERIC_ARGS_ADD_ASCENDING:
    return (NumericArgsCall){ 0.0, 1.0, 2.0, 2.0 };
  case NUMERIC_ARGS_MUL_ASCENDING:
    return (NumericArgsCall){ 1.0, 1.5, 2.0, 2.0 };
  case NUMERIC_ARGS_DIV_ASCENDING:
    return (NumericArgsCall){ -1.0, -0.75, 2.0, -0.5 };
  case NUMERIC_ARGS_DIV_DESCENDING:
    return (NumericArgsCall){ 1.0, 0.75, 2.0, 0.5 };
  case NUMERIC_ARGS_ADD_DESCENDING:
    return (NumericArgsCall){ 1.0, 0.5, -2.0, -1.0 };
  case NUMERIC_ARGS_SUB_DESCENDING:
    return (NumericArgsCall){ 1.0, 0.5, 2.0, -1.0 };
  default:
    assert(!"bad INT-step adjacent profile");
    return (NumericArgsCall){ 0.0, 0.0, 0.0, 0.0 };
  }
}

static void test_int_step_positive_and_guard_exits(
	const NumericArgsProfile *profile)
{
  const NumericArgsIntModeData *data = numeric_args_int_data(profile);
  NumericArgsCall call, body, first, initial;
  lua_State *L = luaL_newstate();
  TGState *tg;
  GCproto *pt;
  GCtrace *T;
  const BCIns *startpc;
  BCIns startins;
  int32_t idle_vmstate;
  int i;

  assert(L != NULL);
  luaL_openlibs(L);
  tg = L2TG(L);
  idle_vmstate = lj_tg_vmstate_load_acq(tg);
  load_int_step_function(L, profile);

  /* X and LIMIT are genuine NUMs while the invariant arrives as an INT. */
  assert(call_triple(L, profile->name, data->record.x, data->record.limit,
	data->record.step, 0, 0, 1) == data->record.result);
  pt = global_proto(L, profile->name);
  expect_only_args_root_kind(L, pt, profile, NUMERIC_ARGS_STEP_INT);
  assert_native_idle(L, idle_vmstate);

  /* One root must consume new values for all three arguments. Keeping one
  ** recording-time value in each call rules out constant specialization. */
  expect_int_step_call(L, pt, profile, &data->reuse, FINAL_EXIT);
  call = (NumericArgsCall){
    data->record.x, data->reuse.limit, data->reuse.step,
    data->substitute_x_result
  };
  expect_int_step_call(L, pt, profile, &call, FINAL_EXIT);
  call = (NumericArgsCall){
    data->reuse.x, data->record.limit, data->reuse.step,
    data->substitute_limit_result
  };
  expect_int_step_call(L, pt, profile, &call,
	profile->evolution == NUMERIC_ARGS_ADD_DESCENDING ||
	profile->evolution == NUMERIC_ARGS_SUB_DESCENDING ?
	PRECOND_EXIT : FINAL_EXIT);
  call = (NumericArgsCall){
    data->reuse.x, data->reuse.limit, data->record.step,
    data->substitute_step_result
  };
  expect_int_step_call(L, pt, profile, &call, FINAL_EXIT);

  /* Equality is observed independently at the body guard, the first native
  ** precondition, and the interpreted condition before JLOOP. */
  numeric_args_int_equality(profile, &body, &first, &initial);
  expect_int_step_call(L, pt, profile, &body, FINAL_EXIT);
  expect_int_step_call(L, pt, profile, &first,
	profile->comparison == NUMERIC_ARGS_INCLUSIVE ?
	FINAL_EXIT : PRECOND_EXIT);
  lj_trace_test_root_entry_reset();
  lj_trace_test_reset_exit_stats();
  assert(call_triple(L, profile->name, initial.x, initial.limit,
	initial.step, 0, 0, 1) == initial.result);
  if (profile->comparison == NUMERIC_ARGS_INCLUSIVE) {
    expect_single_exit(PRECOND_EXIT);
  } else {
    assert(lj_trace_test_root_entry_publishes() == 0);
    assert(lj_trace_test_root_entry_cleanups() == 0);
    assert(lj_trace_test_exit_calls() == 0);
  }
  expect_only_args_root_kind(L, pt, profile, NUMERIC_ARGS_STEP_INT);

  test_xpoll_lifecycle(L, pt, idle_vmstate, profile,
	NUMERIC_ARGS_STEP_INT, &data->record);
  test_int_step_extremes(L, pt, idle_vmstate, profile, data);

  /* The admitted root guards NUM X, INT STEP, then NUM LIMIT. DIV changes an
  ** exiting INT accumulator to NUM in the interpreter and can re-enter; the
  ** other recurrences retain their INT accumulator and keep taking exit 0. */
  lj_trace_test_root_entry_reset();
  lj_trace_test_reset_exit_stats();
  for (i = 0; i < 4; i++)
    assert(call_triple(L, profile->name,
	  data->integer_x.x, data->integer_x.limit,
	  data->integer_x.step, 1, 0, 1) == data->integer_x.result);
  if (profile->evolution == NUMERIC_ARGS_DIV_ASCENDING ||
      profile->evolution == NUMERIC_ARGS_DIV_DESCENDING)
    expect_native_exit(X_OR_STEP_TYPE_EXIT, FINAL_EXIT);
  else
    expect_native_exit(X_OR_STEP_TYPE_EXIT, X_OR_STEP_TYPE_EXIT);
  expect_only_args_root_kind(L, pt, profile, NUMERIC_ARGS_STEP_INT);

  lj_trace_test_root_entry_reset();
  lj_trace_test_reset_exit_stats();
  for (i = 0; i < 4; i++)
    assert(call_triple(L, profile->name,
	  data->record.x, data->record.limit, data->record.step,
	  0, 0, 0) == data->record.result);
  expect_native_exit(X_OR_STEP_TYPE_EXIT, X_OR_STEP_TYPE_EXIT);
  expect_only_args_root_kind(L, pt, profile, NUMERIC_ARGS_STEP_INT);

  lj_trace_test_root_entry_reset();
  lj_trace_test_reset_exit_stats();
  for (i = 0; i < 4; i++)
    assert(call_triple(L, profile->name,
	  data->integer_limit.x, data->integer_limit.limit,
	  data->integer_limit.step, 0, 1, 1) ==
	data->integer_limit.result);
  expect_native_exit(LIMIT_TYPE_EXIT, LIMIT_TYPE_EXIT);
  expect_only_args_root_kind(L, pt, profile, NUMERIC_ARGS_STEP_INT);

  lj_trace_test_root_entry_reset();
  lj_trace_test_reset_exit_stats();
  for (i = 0; i < 4; i++)
    assert(call_triple(L, profile->name,
	  data->precondition.x, data->precondition.limit,
	  data->precondition.step, 0, 0, 1) == data->precondition.result);
  expect_native_exit(PRECOND_EXIT, PRECOND_EXIT);
  expect_only_args_root_kind(L, pt, profile, NUMERIC_ARGS_STEP_INT);
  T = traceref_safe(L2J(L), 1);
  assert(trace_nchild_acq(T) == 0 && trace_nextside_acq(T) == 0);
  assert_native_idle(L, idle_vmstate);

  startpc = trace_startpc_acq(T);
  startins = trace_startins_acq(T);
  run_lua(L, "jit.flush()");
  assert((BCIns)la_load32_acq((const uint32_t *)startpc) == startins);
  assert(bc_op(startins) == BC_LOOP);
  assert(proto_trace_acq(pt) == 0);
  T = traceref_safe(L2J(L), 1);
  assert(T == NULL || !trace_runnable_acq(T, 1));
  assert_native_idle(L, idle_vmstate);

  /* Converting an invariant INT is the sole new boundary. An INT X or LIMIT
  ** at recording time must not admit a root with another conversion. */
  load_int_step_function(L, profile);
  call = numeric_args_int_x_negative(profile);
  for (i = 0; i < 4; i++)
    assert(call_triple(L, profile->name, call.x, call.limit, call.step,
	  1, 0, 1) == call.result);
  expect_no_trace(L, profile->name);

  load_int_step_function(L, profile);
  for (i = 0; i < 4; i++)
    assert(call_triple(L, profile->name,
	  data->integer_limit.x, data->integer_limit.limit,
	  data->integer_limit.step, 0, 1, 1) ==
	data->integer_limit.result);
  expect_no_trace(L, profile->name);
  assert_native_idle(L, idle_vmstate);
  lua_close(L);
}

static void test_fixed_initializers_remain_separate(void)
{
  lua_State *L = luaL_newstate();
  jit_State *J;
  GCproto *pt;
  GCtrace *T;
  assert(L != NULL);
  luaL_openlibs(L);
  run_lua(L,
    "jit.flush(); jit.on(); "
    "jit.opt.start('hotloop=1','hotexit=1','maxtrace=2'); "
    "function __arm64_fixed_initializer(limit,step) local x=0.5 "
      "while x<limit do x=x+step end return x end "
    "assert(__arm64_fixed_initializer(20.25,0.5)==20.5)");
  J = L2J(L);
  pt = global_proto(L, "__arm64_fixed_initializer");
  T = traceref_safe(J, 1);
  assert(pt->framesize == 5 && pt->sizebc == 14 && pt->numparams == 2);
  assert(pt->sizeuv == 0 && pt->sizekn == 1 && pt->sizekgc == 0);
  assert(proto_trace_acq(pt) == 1 && trace_runnable_acq(T, 1));
  assert(trace_startpt_acq(T) == pt);
  assert(trace_startpc_acq(T) == proto_bc(pt)+6u);
  assert(trace_nk_acq(T) == REF_TRUE);
  assert(trace_nins_acq(T) == REF_BASE+12u);

  run_lua(L,
    "jit.flush(); "
    "function __arm64_fixed_half(limit) local x=0.5 "
      "while x<limit do x=x+0.5 end return x end "
    "assert(__arm64_fixed_half(20.25)==20.5)");
  pt = global_proto(L, "__arm64_fixed_half");
  T = traceref_safe(J, 1);
  assert(pt->framesize == 4 && pt->sizebc == 13 && pt->numparams == 1);
  assert(pt->sizeuv == 0 && pt->sizekn == 1 && pt->sizekgc == 0);
  assert(proto_trace_acq(pt) == 1 && trace_runnable_acq(T, 1));
  assert(trace_startpt_acq(T) == pt);
  assert(trace_startpc_acq(T) == proto_bc(pt)+6u);
  assert(trace_nk_acq(T) < REF_TRUE);

  run_lua(L,
    "jit.flush(); "
    "function __arm64_fixed_initializer_inclusive(limit,step) local x=0.5 "
      "while x<=limit do x=x+step end return x end "
    "assert(__arm64_fixed_initializer_inclusive(20.25,0.5)==20.5)");
  expect_no_trace(L, "__arm64_fixed_initializer_inclusive");

  run_lua(L,
    "jit.flush(); "
    "function __arm64_fixed_half_inclusive(limit) local x=0.5 "
      "while x<=limit do x=x+0.5 end return x end "
    "assert(__arm64_fixed_half_inclusive(20.25)==20.5)");
  pt = global_proto(L, "__arm64_fixed_half_inclusive");
  expect_no_trace(L, "__arm64_fixed_half_inclusive");

  run_lua(L,
    "jit.flush(); "
    "function __arm64_fixed_initializer_mul_inclusive(limit,factor) "
      "local x=0.5 while x<=limit do x=x*factor end return x end "
    "assert(__arm64_fixed_initializer_mul_inclusive(20.25,2)==32)");
  expect_no_trace(L, "__arm64_fixed_initializer_mul_inclusive");

  run_lua(L,
    "jit.flush(); "
    "function __arm64_fixed_factor_mul_inclusive(x,limit) "
      "while x<=limit do x=x*2 end return x end "
    "assert(__arm64_fixed_factor_mul_inclusive(0.5,20.25)==32)");
  pt = global_proto(L, "__arm64_fixed_factor_mul_inclusive");
  expect_no_trace(L, "__arm64_fixed_factor_mul_inclusive");

  run_lua(L,
    "jit.flush(); "
    "function __arm64_fixed_initializer_div(limit,divisor) "
      "local x=0.5 while x<limit do x=x/divisor end return x end "
    "assert(__arm64_fixed_initializer_div(20.25,0.5)==32)");
  expect_no_trace(L, "__arm64_fixed_initializer_div");

  run_lua(L,
    "jit.flush(); "
    "function __arm64_fixed_divisor_div(x,limit) "
      "while x<limit do x=x/0.5 end return x end "
    "assert(__arm64_fixed_divisor_div(0.5,20.25)==32)");
  pt = global_proto(L, "__arm64_fixed_divisor_div");
  expect_no_trace(L, "__arm64_fixed_divisor_div");

  run_lua(L,
    "jit.flush(); "
    "function __arm64_fixed_initializer_div_inclusive(limit,divisor) "
      "local x=0.5 while x<=limit do x=x/divisor end return x end "
    "assert(__arm64_fixed_initializer_div_inclusive(20.25,0.5)==32)");
  expect_no_trace(L, "__arm64_fixed_initializer_div_inclusive");

  run_lua(L,
    "jit.flush(); "
    "function __arm64_fixed_divisor_div_inclusive(x,limit) "
      "while x<=limit do x=x/0.5 end return x end "
    "assert(__arm64_fixed_divisor_div_inclusive(0.5,20.25)==32)");
  pt = global_proto(L, "__arm64_fixed_divisor_div_inclusive");
  expect_no_trace(L, "__arm64_fixed_divisor_div_inclusive");

  run_lua(L,
    "jit.flush(); "
    "function __arm64_fixed_initializer_div_descending(limit,divisor) "
      "local x=16.0 while x>limit do x=x/divisor end return x end "
    "assert(__arm64_fixed_initializer_div_descending(1.0,2.0)==1.0)");
  expect_no_trace(L, "__arm64_fixed_initializer_div_descending");

  run_lua(L,
    "jit.flush(); "
    "function __arm64_fixed_divisor_div_descending(x,limit) "
      "while x>limit do x=x/2.0 end return x end "
    "assert(__arm64_fixed_divisor_div_descending(16.0,1.0)==1.0)");
  pt = global_proto(L, "__arm64_fixed_divisor_div_descending");
  expect_no_trace(L, "__arm64_fixed_divisor_div_descending");

  run_lua(L,
    "jit.flush(); "
    "function __arm64_fixed_initializer_div_descending_inclusive"
      "(limit,divisor) local x=16.0 while x>=limit do x=x/divisor end "
      "return x end "
    "assert(__arm64_fixed_initializer_div_descending_inclusive"
      "(1.0,2.0)==0.5)");
  expect_no_trace(L, "__arm64_fixed_initializer_div_descending_inclusive");

  run_lua(L,
    "jit.flush(); "
    "function __arm64_fixed_divisor_div_descending_inclusive(x,limit) "
      "while x>=limit do x=x/2.0 end return x end "
    "assert(__arm64_fixed_divisor_div_descending_inclusive"
      "(16.0,1.0)==0.5)");
  pt = global_proto(L, "__arm64_fixed_divisor_div_descending_inclusive");
  expect_no_trace(L, "__arm64_fixed_divisor_div_descending_inclusive");

  run_lua(L,
    "jit.flush(); "
    "function __arm64_fixed_initializer_add_descending(limit,step) "
      "local x=20.5 while x>limit do x=x+step end return x end "
    "assert(__arm64_fixed_initializer_add_descending(0.25,-0.5)==0.0)");
  expect_no_trace(L, "__arm64_fixed_initializer_add_descending");

  run_lua(L,
    "jit.flush(); "
    "function __arm64_fixed_half_add_descending(limit) local x=20.5 "
      "while x>limit do x=x+(-0.5) end return x end "
    "assert(__arm64_fixed_half_add_descending(0.25)==0.0)");
  pt = global_proto(L, "__arm64_fixed_half_add_descending");
  expect_no_trace(L, "__arm64_fixed_half_add_descending");

  run_lua(L,
    "jit.flush(); "
    "function __arm64_fixed_initializer_add_descending_inclusive(limit,step) "
      "local x=20.5 while x>=limit do x=x+step end return x end "
    "assert(__arm64_fixed_initializer_add_descending_inclusive"
      "(0.5,-0.5)==0.0)");
  expect_no_trace(L, "__arm64_fixed_initializer_add_descending_inclusive");

  run_lua(L,
    "jit.flush(); "
    "function __arm64_fixed_half_add_descending_inclusive(limit) "
      "local x=20.5 while x>=limit do x=x+(-0.5) end return x end "
    "assert(__arm64_fixed_half_add_descending_inclusive(0.5)==0.0)");
  pt = global_proto(L, "__arm64_fixed_half_add_descending_inclusive");
  expect_no_trace(L, "__arm64_fixed_half_add_descending_inclusive");

  run_lua(L,
    "jit.flush(); "
    "function __arm64_fixed_initializer_descending(limit,step) local x=20.5 "
      "while x>limit do x=x-step end return x end "
    "assert(__arm64_fixed_initializer_descending(0.25,0.5)==0.0)");
  expect_no_trace(L, "__arm64_fixed_initializer_descending");

  run_lua(L,
    "jit.flush(); "
    "function __arm64_fixed_half_descending(limit) local x=20.5 "
      "while x>limit do x=x-0.5 end return x end "
    "assert(__arm64_fixed_half_descending(0.25)==0.0)");
  pt = global_proto(L, "__arm64_fixed_half_descending");
  expect_no_trace(L, "__arm64_fixed_half_descending");

  run_lua(L,
    "jit.flush(); "
    "function __arm64_fixed_initializer_descending_inclusive(limit,step) "
      "local x=20.5 while x>=limit do x=x-step end return x end "
    "assert(__arm64_fixed_initializer_descending_inclusive(0.5,0.5)==0.0)");
  expect_no_trace(L, "__arm64_fixed_initializer_descending_inclusive");

  run_lua(L,
    "jit.flush(); "
    "function __arm64_fixed_half_descending_inclusive(limit) local x=20.5 "
      "while x>=limit do x=x-0.5 end return x end "
    "assert(__arm64_fixed_half_descending_inclusive(0.5)==0.0)");
  pt = global_proto(L, "__arm64_fixed_half_descending_inclusive");
  expect_no_trace(L, "__arm64_fixed_half_descending_inclusive");

  run_lua(L, "jit.flush()");
  assert(proto_trace_acq(pt) == 0);
  lua_close(L);
}

static void test_sub_lt_rejected(void)
{
  lua_State *L = luaL_newstate();
  assert(L != NULL);
  luaL_openlibs(L);
  run_lua(L,
    "jit.flush(); jit.on(); "
    "jit.opt.start('hotloop=1','hotexit=1','maxtrace=2'); "
    "function __arm64_args_sub_lt(x,limit,step) "
      "while x<limit do x=x-step end return x end");
  assert(call_triple(L, "__arm64_args_sub_lt",
	0.5, 20.25, -0.5, 0, 0, 0) == 20.5);
  expect_no_trace(L, "__arm64_args_sub_lt");
  lua_close(L);
}

static void test_div_adjacent_rejected(void)
{
  lua_State *L = luaL_newstate();
  assert(L != NULL);
  luaL_openlibs(L);

  run_lua(L,
    "jit.flush(); jit.on(); "
    "jit.opt.start('hotloop=1','hotexit=1','maxtrace=2'); "
    "function __arm64_args_div_reversed_compare(x,limit,divisor) "
      "while limit>x do x=x/divisor end return x end");
  assert(call_triple(L, "__arm64_args_div_reversed_compare",
	0.5, 20.25, 0.5, 0, 0, 0) == 32.0);
  expect_no_trace(L, "__arm64_args_div_reversed_compare");

  run_lua(L,
    "jit.flush(); "
    "function __arm64_args_div_inclusive_reversed_compare"
      "(x,limit,divisor) while limit>=x do x=x/divisor end return x end");
  assert(call_triple(L, "__arm64_args_div_inclusive_reversed_compare",
	0.5, 20.25, 0.5, 0, 0, 0) == 32.0);
  expect_no_trace(L, "__arm64_args_div_inclusive_reversed_compare");

  run_lua(L,
    "jit.flush(); "
    "function __arm64_args_div_reversed(x,limit,divisor) "
      "while x<limit do x=divisor/x end return x end");
  assert(call_triple(L, "__arm64_args_div_reversed",
	0.5, 0.75, 0.5, 0, 0, 0) == 1.0);
  expect_no_trace(L, "__arm64_args_div_reversed");

  run_lua(L,
    "jit.flush(); "
    "function __arm64_args_div_inclusive_reversed(x,limit,divisor) "
      "while x<=limit do x=divisor/x end return x end");
  assert(call_triple(L, "__arm64_args_div_inclusive_reversed",
	0.5, 0.75, 0.5, 0, 0, 0) == 1.0);
  expect_no_trace(L, "__arm64_args_div_inclusive_reversed");

  run_lua(L,
    "jit.flush(); "
    "function __arm64_args_div_extra(x,limit,divisor) "
      "while x<limit do x=x/divisor/divisor end return x end");
  assert(call_triple(L, "__arm64_args_div_extra",
	0.5, 20.25, 0.5, 0, 0, 0) == 32.0);
  expect_no_trace(L, "__arm64_args_div_extra");

  run_lua(L,
    "jit.flush(); "
    "function __arm64_args_div_inclusive_extra(x,limit,divisor) "
      "while x<=limit do x=x/divisor/divisor end return x end");
  assert(call_triple(L, "__arm64_args_div_inclusive_extra",
	0.5, 20.25, 0.5, 0, 0, 0) == 32.0);
  expect_no_trace(L, "__arm64_args_div_inclusive_extra");

  run_lua(L,
    "jit.flush(); "
    "function __arm64_args_div_descending_reversed_compare"
      "(x,limit,divisor) while limit<x do x=x/divisor end return x end");
  assert(call_triple(L, "__arm64_args_div_descending_reversed_compare",
	20.5, 0.5, 2.0, 0, 0, 0) == 0.3203125);
  expect_no_trace(L, "__arm64_args_div_descending_reversed_compare");

  run_lua(L,
    "jit.flush(); "
    "function __arm64_args_div_descending_inclusive_reversed_compare"
      "(x,limit,divisor) while limit<=x do x=x/divisor end return x end");
  assert(call_triple(L,
      "__arm64_args_div_descending_inclusive_reversed_compare",
	20.5, 0.5, 2.0, 0, 0, 0) == 0.3203125);
  expect_no_trace(L,
    "__arm64_args_div_descending_inclusive_reversed_compare");

  run_lua(L,
    "jit.flush(); "
    "function __arm64_args_div_descending_reversed(x,limit,divisor) "
      "while x>limit do x=divisor/x end return x end");
  assert(call_triple(L, "__arm64_args_div_descending_reversed",
	2.0, 1.5, 2.0, 0, 0, 0) == 1.0);
  expect_no_trace(L, "__arm64_args_div_descending_reversed");

  run_lua(L,
    "jit.flush(); "
    "function __arm64_args_div_descending_inclusive_reversed"
      "(x,limit,divisor) while x>=limit do x=divisor/x end return x end");
  assert(call_triple(L, "__arm64_args_div_descending_inclusive_reversed",
	2.0, 1.5, 2.0, 0, 0, 0) == 1.0);
  expect_no_trace(L, "__arm64_args_div_descending_inclusive_reversed");

  run_lua(L,
    "jit.flush(); "
    "function __arm64_args_div_descending_extra(x,limit,divisor) "
      "while x>limit do x=x/divisor/divisor end return x end");
  assert(call_triple(L, "__arm64_args_div_descending_extra",
	20.5, 0.5, 2.0, 0, 0, 0) == 0.3203125);
  expect_no_trace(L, "__arm64_args_div_descending_extra");

  run_lua(L,
    "jit.flush(); "
    "function __arm64_args_div_descending_inclusive_extra"
      "(x,limit,divisor) while x>=limit do x=x/divisor/divisor end "
      "return x end");
  assert(call_triple(L, "__arm64_args_div_descending_inclusive_extra",
	20.5, 0.5, 2.0, 0, 0, 0) == 0.3203125);
  expect_no_trace(L, "__arm64_args_div_descending_inclusive_extra");
  lua_close(L);
}

static void test_adjacent_comparisons_rejected(void)
{
  lua_State *L = luaL_newstate();
  assert(L != NULL);
  luaL_openlibs(L);
  run_lua(L,
    "jit.flush(); jit.on(); "
    "jit.opt.start('hotloop=1','hotexit=1','maxtrace=2'); "
    "function __arm64_args_reversed_compare(x,limit,step) "
      "while limit>=x do x=x+step end return x end");
  assert(call_triple(L, "__arm64_args_reversed_compare",
	0.5, 20.25, 0.5, 0, 0, 0) == 20.5);
  expect_no_trace(L, "__arm64_args_reversed_compare");

  run_lua(L,
    "jit.flush(); "
    "function __arm64_args_reversed_add(x,limit,step) "
      "while x<=limit do x=step+x end return x end");
  assert(call_triple(L, "__arm64_args_reversed_add",
	0.5, 20.25, 0.5, 0, 0, 0) == 20.5);
  expect_no_trace(L, "__arm64_args_reversed_add");
  lua_close(L);
}

static void test_mul_inclusive_adjacent_rejected(void)
{
  lua_State *L = luaL_newstate();
  assert(L != NULL);
  luaL_openlibs(L);

  run_lua(L,
    "jit.flush(); jit.on(); "
    "jit.opt.start('hotloop=1','hotexit=1','maxtrace=2'); "
    "function __arm64_args_mul_inclusive_reversed_compare"
      "(x,limit,factor) while limit>=x do x=x*factor end return x end");
  assert(call_triple(L, "__arm64_args_mul_inclusive_reversed_compare",
	0.5, 20.25, 2.0, 0, 0, 0) == 32.0);
  expect_no_trace(L, "__arm64_args_mul_inclusive_reversed_compare");

  run_lua(L,
    "jit.flush(); "
    "function __arm64_args_mul_inclusive_reversed(x,limit,factor) "
      "while x<=limit do x=factor*x end return x end");
  assert(call_triple(L, "__arm64_args_mul_inclusive_reversed",
	0.5, 20.25, 2.0, 0, 0, 0) == 32.0);
  expect_no_trace(L, "__arm64_args_mul_inclusive_reversed");

  run_lua(L,
    "jit.flush(); "
    "function __arm64_args_mul_inclusive_extra(x,limit,factor) "
      "while x<=limit do x=x*factor*factor end return x end");
  assert(call_triple(L, "__arm64_args_mul_inclusive_extra",
	0.5, 20.25, 2.0, 0, 0, 0) == 32.0);
  expect_no_trace(L, "__arm64_args_mul_inclusive_extra");

  lua_close(L);
}

static void test_add_descending_adjacent_rejected(void)
{
  lua_State *L = luaL_newstate();
  assert(L != NULL);
  luaL_openlibs(L);

  run_lua(L,
    "jit.flush(); jit.on(); "
    "jit.opt.start('hotloop=1','hotexit=1','maxtrace=2'); "
    "function __arm64_args_reversed_add_gt_compare(x,limit,step) "
      "while limit<x do x=x+step end return x end");
  assert(call_triple(L, "__arm64_args_reversed_add_gt_compare",
	20.5, 0.25, -0.5, 0, 0, 0) == 0.0);
  expect_no_trace(L, "__arm64_args_reversed_add_gt_compare");

  run_lua(L,
    "jit.flush(); "
    "function __arm64_args_reversed_add_gt(x,limit,step) "
      "while x>limit do x=step+x end return x end");
  assert(call_triple(L, "__arm64_args_reversed_add_gt",
	20.5, 0.25, -0.5, 0, 0, 0) == 0.0);
  expect_no_trace(L, "__arm64_args_reversed_add_gt");

  run_lua(L,
    "jit.flush(); "
    "function __arm64_args_extra_add_gt(x,limit,step) "
      "while x>limit do x=x+step+step end return x end");
  assert(call_triple(L, "__arm64_args_extra_add_gt",
	20.5, 0.25, -0.25, 0, 0, 0) == 0.0);
  expect_no_trace(L, "__arm64_args_extra_add_gt");

  run_lua(L,
    "jit.flush(); "
    "function __arm64_args_add_gt_mul(x,limit,step) "
      "while x>limit do x=x*step end return x end");
  assert(call_triple(L, "__arm64_args_add_gt_mul",
	20.5, 0.5, 0.5, 0, 0, 0) == 0.3203125);
  expect_no_trace(L, "__arm64_args_add_gt_mul");

  run_lua(L,
    "jit.flush(); "
    "function __arm64_args_reversed_add_ge_compare(x,limit,step) "
      "while limit<=x do x=x+step end return x end");
  assert(call_triple(L, "__arm64_args_reversed_add_ge_compare",
	20.5, 0.5, -0.5, 0, 0, 0) == 0.0);
  expect_no_trace(L, "__arm64_args_reversed_add_ge_compare");

  run_lua(L,
    "jit.flush(); "
    "function __arm64_args_reversed_add_ge(x,limit,step) "
      "while x>=limit do x=step+x end return x end");
  assert(call_triple(L, "__arm64_args_reversed_add_ge",
	20.5, 0.5, -0.5, 0, 0, 0) == 0.0);
  expect_no_trace(L, "__arm64_args_reversed_add_ge");

  run_lua(L,
    "jit.flush(); "
    "function __arm64_args_extra_add_ge(x,limit,step) "
      "while x>=limit do x=x+step+step end return x end");
  assert(call_triple(L, "__arm64_args_extra_add_ge",
	20.5, 0.5, -0.25, 0, 0, 0) == 0.0);
  expect_no_trace(L, "__arm64_args_extra_add_ge");

  run_lua(L,
    "jit.flush(); "
    "function __arm64_args_add_ge_mul(x,limit,step) "
      "while x>=limit do x=x*step end return x end");
  assert(call_triple(L, "__arm64_args_add_ge_mul",
	20.5, 0.5, 0.5, 0, 0, 0) == 0.3203125);
  expect_no_trace(L, "__arm64_args_add_ge_mul");

  lua_close(L);
}

static void test_extra_add_rejected(void)
{
  lua_State *L = luaL_newstate();
  assert(L != NULL);
  luaL_openlibs(L);
  run_lua(L,
    "jit.flush(); jit.on(); "
    "jit.opt.start('hotloop=1','hotexit=1','maxtrace=2'); "
    "function __arm64_args_negative(x,limit,step) "
      "while x<limit do x=x+step+step end return x end");
  assert(call_triple(L, "__arm64_args_negative",
	0.5, 20.25, 0.25, 0, 0, 0) == 20.5);
  expect_no_trace(L, "__arm64_args_negative");

  run_lua(L,
    "jit.flush(); "
    "function __arm64_args_extra_inclusive(x,limit,step) "
      "while x<=limit do x=x+step+step end return x end");
  assert(call_triple(L, "__arm64_args_extra_inclusive",
	0.5, 20.25, 0.25, 0, 0, 0) == 20.5);
  expect_no_trace(L, "__arm64_args_extra_inclusive");
  lua_close(L);
}

static void test_descending_adjacent_rejected(void)
{
  lua_State *L = luaL_newstate();
  int i;
  assert(L != NULL);
  luaL_openlibs(L);

  run_lua(L,
    "jit.flush(); jit.on(); "
    "jit.opt.start('hotloop=1','hotexit=1','maxtrace=2'); "
    "function __arm64_args_sub_le(x,limit,step) "
      "while x<=limit do x=x-step end return x end");
  assert(call_triple(L, "__arm64_args_sub_le",
	0.5, 20.25, -0.5, 0, 0, 0) == 20.5);
  expect_no_trace(L, "__arm64_args_sub_le");

  run_lua(L,
    "jit.flush(); "
    "function __arm64_args_descending_mul(x,limit,step) "
      "while x>limit do x=x*step end return x end");
  assert(call_triple(L, "__arm64_args_descending_mul",
	20.5, 0.5, 0.5, 0, 0, 0) == 0.3203125);
  expect_no_trace(L, "__arm64_args_descending_mul");

  run_lua(L,
    "jit.flush(); "
    "function __arm64_args_descending_inclusive_mul(x,limit,step) "
      "while x>=limit do x=x*step end return x end");
  assert(call_triple(L, "__arm64_args_descending_inclusive_mul",
	20.5, 0.5, 0.5, 0, 0, 0) == 0.3203125);
  expect_no_trace(L, "__arm64_args_descending_inclusive_mul");

  run_lua(L,
    "jit.flush(); "
    "function __arm64_args_reversed_descending_compare(x,limit,step) "
      "while limit<x do x=x-step end return x end");
  assert(call_triple(L, "__arm64_args_reversed_descending_compare",
	20.5, 0.25, 0.5, 0, 0, 0) == 0.0);
  expect_no_trace(L, "__arm64_args_reversed_descending_compare");

  run_lua(L,
    "jit.flush(); "
    "function __arm64_args_reversed_descending_inclusive_compare"
      "(x,limit,step) while limit<=x do x=x-step end return x end");
  assert(call_triple(L, "__arm64_args_reversed_descending_inclusive_compare",
	20.5, 0.5, 0.5, 0, 0, 0) == 0.0);
  expect_no_trace(L, "__arm64_args_reversed_descending_inclusive_compare");

  run_lua(L,
    "jit.flush(); "
    "function __arm64_args_reversed_sub(x,limit,step) "
      "while x>limit do x=step-x end return x end");
  for (i = 0; i < 4; i++)
    assert(call_triple(L, "__arm64_args_reversed_sub",
	2.0, 0.0, 1.0, 0, 0, 0) == -1.0);
  expect_no_trace(L, "__arm64_args_reversed_sub");

  run_lua(L,
    "jit.flush(); "
    "function __arm64_args_reversed_inclusive_sub(x,limit,step) "
      "while x>=limit do x=step-x end return x end");
  for (i = 0; i < 4; i++)
    assert(call_triple(L, "__arm64_args_reversed_inclusive_sub",
	2.0, 0.0, 1.0, 0, 0, 0) == -1.0);
  expect_no_trace(L, "__arm64_args_reversed_inclusive_sub");

  run_lua(L,
    "jit.flush(); "
    "function __arm64_args_extra_sub(x,limit,step) "
      "while x>limit do x=x-step-step end return x end");
  assert(call_triple(L, "__arm64_args_extra_sub",
	20.5, 0.5, 0.25, 0, 0, 0) == 0.5);
  expect_no_trace(L, "__arm64_args_extra_sub");

  run_lua(L,
    "jit.flush(); "
    "function __arm64_args_extra_inclusive_sub(x,limit,step) "
      "while x>=limit do x=x-step-step end return x end");
  assert(call_triple(L, "__arm64_args_extra_inclusive_sub",
	20.5, 0.5, 0.25, 0, 0, 0) == 0.0);
  expect_no_trace(L, "__arm64_args_extra_inclusive_sub");
  lua_close(L);
}

int main(int argc, char **argv)
{
  assert(argc == 1 || argc == 2);
  if (argc == 2)
    assert(strcmp(argv[1], "direct") == 0 ||
	   strcmp(argv[1], "randomized") == 0);
  test_positive_and_guard_exits(&strict_profile);
  test_positive_and_guard_exits(&inclusive_profile);
  test_positive_and_guard_exits(&mul_profile);
  test_positive_and_guard_exits(&mul_inclusive_profile);
  test_positive_and_guard_exits(&div_profile);
  test_positive_and_guard_exits(&div_inclusive_profile);
  test_positive_and_guard_exits(&div_descending_profile);
  test_positive_and_guard_exits(&div_descending_inclusive_profile);
  test_positive_and_guard_exits(&add_descending_profile);
  test_positive_and_guard_exits(&add_descending_inclusive_profile);
  test_positive_and_guard_exits(&descending_profile);
  test_positive_and_guard_exits(&descending_inclusive_profile);
  test_int_step_positive_and_guard_exits(&strict_profile);
  test_int_step_positive_and_guard_exits(&inclusive_profile);
  test_int_step_positive_and_guard_exits(&mul_profile);
  test_int_step_positive_and_guard_exits(&mul_inclusive_profile);
  test_int_step_positive_and_guard_exits(&div_profile);
  test_int_step_positive_and_guard_exits(&div_inclusive_profile);
  test_int_step_positive_and_guard_exits(&div_descending_profile);
  test_int_step_positive_and_guard_exits(
	&div_descending_inclusive_profile);
  test_int_step_positive_and_guard_exits(&add_descending_profile);
  test_int_step_positive_and_guard_exits(
	&add_descending_inclusive_profile);
  test_int_step_positive_and_guard_exits(&descending_profile);
  test_int_step_positive_and_guard_exits(&descending_inclusive_profile);
  test_fixed_initializers_remain_separate();
  test_sub_lt_rejected();
  test_div_adjacent_rejected();
  test_adjacent_comparisons_rejected();
  test_mul_inclusive_adjacent_rejected();
  test_extra_add_rejected();
  test_add_descending_adjacent_rejected();
  test_descending_adjacent_rejected();
  puts("t-arm64-jit-pure-numeric-args OK");
  return 0;
}

#else

int main(void)
{
  puts("t-arm64-jit-pure-numeric-args SKIP");
  return 0;
}

#endif

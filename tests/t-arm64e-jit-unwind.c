/*
** macOS arm64e dynamic JIT-frame unwind contract.
**
** This creates a tiny MAP_JIT function covered by lj_err.c's production JIT
** CIE/FDE, raises a real LuaJIT exception through it, and requires the JIT
** personality to install and resume at a landing inside the generated body.
*/

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <pthread.h>
#include <ptrauth.h>

#include "lua.h"
#include "lauxlib.h"

#include "lj_arch.h"
#include "lj_err.h"
#include "lj_mcode.h"
#include "lj_obj.h"

#if !defined(__APPLE__) || !defined(__arm64e__) || !LJ_TARGET_OSX || \
    !LJ_TARGET_ARM64 || !LJ_ABI_PAUTH || !LJ_UNWIND_JIT || \
    !defined(LJ_ERR_UNWIND_TEST_HELPERS)
#error "t-arm64e-jit-unwind requires macOS arm64e JIT unwind test helpers"
#endif

extern void lj_err_test_arm64e_unwind_arm(uintptr_t start, uintptr_t end,
					   uintptr_t landing);
extern uint32_t lj_err_test_arm64e_unwind_disarm(void);

typedef void (*ThrowFn)(lua_State *L);
typedef int (*ProbeFn)(ThrowFn throwfn, lua_State *L);

enum {
  PROBE_LANDING_WORD = 8,
  PROBE_RESULT = 0x4a17,
  PROBE_COUNTS = 0x010101
};

/*
**   bti c
**   pacibsp
**   stp x29, x30, [sp, #-16]!
**   mov x29, sp
**   mov x16, x0
**   mov x0, x1
**   blraaz x16
**   brk #1                         // the throw must not return
** landing:
**   ldp x29, x30, [sp], #16
**   retab
**
** The authenticated prologue/epilogue makes the saved caller LR contract
** explicit. blraaz consumes the ordinary signed C function pointer passed in
** x0; the landing preserves x0 installed by the personality as the result.
*/
static const uint32_t probe_words[] = {
  0xd503245fu, 0xd503237fu, 0xa9bf7bfdu, 0x910003fdu,
  0xaa0003f0u, 0xaa0103e0u, 0xd63f0a1fu, 0xd4200020u,
  0xa8c17bfdu, 0xd65f0fffu
};

static LJ_NOINLINE void throw_from_mcode(lua_State *L)
{
  lj_err_throw(L, LUA_ERRRUN);
}

int main(void)
{
  lua_State *L;
  size_t pagesz;
  uint8_t *area;
  uint8_t *code;
  uintptr_t landing;
  ProbeFn probe;
  int result;

  pagesz = (size_t)sysconf(_SC_PAGESIZE);
  assert(pagesz >= 4096u);
  area = (uint8_t *)mmap(NULL, pagesz,
	PROT_READ|PROT_WRITE|PROT_EXEC,
	MAP_PRIVATE|MAP_ANON|MAP_JIT, -1, 0);
  assert(area != MAP_FAILED);

  if (pthread_jit_write_protect_supported_np())
    pthread_jit_write_protect_np(0);
  code = lj_err_register_mcode(area, pagesz, area, area);
  assert(code > area && code + sizeof(probe_words) < area + pagesz);
  memcpy(code, probe_words, sizeof(probe_words));
  lj_mcode_sync(code, code + sizeof(probe_words));
  if (pthread_jit_write_protect_supported_np())
    pthread_jit_write_protect_np(1);

  landing = (uintptr_t)(code + PROBE_LANDING_WORD * sizeof(uint32_t));
  probe = (ProbeFn)ptrauth_sign_unauthenticated(code,
	 ptrauth_key_function_pointer, 0);

  L = luaL_newstate();
  assert(L != NULL);
  /* The interpreter personality installs lj_vm_unwind_c_eh through the same
  ** arm64e _Unwind_SetIP contract as the JIT personality. */
  assert(luaL_loadstring(L, "error('arm64e interpreter unwind')") == LUA_OK);
  assert(lua_pcall(L, 0, 0, 0) == LUA_ERRRUN);
  assert(strstr(lua_tostring(L, -1), "arm64e interpreter unwind") != NULL);
  lua_pop(L, 1);

  lj_err_test_arm64e_unwind_arm((uintptr_t)code,
	(uintptr_t)(code + sizeof(probe_words)), landing);
  result = probe(throw_from_mcode, L);
  assert(result == PROBE_RESULT);
  assert(lj_err_test_arm64e_unwind_disarm() == PROBE_COUNTS);
  lua_close(L);

  lj_err_deregister_mcode(area, pagesz, area);
  assert(munmap(area, pagesz) == 0);
  puts("t-arm64e-jit-unwind OK: registered personality handled and landed");
  return 0;
}

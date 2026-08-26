/*
** macOS ARM64e generated FFI callback pointer-authentication contract.
*/

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <ptrauth.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_ctype.h"
#include "lj_ccallback.h"

#include "lib/lua_fixture_helpers.h"

#if !defined(__APPLE__) || !defined(__arm64e__) || !LJ_TARGET_OSX || \
    !LJ_TARGET_ARM64 || !LJ_ABI_PAUTH || !LJ_ABI_BRANCH_TRACK
#error "t-arm64e-ffi-callback-pauth requires macOS ARM64e with PAUTH and BTI"
#endif

typedef int (*PauthCallback)(int);
typedef void (*CaptureCallback)(PauthCallback);

enum { ARM64_BTI_C = 0xd503245f };

static lua_State *mainL;
static CTState *saved_cts;
static PauthCallback saved_cb;
static MSize saved_slot;
static unsigned int capture_count;

static void *callback_bits(PauthCallback cb)
{
  return ptrauth_nop_cast(void *, cb);
}

static void *capture_bits(CaptureCallback cb)
{
  return ptrauth_nop_cast(void *, cb);
}

static void capture_callback(PauthCallback cb)
{
  saved_cts = ctype_cts(mainL);
  saved_cb = cb;
  saved_slot = lj_ccallback_ptr2slot(saved_cts, callback_bits(cb));
  assert(saved_slot != ~0u);
  assert(saved_slot < ctype_cb_sizeid_acq(saved_cts));
  capture_count++;
}

static void assert_callback_pointer_contract(int arg, int expected)
{
  PauthCallback authenticated;
  void *signed_bits = callback_bits(saved_cb);
  uint8_t *raw = (uint8_t *)lj_ptr_strip(signed_bits);
  uint8_t *mcode = (uint8_t *)ctype_cb_mcode_acq(saved_cts);
  uint32_t entry;

  assert(raw >= mcode);
  assert(raw < mcode + LJ_PAGESIZE * LJ_NUM_CBPAGE);
  assert((uintptr_t)signed_bits != (uintptr_t)raw);
  assert(lj_ccallback_ptr2slot(saved_cts, signed_bits) == saved_slot);

  /* An indirect C call authenticates with IA/key-function-pointer and zero
  ** discriminator. Authenticate explicitly too, then make the same call. */
  authenticated = ptrauth_auth_function(saved_cb,
    ptrauth_key_function_pointer, 0);
  assert(authenticated(arg) == expected);
  assert(saved_cb(arg) == expected);

  memcpy(&entry, raw, sizeof(entry));
  assert(entry == ARM64_BTI_C);

  /* Lookup strips the valid callback signature before range arithmetic and
  ** rejects pointers which are not exact slot entries. */
  assert(lj_ccallback_ptr2slot(saved_cts,
    lj_ptr_sign(raw + sizeof(uint32_t), 0)) == ~0u);
  assert(lj_ccallback_ptr2slot(saved_cts,
    lj_ptr_sign(mcode, 0)) == ~0u);
  assert(lj_ccallback_ptr2slot(saved_cts,
    lj_ptr_sign((void *)((uintptr_t)mcode - sizeof(uint32_t)), 0)) == ~0u);
  assert(lj_ccallback_ptr2slot(saved_cts,
    lj_ptr_sign(mcode + LJ_PAGESIZE * LJ_NUM_CBPAGE, 0)) == ~0u);
  assert(lj_ccallback_ptr2slot(saved_cts,
    capture_bits(capture_callback)) == ~0u);
  assert(lj_ccallback_ptr2slot(saved_cts, NULL) == ~0u);
}

int main(void)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  MSize first_slot;
  uintptr_t first_raw;
  uintptr_t first_signed;
  mainL = L;

  lua_pushlightuserdata(L,
    ptrauth_nop_cast(void *, (CaptureCallback)capture_callback));
  lua_setglobal(L, "lj_arm64e_capture_callback");

  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "ffi.cdef[[\n"
    "typedef int (*lj_arm64e_callback_t)(int);\n"
    "typedef void (*lj_arm64e_capture_t)(lj_arm64e_callback_t);\n"
    "]]\n"
    "local capture = ffi.cast('lj_arm64e_capture_t',\n"
    "                         lj_arm64e_capture_callback)\n"
    "local cb = ffi.cast('lj_arm64e_callback_t', function(x)\n"
    "  return x + 7\n"
    "end)\n"
    "capture(cb)\n"
    "arm64e_keep_callback = cb\n");

  assert(capture_count == 1);
  assert_callback_pointer_contract(35, 42);
  first_slot = saved_slot;
  first_raw = (uintptr_t)lj_ptr_strip(callback_bits(saved_cb));
  first_signed = (uintptr_t)callback_bits(saved_cb);

  ljt_lua_dostring(L,
    "arm64e_keep_callback:set(function(x) return x * 3 end)\n");
  assert_callback_pointer_contract(14, 42);

  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "arm64e_keep_callback:free()\n"
    "local cb = ffi.cast('lj_arm64e_callback_t', function(x)\n"
    "  return x - 8\n"
    "end)\n"
    "local capture = ffi.cast('lj_arm64e_capture_t',\n"
    "                         lj_arm64e_capture_callback)\n"
    "capture(cb)\n"
    "arm64e_keep_callback = cb\n");

  assert(capture_count == 2);
  assert(saved_slot == first_slot);
  assert((uintptr_t)lj_ptr_strip(callback_bits(saved_cb)) == first_raw);
  assert((uintptr_t)callback_bits(saved_cb) == first_signed);
  assert_callback_pointer_contract(50, 42);

  ljt_lua_dostring(L,
    "arm64e_keep_callback:free()\n"
    "arm64e_keep_callback = nil\n"
    "collectgarbage('collect')\n");
  lua_close(L);
  puts("t-arm64e-ffi-callback-pauth OK: signed BTI callback create/call/set/free/reuse verified");
  return 0;
}

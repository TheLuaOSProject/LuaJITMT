/* Shared FFI errno and platform last-error preservation regression. */
#include <assert.h>
#include <errno.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "lj_obj.h"
#include "lj_ctype.h"
#include "lj_gc.h"
#include "lib/lua_fixture_helpers.h"

#if defined(_WIN32)
#include <windows.h>
#define TEST_LAST_ERROR ((DWORD)0x1234abcd)
#define set_test_last_error() SetLastError(TEST_LAST_ERROR)
#define check_test_last_error() assert(GetLastError() == TEST_LAST_ERROR)
#else
#define set_test_last_error() ((void)0)
#define check_test_last_error() ((void)0)
#endif

static void check_parse_errno(lua_State *L)
{
  CTState *cts = ctype_ctsG(G(L));
  unsigned int i;
  for (i = 0; i < 2; i++) {
    uint32_t seq = ctype_parse_token_acq(cts);
    assert(!(seq & 1u));
    ctype_parse_token_rel(cts, seq + 1u);
    errno = 73;
    set_test_last_error();
    /* Exercise the real timed kernel wait, both owner modes, and unlock. */
    lj_ctype_parse_wait(cts, i ? L : NULL, seq + 1u);
    assert(errno == 73);
    check_test_last_error();
    lj_ctype_parse_unlock(cts);
    assert(errno == 73);
    check_test_last_error();
    assert(ctype_parse_token_acq(cts) == seq + 2u);
  }
}

static void check_gc_errno(lua_State *L)
{
  unsigned int i;
  for (i = 0; i < 64; i++) {
    lua_newtable(L);
    lj_gc_threshold_store(G(L), 0);
    errno = 73;
    set_test_last_error();
    (void)lj_gc_step(L);
    assert(errno == 73);
    check_test_last_error();
    lj_gc_threshold_store(G(L), 0);
    errno = 73;
    set_test_last_error();
    lj_gc_step_top(L);
    assert(errno == 73);
    check_test_last_error();
    lua_pop(L, 1);
  }
}

int main(void)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  /* Initialize FFI's CTState before reaching through the internal layout. */
  ljt_lua_dostring(L, "local ffi = require('ffi'); ffi.errno(73)");
  check_parse_errno(L);
  check_gc_errno(L);
  lua_close(L);
  return 0;
}

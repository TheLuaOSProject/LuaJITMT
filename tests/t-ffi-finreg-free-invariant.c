/*
** Focused guard for cdata FINREG sweep/free ownership.
*/

#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_cdata.h"
#include "lj_gc.h"

#if LJ_HASFFI
static void disable_core_files(void)
{
  struct rlimit lim;
  lim.rlim_cur = 0;
  lim.rlim_max = 0;
  (void)setrlimit(RLIMIT_CORE, &lim);
}

static void child_must_abort(lua_State *L, GCcdata *cd)
{
  global_State *g = G(L);
  disable_core_files();
  lj_obj_addgcflags_atomic(obj2gco(cd), LJ_GC_CDATA_FIN);
  lj_cdata_free(g, cd);
  _exit(101);
}
#endif

int main(void)
{
#if LJ_HASFFI
  lua_State *L = luaL_newstate();
  GCcdata *cd;
  pid_t pid;
  int status = 0;

  assert(L != NULL);
  luaL_openlibs(L);
  assert(luaL_dostring(L, "require('ffi')") == LUA_OK);
  lua_settop(L, 0);
  cd = lj_cdata_new_(L, CTID_INT32, 4);
  assert(cd != NULL);

  pid = fork();
  assert(pid >= 0);
  if (pid == 0)
    child_must_abort(L, cd);

  assert(waitpid(pid, &status, 0) == pid);
  assert(WIFSIGNALED(status));
  assert(WTERMSIG(status) == SIGABRT);

  lua_close(L);
  printf("t-ffi-finreg-free-invariant OK: finalizable cdata free path aborts\n");
#else
  printf("t-ffi-finreg-free-invariant SKIP: FFI disabled\n");
#endif
  return 0;
}

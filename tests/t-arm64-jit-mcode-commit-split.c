/*
** Exact prepare/publish split for an ARM64 publication transaction.
*/

#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_jit.h"
#include "lj_dispatch.h"
#include "lj_mcode.h"

extern char **environ;

static MCode *reservation_target(MCode *top, MCode *bot)
{
  size_t available;
  assert(top != NULL && bot != NULL && top > bot);
  available = (size_t)(top-bot);
  assert(available >= 16u);
  return top-16u;
}

static lua_State *new_test_state(void)
{
  lua_State *L = luaL_newstate();
  assert(L != NULL);
  luaL_openlibs(L);
  assert(luaL_dostring(L,
    "jit.flush()\n"
    "jit.opt.start('hotloop=1', 'hotexit=1')\n"
    "local function f(n)\n"
    "  local i, x = 0, 0\n"
    "  while i < n do i = i + 1; x = x + i end\n"
    "  return x\n"
    "end\n"
    "for _ = 1, 40 do assert(f(120) == 7260) end\n") == 0);
  return L;
}

static void stale_publish_negative(int ready_fd)
{
  lua_State *L = new_test_state();
  jit_State *J = G2J(G(L));
  LJMCodeCommitPlan stale, replacement;
  MCode *bot, *oldtop, *newtop, *replacement_top;

  oldtop = lj_mcode_reserve(J, &bot);
  newtop = reservation_target(oldtop, bot);
  assert(lj_mcode_commit_prepare(J, newtop, &stale));
  lj_mcode_abort(J);
  replacement_top = lj_mcode_reserve(J, &bot);
  assert(replacement_top == oldtop);
  assert(reservation_target(replacement_top, bot) == newtop);
  assert(lj_mcode_commit_prepare(J, newtop, &replacement));
  assert(replacement.generation != stale.generation);

  /* This must fail-stop even though area, old top and new top are identical. */
  {
    const char ready = 'R';
    ssize_t written;
    do {
      written = write(ready_fd, &ready, 1u);
    } while (written == (ssize_t)-1 && errno == EINTR);
    if (written != 1)
      _exit(98);
    (void)close(ready_fd);
  }
  lj_mcode_commit_publish(J, &stale);
}

static void check_stale_publish_aborts(const char *self)
{
  char fdarg[32];
  char *child_argv[4];
  int ready_pipe[2];
  pid_t child, waited;
  char ready = 0;
  ssize_t nread;
  int rc, status;

  assert(pipe(ready_pipe) == 0);
  assert(snprintf(fdarg, sizeof(fdarg), "%d", ready_pipe[1]) > 0);
  child_argv[0] = (char *)self;
  child_argv[1] = (char *)"--stale-plan";
  child_argv[2] = fdarg;
  child_argv[3] = NULL;
  rc = posix_spawn(&child, self, NULL, NULL, child_argv, environ);
  assert(rc == 0);
  (void)close(ready_pipe[1]);
  do {
    waited = waitpid(child, &status, 0);
  } while (waited == (pid_t)-1 && errno == EINTR);
  do {
    nread = read(ready_pipe[0], &ready, 1u);
  } while (nread == (ssize_t)-1 && errno == EINTR);
  (void)close(ready_pipe[0]);
  assert(waited == child);
  assert(nread == 1 && ready == 'R');
  assert(WIFSIGNALED(status));
  assert(WTERMSIG(status) == SIGABRT);
}

int main(int argc, char **argv)
{
  lua_State *L;
  jit_State *J;
  LJMCodeCommitPlan plan, stale, replacement;
  MCode *bot, *oldtop, *newtop, *replacement_top;
  uint64_t generation;

  if (argc == 3 && strcmp(argv[1], "--stale-plan") == 0) {
    stale_publish_negative(atoi(argv[2]));
    return 99;
  }
  assert(argc == 1);
  L = new_test_state();
  J = G2J(G(L));
  assert(J->mcarea != NULL && J->mctop != NULL && J->mcbot != NULL);

  /* Prepare closes the protection transition but cannot consume space. */
  oldtop = lj_mcode_reserve(J, &bot);
  newtop = reservation_target(oldtop, bot);
  generation = J->mcreserve_generation;
  assert((generation & 1u) != 0);
  assert(J->mctop == oldtop);
  assert(!lj_mcode_commit_prepare(NULL, newtop, &plan));
  assert(plan.oldtop == NULL && plan.newtop == NULL && plan.generation == 0);
  assert(!lj_mcode_commit_prepare(J, newtop, NULL));
  assert(!lj_mcode_commit_prepare(J, NULL, &plan));
  assert(plan.oldtop == NULL && plan.newtop == NULL && plan.generation == 0);
  assert(!lj_mcode_commit_prepare(J,
    (MCode *)(void *)((uintptr_t)(void *)bot-sizeof(MCode)), &plan));
  assert(!lj_mcode_commit_prepare(J,
    (MCode *)(void *)((uintptr_t)(void *)oldtop+sizeof(MCode)), &plan));
  assert(lj_mcode_commit_prepare(J, newtop, &plan));
  assert(plan.oldtop == oldtop && plan.newtop == newtop);
  assert(plan.generation == generation);
  assert(J->mctop == oldtop);
  lj_mcode_commit_publish(J, &plan);
  assert(J->mctop == newtop);
  assert(J->mcreserve_generation == generation+1u);
  assert((J->mcreserve_generation & 1u) == 0);

  /* Abort invalidates an exact stale plan before identical geometry is reused. */
  oldtop = lj_mcode_reserve(J, &bot);
  newtop = reservation_target(oldtop, bot);
  assert(lj_mcode_commit_prepare(J, newtop, &stale));
  assert(stale.oldtop == oldtop && stale.newtop == newtop);
  assert(J->mctop == oldtop);
  lj_mcode_abort(J);
  assert(J->mctop == oldtop);
  assert(J->mcreserve_generation != stale.generation);
  assert((J->mcreserve_generation & 1u) == 0);

  replacement_top = lj_mcode_reserve(J, &bot);
  assert(replacement_top == oldtop);
  assert(reservation_target(replacement_top, bot) == newtop);
  assert(lj_mcode_commit_prepare(J, newtop, &replacement));
  assert(replacement.oldtop == stale.oldtop);
  assert(replacement.newtop == stale.newtop);
  assert(replacement.generation != stale.generation);
  lj_mcode_commit_publish(J, &replacement);
  assert(J->mctop == newtop);

  /* The legacy composite keeps its exact historical result. */
  oldtop = lj_mcode_reserve(J, &bot);
  newtop = reservation_target(oldtop, bot);
  lj_mcode_commit(J, newtop);
  assert(J->mctop == newtop);

  lua_close(L);
  check_stale_publish_aborts(argv[0]);
  puts("t-arm64-jit-mcode-commit-split OK");
  return 0;
}

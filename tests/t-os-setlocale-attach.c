/* Check foreign attachment during locale writes and queries. */

#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <locale.h>
#include <pthread.h>
#include <string.h>
#include <time.h>

#include "lj_thr.h"
#include "lib/lua_fixture_helpers.h"

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static int writer_entered, attach_entered, attach_crossed;
static int attach_complete;
static _Thread_local int pause_query;

char *__real_setlocale(int category, const char *locale);
void __real_lj_locale_wait_attach(void);

char *__wrap_setlocale(int category, const char *locale)
{
  int write = locale && strcmp(locale, "test-paused-write") == 0;
  if (write || (!locale && pause_query)) {
    struct timespec deadline;
    int status = 0;
    pause_query = 0;
    assert(pthread_mutex_lock(&mu) == 0);
    writer_entered = 1;
    assert(pthread_cond_broadcast(&cv) == 0);
    while (!attach_entered)
      assert(pthread_cond_wait(&cv, &mu) == 0);
    assert(clock_gettime(CLOCK_REALTIME, &deadline) == 0);
    deadline.tv_sec++;
    while (!(write ? attach_crossed : attach_complete) && status == 0)
      status = pthread_cond_timedwait(&cv, &mu, &deadline);
    if (write)
      assert(status == ETIMEDOUT && !attach_crossed);
    else
      assert(status == 0 && attach_complete);
    assert(pthread_mutex_unlock(&mu) == 0);
    return __real_setlocale(category, write ? "C" : NULL);
  }
  return __real_setlocale(category, locale);
}

void __wrap_lj_locale_wait_attach(void)
{
  assert(pthread_mutex_lock(&mu) == 0);
  attach_entered = 1;
  assert(pthread_cond_broadcast(&cv) == 0);
  assert(pthread_mutex_unlock(&mu) == 0);
  __real_lj_locale_wait_attach();
  assert(pthread_mutex_lock(&mu) == 0);
  attach_crossed = 1;
  assert(pthread_cond_broadcast(&cv) == 0);
  assert(pthread_mutex_unlock(&mu) == 0);
}

static void *attach(void *arg)
{
  lua_State *L = (lua_State *)arg;
  assert(pthread_mutex_lock(&mu) == 0);
  while (!writer_entered)
    assert(pthread_cond_wait(&cv, &mu) == 0);
  assert(pthread_mutex_unlock(&mu) == 0);
  assert(lj_threading_attach(L));
  assert(pthread_mutex_lock(&mu) == 0);
  attach_complete = 1;
  assert(pthread_cond_broadcast(&cv) == 0);
  assert(pthread_mutex_unlock(&mu) == 0);
  ljt_lua_assert_ok(L, luaL_dostring(L, "assert(os.setlocale() == 'C')"),
                    "query after attach");
  lj_threading_detach(L, 1);
  return NULL;
}

static void run_attach_race(int write)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  lua_State *child = lua_newthread(L);
  pthread_t peer;
  writer_entered = attach_entered = attach_crossed = attach_complete = 0;
  pause_query = !write;
  assert(pthread_create(&peer, NULL, attach, child) == 0);
  ljt_lua_assert_ok(L, luaL_dostring(L, write ?
    "assert(os.setlocale('test-paused-write') == 'C')" :
    "assert(os.setlocale() == 'C')"), "paused setlocale");
  assert(pthread_join(peer, NULL) == 0);
  assert(attach_crossed);
  lua_close(L);
}

int main(void)
{
  run_attach_race(1);
  run_attach_race(0);
  return 0;
}

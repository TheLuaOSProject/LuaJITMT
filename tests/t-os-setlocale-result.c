/* Verify libc locale names are copied before a separate heap can change them. */

#define _GNU_SOURCE
#include <assert.h>
#include <pthread.h>
#include <string.h>

#include "lib/lua_fixture_helpers.h"

static pthread_barrier_t locale_barrier;
static char locale_name[512];
static _Thread_local int is_reader;
static _Thread_local int pause_push;

char *__real_setlocale(int category, const char *locale);
void __real_lua_pushstring(lua_State *L, const char *str);

static void rendezvous(void)
{
  int status = pthread_barrier_wait(&locale_barrier);
  assert(status == 0 || status == PTHREAD_BARRIER_SERIAL_THREAD);
}

char *__wrap_setlocale(int category, const char *locale)
{
  if (!locale && is_reader) {
    pause_push = 1;
    return locale_name;
  }
  if (locale && strcmp(locale, "test-replace") == 0) {
    memset(locale_name, 'b', sizeof(locale_name)-1);
    return locale_name;
  }
  return __real_setlocale(category, locale);
}

void __wrap_lua_pushstring(lua_State *L, const char *str)
{
  if (pause_push) {
    /* Let the writer replace libc's buffer before Lua interns the result. */
    pause_push = 0;
    rendezvous();
    rendezvous();
  }
  __real_lua_pushstring(L, str);
}

static void *replace_locale(void *unused)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  (void)unused;
  rendezvous();
  ljt_lua_assert_ok(L, luaL_dostring(L,
    "assert(os.setlocale('test-replace') == string.rep('b', 511))"),
    "setlocale writer");
  rendezvous();
  lua_close(L);
  return NULL;
}

int main(void)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  pthread_t writer;
  memset(locale_name, 'a', sizeof(locale_name)-1);
  assert(pthread_barrier_init(&locale_barrier, NULL, 2) == 0);
  assert(pthread_create(&writer, NULL, replace_locale, NULL) == 0);
  is_reader = 1;
  ljt_lua_assert_ok(L, luaL_dostring(L,
    "assert(os.setlocale() == string.rep('a', 511))"), "setlocale reader");
  is_reader = 0;
  assert(pthread_join(writer, NULL) == 0);
  assert(pthread_barrier_destroy(&locale_barrier) == 0);
  lua_close(L);
  return 0;
}

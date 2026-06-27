/*
** Focused guard for concurrent FFI callback/blocking blacklist insertion.
*/

#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_ctype.h"

#include "lib/lua_fixture_helpers.h"

#define CBBLACK_THREADS 8
#define CBBLACK_ITERS 512

static CTState *shared_cts;
static pthread_barrier_t start_barrier;

static int cbblack_target(void)
{
  return 42;
}

static void *cbblack_worker(void *arg)
{
  int i;
  int brc;
  UNUSED(arg);
  brc = pthread_barrier_wait(&start_barrier);
  assert(brc == 0 || brc == PTHREAD_BARRIER_SERIAL_THREAD);
  for (i = 0; i < CBBLACK_ITERS; i++)
    lj_ctype_cb_blacklist(shared_cts, (void *)cbblack_target);
  return NULL;
}

static MSize cbblack_used_slots(CTState *cts)
{
  uint64_t *tab = ctype_cbblack_acq(cts);
  MSize size = ctype_cbblack_size_acq(cts);
  MSize i, used = 0;
  assert(tab != NULL && size > 0);
  for (i = 0; i < size; i++)
    if (ctype_cbblack_slot_acq(tab, i) != 0)
      used++;
  return used;
}

int main(void)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  pthread_t threads[CBBLACK_THREADS];
  MSize i;

  ljt_lua_dostring(L, "require('ffi')");
  shared_cts = ctype_cts(L);
  assert(shared_cts != NULL);

  memset(ctype_cbblack_acq(shared_cts), 0,
	 ctype_cbblack_size_acq(shared_cts) * sizeof(uint64_t));
  ctype_cbblack_all_rel(shared_cts, 0);

  assert(pthread_barrier_init(&start_barrier, NULL, CBBLACK_THREADS) == 0);
  for (i = 0; i < CBBLACK_THREADS; i++)
    assert(pthread_create(&threads[i], NULL, cbblack_worker, NULL) == 0);
  for (i = 0; i < CBBLACK_THREADS; i++)
    assert(pthread_join(threads[i], NULL) == 0);
  assert(pthread_barrier_destroy(&start_barrier) == 0);

  assert(lj_ctype_cb_isblacklisted(shared_cts, (void *)cbblack_target));
  assert(ctype_cbblack_all_acq(shared_cts) == 0);
  assert(cbblack_used_slots(shared_cts) == 1);

  lua_close(L);
  printf("t-ffi-cbblack-race OK: duplicate blacklist publishers share one slot\n");
  return 0;
}

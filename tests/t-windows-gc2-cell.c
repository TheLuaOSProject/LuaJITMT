/*
** Production Windows GC2 admitted-thread-cell regression.
*/

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lj_gc2.h"
#include "lj_obj.h"
#include "lj_thr.h"

#if !LJ_TARGET_WINDOWS
#error "t-windows-gc2-cell requires a Windows target"
#endif

#define GC2_CELL_ERROR ERROR_INVALID_DATA

typedef struct RawReaderCtx {
  global_State *g;
  uint32_t ready;
  uint32_t go;
  uint32_t partial;
} RawReaderCtx;

typedef struct AdmittedReaderCtx {
  global_State *outer;
  global_State *inner;
  LJThrGC2TLS *tls;
  uint32_t *ready;
  uint32_t *go;
  uint32_t *partial;
  DWORD error;
} AdmittedReaderCtx;

static void assert_error(DWORD expected)
{
  DWORD actual = GetLastError();
  if (actual != expected)
    fprintf(stderr, "LastError mismatch: expected %lu, got %lu\n",
	    (unsigned long)expected, (unsigned long)actual);
  assert(actual == expected);
}

static void assert_tls_clear(const LJThrGC2TLS *tls)
{
  assert(tls);
  assert(tls->reclaim_g == NULL);
  assert(tls->idle_transition_gate_g == NULL);
  assert(tls->smr_reader_g == NULL);
  assert(tls->idle_reclaim_gate_owned == 0);
  assert(tls->smr_reader_depth == 0);
}

static void *raw_reader(void *arg)
{
  RawReaderCtx *ctx = (RawReaderCtx *)arg;
  SetLastError(GC2_CELL_ERROR);
  assert(lj_thr_gc2_tls_current() == NULL);
  assert_error(GC2_CELL_ERROR);
  assert(lj_gc2_smr_read_try(ctx->g));
  assert(lj_gc2_smr_read_try(ctx->g));
  assert(lj_thr_gc2_tls_current() == NULL);
  assert_error(GC2_CELL_ERROR);
  la_store32_rel(&ctx->ready, 1);
  while (la_load32_acq(&ctx->go) == 0)
    (void)lj_thr_yield(NULL);
  SetLastError(GC2_CELL_ERROR);
  lj_gc2_smr_read_leave(ctx->g);
  assert_error(GC2_CELL_ERROR);
  la_store32_rel(&ctx->partial, 1);
  while (la_load32_acq(&ctx->go) == 1)
    (void)lj_thr_yield(NULL);
  SetLastError(GC2_CELL_ERROR);
  lj_gc2_smr_read_leave(ctx->g);
  assert_error(GC2_CELL_ERROR);
  SetLastError(GC2_CELL_ERROR);
  assert(lj_thr_gc2_tls_current() == NULL);
  assert_error(GC2_CELL_ERROR);
  return NULL;
}

static void *admitted_reader(void *arg)
{
  AdmittedReaderCtx *ctx = (AdmittedReaderCtx *)arg;
  assert(lj_thr_tg_tls_init());
  SetLastError(ctx->error);
  ctx->tls = lj_thr_gc2_tls_current();
  assert_error(ctx->error);
  assert_tls_clear(ctx->tls);

  /* The outer universe owns one elided count. A different nested universe is
  ** fully counted because the one record cannot represent both identities. */
  assert(lj_gc2_smr_read_try(ctx->outer));
  assert(lj_gc2_smr_read_try(ctx->outer));
  assert(lj_gc2_smr_read_try(ctx->inner));
  assert(lj_gc2_smr_read_try(ctx->inner));
  assert_error(ctx->error);
  (void)la_add32_acqrel(ctx->ready, 1);
  while (la_load32_acq(ctx->go) == 0)
    (void)lj_thr_yield(NULL);

  SetLastError(ctx->error);
  lj_gc2_smr_read_leave(ctx->inner);
  lj_gc2_smr_read_leave(ctx->inner);
  lj_gc2_smr_read_leave(ctx->outer);  /* Nested outer leave keeps its count. */
  assert_error(ctx->error);
  (void)la_add32_acqrel(ctx->partial, 1);
  while (la_load32_acq(ctx->go) == 1)
    (void)lj_thr_yield(NULL);

  SetLastError(ctx->error);
  lj_gc2_smr_read_leave(ctx->outer);
  assert_error(ctx->error);
  SetLastError(ctx->error);
  assert_tls_clear(ctx->tls);
  assert_error(ctx->error);
  return NULL;
}

int main(void)
{
  global_State *outer = (global_State *)calloc(1, sizeof(*outer));
  global_State *inner = (global_State *)calloc(1, sizeof(*inner));
  RawReaderCtx raw;
  AdmittedReaderCtx admitted[2];
  LJThr raw_thr = {0}, admitted_thr[2] = {{0}, {0}};
  LJThrGC2TLS *main_tls;
  uint32_t ready = 0, go = 0, partial = 0;
  unsigned int i;

  assert(outer && inner);
  memset(&raw, 0, sizeof(raw));
  memset(admitted, 0, sizeof(admitted));
  assert(lj_thr_tg_tls_init());  /* Publish the process key, not child cells. */
  SetLastError(GC2_CELL_ERROR);
  assert_error(GC2_CELL_ERROR);
  SetLastError(GC2_CELL_ERROR);
  main_tls = lj_thr_gc2_tls_current();
  assert_error(GC2_CELL_ERROR);
  assert_tls_clear(main_tls);

  raw.g = outer;
  assert(lj_thr_create(&raw_thr, raw_reader, &raw) == 0);
  while (la_load32_acq(&raw.ready) == 0)
    (void)lj_thr_yield(NULL);
  assert(gc2_smr_readers_acq(outer) == 2u);
  la_store32_rel(&raw.go, 1);
  while (la_load32_acq(&raw.partial) == 0)
    (void)lj_thr_yield(NULL);
  assert(gc2_smr_readers_acq(outer) == 1u);
  la_store32_rel(&raw.go, 2);
  assert(lj_thr_join(&raw_thr, NULL) == 0);
  assert(gc2_smr_readers_acq(outer) == 0);

  for (i = 0; i < 2; i++) {
    admitted[i].outer = outer;
    admitted[i].inner = inner;
    admitted[i].ready = &ready;
    admitted[i].go = &go;
    admitted[i].partial = &partial;
    admitted[i].error = GC2_CELL_ERROR + (DWORD)i + 1u;
    assert(lj_thr_create(&admitted_thr[i], admitted_reader,
                         &admitted[i]) == 0);
  }
  while (la_load32_acq(&ready) != 2u)
    (void)lj_thr_yield(NULL);
  assert(admitted[0].tls && admitted[1].tls);
  assert(admitted[0].tls != admitted[1].tls);
  assert(gc2_smr_readers_acq(outer) == 2u);
  assert(gc2_smr_readers_acq(inner) == 4u);
  la_store32_rel(&go, 1);
  while (la_load32_acq(&partial) != 2u)
    (void)lj_thr_yield(NULL);
  assert(gc2_smr_readers_acq(outer) == 2u);
  assert(gc2_smr_readers_acq(inner) == 0);
  la_store32_rel(&go, 2);
  for (i = 0; i < 2; i++)
    assert(lj_thr_join(&admitted_thr[i], NULL) == 0);
  assert(gc2_smr_readers_acq(outer) == 0);
  assert(gc2_smr_readers_acq(inner) == 0);
  assert_tls_clear(admitted[0].tls);
  assert_tls_clear(admitted[1].tls);

  free(inner);
  free(outer);
  puts("t-windows-gc2-cell OK: admitted capabilities and raw SMR fallback verified");
  return 0;
}

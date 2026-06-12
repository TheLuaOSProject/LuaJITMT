/*
** Lockless channel substrate for LuaJIT-MT.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#define lj_chan_c
#define LUA_CORE

#include "lj_obj.h"
#include "lj_atomic.h"
#include "lj_chan.h"
#include "lj_safepoint.h"
#include "lj_tg.h"

#include <errno.h>
#include <limits.h>
#include <time.h>

#define LJ_CHAN_SPINS	64

uint32_t lj_chan_round_capacity(uint32_t capacity)
{
  uint32_t cap = capacity ? capacity : 1u;
  cap--;
  cap |= cap >> 1;
  cap |= cap >> 2;
  cap |= cap >> 4;
  cap |= cap >> 8;
  cap |= cap >> 16;
  cap++;
  return cap ? cap : 1u;
}

MSize lj_chan_memsize(uint32_t capacity)
{
  uint32_t cap = lj_chan_round_capacity(capacity);
  return (MSize)(sizeof(LJChan) + (cap - 1u) * sizeof(LJChanSlot));
}

void lj_chan_init(LJChan *ch, uint32_t capacity)
{
  uint32_t cap = lj_chan_round_capacity(capacity);
  uint32_t i;
  ch->cap = cap;
  ch->mask = cap - 1u;
  ch->rendezvous = capacity == 0;
  ch->closed = 0;
  ch->enq = 0;
  ch->deq = 0;
  ch->futex = 0;
  ch->pad = 0;
  for (i = 0; i < cap; i++) {
    ch->slot[i].seq = i;
    setnilV(&ch->slot[i].tv);
  }
}

static void chan_wake(LJChan *ch)
{
  la_add32_rlx(&ch->futex, 1);
  la_futex_wake(&ch->futex, INT_MAX);
}

static void chan_wait(lua_State *L, LJChan *ch)
{
  uint32_t f = la_load32_acq(&ch->futex);
  TGState *tg = L ? L2TG(L) : NULL;
  uint32_t actions = 0;
  if (tg)
    lj_native_enter(tg);  /* 09 section 9.5: channel park is native. */
  (void)la_futex_wait(&ch->futex, f, 1000000);
  if (L)
    actions = lj_native_leave(L);
  else if (tg)
    la_store8_rlx(&tg->in_native, 0);
  lj_safepoint_checkstop(L, actions);
}

static int64_t chan_now_ns(void)
{
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
    return 0;
  return (int64_t)ts.tv_sec * 1000000000ll + (int64_t)ts.tv_nsec;
}

static int64_t chan_deadline_ns(int64_t ns)
{
  int64_t now = chan_now_ns();
  if (ns > INT64_MAX - now)
    return INT64_MAX;
  return now + ns;
}

static int64_t chan_remaining_ns(int64_t deadline)
{
  int64_t now = chan_now_ns();
  return deadline > now ? deadline - now : 0;
}

static int chan_wait_timeout(lua_State *L, LJChan *ch, int64_t ns)
{
  uint32_t f;
  TGState *tg;
  int rc;
  if (ns <= 0)
    return 1;
  if (ns > 1000000)
    ns = 1000000;
  f = la_load32_acq(&ch->futex);
  tg = L ? L2TG(L) : NULL;
  if (tg)
    lj_native_enter(tg);  /* 09 section 9.5: timed channel park. */
  rc = la_futex_wait(&ch->futex, f, ns);
  if (L)
    lj_safepoint_checkstop(L, lj_native_leave(L));
  else if (tg)
    la_store8_rlx(&tg->in_native, 0);
  return rc != 0 && errno == ETIMEDOUT;
}

static int chan_full_at(LJChan *ch, uint64_t pos)
{
  uint64_t deq = la_load64_acq(&ch->deq);
  return (int64_t)(pos - deq) >= (int64_t)ch->cap;
}

static int chan_try_send_pos(LJChan *ch, cTValue *tv, uint64_t *ppos)
{
  uint64_t pos;
  if (!ch || !tv)
    return LJ_CHAN_CLOSED;
  if (la_load32_acq(&ch->closed))
    return LJ_CHAN_CLOSED;
  pos = la_load64_acq(&ch->enq);
  for (;;) {
    LJChanSlot *slot = &ch->slot[(MSize)(pos & ch->mask)];
    uint64_t seq = la_load64_acq(&slot->seq);
    int64_t dif = (int64_t)(seq - pos);
    if (chan_full_at(ch, pos))
      return LJ_CHAN_FULL;
    if (dif == 0) {
      uint64_t expect = pos;
      if (la_load32_acq(&ch->closed))
	return LJ_CHAN_CLOSED;
      if (la_cas64(&ch->enq, &expect, pos + 1u,
		   LA_ACQ_REL, LA_ACQ)) {  /* 09 section 9.5 enqueue ticket. */
	slot->tv = *tv;
	la_store64_rel(&slot->seq, pos + 1u);
	if (ppos)
	  *ppos = pos;
	chan_wake(ch);
	return LJ_CHAN_OK;
      }
      pos = expect;
    } else if (dif < 0) {
      return LJ_CHAN_FULL;
    } else {
      pos = la_load64_acq(&ch->enq);
    }
  }
}

static int chan_cancel_rendezvous_send(LJChan *ch, uint64_t pos)
{
  uint64_t expect = pos;
  if (la_cas64(&ch->deq, &expect, pos + 1u, LA_ACQ_REL, LA_ACQ)) {
    LJChanSlot *slot = &ch->slot[(MSize)(pos & ch->mask)];
    setnilV(&slot->tv);
    la_store64_rel(&slot->seq, pos + ch->cap);
    chan_wake(ch);
    return 1;
  }
  return 0;
}

static int chan_wait_rendezvous_send(lua_State *L, LJChan *ch, uint64_t pos,
				     int64_t deadline)
{
  uint32_t spins = 0;
  for (;;) {
    int64_t waitns;
    if (la_load64_acq(&ch->deq) > pos || la_load32_acq(&ch->closed))
      return LJ_CHAN_OK;
    if (spins++ < LJ_CHAN_SPINS) {
      la_cpu_pause();
      continue;
    }
    waitns = chan_remaining_ns(deadline);
    if (waitns == 0)
      return chan_cancel_rendezvous_send(ch, pos) ? LJ_CHAN_TIMEOUT :
						    LJ_CHAN_OK;
    (void)chan_wait_timeout(L, ch, waitns);
  }
}

int lj_chan_try_send(LJChan *ch, cTValue *tv)
{
  return chan_try_send_pos(ch, tv, NULL);
}

int lj_chan_try_recv(LJChan *ch, TValue *out)
{
  uint64_t pos;
  if (!ch || !out)
    return LJ_CHAN_CLOSED;
  pos = la_load64_acq(&ch->deq);
  for (;;) {
    LJChanSlot *slot = &ch->slot[(MSize)(pos & ch->mask)];
    uint64_t seq = la_load64_acq(&slot->seq);
    int64_t dif = (int64_t)(seq - (pos + 1u));
    if (dif == 0) {
      uint64_t expect = pos;
      if (la_cas64(&ch->deq, &expect, pos + 1u,
		   LA_ACQ_REL, LA_ACQ)) {  /* 09 section 9.5 dequeue ticket. */
	*out = slot->tv;
	setnilV(&slot->tv);
	la_store64_rel(&slot->seq, pos + ch->cap);
	chan_wake(ch);
	return LJ_CHAN_OK;
      }
      pos = expect;
    } else if (dif < 0) {
      if (la_load32_acq(&ch->closed) && la_load64_acq(&ch->enq) <= pos)
	return LJ_CHAN_CLOSED;
      return LJ_CHAN_EMPTY;
    } else {
      pos = la_load64_acq(&ch->deq);
    }
  }
}

int lj_chan_send(lua_State *L, LJChan *ch, cTValue *tv)
{
  uint32_t spins = 0;
  for (;;) {
    uint64_t pos = 0;
    int rc = chan_try_send_pos(ch, tv, &pos);
    if (rc == LJ_CHAN_OK) {
      if (ch->rendezvous) {
	while (la_load64_acq(&ch->deq) <= pos && !la_load32_acq(&ch->closed))
	  chan_wait(L, ch);
      }
      return LJ_CHAN_OK;
    }
    if (rc == LJ_CHAN_CLOSED)
      return rc;
    if (spins++ < LJ_CHAN_SPINS)
      la_cpu_pause();
    else
      chan_wait(L, ch);
  }
}

int lj_chan_recv(lua_State *L, LJChan *ch, TValue *out)
{
  uint32_t spins = 0;
  for (;;) {
    int rc = lj_chan_try_recv(ch, out);
    if (rc != LJ_CHAN_EMPTY)
      return rc;
    if (spins++ < LJ_CHAN_SPINS)
      la_cpu_pause();
    else
      chan_wait(L, ch);
  }
}

int lj_chan_send_timeout(lua_State *L, LJChan *ch, cTValue *tv, int64_t ns)
{
  uint32_t spins = 0;
  int64_t deadline;
  if (ns < 0)
    return lj_chan_send(L, ch, tv);
  deadline = chan_deadline_ns(ns);
  for (;;) {
    uint64_t pos = 0;
    int64_t waitns;
    int rc;
    rc = chan_try_send_pos(ch, tv, &pos);
    if (rc == LJ_CHAN_OK) {
      if (ch->rendezvous)
	return chan_wait_rendezvous_send(L, ch, pos, deadline);
      return LJ_CHAN_OK;
    }
    if (rc == LJ_CHAN_CLOSED)
      return rc;
    if (spins++ < LJ_CHAN_SPINS) {
      la_cpu_pause();
      continue;
    }
    waitns = chan_remaining_ns(deadline);
    if (waitns == 0)
      return LJ_CHAN_TIMEOUT;
    (void)chan_wait_timeout(L, ch, waitns);
  }
}

int lj_chan_recv_timeout(lua_State *L, LJChan *ch, TValue *out, int64_t ns)
{
  uint32_t spins = 0;
  int64_t deadline;
  if (ns < 0)
    return lj_chan_recv(L, ch, out);
  deadline = chan_deadline_ns(ns);
  for (;;) {
    int64_t waitns;
    int rc = lj_chan_try_recv(ch, out);
    if (rc == LJ_CHAN_OK || rc == LJ_CHAN_CLOSED)
      return rc;
    if (spins++ < LJ_CHAN_SPINS) {
      la_cpu_pause();
      continue;
    }
    waitns = chan_remaining_ns(deadline);
    if (waitns == 0)
      return LJ_CHAN_TIMEOUT;
    (void)chan_wait_timeout(L, ch, waitns);
  }
}

int lj_chan_peek(LJChan *ch, TValue *out)
{
  uint64_t pos;
  if (!ch || !out)
    return LJ_CHAN_CLOSED;
  pos = la_load64_acq(&ch->deq);
  for (;;) {
    LJChanSlot *slot = &ch->slot[(MSize)(pos & ch->mask)];
    uint64_t seq = la_load64_acq(&slot->seq);
    int64_t dif = (int64_t)(seq - (pos + 1u));
    if (dif == 0) {
      *out = slot->tv;  /* 09 section 9.5: acquire seq publishes value. */
      return LJ_CHAN_OK;
    } else if (dif < 0) {
      if (la_load32_acq(&ch->closed) && la_load64_acq(&ch->enq) <= pos)
	return LJ_CHAN_CLOSED;
      return LJ_CHAN_EMPTY;
    } else {
      pos = la_load64_acq(&ch->deq);
    }
  }
}

void lj_chan_close(LJChan *ch)
{
  if (ch) {
    la_store32_rel(&ch->closed, 1);
    chan_wake(ch);
  }
}

int lj_chan_closed(LJChan *ch)
{
  return !ch || la_load32_acq(&ch->closed);
}

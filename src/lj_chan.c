/*
** Lockless channel substrate for LuaJIT-MT.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#define lj_chan_c
#define LUA_CORE

#include "lj_obj.h"
#include "lj_atomic.h"
#include "lj_chan.h"
#include "lj_gc.h"
#include "lj_state.h"
#include "lj_safepoint.h"
#include "lj_thr.h"
#include "lj_tg.h"

#include <errno.h>
#include <limits.h>

static LJ_AINLINE void chan_storetv_rel(LJChanSlot *slot, cTValue *tv)
{
  tv_rawstore_rel(&slot->tv, tv_rawload(tv));
}

static LJ_AINLINE void chan_loadtv_acq(TValue *out, LJChanSlot *slot)
{
  tv_rawstore_rel(out, tv_rawload_acq(&slot->tv));
}

static LJ_AINLINE void chan_cleartv_rel(LJChanSlot *slot)
{
  tv_rawstore_rel(&slot->tv, ~(uint64_t)0);
}

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
    chan_cleartv_rel(&ch->slot[i]);
  }
}

static void chan_wake_n(LJChan *ch, int n)
{
  la_add32_rlx(&ch->futex, 1);
  la_futex_wake(&ch->futex, n);
}

static void chan_wake_one(LJChan *ch)
{
  chan_wake_n(ch, 1);
}

static void chan_wake_all(LJChan *ch)
{
  chan_wake_n(ch, INT_MAX);
}

static int chan_had_stopreq(lua_State *L)
{
  TGState *tg = L ? L2TG(L) : NULL;
  return tg && lj_tg_flags_test_acq(tg, TGF_STOPREQ);
}

static int chan_pending_stopreq(lua_State *L)
{
  TGState *tg = L ? L2TG(L) : NULL;
  return tg && (lj_tg_reqmask_acq(tg) & LJ_GC2_HS_STOPREQ);
}

static uint32_t chan_poll_pending_stopreq(lua_State *L, uint32_t actions)
{
  if (!(actions & LJ_GC2_HS_STOPREQ) && chan_pending_stopreq(L))
    actions |= lj_safepoint_poll(L);
  return actions;
}

static int chan_fresh_stopreq(lua_State *L, uint32_t actions, int had_stopreq)
{
  return lj_safepoint_fresh_stopreq(L, actions, had_stopreq);
}

static void chan_checkstop_fresh(lua_State *L, uint32_t actions,
				 int had_stopreq)
{
  actions = chan_poll_pending_stopreq(L, actions);
  if (chan_fresh_stopreq(L, actions, had_stopreq))
    lj_safepoint_checkstop(L, actions);
}

static void chan_wait(lua_State *L, LJChan *ch)
{
  uint32_t f = la_load32_acq(&ch->futex);
  TGState *tg = L ? L2TG(L) : NULL;
  uint32_t actions = 0;
  int had_stopreq = chan_had_stopreq(L);
  if (tg)
    lj_native_enter(tg);  /* 09 section 9.5: channel park is native. */
  (void)la_futex_wait(&ch->futex, f, 1000000);
  if (L)
    actions = lj_native_leave(L);
  else if (tg)
    lj_tg_in_native_store_rlx(tg, 0);
  chan_checkstop_fresh(L, actions, had_stopreq);
}

static int64_t chan_now_ns(void)
{
  return (int64_t)lj_thr_now_ns();
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
  uint32_t actions = 0;
  int had_stopreq = chan_had_stopreq(L);
  int rc;
  if (ns <= 0)
    return 1;
  if (ns > 1000000)
    ns = 1000000;
  f = la_load32_acq(&ch->futex);
  tg = L ? L2TG(L) : NULL;
  if (L)
    lj_state_stack_pubrange(L, L);
  if (tg)
    lj_native_enter(tg);  /* 09 section 9.5: timed channel park. */
  rc = la_futex_wait(&ch->futex, f, ns);
  if (L)
    actions = lj_native_leave(L);
  else if (tg)
    lj_tg_in_native_store_rlx(tg, 0);
  chan_checkstop_fresh(L, actions, had_stopreq);
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
	chan_storetv_rel(slot, tv);
	la_store64_rel(&slot->seq, pos + 1u);
	if (ppos)
	  *ppos = pos;
	if (ch->rendezvous)
	  chan_wake_all(ch);
	else
	  chan_wake_one(ch);
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
    chan_cleartv_rel(slot);
    la_store64_rel(&slot->seq, pos + ch->cap);
    chan_wake_all(ch);
    return 1;
  }
  return 0;
}

static int chan_rendezvous_send_status(LJChan *ch, uint64_t pos)
{
  if (la_load64_acq(&ch->deq) > pos)
    return LJ_CHAN_OK;
  if (la_load32_acq(&ch->closed))
    return chan_cancel_rendezvous_send(ch, pos) ? LJ_CHAN_CLOSED :
						  LJ_CHAN_OK;
  return LJ_CHAN_FULL;
}

static int chan_wait_rendezvous_send(lua_State *L, LJChan *ch, uint64_t pos,
				     int64_t deadline)
{
  for (;;) {
    int rc = chan_rendezvous_send_status(ch, pos);
    int64_t waitns;
    if (rc != LJ_CHAN_FULL)
      return rc;
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

static int chan_try_recv_pub(lua_State *L, LJChan *ch, TValue *out)
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
	chan_loadtv_acq(out, slot);
	if (L)
	  lj_gc_pubroot(L, out);
	chan_cleartv_rel(slot);
	la_store64_rel(&slot->seq, pos + ch->cap);
	if (ch->rendezvous)
	  chan_wake_all(ch);
	else
	  chan_wake_one(ch);
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

int lj_chan_try_recv(LJChan *ch, TValue *out)
{
  return chan_try_recv_pub(NULL, ch, out);
}

int lj_chan_try_recv_gc(lua_State *L, LJChan *ch, TValue *out)
{
  return chan_try_recv_pub(L, ch, out);
}

int lj_chan_send(lua_State *L, LJChan *ch, cTValue *tv)
{
  for (;;) {
    uint64_t pos = 0;
    int rc = chan_try_send_pos(ch, tv, &pos);
    if (rc == LJ_CHAN_OK) {
      if (ch->rendezvous) {
	while ((rc = chan_rendezvous_send_status(ch, pos)) == LJ_CHAN_FULL)
	  chan_wait(L, ch);
      }
      return rc;
    }
    if (rc == LJ_CHAN_CLOSED)
      return rc;
    chan_wait(L, ch);
  }
}

int lj_chan_recv(lua_State *L, LJChan *ch, TValue *out)
{
  for (;;) {
    int rc = chan_try_recv_pub(NULL, ch, out);
    if (rc != LJ_CHAN_EMPTY)
      return rc;
    chan_wait(L, ch);
  }
}

int lj_chan_recv_gc(lua_State *L, LJChan *ch, TValue *out)
{
  for (;;) {
    int rc = chan_try_recv_pub(L, ch, out);
    if (rc != LJ_CHAN_EMPTY)
      return rc;
    chan_wait(L, ch);
  }
}

int lj_chan_send_timeout(lua_State *L, LJChan *ch, cTValue *tv, int64_t ns)
{
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
    waitns = chan_remaining_ns(deadline);
    if (waitns == 0)
      return LJ_CHAN_TIMEOUT;
    (void)chan_wait_timeout(L, ch, waitns);
  }
}

int lj_chan_recv_timeout(lua_State *L, LJChan *ch, TValue *out, int64_t ns)
{
  int64_t deadline;
  if (ns < 0)
    return lj_chan_recv(L, ch, out);
  deadline = chan_deadline_ns(ns);
  for (;;) {
    int64_t waitns;
    int rc = chan_try_recv_pub(NULL, ch, out);
    if (rc == LJ_CHAN_OK || rc == LJ_CHAN_CLOSED)
      return rc;
    waitns = chan_remaining_ns(deadline);
    if (waitns == 0)
      return LJ_CHAN_TIMEOUT;
    (void)chan_wait_timeout(L, ch, waitns);
  }
}

int lj_chan_recv_timeout_gc(lua_State *L, LJChan *ch, TValue *out, int64_t ns)
{
  int64_t deadline;
  if (ns < 0)
    return lj_chan_recv_gc(L, ch, out);
  deadline = chan_deadline_ns(ns);
  for (;;) {
    int64_t waitns;
    int rc = chan_try_recv_pub(L, ch, out);
    if (rc == LJ_CHAN_OK || rc == LJ_CHAN_CLOSED)
      return rc;
    waitns = chan_remaining_ns(deadline);
    if (waitns == 0)
      return LJ_CHAN_TIMEOUT;
    (void)chan_wait_timeout(L, ch, waitns);
  }
}

int lj_chan_peek_gc(lua_State *L, LJChan *ch, TValue *out)
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
      TValue snap;
      uint64_t raw;
      chan_loadtv_acq(&snap, slot);  /* 09 section 9.5: acquire seq publishes value. */
      raw = tv_rawload(&snap);
      if (L)
	lj_gc_pubroot(L, &snap);
      if (la_load64_acq(&ch->deq) == pos &&
	  la_load64_acq(&slot->seq) == seq &&
	  tv_rawload_acq(&slot->tv) == raw) {
	tv_rawstore_rel(out, raw);
	return LJ_CHAN_OK;
      }
      pos = la_load64_acq(&ch->deq);
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
    chan_wake_all(ch);
  }
}

int lj_chan_closed(LJChan *ch)
{
  return !ch || la_load32_acq(&ch->closed);
}

/*
** Lockless channel substrate for LuaJIT-MT.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#ifndef _LJ_CHAN_H
#define _LJ_CHAN_H

#include "lj_obj.h"

#define LJ_CHAN_OK		0
#define LJ_CHAN_CLOSED		1
#define LJ_CHAN_FULL		2
#define LJ_CHAN_EMPTY		3

typedef struct LJChanSlot {
  LJ_ALIGN(16) uint64_t seq;
  TValue tv;
} LJChanSlot;

typedef struct LJChan {
  uint32_t cap;
  uint32_t mask;
  uint32_t rendezvous;
  uint32_t closed;
  uint64_t enq;
  uint64_t deq;
  uint32_t futex;
  uint32_t pad;
  LJChanSlot slot[1];
} LJChan;

LJ_FUNC uint32_t lj_chan_round_capacity(uint32_t capacity);
LJ_FUNC MSize lj_chan_memsize(uint32_t capacity);
LJ_FUNC void lj_chan_init(LJChan *ch, uint32_t capacity);
LJ_FUNC int lj_chan_try_send(LJChan *ch, cTValue *tv);
LJ_FUNC int lj_chan_try_recv(LJChan *ch, TValue *out);
LJ_FUNC int lj_chan_send(lua_State *L, LJChan *ch, cTValue *tv);
LJ_FUNC int lj_chan_recv(lua_State *L, LJChan *ch, TValue *out);
LJ_FUNC void lj_chan_close(LJChan *ch);
LJ_FUNC int lj_chan_closed(LJChan *ch);

#endif

/*
** Stack-local OS error-state snapshots.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#ifndef _LJ_OSERR_H
#define _LJ_OSERR_H

#include <errno.h>

#include "lj_obj.h"

#if LJ_TARGET_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

typedef struct LJOSerrState {
  int errnum;
  uint32_t winerr;
} LJOSerrState;

LJ_STATIC_ASSERT(sizeof(int) == sizeof(uint32_t));

static LJ_AINLINE void lj_oserr_save(LJOSerrState *err)
{
#if LJ_TARGET_WINDOWS
  err->winerr = (uint32_t)GetLastError();
#else
  err->winerr = 0;
#endif
  err->errnum = errno;
}

static LJ_AINLINE void lj_oserr_restore(const LJOSerrState *err)
{
  errno = err->errnum;
#if LJ_TARGET_WINDOWS
  SetLastError((DWORD)err->winerr);
#endif
}

/* A single x64 context register/stack slot can carry the complete pair. */
static LJ_AINLINE uint64_t lj_oserr_pack(const LJOSerrState *err)
{
  return (uint64_t)(uint32_t)err->errnum |
         ((uint64_t)err->winerr << 32);
}

static LJ_AINLINE void lj_oserr_unpack(LJOSerrState *err, uint64_t packed)
{
  err->errnum = (int)(int32_t)(uint32_t)packed;
  err->winerr = (uint32_t)(packed >> 32);
}

#endif

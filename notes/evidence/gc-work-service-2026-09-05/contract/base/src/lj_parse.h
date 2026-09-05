/*
** Lua parser (source code -> bytecode).
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#ifndef _LJ_PARSE_H
#define _LJ_PARSE_H

#include "lj_obj.h"
#include "lj_lex.h"

/* Returns with the top-level prototype retained in the TG anchor at
** *anchoridx. The loader must keep or replace that exact anchor through its
** function/stack handoff, then pop it. */
LJ_FUNC GCproto *lj_parse(LexState *ls, uint32_t *anchoridx);
LJ_FUNC GCstr *lj_parse_keepstr(LexState *ls, const char *str, size_t l);
#if LJ_HASFFI
LJ_FUNC void lj_parse_keepcdata(LexState *ls, TValue *tv, GCcdata *cd);
#endif

#endif

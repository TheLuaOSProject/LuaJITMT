/*
** Shared helpers for C fixtures that manipulate TG STOPREQ state.
*/

#ifndef TESTS_LIB_TG_STOPREQ_FIXTURE_HELPERS_H
#define TESTS_LIB_TG_STOPREQ_FIXTURE_HELPERS_H

#include <stdint.h>

#include "lj_tg.h"

static inline void ljt_tg_set_stopreq(TGState *tg)
{
  (void)lj_tg_flags_or_rlx(tg, TGF_STOPREQ);
}

static inline void ljt_tg_clear_stopreq(TGState *tg)
{
  (void)lj_tg_flags_and_rlx(tg, (uint8_t)~(TGF_STOPREQ|TGF_STOPREQ_FRESH));
}

static inline int ljt_tg_has_stopreq(TGState *tg)
{
  return lj_tg_flags_test_acq(tg, TGF_STOPREQ) != 0;
}

#endif

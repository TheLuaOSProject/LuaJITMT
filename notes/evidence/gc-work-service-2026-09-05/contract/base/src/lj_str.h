/*
** String handling.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#ifndef _LJ_STR_H
#define _LJ_STR_H

#include <stdarg.h>

#include "lj_obj.h"

#define LJ_STRHASH_DEAD		((uintptr_t)1)
#define LJ_STRHASH_SECONDARY	((uintptr_t)2)
#define LJ_STRHASH_LINKMASK	(LJ_STRHASH_DEAD|LJ_STRHASH_SECONDARY)

/* Per-body canonical lifecycle. Record-carrying states use the low three
** alignment bits of an authoritative StrCanonRec pointer. */
#define LJ_STR_CANON_STATE_MASK	((uintptr_t)7u)
enum {
  LJ_STR_CANON_LIVE = 0,
  LJ_STR_CANON_CANDIDATE = 1,
  LJ_STR_CANON_QACTIVE = 2,
  LJ_STR_CANON_QRESCUED = 3,
  LJ_STR_CANON_QCLOSING = 4,
  LJ_STR_CANON_QCOMMIT = 5,
  LJ_STR_CANON_FREEING = 6
};

#define LJ_STR_CANONREC_Q_LINKED	0x01u
#define LJ_STR_CANONREC_BODY_OWNED	0x02u
#define LJ_STR_CANONREC_CANCELLED	0x04u
#define LJ_STR_CANONREC_LIST_ONLY	0x08u

#define LJ_STR_CANON_MINMASK	63u

static LJ_AINLINE uintptr_t lj_str_canon_acq(const GCstr *s)
{
  return la_loaduptr_acq(&s->canon);
}

static LJ_AINLINE void lj_str_canon_store_rlx(GCstr *s, uintptr_t canon)
{
  la_storeuptr_rlx(&s->canon, canon);
}

static LJ_AINLINE void lj_str_canon_rel(GCstr *s, uintptr_t canon)
{
  la_storeuptr_rel(&s->canon, canon);
}

static LJ_AINLINE int lj_str_canon_cas(GCstr *s, uintptr_t *oldp,
				       uintptr_t canon)
{
  return la_casuptr(&s->canon, oldp, canon, LA_ACQ_REL, LA_ACQ);
}

static LJ_AINLINE uint32_t lj_str_canon_state(uintptr_t canon)
{
  return (uint32_t)(canon & LJ_STR_CANON_STATE_MASK);
}

static LJ_AINLINE StrCanonRec *lj_str_canon_record(uintptr_t canon)
{
  return (StrCanonRec *)(void *)(canon & ~(uintptr_t)LJ_STR_CANON_STATE_MASK);
}

static LJ_AINLINE uintptr_t lj_str_canon_pack(StrCanonRec *rec,
				       uint32_t state)
{
  lj_assertX(state >= LJ_STR_CANON_QACTIVE && state <= LJ_STR_CANON_FREEING,
	     "recordless canonical state");
  lj_assertX(((uintptr_t)rec & LJ_STR_CANON_STATE_MASK) == 0,
	     "unaligned canonical string record");
  return (uintptr_t)rec | (uintptr_t)state;
}

/*
** Runtime GCstr unlink/free remains safety-gated until every sweep-time string
** publication can atomically defeat retirement and every native raw-byte borrow
** is represented by a quiescence/hazard contract. The implementation below is
** retained for focused development, but production builds must not enter it.
*/
#define LJ_GC2_STRING_BODY_RECLAIM 0

/* Bounded GC2 string-table subphase. */
enum {
  LJ_STR_SWEEP_IDLE,
  LJ_STR_SWEEP_ACQUIRE,
  LJ_STR_SWEEP_TAG,
  LJ_STR_SWEEP_TAG_GRACE,
  LJ_STR_SWEEP_UNLINK,
  LJ_STR_SWEEP_UNLINK_GRACE,
  LJ_STR_SWEEP_DONE
};

#define lj_str_hashhead_u(u) \
  ((GCobj *)(void *)((u) & ~(uintptr_t)LJ_STRHASH_LINKMASK))
#define lj_str_tabsize(mask) \
  ((mask) == ~(MSize)0 ? (GCSize)0 : \
   (GCSize)offsetof(StrTabHdr, bucket) + \
   (((GCSize)(mask) + 1u) * (GCSize)sizeof(GCRef)))
#define lj_str_tabbytes(tabh) \
  ((tabh) ? lj_str_tabsize((tabh)->mask) : (GCSize)0)
#define lj_str_qtabsize(mask) \
  ((mask) == ~(MSize)0 ? (GCSize)0 : \
   (GCSize)offsetof(StrCanonHdr, bucket) + \
   (((GCSize)(mask) + 1u) * (GCSize)sizeof(StrCanonRec *)))
#define lj_str_qtabbytes(tabh) \
  ((tabh) ? lj_str_qtabsize((tabh)->mask) : (GCSize)0)

static LJ_AINLINE StrTabHdr *lj_str_tabh_acq(const global_State *g)
{
  return (StrTabHdr *)la_loadptr_acq((void *const *)&g->str.tabh);
}

static LJ_AINLINE StrCanonHdr *lj_str_qtabh_acq(const global_State *g)
{
  return (StrCanonHdr *)la_loadptr_acq((void *const *)&g->str.qtabh);
}

static LJ_AINLINE void lj_str_qtabh_store_rlx(global_State *g,
				       StrCanonHdr *hdr)
{
  la_storeptr_rlx((void **)&g->str.qtabh, hdr);
}

static LJ_AINLINE void lj_str_qtabh_rel(global_State *g, StrCanonHdr *hdr)
{
  la_storeptr_rel((void **)&g->str.qtabh, hdr);
}

static LJ_AINLINE StrCanonHdr *lj_str_qtabh_xchg_acqrel(global_State *g,
						 StrCanonHdr *hdr)
{
  return (StrCanonHdr *)la_xchgptr_acqrel((void **)&g->str.qtabh, hdr);
}

static LJ_AINLINE MSize lj_str_qmask_acq(const global_State *g)
{
  return (MSize)la_load32_acq(&g->str.qmask);
}

static LJ_AINLINE void lj_str_qmask_store_rlx(global_State *g, MSize mask)
{
  la_store32_rlx(&g->str.qmask, mask);
}

static LJ_AINLINE void lj_str_qmask_rel(global_State *g, MSize mask)
{
  la_store32_rel(&g->str.qmask, mask);
}

static LJ_AINLINE MSize lj_str_qcount_acq(const global_State *g)
{
  return (MSize)la_load32_acq(&g->str.qcount);
}

static LJ_AINLINE void lj_str_qcount_store_rlx(global_State *g, MSize n)
{
  la_store32_rlx(&g->str.qcount, (uint32_t)n);
}

/* Stage A never removes an authoritative quarantine record. Keep this count
** exact until saturation and permanently non-zero afterwards: zero is a
** correctness fast-path, so wrapping through zero would permit a duplicate
** main-table publication. Later Q-unlink stages need a separate exact live
** count instead of decrementing a saturated value. */
static LJ_AINLINE MSize lj_str_qcount_inc_sat_acqrel(global_State *g)
{
  uint32_t old = la_load32_acq(&g->str.qcount);
  while (old != UINT32_MAX) {
    uint32_t next = old + 1u;
    if (la_cas32(&g->str.qcount, &old, next, LA_ACQ_REL, LA_ACQ))
      break;
  }
  return (MSize)old;
}

static LJ_AINLINE StrCanonRec *lj_str_qbucket_acq(StrCanonRec *const *bucket)
{
  return (StrCanonRec *)la_loadptr_acq((void *const *)bucket);
}

static LJ_AINLINE void lj_str_qbucket_rel(StrCanonRec **bucket,
					   StrCanonRec *rec)
{
  la_storeptr_rel((void **)bucket, rec);
}

static LJ_AINLINE int lj_str_qbucket_cas(StrCanonRec **bucket,
					  StrCanonRec **oldp,
					  StrCanonRec *rec)
{
  return la_casptr((void **)bucket, (void **)oldp, rec,
		   LA_ACQ_REL, LA_ACQ);
}

static LJ_AINLINE StrCanonRec *lj_str_qnext_acq(const StrCanonRec *rec)
{
  return (StrCanonRec *)la_loadptr_acq((void *const *)&rec->qnext);
}

static LJ_AINLINE void lj_str_qnext_rel(StrCanonRec *rec,
					 StrCanonRec *next)
{
  la_storeptr_rel((void **)&rec->qnext, next);
}

static LJ_AINLINE StrCanonHdr *lj_str_qretired_head_acq(
  const global_State *g)
{
  return (StrCanonHdr *)la_loadptr_acq((void *const *)&g->str.qretired);
}

static LJ_AINLINE void lj_str_qretired_head_store_rlx(global_State *g,
						       StrCanonHdr *hdr)
{
  la_storeptr_rlx((void **)&g->str.qretired, hdr);
}

static LJ_AINLINE int lj_str_qretired_head_cas(global_State *g,
						StrCanonHdr **oldp,
						StrCanonHdr *hdr)
{
  return la_casptr((void **)&g->str.qretired, (void **)oldp, hdr,
		   LA_ACQ_REL, LA_ACQ);
}

static LJ_AINLINE StrCanonHdr *lj_str_qretired_head_xchg_acqrel(
  global_State *g, StrCanonHdr *hdr)
{
  return (StrCanonHdr *)la_xchgptr_acqrel((void **)&g->str.qretired, hdr);
}

static LJ_AINLINE uint64_t lj_str_qretire_epoch_acq(
  const StrCanonHdr *hdr)
{
  return la_load64_acq(&hdr->retire_epoch);
}

static LJ_AINLINE void lj_str_qretire_epoch_rel(StrCanonHdr *hdr,
						 uint64_t epoch)
{
  la_store64_rel(&hdr->retire_epoch, epoch);
}

static LJ_AINLINE StrCanonHdr *lj_str_qretired_next_acq(
  const StrCanonHdr *hdr)
{
  return (StrCanonHdr *)la_loadptr_acq((void *const *)&hdr->retired_next);
}

static LJ_AINLINE void lj_str_qretired_next_rel(StrCanonHdr *hdr,
						 StrCanonHdr *next)
{
  la_storeptr_rel((void **)&hdr->retired_next, next);
}

static LJ_AINLINE void lj_str_tabh_store_rlx(global_State *g, StrTabHdr *hdr)
{
  la_storeptr_rlx((void **)&g->str.tabh, hdr);
}

static LJ_AINLINE void lj_str_tabh_rel(global_State *g, StrTabHdr *hdr)
{
  /* 06 section 6.5: RCU string table publish. */
  la_storeptr_rel((void **)&g->str.tabh, hdr);
}

static LJ_AINLINE MSize lj_str_mask_acq(const global_State *g)
{
  return la_load32_acq(&g->str.mask);
}

static LJ_AINLINE void lj_str_mask_store_rlx(global_State *g, MSize mask)
{
  la_store32_rlx(&g->str.mask, mask);
}

static LJ_AINLINE void lj_str_mask_rel(global_State *g, MSize mask)
{
  /*
  ** The header remains the authoritative string-table snapshot. This mirror is
  ** retained for existing fast paths and diagnostics, so update it atomically with
  ** the same publication discipline as the header replacement.
  */
  la_store32_rel(&g->str.mask, mask);
}

static LJ_AINLINE MSize lj_str_num_acq(const global_State *g)
{
  return (MSize)la_load32_acq(&g->str.num);
}

static LJ_AINLINE MSize lj_str_num_add_rlx(global_State *g, MSize n)
{
  return (MSize)la_add32_rlx(&g->str.num, (uint32_t)n);
}

static LJ_AINLINE MSize lj_str_num_sub_acqrel(global_State *g, MSize n)
{
  return (MSize)la_sub32_acqrel(&g->str.num, (uint32_t)n);
}

static LJ_AINLINE StrID lj_str_id_add_rlx(global_State *g, StrID n)
{
  return (StrID)la_add32_rlx(&g->str.id, n);
}

static LJ_AINLINE uint8_t lj_str_second_acq(const global_State *g)
{
  return la_load8_acq(&g->str.second);
}

static LJ_AINLINE void lj_str_second_rel(global_State *g, uint8_t second)
{
  la_store8_rel(&g->str.second, second);
}

static LJ_AINLINE StrTabHdr *lj_str_tabh_xchg_acqrel(global_State *g,
						     StrTabHdr *hdr)
{
  return (StrTabHdr *)la_xchgptr_acqrel((void **)&g->str.tabh, hdr);
}

static LJ_AINLINE StrTabHdr *lj_str_retired_head_acq(const global_State *g)
{
  return (StrTabHdr *)la_loadptr_acq((void *const *)&g->str.retired);
}

static LJ_AINLINE void lj_str_retired_head_store_rlx(global_State *g,
						     StrTabHdr *hdr)
{
  la_storeptr_rlx((void **)&g->str.retired, hdr);
}

static LJ_AINLINE int lj_str_retired_head_cas(global_State *g,
					     StrTabHdr **oldp,
					     StrTabHdr *hdr)
{
  return la_casptr((void **)&g->str.retired, (void **)oldp, hdr,
		   LA_ACQ_REL, LA_ACQ);
}

static LJ_AINLINE StrTabHdr *lj_str_retired_head_xchg_acqrel(global_State *g,
							     StrTabHdr *hdr)
{
  return (StrTabHdr *)la_xchgptr_acqrel((void **)&g->str.retired, hdr);
}

static LJ_AINLINE uint64_t lj_str_retire_epoch_acq(const StrTabHdr *hdr)
{
  return la_load64_acq(&hdr->retire_epoch);
}

static LJ_AINLINE void lj_str_retire_epoch_rel(StrTabHdr *hdr, uint64_t epoch)
{
  la_store64_rel(&hdr->retire_epoch, epoch);
}

static LJ_AINLINE StrTabHdr *lj_str_retired_next_acq(const StrTabHdr *hdr)
{
  return (StrTabHdr *)la_loadptr_acq((void *const *)&hdr->retired_next);
}

static LJ_AINLINE void lj_str_retired_next_rel(StrTabHdr *hdr,
					       StrTabHdr *next)
{
  la_storeptr_rel((void **)&hdr->retired_next, next);
}

static LJ_AINLINE StrBodyRetire *lj_str_body_retired_head_acq(
  const global_State *g)
{
  return (StrBodyRetire *)la_loadptr_acq(
    (void *const *)&g->str.retired_body);
}

static LJ_AINLINE int lj_str_body_retired_head_cas(global_State *g,
						   StrBodyRetire **oldp,
						   StrBodyRetire *ret)
{
  return la_casptr((void **)&g->str.retired_body, (void **)oldp, ret,
		   LA_ACQ_REL, LA_ACQ);
}

static LJ_AINLINE StrBodyRetire *lj_str_body_retired_head_xchg_acqrel(
  global_State *g, StrBodyRetire *ret)
{
  return (StrBodyRetire *)la_xchgptr_acqrel(
    (void **)&g->str.retired_body, ret);
}

static LJ_AINLINE StrBodyRetire *lj_str_body_retired_next_acq(
  const StrBodyRetire *ret)
{
  return (StrBodyRetire *)la_loadptr_acq((void *const *)&ret->next);
}

static LJ_AINLINE void lj_str_body_retired_next_rel(StrBodyRetire *ret,
						    StrBodyRetire *next)
{
  la_storeptr_rel((void **)&ret->next, next);
}

static LJ_AINLINE GCstr *lj_str_body_retired_str_acq(
  const StrBodyRetire *ret)
{
  return (GCstr *)la_loadptr_acq((void *const *)&ret->str);
}

static LJ_AINLINE void lj_str_body_retired_str_rel(StrBodyRetire *ret,
						   GCstr *str)
{
  la_storeptr_rel((void **)&ret->str, str);
}

static LJ_AINLINE StrTabHdr *lj_str_body_retired_hdr_acq(
  const StrBodyRetire *ret)
{
  return (StrTabHdr *)la_loadptr_acq((void *const *)&ret->hdr);
}

static LJ_AINLINE void lj_str_body_retired_hdr_rel(StrBodyRetire *ret,
						   StrTabHdr *hdr)
{
  la_storeptr_rel((void **)&ret->hdr, hdr);
}

static LJ_AINLINE uint64_t lj_str_body_retired_epoch_acq(
  const StrBodyRetire *ret)
{
  return la_load64_acq(&ret->retire_epoch);
}

static LJ_AINLINE void lj_str_body_retired_epoch_rel(StrBodyRetire *ret,
						     uint64_t epoch)
{
  la_store64_rel(&ret->retire_epoch, epoch);
}

static LJ_AINLINE uint32_t lj_str_body_retired_main_linked_acq(
  const StrBodyRetire *ret)
{
  return la_load32_acq(&ret->main_linked);
}

static LJ_AINLINE void lj_str_body_retired_main_linked_rel(
  StrBodyRetire *ret, uint32_t linked)
{
  la_store32_rel(&ret->main_linked, linked);
}

static LJ_AINLINE StrBodyRetire *lj_str_sweep_pending_acq(
  const global_State *g)
{
  return (StrBodyRetire *)la_loadptr_acq(
    (void *const *)&g->str.sweep_pending);
}

static LJ_AINLINE void lj_str_sweep_pending_rel(global_State *g,
						StrBodyRetire *ret)
{
  la_storeptr_rel((void **)&g->str.sweep_pending, ret);
}

static LJ_AINLINE StrRetireBatch *lj_str_retired_batch_head_acq(
  const global_State *g)
{
  return (StrRetireBatch *)la_loadptr_acq(
    (void *const *)&g->str.retired_batch);
}

static LJ_AINLINE int lj_str_retired_batch_head_cas(global_State *g,
						     StrRetireBatch **oldp,
						     StrRetireBatch *batch)
{
  return la_casptr((void **)&g->str.retired_batch, (void **)oldp, batch,
		   LA_ACQ_REL, LA_ACQ);
}

static LJ_AINLINE StrRetireBatch *lj_str_retired_batch_head_xchg_acqrel(
  global_State *g, StrRetireBatch *batch)
{
  return (StrRetireBatch *)la_xchgptr_acqrel(
    (void **)&g->str.retired_batch, batch);
}

static LJ_AINLINE StrRetireBatch *lj_str_sweep_batch_acq(
  const global_State *g)
{
  return (StrRetireBatch *)la_loadptr_acq(
    (void *const *)&g->str.sweep_batch);
}

static LJ_AINLINE void lj_str_sweep_batch_rel(global_State *g,
					       StrRetireBatch *batch)
{
  la_storeptr_rel((void **)&g->str.sweep_batch, batch);
}

static LJ_AINLINE uint32_t lj_str_sweep_batch_pending_acq(
  const global_State *g)
{
  return la_load32_acq(&g->str.sweep_batch_pending);
}

static LJ_AINLINE void lj_str_sweep_batch_pending_rel(global_State *g,
						       uint32_t pending)
{
  la_store32_rel(&g->str.sweep_batch_pending, pending);
}

static LJ_AINLINE uint32_t lj_str_reclaim_requested_acq(
  const global_State *g)
{
  return la_load32_acq(&g->str.reclaim_requested);
}

static LJ_AINLINE void lj_str_reclaim_requested_rel(global_State *g,
						     uint32_t requested)
{
  la_store32_rel(&g->str.reclaim_requested, requested);
}

static LJ_AINLINE uint32_t lj_str_reclaim_exclusive_acq(
  const global_State *g)
{
  return la_load32_acq(&g->str.reclaim_exclusive);
}

static LJ_AINLINE int lj_str_reclaim_exclusive_cas(global_State *g,
						     uint32_t *oldp,
						     uint32_t exclusive)
{
  return la_cas32(&g->str.reclaim_exclusive, oldp, exclusive,
		   LA_ACQ_REL, LA_ACQ);
}

static LJ_AINLINE void lj_str_reclaim_exclusive_rel(global_State *g,
						     uint32_t exclusive)
{
  la_store32_rel(&g->str.reclaim_exclusive, exclusive);
}

static LJ_AINLINE StrTabHdr *lj_str_sweep_hdr_acq(const global_State *g)
{
  return (StrTabHdr *)la_loadptr_acq((void *const *)&g->str.sweep_hdr);
}

static LJ_AINLINE void lj_str_sweep_hdr_rel(global_State *g, StrTabHdr *hdr)
{
  la_storeptr_rel((void **)&g->str.sweep_hdr, hdr);
}

static LJ_AINLINE GCRef *lj_str_sweep_link_acq(const global_State *g)
{
  return (GCRef *)la_loadptr_acq((void *const *)&g->str.sweep_link);
}

static LJ_AINLINE void lj_str_sweep_link_rel(global_State *g, GCRef *link)
{
  la_storeptr_rel((void **)&g->str.sweep_link, link);
}

static LJ_AINLINE MSize lj_str_sweep_bucket_acq(const global_State *g)
{
  return (MSize)la_load32_acq(&g->str.sweep_bucket);
}

static LJ_AINLINE void lj_str_sweep_bucket_rel(global_State *g, MSize bucket)
{
  la_store32_rel(&g->str.sweep_bucket, bucket);
}

static LJ_AINLINE uint32_t lj_str_sweep_phase_acq(const global_State *g)
{
  return la_load32_acq(&g->str.sweep_phase);
}

static LJ_AINLINE void lj_str_sweep_phase_rel(global_State *g,
					      uint32_t phase)
{
  la_store32_rel(&g->str.sweep_phase, phase);
}

static LJ_AINLINE uint64_t lj_str_sweep_grace_epoch_acq(
  const global_State *g)
{
  return la_load64_acq(&g->str.sweep_grace_epoch);
}

static LJ_AINLINE void lj_str_sweep_grace_epoch_rel(global_State *g,
						    uint64_t epoch)
{
  la_store64_rel(&g->str.sweep_grace_epoch, epoch);
}

static LJ_AINLINE void lj_str_sweep_tagged_add(global_State *g, uint64_t n)
{
  (void)la_add64_rlx(&g->str.sweep_tagged, n);
}

static LJ_AINLINE void lj_str_sweep_rescued_add(global_State *g, uint64_t n)
{
  (void)la_add64_rlx(&g->str.sweep_rescued, n);
}

static LJ_AINLINE void lj_str_sweep_unlinked_add(global_State *g, uint64_t n)
{
  (void)la_add64_rlx(&g->str.sweep_unlinked, n);
}

static LJ_AINLINE void lj_str_sweep_reclaimed_add(global_State *g, uint64_t n)
{
  (void)la_add64_rlx(&g->str.sweep_reclaimed, n);
}

#define lj_str_buckets(g)	(lj_str_tabh_acq((g))->bucket)

static LJ_AINLINE uintptr_t lj_str_link_load_acq(const GCRef *r)
{
  return (uintptr_t)la_load64_acq(&r->gcptr64);
}

static LJ_AINLINE int lj_str_link_cas_acqrel(GCRef *r, uintptr_t *oldp,
					     uintptr_t want)
{
  uint64_t old = (uint64_t)*oldp;
  int ok = la_cas64(&r->gcptr64, &old, (uint64_t)want,
		    LA_ACQ_REL, LA_ACQ);
  *oldp = (uintptr_t)old;
  return ok;
}

static LJ_AINLINE GCobj *lj_str_link_target(uintptr_t link)
{
  return lj_str_hashhead_u(link);
}

static LJ_AINLINE uintptr_t lj_str_ref_load_acq(const GCRef *r)
{
  return lj_str_link_load_acq(r);
}

static LJ_AINLINE void lj_str_ref_store_rel(GCRef *r, uintptr_t u)
{
  la_store64_rel(&r->gcptr64, (uint64_t)u);
}

static LJ_AINLINE GCobj *lj_str_hashhead_acq(const GCRef *r)
{
  return lj_str_link_target(lj_str_link_load_acq(r));
}

static LJ_AINLINE uintptr_t lj_str_hashflags_acq(const GCRef *r)
{
  return lj_str_ref_load_acq(r) & LJ_STRHASH_LINKMASK;
}

static LJ_AINLINE uintptr_t lj_str_hashsecondary_acq(const GCRef *r)
{
  return lj_str_ref_load_acq(r) & LJ_STRHASH_SECONDARY;
}

#define lj_str_hashhead(r)		lj_str_hashhead_acq(&(r))
#define lj_str_hashflags(r)		lj_str_hashflags_acq(&(r))
#define lj_str_hashsecondary(r)		lj_str_hashsecondary_acq(&(r))

static LJ_AINLINE GCobj *lj_str_next_acq(const GCobj *o)
{
  return lj_str_link_target(lj_str_link_load_acq(
	lj_obj_gcwref((GCobj *)o)));
}

static LJ_AINLINE uintptr_t lj_str_next_link_acq(const GCobj *o)
{
  return lj_str_link_load_acq(lj_obj_gcwref((GCobj *)o));
}

static LJ_AINLINE void lj_str_next_store_rel(GCobj *o, uintptr_t next)
{
  lj_str_ref_store_rel(lj_obj_gcwref(o),
		       next & ~(uintptr_t)LJ_STRHASH_SECONDARY);
}

static LJ_AINLINE void lj_str_bucket_store_rel(GCRef *r, GCobj *o,
					       uintptr_t flags)
{
  /* Rechainers must carry the target's incoming DEAD tag to its new edge. */
  lj_str_ref_store_rel(r, (uintptr_t)o | (flags & LJ_STRHASH_LINKMASK));
}

/* String helpers. */
LJ_FUNC int32_t LJ_FASTCALL lj_str_cmp(GCstr *a, GCstr *b);
LJ_FUNC const char *lj_str_find(const char *s, const char *f,
				MSize slen, MSize flen);
LJ_FUNC int lj_str_haspattern(GCstr *s);

/* String interning. */
LJ_FUNC void lj_str_resize(lua_State *L, MSize newmask);
LJ_FUNC int lj_str_sweep_claim(lua_State *L, StrTabHdr *hdr);
LJ_FUNC void lj_str_sweep_release(StrTabHdr *hdr);
LJ_FUNCA GCstr *lj_str_new(lua_State *L, const char *str, size_t len);
LJ_FUNC void LJ_FASTCALL lj_str_free(global_State *g, GCstr *s);
LJ_FUNC int LJ_FASTCALL lj_str_free_try(global_State *g, GCstr *s);
struct LJGCDestructCtx;
/* Terminal joined-world bucket removal is a two-LP transaction: prepare()
** acquires exact physical lifetime without mutating the string, the caller
** removes its incoming bucket edge, and commit() consumes type/count/body.
** commit() is valid only after prepare() returns DESTRUCT_ACQUIRED. */
LJ_FUNC int lj_str_free_prepare(global_State *g, GCstr *s,
				 struct LJGCDestructCtx *ctx);
LJ_FUNC void lj_str_free_commit(global_State *g, GCstr *s,
				 struct LJGCDestructCtx *ctx);
LJ_FUNC void lj_str_flush_num_credit(global_State *g, TGState *tg);
LJ_FUNC void LJ_FASTCALL lj_str_init(lua_State *L);
LJ_FUNC int lj_str_quarantine_resize(lua_State *L, MSize newmask);
/* Runtime drain only: caller holds GC2's exact-thread exclusive-reclaimer
** scope. Terminal joined-world cleanup uses the separate free helpers below. */
LJ_FUNC uint32_t lj_str_reclaim_retired(global_State *g,
					uint64_t completed_epoch);
LJ_FUNC void lj_str_gc2_reclaim_request(global_State *g);
LJ_FUNC void lj_str_gc2_reclaim_cancel(global_State *g);
LJ_FUNC void lj_str_gc2_sweep_begin(global_State *g, int major);
LJ_FUNC uint32_t lj_str_gc2_sweep_step(global_State *g, uint32_t limit);
LJ_FUNC int lj_str_gc2_sweep_pending(global_State *g);
LJ_FUNC void lj_str_gc2_sweep_abort(global_State *g);
LJ_FUNC void lj_str_gc2_sweep_finish(global_State *g);
LJ_FUNC int lj_str_gc2_reclaim_complete(global_State *g);
LJ_FUNC void lj_str_free_retired_bodies(global_State *g);
LJ_FUNC void lj_str_freetab(global_State *g);

enum {
  LJ_STR_TEST_MATCH_AFTER_COMPARE,
  LJ_STR_TEST_MATCH_BEFORE_RESCUE_CAS
};
enum {
  LJ_STR_TEST_CANON_Q_PINNED_BEFORE_PUBLISH,
  LJ_STR_TEST_CANON_MAIN_UNLINKED
};
enum {
  LJ_STR_TEST_RECLAIM_EXCLUSIVE_CLAIMED
};
#ifdef LJ_STR_TEST_HELPERS
typedef struct LJStrTestSweepSnapshot {
  uint64_t tagged;
  uint64_t rescued;
  uint64_t unlinked;
  uint64_t retired;
  uint64_t reclaimed;
  uint32_t phase;
  uint32_t pending;
} LJStrTestSweepSnapshot;
typedef void (*LJStrTestMatchHook)(lua_State *L, GCRef *link,
				   GCobj *target, uintptr_t observed,
				   uint32_t stage);
typedef void (*LJStrTestCanonHook)(lua_State *L, GCstr *str,
				   StrCanonRec *rec, uint32_t stage);
typedef void (*LJStrTestReclaimHook)(global_State *g, uint32_t stage);
LJ_FUNC void lj_str_test_set_match_hook(LJStrTestMatchHook hook);
LJ_FUNC void lj_str_test_set_canon_hook(LJStrTestCanonHook hook);
LJ_FUNC void lj_str_test_set_reclaim_hook(LJStrTestReclaimHook hook);
LJ_FUNC uint32_t lj_str_test_id_refills(void);
LJ_FUNC void lj_str_test_reset_id_refills(void);
LJ_FUNC uint32_t lj_str_test_num_refills(void);
LJ_FUNC void lj_str_test_reset_num_refills(void);
LJ_FUNC int lj_str_test_body_retired_chain_valid(global_State *g,
						  StrBodyRetire *head);
LJ_FUNC void lj_str_test_reset_sweep_counters(global_State *g);
LJ_FUNC void lj_str_test_sweep_snapshot(global_State *g,
					LJStrTestSweepSnapshot *snapshot);
LJ_FUNC int lj_str_test_quarantine_detach(lua_State *L, GCstr *s);
#endif

#define lj_str_newz(L, s)	(lj_str_new(L, s, strlen(s)))
#define lj_str_newlit(L, s)	(lj_str_new(L, "" s, sizeof(s)-1))
#define lj_str_size(len)	(sizeof(GCstr) + (((len)+4) & ~(MSize)3))

#endif

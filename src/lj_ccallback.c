/*
** FFI C callback handling.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#include "lj_obj.h"

#if LJ_HASFFI

#include "lj_atomic.h"
#include "lj_gc.h"
#include "lj_err.h"
#include "lj_tab.h"
#include "lj_state.h"
#include "lj_frame.h"
#include "lj_ctype.h"
#include "lj_cconv.h"
#include "lj_ccall.h"
#include "lj_ccallback.h"
#include "lj_safepoint.h"
#include "lj_target.h"
#include "lj_mcode.h"
#include "lj_trace.h"
#include "lj_tg.h"
#include "lj_thr.h"
#include "lj_vm.h"

/* -- Target-specific handling of callback slots -------------------------- */

#define CALLBACK_MCODE_SIZE	(LJ_PAGESIZE * LJ_NUM_CBPAGE)

static LJ_AINLINE CTypeID1 callback_cbid_load(CTypeID1 *cbid, MSize slot)
{
  return ctype_cb_cbid_slot_acq(cbid, slot);  /* 11.5 callback slot publish. */
}

static LJ_AINLINE void callback_cbid_store(CTypeID1 *cbid, MSize slot,
					   CTypeID id)
{
  ctype_cb_cbid_slot_rel(cbid, slot, id);  /* 11.5 callback slot publish. */
}

static LJ_AINLINE lua_State *callback_owner_load(lua_State **owner, MSize slot)
{
  return ctype_cb_owner_slot_acq(owner, slot);
}

static LJ_AINLINE int callback_owner_claim(lua_State **owner, MSize slot,
					   lua_State *L)
{
  return ctype_cb_owner_slot_claim(owner, slot,
				   L);  /* 11.5 callback slot owner claim. */
}

static LJ_AINLINE int callback_owner_clear(lua_State **owner, MSize slot,
					   lua_State *L)
{
  return ctype_cb_owner_slot_clear(owner, slot,
				   L);  /* 11.5 callback owner disown. */
}

static lua_State *callback_carrier_new_l(lua_State *L)
{
  lua_State *carrier;
  lj_gc_check(L);
  carrier = lj_state_new(L);
  carrier->tg_hint = NULL;  /* Hidden carrier is attached only on demand. */
  return carrier;
}

static void callback_owner_barrier_l(lua_State *L, lua_State *carrier)
{
  TValue tv;
  setthreadV(L, &tv, carrier);
  lj_gc_pubroot(L, &tv);  /* 11.5 callback carrier side root. */
}

static LJ_AINLINE TValue *callback_func_slots(CTState *cts)
{
  return ctype_cb_func_acq(cts);
}

static LJ_AINLINE void callback_func_load(CTState *cts, MSize slot,
					  TValue *out)
{
  TValue *func = callback_func_slots(cts);
  if (LJ_LIKELY(func != NULL))
    lj_tv_load_acq(out, &func[slot]);
  else
    setnilV(out);
}

void lj_ccallback_func_store_l(lua_State *L, CTState *cts, MSize slot,
			       GCfunc *fn)
{
  TValue *func = callback_func_slots(cts);
  TValue tv;
  if (LJ_UNLIKELY(func == NULL ||
		  slot >= ctype_cb_sizeid_acq(cts)))
    lj_err_caller(L, LJ_ERR_FFI_CBACKOV);
  setfuncV(L, &tv, fn);
  copyTVrel(L, &func[slot], &tv);
  lj_gc_pubroot(L, &func[slot]);  /* 11.5 callback function side root. */
}

void lj_ccallback_func_clear(CTState *cts, MSize slot)
{
  TValue *func = callback_func_slots(cts);
  if (LJ_LIKELY(func != NULL &&
		slot < ctype_cb_sizeid_acq(cts))) {
    TValue nilv;
    setnilV(&nilv);
    copyTVrel(mainthread_acq(cts->g), &func[slot], &nilv);
  }
}

#if LJ_OS_NOJIT

/* Callbacks disabled. */
#define CALLBACK_SLOT2OFS(slot)	(0*(slot))
#define CALLBACK_OFS2SLOT(ofs)	(0*(ofs))
#define CALLBACK_MAX_SLOT	0

#elif LJ_TARGET_X86ORX64

#if LJ_ABI_BRANCH_TRACK
#define CALLBACK_MCODE_SLOTSZ	8
#else
#define CALLBACK_MCODE_SLOTSZ	4
#endif
#define CALLBACK_MCODE_NSLOT	(128 / CALLBACK_MCODE_SLOTSZ)

#define CALLBACK_MCODE_HEAD	(LJ_64 ? 8 : 0)
#define CALLBACK_MCODE_GROUP	(-2+1+2+(LJ_GC64 ? 10 : 5)+(LJ_64 ? 6 : 5))

#define CALLBACK_SLOT2OFS(slot) \
  (CALLBACK_MCODE_HEAD + CALLBACK_MCODE_GROUP*((slot)/CALLBACK_MCODE_NSLOT) + CALLBACK_MCODE_SLOTSZ*(slot))

static MSize CALLBACK_OFS2SLOT(MSize ofs)
{
  MSize group;
  ofs -= CALLBACK_MCODE_HEAD;
  group = ofs / (128 + CALLBACK_MCODE_GROUP);
  return (ofs % (128 + CALLBACK_MCODE_GROUP))/CALLBACK_MCODE_SLOTSZ + group*CALLBACK_MCODE_NSLOT;
}

#define CALLBACK_MAX_SLOT \
  (((CALLBACK_MCODE_SIZE-CALLBACK_MCODE_HEAD)/(CALLBACK_MCODE_GROUP+128))*CALLBACK_MCODE_NSLOT)

#elif LJ_TARGET_ARM

#define CALLBACK_MCODE_HEAD		32

#elif LJ_TARGET_ARM64

#if LJ_ABI_BRANCH_TRACK
#define CALLBACK_MCODE_SLOTSZ		12
#endif

#define CALLBACK_MCODE_HEAD		32

#elif LJ_TARGET_PPC

#define CALLBACK_MCODE_HEAD		24

#elif LJ_TARGET_MIPS32

#define CALLBACK_MCODE_HEAD		20

#elif LJ_TARGET_MIPS64

#define CALLBACK_MCODE_HEAD		52

#else

/* Missing support for this architecture. */
#define CALLBACK_SLOT2OFS(slot)	(0*(slot))
#define CALLBACK_OFS2SLOT(ofs)	(0*(ofs))
#define CALLBACK_MAX_SLOT	0

#endif

#ifndef CALLBACK_SLOT2OFS
#ifndef CALLBACK_MCODE_SLOTSZ
#define CALLBACK_MCODE_SLOTSZ		8
#endif
#define CALLBACK_SLOT2OFS(slot)		(CALLBACK_MCODE_HEAD + CALLBACK_MCODE_SLOTSZ*(slot))
#define CALLBACK_OFS2SLOT(ofs)		(((ofs)-CALLBACK_MCODE_HEAD)/CALLBACK_MCODE_SLOTSZ)
#define CALLBACK_MAX_SLOT		(CALLBACK_OFS2SLOT(CALLBACK_MCODE_SIZE))
#endif

/* Convert callback slot number to callback function pointer. */
static void *callback_slot2ptr(CTState *cts, MSize slot)
{
  return (uint8_t *)ctype_cb_mcode_acq(cts) + CALLBACK_SLOT2OFS(slot);
}

/* Convert callback function pointer to slot number. */
MSize lj_ccallback_ptr2slot(CTState *cts, void *p)
{
  uint8_t *mcode = (uint8_t *)ctype_cb_mcode_acq(cts);
  uintptr_t ofs = (uintptr_t)((uint8_t *)p - mcode);
  if (ofs < CALLBACK_MCODE_SIZE) {
    MSize slot = CALLBACK_OFS2SLOT((MSize)ofs);
    if (CALLBACK_SLOT2OFS(slot) == (MSize)ofs)
      return slot;
  }
  return ~0u;  /* Not a known callback function pointer. */
}

/* Initialize machine code for callback function pointers. */
#if LJ_OS_NOJIT
/* Disabled callback support. */
#define callback_mcode_init(g, p)	(p)
#elif LJ_TARGET_X86ORX64
static void *callback_mcode_init(global_State *g, uint8_t *page)
{
  uint8_t *p = page;
  uint8_t *target = (uint8_t *)(void *)lj_vm_ffi_callback;
  MSize slot;
#if LJ_64
  *(void **)p = target; p += 8;
#endif
  for (slot = 0; slot < CALLBACK_MAX_SLOT; slot++) {
#if LJ_ABI_BRANCH_TRACK
    *(uint32_t *)p = XI_ENDBR64; p += 4;
#endif
    /* mov al, slot; jmp group */
    *p++ = XI_MOVrib | RID_EAX; *p++ = (uint8_t)slot;
    if ((slot & (CALLBACK_MCODE_NSLOT-1)) == (CALLBACK_MCODE_NSLOT-1) ||
	slot == CALLBACK_MAX_SLOT-1) {
      /* push ebp/rbp; mov ah, slot>>8; mov ebp, &g. */
      *p++ = XI_PUSH + RID_EBP;
      *p++ = XI_MOVrib | (RID_EAX+4); *p++ = (uint8_t)(slot >> 8);
#if LJ_GC64
      *p++ = 0x48; *p++ = XI_MOVri | RID_EBP;
      *(uint64_t *)p = (uint64_t)(g); p += 8;
#else
      *p++ = XI_MOVri | RID_EBP;
      *(int32_t *)p = i32ptr(g); p += 4;
#endif
#if LJ_64
      /* jmp [rip-pageofs] where lj_vm_ffi_callback is stored. */
      *p++ = XI_GROUP5; *p++ = XM_OFS0 + (XOg_JMP<<3) + RID_EBP;
      *(int32_t *)p = (int32_t)(page-(p+4)); p += 4;
#else
      /* jmp lj_vm_ffi_callback. */
      *p++ = XI_JMP; *(int32_t *)p = target-(p+4); p += 4;
#endif
    } else {
      *p++ = XI_JMPs;
      *p++ = (uint8_t)(CALLBACK_MCODE_SLOTSZ*(CALLBACK_MCODE_NSLOT-1-(slot&(CALLBACK_MCODE_NSLOT-1))) - 2);
    }
  }
  return p;
}
#elif LJ_TARGET_ARM
static void *callback_mcode_init(global_State *g, uint32_t *page)
{
  uint32_t *p = page;
  void *target = (void *)lj_vm_ffi_callback;
  MSize slot;
  /* This must match with the saveregs macro in buildvm_arm.dasc. */
  *p++ = ARMI_SUB|ARMF_D(RID_R12)|ARMF_N(RID_R12)|ARMF_M(RID_PC);
  *p++ = ARMI_PUSH|ARMF_N(RID_SP)|RSET_RANGE(RID_R4,RID_R11+1)|RID2RSET(RID_LR);
  *p++ = ARMI_SUB|ARMI_K12|ARMF_D(RID_R12)|ARMF_N(RID_R12)|CALLBACK_MCODE_HEAD;
  *p++ = ARMI_STR|ARMI_LS_P|ARMI_LS_W|ARMF_D(RID_R12)|ARMF_N(RID_SP)|(CFRAME_SIZE-4*9);
  *p++ = ARMI_LDR|ARMI_LS_P|ARMI_LS_U|ARMF_D(RID_R12)|ARMF_N(RID_PC);
  *p++ = ARMI_LDR|ARMI_LS_P|ARMI_LS_U|ARMF_D(RID_PC)|ARMF_N(RID_PC);
  *p++ = u32ptr(g);
  *p++ = u32ptr(target);
  for (slot = 0; slot < CALLBACK_MAX_SLOT; slot++) {
    *p++ = ARMI_MOV|ARMF_D(RID_R12)|ARMF_M(RID_PC);
    *p = ARMI_B | ((page-p-2) & 0x00ffffffu);
    p++;
  }
  return p;
}
#elif LJ_TARGET_ARM64
static void *callback_mcode_init(global_State *g, uint32_t *page)
{
  uint32_t *p = page;
  ASMFunction target = lj_vm_ffi_callback;
  MSize slot;
  *p++ = A64I_LE(A64I_LDRLx | A64F_D(RID_X11) | A64F_S19(4));
  *p++ = A64I_LE(A64I_LDRLx | A64F_D(RID_X10) | A64F_S19(5));
  *p++ = A64I_LE(A64I_BR_AUTH | A64F_N(RID_X11));
  *p++ = A64I_LE(A64I_NOP);
  ((ASMFunction *)p)[0] = target;
  ((void **)p)[1] = g;
  p += 4;
  for (slot = 0; slot < CALLBACK_MAX_SLOT; slot++) {
#if LJ_ABI_BRANCH_TRACK
    *p++ = A64I_BTI_C;
#endif
    *p++ = A64I_LE(A64I_MOVZw | A64F_D(RID_X9) | A64F_U16(slot));
    *p = A64I_LE(A64I_B | A64F_S26((page-p) & 0x03ffffffu));
    p++;
  }
  return p;
}
#elif LJ_TARGET_PPC
static void *callback_mcode_init(global_State *g, uint32_t *page)
{
  uint32_t *p = page;
  void *target = (void *)lj_vm_ffi_callback;
  MSize slot;
  *p++ = PPCI_LIS | PPCF_T(RID_TMP) | (u32ptr(target) >> 16);
  *p++ = PPCI_LIS | PPCF_T(RID_R12) | (u32ptr(g) >> 16);
  *p++ = PPCI_ORI | PPCF_A(RID_TMP)|PPCF_T(RID_TMP) | (u32ptr(target) & 0xffff);
  *p++ = PPCI_ORI | PPCF_A(RID_R12)|PPCF_T(RID_R12) | (u32ptr(g) & 0xffff);
  *p++ = PPCI_MTCTR | PPCF_T(RID_TMP);
  *p++ = PPCI_BCTR;
  for (slot = 0; slot < CALLBACK_MAX_SLOT; slot++) {
    *p++ = PPCI_LI | PPCF_T(RID_R11) | slot;
    *p = PPCI_B | (((page-p) & 0x00ffffffu) << 2);
    p++;
  }
  return p;
}
#elif LJ_TARGET_MIPS
static void *callback_mcode_init(global_State *g, uint32_t *page)
{
  uint32_t *p = page;
  uintptr_t target = (uintptr_t)(void *)lj_vm_ffi_callback;
  uintptr_t ug = (uintptr_t)(void *)g;
  MSize slot;
#if LJ_TARGET_MIPS32
  *p++ = MIPSI_LUI | MIPSF_T(RID_R3) | (target >> 16);
  *p++ = MIPSI_LUI | MIPSF_T(RID_R2) | (ug >> 16);
#else
  *p++ = MIPSI_LUI  | MIPSF_T(RID_R3) | (target >> 48);
  *p++ = MIPSI_LUI  | MIPSF_T(RID_R2) | (ug >> 48);
  *p++ = MIPSI_ORI  | MIPSF_T(RID_R3)|MIPSF_S(RID_R3) | ((target >> 32) & 0xffff);
  *p++ = MIPSI_ORI  | MIPSF_T(RID_R2)|MIPSF_S(RID_R2) | ((ug >> 32) & 0xffff);
  *p++ = MIPSI_DSLL | MIPSF_D(RID_R3)|MIPSF_T(RID_R3) | MIPSF_A(16);
  *p++ = MIPSI_DSLL | MIPSF_D(RID_R2)|MIPSF_T(RID_R2) | MIPSF_A(16);
  *p++ = MIPSI_ORI  | MIPSF_T(RID_R3)|MIPSF_S(RID_R3) | ((target >> 16) & 0xffff);
  *p++ = MIPSI_ORI  | MIPSF_T(RID_R2)|MIPSF_S(RID_R2) | ((ug >> 16) & 0xffff);
  *p++ = MIPSI_DSLL | MIPSF_D(RID_R3)|MIPSF_T(RID_R3) | MIPSF_A(16);
  *p++ = MIPSI_DSLL | MIPSF_D(RID_R2)|MIPSF_T(RID_R2) | MIPSF_A(16);
#endif
  *p++ = MIPSI_ORI  | MIPSF_T(RID_R3)|MIPSF_S(RID_R3) | (target & 0xffff);
  *p++ = MIPSI_JR | MIPSF_S(RID_R3);
  *p++ = MIPSI_ORI | MIPSF_T(RID_R2)|MIPSF_S(RID_R2) | (ug & 0xffff);
  for (slot = 0; slot < CALLBACK_MAX_SLOT; slot++) {
    *p = MIPSI_B | ((page-p-1) & 0x0000ffffu);
    p++;
    *p++ = MIPSI_LI | MIPSF_T(RID_R1) | slot;
  }
  return p;
}
#else
/* Missing support for this architecture. */
#define callback_mcode_init(g, p)	(p)
#endif

/* -- Machine code management --------------------------------------------- */

#if LJ_TARGET_WINDOWS

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#elif LJ_TARGET_POSIX

#include <sys/mman.h>
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS   MAP_ANON
#endif
#ifdef PROT_MPROTECT
#define CCPROT_CREATE	(PROT_MPROTECT(PROT_EXEC))
#else
#define CCPROT_CREATE	0
#endif

/* Check for macOS hardened runtime. */
#if defined(LUAJIT_ENABLE_OSX_HRT) && LUAJIT_SECURITY_MCODE != 0 && defined(MAP_JIT) && __ENVIRONMENT_MAC_OS_X_VERSION_MIN_REQUIRED__ >= 110000
#include <pthread.h>
#define CCMAP_CREATE	MAP_JIT
#else
#define CCMAP_CREATE	0
#endif

#endif

/* Allocate and initialize area for callback function pointers. */
LJ_NORET static void callback_err(lua_State *L)
{
  lj_err_caller(L, LJ_ERR_FFI_CBACKOV);
}

static int callback_mcode_had_stopreq(lua_State *L)
{
  TGState *tg = L ? L2TG(L) : NULL;
  return tg && lj_tg_flags_test_acq(tg, TGF_STOPREQ);
}

static int callback_mcode_fresh_stopreq(lua_State *L, uint32_t actions,
					int had_stopreq)
{
  TGState *tg = L ? L2TG(L) : NULL;
  return (actions & LJ_GC2_HS_STOPREQ) ||
    (!had_stopreq && tg && lj_tg_flags_test_acq(tg, TGF_STOPREQ));
}

static void callback_mcode_discard(lua_State *L, void *p, size_t sz)
{
  if (p == NULL)
    return;
#if LJ_TARGET_WINDOWS
  VirtualFree(p, 0, MEM_RELEASE);
  UNUSED(L); UNUSED(sz);
#elif LJ_TARGET_POSIX
  if (p != MAP_FAILED)
    munmap(p, sz);
  UNUSED(L);
#else
  lj_mem_free(G(L), p, sz);
#endif
}

static void callback_mcode_checkstop(lua_State *L, uint32_t actions,
				     int had_stopreq, void *p, size_t sz)
{
  if (callback_mcode_fresh_stopreq(L, actions, had_stopreq)) {
    callback_mcode_discard(L, p, sz);
    lj_safepoint_checkstop(L, actions);
  }
}

static void callback_mcode_new_l(lua_State *L, CTState *cts)
{
  size_t sz = (size_t)CALLBACK_MCODE_SIZE;
  void *p, *pe;
  int had_stopreq = callback_mcode_had_stopreq(L);
  uint32_t actions;
  if (CALLBACK_MAX_SLOT == 0)
    callback_err(L);
#if LJ_TARGET_WINDOWS
  lj_native_enter(L2TG(L));
  p = LJ_WIN_VALLOC(NULL, sz, MEM_RESERVE|MEM_COMMIT, PAGE_READWRITE);
  actions = lj_native_leave(L);
  if (!p) {
    if (callback_mcode_fresh_stopreq(L, actions, had_stopreq))
      lj_safepoint_checkstop(L, actions);
    callback_err(L);
  }
  callback_mcode_checkstop(L, actions, had_stopreq, p, sz);
#elif LJ_TARGET_POSIX
  lj_native_enter(L2TG(L));
  p = mmap(NULL, sz, PROT_READ|PROT_WRITE|CCPROT_CREATE,
	   MAP_PRIVATE|MAP_ANONYMOUS|CCMAP_CREATE, -1, 0);
  actions = lj_native_leave(L);
  if (p == MAP_FAILED) {
    if (callback_mcode_fresh_stopreq(L, actions, had_stopreq))
      lj_safepoint_checkstop(L, actions);
    callback_err(L);
  }
  callback_mcode_checkstop(L, actions, had_stopreq, p, sz);
#if CCMAP_CREATE
  pthread_jit_write_protect_np(0);
#endif
#else
  /* Fallback allocator. Fails if memory is not executable by default. */
  p = lj_mem_new(L, sz);
#endif
  pe = callback_mcode_init(cts->g, p);
  UNUSED(pe);
  lj_assertCTS((size_t)((char *)pe - (char *)p) <= sz,
	       "miscalculated CALLBACK_MAX_SLOT");
  lj_mcode_sync(p, (char *)p + sz);
#if LJ_TARGET_WINDOWS
  {
    DWORD oprot;
    lj_native_enter(L2TG(L));
    LJ_WIN_VPROTECT(p, sz, PAGE_EXECUTE_READ, &oprot);
    actions = lj_native_leave(L);
    callback_mcode_checkstop(L, actions, had_stopreq, p, sz);
  }
#elif LJ_TARGET_POSIX
#if CCMAP_CREATE
  pthread_jit_write_protect_np(1);
#else
  lj_native_enter(L2TG(L));
  (void)mprotect(p, sz, (PROT_READ|PROT_EXEC));
  actions = lj_native_leave(L);
  callback_mcode_checkstop(L, actions, had_stopreq, p, sz);
#endif
#endif
  ctype_cb_mcode_rel(cts, p);  /* 11.5 publish mcode. */
}

/* Free area for callback function pointers. */
void lj_ccallback_mcode_free(CTState *cts)
{
  size_t sz = (size_t)CALLBACK_MCODE_SIZE;
  void *p = ctype_cb_mcode_acq(cts);
  if (p == NULL) return;
#if LJ_TARGET_WINDOWS
  VirtualFree(p, 0, MEM_RELEASE);
  UNUSED(sz);
#elif LJ_TARGET_POSIX
  munmap(p, sz);
#else
  lj_mem_free(cts->g, p, sz);
#endif
}

/* -- C callback entry ---------------------------------------------------- */

/* Target-specific handling of register arguments. Similar to lj_ccall.c. */
#if LJ_TARGET_X86

#define CALLBACK_HANDLE_REGARG \
  if (!isfp) {  /* Only non-FP values may be passed in registers. */ \
    if (n > 1) {  /* Anything > 32 bit is passed on the stack. */ \
      if (!LJ_ABI_WIN) ngpr = maxgpr;  /* Prevent reordering. */ \
    } else if (ngpr + 1 <= maxgpr) { \
      sp = &cb->gpr[ngpr]; \
      ngpr += n; \
      goto done; \
    } \
  }

#elif LJ_TARGET_X64 && LJ_ABI_WIN

/* Windows/x64 argument registers are strictly positional (use ngpr). */
#define CALLBACK_HANDLE_REGARG \
  if (isfp) { \
    if (ngpr < maxgpr) { sp = &cb->fpr[ngpr++]; UNUSED(nfpr); goto done; } \
  } else { \
    if (ngpr < maxgpr) { sp = &cb->gpr[ngpr++]; goto done; } \
  }

#elif LJ_TARGET_X64

#define CALLBACK_HANDLE_REGARG \
  if (isfp) { \
    if (nfpr + n <= CCALL_NARG_FPR) { \
      sp = &cb->fpr[nfpr]; \
      nfpr += n; \
      goto done; \
    } \
  } else { \
    if (ngpr + n <= maxgpr) { \
      sp = &cb->gpr[ngpr]; \
      ngpr += n; \
      goto done; \
    } \
  }

#elif LJ_TARGET_ARM

#if LJ_ABI_SOFTFP

#define CALLBACK_HANDLE_REGARG_FP1	UNUSED(isfp);
#define CALLBACK_HANDLE_REGARG_FP2

#else

#define CALLBACK_HANDLE_REGARG_FP1 \
  if (isfp) { \
    if (n == 1) { \
      if (fprodd) { \
	sp = &cb->fpr[fprodd-1]; \
	fprodd = 0; \
	goto done; \
      } else if (nfpr + 1 <= CCALL_NARG_FPR) { \
	sp = &cb->fpr[nfpr++]; \
	fprodd = nfpr; \
	goto done; \
      } \
    } else { \
      if (nfpr + 1 <= CCALL_NARG_FPR) { \
	sp = &cb->fpr[nfpr++]; \
	goto done; \
      } \
    } \
    fprodd = 0;  /* No reordering after the first FP value is on stack. */ \
  } else {

#define CALLBACK_HANDLE_REGARG_FP2	}

#endif

#define CALLBACK_HANDLE_REGARG \
  CALLBACK_HANDLE_REGARG_FP1 \
  if (n > 1) ngpr = (ngpr + 1u) & ~1u;  /* Align to regpair. */ \
  if (ngpr + n <= maxgpr) { \
    sp = &cb->gpr[ngpr]; \
    ngpr += n; \
    goto done; \
  } CALLBACK_HANDLE_REGARG_FP2

#elif LJ_TARGET_ARM64

#define CALLBACK_HANDLE_REGARG \
  if (isfp) { \
    if (nfpr + n <= CCALL_NARG_FPR) { \
      sp = &cb->fpr[nfpr]; \
      nfpr += n; \
      goto done; \
    } else { \
      nfpr = CCALL_NARG_FPR;  /* Prevent reordering. */ \
    } \
  } else { \
    if (!LJ_TARGET_OSX && n > 1) \
      ngpr = (ngpr + 1u) & ~1u;  /* Align to regpair. */ \
    if (ngpr + n <= maxgpr) { \
      sp = &cb->gpr[ngpr]; \
      ngpr += n; \
      goto done; \
    } else { \
      ngpr = CCALL_NARG_GPR;  /* Prevent reordering. */ \
    } \
  }

#elif LJ_TARGET_PPC

#define CALLBACK_HANDLE_GPR \
  if (n > 1) { \
    lj_assertCTS(((LJ_ABI_SOFTFP && ctype_isnum(cta->info)) ||  /* double. */ \
		 ctype_isinteger(cta->info)) && n == 2,  /* int64_t. */ \
		 "bad GPR type"); \
    ngpr = (ngpr + 1u) & ~1u;  /* Align int64_t to regpair. */ \
  } \
  if (ngpr + n <= maxgpr) { \
    sp = &cb->gpr[ngpr]; \
    ngpr += n; \
    goto done; \
  }

#if LJ_ABI_SOFTFP
#define CALLBACK_HANDLE_REGARG \
  CALLBACK_HANDLE_GPR \
  UNUSED(isfp);
#else
#define CALLBACK_HANDLE_REGARG \
  if (isfp) { \
    if (nfpr + 1 <= CCALL_NARG_FPR) { \
      sp = &cb->fpr[nfpr++]; \
      cta = ctype_get(cts, CTID_DOUBLE);  /* FPRs always hold doubles. */ \
      goto done; \
    } \
  } else {  /* Try to pass argument in GPRs. */ \
    CALLBACK_HANDLE_GPR \
  }
#endif

#if !LJ_ABI_SOFTFP
#define CALLBACK_HANDLE_RET \
  if (ctype_isfp(ctr->info) && ctr->size == sizeof(float)) \
    *(double *)dp = *(float *)dp;  /* FPRs always hold doubles. */
#endif

#elif LJ_TARGET_MIPS32

#define CALLBACK_HANDLE_GPR \
  if (n > 1) ngpr = (ngpr + 1u) & ~1u;  /* Align to regpair. */ \
  if (ngpr + n <= maxgpr) { \
    sp = &cb->gpr[ngpr]; \
    ngpr += n; \
    goto done; \
  }

#if !LJ_ABI_SOFTFP	/* MIPS32 hard-float */
#define CALLBACK_HANDLE_REGARG \
  if (isfp && nfpr < CCALL_NARG_FPR) {  /* Try to pass argument in FPRs. */ \
    sp = (void *)((uint8_t *)&cb->fpr[nfpr] + ((LJ_BE && n==1) ? 4 : 0)); \
    nfpr++; ngpr += n; \
    goto done; \
  } else {  /* Try to pass argument in GPRs. */ \
    nfpr = CCALL_NARG_FPR; \
    CALLBACK_HANDLE_GPR \
  }
#else			/* MIPS32 soft-float */
#define CALLBACK_HANDLE_REGARG \
  CALLBACK_HANDLE_GPR \
  UNUSED(isfp);
#endif

#define CALLBACK_HANDLE_RET \
  if (ctype_isfp(ctr->info) && ctr->size == sizeof(float)) \
    ((float *)dp)[1] = *(float *)dp;

#elif LJ_TARGET_MIPS64

#if !LJ_ABI_SOFTFP	/* MIPS64 hard-float */
#define CALLBACK_HANDLE_REGARG \
  if (ngpr + n <= maxgpr) { \
    sp = isfp ? (void*) &cb->fpr[ngpr] : (void*) &cb->gpr[ngpr]; \
    ngpr += n; \
    goto done; \
  }
#else			/* MIPS64 soft-float */
#define CALLBACK_HANDLE_REGARG \
  if (ngpr + n <= maxgpr) { \
    UNUSED(isfp); \
    sp = (void*) &cb->gpr[ngpr]; \
    ngpr += n; \
    goto done; \
  }
#endif

#define CALLBACK_HANDLE_RET \
  if (ctype_isfp(ctr->info) && ctr->size == sizeof(float)) \
    ((float *)dp)[1] = *(float *)dp;

#else
#error "Missing calling convention definitions for this architecture"
#endif

static void callback_frame_push(lua_State *L, CCallbackRuntime *cb,
				TValue *cont, uint32_t native_depth,
				uint8_t auto_detach)
{
  MSize depth = ccallback_depth_acq(cb);
  if (LJ_UNLIKELY(depth >= CCALLBACK_MAX_NEST))
    lj_err_caller(L, LJ_ERR_FFI_CBACKOV);
  cb->frame[depth].L = L;
  cb->frame[depth].cont = cont;
  cb->frame[depth].native_depth = native_depth;
  cb->frame[depth].auto_detach = auto_detach;
  ccallback_depth_rel(cb, depth + 1);
}

static CCallbackFrame *callback_frame_top(CCallbackRuntime *cb)
{
  MSize depth = ccallback_depth_acq(cb);
  return depth == 0 ? NULL : &cb->frame[depth-1];
}

static void callback_frame_pop(CCallbackRuntime *cb)
{
  MSize depth = ccallback_depth_acq(cb);
  CCallbackFrame *frame;
  if (depth == 0)
    return;
  depth--;
  frame = &cb->frame[depth];
  frame->L = NULL;
  frame->cont = NULL;
  frame->native_depth = 0;
  frame->auto_detach = 0;
  ccallback_depth_rel(cb, depth);
}

static int callback_auto_attach(CTState *cts, MSize slot)
{
  lua_State **owner;
  lua_State *L;
  if (slot >= ctype_cb_sizeid_acq(cts))
    return 0;
  owner = ctype_cb_owner_acq(cts);
  if (owner == NULL)
    return 0;
  L = callback_owner_load(owner, slot);
  if (L == NULL || G(L) != cts->g)
    return 0;
  return lj_threading_attach_wait(L);
}

CCallbackRuntime * LJ_FASTCALL lj_ccallback_prepare(CTState *cts, MSize slot)
{
  TGState *tg = lj_thr_get_tg();
  lua_State *L;
  CCallbackRuntime *cb;
  uint8_t auto_detach = 0;
  if (LJ_UNLIKELY(tg == NULL || tg->gl != cts->g ||
		  lj_tg_load_cur_L(tg) == NULL)) {
    if (tg == NULL && callback_auto_attach(cts, slot)) {
      tg = lj_thr_get_tg();
      auto_detach = 1;
    }
    if (tg == NULL || tg->gl != cts->g || lj_tg_load_cur_L(tg) == NULL)
      return NULL;  /* No legal callback carrier for this foreign pthread. */
  }
  L = lj_tg_load_cur_L(tg);
  cb = &tg->cb;
  ccallback_L_rel(cb, L);  /* Carrier TG from current TLS, not slot owner. */
  ccallback_slot_rel(cb, slot);
  ccallback_auto_detach_rel(cb, auto_detach);
  return cb;
}

void lj_ccallback_unwind(lua_State *L, TValue *cont)
{
  TGState *tg = lj_thr_get_tg();
  CCallbackRuntime *cb;
  CCallbackFrame *frame;
  if (L == NULL || tg == NULL || tg->gl != G(L))
    return;
  cb = &tg->cb;
  lj_tg_ffi_call_func_rel(tg, NULL);
  ccallback_native_had_stopreq_rel(cb, 0);
  frame = callback_frame_top(cb);
  if (frame != NULL && frame->cont == cont) {
    uint8_t auto_detach = frame->auto_detach;
    callback_frame_pop(cb);
    ccallback_auto_detach_rel(cb, 0);
    if (auto_detach)
      lj_threading_detach(L, 0);
  } else {
    uint8_t auto_detach = ccallback_auto_detach_acq(cb);
    if (auto_detach) {
      ccallback_auto_detach_rel(cb, 0);
      lj_threading_detach(L, 0);
    }
  }
}

static int callback_ctype_snapshot_wait(lua_State *L, CTState *cts,
					 CTypeID id, CType *out)
{
  for (;;) {
    int ok = lj_ctype_snapshot(cts, id, out);
    if (ok >= 0)
      return ok;
    lj_ctype_parse_wait(cts, L, ctype_parse_token_acq(cts));
  }
}

static int callback_rawctype_wait(lua_State *L, CTState *cts, CTypeID id,
				  CTypeID *ridp, CType *out)
{
  CTInfo info;
  CTSize size;
  return lj_ctype_info_wait(L, cts, id, &info, &size, ridp, out);
}

static void callback_frame_rid_store(lua_State *L, CTypeID rid)
{
#if LJ_FR2
  (L->base-3)->u64 = ((L->base-3)->u64 & ~(uint64_t)0xffffu) |
		     (uint16_t)rid;
#else
  (L->base-2)->u32.hi = ((L->base-2)->u32.hi & ~0xffffu) |
			(uint16_t)rid;
#endif
}

static CTypeID callback_frame_rid_load(lua_State *L)
{
#if LJ_FR2
  return (CTypeID)(uint16_t)(L->base-3)->u64;
#else
  return (CTypeID)(uint16_t)(L->base-2)->u32.hi;
#endif
}

/* Convert and push callback arguments to Lua stack. */
static void callback_conv_args(CTState *cts, lua_State *L, CCallbackRuntime *cb)
{
  TValue *o = L->top;
  intptr_t *stack = cb->stack;
  MSize slot = ccallback_slot_acq(cb);
  CTypeID id = 0, rid, fid;
  CTypeID1 *cbid;
  int gcsteps = 0;
  CType ctsnap;
  CType *ct;
  CTInfo ctinfo = 0;
  GCfunc *fn;
  int fntp;
  MSize ngpr = 0, nsp = 0, maxgpr = CCALL_NARG_GPR;
#if CCALL_NARG_FPR
  MSize nfpr = 0;
#if LJ_TARGET_ARM
  MSize fprodd = 0;
#endif
#endif

  if (slot < ctype_cb_sizeid_acq(cts) &&
      (cbid = ctype_cb_cbid_acq(cts)) != NULL &&
      (id = callback_cbid_load(cbid, slot)) != 0) {
    TValue tv;
    callback_func_load(cts, slot, &tv);
    if (tvisfunc(&tv)) {
      ct = &ctsnap;
      rid = 0;
      fn = funcV(&tv);
      fntp = LJ_TFUNC;
    } else {
      ct = NULL;
      rid = 0;
      fn = (GCfunc *)L;
      fntp = LJ_TTHREAD;
    }
  } else {  /* Must set up frame first, before throwing the error. */
    ct = NULL;
    rid = 0;
    fn = (GCfunc *)L;
    fntp = LJ_TTHREAD;
  }
  /* Continuation returns from callback. */
  if (LJ_FR2) {
    (o++)->u64 = LJ_CONT_FFI_CALLBACK;
    (o++)->u64 = rid;
  } else {
    o->u32.lo = LJ_CONT_FFI_CALLBACK;
    o->u32.hi = rid;
    o++;
  }
  setframe_gc(o, obj2gco(fn), fntp);
  if (LJ_FR2) o++;
  setframe_ftsz(o, ((char *)(o+1) - (char *)L->base) + FRAME_CONT);
  L->top = L->base = ++o;
  if (!ct)
    lj_err_caller(L, LJ_ERR_FFI_BADCBACK);
  if (isluafunc(fn))
    setcframe_pc(L->cframe, proto_bc(funcproto(fn))+1);
  if (!callback_ctype_snapshot_wait(L, cts, id, &ctsnap))
    lj_err_caller(L, LJ_ERR_FFI_BADCBACK);
  ctinfo = ctype_info_acq(ct);
  if (!ctype_isfunc(ctinfo))
    lj_err_caller(L, LJ_ERR_FFI_BADCBACK);
  rid = ctype_cid(ctinfo);  /* Return type. x86: +(spadj<<16). */
  callback_frame_rid_store(L, rid);
  lj_state_checkstack(L, LUA_MINSTACK);  /* May throw. */
  o = L->base;  /* Might have been reallocated. */

#if LJ_TARGET_X86
  /* x86 has several different calling conventions. */
  switch (ctype_cconv(ctinfo)) {
  case CTCC_FASTCALL: maxgpr = 2; break;
  case CTCC_THISCALL: maxgpr = 1; break;
  default: maxgpr = 0; break;
  }
#endif

  fid = ctype_sib_acq(ct);
  while (fid) {
    CType ctfsnap, ctasnap;
    CType *ctf = &ctfsnap;
    CTInfo finfo;
    if (!callback_ctype_snapshot_wait(L, cts, fid, ctf))
      lj_err_caller(L, LJ_ERR_FFI_BADCBACK);
    finfo = ctype_info_acq(ctf);
    if (!ctype_isattrib(finfo)) {
      CType *cta = &ctasnap;
      void *sp;
      CTSize sz;
      CTSize asize;
      CTInfo ainfo;
      int isfp;
      MSize n;
      CTypeID aid;
      lj_assertCTS(ctype_isfield(finfo), "field expected");
      if (!callback_rawctype_wait(L, cts, ctype_cid(finfo), &aid, cta))
	lj_err_caller(L, LJ_ERR_FFI_BADCBACK);
      ainfo = ctype_info_acq(cta);
      asize = ctype_size_acq(cta);
      isfp = ctype_isfp(ainfo);
      sz = (asize + CTSIZE_PTR-1) & ~(CTSIZE_PTR-1);
      n = sz / CTSIZE_PTR;  /* Number of GPRs or stack slots needed. */

      CALLBACK_HANDLE_REGARG  /* Handle register arguments. */

      /* Otherwise pass argument on stack. */
      if (CCALL_ALIGN_STACKARG && LJ_32 && sz == 8)
	nsp = (nsp + 1) & ~1u;  /* Align 64 bit argument on stack. */
      sp = &stack[nsp];
      nsp += n;

    done:
      if (LJ_BE && asize < CTSIZE_PTR
#if LJ_TARGET_MIPS64
	  && !(isfp && nsp)
#endif
	 )
	sp = (void *)((uint8_t *)sp + CTSIZE_PTR-asize);
      gcsteps += lj_cconv_tv_ct_l(L, cts, cta, aid, o++, sp);
    }
    fid = ctype_sib_acq(ctf);
  }
  L->top = o;
#if LJ_TARGET_X86
  /* Store stack adjustment for returns from non-cdecl callbacks. */
  if (ctype_cconv(ctinfo) != CTCC_CDECL) {
#if LJ_FR2
    (L->base-3)->u64 |= (nsp << (16+2));
#else
    (L->base-2)->u32.hi |= (nsp << (16+2));
#endif
  }
#endif
  while (gcsteps-- > 0)
    lj_gc_check(L);
}

/* Convert Lua object to callback result. */
static void callback_conv_result(CTState *cts, lua_State *L, TValue *o,
				 CCallbackRuntime *cb)
{
  CTypeID rid;
  CType ctrsnap;
  CType *ctr = &ctrsnap;
  CTInfo rinfo;
  CTSize rsize;
  if (!callback_rawctype_wait(L, cts, callback_frame_rid_load(L), &rid, ctr))
    lj_err_caller(L, LJ_ERR_FFI_BADCBACK);
#if LJ_TARGET_X86
  cb->gpr[2] = 0;
#endif
  rinfo = ctype_info_acq(ctr);
  rsize = ctype_size_acq(ctr);
  if (!ctype_isvoid(rinfo)) {
    uint8_t *dp = (uint8_t *)&cb->gpr[0];
#if CCALL_NUM_FPR
    if (ctype_isfp(rinfo))
      dp = (uint8_t *)&cb->fpr[0];
#endif
#if LJ_TARGET_ARM64 && LJ_BE
    if (ctype_isfp(rinfo) && rsize == sizeof(float))
      dp = (uint8_t *)&cb->fpr[0].f[1];
#endif
    lj_cconv_ct_tv_l(L, cts, ctr, rid, dp, o, 0);
#ifdef CALLBACK_HANDLE_RET
    CALLBACK_HANDLE_RET
#endif
    /* Extend returned integers to (at least) 32 bits. */
    if (ctype_isinteger_or_bool(rinfo) && rsize < 4) {
      if (rinfo & CTF_UNSIGNED)
	*(uint32_t *)dp = rsize == 1 ? (uint32_t)*(uint8_t *)dp :
					   (uint32_t)*(uint16_t *)dp;
      else
	*(int32_t *)dp = rsize == 1 ? (int32_t)*(int8_t *)dp :
					  (int32_t)*(int16_t *)dp;
    }
#if LJ_TARGET_MIPS64 || (LJ_TARGET_ARM64 && LJ_BE)
    /* Always sign-extend results to 64 bits. Even a soft-fp 'float'. */
    if (rsize <= 4 &&
	(LJ_ABI_SOFTFP || ctype_isinteger_or_bool(rinfo)))
      *(int64_t *)dp = (int64_t)*(int32_t *)dp;
#endif
#if LJ_TARGET_X86
    if (ctype_isfp(rinfo))
      cb->gpr[2] = rsize == sizeof(float) ? 1 : 2;
#endif
  }
}

static int ccallback_had_stopreq(CCallbackRuntime *cb)
{
  return ccallback_native_had_stopreq_acq(cb) != 0;
}

static int ccallback_fresh_stopreq(lua_State *L, uint32_t actions,
				   int had_stopreq)
{
  TGState *tg = L ? L2TG(L) : NULL;
  return (actions & LJ_GC2_HS_STOPREQ) ||
    (!had_stopreq && tg && lj_tg_flags_test_acq(tg, TGF_STOPREQ));
}

/* Enter callback. */
lua_State * LJ_FASTCALL lj_ccallback_enter(CTState *cts, void *cf,
					   CCallbackRuntime *cb)
{
  lua_State *L = ccallback_L_acq(cb);
  global_State *g = cts->g;
  TGState *tg = lj_thr_get_tg();
  uint8_t auto_detach = ccallback_auto_detach_acq(cb);
  uint32_t native_depth;
  uint32_t actions = 0;
  int had_stopreq = 0;
  void *ffi_call_func;
  if (LJ_UNLIKELY(tg == NULL || tg->gl != g || cb != &tg->cb ||
		  L == NULL || lj_tg_load_cur_L(tg) != L || L2TG(L) != tg))
    abort();
  if (lj_tg_jit_base(g)) {
    setstrV(L, L->top++, lj_err_str(L, LJ_ERR_FFI_BADCBACK));
    if (g->panic) g->panic(L);
    exit(EXIT_FAILURE);
  }
  lj_trace_abort(g);  /* Never record across callback. */
  /* Setup C frame. */
  cframe_prev(cf) = L->cframe;
  setcframe_L(cf, L);
  cframe_errfunc(cf) = -1;
  cframe_nres(cf) = 0;
  L->cframe = cf;
  native_depth = lj_tg_in_native_acq(tg);
  if (native_depth != 0) {
    ffi_call_func = lj_tg_ffi_call_func_acq(tg);
    if (ffi_call_func != NULL)
      lj_ctype_cb_blacklist(L, cts, ffi_call_func);
    had_stopreq = ccallback_had_stopreq(cb);
    lj_tg_in_native_store_rlx(tg, 0);
    actions = lj_safepoint_poll(L);
  }
  callback_conv_args(cts, L, cb);
  callback_frame_push(L, cb, L->base-1, native_depth, auto_detach);
  ccallback_auto_detach_rel(cb, 0);
  if (native_depth != 0) {
    if (ccallback_fresh_stopreq(L, actions, had_stopreq)) {
      callback_frame_top(cb)->native_depth = 0;
      lj_safepoint_checkstop(L, actions);
    }
  }
  return L;  /* Now call the function on this stack. */
}

/* Leave callback. */
void LJ_FASTCALL lj_ccallback_leave(CTState *cts, TValue *o,
				    CCallbackRuntime *cb)
{
  CCallbackFrame *frame = callback_frame_top(cb);
  lua_State *L = frame ? frame->L : ccallback_L_acq(cb);
  uint32_t native_depth = frame ? frame->native_depth : 0;
  uint8_t auto_detach = frame ? frame->auto_detach :
    ccallback_auto_detach_acq(cb);
  TGState *tg = lj_thr_get_tg();
  GCfunc *fn;
  TValue *obase;
  if (LJ_UNLIKELY(tg == NULL || tg->gl != cts->g || cb != &tg->cb ||
		  L == NULL || L2TG(L) != tg))
    abort();
  obase = L->base;
  L->base = L->top;  /* Keep continuation frame for throwing errors. */
  if (o >= L->base) {
    /* PC of RET* is lost. Point to last line for result conv. errors. */
    fn = curr_func(L);
    if (isluafunc(fn)) {
      GCproto *pt = funcproto(fn);
      setcframe_pc(L->cframe, proto_bc(pt)+pt->sizebc+1);
    }
  }
  callback_conv_result(cts, L, o, cb);
  /* Finally drop C frame and continuation frame. */
  L->top -= 2+2*LJ_FR2;
  L->base = obase;
  L->cframe = cframe_prev(L->cframe);
  ccallback_slot_rel(cb, 0);  /* Blacklist C function that called the callback. */
  callback_frame_pop(cb);
  ccallback_auto_detach_rel(cb, 0);
  if (native_depth != 0)
    lj_tg_in_native_rel(tg, native_depth);
  if (auto_detach)
    lj_threading_detach(L, 0);
}

/* -- C callback management ----------------------------------------------- */

MSize lj_ccallback_maxslot(void)
{
  return CALLBACK_MAX_SLOT;
}

void lj_ccallback_disown_state(lua_State *L)
{
  CTState *cts;
  lua_State **owner;
  MSize slot, sizeid;
  if (L == NULL)
    return;
  cts = ctype_ctsG(G(L));
  if (cts == NULL)
    return;
  owner = ctype_cb_owner_acq(cts);
  sizeid = ctype_cb_sizeid_acq(cts);
  if (owner == NULL || sizeid == 0)
    return;
  for (slot = 0; slot < sizeid; slot++)
    if (callback_owner_load(owner, slot) == L)
      (void)callback_owner_clear(owner, slot, L);
}

static CTypeID1 *callback_slots_init_l(lua_State *L, CTState *cts)
{
  if (ctype_cb_owner_acq(cts) == NULL) {
    lua_State **owner = lj_mem_newvec(L, CALLBACK_MAX_SLOT, lua_State *);
    memset(owner, 0, CALLBACK_MAX_SLOT*sizeof(lua_State *));
    ctype_cb_owner_rel(cts, owner);
  }
  if (ctype_cb_cbid_acq(cts) == NULL) {
    CTypeID1 *cbid = lj_mem_newvec(L, CALLBACK_MAX_SLOT, CTypeID1);
    memset(cbid, 0, CALLBACK_MAX_SLOT*sizeof(CTypeID1));
    ctype_cb_cbid_rel(cts, cbid);
  }
  if (ctype_cb_func_acq(cts) == NULL) {
    TValue *func = lj_mem_newvec(L, CALLBACK_MAX_SLOT, TValue);
    MSize i;
    for (i = 0; i < CALLBACK_MAX_SLOT; i++)
      setnilV(&func[i]);
    ctype_cb_func_rel(cts, func);
    ctype_cb_sizeid_rel(cts, CALLBACK_MAX_SLOT);
  }
  return ctype_cb_cbid_acq(cts);
}

void lj_ccallback_init_l(lua_State *L, CTState *cts)
{
#if CALLBACK_MAX_SLOT
  (void)callback_slots_init_l(L, cts);
  callback_mcode_new_l(L, cts);  /* 11.5: mcode read-only after FFI init. */
#else
  UNUSED(L); UNUSED(cts);
#endif
}

/* Get an unused slot in the callback slot table. */
static MSize callback_slot_claim_l(lua_State *L, CTState *cts)
{
  CTypeID1 *cbid = ctype_cb_cbid_acq(cts);
  lua_State **owner = ctype_cb_owner_acq(cts);
  TValue *func = callback_func_slots(cts);
  lua_State *carrier = NULL;
  MSize top, sizeid;
  sizeid = ctype_cb_sizeid_acq(cts);
  if (cbid == NULL || owner == NULL || func == NULL || sizeid == 0)
    lj_err_caller(L, LJ_ERR_FFI_CBACKOV);
  for (top = 0; top < sizeid; top++) {
    if (LJ_LIKELY(callback_cbid_load(cbid, top) == 0 &&
		  callback_owner_load(owner, top) == NULL)) {
      if (carrier == NULL)
	carrier = callback_carrier_new_l(L);
      if (LJ_LIKELY(callback_owner_claim(owner, top, carrier))) {
	callback_owner_barrier_l(L, carrier);
	return top;
      }
    }
  }
#if CALLBACK_MAX_SLOT
  if (top >= CALLBACK_MAX_SLOT)
#endif
    lj_err_caller(L, LJ_ERR_FFI_CBACKOV);
  return ~0u;
}

/* Check for function pointer and supported argument/result types. */
static int callback_checkfunc(lua_State *L, CTState *cts, CTypeID ptrid,
			      CTypeID *idp)
{
  int narg = 0;
  CType ptrsnap, ctsnap, ctrsnap;
  CType *ct = &ptrsnap;
  CTInfo info;
  CTSize size;
  if (!callback_ctype_snapshot_wait(L, cts, ptrid, ct))
    return 0;
  info = ctype_info_acq(ct);
  size = ctype_size_acq(ct);
  if (!ctype_isptr(info) || (LJ_64 && size != CTSIZE_PTR))
    return 0;
  ct = &ctsnap;
  if (!callback_rawctype_wait(L, cts, ctype_cid(info), idp, ct))
    return 0;
  info = ctype_info_acq(ct);
  if (ctype_isfunc(info)) {
    CTypeID rid;
    CType *ctr = &ctrsnap;
    CTypeID fid = ctype_sib_acq(ct);
    CTInfo rinfo;
    CTSize rsize;
    if (!callback_rawctype_wait(L, cts, ctype_cid(info), &rid, ctr))
      return 0;
    rinfo = ctype_info_acq(ctr);
    rsize = ctype_size_acq(ctr);
    if (!(ctype_isvoid(rinfo) || ctype_isenum(rinfo) ||
	  ctype_isptr(rinfo) || (ctype_isnum(rinfo) && rsize <= 8)))
      return 0;
    if ((info & CTF_VARARG))
      return 0;
    while (fid) {
      CType ctfsnap, ctasnap;
      CType *ctf = &ctfsnap;
      CTInfo finfo;
      if (!callback_ctype_snapshot_wait(L, cts, fid, ctf))
	return 0;
      finfo = ctype_info_acq(ctf);
      if (!ctype_isattrib(finfo)) {
	CType *cta = &ctasnap;
	CTypeID aid;
	CTInfo ainfo;
	CTSize asize;
	lj_assertCTS(ctype_isfield(finfo), "field expected");
	if (!callback_rawctype_wait(L, cts, ctype_cid(finfo), &aid, cta))
	  return 0;
	ainfo = ctype_info_acq(cta);
	asize = ctype_size_acq(cta);
	if (!(ctype_isenum(ainfo) || ctype_isptr(ainfo) ||
	      (ctype_isnum(ainfo) && asize <= 8)) ||
	    ++narg >= LUA_MINSTACK-3)
	  return 0;
      }
      fid = ctype_sib_acq(ctf);
    }
    return 1;
  }
  return 0;
}

/* Create a new callback and return the callback function pointer. */
void *lj_ccallback_new_l(lua_State *L, CTState *cts, CTypeID id, GCfunc *fn)
{
  CTypeID cbid_id = 0;
  if (callback_checkfunc(L, cts, id, &cbid_id)) {
    MSize slot;
    CTypeID1 *cbid;
    slot = callback_slot_claim_l(L, cts);
    cbid = ctype_cb_cbid_acq(cts);
    lj_ccallback_func_store_l(L, cts, slot, fn);
    callback_cbid_store(cbid, slot, cbid_id);
    return callback_slot2ptr(cts, slot);
  }
  return NULL;  /* Bad conversion. */
}

#endif

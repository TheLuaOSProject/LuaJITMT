/*
** Machine code management.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#define lj_mcode_c
#define LUA_CORE

#include "lj_obj.h"
#if LJ_HASJIT
#include "lj_atomic.h"
#include "lj_gc.h"
#include "lj_gc2.h"
#include "lj_err.h"
#include "lj_jit.h"
#include "lj_mcode.h"
#include "lj_safepoint.h"
#include "lj_trace.h"
#include "lj_dispatch.h"
#include "lj_prng.h"
#endif
#if LJ_HASJIT || LJ_HASFFI
#include "lj_vm.h"
#endif

/* -- OS-specific functions ----------------------------------------------- */

#if LJ_HASJIT || LJ_HASFFI

/* Define this if you want to run LuaJIT with Valgrind. */
#ifdef LUAJIT_USE_VALGRIND
#include <valgrind/valgrind.h>
#endif

#if LJ_TARGET_WINDOWS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#if LJ_TARGET_IOS
void sys_icache_invalidate(void *start, size_t len);
#endif

/* Synchronize data/instruction cache. */
void lj_mcode_sync(void *start, void *end)
{
#ifdef LUAJIT_USE_VALGRIND
  VALGRIND_DISCARD_TRANSLATIONS(start, (char *)end-(char *)start);
#endif
#if LJ_TARGET_X86ORX64
  UNUSED(start); UNUSED(end);
#elif LJ_TARGET_WINDOWS
  FlushInstructionCache(GetCurrentProcess(), start, (char *)end-(char *)start);
#elif LJ_TARGET_IOS
  sys_icache_invalidate(start, (char *)end-(char *)start);
#elif LJ_TARGET_PPC
  lj_vm_cachesync(start, end);
#elif defined(__GNUC__) || defined(__clang__)
  __clear_cache(start, end);
#else
#error "Missing builtin to flush instruction cache"
#endif
}

#endif

#if LJ_HASJIT

#if defined(__linux__) && LJ_TARGET_X64 && LUAJIT_SECURITY_MCODE != 0
#define LJ_MCODE_DUALMAP	1
#endif

#if LJ_TARGET_POSIX
#include <sys/types.h>
#include <sys/mman.h>
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS	MAP_ANON
#endif

/* Use stable MAP_JIT mcode on macOS 11+. */
#if LJ_TARGET_OSX && LJ_TARGET_X64 && LUAJIT_SECURITY_MCODE != 0 && \
    defined(MAP_JIT) && defined(__ENVIRONMENT_MAC_OS_X_VERSION_MIN_REQUIRED__) && \
    __ENVIRONMENT_MAC_OS_X_VERSION_MIN_REQUIRED__ >= 110000
#define LJ_MCODE_MAPJIT	1
#else
#define LJ_MCODE_MAPJIT	0
#endif
#else
#define LJ_MCODE_MAPJIT	0
#endif

void lj_mcode_init(global_State *g)
{
#if defined(__linux__) && LJ_TARGET_X64
  if (la_membarrier_register_synccore() == 0)
    la_store32_rel(&g->jit_mcode_synccore, 1);  /* 08 section 8.5. */
#else
  UNUSED(g);
#endif
}

#if LJ_TARGET_POSIX
static lua_State *mcode_native_enter(jit_State *J)
{
  lua_State *L = J ? J->L : NULL;
  if (L)
    lj_native_enter(L2TG(L));
  return L;
}

static void mcode_native_leave(lua_State *L)
{
  if (L)
    (void)lj_native_leave(L);
}
#endif

void lj_mcode_sync_core(jit_State *J)
{
#if defined(__linux__) && LJ_TARGET_X64
  global_State *g = J2G(J);
  if (LJ_LIKELY(la_load32_acq(&g->jit_mcode_synccore) != 0)) {
    lua_State *L = mcode_native_enter(J);
    if (LJ_UNLIKELY(la_membarrier_synccore() != 0))
      la_store32_rel(&g->jit_mcode_synccore, 0);  /* Fall back to serial path. */
    mcode_native_leave(L);
  }
#else
  UNUSED(J);
#endif
}

#if LUAJIT_SECURITY_MCODE != 0 && !LJ_MCODE_DUALMAP && !LJ_MCODE_MAPJIT
/* Protection twiddling failed. Probably due to kernel security. */
static LJ_NORET LJ_NOINLINE void mcode_protfail(jit_State *J)
{
  lua_CFunction panic = panicf_load(J2G(J));
  if (panic) {
    lua_State *L = J->L;
    setstrV(L, L->top++, lj_err_str(L, LJ_ERR_JITPROT));
    panic(L);
  }
  exit(EXIT_FAILURE);
}
#endif

#if LJ_TARGET_WINDOWS

#define MCPROT_RW	PAGE_READWRITE
#define MCPROT_RX	PAGE_EXECUTE_READ
#define MCPROT_RWX	PAGE_EXECUTE_READWRITE

static void *mcode_alloc_at(jit_State *J, uintptr_t hint, size_t sz, DWORD prot)
{
  UNUSED(J);
  return LJ_WIN_VALLOC((void *)hint, sz,
		       MEM_RESERVE|MEM_COMMIT|MEM_TOP_DOWN, prot);
}

static void mcode_free(void *p, size_t sz)
{
  UNUSED(sz);
  VirtualFree(p, 0, MEM_RELEASE);
}

static void mcode_setprot(jit_State *J, void *p, size_t sz, DWORD prot)
{
#if LUAJIT_SECURITY_MCODE != 0
  DWORD oprot;
  if (!LJ_WIN_VPROTECT(p, sz, prot, &oprot)) mcode_protfail(J);
#else
  UNUSED(J); UNUSED(p); UNUSED(sz); UNUSED(prot);
#endif
}

#elif LJ_TARGET_POSIX

#if LJ_MCODE_DUALMAP
#include <fcntl.h>
#include <sys/syscall.h>
#include <unistd.h>
#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC	0x0001U
#endif
#ifndef SYS_memfd_create
#ifdef __NR_memfd_create
#define SYS_memfd_create	__NR_memfd_create
#endif
#endif
#endif

#if LJ_MCODE_MAPJIT
#include <pthread.h>
#define MCMAP_CREATE	MAP_JIT
static uint32_t mcode_mapjit_wprotect_supported = 2u;

static int mcode_mapjit_wprotect(void)
{
  uint32_t supported = la_load32_acq(&mcode_mapjit_wprotect_supported);
  if (LJ_UNLIKELY(supported == 2u)) {
    supported = pthread_jit_write_protect_supported_np() ? 1u : 0u;
    la_store32_rel(&mcode_mapjit_wprotect_supported, supported);
  }
  return supported != 0;
}

static void mcode_mapjit_set_wprotect(int enabled)
{
  if (mcode_mapjit_wprotect())
    pthread_jit_write_protect_np(enabled);
}
#else
#define MCMAP_CREATE	0
#endif

#define MCPROT_RW	(PROT_READ|PROT_WRITE)
#define MCPROT_RX	(PROT_READ|PROT_EXEC)
#define MCPROT_RWX	(PROT_READ|PROT_WRITE|PROT_EXEC)
#ifdef PROT_MPROTECT
#define MCPROT_CREATE	(PROT_MPROTECT(MCPROT_RWX))
#elif MCMAP_CREATE
#define MCPROT_CREATE	PROT_EXEC
#else
#define MCPROT_CREATE	0
#endif

#if LJ_MCODE_DUALMAP
static int mcode_memfd_create(void)
{
#ifdef SYS_memfd_create
  return (int)syscall(SYS_memfd_create, "luajit-mcode", MFD_CLOEXEC);
#else
  return -1;
#endif
}

static void *mcode_alloc_dualmap(jit_State *J, uintptr_t hint, size_t sz)
{
  lua_State *L = mcode_native_enter(J);
  int fd = mcode_memfd_create();
  void *rx = MAP_FAILED, *rw = MAP_FAILED, *result = NULL;
  if (fd == -1)
    goto done;
  if (ftruncate(fd, (off_t)sz) != 0)
    goto done;
  rx = mmap((void *)hint, sz, MCPROT_RX, MAP_SHARED, fd, 0);
  if (rx == MAP_FAILED)
    goto done;
  rw = mmap(NULL, sz, MCPROT_RW, MAP_SHARED, fd, 0);
  if (rw == MAP_FAILED)
    goto done;
  ((MCLink *)rw)->rw = (MCode *)rw;
  result = rx;
done:
  if (fd != -1)
    close(fd);
  if (result == NULL) {
    if (rw != MAP_FAILED)
      munmap(rw, sz);
    if (rx != MAP_FAILED)
      munmap(rx, sz);
  }
  mcode_native_leave(L);
  return result;
}
#endif

static void *mcode_alloc_at(jit_State *J, uintptr_t hint, size_t sz, int prot)
{
#if LJ_MCODE_DUALMAP
  UNUSED(prot);
  return mcode_alloc_dualmap(J, hint, sz);
#else
  void *p;
  lua_State *L = mcode_native_enter(J);
  p = mmap((void *)hint, sz, prot|MCPROT_CREATE, MAP_PRIVATE|MAP_ANONYMOUS|MCMAP_CREATE, -1, 0);
  mcode_native_leave(L);
  if (p == MAP_FAILED) return NULL;
#if MCMAP_CREATE
  mcode_mapjit_set_wprotect(0);
#endif
  return p;
#endif
}

static void mcode_free(void *p, size_t sz)
{
  munmap(p, sz);
}

#if !LJ_MCODE_DUALMAP
static void mcode_setprot(jit_State *J, void *p, size_t sz, int prot)
{
#if LUAJIT_SECURITY_MCODE != 0
#if MCMAP_CREATE
  UNUSED(J); UNUSED(p); UNUSED(sz);
  mcode_mapjit_set_wprotect((prot & PROT_EXEC));
  return;
#else
  if (mprotect(p, sz, prot)) mcode_protfail(J);
#endif
#else
  UNUSED(J); UNUSED(p); UNUSED(sz); UNUSED(prot);
#endif
}
#endif

#else

#error "Missing OS support for explicit placement of executable memory"

#endif

#ifdef LUAJIT_MCODE_TEST
/* Test wrapper for mcode allocation. DO NOT ENABLE in production! Try:
**   LUAJIT_MCODE_TEST=hhhhhhhhhhhhhhhh luajit -jv main.lua
**   LUAJIT_MCODE_TEST=F luajit -jv main.lua
*/
static void *mcode_alloc_at_TEST(jit_State *J, uintptr_t hint, size_t sz, int prot)
{
  static int test_ofs = 0;
  static const char *test_str;
  if (!test_str) {
    test_str = getenv("LUAJIT_MCODE_TEST");
    if (!test_str) test_str = "";
  }
  switch (test_str[test_ofs]) {
  case 'a':  /* OK for one allocation. */
    test_ofs++;
    /* fallthrough */
  case '\0':  /* EOS: OK for any further allocations. */
    break;
  case 'h':  /* Ignore one hint. */
    test_ofs++;
    /* fallthrough */
  case 'H':  /* Ignore any further hints. */
    hint = 0u;
    break;
  case 'r':  /* Randomize one hint. */
    test_ofs++;
    /* fallthrough */
  case 'R':  /* Randomize any further hints. */
    hint = lj_prng_u64(&J2TG(J)->prng) & ~(uintptr_t)0xffffu;
    hint &= ((uintptr_t)1 << (LJ_64 ? 47 : 31)) - 1;
    break;
  case 'f':  /* Fail one allocation. */
    test_ofs++;
    /* fallthrough */
  default:  /* 'F' or unknown: Fail any further allocations. */
    return NULL;
  }
  return mcode_alloc_at(J, hint, sz, prot);
}
#define mcode_alloc_at(J, hint, sz, prot) \
  mcode_alloc_at_TEST((J), (hint), (sz), (prot))
#endif

static void mcode_free_mapping(MCode *area, size_t sz)
{
  MCode *rw = lj_mcode_area_rw(area);
  if (rw && rw != area)
    mcode_free(rw, sz);
  mcode_free(area, sz);
}

/* -- MCode area mode/protection ------------------------------------------ */

#if LUAJIT_SECURITY_MCODE == 0

/* Define this ONLY if page protection twiddling becomes a bottleneck.
**
** It's generally considered to be a potential security risk to have
** pages with simultaneous write *and* execute access in a process.
**
** Do not even think about using this mode for server processes or
** apps handling untrusted external data.
**
** The security risk is not in LuaJIT itself -- but if an adversary finds
** any *other* flaw in your C application logic, then any RWX memory pages
** simplify writing an exploit considerably.
*/
#define MCPROT_GEN	MCPROT_RWX
#define MCPROT_RUN	MCPROT_RWX

static void mcode_set_current_mode(jit_State *J, int prot)
{
  UNUSED(J); UNUSED(prot);
}

static void mcode_set_area_mode(jit_State *J, MCode *area, int prot)
{
  UNUSED(J); UNUSED(area); UNUSED(prot);
}

#else

/* This is the default behaviour and much safer:
**
** Most of the time the memory pages holding machine code are executable,
** but NONE of them is writable.
**
** The current memory area is marked read-write (but NOT executable) only
** during the short time window while the assembler generates machine code.
*/
#define MCPROT_GEN	MCPROT_RW
#define MCPROT_RUN	MCPROT_RX

#if (defined(__linux__) && LJ_TARGET_X64) || LJ_MCODE_MAPJIT
/* M6 bridge: keep published mcode execute-stable for peer TGs. The Linux/x64
** path uses a memfd dual-map W^X write view. The macOS/x64 path uses MAP_JIT
** to avoid toggling execute permission out from under peer threads; on Intel
** macOS the pthread write-protect toggle may be a no-op, so this is an
** execute-stability path, not a separated W^X map guarantee. */
#define LJ_MCODE_EXEC_STABLE	1
#endif

/* Change generation mode for the current MCode area. */
static void mcode_set_current_mode(jit_State *J, int prot)
{
#if LJ_MCODE_DUALMAP
  J->mcprot = prot;  /* Writes use the RW alias; the RX map stays executable. */
#else
  if (J->mcprot != prot) {
    mcode_setprot(J, J->mcarea, J->szmcarea, prot);
    J->mcprot = prot;
  }
#endif
}

static void mcode_set_area_mode(jit_State *J, MCode *area, int prot)
{
  if (J->mcarea == area) {
    mcode_set_current_mode(J, prot);
  } else {
#if LJ_MCODE_DUALMAP
    UNUSED(J); UNUSED(area); UNUSED(prot);
#else
    mcode_setprot(J, area, ((MCLink *)area)->size, prot);
#endif
  }
}

#endif

/* -- MCode area allocation ----------------------------------------------- */

#ifdef LJ_TARGET_JUMPRANGE

#define MCODE_RANGE64	((1u << LJ_TARGET_JUMPRANGE) - 0x10000u)

/* Set a memory range for mcode allocation with addr in the middle. */
static void mcode_setrange(jit_State *J, uintptr_t addr)
{
#if LJ_TARGET_MIPS
  /* Use the whole 256MB-aligned region. */
  J->mcmin = addr & ~(uintptr_t)((1u << LJ_TARGET_JUMPRANGE) - 1);
  J->mcmax = J->mcmin + (1u << LJ_TARGET_JUMPRANGE);
#else
  /* Every address in the 64KB-aligned range should be able to reach
  ** any other, so MCODE_RANGE64 is only half the (signed) branch range.
  */
  J->mcmin = (addr - (MCODE_RANGE64 >> 1) + 0xffffu) & ~(uintptr_t)0xffffu;
  J->mcmax = J->mcmin + MCODE_RANGE64;
#endif
  /* Avoid wrap-around and the 64KB corners. */
  if (addr < J->mcmin || !J->mcmin) J->mcmin = 0x10000u;
  if (addr > J->mcmax) J->mcmax = ~(uintptr_t)0xffffu;
}

/* Check if an address is in range of the mcode allocation range. */
static LJ_AINLINE int mcode_inrange(jit_State *J, uintptr_t addr, size_t sz)
{
  /* Take care of unsigned wrap-around of addr + sz, too. */
  return addr >= J->mcmin && addr + sz >= J->mcmin && addr + sz <= J->mcmax;
}

/* Get memory within a specific jump range in 64 bit mode. */
static void *mcode_alloc(jit_State *J, size_t sz)
{
  uintptr_t hint;
  int i = 0, j;
  if (!J->mcmin)  /* Place initial range near the interpreter code. */
    mcode_setrange(J, (uintptr_t)(void *)lj_vm_exit_handler);
  else if (!J->mcmax)  /* Switch to a new range (already flushed). */
    goto newrange;
  /* First try a contiguous area below the last one (if in range). */
  hint = (uintptr_t)J->mcarea - sz;
  if (!mcode_inrange(J, hint, sz))  /* Also takes care of NULL J->mcarea. */
    goto probe;
  for (; i < 16; i++) {
    void *p = mcode_alloc_at(J, hint, sz, MCPROT_GEN);
    if (mcode_inrange(J, (uintptr_t)p, sz))
      return p;  /* Success. */
    else if (p)
#if LJ_MCODE_DUALMAP
      mcode_free_mapping((MCode *)p, sz);  /* Free badly placed area. */
#else
      mcode_free(p, sz);  /* Free badly placed area. */
#endif
  probe:
    /* Next try probing 64KB-aligned pseudo-random addresses. */
    j = 0;
    do {
      hint = J->mcmin + (lj_prng_u64(&J2TG(J)->prng) & MCODE_RANGE64);
      if (++j > 15) goto fail;
    } while (!mcode_inrange(J, hint, sz));
  }
fail:
  if (!J->mcarea) {  /* Switch to a new range now. */
    void *p;
  newrange:
    p = mcode_alloc_at(J, 0, sz, MCPROT_GEN);
    if (p) {
      mcode_setrange(J, (uintptr_t)p + (sz >> 1));
      return p;  /* Success. */
    }
  } else {
    J->mcmax = 0;  /* Switch to a new range after the flush. */
  }
  return NULL;
}

#else

/* All memory addresses are reachable by relative jumps. */
static void *mcode_alloc(jit_State *J, size_t sz)
{
#if defined(__OpenBSD__) || defined(__NetBSD__) || LJ_TARGET_UWP
  /* Allow better executable memory allocation for OpenBSD W^X mode. */
  void *p = mcode_alloc_at(J, 0, sz, MCPROT_RUN);
  if (p) mcode_setprot(J, p, sz, MCPROT_GEN);
#else
  void *p = mcode_alloc_at(J, 0, sz, MCPROT_GEN);
#endif
  return p;
}

#endif

/* -- MCode area management ----------------------------------------------- */

static LJ_AINLINE size_t mcode_default_size(jit_State *J)
{
  return (size_t)jit_param_acq(J, JIT_P_sizemcode) << 10;
}

static LJ_AINLINE MCode *mcode_register_area(jit_State *J, MCode *area,
					     size_t sz, MCode *bot)
{
  MCode *rwbot = lj_mcode_rx2rw(area, bot);
  MCode *newbot = (MCode *)lj_err_register_mcode(area, sz, (uint8_t *)bot,
						 (uint8_t *)rwbot);
  UNUSED(J); UNUSED(sz);  /* Some no-unwind configurations elide registration. */
  return newbot;
}

static void mcode_active_push(jit_State *J, MCodeRetire *ret)
{
  MCodeRetire *head = mcode_active_head_acq(J);
  do {
    mcode_retired_next_rel(ret, head);
  } while (!mcode_active_head_cas(J, &head, ret));
}

/* Mark raw retirement nodes across an owner-list publication race. */
static void mcode_preserve_list(global_State *g, MCodeRetire *ret)
{
  for (; ret != NULL && lj_gc2_mem_registered(g, ret);
       ret = mcode_retired_next_acq(ret))
    (void)lj_gc2_markmem_registered(g, ret);
}

/* Allocate a new MCode area and its preowned, GC-visible retirement node. */
static void mcode_allocarea(jit_State *J, size_t sz)
{
  global_State *g = J2G(J);
  MCode *oldarea = J->mcarea;
  MCode *area;
  MCode *rwarea;
  MCode *bot;
  MCLink *rwlink;
  MCodeRetire *ret;

  /* Keep both fallible resources local until the complete owner pair exists. */
  area = (MCode *)mcode_alloc(J, sz);
  if (LJ_UNLIKELY(area == NULL))
    lj_trace_err(J, LJ_TRERR_MCODEAL);
  ret = (MCodeRetire *)lj_mem_new_nothrow(J->L, sizeof(MCodeRetire));
  if (LJ_UNLIKELY(ret == NULL)) {
    mcode_free_mapping(area, sz);
    lj_trace_err(J, LJ_TRERR_MCODEAL);
  }
  ret->area = NULL;
  ret->size = sz;
  ret->retire_epoch = MCODE_RETIRE_EPOCH_ACTIVE;
  mcode_retired_next_rel(ret, NULL);
  (void)lj_gc2_markmem_registered(g, ret);
#if LJ_MCODE_DUALMAP
  rwarea = lj_mcode_area_rw(area);
#else
  rwarea = area;
#endif
  rwlink = (MCLink *)rwarea;
  mcode_area_next_rel(rwarea, oldarea);
  rwlink->size = sz;
  rwlink->rw = rwarea;
  bot = mcode_register_area(J, area, sz,
			    (MCode *)((char *)area + sizeof(MCLink)));
  ret->area = area;

  /*
  ** Own the sidecar before publishing the area. GC root scans mark this list;
  ** the allocation-edge marks close both sides of a concurrent sweep/CAS race.
  ** Mark only the newly published node here: walking the prior active list on
  ** every area allocation would make a many-area recorder grow quadratically.
  */
  (void)lj_gc2_markmem_registered(g, ret);
  mcode_active_push(J, ret);
  (void)lj_gc2_markmem_registered(g, ret);

  J->mcarea = area;
  J->szmcarea = sz;
  J->mcprot = MCPROT_GEN;
  J->mctop = (MCode *)((char *)area + sz);
  J->mcbot = bot;
  J->szallmcarea += sz;
}

static void mcode_retired_push(jit_State *J, MCodeRetire *ret)
{
  MCodeRetire *tail = ret;
  MCodeRetire *next;
  MCodeRetire *head;
  if (!ret)
    return;
  while ((next = mcode_retired_next_acq(tail)) != NULL)
    tail = next;
  head = mcode_retired_head_acq(J);
  do {
    mcode_retired_next_rel(tail, head);
  } while (!mcode_retired_head_cas(J, &head, ret));
  /* 08 section 8.7 mcode SMR. */
}

static void mcode_freearea_direct(global_State *g, MCode *area, size_t size)
{
  jit_State *J = G2J(g);
  lj_err_deregister_mcode(area, size, (uint8_t *)area + sizeof(MCLink));
  mcode_free_mapping(area, size);
  J->szallmcarea -= size;
}

static void mcode_freearea(global_State *g, MCodeRetire *ret)
{
  mcode_freearea_direct(g, ret->area, ret->size);
  lj_mem_freet(g, ret);
}

static LJ_AINLINE int mcode_retire_ready(MCodeRetire *ret,
					  uint64_t completed_epoch)
{
  uint64_t retire_epoch = la_load64_acq(&ret->retire_epoch);
  return retire_epoch != MCODE_RETIRE_EPOCH_ACTIVE &&
	 completed_epoch >= retire_epoch &&
	 completed_epoch - retire_epoch >= LJ_FLUSH_EPOCHS;
}

/* Retire all active MCode areas. */
void lj_mcode_free(jit_State *J)
{
  global_State *g = J2G(J);
  MCode *mc = J->mcarea;
  MCodeRetire *retired, *ret;
  uint64_t epoch;
  if (!mc)
    return;
  epoch = lj_gc2_retire_epoch(g);
  retired = mcode_active_head_xchg_acqrel(J, NULL);
  lj_assertJ(retired != NULL, "active mcode area has no retirement owner");
#ifdef LUA_USE_ASSERT
  {
    MCode *area = mc;
    MCodeRetire *owner = retired;
    while (area != NULL && owner != NULL) {
      lj_assertJ(owner->area == area && owner->size == ((MCLink *)area)->size,
		 "mcode area/retirement owner order mismatch");
      area = mcode_area_next_acq(area);
      owner = mcode_retired_next_acq(owner);
    }
    lj_assertJ(area == NULL && owner == NULL,
	       "mcode area/retirement owner count mismatch");
  }
#endif
  for (ret = retired; ret != NULL; ret = mcode_retired_next_acq(ret)) {
    lj_assertJ(ret->area != NULL &&
	       la_load64_acq(&ret->retire_epoch) == MCODE_RETIRE_EPOCH_ACTIVE,
	       "invalid active mcode retirement owner");
    la_store64_rel(&ret->retire_epoch, epoch);
  }
  /* Publish ownership before detaching the executable-area chain. */
  mcode_preserve_list(g, retired);
  mcode_retired_push(J, retired);
  mcode_preserve_list(g, retired);
  J->mcarea = NULL;
  J->mctop = J->mcbot = NULL;
  J->szmcarea = 0;
}

void lj_mcode_freeall(global_State *g)
{
  jit_State *J;
  MCodeRetire *active;
  if (!g)
    return;
  J = G2J(g);
  active = mcode_active_head_xchg_acqrel(J, NULL);
  J->mcarea = NULL;
  J->mctop = J->mcbot = NULL;
  J->szmcarea = 0;
  while (active) {
    if (LJ_UNLIKELY(!lj_gc2_mem_registered(g, active))) {
      lj_assertG(0, "invalid terminal detached active mcode owner");
      abort();
    }
    MCodeRetire *next = mcode_retired_next_acq(active);
    mcode_freearea(g, active);
    active = next;
  }
  lj_mcode_freeretired(g);
}

uint32_t lj_mcode_reclaim_retired(global_State *g, uint64_t completed_epoch)
{
  jit_State *J;
  MCodeRetire *ret;
  uint32_t reclaimed = 0;
  int retry_same_epoch = 0;
  if (!g || completed_epoch == 0)
    return 0;
  J = G2J(g);
  if (!lj_gc2_jit_reclaim_context_acq(g) || !lj_jit_token_held(J))
    return 0;
  if (J->mcode_reclaim_epoch == completed_epoch)
    return 0;
  lj_assertG(lj_gc2_jit_reclaim_context_acq(g) && lj_jit_token_held(J),
	     "mcode retire-list detach without exclusive reclaim gate");
  ret = mcode_retired_head_xchg_acqrel(J, NULL);
  while (ret) {
    if (LJ_UNLIKELY(
	!lj_gc2_mem_registered_known_reclaim_held(g, ret))) {
      /* xchg made this thread the only list owner. Never drop an invalid head
      ** or read its successor without the exact reclaimer certificate. */
      lj_assertG(0, "invalid detached mcode retirement owner");
      abort();
    }
    MCodeRetire *next = mcode_retired_next_acq(ret);
    mcode_retired_next_rel(ret, NULL);
    if (mcode_retire_ready(ret, completed_epoch)) {
      if (!lj_trace_retired_mcode_refs(g, ret->area, ret->size)) {
	mcode_freearea(g, ret);
	reclaimed++;
      } else {
	/* A ready trace can shed its last area reference later in this same epoch.
	** Keep that retry eligible; only an entirely epoch-young list is stable. */
	retry_same_epoch = 1;
	mcode_retired_push(J, ret);
      }
    } else {
      mcode_retired_push(J, ret);
    }
    ret = next;
  }
  if (!retry_same_epoch)
    J->mcode_reclaim_epoch = completed_epoch;
  return reclaimed;
}

void lj_mcode_freeretired(global_State *g)
{
  jit_State *J;
  MCodeRetire *ret;
  if (!g)
    return;
  J = G2J(g);
  ret = mcode_retired_head_xchg_acqrel(J, NULL);
  while (ret) {
    if (LJ_UNLIKELY(!lj_gc2_mem_registered(g, ret))) {
      lj_assertG(0, "invalid terminal detached retired mcode owner");
      abort();
    }
    MCodeRetire *next = mcode_retired_next_acq(ret);
    mcode_freearea(g, ret);
    ret = next;
  }
}

typedef struct MCodeRootCycleGuard {
  const void *anchor;
  uint64_t power;
  uint64_t length;
} MCodeRootCycleGuard;

static LJ_AINLINE void mcode_root_cycle_init(MCodeRootCycleGuard *guard,
					     const void *head)
{
  guard->anchor = head;
  guard->power = 1;
  guard->length = 0;
}

static LJ_AINLINE int mcode_root_cycle_step(MCodeRootCycleGuard *guard,
					    const void *next)
{
  if (next && next == guard->anchor)
    return 0;
  guard->length++;
  if (guard->length == guard->power) {
    guard->anchor = next;
    guard->length = 0;
    if (guard->power <= ~(uint64_t)0 / 2u)
      guard->power *= 2u;
  }
  return 1;
}

int lj_mcode_markretired(global_State *g, int gc2)
{
  jit_State *J;
  MCodeRetire *ret, *next;
  MCodeRootCycleGuard guard;
  if (!g)
    return 1;
  J = G2J(g);
  ret = mcode_active_head_acq(J);
  mcode_root_cycle_init(&guard, ret);
  while (ret != NULL && lj_gc2_mem_registered(g, ret)) {
    next = mcode_retired_next_acq(ret);
    if (gc2)
      lj_gc2_markmem_registered(g, ret);
    else
      lj_gc_arena_markmem_registered(g, ret);
    ret = next;
    if (LJ_UNLIKELY(!mcode_root_cycle_step(&guard, ret)))
      return 0;
  }
  if (LJ_UNLIKELY(ret != NULL))
    return 0;
  ret = mcode_retired_head_acq(J);
  mcode_root_cycle_init(&guard, ret);
  while (ret != NULL && lj_gc2_mem_registered(g, ret)) {
    next = mcode_retired_next_acq(ret);
    if (gc2)
      lj_gc2_markmem_registered(g, ret);
    else
      lj_gc_arena_markmem_registered(g, ret);
    ret = next;
    if (LJ_UNLIKELY(!mcode_root_cycle_step(&guard, ret)))
      return 0;
  }
  return ret == NULL;
}

/* -- MCode transactions -------------------------------------------------- */

/* Reserve the remainder of the current MCode area. */
MCode *lj_mcode_reserve(jit_State *J, MCode **lim)
{
  if (!J->mcarea)
    mcode_allocarea(J, mcode_default_size(J));
  else
    mcode_set_current_mode(J, MCPROT_GEN);
  *lim = J->mcbot;
  return J->mctop;
}

/* Commit the top part of the current MCode area. */
void lj_mcode_commit(jit_State *J, MCode *top)
{
  J->mctop = top;
  mcode_set_current_mode(J, MCPROT_RUN);
}

/* Abort the reservation. */
void lj_mcode_abort(jit_State *J)
{
  if (J->mcarea)
    mcode_set_current_mode(J, MCPROT_RUN);
}

/* Set/reset protection to allow patching of MCode areas. */
MCode *lj_mcode_patch(jit_State *J, MCode *ptr, int finish)
{
  if (finish) {
    mcode_set_area_mode(J, ptr, MCPROT_RUN);
    return NULL;
  } else {
    uintptr_t base = (uintptr_t)J->mcarea, addr = (uintptr_t)ptr;
    /* Try current area first to use the protection cache. */
    if (addr >= base && addr < base + J->szmcarea) {
      mcode_set_current_mode(J, MCPROT_GEN);
      return (MCode *)base;
    }
    /* Otherwise search through the list of MCode areas. */
    for (;;) {
      base = (uintptr_t)mcode_area_next_acq((MCode *)base);
      lj_assertJ(base != 0, "broken MCode area chain");
      if (addr >= base && addr < base + ((MCLink *)base)->size) {
	mcode_set_area_mode(J, (MCode *)base, MCPROT_GEN);
	return (MCode *)base;
      }
    }
  }
}

/* Limit of MCode reservation reached. */
void lj_mcode_limiterr(jit_State *J, size_t need)
{
  size_t sizemcode, maxmcode;
  lj_mcode_abort(J);
  sizemcode = mcode_default_size(J);
  maxmcode = (size_t)jit_param_acq(J, JIT_P_maxmcode) << 10;
  if (need * sizeof(MCode) > sizemcode)
    lj_trace_err(J, LJ_TRERR_MCODEOV);  /* Too long for any area. */
  if (J->szallmcarea + sizemcode > maxmcode)
    lj_trace_err(J, LJ_TRERR_MCODEAL);
  mcode_allocarea(J, sizemcode);
  lj_trace_err(J, LJ_TRERR_MCODELM);  /* Retry with new area. */
}

#endif

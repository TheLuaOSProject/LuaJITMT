/*
** lj_atomic.h — atomics layer for LuaJIT-MT.
** Normative implementation of 02_memory_model.md §2.6.
** Drop into src/ unmodified at milestone M1.
**
** Conventions:
**  - operates on PLAIN types (uint8_t/uint32_t/uint64_t/void*), not
**    _Atomic, so struct layouts stay identical to upstream;
**  - suffix = memory order: _rlx relaxed, _acq acquire, _rel release,
**    _seq seq_cst;
**  - every call site must carry a comment naming the invariant (I-n)
**    or doc section that justifies its ordering.
*/
#ifndef _LJ_ATOMIC_H
#define _LJ_ATOMIC_H

#include <stdint.h>
#include <stddef.h>

#if !defined(__GNUC__) && !defined(__clang__)
#error "lj_atomic.h requires GCC or Clang __atomic builtins"
#endif

#define LA_INLINE static inline __attribute__((always_inline))

/* ---- loads ---------------------------------------------------------- */
LA_INLINE uint8_t  la_load8_rlx (const uint8_t  *p){return __atomic_load_n(p,__ATOMIC_RELAXED);}
LA_INLINE uint32_t la_load32_rlx(const uint32_t *p){return __atomic_load_n(p,__ATOMIC_RELAXED);}
LA_INLINE uint64_t la_load64_rlx(const uint64_t *p){return __atomic_load_n(p,__ATOMIC_RELAXED);}
LA_INLINE uint8_t  la_load8_acq (const uint8_t  *p){return __atomic_load_n(p,__ATOMIC_ACQUIRE);}
LA_INLINE uint16_t la_load16_acq(const uint16_t *p){return __atomic_load_n(p,__ATOMIC_ACQUIRE);}
LA_INLINE uint32_t la_load32_acq(const uint32_t *p){return __atomic_load_n(p,__ATOMIC_ACQUIRE);}
LA_INLINE uint64_t la_load64_acq(const uint64_t *p){return __atomic_load_n(p,__ATOMIC_ACQUIRE);}
LA_INLINE void    *la_loadptr_rlx(void *const *p) {return __atomic_load_n((void *const *)p,__ATOMIC_RELAXED);}
LA_INLINE void    *la_loadptr_acq(void *const *p) {return __atomic_load_n((void *const *)p,__ATOMIC_ACQUIRE);}
#define la_loadfunc_acq(p) __atomic_load_n((p),__ATOMIC_ACQUIRE)

/* ---- stores --------------------------------------------------------- */
LA_INLINE void la_store8_rlx (uint8_t  *p,uint8_t  v){__atomic_store_n(p,v,__ATOMIC_RELAXED);}
LA_INLINE void la_store8_rel (uint8_t  *p,uint8_t  v){__atomic_store_n(p,v,__ATOMIC_RELEASE);}
LA_INLINE void la_store32_rlx(uint32_t *p,uint32_t v){__atomic_store_n(p,v,__ATOMIC_RELAXED);}
LA_INLINE void la_store32_rel(uint32_t *p,uint32_t v){__atomic_store_n(p,v,__ATOMIC_RELEASE);}
LA_INLINE void la_store64_rlx(uint64_t *p,uint64_t v){__atomic_store_n(p,v,__ATOMIC_RELAXED);}
LA_INLINE void la_store64_rel(uint64_t *p,uint64_t v){__atomic_store_n(p,v,__ATOMIC_RELEASE);}
LA_INLINE void la_store16_rel(uint16_t *p,uint16_t v){__atomic_store_n(p,v,__ATOMIC_RELEASE);}
LA_INLINE void la_storeptr_rlx(void **p,void *v){__atomic_store_n(p,v,__ATOMIC_RELAXED);}
LA_INLINE void la_storeptr_rel(void **p,void *v){__atomic_store_n(p,v,__ATOMIC_RELEASE);}
#define la_storefunc_rel(p,v) __atomic_store_n((p),(v),__ATOMIC_RELEASE)

/* ---- CAS (strong; returns 1 on success; *exp updated on failure) ---- */
LA_INLINE int la_cas32(uint32_t *p,uint32_t *exp,uint32_t des,int mo_s,int mo_f)
{return __atomic_compare_exchange_n(p,exp,des,0,mo_s,mo_f);}
LA_INLINE int la_cas8(uint8_t *p,uint8_t *exp,uint8_t des,int mo_s,int mo_f)
{return __atomic_compare_exchange_n(p,exp,des,0,mo_s,mo_f);}
LA_INLINE int la_cas16(uint16_t *p,uint16_t *exp,uint16_t des,int mo_s,int mo_f)
{return __atomic_compare_exchange_n(p,exp,des,0,mo_s,mo_f);}
LA_INLINE int la_cas64(uint64_t *p,uint64_t *exp,uint64_t des,int mo_s,int mo_f)
{return __atomic_compare_exchange_n(p,exp,des,0,mo_s,mo_f);}
LA_INLINE int la_casptr(void **p,void **exp,void *des,int mo_s,int mo_f)
{return __atomic_compare_exchange_n(p,exp,des,0,mo_s,mo_f);}
/* convenience orders */
#define LA_RLX __ATOMIC_RELAXED
#define LA_ACQ __ATOMIC_ACQUIRE
#define LA_REL __ATOMIC_RELEASE
#define LA_ACQ_REL __ATOMIC_ACQ_REL
#define LA_SEQ __ATOMIC_SEQ_CST

/* 128-bit CAS for tagged pointers (Treiber stacks, 04 §4.5).
** x86-64: cmpxchg16b (compile with -mcx16). ARM64: LSE casp or LL/SC pair.
** Represented as a 16-byte aligned struct to avoid __int128 strict-alias
** pitfalls in user code; internally uses __int128 builtin. */
typedef struct la_u128 { uint64_t lo, hi; } __attribute__((aligned(16))) la_u128;
LA_INLINE int la_cas128(la_u128 *p, la_u128 *exp, la_u128 des)
{
#if defined(__x86_64__) && defined(__GNUC__) && !defined(__clang__)
  /* gcc ≥7 routes the 16-byte builtin through libatomic even with -mcx16
  ** (PR80878). libatomic is lock-free here, but the PLT call costs and
  ** the lockless fast path wants the real instruction: inline it. */
  uint8_t ok;
  __asm__ __volatile__("lock cmpxchg16b %1"
                       : "=@ccz"(ok), "+m"(*p), "+a"(exp->lo), "+d"(exp->hi)
                       : "b"(des.lo), "c"(des.hi)
                       : "memory");
  return (int)ok;
#else
  __extension__ typedef unsigned __int128 u128;
  u128 e = ((u128)exp->hi << 64) | exp->lo;
  u128 d = ((u128)des.hi << 64) | des.lo;
  int ok = __atomic_compare_exchange_n((u128 *)p, &e, d, 0,
                                       __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
  if (!ok) { exp->lo = (uint64_t)e; exp->hi = (uint64_t)(e >> 64); }
  return ok;
#endif
}

/* ---- fetch ops ------------------------------------------------------ */
LA_INLINE uint32_t la_add32_rlx(uint32_t *p,uint32_t v){return __atomic_fetch_add(p,v,__ATOMIC_RELAXED);}
LA_INLINE uint64_t la_add64_rlx(uint64_t *p,uint64_t v){return __atomic_fetch_add(p,v,__ATOMIC_RELAXED);}
LA_INLINE uint32_t la_sub32_rlx(uint32_t *p,uint32_t v){return __atomic_fetch_sub(p,v,__ATOMIC_RELAXED);}
LA_INLINE uint64_t la_sub64_rlx(uint64_t *p,uint64_t v){return __atomic_fetch_sub(p,v,__ATOMIC_RELAXED);}
LA_INLINE uint32_t la_sub32_acqrel(uint32_t *p,uint32_t v){return __atomic_fetch_sub(p,v,__ATOMIC_ACQ_REL);}
LA_INLINE uint8_t  la_or8_rlx (uint8_t  *p,uint8_t  v){return __atomic_fetch_or(p,v,__ATOMIC_RELAXED);}
LA_INLINE uint8_t  la_and8_rlx(uint8_t  *p,uint8_t  v){return __atomic_fetch_and(p,v,__ATOMIC_RELAXED);}
LA_INLINE uint64_t la_or64_rlx(uint64_t *p,uint64_t v){return __atomic_fetch_or(p,v,__ATOMIC_RELAXED);}
LA_INLINE uint64_t la_and64_rlx(uint64_t *p,uint64_t v){return __atomic_fetch_and(p,v,__ATOMIC_RELAXED);}
LA_INLINE uint32_t la_xchg32_acqrel(uint32_t *p,uint32_t v){return __atomic_exchange_n(p,v,__ATOMIC_ACQ_REL);}
LA_INLINE uint64_t la_xchg64_acqrel(uint64_t *p,uint64_t v){return __atomic_exchange_n(p,v,__ATOMIC_ACQ_REL);}
LA_INLINE void    *la_xchgptr_acqrel(void **p,void *v){return __atomic_exchange_n(p,v,__ATOMIC_ACQ_REL);}

/* Test-and-set one bit in a 64-bit word; returns previous bit value.
** The mark-bitmap primitive (05 §5.6.1): relaxed is sufficient because
** mark publication is ordered by the grey-push release / steal acquire. */
LA_INLINE int la_bit_test_and_set64(uint64_t *word, unsigned bit)
{
  uint64_t m = 1ull << (bit & 63);
  return (__atomic_fetch_or(word, m, __ATOMIC_RELAXED) & m) != 0;
}

/* ---- fences --------------------------------------------------------- */
LA_INLINE void la_fence_acq(void){__atomic_thread_fence(__ATOMIC_ACQUIRE);}
LA_INLINE void la_fence_rel(void){__atomic_thread_fence(__ATOMIC_RELEASE);}
LA_INLINE void la_fence_seq(void){__atomic_thread_fence(__ATOMIC_SEQ_CST);}

/* ---- cpu pause ------------------------------------------------------ */
LA_INLINE void la_cpu_pause(void)
{
#if defined(__x86_64__) || defined(__i386__)
  __builtin_ia32_pause();
#elif defined(__aarch64__)
  __asm__ __volatile__("yield" ::: "memory");
#else
  __asm__ __volatile__("" ::: "memory");
#endif
}

/* ---- futex + membarrier (Linux) ------------------------------------- */
#if defined(__linux__)
#define LA_HAS_FUTEX 1
#include <unistd.h>
#include <sys/syscall.h>
#include <linux/futex.h>
#include <time.h>
#include <errno.h>

/* Wait while *p == val. ns<0: infinite. Returns 0 woken/changed,
** -1 with errno on error (ETIMEDOUT/EINTR are normal). */
LA_INLINE int la_futex_wait(uint32_t *p, uint32_t val, int64_t ns)
{
  struct timespec ts, *tp = 0;
  if (ns >= 0) { ts.tv_sec = ns/1000000000; ts.tv_nsec = ns%1000000000; tp=&ts; }
  return (int)syscall(SYS_futex, p, FUTEX_WAIT_PRIVATE, val, tp, 0, 0);
}
LA_INLINE int la_futex_wake(uint32_t *p, int n)
{
  return (int)syscall(SYS_futex, p, FUTEX_WAKE_PRIVATE, n, 0, 0, 0);
}

#ifndef MEMBARRIER_CMD_PRIVATE_EXPEDITED_SYNC_CORE
#define MEMBARRIER_CMD_PRIVATE_EXPEDITED_SYNC_CORE (1<<5)
#define MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED_SYNC_CORE (1<<6)
#endif
#ifndef SYS_membarrier
#define SYS_membarrier 324  /* x86-64; aarch64=283 — guarded below */
#endif
LA_INLINE int la_membarrier_register_synccore(void)
{
  return (int)syscall(SYS_membarrier,
                      MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED_SYNC_CORE, 0, 0);
}
/* Cross-core code-publication barrier (08 §8.5 step 3). Returns 0 ok. */
LA_INLINE int la_membarrier_synccore(void)
{
  return (int)syscall(SYS_membarrier,
                      MEMBARRIER_CMD_PRIVATE_EXPEDITED_SYNC_CORE, 0, 0);
}
#endif /* __linux__ */

/* ---- futex-style waits (Darwin) -------------------------------------- */
#if defined(__APPLE__) && defined(__MACH__)
#define LA_HAS_FUTEX 1
#include <errno.h>
#include <limits.h>

extern int __ulock_wait(uint32_t op, void *addr, uint64_t value,
			uint32_t timeout_us);
extern int __ulock_wake(uint32_t op, void *addr, uint64_t wake_value);

#ifndef UL_COMPARE_AND_WAIT
#define UL_COMPARE_AND_WAIT 1
#endif
#ifndef ULF_WAKE_ALL
#define ULF_WAKE_ALL 0x00000100
#endif

LA_INLINE uint32_t la_timeout_ns_to_darwin_us(int64_t ns)
{
  uint64_t us;
  if (ns < 0)
    return 0;  /* Darwin ulock timeout 0 means no timeout. */
  us = ((uint64_t)ns + 999u) / 1000u;
  if (us == 0)
    us = 1;
  return us > UINT_MAX ? UINT_MAX : (uint32_t)us;
}

/* Wait while *p == val. ns<0: infinite. Returns 0 woken/changed,
** -1 with errno on error (ETIMEDOUT/EINTR/EWOULDBLOCK are normal). */
LA_INLINE int la_futex_wait(uint32_t *p, uint32_t val, int64_t ns)
{
  return __ulock_wait(UL_COMPARE_AND_WAIT, p, (uint64_t)val,
		      la_timeout_ns_to_darwin_us(ns));
}

LA_INLINE int la_futex_wake(uint32_t *p, int n)
{
  uint32_t op;
  if (n <= 0)
    return 0;
  op = UL_COMPARE_AND_WAIT;
  if (n != 1)
    op |= ULF_WAKE_ALL;
  return __ulock_wake(op, p, 0);
}
#endif /* __APPLE__ && __MACH__ */

/* ---- futex-style waits (Windows) ------------------------------------- */
#if defined(_WIN32)
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0602
#elif _WIN32_WINNT < 0x0602
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0602
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <errno.h>

#define LA_HAS_FUTEX 1

LA_INLINE DWORD la_timeout_ns_to_windows_ms(int64_t ns)
{
  uint64_t ms;
  if (ns < 0)
    return INFINITE;
  ms = ((uint64_t)ns + 999999u) / 1000000u;
  if (ms == 0)
    ms = 1;
  return ms >= INFINITE ? INFINITE - 1u : (DWORD)ms;
}

/* Wait while *p == val. ns<0: infinite. Returns 0 woken/changed,
** -1 with errno on error (ETIMEDOUT is normal). */
LA_INLINE int la_futex_wait(uint32_t *p, uint32_t val, int64_t ns)
{
  if (WaitOnAddress((volatile VOID *)p, &val, sizeof(val),
		    la_timeout_ns_to_windows_ms(ns)))
    return 0;
  switch (GetLastError()) {
  case ERROR_TIMEOUT: errno = ETIMEDOUT; break;
  case ERROR_INVALID_ADDRESS: errno = EINVAL; break;
  default: errno = EAGAIN; break;
  }
  return -1;
}

LA_INLINE int la_futex_wake(uint32_t *p, int n)
{
  if (n == 1)
    WakeByAddressSingle((PVOID)p);
  else if (n > 1)
    WakeByAddressAll((PVOID)p);
  return 0;
}
#endif /* _WIN32 */

/* ---- compile-time checks ------------------------------------------- */
typedef char la_assert_ptr8[sizeof(void *) == 8 ? 1 : -1];
/* x86-64: build with -mcx16 so la_cas128 lowers to cmpxchg16b; without it
** the builtin routes through libatomic, which uses a lock. Verify this
** contract through generated disassembly or ASM artifact checks (12 §M0),
** not by grepping repository source. */

#endif /* _LJ_ATOMIC_H */

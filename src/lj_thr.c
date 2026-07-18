/*
** OS-thread substrate for LuaJIT-MT.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#define lj_thr_c
#define LUA_CORE

#include "lj_obj.h"
#include "lj_atomic.h"
#include "lj_gc2.h"
#include "lj_safepoint.h"
#include "lj_state.h"
#include "lj_tg.h"
#include "lj_thr.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#if LJ_TARGET_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <malloc.h>
#else
#include <sched.h>
#include <time.h>
#include <unistd.h>
#if LJ_TARGET_LINUX
#include <sys/mman.h>
#ifndef MADV_WIPEONFORK
#define MADV_WIPEONFORK 18
#endif
#endif
#endif

#define LJ_THR_TG_EXACT_TAG ((uintptr_t)1u)
#define LJ_THR_TG_TAG_MASK LJ_THR_TG_EXACT_TAG
#define LJ_THR_TG_SIGNAL_RAW_TAG ((uintptr_t)2u)
#define LJ_THR_TG_SIGNAL_TAG_MASK \
  (LJ_THR_TG_EXACT_TAG|LJ_THR_TG_SIGNAL_RAW_TAG)
typedef char lj_thr_tg_tag_requires_alignment[
  __alignof__(TGState) >= 4 ? 1 : -1];
typedef char lj_thr_tg_tag_requires_pointer_width[
  sizeof(uintptr_t) == sizeof(void *) ? 1 : -1];

#if LJ_TARGET_WINDOWS
/* The process TLS slot holds a stable per-thread cell. The cell's tagged word
** is the complete hot binding: low bit zero is a raw compatibility TG pointer;
** low bit one means TLS owns one exact registry lease for the masked body.
** This indirection makes every mutation after first admission an atomic cell
** store instead of another fallible TlsSetValue call. */
#define LJ_THR_TG_CELL_SIZE 64u
typedef struct LJThrTGCell {
  uintptr_t tagged_word;
  LJThrGC2TLS gc2;
  uint8_t pad[LJ_THR_TG_CELL_SIZE - sizeof(uintptr_t) - sizeof(LJThrGC2TLS)];
} LJThrTGCell;
typedef char lj_thr_tg_cell_is_one_cacheline[
  sizeof(LJThrTGCell) == LJ_THR_TG_CELL_SIZE ? 1 : -1];

static uint32_t lj_tls_tg_key = TLS_OUT_OF_INDEXES;
static INIT_ONCE lj_tls_tg_once = INIT_ONCE_STATIC_INIT;
#if defined(LJ_THR_TLS_TEST_HELPERS)
static uint32_t lj_tls_test_fail_index_alloc_after;
static uint32_t lj_tls_test_fail_cell_alloc_after;
static uint32_t lj_tls_test_fail_cell_publish_after;

void lj_thr_tls_test_fail_index_alloc(uint32_t nth)
{
  la_store32_rel(&lj_tls_test_fail_index_alloc_after, nth);
}

void lj_thr_tls_test_fail_cell_alloc(uint32_t nth)
{
  la_store32_rel(&lj_tls_test_fail_cell_alloc_after, nth);
}

void lj_thr_tls_test_fail_cell_publish(uint32_t nth)
{
  la_store32_rel(&lj_tls_test_fail_cell_publish_after, nth);
}

static int lj_thr_tls_test_fail_now(uint32_t *count)
{
  uint32_t current = la_load32_acq(count);
  while (current != 0) {
    uint32_t next = current - 1u;
    if (la_cas32(count, &current, next, LA_ACQ_REL, LA_ACQ))
      return next == 0;
  }
  return 0;
}
#endif

static DWORD lj_thr_tls_alloc_index(void)
{
#if defined(LJ_THR_TLS_TEST_HELPERS)
  if (lj_thr_tls_test_fail_now(&lj_tls_test_fail_index_alloc_after)) {
    SetLastError(ERROR_NOT_ENOUGH_MEMORY);
    return TLS_OUT_OF_INDEXES;
  }
#endif
  return TlsAlloc();
}

static LJThrTGCell *lj_thr_tls_alloc_cell(void)
{
#if defined(LJ_THR_TLS_TEST_HELPERS)
  if (lj_thr_tls_test_fail_now(&lj_tls_test_fail_cell_alloc_after)) {
    SetLastError(ERROR_NOT_ENOUGH_MEMORY);
    return NULL;
  }
#endif
  return (LJThrTGCell *)_aligned_malloc(sizeof(LJThrTGCell),
					       LJ_THR_TG_CELL_SIZE);
}

static BOOL lj_thr_tls_publish_cell(DWORD key, LJThrTGCell *cell)
{
#if defined(LJ_THR_TLS_TEST_HELPERS)
  if (lj_thr_tls_test_fail_now(&lj_tls_test_fail_cell_publish_after)) {
    SetLastError(ERROR_NOT_ENOUGH_MEMORY);
    return FALSE;
  }
#endif
  return TlsSetValue(key, cell);
}
#else
static LJ_TLS uintptr_t lj_tls_tg_word;
#endif
static uint32_t lj_thr_next_tid;

#if LJ_THR_TG_SIGNAL_CACHE
/* Immutable hash nodes are never reclaimed. A signal handler snapshots one
** bucket head and follows only next links initialized before publication, so
** registration cannot create a retry/unlink race. Tag 1 mirrors an exact TLS
** lease; tag 2 is the explicitly temporary same-thread profiler/raw bridge. */
#define LJ_THR_TG_SIGNAL_BUCKETS 256u
typedef struct LJThrTGSignalCell {
  struct LJThrTGSignalCell *next;
  uint64_t generation;
  uintptr_t process;
  uintptr_t owner;
  uintptr_t tagged_word;
} LJThrTGSignalCell;

static LJThrTGSignalCell *lj_tg_signal_buckets[LJ_THR_TG_SIGNAL_BUCKETS];
static pthread_key_t lj_tg_signal_key;
static uint32_t lj_tg_signal_key_state;
static uint32_t lj_tg_signal_atfork_state;
static uint32_t lj_tg_signal_process_poisoned;
static uint64_t lj_tg_signal_generation = 1u;
static uintptr_t lj_tg_signal_process_cached;
#if LJ_TARGET_LINUX
/* Linux applies MADV_WIPEONFORK in the kernel for libc and raw fork/clone
** paths alike. The page is dedicated so zeroing cannot corrupt unrelated
** state. A handler accepts only READY; owner context advances the generation
** before publishing READY again. */
#define LJ_THR_SIGNAL_FORK_PAGE_SIZE 4096u
#define LJ_THR_SIGNAL_FORK_DIRTY 0u
#define LJ_THR_SIGNAL_FORK_BUILDING 1u
#define LJ_THR_SIGNAL_FORK_READY 2u
typedef struct LJThrSignalForkPage {
  uint32_t state;
} LJThrSignalForkPage;
static LJThrSignalForkPage *lj_tg_signal_fork_page;
#endif
#if defined(LJ_THR_SIGNAL_TEST_HELPERS)
static uint32_t lj_tg_signal_test_fail_key_create_after;
static uint32_t lj_tg_signal_test_fail_cell_alloc_after;
static uint32_t lj_tg_signal_test_fail_cell_publish_after;
#if LJ_TARGET_LINUX
static uint32_t lj_tg_signal_test_fail_fork_page_after;
#endif
static uint32_t lj_tg_signal_test_destructor_count;
static uintptr_t lj_tg_signal_test_destructor_last_word;
static int lj_thr_signal_test_fail_now(uint32_t *count);
#endif

typedef char lj_thr_signal_pthread_id_must_fit[
  sizeof(pthread_t) == sizeof(uintptr_t) ? 1 : -1];

#define LJ_THR_SIGNAL_KEY_EMPTY 0u
#define LJ_THR_SIGNAL_KEY_BUILDING 1u
#define LJ_THR_SIGNAL_KEY_READY 2u
#define LJ_THR_SIGNAL_KEY_DEAD 3u

/* Function-address relocations are resolved when the image is loaded, unlike
** a lazy PLT/TLV call first reached from a signal. Artifact gates check that
** the handler getter calls only these eagerly-bound POSIX functions. */
static pthread_t (*volatile lj_thr_signal_pthread_self_fn)(void) =
  pthread_self;
static pid_t (*volatile lj_thr_signal_getpid_fn)(void) = getpid;

static LJ_AINLINE uintptr_t lj_thr_signal_process(void)
{
  return (uintptr_t)lj_thr_signal_getpid_fn();
}

static LJ_AINLINE uintptr_t lj_thr_signal_owner(void)
{
  /* pthread_self() is async-signal-safe on the supported targets. Both ABIs
  ** represent pthread_t in one pointer-sized scalar; no TLS address is
  ** taken. */
  return (uintptr_t)lj_thr_signal_pthread_self_fn();
}

static LJ_AINLINE uint32_t lj_thr_signal_bucket(uint64_t generation,
                                                uintptr_t process,
                                                uintptr_t owner)
{
  uint64_t x = (uint64_t)owner ^ ((uint64_t)process << 32) ^ generation;
  x ^= x >> 33;
  x *= UINT64_C(0xff51afd7ed558ccd);
  x ^= x >> 33;
  return (uint32_t)x & (LJ_THR_TG_SIGNAL_BUCKETS - 1u);
}

#if LJ_TARGET_LINUX
static LJThrSignalForkPage *lj_thr_signal_fork_page_ensure(void)
{
  LJThrSignalForkPage *page = (LJThrSignalForkPage *)
    la_loadptr_acq((void *const *)&lj_tg_signal_fork_page);
  if (!page) {
#if defined(LJ_THR_SIGNAL_TEST_HELPERS)
    if (lj_thr_signal_test_fail_now(
          &lj_tg_signal_test_fail_fork_page_after))
      return NULL;
#endif
    LJThrSignalForkPage *candidate = (LJThrSignalForkPage *)mmap(
      NULL, LJ_THR_SIGNAL_FORK_PAGE_SIZE, PROT_READ | PROT_WRITE,
      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    void *expected = NULL;
    if (candidate == MAP_FAILED)
      return NULL;
    if (madvise(candidate, LJ_THR_SIGNAL_FORK_PAGE_SIZE,
                MADV_WIPEONFORK) != 0) {
      (void)munmap(candidate, LJ_THR_SIGNAL_FORK_PAGE_SIZE);
      return NULL;
    }
    la_store32_rlx(&candidate->state, LJ_THR_SIGNAL_FORK_READY);
    if (la_casptr((void **)&lj_tg_signal_fork_page, &expected, candidate,
                  LA_REL, LA_ACQ)) {
      page = candidate;
    } else {
      (void)munmap(candidate, LJ_THR_SIGNAL_FORK_PAGE_SIZE);
      page = (LJThrSignalForkPage *)expected;
    }
  }
  return page;
}

static LJ_AINLINE int lj_thr_signal_fork_ready(void)
{
  LJThrSignalForkPage *page = (LJThrSignalForkPage *)
    la_loadptr_acq((void *const *)&lj_tg_signal_fork_page);
  return page && la_load32_acq(&page->state) == LJ_THR_SIGNAL_FORK_READY;
}

static LJ_AINLINE void lj_thr_signal_fork_publish_ready(void)
{
  LJThrSignalForkPage *page = (LJThrSignalForkPage *)
    la_loadptr_acq((void *const *)&lj_tg_signal_fork_page);
  if (page)
    la_store32_rel(&page->state, LJ_THR_SIGNAL_FORK_READY);
}
#else
static LJ_AINLINE int lj_thr_signal_fork_ready(void)
{
  return 1;
}

static LJ_AINLINE void lj_thr_signal_fork_publish_ready(void)
{
}
#endif

/* Advance the process incarnation without ever wrapping. The cached PID is
** zero throughout the transition, so the asynchronous reader fails closed.
** A saturated generation permanently poisons this process copy. */
static void lj_thr_signal_process_advance(uintptr_t process)
{
  uint64_t generation = la_load64_acq(&lj_tg_signal_generation);
  la_storeuptr_rel(&lj_tg_signal_process_cached, 0);
  if (generation == UINT64_MAX) {
    la_store32_rel(&lj_tg_signal_process_poisoned, 1);
  } else {
    la_store64_rel(&lj_tg_signal_generation, generation + 1u);
  }
  if (la_load32_acq(&lj_tg_signal_key_state) ==
      LJ_THR_SIGNAL_KEY_BUILDING)
    la_store32_rel(&lj_tg_signal_key_state, LJ_THR_SIGNAL_KEY_EMPTY);
  if (la_load32_acq(&lj_tg_signal_atfork_state) ==
      LJ_THR_SIGNAL_KEY_BUILDING)
    la_store32_rel(&lj_tg_signal_atfork_state, LJ_THR_SIGNAL_KEY_EMPTY);
  la_storeuptr_rel(&lj_tg_signal_process_cached, process);
}

/* pthread_atfork child hook. Only async-signal-safe, lock-free x86-64 atomics
** and the eagerly relocated getpid entry are reachable from here. Resetting a
** copied BUILDING state prevents a child from waiting for a vanished builder. */
static void lj_thr_signal_atfork_child(void)
{
  lj_thr_signal_process_advance(lj_thr_signal_process());
  lj_thr_signal_fork_publish_ready();
}

/* Repair a fork which bypassed pthread_atfork (raw syscall/foreign runtime),
** and initialize the first process identity. Every cold cache admission calls
** this even when the pthread key already exists. */
static int lj_thr_signal_process_repair(uint32_t *advanced)
{
  int saved_errno = errno;
  uintptr_t process = lj_thr_signal_process();
  uintptr_t cached;
#if LJ_TARGET_LINUX
  LJThrSignalForkPage *page;
#endif
  if (advanced)
    *advanced = 0;
  if (process == 0) {
    errno = saved_errno;
    return 0;
  }
#if LJ_TARGET_LINUX
  page = lj_thr_signal_fork_page_ensure();
  if (!page) {
    errno = saved_errno;
    return 0;
  }
  for (;;) {
    uint32_t fork_state = la_load32_acq(&page->state);
    if (fork_state == LJ_THR_SIGNAL_FORK_READY)
      break;
    if (fork_state == LJ_THR_SIGNAL_FORK_DIRTY) {
      uint32_t expected = LJ_THR_SIGNAL_FORK_DIRTY;
      if (la_cas32(&page->state, &expected, LJ_THR_SIGNAL_FORK_BUILDING,
                   LA_ACQ_REL, LA_ACQ)) {
        /* The kernel witness, unlike a numeric PID, cannot be resurrected by
        ** an uninterrupted chain of raw forks and ancestor PID reuse. */
        lj_thr_signal_process_advance(process);
        if (advanced)
          *advanced = 1;
        la_store32_rel(&page->state, LJ_THR_SIGNAL_FORK_READY);
        errno = saved_errno;
        return !la_load32_acq(&lj_tg_signal_process_poisoned);
      }
      continue;
    }
    if (fork_state != LJ_THR_SIGNAL_FORK_BUILDING) {
      errno = saved_errno;
      return 0;
    }
    (void)sched_yield();
  }
#endif
  cached = la_loaduptr_acq(&lj_tg_signal_process_cached);
  if (cached == 0) {
    uintptr_t expected = 0;
    if (!la_casuptr(&lj_tg_signal_process_cached, &expected, process,
                    LA_REL, LA_ACQ))
      cached = expected;
    else
      cached = process;
  }
  if (cached != process) {
    uintptr_t expected = cached;
    if (la_casuptr(&lj_tg_signal_process_cached, &expected, 0,
                   LA_ACQ_REL, LA_ACQ)) {
      lj_thr_signal_process_advance(process);
      if (advanced)
        *advanced = 1;
    } else {
      do {
        cached = la_loaduptr_acq(&lj_tg_signal_process_cached);
        if (cached == process)
          break;
        (void)sched_yield();
      } while (cached == 0);
      if (cached != process) {
        errno = saved_errno;
        return 0;
      }
    }
  }
  errno = saved_errno;
  return !la_load32_acq(&lj_tg_signal_process_poisoned);
}

int lj_thr_tg_signal_process_snapshot(uint64_t *generation,
                                      uint32_t *advanced)
{
  int usable = lj_thr_signal_process_repair(advanced);
  if (generation)
    *generation = la_load64_acq(&lj_tg_signal_generation);
  return usable;
}

static int lj_thr_signal_atfork_ensure(void)
{
  int saved_errno = errno;
  for (;;) {
    uint32_t state = la_load32_acq(&lj_tg_signal_atfork_state);
    if (state == LJ_THR_SIGNAL_KEY_READY) {
      errno = saved_errno;
      return 1;
    }
    if (state == LJ_THR_SIGNAL_KEY_EMPTY) {
      uint32_t expected = LJ_THR_SIGNAL_KEY_EMPTY;
      if (la_cas32(&lj_tg_signal_atfork_state, &expected,
                   LJ_THR_SIGNAL_KEY_BUILDING, LA_ACQ_REL, LA_ACQ)) {
        int rc = pthread_atfork(NULL, NULL, lj_thr_signal_atfork_child);
        la_store32_rel(&lj_tg_signal_atfork_state,
                       rc == 0 ? LJ_THR_SIGNAL_KEY_READY :
                                 LJ_THR_SIGNAL_KEY_EMPTY);
        errno = saved_errno;
        return rc == 0;
      }
      continue;
    }
    (void)sched_yield();
    if (!lj_thr_signal_process_repair(NULL)) {
      errno = saved_errno;
      return 0;
    }
  }
}

#if defined(LJ_THR_SIGNAL_TEST_HELPERS)
static int lj_thr_signal_test_fail_now(uint32_t *count)
{
  uint32_t current = la_load32_acq(count);
  while (current != 0) {
    uint32_t next = current - 1u;
    if (la_cas32(count, &current, next, LA_ACQ_REL, LA_ACQ))
      return next == 0;
  }
  return 0;
}

void lj_thr_tg_signal_test_fail_key_create(uint32_t nth)
{
  la_store32_rel(&lj_tg_signal_test_fail_key_create_after, nth);
}

void lj_thr_tg_signal_test_fail_cell_alloc(uint32_t nth)
{
  la_store32_rel(&lj_tg_signal_test_fail_cell_alloc_after, nth);
}

void lj_thr_tg_signal_test_fail_cell_publish(uint32_t nth)
{
  la_store32_rel(&lj_tg_signal_test_fail_cell_publish_after, nth);
}

#if LJ_TARGET_LINUX
void lj_thr_tg_signal_test_fail_fork_page(uint32_t nth)
{
  la_store32_rel(&lj_tg_signal_test_fail_fork_page_after, nth);
}
#endif

uint64_t lj_thr_tg_signal_test_generation(void)
{
  return la_load64_acq(&lj_tg_signal_generation);
}

uintptr_t lj_thr_tg_signal_test_process(void)
{
  return la_loaduptr_acq(&lj_tg_signal_process_cached);
}

uint32_t lj_thr_tg_signal_test_poisoned(void)
{
  return la_load32_acq(&lj_tg_signal_process_poisoned);
}

void lj_thr_tg_signal_test_force_generation(uint64_t generation)
{
  la_store64_rel(&lj_tg_signal_generation, generation);
  la_store32_rel(&lj_tg_signal_process_poisoned, 0);
}

void lj_thr_tg_signal_test_force_process(uintptr_t process)
{
  la_storeuptr_rel(&lj_tg_signal_process_cached, process);
}

void lj_thr_tg_signal_test_advance_same_process(void)
{
  lj_thr_signal_process_advance(lj_thr_signal_process());
}

void lj_thr_tg_signal_test_force_building(void)
{
  la_store32_rel(&lj_tg_signal_key_state, LJ_THR_SIGNAL_KEY_BUILDING);
  la_store32_rel(&lj_tg_signal_atfork_state, LJ_THR_SIGNAL_KEY_BUILDING);
}

uint32_t lj_thr_tg_signal_test_key_state(void)
{
  return la_load32_acq(&lj_tg_signal_key_state);
}
#endif

static void lj_thr_signal_cell_destructor(void *ptr)
{
  LJThrTGSignalCell *cell = (LJThrTGSignalCell *)ptr;
  uintptr_t word;
  if (!cell)
    return;
  /* Clear handler visibility first. For an exact tag, the lease remains
  ** represented by compiler TLS and is intentionally leaked by an unclean
  ** lifecycle exit until the production detach handoff owns its release. A
  ** raw compatibility tag owns no lease and simply dies with the thread. */
  word = la_loaduptr_acq(&cell->tagged_word);
  la_storeuptr_rel(&cell->tagged_word, 0);
#if defined(LJ_THR_SIGNAL_TEST_HELPERS)
  la_storeuptr_rel(&lj_tg_signal_test_destructor_last_word, word);
  (void)la_add32_acqrel(&lj_tg_signal_test_destructor_count, 1);
#else
  UNUSED(word);
#endif
}

static int lj_thr_signal_key_ensure(void)
{
  /* Temporary cold admission bridge: pthread_key_create and the losing
  ** BUILDING sched_yield path are not part of the final nonblocking proof.
  ** Failure reopens EMPTY so a later admission can retry instead of inheriting
  ** pthread_once's permanent failed initialization. */
  int saved_errno = errno;
  if (!lj_thr_signal_process_repair(NULL)) {
    errno = saved_errno;
    return 0;
  }
  for (;;) {
    uint32_t state = la_load32_acq(&lj_tg_signal_key_state);
    if (state == LJ_THR_SIGNAL_KEY_READY) {
      errno = saved_errno;
      return 1;
    }
    if (state == LJ_THR_SIGNAL_KEY_DEAD) {
      errno = saved_errno;
      return 0;
    }
    if (state == LJ_THR_SIGNAL_KEY_EMPTY) {
      uint32_t expected = LJ_THR_SIGNAL_KEY_EMPTY;
      if (la_cas32(&lj_tg_signal_key_state, &expected,
                   LJ_THR_SIGNAL_KEY_BUILDING, LA_ACQ_REL, LA_ACQ)) {
        int rc;
#if defined(LJ_THR_SIGNAL_TEST_HELPERS)
        if (lj_thr_signal_test_fail_now(
              &lj_tg_signal_test_fail_key_create_after))
          rc = EAGAIN;
        else
#endif
          rc = pthread_key_create(&lj_tg_signal_key,
                                  lj_thr_signal_cell_destructor);
        la_store32_rel(&lj_tg_signal_key_state,
                       rc == 0 ? LJ_THR_SIGNAL_KEY_READY :
                                 LJ_THR_SIGNAL_KEY_EMPTY);
        errno = saved_errno;
        return rc == 0;
      }
      continue;
    }
    (void)sched_yield();
  }
}

int lj_thr_tg_signal_activate(void)
{
  /* The caller must make this image process-stable before registration:
  ** pthread_atfork has no unregister operation. profile_timer_start pins its
  ** containing image first; the standalone fixture runs in the main image. */
  return lj_thr_signal_process_repair(NULL) && lj_thr_signal_atfork_ensure();
}

static LJThrTGSignalCell *lj_thr_signal_cell_specific(void)
{
  if (la_load32_acq(&lj_tg_signal_key_state) !=
      LJ_THR_SIGNAL_KEY_READY)
    return NULL;
  return (LJThrTGSignalCell *)pthread_getspecific(lj_tg_signal_key);
}

static LJThrTGSignalCell *lj_thr_signal_cell_prepare(void)
{
  LJThrTGSignalCell *cell, *head;
  uint64_t generation;
  uintptr_t process, owner;
  uint32_t bucket;
  int saved_errno = errno;
  if (!lj_thr_signal_key_ensure())
    return NULL;
  cell = (LJThrTGSignalCell *)pthread_getspecific(lj_tg_signal_key);
  process = la_loaduptr_acq(&lj_tg_signal_process_cached);
  generation = la_load64_acq(&lj_tg_signal_generation);
  owner = lj_thr_signal_owner();
  if (cell && cell->generation == generation && cell->process == process &&
      cell->owner == owner) {
    errno = saved_errno;
    return cell;
  }
  if (process == 0 || owner == 0) {
    errno = saved_errno;
    return NULL;
  }
  cell = NULL;
#if defined(LJ_THR_SIGNAL_TEST_HELPERS)
  if (!lj_thr_signal_test_fail_now(&lj_tg_signal_test_fail_cell_alloc_after))
#endif
    cell = (LJThrTGSignalCell *)malloc(sizeof(*cell));
  if (!cell) {
    errno = saved_errno;
    return NULL;
  }
  cell->next = NULL;
  cell->generation = generation;
  cell->process = process;
  cell->owner = owner;
  la_storeuptr_rlx(&cell->tagged_word, 0);
  /* A destructor may only observe a globally unpublished zero cell here. */
  {
    int publish_error;
#if defined(LJ_THR_SIGNAL_TEST_HELPERS)
    publish_error = lj_thr_signal_test_fail_now(
      &lj_tg_signal_test_fail_cell_publish_after) ? ENOMEM :
                                                    pthread_setspecific(
                                                      lj_tg_signal_key, cell);
#else
    publish_error = pthread_setspecific(lj_tg_signal_key, cell);
#endif
    if (publish_error != 0) {
      free(cell);
      errno = saved_errno;
      return NULL;
    }
  }
  bucket = lj_thr_signal_bucket(generation, process, owner);
  do {
    head = (LJThrTGSignalCell *)
      la_loadptr_acq((void *const *)&lj_tg_signal_buckets[bucket]);
    cell->next = head;
  } while (!la_casptr((void **)&lj_tg_signal_buckets[bucket],
                      (void **)&head, cell, LA_REL, LA_ACQ));
  errno = saved_errno;
  return cell;
}

static void lj_thr_signal_word_set(uintptr_t word)
{
  LJThrTGSignalCell *cell = lj_thr_signal_cell_specific();
  if (!cell)
    abort();  /* Exact admission prepared the process-stable signal cell. */
  la_storeuptr_rel(&cell->tagged_word, word);
}

static void lj_thr_signal_word_clear_if_registered(void)
{
  LJThrTGSignalCell *cell = lj_thr_signal_cell_specific();
  if (cell)
    la_storeuptr_rel(&cell->tagged_word, 0);
}

static LJ_AINLINE uintptr_t lj_thr_signal_word_get(void)
{
  uintptr_t process = lj_thr_signal_process();
  uintptr_t cached = la_loaduptr_acq(&lj_tg_signal_process_cached);
  uint64_t generation;
  uintptr_t owner = lj_thr_signal_owner();
  LJThrTGSignalCell *cell;
  uintptr_t word;
  uint32_t bucket;
  /* getpid is a mandatory eager backstop on every lookup. It rejects a raw or
  ** missed fork before any inherited bucket node or TG body can be consumed. */
  if (process == 0 || process != cached || owner == 0 ||
      !lj_thr_signal_fork_ready() ||
      la_load32_acq(&lj_tg_signal_process_poisoned))
    return 0;
  generation = la_load64_acq(&lj_tg_signal_generation);
  bucket = lj_thr_signal_bucket(generation, process, owner);
  cell = (LJThrTGSignalCell *)
    la_loadptr_acq((void *const *)&lj_tg_signal_buckets[bucket]);
  while (cell) {
    if (cell->generation == generation && cell->process == process &&
        cell->owner == owner) {
      word = la_loaduptr_acq(&cell->tagged_word);
      return word;
    }
    cell = cell->next;
  }
  return 0;
}

static LJ_AINLINE TGState *lj_thr_signal_word_decode(int accept_raw)
{
  uintptr_t word = lj_thr_signal_word_get();
  uintptr_t tag = word & LJ_THR_TG_SIGNAL_TAG_MASK;
  if (tag != LJ_THR_TG_EXACT_TAG &&
      !(accept_raw && tag == LJ_THR_TG_SIGNAL_RAW_TAG))
    return NULL;
  word &= ~LJ_THR_TG_SIGNAL_TAG_MASK;
  return word != 0 && word % (uintptr_t)__alignof__(TGState) == 0 ?
         (TGState *)word : NULL;
}

TGState *lj_thr_get_tg_signal(void)
{
  return lj_thr_signal_word_decode(0);
}

TGState *lj_thr_get_tg_profile_signal(void)
{
  return lj_thr_signal_word_decode(1);
}

#if defined(LJ_THR_SIGNAL_TEST_HELPERS)
#if LJ_TARGET_OSX
LUA_API TGState *luaJIT_thr_tg_signal_test_get(void)
{
  return lj_thr_signal_word_decode(0);
}

LUA_API TGState *luaJIT_thr_tg_profile_signal_test_get(void)
{
  return lj_thr_signal_word_decode(1);
}
#else
LUA_API TGState *luaJIT_thr_tg_signal_test_get(void)
  __attribute__((alias("lj_thr_get_tg_signal")));
LUA_API TGState *luaJIT_thr_tg_profile_signal_test_get(void)
  __attribute__((alias("lj_thr_get_tg_profile_signal")));
#endif
/* Mach-O nlist entries carry no symbol size. Keep an exported helper boundary
** after the two full-body getters so llvm-objdump does not include later
** stripped local functions in the final-image artifact check. */
LUA_API void luaJIT_thr_tg_signal_test_end(void)
{
}
#endif

/* pthread_key_delete prevents a later live-thread exit from calling back into
** an unloaded shared object. Concurrent dlclose/use remains outside the C ABI
** contract, but merely unloading after users stopped is safe. */
static void __attribute__((destructor)) lj_thr_signal_cache_fini(void)
{
  uint32_t state = la_load32_acq(&lj_tg_signal_key_state);
  uint32_t bucket;
  if (state == LJ_THR_SIGNAL_KEY_READY) {
    la_store32_rel(&lj_tg_signal_key_state, LJ_THR_SIGNAL_KEY_DEAD);
    for (bucket = 0; bucket < LJ_THR_TG_SIGNAL_BUCKETS; bucket++) {
      LJThrTGSignalCell *cell = (LJThrTGSignalCell *)
        la_loadptr_acq((void *const *)&lj_tg_signal_buckets[bucket]);
      while (cell) {
        la_storeuptr_rel(&cell->tagged_word, 0);
        cell = cell->next;
      }
    }
    (void)pthread_key_delete(lj_tg_signal_key);
  }
#if LJ_TARGET_LINUX
  /* Once atfork is READY the caller's image is process-stable and a handler
  ** may have selected this page, so leave it mapped for kernel reclamation.
  ** Before activation (notably a failed profile pin), no callback/handler was
  ** installed and an ordinary stopped-user dlclose may reclaim it. */
  if (la_load32_acq(&lj_tg_signal_atfork_state) ==
      LJ_THR_SIGNAL_KEY_EMPTY) {
    LJThrSignalForkPage *page = (LJThrSignalForkPage *)
      la_loadptr_acq((void *const *)&lj_tg_signal_fork_page);
    la_storeptr_rel((void **)&lj_tg_signal_fork_page, NULL);
    if (page)
      (void)munmap(page, LJ_THR_SIGNAL_FORK_PAGE_SIZE);
  }
#endif
}

#if defined(LJ_THR_SIGNAL_TEST_HELPERS)
void lj_thr_tg_signal_test_reset_destructors(void)
{
  la_store32_rel(&lj_tg_signal_test_destructor_count, 0);
  la_storeuptr_rel(&lj_tg_signal_test_destructor_last_word, 0);
}

uint32_t lj_thr_tg_signal_test_destructors(void)
{
  return la_load32_acq(&lj_tg_signal_test_destructor_count);
}

uintptr_t lj_thr_tg_signal_test_last_destructor_word(void)
{
  return la_loaduptr_acq(&lj_tg_signal_test_destructor_last_word);
}
#endif
#else
static LJ_AINLINE void *lj_thr_signal_cell_prepare(void)
{
  return (void *)1;
}

static LJ_AINLINE void lj_thr_signal_word_set(uintptr_t word)
{
  UNUSED(word);
}

static LJ_AINLINE void lj_thr_signal_word_clear_if_registered(void)
{
}

TGState *lj_thr_get_tg_signal(void)
{
  return NULL;
}

TGState *lj_thr_get_tg_profile_signal(void)
{
  return NULL;
}

int lj_thr_tg_signal_activate(void)
{
  return 1;
}

int lj_thr_tg_signal_prepare_current(TGState *expected_tg)
{
  UNUSED(expected_tg);
  return 1;
}

int lj_thr_tg_signal_process_snapshot(uint64_t *generation,
                                      uint32_t *advanced)
{
  if (generation)
    *generation = 1u;
  if (advanced)
    *advanced = 0;
  return 1;
}
#endif

uint32_t lj_thr_newid(void)
{
  return lj_thr_id_alloc(&lj_thr_next_tid);  /* 09 section 9.2. */
}

#if LJ_TARGET_WINDOWS
static BOOL CALLBACK lj_thr_tls_init(PINIT_ONCE once, PVOID param,
				     PVOID *ctx)
{
  DWORD key;
  UNUSED(once);
  UNUSED(param);
  UNUSED(ctx);
  key = lj_thr_tls_alloc_index();
  if (key == TLS_OUT_OF_INDEXES) {
    la_store32_rel(&lj_tls_tg_key, TLS_OUT_OF_INDEXES);
    return FALSE;
  }
  la_store32_rel(&lj_tls_tg_key, (uint32_t)key);
  return TRUE;
}

int lj_thr_tg_tls_init(void)
{
  LJThrTGCell *cell;
  DWORD key;
  if (!InitOnceExecuteOnce(&lj_tls_tg_once, lj_thr_tls_init, NULL, NULL))
    return 0;
  key = (DWORD)la_load32_acq(&lj_tls_tg_key);
  if (key == TLS_OUT_OF_INDEXES)
    return 0;
  cell = (LJThrTGCell *)TlsGetValue(key);
  if (cell)
    return 1;
  cell = lj_thr_tls_alloc_cell();
  if (!cell)
    return 0;
  memset(cell, 0, sizeof(*cell));
  if (!lj_thr_tls_publish_cell(key, cell)) {
    _aligned_free(cell);
    return 0;
  }
  return 1;
}

static DWORD lj_thr_tls_key(void)
{
  return (DWORD)la_load32_acq(&lj_tls_tg_key);
}

LJThrGC2TLS *lj_thr_gc2_tls_current(void)
{
  DWORD key = lj_thr_tls_key();
  LJThrTGCell *cell;
  DWORD saved_error;
  if (key == TLS_OUT_OF_INDEXES)
    return NULL;
  /* TlsGetValue clears LastError on a successful NULL or non-NULL lookup.
  ** GC/SMR can run between a foreign call/callback body and the exact error
  ** snapshot, so the lookup must be invisible to that API contract. */
  saved_error = GetLastError();
  cell = (LJThrTGCell *)TlsGetValue(key);
  SetLastError(saved_error);
  return cell ? &cell->gc2 : NULL;
}

static uintptr_t lj_thr_tls_word_get(void)
{
  DWORD key = lj_thr_tls_key();
  LJThrTGCell *cell = key != TLS_OUT_OF_INDEXES ?
    (LJThrTGCell *)TlsGetValue(key) : NULL;
  return cell ? la_loaduptr_acq(&cell->tagged_word) : 0;
}

static void lj_thr_tls_word_set(uintptr_t word)
{
  DWORD key = lj_thr_tls_key();
  LJThrTGCell *cell = key != TLS_OUT_OF_INDEXES ?
    (LJThrTGCell *)TlsGetValue(key) : NULL;
  if (!cell)
    abort();  /* Install admitted this thread, or an exact view proved it. */
  la_storeuptr_rel(&cell->tagged_word, word);
}

static DWORD WINAPI lj_thr_windows_main(void *arg)
{
  LJThr *thr = (LJThr *)arg;
  thr->ret = thr->func(thr->arg);
  return 0;
}
#else
int lj_thr_tg_tls_init(void)
{
  return 1;
}

static uintptr_t lj_thr_tls_word_get(void)
{
  return la_loaduptr_acq(&lj_tls_tg_word);
}

static void lj_thr_tls_word_set(uintptr_t word)
{
  la_storeuptr_rel(&lj_tls_tg_word, word);
}
#endif

#if defined(LJ_THR_TLS_TEST_HELPERS)
void lj_thr_tls_test_set_word(uintptr_t word)
{
  if (!lj_thr_tg_tls_init())
    abort();
  lj_thr_tls_word_set(word);
}
#endif

static TGState *lj_thr_tls_get(void)
{
  return (TGState *)(lj_thr_tls_word_get() & ~LJ_THR_TG_TAG_MASK);
}

#if LJ_THR_TG_SIGNAL_CACHE
int lj_thr_tg_signal_prepare_current(TGState *expected_tg)
{
  LJThrTGSignalCell *cell;
  uintptr_t word = lj_thr_tls_word_get();
  uintptr_t tag = word & LJ_THR_TG_SIGNAL_TAG_MASK;
  int saved_errno = errno;
  /* A profiler started from a thread with no current TG could never route a
  ** sample safely. Likewise, tags other than the exact TLS lease are corrupt
  ** here: raw compatibility pointers are naturally aligned and untagged. */
  if (word == 0 || (tag != 0 && tag != LJ_THR_TG_EXACT_TAG) ||
      (TGState *)(word & ~LJ_THR_TG_SIGNAL_TAG_MASK) != expected_tg) {
    errno = saved_errno;
    return 0;
  }
  cell = lj_thr_signal_cell_prepare();
  if (!cell) {
    errno = saved_errno;
    return 0;
  }
  la_storeuptr_rel(&cell->tagged_word,
                   tag == LJ_THR_TG_EXACT_TAG ? word :
                   word | LJ_THR_TG_SIGNAL_RAW_TAG);
  errno = saved_errno;
  return 1;
}
#endif

int lj_thr_create(LJThr *thr, LJThrFunc func, void *arg)
{
  if (!thr || !func)
    return EINVAL;
  if (thr->tid == 0)
    thr->tid = lj_thr_newid();
  if (!lj_thr_id_is_owner(thr->tid)) {
    thr->tid = 0;
    return EAGAIN;
  }
#if LJ_TARGET_WINDOWS
  thr->func = func;
  thr->arg = arg;
  thr->ret = NULL;
  thr->handle = CreateThread(NULL, 0, lj_thr_windows_main, thr, 0,
			     &thr->sysid);  /* 09 section 9.3. */
  if (thr->handle == NULL) {
    thr->tid = 0;
    return EAGAIN;
  }
  return 0;
#else
  {
    int rc;
    rc = pthread_create(&thr->handle, NULL, func, arg);  /* 09 section 9.3. */
    if (rc != 0)
      thr->tid = 0;
    return rc;
  }
#endif
}

int lj_thr_join(LJThr *thr, void **ret)
{
  if (!thr)
    return EINVAL;
#if LJ_TARGET_WINDOWS
  if (WaitForSingleObject(thr->handle, INFINITE) != WAIT_OBJECT_0)
    return EINVAL;
  if (ret)
    *ret = thr->ret;
  CloseHandle(thr->handle);
  thr->handle = NULL;
  return 0;
#else
  return pthread_join(thr->handle, ret);  /* 09 section 9.4 substrate. */
#endif
}

uint32_t lj_thr_id(const LJThr *thr)
{
  return thr ? thr->tid : 0;
}

uint32_t lj_thr_current_id(global_State *g)
{
  TGState *tg = lj_thr_get_tg_fallback(g);
  return tg ? lj_tg_tid_acq(tg) : 0;
}

uint64_t lj_thr_now_ns(void)
{
#if LJ_TARGET_WINDOWS
  LARGE_INTEGER freq, ctr;
  if (!QueryPerformanceFrequency(&freq) || !QueryPerformanceCounter(&ctr) ||
      freq.QuadPart <= 0)
    return 0;
  return (uint64_t)ctr.QuadPart / (uint64_t)freq.QuadPart * 1000000000ull +
    (uint64_t)ctr.QuadPart % (uint64_t)freq.QuadPart * 1000000000ull /
    (uint64_t)freq.QuadPart;
#else
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
    return 0;
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

typedef enum LJThrTGMode {
  LJ_THR_TG_MODE_EMPTY,
  LJ_THR_TG_MODE_RAW,
  LJ_THR_TG_MODE_KEYED,
  LJ_THR_TG_MODE_CORRUPT
} LJThrTGMode;

typedef struct LJThrTGView {
  TGState *body;
  LJTGRegistryKey key;
} LJThrTGView;

static int lj_thr_tg_borrow_empty(const LJTGRegistryBorrow *hold)
{
  return hold && !hold->active && hold->body == NULL &&
    hold->key.slot == NULL &&
    hold->key.incarnation == LJ_TGSLOT_INCARNATION_NONE;
}

static LJThrTGMode lj_thr_tg_view(LJThrTGView *view)
{
  uintptr_t word = lj_thr_tls_word_get();
  if (!view)
    return LJ_THR_TG_MODE_CORRUPT;
  view->body = NULL;
  view->key.slot = NULL;
  view->key.incarnation = LJ_TGSLOT_INCARNATION_NONE;
  if (word == 0)
    return LJ_THR_TG_MODE_EMPTY;
  view->body = (TGState *)(word & ~LJ_THR_TG_TAG_MASK);
  if ((word & LJ_THR_TG_EXACT_TAG) == 0)
    return LJ_THR_TG_MODE_RAW;
  if (!view->body ||
      (uintptr_t)view->body % (uintptr_t)__alignof__(TGState) != 0)
    return LJ_THR_TG_MODE_CORRUPT;
  /* The tag is ownership evidence for one ordinary lease. That lease keeps
  ** this body and its immutable embedded key alive while we reconstruct it. */
  view->key = view->body->registry_key;
  return lj_tgregistry_key_valid(&view->key) ? LJ_THR_TG_MODE_KEYED :
                                               LJ_THR_TG_MODE_CORRUPT;
}

static LJThrTGResult
lj_thr_tg_validate_view(const LJThrTGView *view, int new_target)
{
  LJTGRegistryBodySnap body;
  LJTGRegistrySlot *slot, *slow, *fast;
  LJTGSlotSnap snap;
  LJTGSlotResult status;
  TGState *tg;
  global_State *g;
  if (!view || !view->body ||
      (uintptr_t)view->body % (uintptr_t)__alignof__(TGState) != 0 ||
      !lj_tgregistry_key_valid(&view->key))
    return LJ_THR_TG_INVALID;
  snap.incarnation = LJ_TGSLOT_INCARNATION_NONE;
  snap.lease_count = 0;
  snap.state = LJ_TGSLOT_EMPTY;
  status = lj_tgregistry_key_snapshot(&view->key, &snap);
  if (!((status == LJ_TGSLOT_OK &&
         (snap.state == LJ_TGSLOT_ATTACHING ||
          snap.state == LJ_TGSLOT_LIVE ||
          snap.state == LJ_TGSLOT_DETACHING ||
          snap.state == LJ_TGSLOT_RETIRED)) ||
        (!new_target && status == LJ_TGSLOT_PINNED_RESULT &&
         snap.state == LJ_TGSLOT_PINNED)) ||
      snap.lease_count < 2u)
    return LJ_THR_TG_CORRUPT;
  if (new_target && snap.state != LJ_TGSLOT_ATTACHING &&
      snap.state != LJ_TGSLOT_LIVE)
    return LJ_THR_TG_CORRUPT;
  body = lj_tgregistry_slot_body_snapshot(view->key.slot);
  if (!lj_tgregistry_body_snap_is(&body, view->body,
                                  view->key.incarnation))
    return LJ_THR_TG_CORRUPT;
  tg = view->body;
  if (!lj_tgregistry_key_equal(&tg->registry_key, &view->key) || !tg->gl)
    return LJ_THR_TG_CORRUPT;
  /* The caller retains the universe lifetime while this dereferences tg->gl.
  ** Floyd detection rejects a corrupt cycle without imposing a size limit. */
  g = tg->gl;
  slot = gc2_tg_registry_head_acq(g);
  slow = fast = slot;
  while (slot) {
    if (slot == view->key.slot)
      return LJ_THR_TG_OK;
    slot = lj_tgregistry_slot_next_all(slot);
    slow = lj_tgregistry_slot_next_all(slow);
    fast = lj_tgregistry_slot_next_all(fast);
    if (fast)
      fast = lj_tgregistry_slot_next_all(fast);
    if (slow && slow == fast)
      return LJ_THR_TG_CORRUPT;
  }
  return LJ_THR_TG_CORRUPT;
}

static void lj_thr_tg_move_out(LJTGRegistryBorrow *hold,
                               const LJThrTGView *view)
{
  hold->key = view->key;
  hold->body = view->body;
  hold->active = 1;
}

LJThrTGResult lj_thr_tg_install(LJTGRegistryBorrow *new_hold)
{
  LJThrTGView current, incoming;
  LJThrTGMode mode;
  LJThrTGResult result;
  uintptr_t word;
  if (!new_hold || !new_hold->active || !new_hold->body ||
      !lj_tgregistry_key_valid(&new_hold->key) ||
      ((uintptr_t)new_hold->body & LJ_THR_TG_TAG_MASK) != 0)
    return LJ_THR_TG_INVALID;
  if (!lj_thr_tg_tls_init())
    return LJ_THR_TG_TLS_FAILURE;
  mode = lj_thr_tg_view(&current);
  if (mode == LJ_THR_TG_MODE_CORRUPT)
    return LJ_THR_TG_CORRUPT;
  if (mode != LJ_THR_TG_MODE_EMPTY)
    return LJ_THR_TG_EXPECT_MISMATCH;
  incoming.body = (TGState *)new_hold->body;
  incoming.key = new_hold->key;
  result = lj_thr_tg_validate_view(&incoming, 1);
  if (result != LJ_THR_TG_OK)
    return result;
  if (!lj_thr_signal_cell_prepare())
    return LJ_THR_TG_TLS_FAILURE;
  word = (uintptr_t)incoming.body | LJ_THR_TG_EXACT_TAG;
  /* TLS acquires ownership before the signal-visible mirror. A handler in the
  ** gap merely drops a sample; once it sees the mirror, the lease is live. */
  lj_thr_tls_word_set(word);
  lj_thr_signal_word_set(word);
  lj_tgregistry_borrow_init(new_hold);
  return LJ_THR_TG_OK;
}

LJThrTGResult lj_thr_tg_swap(const LJTGRegistryKey *expected_old,
                             LJTGRegistryBorrow *new_hold,
                             LJTGRegistryBorrow *old_hold)
{
  LJThrTGView current, incoming;
  LJThrTGMode mode;
  LJThrTGResult result;
  uintptr_t word;
  if (!lj_tgregistry_key_valid(expected_old) || !new_hold ||
      !new_hold->active || !new_hold->body || old_hold == new_hold ||
      !lj_thr_tg_borrow_empty(old_hold) ||
      ((uintptr_t)new_hold->body & LJ_THR_TG_TAG_MASK) != 0)
    return LJ_THR_TG_INVALID;
  mode = lj_thr_tg_view(&current);
  if (mode == LJ_THR_TG_MODE_CORRUPT)
    return LJ_THR_TG_CORRUPT;
  if (mode != LJ_THR_TG_MODE_KEYED ||
      !lj_tgregistry_key_equal(&current.key, expected_old))
    return LJ_THR_TG_EXPECT_MISMATCH;
  result = lj_thr_tg_validate_view(&current, 0);
  if (result != LJ_THR_TG_OK)
    return result;
  incoming.body = (TGState *)new_hold->body;
  incoming.key = new_hold->key;
  result = lj_thr_tg_validate_view(&incoming, 1);
  if (result != LJ_THR_TG_OK)
    return result;
  /* fork(2) changes the process identity represented by a signal cell. The
  ** inherited exact TLS lease stays valid in the child copy, but a swap must
  ** first admit a child-local mirror before either binding is changed. */
  if (!lj_thr_signal_cell_prepare())
    return LJ_THR_TG_TLS_FAILURE;
  word = (uintptr_t)incoming.body | LJ_THR_TG_EXACT_TAG;
  /* The old lease remains operation-owned until old_hold is materialized.
  ** Publish the new TLS owner before changing the signal mirror. */
  lj_thr_tls_word_set(word);
  lj_thr_signal_word_set(word);
  /* Both fungible token counts protect their exact bodies at this LP. */
  lj_thr_tg_move_out(old_hold, &current);
  lj_tgregistry_borrow_init(new_hold);
  return LJ_THR_TG_OK;
}

LJThrTGResult lj_thr_tg_clear(const LJTGRegistryKey *expected_old,
                              LJTGRegistryBorrow *old_hold)
{
  LJThrTGView current;
  LJThrTGMode mode;
  LJThrTGResult result;
  if (!lj_tgregistry_key_valid(expected_old) ||
      !lj_thr_tg_borrow_empty(old_hold))
    return LJ_THR_TG_INVALID;
  mode = lj_thr_tg_view(&current);
  if (mode == LJ_THR_TG_MODE_CORRUPT)
    return LJ_THR_TG_CORRUPT;
  if (mode != LJ_THR_TG_MODE_KEYED ||
      !lj_tgregistry_key_equal(&current.key, expected_old))
    return LJ_THR_TG_EXPECT_MISMATCH;
  result = lj_thr_tg_validate_view(&current, 0);
  if (result != LJ_THR_TG_OK)
    return result;
  /* A handler must stop seeing the body before TLS relinquishes ownership and
  ** before the caller can receive and release the reconstructed lease. */
  lj_thr_signal_word_set(0);
  lj_thr_tls_word_set(0);
  /* The lease count remains owned by this operation until materialized. */
  lj_thr_tg_move_out(old_hold, &current);
  return LJ_THR_TG_OK;
}

int lj_thr_tg_current_key(LJTGRegistryKey *key)
{
  LJThrTGView current;
  LJThrTGMode mode;
  LJThrTGResult result;
  if (!key)
    return LJ_THR_TG_INVALID;
  key->slot = NULL;
  key->incarnation = LJ_TGSLOT_INCARNATION_NONE;
  mode = lj_thr_tg_view(&current);
  if (mode == LJ_THR_TG_MODE_EMPTY || mode == LJ_THR_TG_MODE_RAW)
    return LJ_THR_TG_EXPECT_MISMATCH;
  if (mode == LJ_THR_TG_MODE_CORRUPT)
    return LJ_THR_TG_CORRUPT;
  result = lj_thr_tg_validate_view(&current, 0);
  if (result != LJ_THR_TG_OK)
    return result;
  *key = current.key;
  return LJ_THR_TG_OK;
}

static void lj_thr_tls_set_raw(TGState *tg)
{
  uintptr_t word;
#if LJ_THR_TG_SIGNAL_CACHE
  LJThrTGSignalCell *signal_cell = NULL;
#endif
  if (tg && ((uintptr_t)tg & LJ_THR_TG_SIGNAL_TAG_MASK) != 0)
    abort();
  if (!lj_thr_tg_tls_init())
    abort();  /* Void raw callers cannot safely report per-thread admission. */
  word = lj_thr_tls_word_get();
  if ((word & LJ_THR_TG_EXACT_TAG) != 0)
    abort();  /* A void raw call cannot move the exact lease represented here. */
  /* Clear the old mirror before changing a raw binding. The transitional raw
  ** profile tag is same-thread-only and rejected by the exact signal getter.
  ** Admission failure merely drops profiler samples. */
  lj_thr_signal_word_clear_if_registered();
#if LJ_THR_TG_SIGNAL_CACHE
  if (tg)
    signal_cell = lj_thr_signal_cell_prepare();
#endif
  lj_thr_tls_word_set((uintptr_t)tg);
#if LJ_THR_TG_SIGNAL_CACHE
  if (signal_cell)
    la_storeuptr_rel(&signal_cell->tagged_word,
                     (uintptr_t)tg | LJ_THR_TG_SIGNAL_RAW_TAG);
#endif
}

void lj_thr_set_tg(TGState *tg)
{
  lj_thr_tls_set_raw(tg);  /* Raw compatibility until lifecycle migration. */
}

TGState *lj_thr_get_tg(void)
{
  return lj_thr_tls_get();
}

TGState *lj_thr_get_tg_fallback(global_State *g)
{
  TGState *tg = lj_thr_tls_get();
  if (!g)
    return tg;
  return tg && tg->gl == g ? tg : g->main_tg;
}

static void state_gcscan_wait_no_l(void)
{
  (void)lj_thr_retry_yield(NULL);
}

int lj_state_claim(lua_State *L, uint32_t tid)
{
  uint32_t owner;
  if (!L || !lj_thr_id_is_owner(tid))
    return 0;
  for (;;) {
    if (lj_state_gcprep_state_acq(L) != LJ_STATE_GCPREP_NONE)
      return 0;
    owner = lj_state_owner_acq(L);
    if (owner == tid)
      return 1;
    if (owner == 0) {
      uint32_t expect = 0;
      if (lj_state_owner_cas(L, &expect, tid)) {
	if (LJ_LIKELY(lj_state_gcprep_state_acq(L) ==
		      LJ_STATE_GCPREP_NONE))
	  return 1;
	lj_state_release(L, tid);
	return 0;
      }
      continue;
    }
    if (owner == LJ_THREAD_GCPREP)
      return 0;
    if (owner == LJ_THREAD_GCSCAN) {
      state_gcscan_wait_no_l();
      continue;
    }
    return 0;
  }
}

int lj_state_tryclaim(lua_State *L, uint32_t tid, LJStateClaim *claim)
{
  uint32_t owner;
  if (claim) {
    claim->L = NULL;
    claim->tg_hint = NULL;
    claim->tid = 0;
    claim->release = 0;
  }
  if (!L || !lj_thr_id_is_owner(tid))
    return 0;
  for (;;) {
    if (lj_state_gcprep_state_acq(L) != LJ_STATE_GCPREP_NONE)
      return 0;
    owner = lj_state_owner_acq(L);
    if (owner == tid) {
      if (claim) {
	claim->L = L;
	claim->tid = tid;
      }
      return 1;
    }
    if (owner == 0) {
      uint32_t expect = 0;
      if (lj_state_owner_cas(L, &expect, tid)) {
	if (LJ_UNLIKELY(lj_state_gcprep_state_acq(L) !=
			LJ_STATE_GCPREP_NONE)) {
	  lj_state_release(L, tid);
	  return 0;
	}
	if (claim) {
	  claim->L = L;
	  claim->tg_hint = NULL;
	  claim->tid = tid;
	  claim->release = 1;
	}
	return 1;
      }
      continue;
    }
    if (owner == LJ_THREAD_GCPREP)
      return 0;
    if (owner == LJ_THREAD_GCSCAN) {
      state_gcscan_wait_no_l();
      continue;
    }
    return 0;
  }
}

int lj_state_resumeclaim(lua_State *L, uint32_t tid, LJStateClaim *claim)
{
  if (!lj_state_tryclaim(L, tid, claim))
    return 0;
  /*
  ** Suspended coroutines are TG-neutral. A resume claim makes the coroutine
  ** temporarily run on the resumer's TG, then restores the previous hint
  ** before publishing the stack as unowned. The previous hint is usually NULL,
  ** but VM-event/callback states may already be attached while ownerless.
  */
  if (claim && claim->release) {
    claim->tg_hint = L->tg_hint;
    L->tg_hint = lj_thr_get_tg_fallback(G(L));
  }
  return 1;
}

int lj_state_gcscan_claim(lua_State *L, LJStateClaim *claim)
{
  uint32_t owner;
  if (claim) {
    claim->L = NULL;
    claim->tg_hint = NULL;
    claim->tid = 0;
    claim->release = 0;
  }
  if (!L)
    return 0;
  for (;;) {
    if (lj_state_gcprep_state_acq(L) != LJ_STATE_GCPREP_NONE)
      return 0;
    owner = lj_state_owner_acq(L);
    if (owner == 0) {
      uint32_t expect = 0;
      if (lj_state_owner_cas(L, &expect, LJ_THREAD_GCSCAN)) {
	if (claim) {
	  claim->L = L;
	  claim->tg_hint = NULL;
	  claim->tid = LJ_THREAD_GCSCAN;
	  claim->release = 1;
	}
	return 1;
      }
      continue;
    }
    if (owner == LJ_THREAD_GCSCAN) {
      /*
      ** The scan sentinel intentionally carries no owner id. A GC scanner can
      ** rediscover the same lua_State through another grey edge while it owns
      ** that sentinel, so waiting here can self-deadlock. Report the state as
      ** busy; GC2 will requeue it or let the owning thread satisfy NEEDSCAN.
      */
      return 0;  /* 05 section 5.7.2: scan claim handoff. */
    }
    if (owner == LJ_THREAD_GCPREP)
      return 0;
    return 0;
  }
}

static void state_stack_dirty(lua_State *L, uint32_t tid)
{
  TGState *tg;
  if (!L || !lj_thr_id_is_owner(tid))
    return;
  tg = lj_tg_find_owner(G(L), tid);
  if (tg)
    lj_tg_stack_dirty_epoch_add_rlx(tg, 1);
}

void lj_state_dropclaim(LJStateClaim *claim)
{
  if (claim && claim->release) {
    lj_state_release(claim->L, claim->tid);
    claim->release = 0;
  }
}

void lj_state_resume_release(lua_State *L, uint32_t tid)
{
  if (L && tid != 0) {
    L->tg_hint = NULL;
    lj_state_release(L, tid);
  }
}

uint32_t lj_state_resume_release_result(lua_State *L, uint32_t tid,
					 uint32_t result)
{
  lj_state_resume_release(L, tid);
  return result;
}

void lj_state_dropresumeclaim(LJStateClaim *claim)
{
  if (claim && claim->release) {
    claim->L->tg_hint = claim->tg_hint;
    lj_state_release(claim->L, claim->tid);
    claim->release = 0;
  }
}

uint32_t lj_state_owner_wait(lua_State *L, lua_State *target, uint32_t owner,
			     int64_t ns)
{
  TGState *tg = L ? L2TG(L) : lj_thr_tls_get();
  uint32_t actions = 0;
  if (!target || owner == 0)
    return 0;
  /*
  ** State-owner waits are native waits, not VM stalls. Mark the TG native so
  ** safepoint/STOPREQ handshakes can observe it while the futex wait blocks.
  */
  if (tg)
    lj_native_enter(tg);
  lj_state_owner_futex_wait(target, owner, ns);
  if (L) {
    actions = lj_native_leave(L);
  } else if (tg) {
    actions = lj_native_leave_tg(tg);
  }
  return actions;
}

void lj_state_release(lua_State *L, uint32_t tid)
{
  if (L && tid != 0) {
    uint32_t owner = lj_state_owner_acq(L);
    lj_assertX(owner == tid, "lua_State owner mismatch");
    state_stack_dirty(L, tid);
    if (lj_thr_id_is_owner(tid)) {
      uint32_t expect = tid;
      /*
      ** Publish a short terminal ownership interval before making the state
      ** claimable. A collector which sampled the old owner either publishes
      ** NEEDSCAN before this CAS (and is observed below), or observes GCSCAN
      ** and retains concrete work. This closes the set-NEEDSCAN/release race
      ** without a lock, wait, or post-release body access.
      */
      if (LJ_UNLIKELY(!lj_state_owner_cas(
			 L, &expect, LJ_THREAD_GCSCAN))) {
	lj_assertX(0, "lua_State release sentinel CAS failed");
	return;
      }
      lj_gc2_thread_owner_releasing(G(L), L, tid);
      lj_state_owner_rel(L, 0);
    } else {
      UNUSED(owner);
      lj_state_owner_rel(L, 0);
    }
    lj_state_owner_futex_wake(L, 0x7fffffff);
  }
}

uint32_t lj_thr_cpucount(void)
{
#if LJ_TARGET_WINDOWS
  SYSTEM_INFO si;
  GetSystemInfo(&si);
  return si.dwNumberOfProcessors ? (uint32_t)si.dwNumberOfProcessors : 1u;
#else
  long n = sysconf(_SC_NPROCESSORS_ONLN);
  return n > 0 ? (uint32_t)n : 1u;
#endif
}

void lj_thr_fence(void)
{
  la_fence_seq();  /* 09 section 9.1 threading.fence memory edge. */
}

uint32_t lj_thr_yield(lua_State *L)
{
  TGState *tg = L ? L2TG(L) : lj_thr_tls_get();
  uint32_t actions = 0;
  if (tg)
    lj_native_enter(tg);
#if LJ_TARGET_WINDOWS
  if (!SwitchToThread())
    Sleep(0);
#else
  (void)sched_yield();
#endif
  if (L) {
    actions = lj_native_leave(L);
  } else if (tg) {
    actions = lj_native_leave_tg(tg);
  }
  return actions;
}

uint32_t lj_thr_retry_yield(lua_State *L)
{
  uint32_t i;
  for (i = 0; i < 64; i++)
    la_cpu_pause();
  return lj_thr_yield(L);
}

uint32_t lj_thr_sleep_ns(lua_State *L, int64_t ns)
{
  TGState *tg = L ? L2TG(L) : lj_thr_tls_get();
  uint32_t actions = 0;
  if (tg)
    lj_native_enter(tg);  /* 09 section 9.1 sleep is a native region. */
  if (ns > 0) {
#if LJ_TARGET_WINDOWS
    uint64_t ms = ((uint64_t)ns + 999999u) / 1000000u;
    if (ms == 0)
      ms = 1;
    Sleep(ms >= INFINITE ? INFINITE - 1u : (DWORD)ms);
#else
    struct timespec req;
    req.tv_sec = (time_t)(ns / 1000000000ll);
    req.tv_nsec = (long)(ns % 1000000000ll);
    while (nanosleep(&req, &req) != 0 && errno == EINTR)
      ;
#endif
  }
  if (L) {
    actions = lj_native_leave(L);
  } else if (tg) {
    actions = lj_native_leave_tg(tg);  /* TG-private owner poll, no Lua stack. */
  }
  return actions;
}

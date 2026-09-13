/*
** nbtab_model.c — standalone, stress-tested model of the concurrent table
** protocol of 06_concurrent_objects.md §6.2–6.3 (KEYLOCK claims, FORWARD
** migration, cooperative resize with helpers, chained hash, prepend-wins
** linearization). Integer keys/values stand in for TValues; the structure
** and orderings are the normative part — port them verbatim (M5).
**
** Build & run:
**   cc -O2 -Wall -Wextra -Werror -pthread nbtab_model.c -o nbtab && ./nbtab
**
** REFINEMENTS THIS MODEL DISCOVERED (normative; fold into 06 at M5):
**  R-a  Concurrent inserts of one key may create duplicate chain nodes;
**       reads/writes use FIRST match from head (prepend = linearization
**       point); shadowed duplicates are dropped at migration by the
**       "first-match-only migrates" rule.
**  R-b  Insert-publish vs migration-start needs a seq_cst fence pair:
**       inserter fences after linking, then re-checks gen->next_gen;
**       the resize claimant fences after publishing next_gen, before the
**       cursor scans keys. Otherwise a just-linked node can be missed by
**       the cursor while the inserter also misses the resize.
**  R-c  Any key-claim consumes one freecount reservation taken up front;
**       abandoned claims return it.
**  R-d  The resize CLAIM IS the next_gen CAS (NULL→g2). A separate
**       "copying" flag is observable before next_gen and races helpers
**       into a NULL gen (found by ASAN). States: next_gen==NULL idle;
**       next_gen!=NULL && !done copying; done set by last migrated index.
**  R-e  A value store into a fresh claimed node must be CAS(NIL→v): if it
**       meets FORWARD the freeze beat the publish — abandon the node
**       (key→EMPTY, reservation returned; slot stays poisoned-FORWARD
**       until the gen retires) and redo the op in the next gen.
**  R-f  Writer meeting FORWARD on an existing key hops to next_gen and
**       performs the write THERE (never "treat as done") — else the
**       update is lost (found by the seq oracle).
**  R-g  Migration inserts with PUT-IF-ABSENT into the new gen: presence
**       there (even a NIL tombstone) means a post-freeze, newer write
**       landed; the frozen value must not clobber it. The in-flight
**       inserter's R-b redo, by contrast, is a normal set (its op has not
**       returned, so late linearization is legal).
**  R-h  gen publication advances along the next_gen chain ("done" gens),
**       so nested resizes (G→G2→G3) compose.
**
** Memory reclamation: retired gens go to an exit-time free list. In the
** runtime this is defer_free with handshake-epoch grace (05 §5.9); the
** model has no safepoints, and leak-until-exit is sound for protocol
** validation.
*/
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <pthread.h>

#define A_RLX __ATOMIC_RELAXED
#define A_ACQ __ATOMIC_ACQUIRE
#define A_REL __ATOMIC_RELEASE
#define A_ACQ_REL __ATOMIC_ACQ_REL
#define ld64(p,mo)    __atomic_load_n((p),mo)
#define st64(p,v,mo)  __atomic_store_n((p),(v),mo)
#define ld32(p,mo)    __atomic_load_n((p),mo)
#define st32(p,v,mo)  __atomic_store_n((p),(v),mo)
#define ldp(p,mo)     __atomic_load_n((p),mo)
#define stp(p,v,mo)   __atomic_store_n((p),(v),mo)
#define cas64(p,e,d,s,f) __atomic_compare_exchange_n((p),(e),(d),0,s,f)
#define cas32(p,e,d,s,f) __atomic_compare_exchange_n((p),(e),(d),0,s,f)
#define casp(p,e,d,s,f)  __atomic_compare_exchange_n((p),(e),(d),0,s,f)
#define fadd32(p,v,mo)   __atomic_fetch_add((p),(v),mo)
#define fsub32(p,v,mo)   __atomic_fetch_sub((p),(v),mo)
#define fence_seq()      __atomic_thread_fence(__ATOMIC_SEQ_CST)

#define KEY_EMPTY   0ull
#define KEY_LOCK    (~0ull)            /* LJ_TKEYLOCK analog              */
#define VAL_NIL     0ull
#define VAL_FORWARD (~0ull)            /* LJ_TFORWARD analog              */
#define NO_NODE     0xffffffffu

typedef struct Node {
  uint64_t key;                        /* EMPTY→LOCK→K, then immutable    */
  uint64_t val;
  uint32_t next;                       /* chain index, NO_NODE = end      */
  uint32_t _pad;
} Node;

typedef struct Gen {
  uint32_t hmask;                      /* capacity-1                      */
  uint32_t done;                       /* all indices migrated (R-d)      */
  struct Gen *next_gen;                /* claim & copy target (R-d)       */
  _Alignas(64) int32_t  freecount;     /* reservations left (R-c)         */
  _Alignas(64) uint32_t cursor;        /* migration index ticket          */
  _Alignas(64) uint32_t done_count;
  struct Gen *retire_link;             /* model-only free list            */
  Node nodes[];
} Gen;

typedef struct Tab {
  Gen *gen;                            /* RCU pointer (acq/rel)           */
  _Alignas(64) Gen *retired;
} Tab;

static uint32_t hash64(uint64_t k)
{ k ^= k>>33; k *= 0xff51afd7ed558ccdull; k ^= k>>33; return (uint32_t)k; }

static Gen *gen_new(uint32_t cap)
{
  Gen *g = calloc(1, sizeof(Gen) + cap*sizeof(Node));
  assert(g && (cap & (cap-1)) == 0);
  g->hmask = cap-1; g->freecount = (int32_t)cap;
  for (uint32_t i = 0; i < cap; i++) g->nodes[i].next = NO_NODE;
  return g;
}

static void tab_init(Tab *t, uint32_t cap){ memset(t,0,sizeof *t); t->gen = gen_new(cap); }

static void migrate_help(Tab *t, Gen *g);

/* advance t->gen along completed migrations (R-h) */
static void help_publish(Tab *t)
{
  for (;;) {
    Gen *g = ldp(&t->gen, A_ACQ);
    if (!ld32(&g->done, A_ACQ)) return;
    Gen *nx = ldp(&g->next_gen, A_ACQ);
    if (!nx) return;
    if (casp(&t->gen, &g, nx, A_REL, A_RLX)) {
      Gen *r;
      do { r = ldp(&t->retired, A_RLX); g->retire_link = r; }
      while (!casp(&t->retired, &r, g, A_REL, A_RLX));
    }
  }
}

/* ---- lookup (06 §6.3.3): wait-free modulo bounded re-walk ------------- */
static uint64_t tab_get(Tab *t, uint64_t k)
{
  Gen *g = ldp(&t->gen, A_ACQ);
  while (g) {
    uint32_t h = hash64(k) & g->hmask;
    int saw_lock, rewalked = 0;
walk:
    saw_lock = 0;
    for (uint32_t n = h; n != NO_NODE; n = ld32(&g->nodes[n].next, A_ACQ)) {
      uint64_t nk = ld64(&g->nodes[n].key, A_RLX);
      if (nk == k) {
        uint64_t v = ld64(&g->nodes[n].val, A_ACQ);
        if (v == VAL_FORWARD) goto nextgen;   /* migrated: value lives on */
        return v;                              /* first match wins (R-a)  */
      }
      if (nk == KEY_LOCK) saw_lock = 1;
    }
    if (saw_lock && !rewalked) { rewalked = 1; goto walk; }
nextgen:
    g = ldp(&g->next_gen, A_ACQ);
  }
  return VAL_NIL;
}

/* ---- insert/set into one gen; may hop forward on FORWARD (R-f) -------- */
/* mode: 0 = set, 1 = put-if-absent (migration, R-g).
** Returns 1 done, 0 = gen chain end was FULL (caller resizes & retries). */
static int gen_insert(Tab *t, Gen *g, uint64_t k, uint64_t v, int ifabsent)
{
  (void)t;
  for (;;) {                                   /* gen-hop loop            */
    uint32_t h = hash64(k) & g->hmask;
    /* existing first-match? */
    for (uint32_t n = h; n != NO_NODE; n = ld32(&g->nodes[n].next, A_ACQ)) {
      if (ld64(&g->nodes[n].key, A_RLX) == k) {
        uint64_t cur = ld64(&g->nodes[n].val, A_ACQ);
        for (;;) {
          if (cur == VAL_FORWARD) {            /* R-f: write home moved   */
            Gen *nx = ldp(&g->next_gen, A_ACQ);
            assert(nx && "FORWARD implies a claimed next gen");
            g = nx; goto genhop;
          }
          if (ifabsent) return 1;              /* R-g: newer write exists */
          if (cas64(&g->nodes[n].val, &cur, v, A_REL, A_ACQ)) return 1;
        }
      }
    }
    /* miss in this gen. If a copy target exists, this gen is frozen for
    ** NEW keys: insert into the newest gen instead (helps keep chains
    ** single-home; lookups hop). */
    {
      Gen *nx = ldp(&g->next_gen, A_ACQ);
      if (nx) { g = nx; goto genhop; }
    }
    /* reserve a node (R-c) */
    if (fsub32(&g->freecount, 1, A_ACQ_REL) <= 0) {
      fadd32(&g->freecount, 1, A_RLX);
      return 0;                                /* full → caller resizes   */
    }
    /* claim main or scan a free node */
    {
      uint32_t slot = NO_NODE; int is_main = 0;
      uint64_t ek = KEY_EMPTY;
      if (cas64(&g->nodes[h].key, &ek, KEY_LOCK, A_ACQ_REL, A_RLX)) {
        slot = h; is_main = 1;
      } else {
        for (uint32_t probe = 1;; probe++) {
          uint32_t i = (h + probe) & g->hmask;
          ek = KEY_EMPTY;
          if (ld64(&g->nodes[i].key, A_RLX) == KEY_EMPTY &&
              cas64(&g->nodes[i].key, &ek, KEY_LOCK, A_ACQ_REL, A_RLX)) {
            slot = i; break;
          }
        }
      }
      /* publish value first, guarded against a racing freeze (R-e) */
      uint64_t expv = VAL_NIL;
      if (!cas64(&g->nodes[slot].val, &expv, v, A_REL, A_ACQ)) {
        assert(expv == VAL_FORWARD);
        st64(&g->nodes[slot].key, KEY_EMPTY, A_REL);  /* abandon (R-e)   */
        fadd32(&g->freecount, 1, A_RLX);
        Gen *nx = ldp(&g->next_gen, A_ACQ);
        assert(nx && "freeze implies claimed next gen");
        g = nx; goto genhop;
      }
      st64(&g->nodes[slot].key, k, A_REL);     /* publish key             */
      if (!is_main) {                          /* CAS-prepend into chain  */
        uint32_t old = ld32(&g->nodes[h].next, A_ACQ);
        do { st32(&g->nodes[slot].next, old, A_RLX); }
        while (!cas32(&g->nodes[h].next, &old, slot, A_REL, A_ACQ));
      }
      fence_seq();                             /* R-b: inserter side      */
      Gen *nx = ldp(&g->next_gen, A_ACQ);
      if (nx) {
        /* cursor may have missed us (key was LOCK / link in flight) and
        ** may have frozen-or-not our value: redo as a normal set in the
        ** successor (legal late linearization; see R-g note). */
        g = nx; ifabsent = 0; goto genhop;
      }
      return 1;
    }
genhop: ;
  }
}

/* ---- migration (R-a, R-d, R-g, R-h) ----------------------------------- */
static void migrate_index(Tab *t, Gen *g, uint32_t idx)
{
  Node *nd = &g->nodes[idx];
  uint64_t v = ld64(&nd->val, A_ACQ);
  while (v != VAL_FORWARD &&
         !cas64(&nd->val, &v, VAL_FORWARD, A_ACQ_REL, A_ACQ))
    ;
  if (v != VAL_FORWARD && v != VAL_NIL) {
    uint64_t k = ld64(&nd->key, A_RLX);
    if (k != KEY_EMPTY && k != KEY_LOCK) {
      /* dedupe shadowed duplicates (R-a): only the head-most node for k
      ** carries the live value */
      int first = 0;
      for (uint32_t n = hash64(k) & g->hmask; n != NO_NODE;
           n = ld32(&g->nodes[n].next, A_ACQ)) {
        if (ld64(&g->nodes[n].key, A_RLX) == k) { first = (n == idx); break; }
      }
      if (first) {
        Gen *g2 = ldp(&g->next_gen, A_ACQ);    /* non-NULL by R-d         */
        while (!gen_insert(t, g2, k, v, 1)) {  /* put-if-absent (R-g)     */
          /* nested resize needed on g2 */
          Gen *g3 = gen_new((g2->hmask+1)*2);
          Gen *expect = NULL;
          if (!casp(&g2->next_gen, &expect, g3, A_ACQ_REL, A_ACQ)) free(g3);
          fence_seq();                         /* R-b claimant side       */
          g2 = ldp(&g2->next_gen, A_ACQ);
        }
      }
    }
  }
  if (fadd32(&g->done_count, 1, A_ACQ_REL) + 1 == g->hmask + 1) {
    st32(&g->done, 1, A_REL);
    help_publish(t);
  }
}

static void migrate_help(Tab *t, Gen *g)
{
  for (;;) {
    uint32_t idx = fadd32(&g->cursor, 1, A_RLX);
    if (idx > g->hmask) break;
    migrate_index(t, g, idx);
  }
  /* stragglers finish their claimed indices; operation-level lock-free
  ** (someone always completes); model spins briefly until done. */
  while (!ld32(&g->done, A_ACQ))
    for (volatile int s = 0; s < 64; s++) ;
  help_publish(t);
}

static void start_resize(Tab *t, Gen *g)
{
  Gen *g2 = gen_new((g->hmask + 1) * 2);
  Gen *expect = NULL;
  if (!casp(&g->next_gen, &expect, g2, A_ACQ_REL, A_ACQ))
    free(g2);                                  /* lost the claim (R-d)    */
  fence_seq();                                 /* R-b: claimant side      */
  migrate_help(t, g);
}

/* ---- public ops -------------------------------------------------------- */
static void tab_set(Tab *t, uint64_t k, uint64_t v)
{
  for (;;) {
    Gen *g = ldp(&t->gen, A_ACQ);
    if (gen_insert(t, g, k, v, 0)) { help_publish(t); return; }
    /* chain end was full: find it and resize it */
    Gen *cur = g, *nx;
    while ((nx = ldp(&cur->next_gen, A_ACQ)) != NULL) cur = nx;
    start_resize(t, cur);
  }
}
static void tab_del(Tab *t, uint64_t k){ tab_set(t, k, VAL_NIL); }

/* ======================= stress harness ================================ */
#define NTHREADS   8
#define KEYSPACE   512
#define PRIV_KEYS  256
#define OPS        60000
#define TOTKEYS    (KEYSPACE + NTHREADS*PRIV_KEYS)

static Tab T;
static uint64_t oracle[NTHREADS][TOTKEYS];     /* max seq written, +1     */

static uint64_t mkval(int tid, uint64_t seq){ return ((uint64_t)(tid+1)<<48) | (seq+1); }
static int      val_tid(uint64_t v){ return (int)(v>>48) - 1; }
static uint64_t val_seq(uint64_t v){ return (v & 0xffffffffffffull) - 1; }

typedef struct { int tid; uint64_t rng; long checks; } W;
static uint64_t xr(uint64_t *s){ *s ^= *s<<13; *s ^= *s>>7; *s ^= *s<<17; return *s; }

static void *worker(void *arg)
{
  W *w = arg; int tid = w->tid; uint64_t seq = 0;
  uint64_t privbase = KEYSPACE + (uint64_t)tid * PRIV_KEYS;
  for (int op = 0; op < OPS; op++) {
    uint64_t r = xr(&w->rng);
    uint64_t k = (r & 1) ? (privbase + 1 + (r>>8) % PRIV_KEYS)
                         : (1 + (r>>8) % KEYSPACE);
    uint32_t kind = (uint32_t)((r>>32) % 10);
    if (kind < 5) {
      uint64_t s = seq++;
      st64(&oracle[tid][k-1], s+1, A_REL);     /* before table write      */
      tab_set(&T, k, mkval(tid, s));
    } else if (kind < 6) {
      tab_del(&T, k);
    } else {
      uint64_t v = tab_get(&T, k);
      if (v != VAL_NIL) {
        assert(v != VAL_FORWARD && v != KEY_LOCK);
        int wt = val_tid(v); uint64_t ws = val_seq(v);
        assert(wt >= 0 && wt < NTHREADS);
        uint64_t omax = ld64(&oracle[wt][k-1], A_ACQ);
        assert(omax >= ws+1 && "read a value never written (HB violation)");
        w->checks++;
      }
    }
    if ((r % 97) == 0) {                       /* single-writer program order */
      uint64_t pk = privbase + 1 + (r>>40) % PRIV_KEYS;
      uint64_t s = seq++;
      st64(&oracle[tid][pk-1], s+1, A_REL);
      tab_set(&T, pk, mkval(tid, s));
      uint64_t v = tab_get(&T, pk);
      assert(v != VAL_NIL && val_tid(v) == tid && val_seq(v) == s &&
             "lost my own write (single-writer key)");
      w->checks++;
    }
  }
  return 0;
}

static void final_validate(void)
{
  long live = 0;
  for (uint64_t k = 1; k <= TOTKEYS; k++) {
    uint64_t v = tab_get(&T, k);
    assert(v != VAL_FORWARD);
    if (v != VAL_NIL) {
      int wt = val_tid(v);
      assert(wt >= 0 && wt < NTHREADS);
      assert(ld64(&oracle[wt][k-1], A_RLX) >= val_seq(v)+1);
      live++;
    }
  }
  /* the final gen must be quiescent: no LOCK keys, no FORWARD vals */
  Gen *g = T.gen;
  assert(g->next_gen == NULL);
  for (uint32_t i = 0; i <= g->hmask; i++) {
    assert(ld64(&g->nodes[i].key, A_RLX) != KEY_LOCK);
    assert(ld64(&g->nodes[i].val, A_RLX) != VAL_FORWARD);
  }
  printf("  final: %ld live keys validated, cap %u quiescent\n",
         live, g->hmask+1);
}

int main(void)
{
  pthread_t th[NTHREADS]; W ws[NTHREADS];
  tab_init(&T, 16);                            /* tiny: forces resizes    */
  for (int i = 0; i < NTHREADS; i++) {
    ws[i] = (W){ .tid = i, .rng = 0x1234 + (uint64_t)i*0x9e37, .checks = 0 };
    int rc = pthread_create(&th[i], 0, worker, &ws[i]); assert(!rc);
  }
  long checks = 0;
  for (int i = 0; i < NTHREADS; i++) { pthread_join(th[i], 0); checks += ws[i].checks; }
  final_validate();
  long resizes = 0;
  for (Gen *g = T.retired; g; g = g->retire_link) resizes++;
  printf("nbtab_model OK: %d threads x %d ops, %ld validated reads, "
         "%ld gens retired, final cap %u\n",
         NTHREADS, OPS, checks, resizes, T.gen->hmask + 1);
  assert(resizes >= 3 && "stress did not exercise resize");
  return 0;
}

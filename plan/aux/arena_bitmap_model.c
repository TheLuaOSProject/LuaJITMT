/*
** arena_bitmap_model.c — standalone, tested model of the arena block/mark
** bitmap design (04_allocator.md §4.1.1) and its sweep identities.
**
** Build & run:   cc -O2 -Wall -Wextra -Werror arena_bitmap_model.c && ./a.out
**
** What it proves, on randomized heaps (fixed seed, reproducible):
**   P1  encode→decode roundtrip of the differential (block,mark) encoding.
**   P2  major sweep identity   block' = block & mark ; mark' = block ^ mark
**       frees exactly the white blocks, turns black→white, preserves
**       extents — verified cell-by-cell against a shadow object model.
**   P3  minor sweep identity   block' = block & mark ; mark' = block | mark
**       frees whites, KEEPS blacks black.
**   P4  the free-run scanner (the allocator's bin refill loop, 04 §4.6)
**       finds exactly the maximal free ranges, using word ops + ctz.
**   P5  "object starts" mask is block|mark (used by markers to validate
**       interior pointers if ever needed, and by the scanner).
**
** Port the sweep + scanner loops into lj_arena.c verbatim (milestone M2).
*/
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#define NCELLS   4096
#define NWORDS   (NCELLS/64)
#define FIRST    64            /* cells 0..63 = metadata, never objects */

typedef struct { uint64_t block[NWORDS], mark[NWORDS]; } BM;

/* shadow: per-cell state */
enum { S_EXT=0, S_FREE=1, S_WHITE=2, S_BLACK=3 };

static inline int  bm_get(const uint64_t *b, int i){ return (b[i>>6]>>(i&63))&1; }
static inline void bm_set(uint64_t *b, int i){ b[i>>6] |= 1ull<<(i&63); }

static int decode_cell(const BM *bm, int i)
{ return (bm_get(bm->block,i)<<1) | bm_get(bm->mark,i); }
/* (block,mark): 00=EXT 01=FREE 10=WHITE 11=BLACK — matches enum order:
**  S_EXT=0b00, S_FREE=0b01, S_WHITE=0b10, S_BLACK=0b11 */

static void encode(BM *bm, const uint8_t *shadow)
{
  memset(bm, 0, sizeof *bm);
  for (int i = 0; i < NCELLS; i++) {
    if (shadow[i] & 2) bm_set(bm->block, i);
    if (shadow[i] & 1) bm_set(bm->mark,  i);
  }
}

/* ---- P2/P3: sweep ---------------------------------------------------- */
static void sweep_words(BM *bm, int minor)
{
  for (int w = 0; w < NWORDS; w++) {
    uint64_t b = bm->block[w], m = bm->mark[w];
    bm->block[w] = b & m;
    bm->mark[w]  = minor ? (b | m) : (b ^ m);
  }
}
static void sweep_shadow(uint8_t *s, int minor)
{
  for (int i = FIRST; i < NCELLS; ) {
    int st = s[i], len = 1;
    while (i+len < NCELLS && s[i+len] == S_EXT) len++;
    if (st == S_WHITE) {                      /* freed: start + extents   */
      s[i] = S_FREE;                          /* (extents stay 00)        */
    } else if (st == S_BLACK) {
      s[i] = minor ? S_BLACK : S_WHITE;
    } /* S_FREE stays free */
    i += len;
  }
}

/* ---- P4: free-run scanner (allocator bin refill) --------------------- */
/* A maximal free run = consecutive cells covered by FREE blocks.
** Returns runs via callback; word-accelerated start-bit iteration. */
typedef void (*run_cb)(int start, int len, void *u);
static void scan_free_runs(const BM *bm, run_cb cb, void *u)
{
  int run_start = -1, i = FIRST;
  while (i < NCELLS) {
    uint64_t starts = (bm->block[i>>6] | bm->mark[i>>6]) >> (i&63);
    if (!starts) {                       /* rest of word is extents       */
      i = (i|63) + 1; continue;          /* extents extend current block  */
    }
    int hop = __builtin_ctzll(starts);
    i += hop;                            /* i = next block start ≥ old i  */
    if (i >= NCELLS) break;
    int st = decode_cell(bm, i);
    if (st == S_FREE) { if (run_start < 0) run_start = i; }
    else { if (run_start >= 0) { cb(run_start, i - run_start, u); run_start = -1; } }
    i++;                                 /* move past this start bit      */
  }
  /* trailing extents after the last start belong to the last block; if it
  ** was free the run extends to NCELLS */
  if (run_start >= 0) cb(run_start, NCELLS - run_start, u);
}
static void scan_free_runs_ref(const uint8_t *s, run_cb cb, void *u)
{
  int run_start = -1;
  for (int i = FIRST; i < NCELLS; ) {
    int len = 1; while (i+len < NCELLS && s[i+len] == S_EXT) len++;
    if (s[i] == S_FREE) { if (run_start < 0) run_start = i; }
    else { if (run_start >= 0) { cb(run_start, i - run_start, u); run_start = -1; } }
    i += len;
  }
  if (run_start >= 0) cb(run_start, NCELLS - run_start, u);
}
struct runlist { int n; int start[NCELLS]; int len[NCELLS]; };
static void collect(int s, int l, void *u)
{ struct runlist *r = u; r->start[r->n]=s; r->len[r->n]=l; r->n++; }

/* ---- harness ---------------------------------------------------------- */
static uint64_t rngs = 0x9e3779b97f4a7c15ull;
static uint64_t rnd(void){ rngs ^= rngs<<13; rngs ^= rngs>>7; rngs ^= rngs<<17; return rngs; }

int main(void)
{
  uint8_t shadow[NCELLS], shadow2[NCELLS];
  BM bm;
  int cycles = 20000, total_runs = 0;

  for (int c = 0; c < cycles; c++) {
    /* build a random heap: sequence of objects (1..16 cells) and gaps */
    memset(shadow, S_EXT, sizeof shadow);
    for (int i = 0; i < FIRST; i++) shadow[i] = S_EXT;  /* meta: never read */
    int i = FIRST;
    while (i < NCELLS) {
      int len = 1 + (int)(rnd() % 16); if (i+len > NCELLS) len = NCELLS - i;
      uint32_t kind = rnd() % 10;
      shadow[i] = (kind < 2) ? S_FREE : (kind < 8) ? S_WHITE : S_BLACK;
      for (int k = 1; k < len; k++) shadow[i+k] = S_EXT;
      i += len;
    }
    encode(&bm, shadow);

    /* P1: roundtrip */
    for (int k = FIRST; k < NCELLS; k++)
      assert(decode_cell(&bm, k) == shadow[k]);

    /* P5: starts mask */
    for (int k = FIRST; k < NCELLS; k++) {
      int is_start = shadow[k] != S_EXT;
      assert(((bm_get(bm.block,k)|bm_get(bm.mark,k)) != 0) == is_start);
    }

    /* random extra marking: promote some whites to black (mark phase) */
    for (int k = FIRST; k < NCELLS; k++)
      if (shadow[k] == S_WHITE && (rnd() & 3) == 0)
        { shadow[k] = S_BLACK; bm_set(bm.mark, k); }

    /* P2 or P3: sweep both ways, compare word-identity vs shadow rules */
    int minor = (int)(rnd() & 1);
    memcpy(shadow2, shadow, sizeof shadow);
    sweep_words(&bm, minor);
    sweep_shadow(shadow2, minor);
    for (int k = FIRST; k < NCELLS; k++)
      assert(decode_cell(&bm, k) == shadow2[k]);

    /* P4: free-run scanner vs reference */
    struct runlist a = {0}, b = {0};
    scan_free_runs(&bm, collect, &a);
    scan_free_runs_ref(shadow2, collect, &b);
    assert(a.n == b.n);
    for (int k = 0; k < a.n; k++) {
      assert(a.start[k] == b.start[k] && a.len[k] == b.len[k]);
      assert(a.len[k] >= 1);
    }
    total_runs += a.n;
  }
  printf("arena_bitmap_model OK: %d cycles, %d free runs verified\n",
         cycles, total_runs);
  return 0;
}

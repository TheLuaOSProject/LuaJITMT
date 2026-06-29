# LuaJIT-MT: Lockless Multithreaded LuaJIT — Implementation Report

## 00. READ ME FIRST — Operating Instructions for the Implementing Agent

This is the entry point of a 15-document implementation specification plus
auxiliary code. It tells you exactly how to consume the rest, what the ground
rules are, and how to verify your work at every step.

### 0.1 What you are building

A fork of LuaJIT v2.1 in which multiple OS threads execute Lua code in
parallel against **one shared heap** (one `global_State`), with:

- a **lockless runtime**: no mutexes anywhere on hot paths of the
  interpreter, JIT-compiled code, allocator, or GC; coordination is done with
  atomics, per-thread ownership, soft handshakes, and cooperative lock-free
  protocols (precise definition of "lockless" in `02_memory_model.md` §2.2);
- a **fully concurrent GC**: on-the-fly (no global stop-the-world) parallel
  mark & sweep, derived from Mike Pall's LuaJIT 3.0 arena GC design and the
  FUGC/DLG family of on-the-fly collectors;
- **full JIT support**: tracing, trace execution, side traces, FFI on trace —
  all while other threads run;
- **full Lua semantics** including shared mutable upvalues across threads
  (via the "cell" upvalue model, `06_concurrent_objects.md` §6.4);
- a `threading.*` standard library (spawn/join/channels/etc.,
  `09_threading_api.md`);
- **current lockless bytecode loading** (`BCDUMP_VERSION 4` chunks load and
  run; pre-lockless v2/v3 dumps are rejected at the header because their
  open-upvalue encoding is not the supported lockless ABI; see
  `10_bytecode_compat.md`).

### 0.2 Pinned source

All file names, line numbers, struct fields and function names in these
documents were verified against:

    repo:   https://github.com/LuaJIT/LuaJIT.git  branch v2.1
    commit: b925b3e3fc6771171602323b45fbe9fb8fc90369
    ident:  "LuaJIT 2.1.1780076327"

If you work from a different commit, re-verify every cited symbol with grep
before editing; the documents tell you the grep to run wherever a list is
long or likely to drift. Never trust a line number over a symbol name.

### 0.3 Reading order

Read in numeric order once, completely, before writing any code:

    00_README_AGENT.md            (this file)
    01_architecture_overview.md   architecture + all binding decisions (ADRs)
    02_memory_model.md            atomicity rules; the lj_atomic.h layer
    03_runtime_state_split.md     global_State / GG_State / per-thread TG block
    04_allocator.md               lock-free arena allocator (replaces lj_alloc)
    05_gc_concurrent.md           the concurrent GC (largest document)
    06_concurrent_objects.md      tables, strings, upvalue cells, misc objects
    07_interpreter_vm.md          vm_x64.dasc changes (safepoints, barriers, ops)
    08_jit_compiler.md            recorder token, trace publication, mcode, IR
    09_threading_api.md           threading.* library spec + implementation
    10_bytecode_compat.md         BCDUMP v4, new opcodes, current dump handling
    11_ffi_concurrency.md         CTState, cdata, callbacks, native regions
    12_implementation_plan.md     milestone playbook M0..M10 — YOUR TASK LIST
    13_testing_and_benchmarks.md  baselines, harness, stress/litmus tests, TSAN
    14_risks_alternatives_bibliography.md

Then execute `12_implementation_plan.md` milestone by milestone. Each
milestone names its tasks, the documents/sections that specify them, the
tests that gate completion, and what to do if a gate fails.

Auxiliary code shipped with this report (under `aux/`):

    aux/lj_atomic.h          complete atomics layer — drop into src/ as-is
    aux/arena_bitmap_model.c standalone, tested model of the arena bitmap math
    aux/nbtab_model.c        standalone, tested model of the lock-free table
    aux/bench/bench.lua      single-thread baseline benchmark harness (run!)
    aux/bench/bench_mt.lua   multi-thread scaling benchmarks (post-M4)
    aux/tests/*.lua          threading conformance + stress suite (post-M4)

The two `*_model.c` files compile and run standalone today; they are the
executable form of the trickiest algorithms. Port them, do not reinvent them.

### 0.4 Ground rules (binding)

1. **Platforms**: x86-64 Linux for this implementation pass. `LJ_GC64` is
   mandatory; the build must `#error` if built with `LJ_GC64 == 0` or on any
   non-x86-64 Linux target. 32-bit, Windows, macOS, ARM64, and console ports
   are explicitly out of scope for v1 (see 01 §ADR-1).
2. **No compatibility flag wall**: the lockless runtime is the primary build
   path. Do not hide the implementation behind `LUAJIT_THREADSAFE`/`LJ_MT`, and
   do not preserve a parallel legacy path merely for flag-off compatibility.
   Keep temporary reference code only when a milestone explicitly needs it as
   an oracle or migration aid.
3. **No locks on hot paths.** pthread mutexes/condvars may appear only in:
   thread parking (futex wait inside channel recv / join / GC worker idle),
   one-time library initialization, and the panic path. A held lock may never
   cover any allocation, table operation, string interning, barrier, or any
   code reachable from the interpreter dispatch loop. CI greps for
   `pthread_mutex` outside the whitelisted files (see 12 §M0).
4. **Every shared-memory access goes through `lj_atomic.h`.** No raw
   `volatile`, no bare loads/stores of shared fields. This both defines the
   semantics and makes ThreadSanitizer meaningful.
5. **No safepoint inside a lock-free critical sequence.** Table reads/writes,
   string interning, allocator fast paths and barrier bodies must not poll,
   allocate-with-GC, or call anything that can reach `lj_safepoint_poll`.
   This is the grace-period invariant that makes deferred reclamation sound
   (05 §5.9). Add the `LJ_NOSAFEPOINT` audit comments as specified.
6. **When a document says DECIDED, implement that option.** Alternatives are
   recorded for context and fallback only; do not relitigate them unless a
   milestone gate explicitly fails and names the fallback.
7. **Measure before and after.** `aux/bench/bench.lua` numbers from the
   reference machine are in 13 §13.2. Re-baseline on your machine at M0 and
   keep a CSV; the single-thread regression budget is ≤10% geomean
   (stretch ≤5%) on the lockless runtime with one thread running.

### 0.5 How to verify as you go

- `make -C src -j` after every task; run both `-joff` and `-jon` where the
  milestone exercises interpreter and JIT behavior separately.
- Run the stock behavior smoke: `src/luajit -e "require'ffi'; print(2^31)"`
  plus the test files listed per milestone.
- From M3 on, run the GC torture mode (`lj_gc2.c` exposes
  `collectgarbage("torture", 1)`: handshake after every N allocations) under
  the Lua test suite.
- From M4 on, run `aux/tests/` and the TSAN build target (13 §13.6).
- Keep `git commit` granularity at one spec-section per commit; commit
  messages reference the section, e.g. `gc2: grey stack push/pop (05 §5.6)`.

### 0.6 Vocabulary used throughout

- **universe** — one `global_State` and everything it owns.
- **OS thread / mutator** — a kernel thread attached to the universe; runs
  exactly one `lua_State` at a time (its *current* coroutine).
- **TG block** — the per-OS-thread block the `DISPATCH` register points to
  (dispatch tables, hot counters, safepoint word, allocator, SSB, …); see 03.
- **handshake / round** — the GC asks every mutator to run a small action at
  its next safepoint and waits for all acks; "soft" because threads ack at
  their own pace and never stop each other. See 05 §5.4.
- **grey object** — marked, queued for tracing. **black** — marked and
  traced. Mark state lives in arena bitmaps, never in object bytes (05 §5.5).
- **cell** — a heap-allocated, always-closed upvalue (06 §6.4).
- **gen** (tables) — one immutable-identity node/array vector generation of a
  table; resize installs a new gen (06 §6.2–6.3).
- **pre-lockless dump** — bytecode dump with an older v2/v3 format. The
  current lockless loader rejects these at the header (10 §10.4).

### 0.7 Deliverable inventory you will produce

New files: `lj_atomic.h`, `lj_tg.h/.c`, `lj_arena.h/.c`, `lj_gc2.h/.c`,
`lj_safepoint.h/.c`, `lj_thr.h/.c` (OS shim), `lib_threading.c`,
`lj_chan.h/.c`, `lj_exittab.h/.c`. Heavily modified: `lj_obj.h`, `lj_gc.*`
(retired/replaced by `lj_gc2.*`), `lj_alloc.*` (retired), `lj_tab.*`,
`lj_str.*`, `lj_func.*`, `lj_state.*`, `lj_dispatch.*`, `lj_parse.c`,
`lj_bcread.c`, `lj_bcwrite.c`, `lj_record.c`, `lj_asm*.{c,h}`, `lj_trace.c`,
`lj_mcode.c`, `lj_snap.c`, `lj_ctype.c`, `lj_clib.c`, `lj_ccall*.c`,
`vm_x64.dasc`, `lib_init.c`, `Makefile`. Full per-file change list: 12 §12.1.

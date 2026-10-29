
# Exact iterator source tuple primitive (2026-10-29)

The next checkpoint adds a shared source-only JMP/ITERC/ITERL decoder. It
requires the exact complete unpatched generation (or exact JITERL generation
when checking an already published source), one result/two arguments, bounded
frame slots, and the matching setup/body/call/backedge geometry. It rereads all
three defining bytecode words before returning the descriptor. It grants no
native recording, assembly, publication, or entry capability by itself.

`tests/t-arm64-jit-iterl-source.c` exercises two real iterator source forms,
nine live-word mutations, six coherent invalid-generation mutations, patched
word mismatches, and out-of-range/null inputs. The dedicated source contract
passes on ARM64 and ARM64e without test helpers, then restores the ordinary
ARM64 build. Evidence is `source-contract-final.log` under
`/Users/frityet/Scratch/ljmt-iterator-record-map-20261029/`. Frame, IR, snapshot
and entry proofs remain required by that source-only checkpoint; the later
native implementation is recorded in `arm64-native-iterl-2026-10-29.md`.

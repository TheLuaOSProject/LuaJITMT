# ARM64 fixed-call vararg leaf checkpoint (2026-10-29)

This checkpoint adds a source-derived fixed `CALL`/`VARG` proof for zero-formal
`FUNCV` leaves. It keeps dynamic/open vararg forms refused. The focused native
contract and existing leaf regression contract passed all four ARM64/arm64e
assert/release profiles; exact scope and evidence are recorded below.

## Exact x64 recorder evidence

The saved x86-64 interpreter/JIT in
`/Users/frityet/Scratch/ljmt-parity-inventory-20261028/x64/src/luajit` was run
with its matching `src` Lua modules on a two-argument example:

```lua
local leaf = function(...) local x,y=...; return x.v+y end
local function run(fn,obj,n)
  local sum=.5
  for i=1,n do sum=sum+fn(obj,.25) end
  return sum
end
```

`/Users/frityet/Scratch/vararg-pair-x64.dump` records root `CALL 9 2 3`, child
`FUNCV`, `VARG 0 3 0`, `CGET 2 0`, `TGETS 2 2 0`, `CGET 3 1`, `ADDVV 2 2 3`, `RET1 2 2`.
The dump's child-frame snapshot #2 is at IR reference 8 with topslot 20. It
contains matching leaf function references at slots 11 and 15, ordinary CALL
frame marker slot 12, FRAME_VARG marker slot 16, and supplied actuals at slots
13 and 14. The two requested VARG outputs occupy slots 17 and 18. The first
marker KNUM carries raw return PC bits `0x0000000109358f2c`; the second carries
`0x23` (`FRAME_VARG=3` plus a 32-byte delta). The frame footer makes slot 17
the current base. Decoding output is preserved in
`/Users/frityet/Scratch/vararg-markerdecode2-x64.log` and marker bitcasts in the
corresponding temporary probe (outside the repository).

## Source-derived mapping

For a fixed ordinary `CALL`, `nargs = C - 1`. In a GC64/FR2 frame, actual
argument `i` is in caller-relative slot `A + 1 + LJ_FR2 + i`. FUNCV's added
frame delta is derived from the active argument extent: `nargs + 1 + LJ_FR2`
slots, encoded as `8*(nargs + 1 + LJ_FR2) + FRAME_VARG`. This extra VARG frame
copies the function reference and adds a second frame marker; the lower
ordinary CALL marker remains present.

A fixed `VARG` requests `B - 1` outputs. The available vararg count is
`max(nargs - numparams, 0)`. Lua truncates extra values and pads missing ones
with nil. Recorder `rec_varg` independently expresses destination `i` as
`getslot(i - nvararg - 1 - LJ_FR2)` after the VARG-base shift. An admission
proof must derive these counts from source bytecode and bind every requested
output to the exact supplied argument or `REF_NIL`; it must validate all
supplied arguments even when the callee does not use them.

`CALLM`, `CALLT`, `CALLMT`, `VARGM`, `VARG B=0`, and other open/dynamic result
shapes remain outside this bounded fixed-call slice. The positive mapping must
not assume `requested == available`; tests cover unused padding and truncation.

## Runtime frame normalization

At the initial pre-setup `FUNCV` recorder dispatch, the physical frame can
still be the ordinary Lua CALL frame. After FUNCV setup, the physical top
marker is `FRAME_VARG` and `frame_islua` is false there. `frame_prevd`
subtracts the VARG-sized delta and reaches the ordinary Lua call frame.
Recorder and VM setup must be distinguished: later recorder state includes
the extra `vframe` shift in `J->base`/`J->baseslot` and `J->framedepth`.
The dispatch ownership check normalizes an installed physical VARG frame
before walking parent Lua CALLs, and separately validates its exact delta
and callee copy. Snapshot admission accepts the paired markers only after
semantic and allocated proofs bind both function slots, both frame values,
the CALL return PC, the VARG bytecode count, the footer/current base, and all
mapped vararg values.

## Ordinary companion

`tests/t-arm64-jit-lua-varargs.lua` exercises the interpreter oracle and root
loop publication for two requested/two supplied values, unused nil padding,
unused extra arguments, and a primitive result. It also checks open-VARG semantics as a boundary case. Ordinary x64 LuaJIT
may publish an open-VARG trace, so the companion records that host's trace
observation without asserting an architecture-independent refusal. The C
admission fixture must prove that ARM64 still rejects open/dynamic forms. As
with the leaf-call companion, publication is not native execution evidence;
native entry, exact guard exit, peer lifetime, GC/reuse, and separate
semantic/post-RA corruption coverage belong in the C contract.

## Implemented scope and evidence

The candidate source preflight accepts only a zero-formal vararg prototype
whose first body instruction is one fixed `VARG A=0, B>=2, C=0`; it rejects
`VARGM`, open `VARG B=0`, nested CALLs from that vararg leaf, and unsupported
source operations. Semantic and post-RA paths separately derive actual
argument values from the caller's exact ordinary CALL snapshot, seed requested
VARG outputs from those values, and seed missing requested outputs as nil.
Both passes admit the paired frame map only when the lower KNUM is the CALL's
return PC, the upper KNUM is the exact FRAME_VARG delta, both function slots
name the same leaf, and the derived upper frame base matches the source CALL.

Runtime admission distinguishes the first `FUNCV` dispatch (the ordinary Lua
frame is allowed before VARG setup) from later VARG/body dispatches requiring
the installed FRAME_VARG. It bounds the physical delta
subtraction before walking the frame, then checks the actual CALL-derived
delta, copied closure identity, JIT frame depth/base, and lower return-PC link.
The four-profile contract is
`/Users/frityet/Scratch/ljmt-varargs-recovery-20261028/final-four-profile.log`.
For each profile, its C fixture reports nine native entries across the fixed
two-request/two-supplied overflow case, unused nil padding, unused extra
arguments, and NUM arithmetic. The exact child overflow case records one
guard exit; the three cases with paired exits validate their exact
`FRAME_VARG` maps. The primitive-boolean shape remains semantic-only because
the bounded caller-result grammar does not publish it. The NUM-only shape
proves root entry/GC/reuse but may optimize away the paired child map, so it is
not counted as a paired-frame witness. Each profile also rejects ten semantic
and ten post-RA corruptions. The ordinary Lua companion checks interpreter
results and root publication for the four closed shapes plus open-result
semantics; it makes no native-execution claim.

The same four-profile matrix passed the existing leaf regression contract at
`/Users/frityet/Scratch/ljmt-varargs-recovery-20261028/leaf-regression.log`,
covering fixed, nested, sparse, primitive, multi-result, and return paths.

## Recovery provenance

This isolated candidate was reconstructed after a source cleanup accident from
the correct session log,
`/Users/frityet/.codex/sessions/2026/10/28/rollout-2026-10-28T17-41-22-01a1839a-2ebd-72e2-ab7a-bfed56ce4ff0.jsonl`,
starting at the fixed-two-result checkpoint and replaying only source changes
for the `arm64-strings` worktree before the faulty cleanup. The recovered
pre-cleanup source patch is preserved at
`/Users/frityet/Scratch/ljmt-varargs-recovery-20261028/pre-cleanup-current.patch`.
That diagnostic replay run is distinct from the final clean-source run above;
all temporary `VARARG_*` stderr diagnostics and their temporary release-only
stdio include were removed before the recorded final contract pass.

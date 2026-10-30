# ARM64 computed math-call results across loop calls

Base: `c75412a71bf863c1aa347a08242c19208a41002d`.

## Gap and cause

The matched `math.sin` loop assigns the computed result on every iteration:

```lua
local function computed(n, x)
  local i, r = 0, 0.5
  while i < n do
    i = i + 1
    r = math.sin(x + i * 0.25)
  end
  return r
end
```

Before this change, ARM64 recorded the loop but rejected it during final
post-RA admission with NYIIR (IR instruction 22). The x86_64 trace published
and entered native code. The decisive allocated pattern is a prior `CALLN`
result used as the left input to a loop `PHI`; the next `CALLN` clobbers the
original D0 allocation. The allocator emits a `RENAME` for that prior result
at the exact loop snapshot into preserved D15 before the next body call.
`arm64_postra_value_survives_call()` correctly rejects the original D0
location, but the math-specific liveness walk did not consult the loop
snapshot's renamed location.

The assertion trace had `CALLN` ref 32800, prior result ref 32784 in D0, LOOP
ref 32788, PHI ref 32804 with `{op1=32784, op2=32800}`, and RENAME ref 32805
for ref 32784 at snapshot 6 (`snap_ref=32788`) in D15. The while variant had
the same shape with refs 32783/32787/32800/32803/32806. These refs are
diagnostic evidence, not production constants.

## Change

The math post-RA liveness check now has one additional fail-closed case: for a
value before the loop and a math call after it, it requires one snapshot whose
reference is exactly the IR_LOOP, exactly one rename for that value at that
snapshot, and a resolved location in the AAPCS64 callee-preserved FP register
bank D8-D15. The rename itself must have no spill marker, and the generic
snapshot resolver must produce the same RegSP as that exact rename. The
generic call-survival predicate is unchanged. Other calls, non-loop values,
values without a matching snapshot, and malformed rename sources or
locations remain rejected.

The scalar capability fixture includes the computed-result loop and mutates
the rename source, snapshot ordinal, resolved location to D0, invalid spill
location, and PHI into an ordinary arithmetic consumer. Each mutation
independently fails the focused liveness check and full post-RA admission; the
restored control is admitted. The post-RA exception is limited to a pre-loop
numeric CALLN value used exactly once as the initial operand of a numeric PHI
whose backedge operand is a numeric CALLN body operation. Other ordinary uses
retain the existing generic call-survival rule.
The ordinary Lua companion checks the result against interpreted `math.sin`,
observes native selected-root entry and exit, forces full GC, and re-enters the
same root.

## Validation

The focused scalar capability contract passed with the fixture on all four
ARM profiles:

| Profile | XCFLAGS | Result |
| --- | --- | --- |
| arm64 assertion | `-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL -DLJ_TRACE_TEST_HELPERS -DLUA_USE_ASSERT` | pass |
| arm64 release | `-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL -DLJ_TRACE_TEST_HELPERS` | pass |
| arm64e assertion | same assertion flags, `-arch arm64e -mbranch-protection=bti` | pass |
| arm64e release | same release flags, `-arch arm64e -mbranch-protection=bti` | pass |

Logs are under `/Users/frityet/Scratch/ljmt-math-fmod-gap-20261030/`:
`math-computed-result-arm64-assert.log`, `math-computed-result-arm64-release.log`,
`math-computed-result-arm64e-assert.log`, and
`math-computed-result-arm64e-release.log`. The wrapper restored an ordinary
ARM64 archive and runtime after the matrix.

An ordinary x86_64 build from the same source also ran the matched standalone
probe: FORL and while each selected root 1 and observed 10 exits. Its profile
and output are `math-computed-result-x64-build.log` and
`math-computed-result-x64-probe.log`.

After the PHI/use/body-call tightening, the final four-profile rerun is
recorded in `computed-result-final-four-profiles.log` in the same Scratch
directory. The restored no-helper ARM64 build (profile
`b90f3b603e8d84fd524b0325abc7502ef21046296a52b6cb4881cdb3cd87d30d`, archive
SHA-256 `535bbcd7195cfee9bb23e38137d669d97b89da7eb130301388eafeab6e5faf08`,
runtime SHA-256 `8ec5676d71fe8ff3964f19357c230874c8300b1b999e82d92e5b2e682aa7bb9f`)
ran `sin-stable-num.lua` with `LUA_PATH=src/?.lua;;`. Both FORL and while
selected root 1, produced 166/168 matching selected-root exits across the two
numeric phases, retained identical machine code across post-GC reentry, and
matched their JIT-disabled peers for the tested input set. The peer was
started before compilation and remained live through both phases. The frozen
ordinary x86_64 run of the same probe is in `x86_64-sin-stable-num.log`; both
loop forms selected root 1 and produced 10 exits.

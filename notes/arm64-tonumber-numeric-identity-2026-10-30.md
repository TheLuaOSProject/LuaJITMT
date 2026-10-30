# ARM64 numeric-input `tonumber` identity

The ARM64 root recorder now admits the narrow `tonumber(x)` case where `x` is
a guarded integer or number and the call has exactly one argument and one
result. `recff_tonumber` forwards that numeric TRef unchanged. Semantic and
post-RA admission independently bind the exact source CALL, guarded function
and argument slots, CSET target, and terminal CGET/RET1 cell map. The accepted
terminal cell must still contain that same guarded numeric SLOAD. String
parsing, cdata conversion, optional-base calls, and nonnumeric inputs remain
outside this identity proof.

The shared identity proof also preserves other recorder cases that return the
same input: `math.min/max(x)` for guarded INT or NUM, and `math.floor/ceil(x)`
and one-argument `bit.tobit/band/bor/bxor(x)` only for guarded INT. It rejects
those integer-only identities for a NUM source. Other admitted builtin
constants cannot replace `tonumber` while the terminal result remains the
argument. This avoids treating a numeric identity cell map as evidence that
`abs`, `floor` on a NUM, or another transforming builtin is a no-op.

The focused fixture covers while/LOOP and FORL roots for both INT and NUM
arguments, selected-root native execution before and after full GC, NaN,
signed zero, and both infinities. Type and function changes must exit to normal
interpreter behavior. Cloned-trace checks mutate the builtin constant, cell
source, CALL arity, control-flow edge, terminal map, footer, and frame size;
semantic and post-RA checks must reject each invalid proof independently.
The Lua companion also checks optional-base parsing and the other one-argument
identity builtins.

Validation on 2026-10-30:

- `tools/ci/arm64_jit_tonumber_contract.sh`: ARM64/arm64e assert and release
  profiles passed. Durable run output:
  `/Users/frityet/Scratch/ljmt-tonumber-identity-final2-20261030/four-profile.log`.
- Ordinary ARM64 build without test helpers, using the same candidate source:
  `/Users/frityet/Scratch/ljmt-tonumber-identity-final2-20261030/ordinary-arm64/probe.log`
  reports selected roots for all four shapes and native selected-root exits
  of 12 before GC and 19–20 after GC. Runtime SHA-256 is in adjacent
  `runtime.sha256`.
- Preserved ordinary x86_64 control from `dae-x64`:
  `/Users/frityet/Scratch/ljmt-tonumber-identity-final2-20261030/ordinary-x86_64/probe.log`
  reports selected roots and 12→20 exits for all four shapes. Runtime SHA-256
  is in adjacent `runtime.sha256`.
- The ordinary Lua companion passed on both preserved runtimes; logs are
  `ordinary-arm64/companion.log` and `ordinary-x86_64/companion.log`.

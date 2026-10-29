# Scalar iterator and aggregate-stack integration (2026-10-29)

The integration branch now contains native scalar ITERL roots (`00f1d9c25`),
three/four-member HFA arguments (`e6a70009a`) and the fixed two-float HFA stack
argument (`cee48d3b5`). The integrated aggregate contract passes all four
ARM64/ARM64e assertion/release profiles. The source tuple contract also passes
after integration. Native iterator validation on its source branch includes
four helper profiles and ordinary ARM64/x64 topology comparisons; details are
in `arm64-native-iterl-2026-10-29.md`.

The first combined gate at `00f1d9c25` found a real no-JIT build regression:
the JITERL host generator referenced `GG_G2J_HI/LO` in an unguarded C branch.
The opcode case's runtime early break does not remove that code from host
compilation. Commit `aa8ec9e2b` wraps the native entry emission in the same
DynASM JIT condition used by JFORL. The gate was restarted from its beginning
at that corrected production revision and has passed the no-JIT build stage.
Its final outcome is still pending at this notes checkpoint.

The corrected run passed through the complete callback/profile contract and
the scalar FUNCF contract. It then stopped before native-FORL execution on a
source-text assertion that still expected the old three-way admission-marker
selector. Test-only commit `69155ed62` updates that assertion to include the
ITERL marker while preserving the FORL checks. A saved continuation begins
at the failed native-FORL contract; its production source is unchanged from
`aa8ec9e2b`. This is resumed coverage, not an uninterrupted gate pass. The
current passing callback run does not explain the older profiling timeout.

The first continuation passed the native FORL, source/native ITERL and exit
contracts. It then reached an obsolete root-entry fixture expectation that
JITERL never attempts a lease. The runtime now correctly attempts and cleans
that lease when the trace table is absent. Test-only commit `a42ba7b37`
requires matching nonzero publication/cleanup, zero native successes and
consumed-source recovery. A second source check still expected three entry
call relocations; `2739c7334` updates it to four and adds JITERL instruction
ordering checks. `d3d02f2e6` makes ordering failures explicit in the shell
and recognizes the plain ARM64 branch spelling as well as ARM64e.

The final continuation from root-entry has completed successfully through all
combined FFI profiles and the umbrella OK marker. The production revision
remained `aa8ec9e2b`; only tests/notes changed after that no-JIT correction.
The original prefix and two successful continuation segments cover the gate,
including all four native iterator profiles. This remains resumed coverage.
The strengthened shell-order checks added after the root-entry child script
had started are scheduled for a fresh focused run with the next integration.

Evidence:

- `/Users/frityet/Scratch/ljmt-hfa-multilane-integrated-20261029/arguments-integrated.log`
  and `iterator-source-integrated.log`: integrated multilane/source contracts.
- `/Users/frityet/Scratch/ljmt-hfa-stack-20261029/integrated-four-profile.log`:
  integrated stack-HFA contract.
- `/Users/frityet/Scratch/ljmt-iterl-stack-integrated-20261029/`: original
  no-JIT failure log/source ID and the restarted gate log/source ID.

This is an incremental checkpoint. Nil-terminated iterators, general tail
calls and nested side graphs still have work in progress. Passing semantic
comparisons alone does not establish equivalent native compilation coverage.

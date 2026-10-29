# ARM64 one-member HFA results

The Darwin ARM64 CALLXS path now supports direct aggregate results containing
exactly one `float` or one `double`. The classifier records the result as an
FPR `FLOAT_HFA_MEMBER` with count one; the recorder stores the returned raw
S0/D0 carrier into a fresh CData box whose CType ID is the declared aggregate
type. The Lua result is that CData box. A two-member HFA result remains closed:
AAPCS64 returns its members separately in S0/S1, while the current IR_CALLXS
result and assembler expose only one FPR result.

Both semantic and post-RA admission independently check the exact callable
signature, result IR type, declared CData box, return snapshot slot, and the
CALLXS-to-XSTORE flow. The raw floating carrier must not appear in any
snapshot. The ordinary scalar-result snapshot rule is skipped for this boxed
aggregate result in both walks. The HFA classifier also rejects CData
references and declared CTA_ALIGN wrappers with alignment above the member's
natural alignment.

The existing integer-aggregate CALLXS contract now includes `float` and
`double` HFA results. Each case compares candidate and JIT-disabled oracle
values and exact returned CData type, witnesses native CALLXS/CNEW/XSTORE on
the selected candidate root, forces a mismatched-result exit, and verifies
native re-entry and effect counts after full GC. Private IR clones independently
reject a raw result snapshot, wrong XSTORE type, wrong CData CType, and wrong
XSTORE source in semantic and post-RA admission. An over-aligned HFA result
signature is required to remain closed.

Validation command:

```sh
LJ_ARM64_INTEGER_AGGREGATE_PROFILES='arm64-assert arm64e-assert arm64-release arm64e-release' \
  tools/ci/arm64_jit_integer_aggregate_args_contract.sh
```

All four profiles passed on native macOS ARM64. The captured log is
`/Users/frityet/Scratch/ljmt-hfa-results-20261029/four-profile.log`. The
contract's three expected fail-closed trace aborts per profile are existing
negative probes; the script completed successfully and restored the ordinary
ARM64 assertion build. No lock or shared mutable register state was added.

## Ordinary integrated builds

Root integration `0af2319d6`, followed by the expanded portable probe at
`63156cc14`, passes ordinary ARM64 and x64 assertion builds. The ten aggregate
rows perform 250 interpreter comparisons and 27,520 foreign calls per target.
The new single-float and single-double argument/result rows each publish a
loop with two CALLXS, two CNEW and two XSTORE instructions and produce 25
selected-root exits on both targets. Each row retains all 50 returned objects
through a full collection and checks their declared CType and fields against
independent fixture arithmetic. The retained two-float result remains x64-only.

Source ID, build/run logs, immutable runtimes, archives, modules and fixture
libraries, and SHA-256 hashes are saved in
`/Users/frityet/Scratch/ljmt-hfa-results-integration-20261029/`.
The complete gate at `b549a895b` predates this follow-up; this check does not
claim a complete gate of the HFA-result extension.

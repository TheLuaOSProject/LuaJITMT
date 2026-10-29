# Lua integer literals and FFI accumulator integration

Source `b5199b149` combines the preceding Lua forwarding, binary math and
variadic indirect-result checkpoint with the integer-literal CALLM replay
repair (`fb5157fb9`) and guarded loop-carried CALLXS integer ADDOV admission
(`b04fe5e22`, narrowed at `493767141`). The PHI exception applies only to
ADDOV; SUBOV and MULOV retain their previous flag requirements. No locks are
added and no FFI argument/result classification changes in these follow-ups.

The complete table-call contract passes all four ARM64/arm64e
assertion/release profiles on this combined source. It includes scalar and
GC-valued table calls, held results, numeric and integer arguments, cdata
arguments, aggregates, function pointers, variadics, and independent
semantic/post-RA admission and lifecycle checks. The new integer overflow
case verifies the selected exit's source continuation and exact callback
count, so the completed FFI call is not replayed.

After ordinary assertion ARM64 restoration without test helpers, both the
509-case source corpus and 509-case original v2-bytecode corpus pass. The
outer command exits zero. Evidence is under
`/Users/frityet/Scratch/ljmt-deep-callm-integration-20261029`:

- `literal-intacc-table-four-profile.log`
- `literal-intacc-stock-source.log`
- `literal-intacc-stock-bytecode.log`

Final ordinary profile:
`499b8b9802a0bfe0718df122012b7708555f0673df85d6c53140f98ba5dd4c40`.
The separate complete gate is pinned to preceding `6c46f9bff`; it does not
cover these later changes. General x86_64 parity remains incomplete.

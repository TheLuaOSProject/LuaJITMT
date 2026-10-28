# ARM64 Lua leaf early-return branches — 2026-10-28

The bounded leaf recognizer and both independent admission passes now accept an acyclic single-comparison numeric branch whose two arms each end in their own `RET1`. This fills the inventory shape `if x > 16 then return x + .25 else return x - .25 end` while retaining the existing common-join branch grammar.

The source proof decodes forward successors, checks disjoint arm reachability, requires exactly one defined numeric `RET1` on each arm, and leaves the compiler-emitted trailing `RET0` sentinel unreachable. It does not match a fixed instruction count, local slot, or arithmetic opcode. Semantic and post-RA validators independently select the source return for each path and bind a resumed arithmetic child's return to the caller continuation. At a cold comparison snapshot no return has happened yet, so the snapshot is validated as pre-branch state and does not claim a returned value.

The native return fixture forces the cold comparison edge, checks interpreter/native results, and verifies same-trace plus GC reuse. Its negative changes the selected arm's `RET1` source to a different defined numeric local and requires both semantic and allocated-view admission to reject. The ordinary Lua companion checks publication only.

Validation used the frozen source after the final code review:

- `tools/ci/arm64_jit_lua_leaf_contract.sh` passed all four ARM64 / ARM64e assert and release profiles (exit 0).
- Each native profile reported `cases=97 native=402 terminal=96 reuse=306 gc=97 controlled-old-tags=96`; one additional early-return case was checked through its branch-edge exit and reuse path.
- Each ordinary Lua return fixture reported `comparison-early-return published=1 cold_calls=1 numeric_ranges=1 same_trace_reuses=4 result_match=yes`.
- Full log: `/Users/frityet/Scratch/ljmt-arm64-branch-early-return-20261028/leaf-contract-frozen.log`.

This remains a bounded grammar: multiple comparisons, cycles/backedges in the leaf body, branch arms with a shared join after a path-specific return, and more than one return per arm are not admitted by this change. The contract's ordinary-Lua result is publication evidence; native-execution evidence comes from the C fixture.

# M1 x64 VM TG Baseline

The previous raw disassembly snapshot from the early TG-addressing migration was
useful for one-time orientation, but it was not a durable invariant and should
not live in the repository as a source or generated-code contract.

The lasting M1 requirement is documented in `plan/12_implementation_plan.md`:
the VM keeps x64 behavior compatible while TG-owned state is reached through the
TG helper layer. Current coverage belongs in stock semantics tests, focused x64
behavior fixtures, and comments beside the TG access helpers. Local objdump
output is still useful during review, but it is diagnostic only and must not be
committed as a pass/fail fixture.

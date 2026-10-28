# Bounded nested parameter calls in the permanent leaf gate

The port branch through `a21eca57a` combines the negative dynamic byte-load
support with the reviewed parameter-passed nested Lua call implementation.
The leaf contract now builds and runs `t-arm64-jit-lua-nested-calls.c` in each
of its four profiles, so this proof also runs in the aggregate experimental
gate rather than depending on an external validation script.

The complete leaf contract passed ARM64 and arm64e assertion and release
profiles on this combined source. The nested fixture proves native entry,
exact inner and outer function identity exits, reuse after collection while
a peer TG is live, and independent semantic/post-allocation malformed-source
rejection. Existing leaf, child, compound, argument, sparse, normalized-argument,
and return fixtures also passed.

Evidence:
`/Users/frityet/Scratch/ljmt-negative-nested-20261028/nested-parameter-four-profile.log`.
This checkpoint covers ordinary fixed calls with parameter-passed functions
and one live child snapshot frame. Captured callees, fixed multiple results,
variable arguments, and general nested snapshot chains are separate work.

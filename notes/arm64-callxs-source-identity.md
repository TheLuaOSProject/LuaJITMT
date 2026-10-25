# ARM64 CALLXS source identity

The ARM64 CALLXS admission proof binds its native call and post-call snapshot
to the originating Lua `BC_CALL` continuation. Exit snapshots alone are not
source authority: a copied trace can change both a footer and its saved
`ffi.meta.__call` frame PC to another valid call instruction. Each CALLXS
`IR_XSAVE` therefore carries the bytecode continuation index captured from
the live trace-root proto and VM frame when the recorder emits the marker.

The two 15-bit literal operands store the low and high halves of that index.
This covers every legal prototype offset (`LJ_MAX_BCINS` is `1<<26`) while
keeping each literal below `REF_BIAS`; the loop optimizer can therefore keep
its existing literal/ref substitution behavior unchanged. `0/0` remains the
ordinary unanchored XSAVE form used outside this path; CALLXS admission rejects
an absent anchor. Literal operands do not refer to
IR or constant-pool entries, so loop copying, DCE, and constant relocation do
not rewrite them. Folding emits XSAVE directly before constructing its operand
key, because a low literal half can overlap a constant-reference number and
every ordered materialization boundary must survive. Existing non-CALLXS
recorders continue emitting `0/0`.

The recorder resolves the exact root proto from the in-progress trace, checks
the saved return PC's numeric range and instruction alignment before reading
the preceding instruction. It captures CALL, CALLM and ITERC frame offsets;
the current native admission contract still requires `BC_CALL`. It bounds `BC_A` to the
proto frame and the derived caller frame to the live Lua stack before checking
that the caller function owns the same proto. The admission walkers decode
the literal independently and require its preceding bytecode instruction to
be a supported CALL. Post-call caller-frame footers and their paired FFI
meta-frame snapshots must agree with this immutable source anchor, preventing
a completed foreign effect from resuming at another call site.

Allocation ownership is limited to each snapshot's IR interval. For a caller
footer, replay may cross only side-effect-free argument setup before the
anchored CALL. A meta-frame allocation snapshot instead carries the caller's
return PC, which must equal the same source anchor even though it precedes
XSAVE. Binding only the post-call pair leaves this allocation snapshot unchecked.

The pointer fixture now gives that allocation snapshot its own KNUM frame-PC
constant. It first accepts the detached constant with the original PC, then
changes only its payload to a second valid CALL continuation in an unexecuted
branch of the same prototype. The old semantic validator accepted that
corruption (`/tmp/ljmt-prealloc-anchor-negative-control.log`); the fixed semantic
and post-RA walkers both reject it, with pristine positive controls
(`/tmp/ljmt-prealloc-anchor-fixed.log`). No published bytecode is modified for
this regression. Tail RENAME negatives likewise start from independent clones
and cover self/forward/metadata/constant-payload sources and destination aliases.

Early private M7 validation claims were withdrawn after discovering that their
shells ran in the main checkout. Explicit-directory reruns found and fixed the
canonical NOP suffix handling and the pointer fixture's patched-JFORL lookup.
The complete ARM64 M7 suite passed at private checkpoint `20e326cd`, with
matching fixture/archive flags and explicit source directory and SHA in
`/tmp/ljmt-anchor-integrated-m7-20e326cd.log`. This includes ABI classes,
1,195 observer-confirmed generated calls through the C++ unwind fixture,
remote GC/flush, and scalar/double/pointer lifecycle tests. arm64e and release
validation remain separate gates.
The long-callsite case requires at least 200 additional observer-confirmed
native calls; a 300-call aggregate alone includes interpreted calls.

That stronger witness exposed a separate allocated-register mask error:
`RSET_GPR` already excludes reserved registers, so subtracting `RSET_FIXED`
again wrongly excluded X19. The checker now uses `RSET_GPR` directly, with
reserved X18/dispatch negatives. The unchanged large-function fixture now
executes natively and exercises nonzero high XSAVE literal halves.

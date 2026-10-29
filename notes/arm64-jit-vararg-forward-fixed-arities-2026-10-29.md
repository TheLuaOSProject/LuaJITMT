# Vararg tail forwarders with fixed target arities

The bounded zero-formal `...` forwarder now admits immutable numeric leaf
targets with one, two or three fixed parameters. The forwarder must retain the
exact `UGET; VARG; CALLMT` source shape. The caller's ordinary fixed `CALL`
must supply exactly the target's formal count, and both semantic and post-RA
proofs bind every argument slot in the caller map to the same contiguous
window copied by `VARG`. The target body is independently restricted to the
existing numeric leaf grammar and a single scalar return. Open calls,
vararg targets, nonnumeric arguments and other arities remain refused.

Runtime frame preflight also checks the exact caller argument count against
the immutable upvalue target. After `CALLMT` replaces the forwarding frame,
the physical caller argument window is compared element by element with the
replacement target frame before the trace can be admitted. The source and
allocated validators independently repeat the count, target, map-window and
numeric-body checks.

The focused four-profile contract passed on arm64 and arm64e, with assertions
and release builds. Each profile published the three supported arities and
made three native calls per arity, including an equivalent-target identity
change that exited the selected root, full-GC reuse, and return to the original
target. The fixture also ran the source/window/target mutations through both
semantic and post-RA admission: each profile reported 52 semantic and 52
post-RA negatives. Existing fixed-vararg pad/truncate and child-overflow cases
continued to pass (18 entries and one exact child overflow exit per profile).

The full log is
`/Users/frityet/Scratch/ljmt-vararg-forward-arity-20261029/four-profile-final.log`.
An earlier run stopped before testing because generated Lua indexed a table
literal without parentheses; the fixture was corrected and the complete
four-profile run above is the final evidence. This change covers only fixed
numeric target arities one through three for the existing bounded forwarder
shape; it does not admit open or variable target arities.

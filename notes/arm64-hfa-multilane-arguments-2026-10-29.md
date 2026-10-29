# ARM64 three/four-member HFA arguments — 2026-10-29

The Darwin ARM64 fixed-argument classifier now accepts direct homogeneous
aggregates with three or four `float` or `double` members. The recorder lowers
each member to its own typed XLOAD and FPR carrier. The ABI signature keeps
the Lua source parameter index and member index on every physical carrier,
so the three- or four-lane expansion remains bound to one rooted CData slot.
The following scalar FPR argument is checked after the aggregate lanes.

The semantic and post-RA proofs independently validate every carrier's
source CData read, guarded CType identity, field offset/type, lane index,
snapshot-root provenance, and CARG/CALLXS use. The tests mutate the last member
of the three- and four-member carriers in both views and require rejection.
They also exercise allocation/type and snapshot mutations through the existing
private trace-clone fixture.

Boundary behavior stays conservative: five-member, over-aligned, and
by-value layouts that exhaust the remaining eight FPR argument registers are
not admitted. HFA values in variadic signatures remain closed. Reference
aliases use the pointer ABI and do not enter the HFA-by-value classifier.
Stack-passed HFAs are not implemented by this slice.

## Validation

- The focused contract passed all four profiles:
  `arm64-assert`, `arm64e-assert`, `arm64-release`, and `arm64e-release`.
  Log: `/Users/frityet/Scratch/ljmt-hfa-arg-multilane-20261029/final-four/four-profile.log`.
- The test fixture requires native selected-root entry, forced exit, exact
  foreign-call counts, full-GC re-entry, and preserved effects for a
  three-float argument and a four-double argument followed by a scalar double.
  ABI signature assertions check FPR indices 0–2 and 0–3 respectively, then
  the scalar argument's next FPR.
- A strict Darwin x86_64 GC64 build with `CFLAGS=-Werror` passed after the
  ARM64 changes. Build log: `/Users/frityet/Scratch/ljmt-hfa-arg-multilane-20261029/x64-strict.log`.
- Five-member, over-aligned, and FPR-overflow signatures fail classification.
  A triple-float reference signature is checked as a pointer carrier rather
  than four or three HFA lanes.

The implementation uses the Darwin AAPCS64 homogeneous aggregate rule, whose
maximum HFA member count is four. It does not add lock-taking runtime state or
raw aggregate values to Lua snapshots.

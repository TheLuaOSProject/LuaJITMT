# ARM64 scalar CData variadic CALLXS suffix

The ARM64 CALLXS path now admits a bounded scalar CData suffix for a variadic
C function. It keeps the declared fixed-parameter plan unchanged and places
unnamed arguments after the exact CARG nil marker. Supported payloads are
numeric CData with the existing numeric CType/width checks: integer and
floating types use C default argument promotions, then the promoted value is
placed in the Darwin variadic stack area. Pointer CData, aggregates, booleans,
enums, and unsupported widths remain closed.

Both semantic IR admission and post-RA admission independently bind each
suffix carrier to the exact XSAVE source slot and guarded CData owner. They
validate the payload CType, width/sign, required conversion, tail position,
stack placement, and that raw payload/native carrier refs do not appear in
Lua snapshots. Candidate payloads do not gain global admission merely because
a valid vararg exists elsewhere in the trace: fixed-argument uses still need
their pre-existing fixed CData proof, and a value already classified as a
vararg-only candidate is rejected if reused in a fixed parameter position.

A regression found during validation was in the post-RA walk: it registered a
numeric FLOAD as a vararg candidate even when the existing fixed integer
argument proof had already accepted it. The candidate then collided with its
own fixed CALLXS argument and prevented the CData32 baseline trace from being
published. Candidate registration now occurs only when the fixed proof fails
and the exact variadic payload proof succeeds; the CData32 signature-width and
signedness mutation matrix passes again.

Validation on the clean source:

- `LJ_ARM64_TABLE_CALL_CASES='ffi-varargs ffi-cdata32-args-admission' tools/ci/arm64_jit_table_call_contract.sh`
- Four profiles passed: ARM64 and ARM64e, each with assertion and release
  builds. The variadic fixture proves native calls, exact effects, GC/reuse,
  and independent semantic/post-RA malformed-source/map cases. The CData32
  fixture reports semantic `8/92` and post-RA `8/108` admission outcomes.
- The broader four-profile M7 CALLXS suite was launched separately after this
  focused regression passed; its result is pending at this checkpoint.

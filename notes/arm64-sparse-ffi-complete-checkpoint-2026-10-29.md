# Sparse side frames and numeric variadic FFI checkpoint

The complete fail-closed gate passes at source `0dc7716c8`. This combines
the previously verified side-chain capacity work with sparse parent-frame
admission through tested depth 260 and numeric-cdata variadic FFI suffixes.
The gate exits zero, has no skipped checks, and runs the actual final VM and
recorder safepoint fixtures. All four ARM64/arm64e assertion/release M7 FFI
profiles pass, including remote flush, result ownership and call lifetime
checks.

Evidence is in
`/Users/frityet/Scratch/ljmt-deep-callm-integration-20261029`:

- `sparse-ffi-complete-gate.log`: four M7 pass markers, final VM and recorder
  runtime safepoint pass markers, and the final complete-gate pass marker.
- `sparse-ffi-complete-restoration.log`: ordinary assertion ARM64 restoration
  without test helpers, profile
  `5437d1bffbb783889087ecf9a814c9e38505695c769cd38329adb08af6dbe46a`.

The outer gate-and-restoration command exits zero. This evidence is pinned
to the named source. It does not include later fixed FUNCF open CALLM,
CALLMT forwarding, binary math or variadic indirect-result changes. General
Lua/JIT behavior parity remains incomplete.

After promotion, the main parity checkout at `d87d5281f` was independently
rebuilt as ordinary assertion ARM64 without helpers. Its production source
is unchanged from the verified checkpoint. `sparse-ffi-main-build.log`
records profile
`334e1a9f4c09497819332f6369ebdefd322fcd8d22ca2a568d281bc23a66da6f`.

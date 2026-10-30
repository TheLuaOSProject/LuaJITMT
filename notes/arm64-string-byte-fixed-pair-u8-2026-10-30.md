# ARM64 `string.byte` fixed-pair U8 snapshots

The admission extension covers the exact fixed call `string.byte(s, 1, 2)` with
two requested results (`BC_CALL.B == 3`, `BC_CALL.C == 4`). Each raw U8 lane is
bound to its own `STRREF` offset (0 or 1), the string formal, and its matching
result slot. Semantic and post-RA checks independently replay source geometry,
result definitions, and snapshot mappings.

Source validation rejects explicit jumps and implicit `BC_IS*` skip edges that
enter argument setup after its first instruction. Result validation requires
the CALL and any result copy to dominate the saved source PC. The fixture
corrupts argument setup, a skip into setup, a predecessor jump over CALL, lane
offsets, snapshot slots, and the width guard, then checks semantic and allocated
rejection separately. Immediate KINT offsets are handled without indexing the
IR array as if an immediate were a reference.

The completed full contract on the integrated `ea0db2046` source passed all
four ARM64/arm64e assertion/release profiles, including the native pair witness,
live peer, cold exits, full-GC reuse, and independent mutations. The full
multi-byte matrix also passed. Log:
`/Users/frityet/Scratch/ljmt-u8-fixed-forl-20261030/contract-integrated-full.log`.

An earlier attempt on sibling base `7c562842a` lacked the semantic and
post-RA mixed-range dispatches present in the integrated tree. Its mode-11
positive-start/negative-end row rejected `IR_STRREF`; a paired baseline
reproduced that stale-branch refusal. The integrated base contains those
dispatches and the same full contract now passes, so that earlier result is not
a limitation of this slice.

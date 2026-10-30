# ARM64 `string.sub` zero start

The ARM64 source proof now accepts the literal first index `0` when the
certified `STRREF` byte offset is zero. Lua clamps both `0` and `1` to the
first byte; the proof keeps the same exact `string.sub` CALL, formal string,
argument window, extent guards, snapshots, allocation, and post-RA checks.
Other first-index literals still have to match the certified offset.

The focused fixture covers LOOP and FORL with positive, negative-last, and
omitted-last forms. It confirms that changing the source literal from `0` to
`1` remains admissible, while changing it to `-1` is rejected independently
by semantic and post-RA admission. Existing positive/negative extent,
callee, snapshot, layout, and physical-location mutations remain enabled.
The positive `zero-while` case observed fresh native string publication; the
native fixture also checks selected-root exits across GC and peer use.

Validation on the `4c9cc207e` base plus this slice:

- `/Users/frityet/Scratch/ljmt-substring-zero-first-20261030/final-four-profile.log` — all four ARM64/arm64e assertion and release profiles passed.
- `/Users/frityet/Scratch/ljmt-substring-zero-first-20261030/ordinary-arm64.log` — ordinary ARM64 binary selected native roots and exits for zero-start LOOP/FORL, negative-last, and omitted-last cases.
- `/Users/frityet/Scratch/ljmt-substring-zero-first-20261030/ordinary-x64.log` — matched ordinary x86_64 control selected native roots and exits for the same cases.

The ordinary ARM64 runtime was restored by the profile wrapper from the
candidate worktree (binary SHA-256 `973f121aa98fda41c7b52515ecec9b1257eebe0fc6d3b0601211b755c634bed3`).
The x86_64 control is the preserved ordinary runtime at
`/Users/frityet/Scratch/ljmt-default-substring-ordinary-20261030/x86_64-ordinary`
(binary SHA-256 `6f8eebe5e732899fdc1e2a5685e797452ba85a096aef98ecb63e34f9b21d66a1`).

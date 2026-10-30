# Fixed `string.byte` result-pair source lineage

This follow-up binds a fixed two-result `string.byte(s, 1, 2)` CALL to the
specific guarded builtin and to the function value in its CALL frame. The
semantic and allocated validators independently check the CALL-4 callee copy,
the guarded SLOAD source slot, source stability from the FORL body entry, the
guard snapshot's exact function-slot mapping and flags, and control-flow entry
through the callee and argument setup. A copied-source mismatch, a JMP that
skips the callee copy, a changed SLOAD source slot, altered guard-map flags, or
a nonzero guard-footer tag is rejected. The candidate guard count also fails
closed when a trace has an additional `FF_string_byte` identity guard.

The real bytecode uses `CGET R10,R4` for the callee copy. The guarded function
SLOAD is rooted at absolute slot 6 and the CALL function slot is 12; requiring
the SLOAD's slot to equal the CALL slot would reject this valid copy. The proof
instead ties `CGET`'s source register to `SLOAD.op1 - (1 + LJ_FR2)`. It derives
the executed body start from the root FORL backedge, since the bytecode body
precedes the FORL instruction in this prototype.

## Validation

The focused string-byte contract passed ARM64 and arm64e assertion and release
profiles, including native entry/exit, full GC, peer activity, and the new
semantic/post-RA negatives. It restored the standard ARM64 assertion profile
at the end. Log:
`/Users/frityet/Scratch/ljmt-u8-ordinary-20261030/four-profile-final-source.log`.

Ordinary ARM64 build and strict x86_64 assertion/`-Werror` builds both passed
from HEAD `8272ca50dac1c8f29b7b1b359d4da176d0bdec87` plus source diff SHA-256
`ac0aaf89da043263db1db73a7ec62954ecf1b50a9884efd8240624126de38107`. Build
logs:

- `/Users/frityet/Scratch/ljmt-u8-ordinary-20261030/ordinary-arm64-build.log`
- `/Users/frityet/Scratch/ljmt-u8-ordinary-20261030/strict-x86_64-build.log`

Matching runtime and `jit` module bundles are preserved under
`/Users/frityet/Scratch/ljmt-u8-ordinary-20261030/ordinary-arm64/` and
`/Users/frityet/Scratch/ljmt-u8-ordinary-20261030/ordinary-x86_64/`.
The ARM64 binary SHA-256 is
`1c1a7bb430e0faf6aef9e7bde33045fa547992f1fb554d9ff55e95bbf3c5a544`; the
x86_64 binary SHA-256 is
`b0496e1c807f3631ed93a0a1326e7ac2565f3f59ef502ceab3581d5bfeda9756`.

The same ordinary probe passed on both architectures with a live peer created
before candidate compilation. It observed selected loop root 1, two raw byte
loads, 80 selected-root exits before peer GC and 5,137 total after peer GC and
reentry, matching numeric results against a JIT-disabled oracle, stable
machine-code identity, and a still-published loop trace. Creating the peer
after compiling the candidate had flushed the trace before any peer collection;
that probe-order artifact was corrected by starting the peer first. Final
outputs are `probe-final.log` in each matching bundle.

Ordinary runtime identifiers:

```sh
LUA_PATH="$bundle/?.lua;$bundle/jit/?.lua;;" "$bundle/luajit" "$bundle/probe.lua"
```


# AArch64 authenticated exit-word representation checks (2026-08-28)

## Scope

This checkpoint hardens the exact first-side admission, publication, native
entry and retirement certificates before they are exposed to an ordinary
arm64e build. Authenticated exit-table entries are stored data words. Their
complete signed representation, including the discriminator-dependent PAC,
is the authority for generation identity.

This does not open the production side recorder. It preserves the existing
one-shot first-side grammar and changes only how already-required raw target
identity is compared and tested.

## Representation rule

Ordinary C pointer equality is suitable for object identity such as
`GCtrace *`, `MCode *`, snapshot pointers and exit-slot addresses. It is not
used for an encoded exit-table word on arm64e: a compiler is allowed to treat
pointer equality semantically, while the lockless protocol must distinguish
two words which strip to the same address but were signed with different
discriminators.

The first-side code now copies each acquired or freshly encoded `void *` into
`uintptr_t` with `memcpy` and compares those integer representations. Views and
transaction plans cache the representation bits alongside the pointer value,
so double-capture and post-token revalidation prove the exact word they first
observed. This covers:

- the root fallback at hot-side metadata admission;
- every private child exit slot before publication;
- the parent fallback and desired child target in the publication plan;
- the published parent-to-child edge and all child fallbacks at native entry;
- live, detached and retired parent-edge states during transactional
  retirement; and
- the arm64e wrong-discriminator negative and raw compare/exchange fixtures.

The atomic raw compare/exchange remains pointer-sized and receives the
original signed pointer values. The compiler atomic compares the stored
representation; only surrounding C `==`/`!=` checks were replaced. No native
entry, exit-stub or generated-code fast path gains work.

The existing `mcauth` checks for the exact parent and child already use the
same `memcpy`-to-`uintptr_t` rule. The focused first-side fixture now does so as
well.

## Fail-closed behavior

The PAUTH negative creates a target with the correct stripped fallback address
but a deliberately wrong discriminator. Its representation must differ from
the saved slot word, semantic stripping must still produce the same raw
address, and the publication seal must reject it before `PUBLISH`. The exact
word is restored before the VM executes again.

The native-entry and retirement validators similarly reject any same-address,
wrong-signature edge as a lost generation rather than accepting it as a live,
detached or idempotent state.

## Validation

The following focused and adjacent contracts passed on this Apple Silicon
host for arm64 and arm64e, with BTI/PAUTH enabled in the arm64e slices:

- `tools/ci/arm64_jit_side_ingress_metadata_contract.sh`;
- `tools/ci/arm64_jit_exit_contract.sh`;
- `tools/ci/arm64_jit_side_asm_consumption_contract.sh`;
- `tools/ci/arm64_jit_first_side_publish_contract.sh`; and
- `tools/ci/arm64_jit_root_entry_contract.sh`.

The first-side runtime modes covered GC claim, scoped flush and full flush,
twice per architecture. The exit-table negative covered raw, null-context,
trace-context and wrong-global signatures. The only build diagnostic was the
pre-existing unused `ccall_rawchild_wait` warning.

A macOS x86_64 cross-build with `-Wall -Wextra` also completed. A diagnostic-
free `-Werror` cross-build remains unavailable because that existing target
reports unrelated unused parameters and one unused-but-set local in the string,
table, metatable and API sources; none are in this checkpoint's files.

## Next step

With raw authenticated-word identity explicit, the next checkpoint can admit
the existing exact exit-2 first child through ordinary `trace_hotside` and the
finite publication transaction at dynamic trace numbers. Unsupported first
side shapes, side-of-side recording, TRACE callbacks and GDBJIT/PERFTOOLS must
remain fail-closed until their own complete transactions and contracts exist.

The four-file delta in `candidate-v1.patch` implements the reviewed cdata-only comparison contract. It adds a separate helper, its internal declaration/FFI call registration, and a runtime-only branch in `recff_clib_index`. The common general hit helper and the recording-time admission/snapshot/anchor path are unchanged. Sources are frozen in `ir-review-source/src` for this code review; all 224 input hashes and the normal build command are recorded.

The new helper keeps physical owner admission and confirmation, an exact non-NULL native-base interval, stack/root provenance handling, one-shot SMR before source loads, both source leases, common key validation and the common paired-generation resolver. It examines the acquired result word only as a cdata tag and GC object pointer. Both input cells stay unchanged. It returns an integer and releases SMR and both source leases. Its source/code paths do not discover or publish a result object. The native-base check alone does not prove expected-object retention; the recorder emits an actual typed KGC C argument for that purpose, including extern stores.

Eight final native shape processes compare the frozen aee88db5 runtime and candidate for function lookup, extern read, extern write and a numeric constant. Programmatic trace inspection verifies the comparison's final C argument is a constant of Lua type cdata, exactly `rawequal` to the sampled expected object. All four cases require a self-linked root and actual native exit. Raw IR, snapshot maps and machine code are preserved in `ir-v2-*` files. They show one helper in the preheader and one in the loop body, an immediate post-call snapshot/status guard, then the signed volatile close guard before extern loads/stores. Cdata paths have no result VLOAD; the numeric path keeps old hit_try and its actual VLOAD.

| Case | Baseline IR / bytes | Candidate IR / bytes |
| --- | --- | --- |
| Function lookup | 39 / 515 | 35 / 438 |
| Extern read | 41 / 546 | 37 / 469 |
| Extern store | 41 / 530 | 37 / 458 |
| Numeric constant | 37 / 473 | 37 / 473 |

The new compiled helper body is 1,564 bytes; the old general implementation remains 2,309 bytes. These are code sizes, not timing evidence. Initial V1 had eight passing shape witnesses but its trace callback displaced jit.dump's callback; those outputs and exact fixture bytes are retained. V2 removes that callback and uses the unique post-flush loop plus native-exit/KGC witness, producing complete raw dumps. No runtime source/build changed between the probes.

This submission is the code/IR review stage only. Normal/assert/ASan authority, lifetime, close and refusal validation and matched performance measurements have not yet been run. No shared source or build was changed, and no attachment-during-recording experiment was attempted.

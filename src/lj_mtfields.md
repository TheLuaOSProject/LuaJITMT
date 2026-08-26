# LuaJIT-MT Shared Field Classification

Seeded from `plan/02_memory_model.md` §2.5 during M1. Extend this table
whenever a shared field is introduced or migrated.

| field | class | ordering |
|---|---|---|
| TValue in table array/hash, upvalue cell, registry, gcroot | shared slot | rlx (store: rlx; structural publish: rel) |
| GCtab.array / node | RCU pointer | load acq / store rel; readers pair with header snapshots |
| GCtab.arrayhdr / nodehdr (new MRefs, 06) | side-vector header | load acq / store rel; flags CAS acq_rel |
| GCtab.asize / hmask / acap | size/capacity mirrors | load acq / store rel; snapshot against array/node headers where needed |
| GCtab.freetop | hash free-cursor hint | load acq / store rel on GC64; correctness revalidates nodes or scans |
| GCtab.colo | colocated-array/advisory byte | load acq / store rel |
| GCtab.struct_owner | structural mutation claim | CAS acq_rel; owner reads acq; private bootstrap store rlx |
| GCtab.metatable, GCudata.metatable | shared ptr | rlx; publish rel |
| GCtab.nomm | advisory byte | load acq / store rel; construction masks and explicit clears only |
| GCtab.gc2_rescan_state | exact table-rescan aggregate membership | load acq; private construction store rlx; state transitions CAS acq_rel; terminal publication store rel only with exclusive ownership |
| GCobj.gch.gct | immutable after publish | plain read; written pre-publish |
| arena bitmaps (block/mark) | atomic bitset | fetch_or rlx; sweep owns exclusive |
| g->gc2.phase | phase word | load rlx in fast paths; transitions rel + handshake |
| g->gc2.activation | veto-only typed phase mirror | stable acq snapshot; exact CX16 transitions; mismatch vetoes reclaim; no positive authority yet |
| g->gc2.cycle_leader | cycle request / phase-edge gate | exact CAS ownership; request retained through MARK init; GCSCAN excludes worker/phase edges |
| g->gc2.tg_registry_head / LJTGRegistrySlot.next_all | stable shadow TG spine | head CAS acq_rel; next initialized before successful link and immutable thereafter; slots freed only after terminal legacy TG drain |
| g->gc2.tg_registry_incomplete / TG.registry_shadow_missed | sticky universe veto + exact per-body legacy-only marker | failure store rel; readers acq; universe bit never clears; future stable authority must reject an incomplete spine |
| LJTGRegistrySlot.token / body_value | shadow TG lifecycle + tagged body lease | exact CX16 lifecycle/lease CAS; tagged body exact-CAS; negative reclaim veto only while legacy raw-list/SMR gates remain positive authority |
| TG.registry_key | owner lifecycle key | owner-private before slot head release; immutable while attached; cleared only by the legacy reclaimer after RECLAIMING admission |
| TG.poll / TG.reqmask | signal word | store rel by GC; load rlx by owner; ack CAS acq_rel |
| Node.key | write-once | CAS rlx claim; read rlx and re-check |
| Node.next | chain link | CAS rel insert; load acq walk |
| strtab bucket head | chain link | CAS rel insert; load acq walk; bit0 = Harris mark, bit1 = secondary hash |
| g->str.mask | compatibility mirror | bootstrap store rlx; resize publish rel; diagnostic readers acq |
| g->str.num | conservative string count | range fetch_add rlx; free/credit flush fetch_sub acq_rel; readers acq |
| g->str.id | string ID range allocator | range fetch_add rlx; gaps are allowed |
| g->str.second | secondary-hash table flag | store rel under table claim; readers acq |
| J->tracev / TraceVec.slot[i] | RCU vector + publish-once slots | vector store rel / load acq; slot store rel after mcode sync / load acq |
| J->L | recorder-token owner pointer | store rel / load acq; only unchanged token owner may restore or clear |
| J->activemcode | preowned active-mcode retirement records | CAS rel/acq; batch exchange at full flush; raw nodes marked from JIT roots |
| J->retiredmcode | retired mcode records | CAS rel/acq; retire epoch store rel; free after `LJ_FLUSH_EPOCHS` completed epochs |
| J->retiredtraces / GCtrace.retired_next | token-owned tagged retired trace bodies | head CAS/exchange acq_rel; embedded link rel/acq; free only under token + zero-reader gate after `LJ_FLUSH_EPOCHS` |
| GCtrace.retire_epoch | trace-entry retirement gate | token-owned CAS acq_rel of encoded epoch + 1; zero means live |
| GCtrace.native_pins | exact native-body admission + lease count | high `CLOSED` bit and low 31-bit count share one CAS acq_rel word; pin increments only while open under an independent body lease; retirement closes after the epoch LP and before slot disposition; final closed unpin publishes count zero before notifying reclaim and must not dereference the body afterward |
| J->trace_pin_release_seq / J->{trace,mcode}_reclaim_pin_seq | final native-unpin notification + token-owned scan memos | final closed unpin increments by CAS acq_rel; reclaimers load acq before scanning; memo fields are mutated only with recorder token + exact reclaimer gate and are authoritative only when both completed epoch and release sequence match |
| TG.ffi_native_seq | whole-stack generic FFI native-frame sequence | single owner publishes odd with release then a writer barrier before mutation; final even generation stores release; observers load the initial and final generation acquire and accept only the same even value; wrap poisons odd and fail-stops |
| TG.ffi_native_depth / TG.ffi_native_frame[] payload | production generic FFI native-frame stack and preallocated result roots | depth and every remotely sampled payload word use atomic release/acquire access; a coherent snapshot additionally requires the enclosing same-even `ffi_native_seq`; offsets are opaque until the certified stack scanner validates them; `result_root` is marked as an exact thread root in ACTIVE, every SUSPENDED continuation, and POSTCALL until odd-sequence cleanup clears it |
| g->gc2.ffi_native_scan_* | certified native-frame scanner telemetry | relaxed atomic increments; acquire stats snapshots; counters carry no root-scanning authority |
| GCtrace.unused1 entry-gate bits | trace metadata flags | `TRACE_ARM64_INT_LOOP_ADMITTED` is set on the token-private trace only after post-RA validation and becomes immutable before trace-slot release publication; ARM64 root entry loads it acquire on both metadata passes. `TRACE_SCOPE_FLUSH_PENDING` and `TRACE_ENTRY_INVALIDATED` publish by byte CAS acq_rel and VM/C entry loads acq; invalidation gates entry only, while scoped pending authorizes dependency closure + `EXIT_TRACES` retirement; exittab ownership remains an independent relaxed bit |
| g->vmevent_owner | VM-event callback owner TG id | one nonwaiting CAS acq_rel; exact-owner CAS release to zero |
| BCIns at patch sites | generation word | one 32-bit load acq snapshot; idempotent single-writer transitions store rel; competing semantic transitions use exact full-word CAS acq_rel (`bc_publish_cas`), with immutable prototype sidecars for stale JIT recovery |
| GCproto.jit_startins[i] | immutable original-bytecode recovery sidecar | zero at prototype construction; publish the complete original word rel before the first J* publication; load acq after observing J*; a nonzero slot may only be republished identically and lives with the prototype |
| GCtrace.exittab[i] | retarget word | store rel; loaded by indirect branch in mcode |
| L->thr_owner | claim word | CAS acq_rel |
| g->str.tabh | RCU pointer | acq / rel |
| cts->tabh / cts->top | RCU vector + ticket | see 11 §11.2 |

`GCtab.gc2_rescan_state` is not an advisory copy of `LJ_GC_NEEDSCAN`.
`NONE` owns no aggregate credit, `INSTALLING` owns a provisional credit,
`COUNTED` owns one committed credit, and `CANCELLED` leaves settlement to the
installer that owns the provisional credit. Shared code must use the atomic
accessors in `lj_obj.h`; constructors initialize `NONE` before publishing the
table body.

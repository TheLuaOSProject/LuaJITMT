# LuaJIT-MT Shared Field Classification

Seeded from `plan/02_memory_model.md` §2.5 during M1. Extend this table
whenever a shared field is introduced or migrated.

| field | class | ordering |
|---|---|---|
| TValue in table array/hash, upvalue cell, registry, gcroot | shared slot | rlx (store: rlx; structural publish: rel) |
| GCtab.arrayhdr / nodehdr (new MRefs, 06) | RCU pointer | load acq / store rel |
| GCtab.metatable, GCudata.metatable | shared ptr | rlx; publish rel |
| GCtab.nomm | advisory byte | rlx; construction masks and explicit clears only |
| GCobj.gch.gct | immutable after publish | plain read; written pre-publish |
| arena bitmaps (block/mark) | atomic bitset | fetch_or rlx; sweep owns exclusive |
| g->gc2.phase | phase word | load rlx in fast paths; transitions rel + handshake |
| TG.poll / TG.reqmask | signal word | store rel by GC; load rlx by owner; ack CAS acq_rel |
| Node.key | write-once | CAS rlx claim; read rlx and re-check |
| Node.next | chain link | CAS rel insert; load acq walk |
| strtab bucket head | chain link | CAS rel insert; load acq walk; bit0 = Harris mark, bit1 = secondary hash |
| J->tracev / TraceVec.slot[i] | RCU vector + publish-once slots | vector store rel / load acq; slot store rel after mcode sync / load acq |
| J->retiredmcode | retired mcode records | CAS rel/acq; free after `LJ_FLUSH_EPOCHS` completed epochs |
| J->retiredtraces / GCtrace.retired_next | retired trace bodies | CAS rel/acq; free after `LJ_FLUSH_EPOCHS` completed epochs |
| BCIns at patch sites | code word | single 32-bit store rel (`bc_publish`) |
| GCtrace.exittab[i] | retarget word | store rel; loaded by indirect branch in mcode |
| L->thr_owner | claim word | CAS acq_rel |
| g->str.tabh | RCU pointer | acq / rel |
| cts->tabh / cts->top | RCU vector + ticket | see 11 §11.2 |

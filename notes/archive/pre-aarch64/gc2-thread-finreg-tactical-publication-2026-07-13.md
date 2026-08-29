# GC2 threading and FINREG tactical publication

## Scope

Four high-frequency raw-allocation publication pairs now use
`lj_gc2_markmem_registered_publish_try()` instead of the mandatory raw marker:

- userdata FINREG node registration, before and after its active-list CAS;
- `threading.thread` start-root vectors, before and after the release-store of
  `LJThread.start_roots`;
- `LJThreadLive` initialization and the post-active-list-CAS mark; and
- detached `LJThreadLive` nodes, before and after retired-list publication.

The conversion is deliberately explicit. It does not include FINREG lookup or
retirement, close-upvalue paths, or any mandatory GC root scan.

## Independent body ownership

Each tactical marker is only a cycle-start/root-snapshot barrier. It is not the
authority to dereference or retain the raw body:

- A newly allocated FINREG node is owned by the registering mutator until the
  active-list CAS. Active membership owns it after that CAS; unlink first
  transfers it to the retired list, and physical free is deferred until
  terminal list teardown.
- A start-root vector is owned by the spawn constructor while its elements are
  initialized. The count release precedes the pointer release; after the
  pointer linearization point, the valid `UDTYPE_THREAD` userdata owns the
  vector until userdata destruction.
- A new live node is locally owned by the spawn constructor. The active-list
  CAS transfers ownership to native-root membership; removal preserves any
  in-flight scanner under SMR before transferring the detached node to the
  retired list.
- Removal supplies an exact detached live-node owner before retirement. The
  retired-list CAS transfers that body to the append-only tombstone list,
  which retains it until terminal shutdown.

Consequently, an ordinary IDLE metadata writer holding `smr_reclaiming` may
make either tactical mark miss without creating an unowned body. The helper
requests root-certificate repair but does not mutate the typed activation or
grant a body lease. The post-publication call remains required: if phase state
changes around the CAS/store, it reopens the appropriate root scan rather than
silently accepting a missed newly visible root.

## Mandatory scans remain strict

`gc2_scan_finreg_udata_nodes()` and `gc2_scan_threading_live_roots()` retain
their scoped mandatory markers for active and retired lists. Thread-userdata
traversal likewise retains its scoped mandatory marker for `start_roots`.
Those scans have no local constructor ticket that would authorize omission,
so admission failure still rejects the root certificate and retries the scan.

The focused `t-gc2-sidecar-publication` fixture clears all four raw mark bits,
then enters the production IDLE reclaim test scope, including exact TLS
ownership and native-JIT gate closure, and executes the real factored
publication boundaries. It proves the activation snapshot stays bit-identical
with zero SMR readers while the exact reclaimer capability remains held. It
then leaves that scope, starts MARK, runs the unchanged mandatory global and
userdata scans, drains mark work, and proves every published raw body is
marked. The fixture runs both in the default helper build and with
`LUA_USE_ASSERT` plus `LJ_GC2_PARANOIA=1`.

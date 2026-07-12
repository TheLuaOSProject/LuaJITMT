# Userdata construction and specialization publication (2026-07-12)

## Scope

This tranche closes the lifetime and partial-payload windows between allocating
a `GCudata`, publishing its arena `READY` identity, registering it with userdata
FINREG, specializing its private payload, and installing its first ordinary Lua
or native semantic root.  It covers generic `lua_newuserdata`, threading thread,
mutex and channel userdata, `string.buffer`, FFI `CLibrary`, and I/O file userdata.

The ownership/root-spine link is not itself a semantic reachability root in GC2.
A constructor therefore cannot use that link as a substitute for a stack, TG, or
native root while it allocates, waits, or reacquires a Lua-state claim.

## Constructor-root protocol

`LJUdataRoot` reserves a nil slot in the current TG root-anchor stack before the
userdata allocation.  The constructor then:

1. allocates an unlinked traversable body with the nonthrowing allocator;
2. initializes the complete generic userdata header (`UDTYPE_USERDATA`);
3. release-stores the userdata TValue into the reserved anchor while the arena
   identity is still `READY=0`;
4. publishes `READY`;
5. runs the semantic-root publication barrier on the anchor; and
6. links the fully admitted object to the pending after-main ownership chain.

A root scan which sees step 3 rejects the opaque `READY=0` body.  The barrier in
step 5 repairs exactly that observation.  There is no inverse window in which a
`READY=1` body exists without the constructor anchor already naming it.

The final stack handoff is deliberately nonthrowing.  Specialized callers grow
their result stack before creating the root, publish the result slot, increment
`top`, and only then release the anchor.  Generic `lua_newuserdata` retains the
same root while it drops and reacquires the target-state claim and releases it on
every claim/stack-growth error path.

## Explicit error ownership

Lua fast `pcall`/`xpcall` paths do not universally pass through a central C
checkpoint capable of unwinding arbitrary TG anchors.  Consequently no
specialized constructor relies on global error cleanup.

Userdata FINREG now has a nonthrowing registration entry.  Its raw-node
allocation returns OOM before any node publication, FINREG-bit change, registry
mutation, or counter update.  `lj_udata_finreg_mt_rooted` releases the constructor
root before raising that OOM.  Existing callers which do not hold a constructor
root retain the original throwing API, now implemented as a wrapper around the
same primitive.

Initial `string.buffer` storage also uses a nonthrowing empty-buffer allocation.
On OOM it releases the constructor root before raising.  This avoids both a
longjmp leak and a protected-call cost in the normal buffer construction path.

## One-time subtype publication

Specialized payloads remain publicly generic while their fields are initialized.
`lj_gc_udata_payload_valid_as()` validates the complete payload against the
prospective subtype before publication.  A release CAS from `UDTYPE_USERDATA` to
the specialized type is the one-time linearization point; a second specialization
or invalid geometry is an internal invariant failure.  After the CAS, the helper
publishes the userdata as a semantic root again, forcing an active-cycle rescan
of an already-marked userdata (or recording it for a later generational cycle).

The ordering is therefore:

`generic + rooted -> complete payload/edges -> prospective validation -> subtype
release CAS -> forced userdata rescan -> permanent root -> anchor release`.

I/O file userdata now uses the same validated CAS/rescan helper after `fp` and
`type` are initialized; no specialized constructor directly stores `udtype`.

## Specialized cases

- Thread, mutex, and channel userdata are pre-grown on the Lua stack, rooted,
  FINREG-registered while generic, fully initialized, specialized once, rescanned,
  and then pushed.  The duplicate thread subtype store during child-state
  publication was removed.
- A thread startup-root vector is completely initialized, marked as raw GC2
  memory, release-published with count-before-pointer ordering, marked again, and
  followed by a userdata rescan.  This closes bitmap-clear/root-snapshot races.
- Buffer initial backing storage is allocated and marked before the buffer
  subtype CAS, marked again after publication, and covered by the userdata
  rescan.  Dictionary and metatable children are barrier-published first.
- `CLibrary` roots its private cache table in the eventual result stack slot
  before userdata/FINREG allocation.  Its handle starts as `NULL`; cache fields
  are complete before subtype publication.  `dlopen`/`LoadLibrary` now runs only
  after the finalizable CLibrary object is rooted and registered, so a later
  allocation failure cannot strand a successful native handle.  Loader errors
  unwind a destructor-safe object with a null handle.

## Verification

`tests/t-udata-construction-roots.c` checks:

- survival through complete GC2 cycles with only the TG constructor anchor;
- prospective specialization and post-specialization survival;
- permanent-stack-root handoff and anchor release;
- injected FINREG OOM cleanup through public C `lua_pcall`;
- injected FINREG OOM cleanup through Lua fast `pcall` and `xpcall`, with
  `root_anchor_top` checked against its baseline before and after full GC; and
- repeated buffer/mutex/channel/thread/CLibrary construction with forced full GC.

The focused M5 suite case rebuilds with `LJ_UDATA_TEST_HELPERS` and assertions,
runs the fixture, and restores the default build configuration afterward.

## Follow-up boundary

The compatibility `lj_udata_new()` entry releases its temporary constructor root
before returning and is only safe for callers which immediately establish their
own nonthrowing semantic root.  Current generic public creation uses the rooted
entry directly.  Future internal userdata types must use the rooted constructor
and one-time specialization helper rather than adding a direct subtype store.

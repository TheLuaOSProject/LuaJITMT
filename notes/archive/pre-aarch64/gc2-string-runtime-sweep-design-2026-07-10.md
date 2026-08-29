# Nonblocking GC2 interned-string reclamation design

GC2 marks reachable string bodies, but the current table never removes
unmarked strings during a live VM. The old destructive string sweep is gone;
shutdown has an exact terminal drain. This note defines the runtime replacement
without reviving a color state or making an interner wait for the collector.

## Existing synchronization to retain

Each interner pins one `StrTabHdr` in its TG, then traverses immutable string
bytes and the atomic bucket/`nextgc` chain. Resize owns the existing `resize`
word and waits for those pins before rewriting all links. Old headers are
already epoch-retired. `LJ_STRHASH_DEAD` is an unused low pointer tag and can be
the logical-retirement bit for the string named by a bucket/next link.

The production `lj_str_sweep_claim()` is not usable: it waits for all active
pins and would make new interners wait behind a whole-table collector pass.

## Link-tag protocol

Treat each atomic link as `(target pointer, DEAD, SECONDARY)`. `SECONDARY` is
meaningful only on bucket heads; `DEAD` describes the target object.

1. The GC2 sweep owner CAS-tags an unmarked, non-fixed target `DEAD` in place.
   The object and its old `nextgc` remain immutable and mapped.
2. An interner which matches a tagged target CAS-clears `DEAD`, marks/rescues the
   exact string in GC2, and returns the canonical object. A failed CAS restarts
   from the bucket because the target may already have been unlinked.
3. New head insertion must copy an old head's `DEAD` tag into the new string's
   next link; otherwise a prepend would accidentally resurrect an unmarked
   target. It strips only the bucket's `SECONDARY` bit.
4. After every bucket has been tagged and a completed handshake has allowed
   rescues, the owner CAS-unlinks links still tagged `DEAD`. Replacement keeps
   the next target's `DEAD` bit and the current bucket's `SECONDARY` bit.
5. The unlink owner must reserve a side retire record before the CAS. Allocation
   failure leaves the tagged object linked and retries later; it never creates
   an undiscoverable body.

Do not reuse the unlinked string's `nextgc` as a retire-list link. An interner
which loaded the object before unlink may still read that exact next link.
Use an external record containing the string pointer, header generation and
retire epoch. Free the body only after the post-unlink grace and after no TG is
pinned to that header. Reclaim is opportunistic and never waits.

## Collector/resize arbitration

Split `StrTabHdr.resize` into owner bits. Resize retains its exclusive bit.
String sweep uses a separate bit which:

- prevents a resizer from beginning link rewrites;
- does **not** block `strtab_enter()` or ordinary interning;
- is acquired with one CAS and abandoned immediately if resize already owns the
  header;
- covers bounded tag/unlink batches only, with cursor state in `GC2State`.

Resize requested during sweep simply skips and retries on a later growth event;
it must not wait for the collector. Once runtime sweep lands, resize itself
still needs a later immutable-generation redesign to remove its pre-existing
active-pin wait.

## Phase state

GC2 needs explicit string subphases and cursors:

- `TAG`: scan bounded buckets and tag currently unmarked strings;
- `RESCUE_GRACE`: complete a handshake while tagged strings remain canonical;
- `UNLINK`: CAS-remove unrescued targets, publishing side retire records;
- `RECLAIM`: ordinary SMR drain frees records older than the completed epoch.

MARK/WEAK barriers keep using arena mark bits. During SWEEP, a matching interner
must clear the link tag and call the sweep-root rescue path before returning.
Fixed/SFIXED strings are never tagged. String-count credits remain conservative;
the first successful physical destructor consumes one published count.

## Required focused proofs

- tag-before-match and match-before-tag races both return one canonical object;
- head prepend preserves the old target's DEAD state;
- rescue racing unlink either keeps the old canonical object linked or misses it
  and creates a new one only after the old object is irrevocably retired;
- resize and sweep ownership never overlap, and interning continues while sweep
  owns bounded batches;
- a reader paused after loading a soon-unlinked string can finish its next-link
  walk through two completed grace epochs without UAF;
- retire-record OOM retains the tagged string and makes bounded progress later;
- repeated live cycles reduce `str.num` and resident bytes without changing
  intern identity, fixed strings, weak semantics, or throughput materially.

## Landed boundary: tagged-link interner slice

The first bounded slice is now present in `lj_str.h`, `lj_str.c`, and the M5
string CAS fixture. It deliberately stops before collector ownership or
reclamation:

- string links have shared raw acquire-load and acquire/release-CAS helpers;
  target decoding always strips both link tags before dereferencing;
- a new head stores the prior bucket head in its `nextgc` while stripping only
  `SECONDARY`, so the old target's `DEAD` state survives the prepend;
- both pre-allocation and pre-publication intern scans retain the exact incoming
  `GCRef` for each target. A matching tagged target is CAS-cleared and passed to
  the phase-aware `lj_gc2_preserve_sweep_root()` path. A changed edge or lost
  CAS restarts at the bucket head, preserving one canonical intern identity;
- ordinary non-SWEEP untagged matches keep a one-phase-load fast path. Exact
  incoming-edge revalidation is enabled for SWEEP, when a tagger may publish
  `DEAD`, and for the deterministic test hook only;
- the fixture proves DEAD-safe prepend, tag-after-byte-match rescue, a competing
  rescue winning the CAS followed by an actual bucket retry, raw tag-preserving
  CAS behavior, and bucket-only `SECONDARY` semantics.

There is still no production tag scan, sweep cursor, sweep/resize owner bit,
unlink pass, side retire record, grace protocol, string-body destructor, or
`str.num` reclaim credit in this slice. Resize and secondary rehash have not yet
been generalized to transfer an incoming target's `DEAD` bit while rebuilding
chains; a production tagger must not be enabled until collector/resize
arbitration and that tagged rebuild rule land. The old globally waiting
`lj_str_sweep_claim()` is unchanged and is not used by the new interner path.

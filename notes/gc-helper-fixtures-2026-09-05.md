# GC helper fixture repair

The hard-assist and allocation-account fixtures now establish the collector
state they actually measure. This is test maintenance on runtime `79345529`;
it changes no runtime protocol or admission gate.

The hard-assist fixture completes the preceding cycle with the public driver
and requires actual IDLE with no MARK-close intent. A preserving abort could
legitimately leave that intent pending and make the next assist refuse.
Allocation accounting now checks the twenty real old-edge publications across
two meta stores, retaining exact parent counts and adding stored-value checks.
Its later one-item assist case drains graph setup before starting a fresh cycle
and proves free SSB storage, so the flush cannot advance the measured frontier
while recycling older work. Original assist and young-edge assertions remain.

Ten final isolated processes pass across optimized, assertion/APICHECK and
target-only ASan configurations on exact 843 and 793 runtime inputs. Both
repairs also pass in the registered M6 helper build. M10 passes all three
components, including allocation accounting on the default build.

**M6 remains incomplete:** its unchanged idle-reclaim-entry case times out at
the original 20-second limit, leaving the last two cooperative cases unrun.
A separate GDB observation locates main in scalar ITERN through
`lj_tab_next_rooted` while the real IDLE reclaimer remains deliberately paused.
That ordinary-iteration progress dependency is being investigated as runtime
work. The fixture's paused window and outcomes remain intact.

See the [root review](evidence/gc-helper-fixtures-2026-09-05/root/review.md),
[owner's final review](evidence/gc-helper-fixtures-2026-09-05/owner/final-review.md),
[canonical results](evidence/gc-helper-fixtures-2026-09-05/root/canonical.json),
and [verified archive manifest](evidence/gc-helper-fixtures-2026-09-05/manifest.json).
Original failures, superseded diagnostic hypotheses and failed candidates are
preserved. No full-suite, performance or release-readiness claim follows.

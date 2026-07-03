# String intern redundant hash removal

`lj_str_new()` used to compute the sparse hash before entering the intern loop
and then recompute it after snapshotting the current string-table header. The
pre-loop value was only needed by the rare no-table fallback; normal existing
and new string paths used the recomputed value.

The redundant pre-loop hash is removed. The hash is now computed after the
current header is known, and the no-table fallback computes it locally before
allocating a standalone string.

This deliberately does not remove the TG-local active marker from normal
interning. A secondary attach/spawn can publish `mt_entering` while another
thread is already inside `lj_str_new()`, and the active marker is what lets a
concurrent resize/retire observe that in-flight table generation. The protocol
remains the current 06 section 6.5 active-drain bridge; this slice only removes
duplicated CPU work inside that protocol.

# Candidate 2 source freeze

Base remains runtime79345529bd932e68f8159ec17224467a10cad09b. Candidate1 source,
patch, builds, caller-stop fixtures and both detach-overlap exits2 remain intact.

Only the exiting physical worker changes relative to candidate1: it calls
existing lj_native_leave_tg before lj_tg_detach. This closes the startup depth1
native scope, fences and polls on the exact worker TG. Already consumed actions
retain the existing full poll hold before private teardown. A new action after
that close is no longer remotely admitted; normal detach owns its local poll
or final DEAD retirement of the counted slot. The controller's STOP/native/join
and returned action handling are unchanged. No gate or action hold is deleted.
The GC scheduling/result helper is byte-for-byte candidate1.

The exact candidate1 observation-only detach witness has a native0 branch that
requires the same later request to remain owner-pending with no remote action
before allowing detach to finish. It will run unchanged against this source.
No runtime result is claimed at this freeze. Synchronous handshake/whole-chain
flush limits, and the separate constructor retention case, still remain.

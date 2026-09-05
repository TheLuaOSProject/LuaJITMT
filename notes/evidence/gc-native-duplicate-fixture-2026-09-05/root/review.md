# ROOT review: same-epoch native request observation

Runtime at integration is eb8a5b2f (HEAD4b4ed7c2 adds only scheduler cleanup).
The permanent fixture exactly matches owner correction v2, SHA256
f8f0ce140091d4105419177d5c3bb9299858debc46d6f628d8aeceea944cdc4a.
No runtime source changes in this commit.

The original consumed/pre-claim hook assumed the request mask stayed zero.
Production late signal and remote-refusal requeue can publish the same counted
request again before the owner's epoch claim. The owner supplied a real
observation-only reproduction on both base793 and its isolated worker candidate2:
mask4128, callback epoch19 and prior ACK18. Earlier ordinary passing repeats
did not classify this race. Both original assertion failures are preserved.

The accepted fixture reads the mask once. A nonzero mask must equal exactly
SCAN_OWNER_ROOTS|FLUSH_SSB, with matching published handshake epoch and actions.
The owner still has not acknowledged this counted slot at that hook, so the
round cannot finish and advance past its epoch. No request/epoch/poll/native
field is injected. Every later actual scan, exact consumed-poll kernel wait,
no early clear/teardown and final leader/pending assertion remains unchanged.

ROOT verified the worker package's 3,239 artifact hashes before combination;
the focused archive below independently rechecks the selected duplicate
evidence. The owner's eight final ordinary/forced runs cover base793 and worker
candidate2 in assertion and ASan/LSan builds. ROOT adds four exact current-runtime
ordinary/forced runs, using the existing ten-define strict and target-ASan fair
builds with all225 runtime/generator inputs verified before and after. Both
forced runs observe mask4128 and execute all original terminal checks. The
registered native-completion suite adds11 passing processes and restores the
default build. Total23 positive runtime processes across these explicit source
generations; compile/debugger runs and original negatives are separate.

The forced observer is archived as a diagnostic fixture; the permanent fixture
retains its ordinary scheduling window. This repairs a test observation, not
the inherited synchronous native-root borrowing protocol. The separate worker
runtime candidate and its new exit ordering are not part of this commit.

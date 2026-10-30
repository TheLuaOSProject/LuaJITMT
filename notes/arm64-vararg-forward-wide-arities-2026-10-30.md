# Wider fixed-arity vararg forwarding

The zero-formal `...` tail-forwarder proof now derives its argument limit from
`LJ_MAX_JSLOTS`. Runtime preflight, source recognition, semantic admission and
post-RA admission retain exact `CALL C`, target parameter count, contiguous
caller slot window, immutable target identity and numeric leaf checks. The
forwarder source extent is checked against the actual `VARG` destination and
the target count. Result vectors remain capped at three; this change widens
forwarded arguments only.

The four-profile vararg contract passed on ARM64 and arm64e in assertion and
release builds. Native runner roots for target arities 1, 2, 3, 4, 8, 16 and
32 each completed three calls, including target identity replacement and
full-GC reuse. Each profile rejected 115 copied mutations independently in
semantic and post-RA admission, including removal of the last source argument
map entry. A 198-argument interpreted case exercised a caller window ending
at slot 249 of the 250-slot VM limit. Its forwarder and target passed source
recognition, the loop did not publish a root, and eight candidate executions
matched the separate interpreter oracle.

The exact ordinary probe runs the candidate with a live threading peer, keeps
factory/observer/oracle functions interpreted, and observes only `texit`.
On ARM64 the selected runner `JFORL` traces were reused with 20 selected-root
exits for each of arities 4, 8, 16 and 32. The saved x86_64 binary selected
and reused its runner loop roots for the same source, with 1,899 selected-root
exits per case. Those exit counts reflect the different warm/reuse sequences;
they prove the selected roots executed and remained reusable, not equal trace
topology. The primary logs are
`/Users/frityet/Scratch/ljmt-vararg-forward-wide-observer-arm64.log` and
`/Users/frityet/Scratch/ljmt-vararg-forward-wide-observer-x64.log`.

An earlier secondary ARM probe omitted the live peer and reported repeated
`UREFO` assembly refusal. Adding the same live peer as the contract changed
that outcome to selected runner roots; the no-peer probe is not used as parity
evidence. Four-profile contract evidence is in
`/Users/frityet/Scratch/ljmt-vararg-forward-wide-contract-cap2.log` records
the initial 108-mutation run. Review restored the previously skipped VARG
destination corruption test in `00de6d8f6`; the full four-profile varargs
contract then passed with 115 semantic and 115 post-RA rejections per profile.
That follow-up was captured in the tool session, without a separate log file.

The focused contract and ordinary probes used source base `a2b20c5` plus the
uncommitted source/test diff whose SHA-256 is
`0d126687a95af6a03b879ffb552eb30184cc8205909128eabd06630b3e57b2db`.
The ARM profile wrapper restored ordinary assertion ARM64 without test helpers
after the four-profile run.

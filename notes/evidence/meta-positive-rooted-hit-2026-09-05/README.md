# Positive rooted-hit functional evidence

See notes/meta-positive-rooted-hit-2026-09-05.md. The frozen build/initial
metadata predates only the final header documentation correction; compare
final-source-snapshot.json and measured-to-final-header.patch. Eight initial
strict/ASan C runs pass and the nil-on-failure negative fails as intended.
initial-validation.json preserves the incorrect resize case-selection variable
and its JIT-off observer failure; corrected-driver-validation.json supplies
the actual fifteen general plus three native, weak/finalizer, and remote cases
and repeats both stock modes with the intended module path. canonical.json
records the additional metamethod and x64 rooted-read passes. Independent
review and real queue-pressure evidence have separate subdirectories.
Original executable/build artifacts remain at their recorded /tmp paths.

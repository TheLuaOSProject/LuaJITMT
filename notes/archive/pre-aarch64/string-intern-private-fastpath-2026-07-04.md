# Rejected String Intern Private Fast Path

I tested a single-mutator `lj_str_new()` path that skipped the string-table RCU
active pin and bucket CAS when the current TG appeared to be the sole possible
mutator-observer of the string table. The proposed guard required:

- no nested string-table active pin on the current TG;
- legacy GC is not in `GCSsweepstring`;
- the current TG is not executing through a JIT frame/helper;
- no active or attaching Lua thread;
- no GC2 workers; and
- no active mark slice on the current TG.

The prototype still performed the normal bucket lookup, duplicate return,
dead-string resurrection, string ID allocation, per-TG string-count publication,
secondary hash flag preservation, and post-insert resize. It fell back to the
shared RCU/CAS path for resize-in-progress, collision rehash, and nil-header
cases.

The prototype passed the focused string-table CAS/rehash fixtures and a C probe
that checked bucket cycles across repeated array fills and full collections.
It was rejected because repeated Lua full-GC workloads regressed badly in wall
time, and JIT-enabled versions of the same workloads could time out before the
first benchmark line. The string chains were not obviously cyclic, so the next
attempt should instrument GC state/accounting around `collectgarbage("collect")`
before reintroducing any active-pin-free intern path.

The conservative conclusion is that string resize and legacy string sweep still
need the existing RCU/CAS path until traced and full-GC interning interactions
have dedicated coverage.

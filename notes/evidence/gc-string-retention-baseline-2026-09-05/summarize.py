from pathlib import Path
import json

p = Path(__file__).resolve().parent

def records(filename):
    return [json.loads(line) for line in (p / filename).read_text().splitlines()
            if line.startswith("{")]

matrix = []
for variant in ["normal", "strict", "asan"]:
    runs = json.loads((p / (variant + "-results.json")).read_text())
    for run in runs[1:]:
        rows = records(run["stdout"])
        base = next(r for r in rows if r.get("stage") == "baseline")
        observed = [r for r in rows if r.get("stage") in
                    ["settled", "automatic_progress_missing"]][-1]
        recovered = next(r for r in rows if r.get("stage") == "sole_main_recovered")
        matrix.append({"variant": variant, "case": run["case"],
                       "exit_code": run["exit_code"], "baseline": base,
                       "observed": observed, "recovered": recovered,
                       "completed_delta": observed["completed"] - base["completed"],
                       "starts_delta": observed["cycles"] - base["cycles"],
                       "retained_string_delta": observed["strings"] - base["strings"],
                       "reclaimed_delta": observed["reclaimed"] - base["reclaimed"],
                       "accounted_byte_delta": observed["bytes"] - base["bytes"]})

extra = []
for variant in ["normal", "strict", "asan"]:
    for kind in ["recovery", "trace"] + (["trace_off"] if variant == "strict" else []):
        filename = variant + "-" + kind + "-results.json"
        for run in json.loads((p / filename).read_text())[1:]:
            rows = records(run["stdout"])
            extra.append({"variant": variant, "kind": kind, "run": run,
                          "observations": [r for r in rows if r.get("stage") in
                            ["baseline", "automatic_progress_missing", "workers_disabled",
                             "last_peer_joined", "automatic_after_last_detach",
                             "sole_main_recovered"]]})
summary = {"head": "e34282576c7df0180e8113a4cfba07fd637a36b3",
           "matrix_runtime_processes": len(matrix),
           "matrix_completed_cases": sum(r["exit_code"] == 0 for r in matrix),
           "matrix_missing_progress_cases": sum(r["exit_code"] == 2 for r in matrix),
           "additional_runtime_processes": len(extra),
           "matrix": matrix, "extra": extra}
assert len(matrix) == 24 and len(extra) == 14
assert all(r["exit_code"] in [0, 2] for r in matrix)
for e in extra:
    assert e["run"]["exit_code"] == (0 if e["kind"] == "recovery" else 2)
for v in ["normal", "strict", "asan"]:
    for r in [m for m in matrix if m["variant"] == v]:
        e, peer, workers = map(int, r["case"].split("-"))
        assert r["recovered"]["strings"] == r["baseline"]["strings"]
        if e:
            assert r["completed_delta"] == 12
            assert r["retained_string_delta"] == (24576 if peer or workers else 0)
            assert r["reclaimed_delta"] == (0 if peer or workers else 24576)
        elif not peer and not workers:
            assert r["completed_delta"] >= 18 and r["retained_string_delta"] == 24576
        elif peer:
            assert r["starts_delta"] == r["completed_delta"] == 0
            assert r["retained_string_delta"] == 4096
        else:
            assert r["starts_delta"] >= 1 and r["completed_delta"] < 3
(p / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
for r in matrix:
    print(r["variant"], r["case"], r["exit_code"], r["starts_delta"],
          r["completed_delta"], r["retained_string_delta"], r["reclaimed_delta"],
          r["accounted_byte_delta"], r["observed"]["bytes"])

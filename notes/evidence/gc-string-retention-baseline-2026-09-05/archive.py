from pathlib import Path
import hashlib
import json
import shutil

p = Path(__file__).resolve().parent
dst = p / "evidence"
dst.mkdir(exist_ok=True)
frozen = json.loads((p / "source-identity.json").read_text())
excerpts = []
for name, first, last in [
    ("src/lib_threading.c", 880, 932), ("src/lj_api.c", 2960, 3047),
    ("src/lj_gc.c", 45, 78), ("src/lj_gc.c", 4146, 4213),
    ("src/lj_gc.c", 4281, 4312), ("src/lj_gc2.c", 2755, 2824),
    ("src/lj_gc2.c", 2837, 2901), ("src/lj_gc2.c", 3227, 3272),
    ("src/lj_gc2.c", 3370, 3393), ("src/lj_gc2.c", 14975, 15030),
    ("src/lj_gc2.c", 22343, 22407), ("src/lj_gc2.c", 6504, 6538),
    ("src/lj_str.c", 269, 306), ("src/lj_str.c", 1254, 1300),
    ("src/lj_str.c", 1335, 1416), ("src/lj_str.c", 1729, 1753),
    ("src/lj_str.c", 1893, 1920), ("src/lj_trace.c", 6338, 6354),
    ("src/vm_x64.dasc", 419, 435), ("src/lj_str.h", 77, 85)]:
    raw = (p / "normal" / name).read_bytes()
    assert hashlib.sha256(raw).hexdigest() == frozen["inputs"][name]
    excerpts.append({"path": name, "first_line": first, "last_line": last,
                     "full_source_sha256": frozen["inputs"][name],
                     "text": "".join(raw.decode().splitlines(True)[first - 1:last])})
(p / "source-excerpts.json").write_text(json.dumps(excerpts, indent=2) + "\n")

items = []
top_files = [q for q in p.iterdir() if q.is_file()]
for q in p.iterdir():
    if q.is_dir() and (q.name.endswith("-input") or q.name == "review-input"):
        top_files += [f for f in q.rglob("*") if f.is_file()]
for variant in ["normal", "strict", "asan"]:
    build = json.loads((p / (variant + "-build.json")).read_text())
    top_files += [p / variant / "src" / name for name in build["binaries"]]
for q in sorted(set(top_files)):
    raw = q.read_bytes()
    rel = q.relative_to(p)
    binary = raw.startswith(b"\x7fELF") or q.suffix in [".tar", ".a", ".o"]
    info = {"source": str(q), "relative_path": str(rel),
            "sha256": hashlib.sha256(raw).hexdigest(), "bytes": len(raw),
            "storage": "hash-only" if binary else "text"}
    if not binary:
        raw.decode("utf-8")
        target = dst / rel
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(q, target)
        assert hashlib.sha256(target.read_bytes()).hexdigest() == info["sha256"]
    items.append(info)
manifest = {"head": frozen["head"], "package": str(p),
            "artifacts": items,
            "text_files": sum(i["storage"] == "text" for i in items),
            "text_bytes": sum(i["bytes"] for i in items if i["storage"] == "text"),
            "hash_only_files": sum(i["storage"] == "hash-only" for i in items)}
(dst / "artifact-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
print(json.dumps({k: v for k, v in manifest.items() if k != "artifacts"}))

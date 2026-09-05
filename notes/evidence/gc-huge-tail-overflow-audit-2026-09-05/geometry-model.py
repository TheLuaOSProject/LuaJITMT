"""Recheck proposed geometry only; this does not execute the allocator."""
import json
from pathlib import Path

ROOT = Path(__file__).parent
record = json.loads((ROOT / "source-and-geometry.json").read_text())
A = record["layout"]["arena_alignment"]
H = record["layout"]["header_bytes"]
W = record["layout"]["wide_bytes"]
PAGE = record["host_page_bytes"]
MAX_ROUNDABLE = ((1 << 64) - 1) & ~(A - 1)


def mapsize(size, tail):
    extra = H + (W if tail else 0)
    if size <= A // 4 or size > MAX_ROUNDABLE - extra:
        return 0
    return (size + extra + A - 1) & ~(A - 1)


for example in record["all_huge_tail_geometry_examples"]:
    size = example["logical_size"]
    old = mapsize(size, False)
    new = mapsize(size, True)
    assert old == example["old_map_bytes"]
    assert new == example["tail_map_bytes"]
    assert (new - old if new else None) == example["additional_map_bytes"]
    if new:
        tail = new - W
        end = H + size
        assert tail % W == 0 and tail >= end
        assert tail == example["wide_offset"]
        assert end == example["payload_end_offset"]
        assert (end - 1) // PAGE == example["payload_last_page"]
        assert tail // PAGE == example["wide_page"]
        assert ((end - 1) // PAGE != tail // PAGE) == example[
            "wide_occupies_new_page_if_payload_fully_touched"
        ]

extra = sum(mapsize(A + n, True) != mapsize(A + n, False) for n in range(A))
assert extra == record["uniform_residue_window"]["extra_mapping_residues"] == 16
print(json.dumps({
    "status": "arithmetic assertions passed",
    "examples": len(record["all_huge_tail_geometry_examples"]),
    "additional_mapping_residues": extra,
    "residues": A,
    "runtime_executed": False,
    "timing_measured": False,
}, indent=2))

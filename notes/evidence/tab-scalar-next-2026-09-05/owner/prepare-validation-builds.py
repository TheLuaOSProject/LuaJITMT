from pathlib import Path
import hashlib, json, shutil

p = Path(__file__).resolve().parent
source = p/'candidate'
identity = json.loads((p/'source-identity.json').read_text())
base = json.loads((p/'base-inputs.json').read_text())
sha = lambda f: hashlib.sha256(Path(f).read_bytes()).hexdigest()
for variant in ['optimized', 'asan']:
    tree = p/variant
    assert not tree.exists()
    tree.mkdir()
    for name, values in identity['sources'].items():
        assert sha(source/name) == values['candidate_sha256'], name
        dest = tree/name
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source/name, dest)
    for name, expected in base['metadata'].items():
        assert sha(source/name) == expected
        shutil.copy2(source/name, tree/name)
print('prepared optimized GC2-helper and target-ASan source-identical trees')

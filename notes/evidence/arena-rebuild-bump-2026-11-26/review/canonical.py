from pathlib import Path
import hashlib, json, os, resource, subprocess, time

resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
p = Path(__file__).resolve().parent
root = Path('/workspaces/lj-lockless')
out = p/'canonical'; out.mkdir(exist_ok=False)
meta = json.loads((p/'gcc/build.json').read_text())
src = {k:v for k,v in meta['source_inputs'].items() if k == 'Makefile' or k.startswith(('src/', 'dynasm/'))}
(out/'inputs.json').write_text(json.dumps(src, indent=2)+'\n')
extra = {'LUA_PATH': str(p/'gcc/src/?.lua')+';;', 'JOBS': '6'}
env = os.environ.copy(); env.update(extra)
gates = ['m2_arena_rebuild_bump', 'm2_arena_bin_capacity', 'm2_arena_plain_shrink',
    'm2_arena_plain_progress', 'm2_arena_alloc', 'm2_arena_plain_late',
    'm2_arena_hugetab', 'm2_arena_publication', 'm2_arena_sweep',
    'm2_arena_gcsweep', 'm2_arena_empty_reclaimed_runtime', 'm2_arena_gcphase']
cmd = ['taskset', '-c', '0-7', str(p/'gcc/src/luajit'), 'tools/test.lua', *gates]
t = time.monotonic()
with (out/'stdout').open('wb') as so, (out/'stderr').open('wb') as se:
    try: code = subprocess.run(cmd, cwd=root, env=env, stdout=so, stderr=se, timeout=360).returncode
    except subprocess.TimeoutExpired: code = 124
result = dict(command=cmd, gates=gates, exit=code, seconds=time.monotonic()-t,
    environment=extra, outputs={name:hashlib.sha256((root/'src'/name).read_bytes()).hexdigest()
    for name in ['luajit', 'libluajit.a', 'libluajit.so']},
    source_mismatches=[k for k,v in src.items() if hashlib.sha256((root/k).read_bytes()).hexdigest() != v])
(out/'result.json').write_text(json.dumps(result, indent=2)+'\n')
print(json.dumps(result, indent=2), flush=True)
assert code == 0 and not result['source_mismatches']

from pathlib import Path
import hashlib
import json
import os
import resource
import subprocess
import time

resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
root = Path(__file__).resolve().parent
repo = Path('/workspaces/lj-lockless')
temp = root/'canonical-tmp'
temp.mkdir()
env = {'LJ_TEST_ROOT': str(repo), 'JOBS': '4', 'MAKE_JOBS': '4',
       'TMPDIR': str(temp), 'LUA_PATH': str(repo/'src/?.lua')+';;'}
command = ['taskset', '-c', '0-15', str(root/'strict/src/luajit'),
           str(repo/'tools/test.lua'), 'm3_safepoint_native_completion']
start = time.monotonic()
with (root/'canonical.stdout').open('w') as out, (root/'canonical.stderr').open('w') as err:
    p = subprocess.run(command, cwd=repo, env=dict(os.environ, **env),
                       stdout=out, stderr=err, timeout=240)
(root/'canonical.json').write_text(json.dumps(dict(
    command=command, cwd=str(repo), environment=env, exit=p.returncode,
    seconds=time.monotonic()-start, final_source={
        str(path): hashlib.sha256((repo/path).read_bytes()).hexdigest()
        for path in ['src/lj_safepoint.c', 'src/lj_safepoint.h', 'tests/suites/m3_gc.lua', 'tests/t-safepoint-remote-root-completion.c', 'tests/t-safepoint-local-native-duplicate.c', 'tests/t-safepoint-native-root-hold.c']},
    restored_binaries={name: hashlib.sha256((repo/'src'/name).read_bytes()).hexdigest()
                       for name in ['luajit', 'libluajit.a', 'libluajit.so']}), indent=2)+'\n')
print('canonical m3_safepoint_native_completion', p.returncode, flush=True)
assert p.returncode == 0

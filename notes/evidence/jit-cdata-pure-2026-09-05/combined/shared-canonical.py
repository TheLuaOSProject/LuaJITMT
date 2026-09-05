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
temp = root/'shared-canonical-tmp'
temp.mkdir()
env = {'LJ_TEST_ROOT': str(repo), 'JOBS': '4', 'MAKE_JOBS': '4',
       'TMPDIR': str(temp), 'LUA_PATH': str(repo/'src/?.lua')+';;'}
rows = []
for case in ['m6_jit_cdata_pure', 'm6_jit_cdata_pure_lifecycle']:
    command = ['taskset', '-c', '0-15', str(root/'strict/src/luajit'),
               str(repo/'tools/test.lua'), case]
    start = time.monotonic()
    with (root/(case+'.stdout')).open('w') as out, (root/(case+'.stderr')).open('w') as err:
        p = subprocess.run(command, cwd=repo, env=dict(os.environ, **env),
                           stdout=out, stderr=err, timeout=180)
    rows.append(dict(case=case, command=command, cwd=str(repo), environment=env,
                     exit=p.returncode, seconds=time.monotonic()-start))
    (root/'shared-canonical.json').write_text(json.dumps(rows, indent=2)+'\n')
    print(case, p.returncode, flush=True)
    assert p.returncode == 0
(root/'shared-binaries.json').write_text(json.dumps({
    name: hashlib.sha256((repo/'src'/name).read_bytes()).hexdigest()
    for name in ['luajit', 'libluajit.a', 'libluajit.so']}, indent=2)+'\n')

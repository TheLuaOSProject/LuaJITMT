from pathlib import Path
import hashlib
import json
import os
import resource
import subprocess
import sys
import time

resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
root = Path(__file__).resolve().parent
name = sys.argv[1]
tree = Path(sys.argv[2])
source = root/(name+'.lua')
command = ['taskset', '-c', '0-15', str(tree/'src/luajit'), '-jon', str(source)]
override = {'LJ_M7_FFI_CALLXS_FLUSH_SO': str(root/'fixture.so'),
            'LUA_PATH': str(tree/'src/?.lua')+';;'}
start = time.monotonic()
try:
    p = subprocess.run(command, cwd=tree, env=dict(os.environ, **override),
                       capture_output=True, text=True, timeout=30)
    result = dict(exit=p.returncode, stdout=p.stdout, stderr=p.stderr)
except subprocess.TimeoutExpired as exc:
    result = dict(exit=None, timeout=True,
                  stdout=(exc.stdout or b'').decode(errors='replace'),
                  stderr=(exc.stderr or b'').decode(errors='replace'))
result.update(command=command, cwd=str(tree), environment=override,
              seconds=time.monotonic()-start,
              runtime_sha256=hashlib.sha256((tree/'src/luajit').read_bytes()).hexdigest(),
              source_sha256=hashlib.sha256(source.read_bytes()).hexdigest())
(root/(name+'-'+tree.parent.name+'-result.json')).write_text(json.dumps(result, indent=2)+'\n')
print(json.dumps(result, indent=2), flush=True)

from pathlib import Path
import os, subprocess, time, json, hashlib
r = Path(__file__).resolve().parent
t = r/'debug-six'
flags = '-DLUA_USE_ASSERT -DLUA_USE_APICHECK -DLJ_GC2_TEST_HELPERS -DLJ_TRACE_TEST_HELPERS -DLJ_TAB_TEST_HELPERS -DLJ_FUNC_TEST_HELPERS -DLJ_UDATA_TEST_HELPERS -DLJ_STR_TEST_HELPERS'
argv = ['make', '-C', 'src', '-j4', 'BUILDMODE=static', 'XCFLAGS='+flags,
        'CCDEBUG=-g', 'TARGET_STRIP=:']
st = time.monotonic()
with (r/'build.stdout').open('w') as o, (r/'build.stderr').open('w') as e:
    p = subprocess.run(argv, cwd=t, stdout=o, stderr=e)
row = {'argv': argv, 'cwd': str(t), 'exit_code': p.returncode,
       'seconds': time.monotonic()-st,
       'compiler': subprocess.check_output(['cc', '--version'], text=True),
       'note': 'Same source and six helper macros as frozen initial candidate; only debug metadata enabled. Original optimization settings retained.'}
row['identities'] = {}
for f in ['src/luajit', 'src/libluajit.a', 'src/lj_gc2.o', 'src/lj_func.o']:
    fp = t/f
    if fp.exists():
        b = fp.read_bytes()
        row['identities'][f] = {'bytes': len(b), 'sha256': hashlib.sha256(b).hexdigest()}
(r/'build.json').write_text(json.dumps(row, indent=2)+'\n')
print(json.dumps({'exit_code': p.returncode, 'seconds': row['seconds']}), flush=True)
raise SystemExit(p.returncode)

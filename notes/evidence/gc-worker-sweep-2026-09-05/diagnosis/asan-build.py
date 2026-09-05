from pathlib import Path
import hashlib,json,subprocess,time
r=Path(__file__).resolve().parent
cmd=['make','-C','src','-j4','BUILDMODE=static','CC=clang','HOST_CC=clang',
     'CCOPT=-O1','CCDEBUG=-g','XCFLAGS=-DLUA_USE_ASSERT -DLUA_USE_APICHECK',
     'TARGET_CFLAGS=-fsanitize=address -fno-omit-frame-pointer',
     'TARGET_LDFLAGS=-fsanitize=address','TARGET_STRIP=:']
st=time.monotonic()
with (r/'asan-build.stdout').open('w') as o,(r/'asan-build.stderr').open('w') as e:
    c=subprocess.run(cmd,cwd=r/'asan',stdout=o,stderr=e,timeout=180)
identities={p.name:dict(sha256=hashlib.sha256(p.read_bytes()).hexdigest(),bytes=p.stat().st_size)
            for p in [r/'asan/src/luajit',r/'asan/src/libluajit.a'] if p.exists()}
result=dict(argv=cmd,cwd=str(r/'asan'),exit_code=c.returncode,seconds=time.monotonic()-st,
            binaries=identities)
(r/'asan-build.json').write_text(json.dumps(result,indent=2)+'\n')
print(json.dumps(result),flush=True)
raise SystemExit(c.returncode)

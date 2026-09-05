from pathlib import Path
import subprocess,json,time,sys,hashlib,tarfile
r=Path(__file__).resolve().parent
v=sys.argv[1];tree=r/v
if not tree.exists():
 tree.mkdir()
 with tarfile.open(r/'runtime-base.tar') as t:t.extractall(tree,filter='data')
cmd=['make','-C','src','-j2']
if v.endswith('-asan'):
 cmd+=['BUILDMODE=static','CC=clang','HOST_CC=clang','CCOPT=-O1','CCDEBUG=-g','TARGET_STRIP=:', 'XCFLAGS=-DLUA_USE_ASSERT -DLUA_USE_APICHECK','TARGET_CFLAGS=-fsanitize=address -fno-omit-frame-pointer','TARGET_LDFLAGS=-fsanitize=address']
st=time.monotonic()
with (r/(v+'-build.stdout')).open('w') as out,(r/(v+'-build.stderr')).open('w') as err:
 p=subprocess.run(cmd,cwd=tree,stdout=out,stderr=err)
d={'argv':cmd,'cwd':str(tree),'exit_code':p.returncode,'seconds':time.monotonic()-st,'binaries':{}}
for n in ['luajit','libluajit.a','libluajit.so','host/buildvm','host/minilua']:
 f=tree/'src'/n
 if f.is_file():d['binaries'][n]={'sha256':hashlib.sha256(f.read_bytes()).hexdigest(),'bytes':f.stat().st_size}
(r/(v+'-build.json')).write_text(json.dumps(d,indent=2)+'\n')
print(json.dumps(d),flush=True)
raise SystemExit(p.returncode)

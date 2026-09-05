import pathlib,subprocess,time,json,hashlib,concurrent.futures
r=pathlib.Path(__file__).resolve().parent

def build(name):
 t=r/name;cmd=['taskset','-c','0-15','make','-C',str(t/'src'),'-j4','BUILDMODE=static','CCDEBUG=-g','TARGET_STRIP=:']
 if name=='strict':cmd+=['XCFLAGS=-DLUA_USE_ASSERT']
 start=time.monotonic()
 with (r/(name+'-build.stdout')).open('w') as out,(r/(name+'-build.stderr')).open('w') as err:p=subprocess.run(cmd,cwd=t,stdout=out,stderr=err,timeout=120)
 row={'variant':name,'command':cmd,'exit':p.returncode,'seconds':time.monotonic()-start}
 if not p.returncode:row['runtime_sha256']=hashlib.sha256((t/'src/luajit').read_bytes()).hexdigest();row['archive_sha256']=hashlib.sha256((t/'src/libluajit.a').read_bytes()).hexdigest()
 return row
with concurrent.futures.ThreadPoolExecutor(max_workers=2) as ex:rows=list(ex.map(build,['tree','strict']))
(r/'build-results.json').write_text(json.dumps(rows,indent=2)+'\n');print(json.dumps(rows,indent=2),flush=True)

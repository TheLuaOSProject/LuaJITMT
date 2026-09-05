import pathlib, subprocess, time, json, hashlib, concurrent.futures
root=pathlib.Path(__file__).resolve().parent
flags={'base-normal':'','fix-normal':'','fix-assert':'-DLUA_USE_ASSERT -DLJ_XSAVE_TEST_HELPERS -DLJ_GC2_TEST_HELPERS'}
def run(name):
 tree=root/name
 cmd=['taskset','-c','0-15','make','-C',str(tree/'src'),'-j4','BUILDMODE=static','CCDEBUG=-g','TARGET_STRIP=:']
 if flags[name]: cmd.append('XCFLAGS='+flags[name])
 start=time.monotonic()
 with (root/(name+'-build.stdout')).open('w') as out,(root/(name+'-build.stderr')).open('w') as err:
  p=subprocess.run(cmd,stdout=out,stderr=err,timeout=120)
 row={'variant':name,'command':cmd,'cwd':str(root),'exit':p.returncode,'seconds':time.monotonic()-start}
 if not p.returncode:
  row['runtime_sha256']=hashlib.sha256((tree/'src/luajit').read_bytes()).hexdigest()
  row['archive_sha256']=hashlib.sha256((tree/'src/libluajit.a').read_bytes()).hexdigest()
 return row
with concurrent.futures.ThreadPoolExecutor(max_workers=2) as ex: rows=list(ex.map(run,flags))
(root/'build-results.json').write_text(json.dumps(rows,indent=2)+'\n')
print(json.dumps(rows,indent=2),flush=True)

from pathlib import Path
import subprocess,hashlib,json,time,concurrent.futures,difflib
r=Path(__file__).resolve().parent
(r/'pre-mt-cdata-method-guards-v1.patch').write_bytes((r/'pre-mt-cdata-method-guards.patch').read_bytes())
old=(r/'base-normal/src/lj_record.c').read_text()
for variant in ('fix-normal','fix-assert'):
 p=r/variant/'src/lj_record.c'; s=p.read_text(); assert s.count('  immutable_mt:\n')==1
 p.write_text(s.replace('  immutable_mt:\n',''))
new=(r/'fix-normal/src/lj_record.c').read_text()
(r/'pre-mt-cdata-method-guards.patch').write_text(''.join(difflib.unified_diff(old.splitlines(True),new.splitlines(True),fromfile='a/src/lj_record.c',tofile='b/src/lj_record.c')))
helpers='-DLUA_USE_ASSERT -DLJ_GC2_TEST_HELPERS -DLJ_TRACE_TEST_HELPERS -DLJ_TAB_TEST_HELPERS'
def build(variant):
 tree=r/variant
 cmd=['taskset','-c','0-15','make','-C',str(tree/'src'),'-j4','BUILDMODE=static','CCDEBUG=-g','TARGET_STRIP=:']
 if variant=='fix-assert':cmd+=['XCFLAGS='+helpers]
 t=time.monotonic();p=subprocess.run(cmd,cwd=tree,capture_output=True,text=True,timeout=120)
 (r/(variant+'-build-v2.stdout')).write_text(p.stdout);(r/(variant+'-build-v2.stderr')).write_text(p.stderr)
 result={'variant':variant,'command':cmd,'cwd':str(tree),'exit':p.returncode,'seconds':time.monotonic()-t,'source_sha256':hashlib.sha256((tree/'src/lj_record.c').read_bytes()).hexdigest(),'runtime_sha256':hashlib.sha256((tree/'src/luajit').read_bytes()).hexdigest(),'archive_sha256':hashlib.sha256((tree/'src/libluajit.a').read_bytes()).hexdigest()}
 print(variant,p.returncode,flush=True);return result
with concurrent.futures.ThreadPoolExecutor(max_workers=2) as ex:results=list(ex.map(build,('fix-normal','fix-assert')))
(r/'guard-build-v2-results.json').write_text(json.dumps(results,indent=2)+'\n')
assert all(x['exit']==0 for x in results)

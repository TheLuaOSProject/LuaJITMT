from pathlib import Path
import hashlib,json,subprocess,os,platform,re
r=Path(__file__).resolve().parent;repo=Path('/workspaces/lj-lockless');base='dd2c439179b1e12564710484d8511e4cee617f7f'
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def blob(p):
 b=p.read_bytes();return hashlib.sha1(b'blob '+str(len(b)).encode()+b'\0'+b).hexdigest()
entries=subprocess.check_output(['git','ls-tree','-r',base,'--','src','dynasm'],cwd=repo,text=True).splitlines();tracked={}
for line in entries:
 info,path=line.split('\t');mode,typ,oid=info.split()
 if typ=='blob':tracked[path]=oid
rows=[]
for variant in ('base-normal','fix-normal','fix-assert','canonical'):
 tree=r/variant;source={};changed=[]
 for path,oid in tracked.items():
  p=tree/path
  source[path]={'git_blob':blob(p),'sha256':sha(p),'bytes':p.stat().st_size}
  if source[path]['git_blob']!=oid:changed.append(path)
 expected=[] if variant=='base-normal' else ['src/lj_record.c']
 assert changed==expected,(variant,changed)
 generated={str(p.relative_to(tree)):sha(p) for p in sorted((tree/'src').rglob('*')) if p.is_file() and str(p.relative_to(tree)) not in tracked and p.suffix in ('.c','.h','.dasc','.lua')}
 objects={str(p.relative_to(tree)):sha(p) for p in sorted((tree/'src').rglob('*')) if p.is_file() and (p.suffix in ('.o','.a','.so') or p.name in ('luajit','buildvm','minilua'))}
 rows.append({'variant':variant,'path':str(tree),'tracked_sources':source,'changed_from_base':changed,'generated_sources':generated,'binary_objects':objects})
(r/'source-binary-manifest.json').write_text(json.dumps({'base':base,'base_tree':subprocess.check_output(['git','rev-parse',base+'^{tree}'],cwd=repo,text=True).strip(),'source_count_per_tree':len(tracked),'variants':rows},indent=2)+'\n')
finalfiles=['src/lj_record.c','tests/suites/m6_jit.lua','tests/t-jit-cdata-basemt-guards.lua']
(r/'candidate-source-hashes-final.json').write_text(json.dumps({'base':base,'files':{f:{'sha256':sha(r/'canonical'/f),'git_blob':blob(r/'canonical'/f)} for f in finalfiles}},indent=2)+'\n')
check=r/'patch-check';check.mkdir()
for f in finalfiles[:2]:p=check/f;p.parent.mkdir(parents=True,exist_ok=True);p.write_bytes((r/'base-normal'/f).read_bytes())
cmd=['git','apply','--check',str(r/'pre-mt-cdata-guards-review.patch')];p=subprocess.run(cmd,cwd=check,capture_output=True,text=True);assert p.returncode==0,p.stderr
subprocess.run(['git','apply',str(r/'pre-mt-cdata-guards-review.patch')],cwd=check,check=True)
assert all(sha(check/f)==sha(r/'canonical'/f) for f in finalfiles)
(r/'patch-verification.json').write_text(json.dumps({'command':cmd,'cwd':str(check),'exit':p.returncode,'stdout':p.stdout,'stderr':p.stderr,'patch_sha256':sha(r/'pre-mt-cdata-guards-review.patch'),'reconstructed_sha256':{f:sha(check/f) for f in finalfiles}},indent=2)+'\n')
(r/'environment.json').write_text(json.dumps({'platform':platform.platform(),'cpu_allowed':sorted(os.sched_getaffinity(0)),'compiler':subprocess.check_output(['gcc','--version'],text=True),'linker':subprocess.check_output(['ld','--version'],text=True),'make':subprocess.check_output(['make','--version'],text=True),'build_environment':{key:os.environ[key] for key in ['CC','CXX','CFLAGS','CPPFLAGS','LDFLAGS','XCFLAGS','CCOPT','CCDEBUG','JOBS','MAKEFLAGS','LUA_PATH','LUA_CPATH'] if key in os.environ},'cpu_model':[x for x in Path('/proc/cpuinfo').read_text().splitlines() if x.startswith('model name')][:1],'timing_affinity':'CPU 31, other agents used separate cores; whole host not isolated'},indent=2)+'\n')
print('verified',len(tracked),'tracked sources per tree and exact three-file patch',flush=True)

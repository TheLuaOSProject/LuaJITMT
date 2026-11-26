from pathlib import Path
import hashlib,json,resource,shutil,subprocess
resource.setrlimit(resource.RLIMIT_CORE,(0,0))
p=Path(__file__).resolve().parent
meta=json.loads((p/'metadata.json').read_text())
meta['candidate_tree']='/tmp/lj-plain-shrink-final-20261126-lxy_jr_l/gcc'
sha=lambda f:hashlib.sha256(Path(f).read_bytes()).hexdigest()
builds=[]
for variant in ['baseline','candidate']:
 tree=Path(meta[variant+'_tree']);build=json.loads((tree/'build.json').read_text())
 assert build['exit']==0
 for rel,h in build['source_inputs'].items():assert sha(tree/rel)==h,rel
 for rel,h in build['outputs'].items():assert sha(tree/'src'/rel)==h,rel
 shutil.copy2(tree/'build.json',p/(variant+'-build.json'))
 meta.setdefault('library_sha256',{})[variant]=sha(tree/'src/libluajit.a')
 meta.setdefault('luajit_sha256',{})[variant]=sha(tree/'src/luajit')
 for name in ['witness','hot','retention']:
  out=p/(variant+'-'+name)
  cmd=['taskset','-c','0','gcc','-O2','-g','-Wall','-Wextra','-Werror','-mcx16','-MMD','-MF',str(out)+'.d','-I'+str(tree/'src'),str(p/(name+'.c')),str(tree/'src/libluajit.a'),'-Wl,-E','-lm','-ldl','-pthread','-o',str(out)]
  if name=='witness':cmd+=['-Wl,--wrap=mmap','-Wl,--wrap=mmap64']
  with (p/(variant+'-'+name+'-build.stdout')).open('wb') as stdout,(p/(variant+'-'+name+'-build.stderr')).open('wb') as stderr:
   code=subprocess.run(cmd,stdout=stdout,stderr=stderr).returncode
  builds.append(dict(variant=variant,name=name,command=cmd,exit=code,binary_sha256=sha(out) if code==0 else None))
  (p/'builds.json').write_text(json.dumps(builds,indent=2)+'\n')
  assert code==0,(variant,name,code)
meta['source_sha256']={name:sha(p/name) for name in ['witness.c','hot.c','retention.c']}
meta['status']='prepared, witnesses and measurements pending'
(p/'metadata.json').write_text(json.dumps(meta,indent=2)+'\n')
print('prepared',len(builds),'harnesses')

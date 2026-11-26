from pathlib import Path
import hashlib,json,resource,shutil,subprocess,sys
resource.setrlimit(resource.RLIMIT_CORE,(0,0))
p=Path(__file__).resolve().parent
meta=json.loads((p/'metadata.json').read_text())
variant,tree=sys.argv[1],Path(sys.argv[2]).resolve()
assert variant in ('baseline','candidate')
sha=lambda f:hashlib.sha256(Path(f).read_bytes()).hexdigest()
build=json.loads((tree/'build.json').read_text())
assert build['exit']==0
for rel,expected in build['source_inputs'].items():
 assert sha(tree/rel)==expected,rel
for rel,expected in build['outputs'].items():
 assert sha(tree/'src'/rel)==expected,rel
shutil.copy2(tree/'src/luajit',p/(variant+'-luajit'))
shutil.copy2(tree/'build.json',p/(variant+'-build.json'))
meta[variant+'_tree']=str(tree)
meta['binary_sha256'][variant]=sha(p/(variant+'-luajit'))
meta['library_sha256'][variant]=sha(tree/'src/libluajit.a')
commands=[]
for name in ['census','live-growth']:
 out=p/(variant+'-'+name)
 cmd=['taskset','-c','0','gcc','-O2','-g','-Wall','-Wextra','-mcx16','-MMD','-MF',str(out)+'.d','-I'+str(tree/'src'),str(p/(name+'.c')),str(tree/'src/libluajit.a'),'-Wl,-E','-lm','-ldl','-pthread','-o',str(out)]
 with (p/(variant+'-'+name+'-build.stdout')).open('wb') as stdout,(p/(variant+'-'+name+'-build.stderr')).open('wb') as stderr:
  code=subprocess.run(cmd,stdout=stdout,stderr=stderr).returncode
 commands.append(dict(command=cmd,exit=code,binary_sha256=sha(out) if code==0 else None))
 (p/(variant+'-compile.json')).write_text(json.dumps(commands,indent=2)+'\n')
 assert code==0,(name,code)
 meta.setdefault(name.replace('-','_')+'_binary_sha256',{})[variant]=sha(out)
meta['status']='binaries ready; timing only after coordination' if variant=='candidate' else 'baseline ready; waiting for candidate'
(p/'metadata.json').write_text(json.dumps(meta,indent=2)+'\n')
print(variant,meta['binary_sha256'][variant])

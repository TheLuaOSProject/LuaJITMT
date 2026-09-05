from pathlib import Path
import subprocess,json,hashlib,difflib,shutil,os
r=Path(__file__).parent;repo=Path('/workspaces/lj-lockless');base='d680421c4cb50b85437d88255bc89358c5e3a6b1'
files=['src/lj_arena.c','src/lj_arena.h','src/lj_gc2.c','src/vm_x64.dasc','src/lj_asm_x86.h']
tracked=subprocess.check_output(['git','ls-tree','-r','--name-only',base],cwd=repo,text=True).splitlines()
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def blob(p):return hashlib.sha1(b'blob '+str(p.stat().st_size).encode()+b'\0'+p.read_bytes()).hexdigest()
def diff_from_base(tree,names):
 out=[]
 for name in names:
  old=subprocess.check_output(['git','show',base+':'+name],cwd=repo,text=True)
  new=(tree/name).read_text()
  if old!=new:out.append('diff --git a/'+name+' b/'+name+'\n'+''.join(difflib.unified_diff(old.splitlines(True),new.splitlines(True),fromfile='a/'+name,tofile='b/'+name)))
 return ''.join(out)
(r/'corrected-wide-stamp.patch').write_text(diff_from_base(r/'strict',files))
(r/'original-wide-control.patch').write_text(diff_from_base(r/'control',files))
(r/'ignore-era-control.patch').write_text(''.join(difflib.unified_diff((r/'strict/src/lj_gc2.c').read_text().splitlines(True),(r/'negative-era/src/lj_gc2.c').read_text().splitlines(True),fromfile='a/src/lj_gc2.c',tofile='b/src/lj_gc2.c')))
variants={}
for v in ['strict','normal','control','baseline-strict','negative-era']:
 t=r/v;source={p:sha(t/p) for p in tracked if (t/p).is_file()}
 binary={p.relative_to(t).as_posix():sha(p) for p in (t/'src').rglob('*') if p.is_file() and (p.suffix in ('.o','.a') or p.name in ('luajit','minilua','buildvm'))}
 generated={p.relative_to(t).as_posix():sha(p) for p in (t/'src').rglob('*') if p.is_file() and p.relative_to(t).as_posix() not in source and p.suffix in ('.h','.s')}
 variants[v]={'tracked_source_sha256':source,'generated_source_sha256':generated,'binary_sha256':binary,'five_owned_source_blobs':{p:blob(t/p) for p in files}}
base_source=variants['baseline-strict']['tracked_source_sha256']
for v, data in variants.items():
 full=data.pop('tracked_source_sha256')
 data['tracked_source_sha256_overrides']={k:h for k,h in full.items() if base_source.get(k)!=h}
 data['tracked_source_missing']=sorted(set(base_source)-set(full))
 data['resolved_tracked_manifest_sha256']=hashlib.sha256(json.dumps(full,sort_keys=True,separators=(',',':')).encode()).hexdigest()
snapshot={'base_tracked_source_sha256':base_source,'source_manifest_encoding':'Apply each variant overrides to base_tracked_source_sha256 and remove tracked_source_missing; SHA256 of compact sorted JSON yields resolved_tracked_manifest_sha256.','base':base,'root':str(r),'source_purpose':'Functional corrected AoS prototype only; no performance measurement','variants':variants,'fixture_and_driver_sha256':{p.name:sha(p) for p in r.iterdir() if p.is_file() and p.suffix in ('.c','.h','.py')},'fixture_executable_sha256':{p.name:sha(p) for p in r.iterdir() if p.is_file() and p.open('rb').read(4)==b'\x7fELF'},'mcode_sha256':{p.name:sha(p) for p in r.glob('*.mcode')}}
(r/'source-binary-snapshot.json').write_text(json.dumps(snapshot,indent=2)+'\n')
# Mechanical fixture adapters are stored as patches to exact d680 inputs.
for fn,baseline in [('t-wide-fnew.c','tests/t-jit-fnew-bump.c'),('traverse-adapter.c','tests/t-gc2-traverse.c'),('coalescing-adapter.c','tests/t-gc2-sweep-table-coalescing.c'),('fnew-invalid-allocator-control.c','tests/t-jit-fnew-bump.c')]:
 old=subprocess.check_output(['git','show',base+':'+baseline],cwd=repo,text=True);new=(r/fn).read_text()
 (r/(fn+'.patch')).write_text(''.join(difflib.unified_diff(old.splitlines(True),new.splitlines(True),fromfile=baseline,tofile=fn)))
(r/'t-wide-fnew-emitted-audit.c.patch').write_text(''.join(difflib.unified_diff((r/'t-wide-fnew.c').read_text().splitlines(True),(r/'t-wide-fnew-emitted-audit.c').read_text().splitlines(True),fromfile='t-wide-fnew.c',tofile='t-wide-fnew-emitted-audit.c')))
# Record decisive generated addresses and their complete source/object boundary.
audit={'base':base,'source_files':{p:{'sha256':sha(r/'strict'/p),'git_blob':blob(r/'strict'/p)} for p in files},'vm_checks':{},'jit_mcode_records':'fnew-emitted-audit-results.json','source_sites':[
 {'file':'src/lj_arena.h','lines':[97,98,100,106,110,118,1604,1605,1606,1607,1608,1609,1610,1623],'proof_bytes':16,'entry_bytes':32,'token_offset':16,'purpose':'Layout/stride definition and static checks'},
 {'file':'src/lj_arena.c','lines':[379,389,390],'purpose':'Private C reincarnation clears both proof halves; token generation survives'},
 {'file':'src/vm_x64.dasc','lines':[350,351,4543,4656,4724,4725],'purpose':'Both 32-byte index shifts; full TNEW reset after exact claims/token NONE'},
 {'file':'src/lj_asm_x86.h','lines':[1569,1570,1571,1572,1585,1646],'purpose':'Both 32-byte FNEW index shifts; full function/upvalue reset after exact claims/token NONE'}]}
for v in ['strict','normal']:
 audit['vm_checks'][v]={'command':['objdump','-d','--disassemble=lj_BC_TNEW',str(r/v/'src/lj_vm.o')],'object_sha256':sha(r/v/'src/lj_vm.o'),'disassembly':v+'-tnew.disasm','disassembly_sha256':sha(r/(v+'-tnew.disasm')),'instructions':{'0xf57':'SHL 5 r11','0x10ee':'SHL 5 r9','0xf5b/0x10f5':'token at +16','0x11e3':'qword state=0 at entry+0','0x11ea':'qword era=0 at entry+8'}}
(r/'corrected-emitted-audit.json').write_text(json.dumps(audit,indent=2)+'\n')
env={}
for name,cmd in [('compiler',['gcc','--version']),('host',['uname','-a']),('cpu',['lscpu'])]:
 q=subprocess.run(cmd,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True);env[name]={'command':cmd,'rc':q.returncode,'stdout':q.stdout,'stderr':q.stderr}
(r/'environment.json').write_text(json.dumps(env,indent=2)+'\n')
evidence=repo/'notes/evidence/gc-table-authority-wide-corrected-prototype-2026-09-05';evidence.mkdir(parents=True,exist_ok=True)
selected=[]
for p in r.iterdir():
 if p.is_file() and (p.suffix in ('.json','.patch','.disasm','.gdb','.mcode') or p.name in ('t-wide-tnew.c','wide-guards.h','t-wide-stamp.c','t-wide-stamp-controls.c')):
  selected.append(p)
for p in selected:shutil.copy2(p,evidence/p.name)
# Reproducible drivers only; large fixture variants are reconstructed from retained patches.
for p in r.glob('*.py'):shutil.copy2(p,evidence/p.name)
manifest={p.name:sha(p) for p in evidence.iterdir() if p.is_file() and p.name!='artifact-manifest.json'}
(evidence/'artifact-manifest.json').write_text(json.dumps({'root':str(r),'files':manifest},indent=2)+'\n')
print(json.dumps({'evidence':str(evidence),'files':len(manifest),'bytes':sum(p.stat().st_size for p in evidence.iterdir() if p.is_file()),'strict_source':audit['source_files'],'strict_runtime':{k:variants['strict']['binary_sha256'][k] for k in ['src/luajit','src/libluajit.a','src/lj_vm.o','src/lj_asm.o']}},indent=2))

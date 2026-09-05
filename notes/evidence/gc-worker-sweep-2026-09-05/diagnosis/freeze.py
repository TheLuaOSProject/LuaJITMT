from pathlib import Path
import hashlib,json,os,platform,shutil,subprocess

r=Path(__file__).resolve().parent
out=r/'evidence'
assert not out.exists()
out.mkdir()
def digest(p):return hashlib.sha256(p.read_bytes()).hexdigest()
environment={'platform':platform.platform(),'uname':list(os.uname()),
             'tools':{}}
for name,cmd in [('gcc',['cc','--version']),('clang',['clang','--version']),('gdb',['gdb','--version'])]:
    result=subprocess.run(cmd,capture_output=True,text=True,check=True)
    environment['tools'][name]={'argv':cmd,'stdout':result.stdout,'stderr':result.stderr}
(r/'environment.json').write_text(json.dumps(environment,indent=2)+'\n')
excluded={'runtime','debug','asan','asan-candidate','evidence'}
source_names=['lj_gc.c','lj_gc.h','lj_gc2.c','lj_obj.h','lj_api.c','vm_x64.dasc',
              'lj_safepoint.c','lj_thr.c','lj_arena.h','lj_tg.h','lib_threading.c','lj_trace.c']
manifest={'package':str(r),'source':'exact candidate3; no runtime repair',
          'text_artifacts':{},'binary_identities':{}}
for p in sorted(r.rglob('*')):
    rel=p.relative_to(r)
    if not p.is_file() or rel.parts[0] in excluded:continue
    data=p.read_bytes()
    entry={'sha256':hashlib.sha256(data).hexdigest(),'bytes':len(data)}
    try:
        if data.startswith(b'\x7fELF') or p.suffix=='.tar':raise UnicodeError()
        data.decode('utf-8')
    except UnicodeError:
        manifest['binary_identities'][str(rel)]=entry
        continue
    target=out/rel
    target.parent.mkdir(parents=True,exist_ok=True)
    shutil.copyfile(p,target)
    manifest['text_artifacts'][str(rel)]=entry
for name in source_names:
    p=r/'debug/src'/name
    target=out/'production-source'/name
    target.parent.mkdir(exist_ok=True)
    shutil.copyfile(p,target)
    manifest['text_artifacts'][str(target.relative_to(out))]={'sha256':digest(p),'bytes':p.stat().st_size,
                                                          'input':str(p.relative_to(r))}
for variant in ['runtime','debug','asan','asan-candidate']:
    for name in ['luajit','libluajit.a','libluajit.so','host/buildvm','host/minilua']:
        p=r/variant/'src'/name
        if p.exists():manifest['binary_identities'][str(p.relative_to(r))]={'sha256':digest(p),'bytes':p.stat().st_size}
manifest['totals']={'text_entries':len(manifest['text_artifacts']),
                    'text_bytes':sum(e['bytes'] for e in manifest['text_artifacts'].values()),
                    'binary_entries':len(manifest['binary_identities'])}
p=out/'artifact-manifest.json'
p.write_text(json.dumps(manifest,indent=2)+'\n')
print(json.dumps({'manifest':str(p),'sha256':digest(p),'totals':manifest['totals'],
                  'handoff_sha256':digest(r/'HANDOFF.md'),'proposal_sha256':digest(r/'PROPOSAL.md')}),flush=True)

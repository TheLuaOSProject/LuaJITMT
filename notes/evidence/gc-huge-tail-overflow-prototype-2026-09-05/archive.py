from pathlib import Path
import subprocess,json,hashlib,difflib
p=Path(__file__).parent;base='28de50a622e489019fa22845d6454e029b210582';parent=Path('/tmp/lj-dense-overflow-20260905-7tl6kcfk/dense-W-candidate.patch')
files=['src/lj_arena.c','src/lj_arena.h','src/lj_gc2.c','src/lj_gc2.h'];manifest={'base':base,'parent_patch_sha256':hashlib.sha256(parent.read_bytes()).hexdigest(),'variants':{}}
for variant in ['calloc','tail']:
 patch='';rows=[]
 for name in files:
  old=subprocess.check_output(['git','show',base+':'+name],cwd='/workspaces/lj-lockless').decode();new=(p/variant/name).read_text()
  patch+=''.join(difflib.unified_diff(old.splitlines(True),new.splitlines(True),fromfile='a/'+name,tofile='b/'+name))
  rows.append({'path':name,'sha256':hashlib.sha256(new.encode()).hexdigest()})
 (p/(variant+'-integrated.patch')).write_text(patch);manifest['variants'][variant]={'files':rows,'patch_sha256':hashlib.sha256(patch.encode()).hexdigest()}
patch=''
for name in files:
 old=(p/'calloc'/name).read_text();new=(p/'tail'/name).read_text()
 patch+=''.join(difflib.unified_diff(old.splitlines(True),new.splitlines(True),fromfile='a/'+name,tofile='b/'+name))
(p/'tail-vs-calloc.patch').write_text(patch)
(p/'source-manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
print(p)

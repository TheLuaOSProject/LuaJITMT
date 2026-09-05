from pathlib import Path
import subprocess,tarfile,io,difflib,json,hashlib
r=Path(__file__).resolve().parent;repo=Path('/workspaces/lj-lockless');base='dd2c439179b1e12564710484d8511e4cee617f7f';tree=r/'canonical';tree.mkdir()
with tarfile.open(fileobj=io.BytesIO(subprocess.check_output(['git','archive',base],cwd=repo))) as archive:archive.extractall(tree,filter='data')
(tree/'src/lj_record.c').write_bytes((r/'fix-normal/src/lj_record.c').read_bytes())
file='tests/t-jit-cdata-basemt-guards.lua';test=(r/'t-jit-cdata-basemt-guards.lua').read_text();(tree/file).write_text(test)
p=tree/'tests/suites/m6_jit.lua';old=p.read_text()
new=old.replace('  "m6_jit_xbar_xpoll",\n','  "m6_jit_xbar_xpoll",\n  "m6_jit_cdata_basemt_guards",\n',1)
anchor='''  add({
    name = "m6_jit_xbar_xpoll",
'''
block='''  add({
    name = "m6_jit_cdata_basemt_guards",
    description = "native cdata dispatch guards mutable base-table methods",
    run = function(t)
      build_default(t)
      runtime.luajit_script(t, "t-jit-cdata-basemt-guards.lua", nil, {
        joff = true, timeout = "20s"
      })
      runtime.luajit_script(t, "t-jit-cdata-basemt-guards.lua", nil, {
        jon = true, timeout = "20s"
      })
      print("M6 cdata base-table native method guards passed")
    end
  })

'''
assert new.count(anchor)==1;new=new.replace(anchor,block+anchor);p.write_text(new)
patch=(r/'pre-mt-cdata-method-guards.patch').read_text()
patch+=''.join(difflib.unified_diff(old.splitlines(True),new.splitlines(True),fromfile='a/tests/suites/m6_jit.lua',tofile='b/tests/suites/m6_jit.lua'))
patch+=''.join(difflib.unified_diff([],test.splitlines(True),fromfile='/dev/null',tofile='b/'+file))
(r/'pre-mt-cdata-guards-review.patch').write_text(patch)
(r/'candidate-source-hashes.json').write_text(json.dumps({'base':base,'files':{f:hashlib.sha256((tree/f).read_bytes()).hexdigest() for f in ['src/lj_record.c','tests/suites/m6_jit.lua',file]}},indent=2)+'\n')

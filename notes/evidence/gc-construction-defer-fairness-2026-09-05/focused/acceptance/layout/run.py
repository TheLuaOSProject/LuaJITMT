from pathlib import Path
import json, subprocess, hashlib, time
P=Path('/tmp/lj-reclaim-fair-pass-20260905-kw8kfdam'); A=P/'acceptance/layout'
flags='-DLUA_USE_ASSERT -DLUA_USE_APICHECK -DLJ_GC2_TEST_HELPERS -DLJ_TRACE_TEST_HELPERS -DLJ_TAB_TEST_HELPERS -DLJ_FUNC_TEST_HELPERS -DLJ_UDATA_TEST_HELPERS -DLJ_STR_TEST_HELPERS'
rows=[]; ids={}; outputs={}
for name, S in [('rejected',Path('/tmp/lj-reclaim-owner-defer-20260905-gwiiudxk/candidate')),('fair',P/'candidate')]:
 exe=A/(name+'-layout')
 for mode,argv in [('compile',['cc','-std=gnu11','-O2','-Wall','-Wextra','-Werror','-I'+str(S/'src'),*flags.split(),*(['-DFAIR_VARIANT'] if name=='fair' else []),str(A/'layout.c'),'-o',str(exe)]),('run',[str(exe)])]:
  start=time.monotonic();r=subprocess.run(argv,cwd=S,capture_output=True,timeout=60)
  stem=name+'-'+mode; (A/(stem+'.stdout')).write_bytes(r.stdout);(A/(stem+'.stderr')).write_bytes(r.stderr)
  rows.append(dict(name=stem,argv=argv,cwd=str(S),timeout_seconds=60,exit_code=r.returncode,seconds=time.monotonic()-start,stdout=stem+'.stdout',stderr=stem+'.stderr'))
  (A/'results.json').write_text(json.dumps(rows,indent=2)+'\n')
  assert r.returncode==0, stem
  if mode=='run':outputs[name]=dict(line.split('=') for line in r.stdout.decode().splitlines())
 for q in [S/'src/lj_obj.h', S/'src/lj_dispatch.h',exe]:ids[str(q)]=dict(sha256=hashlib.sha256(q.read_bytes()).hexdigest(),bytes=q.stat().st_size)
ids[str(A/'layout.c')]=dict(sha256=hashlib.sha256((A/'layout.c').read_bytes()).hexdigest(),bytes=(A/'layout.c').stat().st_size)
(A/'identities.json').write_text(json.dumps(ids,indent=2)+'\n')
same={k:outputs['fair'][k]==v for k,v in outputs['rejected'].items()}
(A/'comparison.json').write_text(json.dumps(dict(all_existing_fields_same=all(same.values()),same=same,outputs=outputs),indent=2)+'\n')
assert all(same.values())
print(json.dumps(outputs,indent=2))

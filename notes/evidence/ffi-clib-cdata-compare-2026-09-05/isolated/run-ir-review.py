from pathlib import Path
import hashlib,json,os,subprocess,time
p=Path(__file__).resolve().parent
fixture=p/'t-clib-cdata-shape.lua'
so=Path('/tmp/lj-clib-cache-regressions-20260905-741ke1nb/authority.so')
rows=[]
for variant,src in [('baseline',Path('/tmp/lj-clib-cache-root-20260905-i59mqoic/v2/candidate/src')),('candidate',p/'candidate/src')]:
    for kind in ['function','read','write','number']:
        name=variant+'-'+kind
        cmd=['taskset','-c','24',str(src/'luajit'),'-jdump=imsT',str(fixture),kind,str(so),variant]
        envadd={'LUA_PATH':str(src/'?.lua')+';;'}
        start=time.monotonic()
        r=subprocess.run(cmd,cwd=p,env={**os.environ,**envadd},capture_output=True,text=True,timeout=35)
        row={'variant':variant,'kind':kind,'command':cmd,'cwd':str(p),'environment':envadd,'seconds':time.monotonic()-start,'exit':r.returncode,'stdout':r.stdout,'stderr':r.stderr,'inputs':{str(f):hashlib.sha256(f.read_bytes()).hexdigest() for f in [fixture,so,src/'luajit',src/'jit/vmdef.lua',src/'jit/dump.lua']}}
        rows.append(row)
        f=p/('ir-v2-'+name+'.txt');assert not f.exists();f.write_text(r.stdout+r.stderr)
        print(name,r.returncode,[x for x in r.stdout.splitlines() if x.startswith('shape')],r.stderr.strip(),flush=True)
(p/'ir-review-v2-results.json').write_text(json.dumps(rows,indent=2)+'\n')
assert all(r['exit']==0 for r in rows)

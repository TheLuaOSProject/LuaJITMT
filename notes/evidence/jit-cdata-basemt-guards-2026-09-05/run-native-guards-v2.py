from pathlib import Path
import subprocess,os,time,json,hashlib
r=Path(__file__).resolve().parent;fixture=r/'t-jit-cdata-basemt-guards.lua';results=[]
for variant in ('base-normal','fix-normal','fix-assert'):
 for mode in ('call','index','newindex','missing','nonfunction','resize','replace'):
  for jitmode in (('jon','joff') if variant=='fix-normal' else ('jon',)):
   name='native-guards-v2-'+variant+'-'+mode+'-'+jitmode
   exe=r/variant/'src/luajit'; cmd=['taskset','-c','0-15',str(exe),'-'+jitmode,str(fixture),mode]
   t=time.monotonic();p=subprocess.run(cmd,cwd=r/variant,capture_output=True,text=True,timeout=30)
   (r/(name+'.stdout')).write_text(p.stdout);(r/(name+'.stderr')).write_text(p.stderr)
   results.append({'name':name,'variant':variant,'mode':mode,'jit':jitmode,'command':cmd,'cwd':str(r/variant),'exit':p.returncode,'seconds':time.monotonic()-t,'exe_sha256':hashlib.sha256(exe.read_bytes()).hexdigest(),'fixture_sha256':hashlib.sha256(fixture.read_bytes()).hexdigest()})
   print(name,p.returncode,p.stdout.strip().splitlines()[-1:] ,flush=True)
(r/'native-guards-v2-results.json').write_text(json.dumps(results,indent=2)+'\n')

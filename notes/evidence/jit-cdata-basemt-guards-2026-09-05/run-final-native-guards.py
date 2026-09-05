from pathlib import Path
import subprocess,os,time,json,hashlib
r=Path(__file__).resolve().parent;rows=[]
for variant,modes in [('base-normal',['methodlife']),('fix-normal',['methodlife','all']),('fix-assert',['methodlife','all']),('canonical',['all'])]:
 for mode in modes:
  for jitmode in ('jon','joff'):
   exe=r/variant/'src/luajit';fixture=r/'t-jit-cdata-basemt-guards.lua';cmd=['taskset','-c','0-15',str(exe),'-'+jitmode,str(fixture)]+([] if mode=='all' else [mode]);name='final-native-'+variant+'-'+mode+'-'+jitmode
   t=time.monotonic();p=subprocess.run(cmd,cwd=r/variant,capture_output=True,text=True,timeout=20)
   (r/(name+'.stdout')).write_text(p.stdout);(r/(name+'.stderr')).write_text(p.stderr)
   rows.append({'name':name,'command':cmd,'cwd':str(r/variant),'exit':p.returncode,'seconds':time.monotonic()-t,'runtime_sha256':hashlib.sha256(exe.read_bytes()).hexdigest(),'fixture_sha256':hashlib.sha256(fixture.read_bytes()).hexdigest()});print(name,p.returncode,p.stdout.strip(),p.stderr.strip(),flush=True)
(r/'final-native-results.json').write_text(json.dumps(rows,indent=2)+'\n')

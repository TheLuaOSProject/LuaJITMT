from pathlib import Path
import csv,hashlib,json,resource,subprocess,threading
resource.setrlimit(resource.RLIMIT_CORE,(0,0))
p=Path(__file__).resolve().parent
meta=json.loads((p/'metadata.json').read_text());rows=[]
source=(p/'retention.c').read_text().replace('  lj_arena_alloc_fini(&alloc);free(p);free(q);return 0;',
 '  lj_arena_alloc_rebuild_free_kind(&alloc,LJ_ARENAK_PLAIN);emit(&alloc,"rebuilt",0,0,0);\n  lj_arena_alloc_fini(&alloc);free(p);free(q);return 0;')
(p/'retention-rebuild.c').write_text(source)
for variant in ['baseline','candidate']:
 tree=Path(meta[variant+'_tree']);exe=p/(variant+'-retention-rebuild')
 cmd=['taskset','-c','0','gcc','-O2','-g','-Wall','-Wextra','-Werror','-mcx16','-I'+str(tree/'src'),str(p/'retention-rebuild.c'),str(tree/'src/libluajit.a'),'-lm','-ldl','-pthread','-o',str(exe)]
 r=subprocess.run(cmd,capture_output=True,text=True)
 (p/(variant+'-retention-rebuild-build.stdout')).write_text(r.stdout);(p/(variant+'-retention-rebuild-build.stderr')).write_text(r.stderr)
 rows.append(dict(kind='build',variant=variant,command=cmd,exit=r.returncode));assert r.returncode==0
 for size,count in [(4096,8192),(16384,2048)]:
  stem=f'rebuild-{size}-{count}-{variant}';cmd=['taskset','-c','0',str(exe),str(count),str(size)];phases=[]
  with (p/(stem+'.stderr')).open('w') as err,(p/(stem+'.stdout')).open('w') as raw:
   proc=subprocess.Popen(cmd,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=err,text=True,bufsize=1)
   timer=threading.Timer(60,proc.kill);timer.daemon=True;timer.start()
   header=proc.stdout.readline();raw.write(header);columns=next(csv.reader([header]))
   for line in proc.stdout:
    raw.write(line);raw.flush();v=next(csv.reader([line]));assert len(v)==len(columns)
    phases.append(dict(zip(columns,v)));proc.stdin.write('go\n');proc.stdin.flush()
   code=proc.wait(timeout=10);timer.cancel()
  row=dict(kind='run',variant=variant,size=size,count=count,command=cmd,exit=code,binary_sha256=hashlib.sha256(exe.read_bytes()).hexdigest(),phases=phases,stderr=(p/(stem+'.stderr')).read_text())
  rows.append(row);(p/'rebuild-control-results.json').write_text(json.dumps(rows,indent=2)+'\n');assert code==0 and not row['stderr']
  print(variant,size,phases[-2:],flush=True)

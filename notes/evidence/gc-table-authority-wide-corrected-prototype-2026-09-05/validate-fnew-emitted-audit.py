from pathlib import Path
import subprocess,json,os,time,hashlib
r=Path(__file__).parent;s=(r/'t-wide-fnew.c').read_text()
s=s.replace('    size_t stamp_zero1, stamp_zero2;', '    size_t stamp_zero1, stamp_zero2, era_zero1, era_zero2;',1)
needle='''    assert(token_check[3] < stamp_zero1 && stamp_zero1 < stamp_zero2 &&
	   stamp_zero2 < fn_header && stamp_zero2 < uv_header);'''
insert=needle+'''
    era_zero1 = find_mem_qword_zero_store(mc, len, stamp_zero2 + 1u,
                                         offsetof(LJGC2TabStamp, era), -1, NULL);
    era_zero2 = era_zero1 == (size_t)-1 ? (size_t)-1 :
      find_mem_qword_zero_store(mc, len, era_zero1 + 1u,
                               offsetof(LJGC2TabStamp, era), -1, NULL);
    assert(era_zero1 != (size_t)-1 && era_zero2 != (size_t)-1);
    assert(stamp_zero2 < era_zero1 && era_zero1 < era_zero2 &&
           era_zero2 < fn_header && era_zero2 < uv_header);
    {
      const char *path = getenv("LJ_WIDE_MCODE_FILE");
      if (path) {
        FILE *fp = fopen(path, "wb"); assert(fp);
        assert(fwrite(mc, 1, len, fp) == len); assert(fclose(fp) == 0);
        fprintf(stderr, "emitted proof reset offsets: state=%zu,%zu era=%zu,%zu header=%zu,%zu bytes=%zu\\n",
                stamp_zero1, stamp_zero2, era_zero1, era_zero2, fn_header, uv_header, len);
      }
    }'''
assert s.count(needle)==1;s=s.replace(needle,insert)
p=r/'t-wide-fnew-emitted-audit.c';p.write_text(s);t=r/'strict';ex=r/'strict-fnew-emitted-audit'
flags=['-DLJ_GC2_TEST_HELPERS','-DLJ_TAB_TEST_HELPERS','-DLJ_FUNC_TEST_HELPERS','-DLJ_TRACE_TEST_HELPERS','-DLJ_ARENA_TEST_HELPERS','-DLUA_USE_ASSERT']
cmd=['gcc','-std=gnu11','-O2','-g','-Wall','-Wextra','-Werror','-mcx16']+flags+['-I'+str(t/'src'),'-I'+str(t/'tests'),str(p),str(t/'src/libluajit.a'),'-lm','-ldl','-pthread','-o',str(ex)]
rows=[]
q=subprocess.run(cmd,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True);rows.append({'kind':'compile','command':cmd,'rc':q.returncode,'stdout':q.stdout,'stderr':q.stderr})
if q.returncode==0:
 for cell in ('1536','1537'):
  blob=r/('fnew-'+cell+'.mcode');env=os.environ.copy();env['LJ_WIDE_MCODE_FILE']=str(blob);cmd=['taskset','-c','0-15',str(ex),cell];start=time.monotonic();q=subprocess.run(cmd,env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,timeout=40)
  rows.append({'kind':'run','command':cmd,'env':{'LJ_WIDE_MCODE_FILE':str(blob)},'rc':q.returncode,'elapsed':time.monotonic()-start,'stdout':q.stdout,'stderr':q.stderr})
  if q.returncode==0:
   cmd=['objdump','-b','binary','-m','i386:x86-64','-D',str(blob)];d=subprocess.run(cmd,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
   (r/('fnew-'+cell+'.disasm')).write_text(d.stdout)
   rows.append({'kind':'disassemble','command':cmd,'rc':d.returncode,'stdout_file':'fnew-'+cell+'.disasm','stderr':d.stderr,'mcode_sha256':hashlib.sha256(blob.read_bytes()).hexdigest()})
(r/'fnew-emitted-audit-results.json').write_text(json.dumps(rows,indent=2)+'\n')
for row in rows: print(row)

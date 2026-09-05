import gdb,json
seen=False

def state(L):
 g=L['glref']['ptr64'].cast(gdb.lookup_type('global_State').pointer())
 return dict(L=str(L),g=str(g),mt_shutdown=int(g['mt_shutdown']),n_threads=int(g['gc2']['n_threads']),n_workers=int(g['gc2']['n_workers']),worker_active=int(g['gc2']['worker_active']),phase=int(g['gc2']['phase']))

class ClaimReturn(gdb.FinishBreakpoint):
 def __init__(self,frame,L):
  super().__init__(frame,internal=True);self.L=L
 def stop(self):
  row=state(self.L);row.update(tag='close-claim-return',return_value=int(self.return_value),backtrace=gdb.execute('bt 8',to_string=True))
  print('WITNESS '+json.dumps(row),flush=True)
  return True

class ClaimEntry(gdb.Breakpoint):
 def stop(self):
  global seen
  frame=gdb.newest_frame();L=frame.read_var('L');row=state(L)
  if row['mt_shutdown']!=1:return False
  row.update(tag='close-claim-entry',backtrace=gdb.execute('bt 8',to_string=True))
  print('WITNESS '+json.dumps(row),flush=True)
  seen=True;self.enabled=False;ClaimReturn(frame,L)
  return False
ClaimEntry('lj_thr_main_close_claim',internal=True)

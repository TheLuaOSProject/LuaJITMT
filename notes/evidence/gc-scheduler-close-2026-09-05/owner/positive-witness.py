import gdb,json
selected=None
claim_return=None
close_state_seen=False
gc2_fini_seen=False

def state(L):
 g=L['glref']['ptr64'].cast(gdb.lookup_type('global_State').pointer())
 return dict(L=str(L),g=str(g),mt_shutdown=int(g['mt_shutdown']),n_threads=int(g['gc2']['n_threads']),n_workers=int(g['gc2']['n_workers']),worker_active=int(g['gc2']['worker_active']),phase=int(g['gc2']['phase']))

class RealCloseReturn(gdb.FinishBreakpoint):
 def __init__(self,frame,row):super().__init__(frame,internal=True);self.row=row
 def stop(self):
  row=dict(self.row);row.update(tag='real-close-returned',claim_return=claim_return,close_state_entered=close_state_seen,gc2_fini_entered=gc2_fini_seen,backtrace=gdb.execute('bt 6',to_string=True))
  print('WITNESS '+json.dumps(row),flush=True)
  return True

class RealCloseEntry(gdb.Breakpoint):
 def stop(self):
  global selected
  frame=gdb.newest_frame();bt=gdb.execute('bt 6',to_string=True)
  if 'test_multistate_terminal_tg_reclaim' not in bt:return False
  L=frame.read_var('L');row=state(L);selected=dict(L=int(L),g=int(L['glref']['ptr64']))
  row.update(tag='real-close-entry',backtrace=bt)
  print('WITNESS '+json.dumps(row),flush=True)
  RealCloseReturn(frame,dict(L=row['L'],g=row['g']))
  self.enabled=False
  return False

class ClaimReturn(gdb.FinishBreakpoint):
 def __init__(self,frame,L):super().__init__(frame,internal=True);self.L=L
 def stop(self):
  global claim_return
  claim_return=int(self.return_value);row=state(self.L);row.update(tag='restored-close-claim-return',return_value=claim_return)
  print('WITNESS '+json.dumps(row),flush=True)
  return False

class ClaimEntry(gdb.Breakpoint):
 def stop(self):
  frame=gdb.newest_frame();L=frame.read_var('L')
  if selected is None or int(L)!=selected['L']:return False
  ClaimReturn(frame,L);self.enabled=False
  return False

class CloseState(gdb.Breakpoint):
 def stop(self):
  global close_state_seen
  L=gdb.newest_frame().read_var('L')
  if selected is None or int(L)!=selected['L']:return False
  close_state_seen=True;row=state(L);row.update(tag='close-state-entry')
  print('WITNESS '+json.dumps(row),flush=True)
  return False

class GC2Fini(gdb.Breakpoint):
 def stop(self):
  global gc2_fini_seen
  g=gdb.newest_frame().read_var('g')
  if selected is None or int(g)!=selected['g']:return False
  gc2_fini_seen=True
  print('WITNESS '+json.dumps(dict(tag='gc2-fini-entry',g=str(g))),flush=True)
  return False
RealCloseEntry('lua_close',internal=True)
ClaimEntry('lj_thr_main_close_claim',internal=True)
CloseState('close_state',internal=True)
GC2Fini('lj_gc2_fini',internal=True)

set pagination off
set confirm off
set print elements 12
set print pretty on
p/x diag_abort_rdi
set $diag_g = (global_State*)diag_abort_rdi
p $diag_g->gc2
p diag_prepare_count
p diag_prepare_calls[0]@diag_prepare_count
p diag_ready_count
p diag_ready_calls[0]@diag_ready_count
thread apply all bt full
python
import gdb
for th in gdb.selected_inferior().threads():
 th.switch()
 f=gdb.newest_frame()
 while f:
  if f.name() == "diag_abort_observe":
   f.select()
   gdb.execute("info registers rbx rdi fs_base")
   gdb.execute("x/gx $fs_base-120")
   break
  f=f.older()
end
detach
quit

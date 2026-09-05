set pagination off
set confirm off
set print elements 12
set print pretty on
set disable-randomization off
handle SIGUSR2 nostop noprint pass
start
handle SIGTRAP nostop noprint pass
break gc2_idle_transition_handshake.part.0.isra.0.cold
continue
info registers rdi rbx rip fs_base
set $diag_g = (global_State*)$rdi
p $diag_g
p $diag_g->gc2
x/gx $fs_base-120
thread apply all bt full
quit

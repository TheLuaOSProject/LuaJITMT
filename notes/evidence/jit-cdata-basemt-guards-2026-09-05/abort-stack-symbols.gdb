set pagination off
set confirm off
break lj_trace_err_info
commands
silent
printf "RECORDER NYI error=%u\n", $esi
bt 10
continue
end
run

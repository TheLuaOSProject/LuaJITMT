set pagination off
set confirm off
set print pretty off
break lj_trace_err_info
commands
silent
printf "RECORDER NYI error=%d\n", e
bt 9
continue
end
run

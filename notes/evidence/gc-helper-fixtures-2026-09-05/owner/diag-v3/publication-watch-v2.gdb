set pagination off
set confirm off
set print frame-arguments all
break *diagnose_phase
run
set $gc = (global_State *) $rdi
printf "watch-start phase=%u filtered=%llu pushed=%llu\n", $gc->gc2.phase, $gc->gc2.remembered_filtered, $gc->gc2.remembered_pushed
disable 1
watch -location $gc->gc2.remembered_filtered
commands
silent
printf "filtered-event phase=%u filtered=%llu pushed=%llu\n", $gc->gc2.phase, $gc->gc2.remembered_filtered, $gc->gc2.remembered_pushed
bt 12
continue
end
continue

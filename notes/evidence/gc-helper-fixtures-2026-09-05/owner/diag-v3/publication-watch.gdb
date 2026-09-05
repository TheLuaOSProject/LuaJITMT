set pagination off
set confirm off
set print frame-arguments all
break /tmp/lj-gc-helper-assertions-20260905-z9obuha4/t-gc2-alloc-account.c:927
run
set $gc = g
printf "watch-start phase=%u filtered=%llu pushed=%llu\n", $gc->gc2.phase, $gc->gc2.remembered_filtered, $gc->gc2.remembered_pushed
watch -location $gc->gc2.remembered_filtered
commands
silent
printf "filtered-event phase=%u filtered=%llu pushed=%llu\n", $gc->gc2.phase, $gc->gc2.remembered_filtered, $gc->gc2.remembered_pushed
bt 12
continue
end
continue

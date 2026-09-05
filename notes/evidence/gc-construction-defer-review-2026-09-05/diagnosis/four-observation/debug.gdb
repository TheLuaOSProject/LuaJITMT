set pagination off
set confirm off
set print elements 20
source /tmp/lj-func-construction-timeout-20260905-8htcwnmd/four-observation/inspect.py
run
python snapshot('interrupted')
thread apply all bt full

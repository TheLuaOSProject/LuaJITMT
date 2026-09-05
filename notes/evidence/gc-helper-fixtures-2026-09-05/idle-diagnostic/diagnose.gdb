set pagination off
set confirm off
set print pretty on
python
import os, signal, threading
threading.Timer(3.0, lambda: os.kill(os.getpid(), signal.SIGINT)).start()
end
run
thread apply all bt
info threads
quit

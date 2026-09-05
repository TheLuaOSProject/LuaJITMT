set pagination off
set confirm off
set disable-randomization off
python
import threading, os, signal, time
def interrupt_later():
    time.sleep(1)
    os.kill(os.getpid(), signal.SIGINT)
threading.Thread(target=interrupt_later, daemon=True).start()
end
run
thread apply all bt
quit

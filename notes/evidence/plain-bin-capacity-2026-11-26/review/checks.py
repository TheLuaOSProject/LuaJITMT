from pathlib import Path
import concurrent.futures, json, resource, subprocess, sys, time
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
p = Path(__file__).resolve().parent
stage = sys.argv[1]
if stage == 'core':
    jobs = [(m, phase) for m in ['assert', 'asan', 'tsan']
            for phase in ['shrink', 'readers', 'plain', 'arena'] if not (m=='assert' and phase=='arena')]
elif stage == 'broad':
    jobs = [(m, phase) for m in ['gcc', 'assert', 'asan']
            for phase in ['matrix', 'smoke', 'public']]
    jobs += [(m, phase) for m in ['assert', 'asan']
             for phase in ['progress', 'lease', 'traverse']]
    jobs += [('tsan', phase) for phase in ['progress', 'lease', 'public']]
    jobs += [(m, 'extra') for m in ['gcc', 'assert', 'asan', 'tsan', 'noffi', 'nojit', 'nobuf']]
else:
    raise ValueError(stage)
def run(job):
    mode, phase = job
    cmd = [sys.executable, str(p / ('extra.py' if phase == 'extra' else 'validate.py')), mode]
    if phase != 'extra': cmd.append(phase)
    begin = time.monotonic()
    with (p / (stage + '-' + mode + '-' + phase + '.log')).open('wb') as f:
        proc = subprocess.run(['taskset','-c','0-7',*cmd], stdout=f, stderr=subprocess.STDOUT)
    row = dict(mode=mode, phase=phase, command=cmd, exit=proc.returncode,
               seconds=time.monotonic()-begin)
    print(mode, phase, proc.returncode, round(row['seconds'], 3), flush=True)
    return row
rows = []
with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
    for row in pool.map(run, jobs):
        rows.append(row)
        (p / (stage + '-results.json')).write_text(json.dumps(rows, indent=2)+'\n')
sys.exit(any(r['exit'] != 0 for r in rows))

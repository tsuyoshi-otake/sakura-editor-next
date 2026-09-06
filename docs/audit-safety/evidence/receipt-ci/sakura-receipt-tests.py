import argparse
import json
from pathlib import Path
import subprocess
import time

root = Path.cwd().resolve()
assert (root / 'rust/native/Cargo.toml').is_file(), 'Run from the repository root'
out = root / '.codex/goal-loop/audit-safety/receipt-ci'
out.mkdir(exist_ok=True)
p = argparse.ArgumentParser()
p.add_argument('label')
p.add_argument('--repeat', type=int, default=1)
p.add_argument('--serial', action='store_true')
p.add_argument('--debug', action='store_true')
p.add_argument('--filter')
a = p.parse_args()
cmd = ['cargo', '+1.96.0', 'test', '--workspace', '--locked']
if not a.debug:
    cmd.append('--release')
cmd += ['--no-fail-fast']
if a.filter:
    cmd.append(a.filter)
cmd += ['--', '--nocapture']
if a.serial:
    cmd.append('--test-threads=1')
records = []
for index in range(a.repeat):
    start = time.monotonic()
    log = out / f'{a.label}-{index + 1}.log'
    with log.open('wb') as stream:
        child = subprocess.Popen(cmd, cwd=root / 'rust/native', stdout=stream,
                                 stderr=subprocess.STDOUT,
                                 creationflags=subprocess.CREATE_NO_WINDOW)
        try:
            code = child.wait(timeout=240)
        except subprocess.TimeoutExpired:
            subprocess.run(['taskkill', '/PID', str(child.pid), '/T', '/F'], check=False,
                           stdout=subprocess.DEVNULL, timeout=20)
            child.wait(timeout=20)
            code = 124
    audit = subprocess.run(['pwsh', '-NoProfile', '-Command',
        "$r='" + (root / 'rust/native/target').as_posix().replace("'", "''") + "/'; "
        "@(Get-CimInstance Win32_Process | Where-Object { "
        "($_.ExecutablePath -replace '\\\\','/') -like ($r+'*') } | "
        "Select-Object ProcessId,ParentProcessId,Name,ExecutablePath) | ConvertTo-Json -Compress"],
        capture_output=True, text=True, encoding='utf-8', check=True, timeout=20)
    survivors = json.loads(audit.stdout) if audit.stdout.strip() else []
    record = dict(command=cmd, exit=code, elapsed_seconds=round(time.monotonic()-start, 3),
                  log=log.name, survivors=survivors)
    records.append(record)
    print(json.dumps(record), flush=True)
    (out / f'{a.label}-receipt.json').write_text(json.dumps(records, indent=2), encoding='utf-8')
    if survivors:
        raise RuntimeError('Owned Rust test survivors require cleanup')
    if code:
        raise SystemExit(code)

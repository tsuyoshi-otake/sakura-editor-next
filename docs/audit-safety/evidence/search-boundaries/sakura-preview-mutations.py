from pathlib import Path
import subprocess, hashlib, json, time

root = Path('C:/Users/developer/tmp/sakura-audit-safety')
out = root / '.codex/goal-loop/audit-safety'
source = root / 'sakura_core/workbench/search/WorkspaceSearchEngine.cpp'
original = source.read_bytes()
sha = lambda b: hashlib.sha256(b).hexdigest()
receipt = {'baseHead': subprocess.check_output(['git','rev-parse','HEAD'],cwd=root,text=True).strip(), 'originalSha256':sha(original), 'runs':[]}
def run(args, label, timeout=300):
    start = time.time()
    with (out / (label + '-driver.log')).open('w',encoding='utf-8') as log:
        p = subprocess.Popen(args,cwd=root,stdout=log,stderr=subprocess.STDOUT)
        try:
            code = p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            subprocess.run(['taskkill','/PID',str(p.pid),'/T','/F'],capture_output=True)
            p.wait()
            raise
    receipt['runs'].append({'command':args,'label':label,'exit':code,'pid':p.pid,'seconds':time.time()-start,'sourceSha256':sha(source.read_bytes())})
    (out / 'preview-mutation.json').write_text(json.dumps(receipt,indent=2),encoding='utf-8')
    print(label, code, flush=True)
    return code
def build(label):
    assert run(['py','-3','tools/build/sakura_build.py','build','solution','x64','Debug','--jobs','4'],label)==0
def tests(label):
    return run(['pwsh','-NoProfile','-File','C:/Users/developer/tmp/sakura-audit-run-tests.ps1','-Label',label,'-Filter','SearchRequestSafetyTest.*'],label,150)
mutations = {
    'preview-follow-hit': (b'start = std::max(start, matchOffset - context);', b'start = start; // Deliberate runtime mutant.'),
    'preview-start-pair': (b'if (start > 0 && start < lineLength && low(line[start]) && high(line[start - 1])) --start;', b'// Deliberate runtime mutant: omit start pair guard.'),
    'preview-end-pair': (b'if (end > start && end < lineLength && high(line[end - 1]) && low(line[end])) --end;', b'// Deliberate runtime mutant: omit end pair guard.'),
}
try:
    for label,(before,after) in mutations.items():
        assert original.count(before)==1
        source.write_bytes(original.replace(before,after))
        build(label+'-build')
        code=tests(label)
        assert code==1, f'Mutant not killed with test failure: {label} exit {code}'
finally:
    source.write_bytes(original)
    receipt['restoredSha256']=sha(source.read_bytes())
    (out / 'preview-mutation.json').write_text(json.dumps(receipt,indent=2),encoding='utf-8')
    build('preview-restored-build')
    assert tests('preview-restored')==0
assert receipt['restoredSha256']==receipt['originalSha256']

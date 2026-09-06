from pathlib import Path
import subprocess,json,hashlib,time
r=Path('C:/Users/developer/tmp/sakura-audit-safety')
p=r/'src/test/cpp/tests1/workbench/SearchWorkbenchToolTest.cpp'
out=r/'.codex/goal-loop/audit-safety'
original=p.read_bytes()
record={'original_sha256':hashlib.sha256(original).hexdigest(),'runs':[]}
def run(cmd,label):
    start=time.monotonic()
    with (out/(label+'-driver.log')).open('w',encoding='utf-8') as log:
        q=subprocess.Popen(cmd,cwd=r,stdout=log,stderr=subprocess.STDOUT)
        try: code=q.wait(600)
        except subprocess.TimeoutExpired:
            subprocess.run(['taskkill','/PID',str(q.pid),'/T','/F'],capture_output=True);q.wait();raise
    record['runs'].append({'label':label,'command':cmd,'exit':code,'seconds':time.monotonic()-start})
    (out/'search-local-paint-release-receipt.json').write_text(json.dumps(record,indent=2),encoding='utf-8')
    print(label,code,flush=True)
    assert code==0,(label,code)
def build(label):
    run(['py','-3','tools/build/sakura_build.py','build','solution','x64','Release','--jobs','4'],label)
try:
    p.write_bytes(original+b'\r\n'+Path('C:/Users/developer/tmp/sakura-search-local-paint-test.txt').read_bytes())
    record['probe_sha256']=hashlib.sha256(p.read_bytes()).hexdigest()
    build('search-local-paint-release-build')
    run(['pwsh','-NoProfile','-File','C:/Users/developer/tmp/sakura-search-local-paint.ps1','-Configuration','Release'],'search-local-paint-release')
finally:
    p.write_bytes(original)
    record['restored_sha256']=hashlib.sha256(p.read_bytes()).hexdigest()
    build('search-local-paint-release-normal-build')

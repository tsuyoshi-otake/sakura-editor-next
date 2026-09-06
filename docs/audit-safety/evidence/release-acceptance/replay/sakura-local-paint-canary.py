from pathlib import Path
import subprocess,json,hashlib,difflib
r=Path('C:/Users/developer/tmp/sakura-audit-safety')
source=r/'sakura_core/workbench/search/CSearchWorkbenchTool.cpp'
test=r/'src/test/cpp/tests1/workbench/SearchWorkbenchToolTest.cpp'
original={f:f.read_bytes() for f in (source,test)}
out=r/'.codex/goal-loop/audit-safety/search-local-paint-canary'
out.mkdir(exist_ok=True)
sha=lambda b:hashlib.sha256(b).hexdigest()
receipt={'original':{str(f.relative_to(r)):sha(b) for f,b in original.items()},'runs':[]}
def save(): (out/'receipt.json').write_text(json.dumps(receipt,indent=2),encoding='utf-8')
def run(cmd,label,expected=0,timeout=300):
    with (out/(label+'.log')).open('w',encoding='utf-8') as log:
        task=subprocess.Popen(cmd,cwd=r,stdout=log,stderr=subprocess.STDOUT)
        try: code=task.wait(timeout)
        except subprocess.TimeoutExpired:
            subprocess.run(['taskkill','/PID',str(task.pid),'/T','/F'],capture_output=True);task.wait();raise
    receipt['runs'].append({'label':label,'exit':code,'pid':task.pid});save()
    assert code==expected,(label,code)
def build(label): run(['py','-3','tools/build/sakura_build.py','build','solution','x64','Debug','--jobs','4'],label)
s=original[source].decode('utf-8-sig').replace('\r\n','\n')
start=s.index('\tcase WM_PRINTCLIENT: {');end=s.index('\tcase WM_PAINT: {',start)
old=s[start:end]
new=old.replace('\t\timpl.PaintWidget(dc);','\t\timpl.PaintWidget(dc);\n\t\tFillRectangle(dc, client, RGB(255, 0, 255)); // Diagnostic sensor canary only.')
assert old!=new
changed=s[:start]+new+s[end:]
(out/'canary.patch').write_text(''.join(difflib.unified_diff(s.splitlines(True),changed.splitlines(True),fromfile='a/sakura_core/workbench/search/CSearchWorkbenchTool.cpp',tofile='b/sakura_core/workbench/search/CSearchWorkbenchTool.cpp')))
driver=Path('C:/Users/developer/tmp/sakura-search-local-paint.ps1').read_text(encoding='utf-8-sig')
driver=driver.replace('search-paint-local-$Configuration','search-paint-local-canary-$Configuration')
script=Path('C:/Users/developer/tmp/sakura-search-local-paint-canary.ps1')
script.write_text(driver,encoding='utf-8-sig')
try:
    source.write_bytes(changed.encode('utf-8-sig'))
    test.write_bytes(original[test]+b'\r\n'+Path('C:/Users/developer/tmp/sakura-search-local-paint-test.txt').read_bytes())
    receipt['canary']={str(f.relative_to(r)):sha(f.read_bytes()) for f in original};save();build('build-canary')
    run(['pwsh','-NoProfile','-File',str(script)],'capture-canary',1,120)
    captures=r/'.codex/goal-loop/audit-safety/search-paint-local-canary-Debug'
    records=json.loads((captures/'measurements.json').read_text(encoding='utf-8-sig'))
    assert len(records)==30
    assert all(x['difference_percent']>0.05 and not x['pass'] for x in records)
    from PIL import Image
    receipt['screen_probe']=list(Image.open(captures/'trial-00-screen.png').getpixel((2,2)))
    receipt['current_probe']=list(Image.open(captures/'trial-00-current.png').getpixel((2,2)))
    assert receipt['current_probe'][:3]==[255,0,255]
    assert receipt['screen_probe'][:3]!=[255,0,255]
    receipt['canary_detected_trials']=len(records);save()
finally:
    for f,b in original.items(): f.write_bytes(b)
    receipt['restored']={str(f.relative_to(r)):sha(f.read_bytes()) for f in original};save();build('build-restored')

from pathlib import Path
import subprocess,hashlib,json,time,os,shutil,csv,statistics,math
r=Path('C:/Users/developer/tmp/sakura-audit-safety')
out=r/'.codex/goal-loop/audit-safety/release-performance'
out.mkdir(exist_ok=True)
loader=r/'sakura_core/io/CFileLoad.cpp'
engine=r/'sakura_core/workbench/search/WorkspaceSearchEngine.cpp'
test=r/'src/test/cpp/tests1/workbench/SearchWorkbenchToolTest.cpp'
original={f:f.read_bytes() for f in (loader,engine,test)}
sha=lambda b:hashlib.sha256(b).hexdigest()
receipt={'head':subprocess.check_output(['git','rev-parse','HEAD'],cwd=r,text=True).strip(),
         'original':{str(f.relative_to(r)):sha(b) for f,b in original.items()},'runs':[]}
def save(): (out/'receipt.json').write_text(json.dumps(receipt,indent=2),encoding='utf-8')
def run(cmd,label,env=None,timeout=600):
    start=time.monotonic()
    with (out/(label+'.log')).open('w',encoding='utf-8') as log:
        p=subprocess.Popen(cmd,cwd=r,stdout=log,stderr=subprocess.STDOUT,env=env)
        try: code=p.wait(timeout)
        except subprocess.TimeoutExpired:
            subprocess.run(['taskkill','/PID',str(p.pid),'/T','/F'],capture_output=True);p.wait();raise
    receipt['runs'].append({'label':label,'command':cmd,'exit':code,'pid':p.pid,'seconds':time.monotonic()-start})
    save();print(label,code,flush=True)
    assert code==0,(label,code)
def build(label):
    run(['py','-3','tools/build/sakura_build.py','build','solution','x64','Release','--jobs','4'],label,timeout=1200)

common=original[loader].decode('utf-8-sig').replace('\r\n','\n')
anchor='#include "window/CEditWnd.h"'
assert common.count(anchor)==1
common=common.replace(anchor,anchor+'''
#include <array>
namespace audit_performance {
unsigned long long converterCreations{};
struct Slot { CFileLoad* reader{}; bool borrowed{}; };
std::array<Slot,8> slots{};
void Register(CFileLoad* reader,bool borrowed) {
    for(auto& slot:slots) if(!slot.reader) {slot={reader,borrowed};return;}
    throw CError_FileOpen();
}
bool Release(CFileLoad* reader) {
    for(auto& slot:slots) if(slot.reader==reader) {const bool borrowed=slot.borrowed;slot={};return borrowed;}
    return false;
}
}
''')
anchor='\tm_pCodeBase.reset();'
assert common.count(anchor)==1
common=common.replace(anchor,'\tif (audit_performance::Release(this)) (void)m_pCodeBase.release();\n'+anchor)
prepare='\tm_pCodeBase.reset(CCodeFactory::CreateCodeBase(other.m_CharCode, other.m_nFlag));'
opened='\tm_pCodeBase.reset(CCodeFactory::CreateCodeBase(m_CharCode, m_nFlag));'
assert common.count(prepare)==common.count(opened)==1
common=common.replace(opened,'\t++audit_performance::converterCreations;\n'+opened)
fixed=common.replace(prepare,'\taudit_performance::Register(this,false);\n\t++audit_performance::converterCreations;\n'+prepare)
borrowed=common.replace(prepare,'\taudit_performance::Register(this,true);\n\tm_pCodeBase.reset(other.m_pCodeBase.get());')
currentEngine=original[engine].decode('utf-8-sig').replace('\r\n','\n')
oldEngine=subprocess.check_output(['git','show','afaa395c46:sakura_core/workbench/search/WorkspaceSearchEngine.cpp'],cwd=r).decode('utf-8-sig').replace('\r\n','\n')
def preview(text):
    start=text.index('void BuildPreview(')
    end=text.index('\n//! Everything the per-line matcher',start)
    return start,end,text[start:end]
start,end,_=preview(currentEngine)
oldPreview=preview(oldEngine)[2]
baselineEngine=currentEngine[:start]+oldPreview+currentEngine[end:]
probeTest=original[test]+b'\r\n'+Path('C:/Users/developer/tmp/sakura-release-performance-test.txt').read_bytes()
exe={v:r/('x64/Release/tests1-audit-perf-'+v+'.exe') for v in ('fixed','borrowed')}
for p in exe.values(): assert not p.exists(),p
receipt['comparison']='Converter-ownership-only cost reconstruction: bounded 8-slot lifetime bookkeeping in both variants; borrowed variant shares the live parent converter, is scanned serially, and all borrowers die before the parent. It is not the entire old FileLoad implementation. Preview baseline is exact baseline BuildPreview body; its distant-hit output is knowingly incorrect.'
try:
    test.write_bytes(probeTest)
    for variant,body,search in [('fixed',fixed,currentEngine),('borrowed',borrowed,baselineEngine)]:
        loader.write_bytes(body.encode('utf-8-sig'));engine.write_bytes(search.encode('utf-8-sig'))
        receipt[variant]={'sources':{str(f.relative_to(r)):sha(f.read_bytes()) for f in original}}
        build('build-'+variant)
        shutil.copyfile(r/'x64/Release/tests1.exe',exe[variant])
        receipt[variant]['executable_sha256']=sha(exe[variant].read_bytes());save()
    for pair in range(5):
        order=('fixed','borrowed') if pair%2==0 else ('borrowed','fixed')
        for variant in order:
            label=f'pair-{pair}-{variant}'
            env=os.environ.copy()
            env['SAKURA_AUDIT_PERF_OUT']=str(out/(label+'.csv'))
            env['SAKURA_AUDIT_PERF_VARIANT']=variant
            run([str(exe[variant]),'--gtest_filter=SearchRequestSafetyTest.DiagnosticReadAndPreviewCost',
                '--gtest_output=xml:'+str(out/(label+'.xml'))],label,env,120)
    run(['pwsh','-NoProfile','-File','C:/Users/developer/tmp/sakura-perf-cleanup.ps1'],'process-audit',timeout=60)
finally:
    for f,b in original.items():f.write_bytes(b)
    receipt['restored']={str(f.relative_to(r)):sha(f.read_bytes()) for f in original};save()
    build('build-restored')
    run(['pwsh','-NoProfile','-File','C:/Users/developer/tmp/sakura-audit-run-tests.ps1','-Configuration','Release',
         '-Label','release-performance-restored','-Filter','SearchRequestSafetyTest.*:FileLoadOptionsTest.*:SearchWorkbenchToolGeometry.*:ExplorerTool.ProductionWorkerDisplaysJunctionsAsLeaves',
         '-TimeoutSeconds','120'],'restored-tests',timeout=150)

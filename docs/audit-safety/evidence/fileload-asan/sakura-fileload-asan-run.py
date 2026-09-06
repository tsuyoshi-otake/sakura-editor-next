from pathlib import Path
import sys,subprocess,ctypes,shutil,json,time
r=Path('C:/Users/developer/tmp/sakura-audit-safety');out=Path('C:/Users/developer/tmp/sakura-fileload-asan')
sys.path.insert(0,str(r/'tools/build'))
from sakura_build_lib.runner import msvc_environment
env=msvc_environment(r);env['PATH']=str(out)+';'+str(r/'x64/Debug')+';'+env['PATH'];env['ASAN_OPTIONS']='halt_on_error=1'
ctypes.windll.kernel32.SetErrorMode(0x8003)
records=[]
def run(args,label,timeout):
    start=time.monotonic()
    with (out/(label+'.log')).open('w',encoding='utf-8') as f:
        p=subprocess.Popen(args,cwd=out,env=env,stdout=f,stderr=subprocess.STDOUT)
        try: code=p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            subprocess.run(['taskkill','/PID',str(p.pid),'/T','/F'],stdout=f,stderr=subprocess.STDOUT);p.wait(timeout=30);raise
    records.append(dict(label=label,args=args,pid=p.pid,exit_code=code,seconds=time.monotonic()-start))
    (out/'run-receipt.json').write_text(json.dumps(records,indent=2),encoding='utf-8')
    print(label,code,flush=True)
    return code
canary=out/'canary.cpp';canary.write_text('int main(){char* p=new char[8]; volatile int index=9; p[index]=1; delete[] p;}\n',encoding='ascii')
cl=shutil.which('cl.exe',path=env['PATH'])
assert run([cl,'/nologo','/Od','/MTd','/Zi','/fsanitize=address',str(canary),'/Fe'+str(out/'canary.exe'),'/Fo'+str(out/'canary.obj'),'/Fd'+str(out/'canary.pdb'),'/link','/INCREMENTAL:NO'],'canary-build',120)==0
assert run([str(out/'canary.exe')],'canary-run',30)!=0
assert 'heap-buffer-overflow' in (out/'canary-run.log').read_text(encoding='utf-8',errors='replace')
code=run([str(out/'tests1-asan.exe'),'--gtest_filter=FileLoadOptionsTest.*','--gtest_output=xml:'+str(out/'fileload.xml')],'fileload-run',120)
print((out/'fileload-run.log').read_text(encoding='utf-8',errors='replace')[-3000:],flush=True)
sys.exit(code)
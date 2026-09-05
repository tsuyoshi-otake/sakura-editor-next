from pathlib import Path
import sys,subprocess,ctypes,shutil,json,time,hashlib
r=Path('C:/Users/developer/tmp/sakura-audit-safety');out=Path('C:/Users/developer/tmp/sakura-fileload-faults')
sys.path.insert(0,str(r/'tools/build'))
from sakura_build_lib.runner import msvc_environment
env=msvc_environment(r);env['PATH']=str(out)+';'+str(r/'x64/Debug')+';'+env['PATH'];env['ASAN_OPTIONS']='halt_on_error=1'
ctypes.windll.kernel32.SetErrorMode(0x8003)
cl=shutil.which('cl.exe',path=env['PATH']);link=shutil.which('link.exe',path=env['PATH']);records=[]
def run(args,label,timeout=120):
    start=time.monotonic()
    with (out/(label+'.log')).open('w',encoding='utf-8') as f:
        p=subprocess.Popen(args,cwd=r/'sakura_core',env=env,stdout=f,stderr=subprocess.STDOUT)
        try: code=p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            subprocess.run(['taskkill','/PID',str(p.pid),'/T','/F'],stdout=f,stderr=subprocess.STDOUT);p.wait(timeout=30);raise
    records.append(dict(args=args,label=label,pid=p.pid,exit_code=code,seconds=time.monotonic()-start));print(label,code,flush=True);return code
source=out/'CFileLoad-injected.cpp';original=source.read_bytes()
needle=b'    m_mapping = std::make_shared<MappedFile>(pFileName, bBigFile);'
assert original.count(needle)==1
mutant=original.replace(needle,b'    closeOnFailure.release(); // MUTATION: disarm open failure cleanup.\n'+needle)
error=None
try:
    source.write_bytes(mutant)
    assert run([cl,'@'+str(out/'CFileLoad.rsp')],'mutant-compile')==0
    assert run([link,'@'+str(out/'link.rsp')],'mutant-link',300)==0
    assert run([str(out/'tests1-asan.exe'),'--gtest_filter=FileLoadOptionsTest.Injected*'],'mutant-run')==1
    log=(out/'mutant-run.log').read_text(encoding='utf-8',errors='replace')
    assert '[  FAILED  ] FileLoadOptionsTest.InjectedOpenFailuresReleaseResourcesBeforeReuse' in log
except BaseException as e:error=repr(e)
finally:source.write_bytes(original)
try:
    assert run([cl,'@'+str(out/'CFileLoad.rsp')],'restored-compile')==0
    assert run([link,'@'+str(out/'link.rsp')],'restored-link',300)==0
    assert run([str(out/'tests1-asan.exe'),'--gtest_filter=FileLoadOptionsTest.*','--gtest_output=xml:'+str(out/'restored.xml')],'restored-run')==0
except BaseException as e:error=(error or '')+' restore: '+repr(e)
(out/'mutation-receipt.json').write_text(json.dumps(dict(original_injected_source_sha256=hashlib.sha256(original).hexdigest(),mutant_source_sha256=hashlib.sha256(mutant).hexdigest(),mutation='Disarm only FileOpen closeOnFailure guard before mapping acquisition',error=error,records=records),indent=2),encoding='utf-8')
if error:raise RuntimeError(error)
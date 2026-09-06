from pathlib import Path
import sys,subprocess,shutil,json
r=Path('C:/Users/developer/tmp/sakura-audit-safety');out=Path('C:/Users/developer/tmp/sakura-fileload-asan')
sys.path.insert(0,str(r/'tools/build'))
from sakura_build_lib.runner import msvc_environment
e=msvc_environment(r);dump=shutil.which('dumpbin.exe',path=e['PATH'])
checks={}
for name in ['CFileLoad','CJis','test-file']:
    p=subprocess.run([dump,'/symbols',str(out/(name+'.obj'))],env=e,capture_output=True,text=True,errors='replace',timeout=30)
    matches=[x.strip() for x in p.stdout.splitlines() if '__asan_' in x]
    assert p.returncode==0 and matches
    checks[name]=matches[:12]
p=subprocess.run([dump,'/imports',str(out/'tests1-asan.exe')],env=e,capture_output=True,text=True,errors='replace',timeout=30)
checks['asan_runtime_import']=[x.strip() for x in p.stdout.splitlines() if 'asan' in x.lower()]
assert p.returncode==0 and checks['asan_runtime_import']
checks['owned_survivors']=0
(out/'instrumentation-checks.json').write_text(json.dumps(checks,indent=2),encoding='utf-8')
print(json.dumps(checks,indent=2))
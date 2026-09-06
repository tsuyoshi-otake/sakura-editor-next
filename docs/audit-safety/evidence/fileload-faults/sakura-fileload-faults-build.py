from pathlib import Path
import sys,subprocess,ctypes,shutil,json,time,hashlib
r=Path('C:/Users/developer/tmp/sakura-audit-safety'); out=Path('C:/Users/developer/tmp/sakura-fileload-faults')
out.mkdir(exist_ok=True)
sys.path.insert(0,str(r/'tools/build'))
from sakura_build_lib.runner import msvc_environment
env=msvc_environment(r)
cl=shutil.which('cl.exe',path=env['PATH']); linker=shutil.which('link.exe',path=env['PATH'])
parser=ctypes.windll.shell32.CommandLineToArgvW
parser.argtypes=[ctypes.c_wchar_p,ctypes.POINTER(ctypes.c_int)];parser.restype=ctypes.POINTER(ctypes.c_wchar_p)
def split(s):
    n=ctypes.c_int();p=parser('tool '+s,ctypes.byref(n));a=[p[i] for i in range(1,n.value)];ctypes.windll.kernel32.LocalFree(p);return a
records=json.loads((out/'progress.json').read_text(encoding='utf-8')) if '--link-only' in sys.argv else []
def run(args,label,limit=180):
    start=time.monotonic()
    with (out/(label+'.log')).open('w',encoding='utf-8') as f:
        p=subprocess.Popen(args,cwd=r/'sakura_core',env=env,stdout=f,stderr=subprocess.STDOUT)
        try: code=p.wait(timeout=limit)
        except subprocess.TimeoutExpired:
            subprocess.run(['taskkill','/PID',str(p.pid),'/T','/F'],stdout=f,stderr=subprocess.STDOUT);p.wait(timeout=30);raise
    records.append(dict(label=label,args=args,exit_code=code,seconds=time.monotonic()-start))
    print(label,code,flush=True)
    (out/'progress.json').write_text(json.dumps(records,indent=2),encoding='utf-8')
    return code
def compile_source(rel):
    test=rel.startswith('src/test/')
    log=r/('build/x64/Debug/tests1/tests1.tlog/CL.command.1.tlog' if test else 'build/x64/Debug/sakura_core/sakura.tlog/CL.command.1.tlog')
    lines=log.read_text(encoding='utf-16').splitlines();target=str(r/rel).replace('/','\\').upper()
    command=next(lines[i+1] for i in range(len(lines)-1) if lines[i].startswith('^') and target in lines[i][1:].split('|'))
    a=split(command)
    a=[x for x in a if not x.lower().endswith('.cpp') and not x.lower().startswith(('/yu','/yc','/fp','/fo','/fd','/rtc','/mp'))]
    obj=out/(Path(rel).stem+'.obj')
    a+=['/Y-','/fsanitize=address','/D_DISABLE_VECTOR_ANNOTATION','/D_DISABLE_STRING_ANNOTATION','/Fo'+str(obj),'/Fd'+str(out/(Path(rel).stem+'.pdb')),str(out/'CFileLoad-injected.cpp' if rel=='sakura_core/io/CFileLoad.cpp' else out/'test-file-injected.cpp' if rel=='src/test/cpp/tests1/test-file.cpp' else r/rel)]
    rsp=out/(Path(rel).stem+'.rsp');rsp.write_text(subprocess.list2cmdline(a),encoding='utf-16')
    assert run([cl,'@'+str(rsp)],'compile-'+Path(rel).stem)==0
    return obj
sources=['sakura_core/io/CFileLoad.cpp','sakura_core/io/CIoBridge.cpp']+['sakura_core/charset/'+n+'.cpp' for n in ['CCodeBase','CCodeFactory','CJis','CUtf7','CUtf8','CCodePage']]+['sakura_core/mem/'+n+'.cpp' for n in ['CMemory','CNativeW']]+['src/test/cpp/tests1/test-file.cpp']
objects=[out/(Path(p).stem+'.obj') for p in sources] if '--link-only' in sys.argv else [compile_source(p) for p in sources]
lines=(r/'build/x64/Debug/tests1/tests1.tlog/link.command.1.tlog').read_text(encoding='utf-16').splitlines()
a=split(' '.join(x for x in lines if not x.startswith('^')))
a=[x for x in a if not x.upper().startswith(('/OUT:','/PDB:','/IMPLIB:','/ILK:')) and not x.upper().endswith('TEST-FILE.OBJ')]
a+=['/OUT:'+str(out/'tests1-asan.exe'),'/PDB:'+str(out/'tests1-asan.pdb'),'/INCREMENTAL:NO']+[str(p) for p in objects]
rsp=out/'link.rsp';rsp.write_text(subprocess.list2cmdline(a),encoding='utf-16')
assert run([linker,'@'+str(rsp)],'link',300)==0
for p in (r/'x64/Debug').glob('*.dll'):shutil.copy2(p,out/p.name)
receipt=dict(head=subprocess.check_output(['git','rev-parse','HEAD'],cwd=r,text=True).strip(),instrumented_sources={p:hashlib.sha256((r/p).read_bytes()).hexdigest() for p in sources},records=records,limitations=['Other production and test objects are uninstrumented','STL container annotations disabled for mixed-object compatibility','ASan does not prove race freedom or mapping validity'])
(out/'receipt.json').write_text(json.dumps(receipt,indent=2),encoding='utf-8')
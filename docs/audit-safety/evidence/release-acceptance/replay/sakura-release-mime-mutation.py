from pathlib import Path
import subprocess, hashlib, json, time

root = Path('C:/Users/developer/tmp/sakura-audit-safety')
out = root / '.codex/goal-loop/audit-safety'
source = root / 'sakura_core/io/CFileLoad.cpp'
original = source.read_bytes()
sha = lambda b: hashlib.sha256(b).hexdigest()
record = {'head': subprocess.check_output(['git','rev-parse','HEAD'], cwd=root, text=True).strip(),
          'source': str(source.relative_to(root)), 'before_sha256': sha(original), 'runs': []}
def run(command, label, expected=0):
    start = time.monotonic()
    with (out / (label + '-driver.log')).open('w', encoding='utf-8') as log:
        p = subprocess.Popen(command, cwd=root, stdout=log, stderr=subprocess.STDOUT)
        try:
            code = p.wait(600)
        except subprocess.TimeoutExpired:
            subprocess.run(['taskkill','/PID',str(p.pid),'/T','/F'], capture_output=True)
            p.wait()
            raise
    record['runs'].append({'label': label, 'command': command, 'exit': code,
                          'seconds': time.monotonic()-start, 'source_sha256': sha(source.read_bytes())})
    (out/'release-mime-mutation.json').write_text(json.dumps(record,indent=2),encoding='utf-8')
    print(label,code,flush=True)
    assert code == expected, (label, code, expected)
def build(label):
    run(['py','-3','tools/build/sakura_build.py','build','solution','x64','Debug','--jobs','4'], label)
def test(label, expected):
    run(['pwsh','-NoProfile','-File','C:/Users/developer/tmp/sakura-audit-run-tests.ps1',
         '-Label',label,'-Filter','FileLoadOptionsTest.*','-TimeoutSeconds','120'],label,expected)

old = b'\tm_nFlag = nFlag;\r\n\tm_pCodeBase.reset(CCodeFactory::CreateCodeBase(m_CharCode, m_nFlag));'
assert original.count(old) == 1
mutant = original.replace(old, b'\tm_pCodeBase.reset(CCodeFactory::CreateCodeBase(m_CharCode, m_nFlag));\r\n\tm_nFlag = nFlag;')
record['mutant_sha256'] = sha(mutant)
try:
    source.write_bytes(mutant)
    build('release-mime-mutant-build')
    test('release-mime-mutant',1)
finally:
    source.write_bytes(original)
    record['restored_sha256'] = sha(source.read_bytes())
    build('release-mime-restored-build')
    test('release-mime-restored',0)

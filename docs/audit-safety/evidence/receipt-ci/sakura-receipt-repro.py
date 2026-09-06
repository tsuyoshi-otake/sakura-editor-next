from pathlib import Path
import hashlib
import json
import subprocess

root = Path.cwd().resolve()
source = root / 'rust/native/sakura_native_ffi/src/output_provider.rs'
before = source.read_bytes()
assert hashlib.sha256(before.replace(b'\r\n', b'\n')).hexdigest() == hashlib.sha256(
    subprocess.check_output(['git', 'show', '8d35fa298987f38b01767d22a275dcc828f1376a:rust/native/sakura_native_ffi/src/output_provider.rs'])
).hexdigest(), 'Reproduce only from the frozen pre-fix source, not the fixed test suite'
text = before.decode('utf-8')
name = 'fn provider_snapshot_write_rejects_each_mutated_semantic_receipt_field_without_consuming_it()'
start = text.index(name)
position = text.index('        assert!(info.encoded_size > 0);', start) + len('        assert!(info.encoded_size > 0);')
newline = '\r\n' if '\r\n' in text else '\n'
injected = text[:position] + newline + '        provider_snapshot_write_rejects_unmeasured_and_same_size_mutations();' + text[position:]
out = root / '.codex/goal-loop/audit-safety/receipt-ci'
out.mkdir(exist_ok=True)
try:
    source.write_bytes(injected.encode('utf-8'))
    result = subprocess.run(['py', '-3', '-X', 'utf8', str(Path(__file__).with_name('sakura-receipt-tests.py')),
        'forced-eviction-red', '--filter',
        'output_provider::tests::provider_snapshot_write_rejects_each_mutated_semantic_receipt_field_without_consuming_it'], check=False)
    receipt = dict(original_sha256=hashlib.sha256(before).hexdigest(),
                   injected_sha256=hashlib.sha256(source.read_bytes()).hexdigest(), exit=result.returncode,
                   schedule='After victim measure, execute the existing same-size mutation / 64-receipt eviction test to completion before victim write.')
finally:
    source.write_bytes(before)
receipt['restored_sha256'] = hashlib.sha256(source.read_bytes()).hexdigest()
assert receipt['restored_sha256'] == receipt['original_sha256']
(out / 'forced-eviction-source.json').write_text(json.dumps(receipt, indent=2), encoding='utf-8')
print(json.dumps(receipt))
assert receipt['exit'] != 0, 'Forced eviction must reproduce the assertion failure'

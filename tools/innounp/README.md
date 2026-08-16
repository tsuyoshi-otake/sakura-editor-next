# innounp (test/packaging only)

`innounp.exe` unpacks Inno Setup installers so `tools/verify_runtime_artifact_identity.py`
can hash the payload `bregonig.dll` and `migemo.dll`. It is not shipped with
Sakura Editor NEXT.

- Version: 2.70.1 (2026-07-21)
- SHA-256: see `PIN.json`
- 7-Zip cannot open Inno Setup 6 setup executables as archives

Do not install this from a package feed in CI. The committed binary is the
provider, the same way ctags comes from a committed archive.

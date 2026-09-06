"""Copy recorded local diagnostic harnesses with explicit checkout/tmp paths."""
from pathlib import Path
import argparse
import hashlib
import json

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--checkout", type=Path, required=True)
parser.add_argument("--destination", type=Path, required=True)
args = parser.parse_args()
checkout = args.checkout.resolve()
destination = args.destination.resolve()
if not (checkout / "sakura.sln").is_file():
    parser.error("checkout must contain sakura.sln")
for path in (checkout, destination):
    if any(character in path.as_posix() for character in ("'", "\n", "\r", '`', '$')):
        parser.error("path contains a character unsupported by these recorded harnesses")
if destination.exists() and any(destination.iterdir()):
    parser.error("destination must be empty")
destination.mkdir(parents=True, exist_ok=True)
(checkout / ".codex/goal-loop/audit-safety").mkdir(parents=True, exist_ok=True)
manifest = {}
for source in sorted(Path(__file__).parent.iterdir()):
    if source.name == Path(__file__).name or not source.is_file():
        continue
    text = source.read_text(encoding="utf-8-sig")
    text = text.replace("C:/Users/developer/tmp/sakura-audit-safety", checkout.as_posix())
    text = text.replace("C:/Users/developer/tmp", destination.as_posix())
    data = text.encode("utf-8-sig" if source.suffix == ".ps1" else "utf-8")
    target = destination / source.name
    target.write_bytes(data)
    manifest[source.name] = hashlib.sha256(data).hexdigest()
(destination / "prepared-harnesses.json").write_text(
    json.dumps({"checkout": str(checkout), "files": manifest}, indent=2), encoding="utf-8")
print(f"Prepared {len(manifest)} harness files in {destination}; no harness was executed.")

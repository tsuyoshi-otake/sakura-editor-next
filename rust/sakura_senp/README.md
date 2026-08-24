# Sakura Extension Package (`.senp`)

SENP is Sakura Editor NEXT's native extension package. It borrows the useful
package ergonomics of VS Code extensions, including a manifest and a Markdown
README, but it is intentionally not a VSIX container and does not implement the
VS Code Extension API or OpenVSX protocol.

## Version 1 archive

A `.senp` file is a deterministic ZIP archive. Every path uses `/`, every file
name is UTF-8, and these entries are required:

| Path | Purpose |
|---|---|
| `senp.json` | Strict JSON manifest. Unknown or duplicate members are rejected. |
| `README.md` | UTF-8 Markdown shown as the extension's user-facing explanation. |
| `LICENSE` | UTF-8 license text for the packaged extension. |
| `module/extension.wasm` | WebAssembly component implementing the declared SENP ABI. |
| `integrity/SHA256SUMS` | Canonical, sorted SHA-256 coverage for every payload entry. |

`CHANGELOG.md`, `assets/**`, and `signature/ed25519.sig` are optional. No other
archive paths are accepted. Packages are bounded to 64 MiB compressed and
expanded, 256 entries, 32 MiB per entry, and a 100:1 compression ratio.

The manifest for the first built-in extension is representative:

```json
{
  "schemaVersion": 1,
  "id": "sakura-indent-rainbow",
  "displayName": "Indent Rainbow",
  "version": "0.1.0",
  "publisher": "sakura.builtin",
  "description": "Colors indentation levels in the active editor.",
  "engines": { "sakura": ">=0.1.0" },
  "runtime": {
    "abi": "sakura:senp/extension@1.0.0",
    "module": "module/extension.wasm"
  },
  "activationEvents": ["onStartupFinished"],
  "capabilities": ["editor.visibleText", "editor.decorations"],
  "contributes": {
    "editorDecorations": [{ "id": "sakura.indent-rainbow", "kind": "indent" }]
  }
}
```

## Trust and installation

- Built-ins are embedded into the signed application and pinned to an embedded
  archive SHA-256 before installation.
- Publisher packages require a valid Ed25519 signature from a configured key.
- A locally selected developer package may be unsigned, but installs disabled
  and requires an explicit enable decision.
- Verified payloads are extracted to immutable content-addressed directories;
  profile state selects the active digest separately and is published atomically.

The runtime receives only the capabilities declared by the ABI. Version 1 has
no filesystem, network, process, shell, environment, or native-window imports.

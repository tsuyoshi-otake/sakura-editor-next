# `sakura-senp`

This package is part of the independent `rust/senp` Cargo workspace. The
native product workspace lives under [`../../native`](../../native) and has a
separate lockfile, toolchain contract, target directory, and rebuild closure.

This crate owns SENP v1 package validation, deterministic ZIP packing, trust
policy checks, and immutable profile installation. SENP is Sakura Editor NEXT's
native `.senp` format; it is not VSIX, Open VSX, or the VS Code Extension API.

The canonical package format and development guide is
[SENP パッケージ仕様（v1）](../../docs/senp-package-format.md). Read that
document for archive layout, manifest fields, checksum/signature rules, trust
policies, `sakura-senp-tool` commands, runtime Components, and declarative
language/grammar examples.

実装上、`module/extension.wasm` は常に必要なファイルではありません。
`senp.json` に `runtime` がある package だけが
`module/extension.wasm` を必須とし、runtime を持たない declarative
language/grammar package に module があると拒否されます。

主要な公開 API は次のとおりです。

- `pack_directory` — source directory を deterministic `.senp` にする。
- `verify_package` — archive bytes、ZIP entries、checksum、manifest、trust を検証する。
- `install_package` — 検証済み archive を content-addressed profile root に公開する。
- `list_installed` / `list_uninstalled` — profile state と content を再検証して列挙する。
- `set_extension_enabled` / `uninstall_built_in` — profile の有効化と built-in tombstone を管理する。

CLI の入口は [`rust/senp/sakura_senp_tool/src/main.rs`](../sakura_senp_tool/src/main.rs)、
runtime の WIT 境界は [`rust/senp/wit/senp-extension.wit`](../wit/senp-extension.wit)
です。crate の実装変更時は正規仕様書も同時に確認してください。

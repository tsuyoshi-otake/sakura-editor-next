# Issue #18 R0/R1/R2 実装状態

最終更新: 2026-08-09

> **schema v2 baseline と PR 1B gate:** v1 baseline値は履歴資料であり、製品コードの許容負債として
> 再承認したものではない。v2 scannerはGit index上のfirst-party regular fileだけを入力にし、
> gitlink/submodule、third-party、生成物、未追跡ファイルを除外する。PR 1Bではclean exact commitから
> baselineとappend-only acceptance ledgerを受理し、常時実行する`architecture-gates` jobを追加した。

## 今回の完了範囲

Issue #18の意味的分解トラックについて、R0の再現可能な基準台帳と、R1のfinding単位の
増加禁止ラチェットをcanonical build CLIとCIへ接続した。これはEditor Coreを分割し終えたという
意味ではなく、分割前の結合を測定し、新しい結合の増加を止めるための基盤である。

- 実装: `tools/build/sakura_build_lib/semantic_inventory.py`
- CLI: `py -3 tools/build/sakura_build.py inventory semantic`
- baseline: `tools/build/baselines/editor-core-semantic.json`
- immutable ledger: `tools/build/baselines/editor-core-semantic-history/1723ccab53e597f3017a65a91da8e13cb5cae66d.json`
- CI gate: `.github/workflows/architecture-gates.yml` (`architecture-gates`)
- 実行証跡: `build/evidence/r0/editor-core-semantic.json`（生成物、通常はGit管理外）
- 単体テスト: `tools/build/tests/test_semantic_inventory.py`

## R0 baseline

2026-08-09にclean exact commit `1723ccab53e597f3017a65a91da8e13cb5cae66d` を明示的に受理し、
次の値を保存した。Git objectのLFとWindows worktreeのCRLFは同じ論理行として比較する。

| 観測項目 | baseline |
|---|---:|
| source files | 1,557 |
| source lines | 457,931 |
| include directives | 9,802 |
| findings | 12,236 |
| `GetDllShareData` | 731 |
| `GetEditDoc` | 8 |
| `GetEditWnd` | 290 |
| raw `new` / `delete` | 250 / 325 |
| `catch (...)` | 482 |
| Win32 parameter mentions (`HWND`/`WPARAM`/`LPARAM`) | 4,489 |
| private-include hints | 45 |
| tests1/monolith link hints | 12 |
| filtered-test hints | 28 |
| semantic hotspots | 25 |

source fingerprintは `sha256:88b8437d96ff4afa7e80db6d658cdc0c14471661cfe632bdd70393b84d706270`、
scanner hashは `sha256:df148aa51c3495ca5d112d9a5c2cb53748afd4b605679815708e57bb3d0eb8ff`である。
ledgerは旧baseline `sha256:72f9e42e0e340bd5f7f645176804b0742172c9374f2c0d31b7d5fd6d40f46ad5`から
新baseline `sha256:3f87e1bc6a9de27d9debfd3f55d1efa866fddd26de70b32eef6fae6749f237b5`への受理を記録する。

## R1 ratchet

通常の検証は次で行う。

```cmd
py -3 tools/build/sakura_build.py --format json inventory semantic --strict
```

この実行は成功（終了コード0）し、baseline比の増加は空集合だった。`architecture-gates`は
PR、`main`/`develop`へのpush、手動実行でpath filterなしにこのstrict検証、`generate --check`、
`graph check --all-contexts`をfail-closedで実行する。source file/line/include数は診断値であり、
ソース追加だけを理由に分割作業を止めない。

台帳はAST、型の所有権、実行時の共有状態、プロトコル互換性を証明しない。正規表現による
保守的な観測であるため、R2以降では対象コンポーネントごとにtyped port、owner、thread/lifecycle、
fake交換、contract test、monolith非リンクを別の受入条件として追加する。

## R2 Selection/Caret pilot

R2の最初の安全な縦切りとして、選択の表示座標や範囲を先に移動せず、presentation-neutralな
phase/modeだけを `sakura_editor_selection` へ抽出した。`CViewSelect` は既存Win32入力・描画と
legacyの選択範囲、ロック、座標状態を所有するadapterのままであり、このpilotを選択機能全体の
L4独立性とは扱わない。

- provider: `sakura_editor_selection` (`SelectionSession`)
- consumer: `sakura_app` -> `sakura_editor_selection`（consumer -> provider）
- compatibility adapter: `CViewSelect` / `CEditView_Mouse` / `CViewCommander_Select`
- contract: `sakura/editor/SelectionSession.h`（`ESelectionMode`、`ESelectionTransition`、`SelectionSession`）
- dedicated runner: `sakura_editor_selection_tests`（resource/package-less、provider private header非公開）
- terminal coverage: start/restart/end/no-op/mode fallbackを4件で検証

次のコマンドで、生成物を再生成せずにmanifest、依存DAG、専用runnerを確認できる。

```cmd
py -3 tools/build/sakura_build.py generate --check
py -3 tools/build/sakura_build.py graph check --all-contexts
py -3 tools/build/sakura_build.py --format json build component sakura_editor_selection_tests --context msvc-x64-debug
py -3 tools/build/sakura_build.py --format json test component sakura_editor_selection_tests --context msvc-x64-debug
```

MSVC x64 Debug/ReleaseおよびCMake MSVC x64 Debug/Releaseで本体・runnerのbuild/testが成功し、
本体統合（MSVC x64 Debug）も0 errorでリンクできた。CMakeでは既存の長いobject pathに対する
`CMAKE_OBJECT_PATH_MAX` 警告が残るため、これはSelection固有の失敗ではなく、ビルドパス短縮を
別gateで扱う。専用runnerは資源bundle、言語DLL、配布asset、全体package restoreに依存しない。

## 未完了と次のgraduation gate

- R0のraw pointer/public mutable field/`CEditApp` lifecycleの完全な型解析は未実施。
- Selection/Caretのphase/mode pilotは完了したが、選択range/lock/geometryとWorking Copyは未着手。
- R3のWin32 typed event adapter、R4のDocument Core、R5のDLLSHAREDATA capability facadeは未着手。
- tests1の実体分割と既存global getterの減少は未完了。strict CIは`architecture-gates`で実行する。
- Issue #15のresource/package/runtimeとControl IPC transportのgraduationも別途継続中。

したがって、Issue #18は完了扱いにせず、今回のコミットはR0/R1基盤のgraduationとして扱う。

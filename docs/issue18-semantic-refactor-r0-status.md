# Issue #18 R0/R1 実装状態

最終更新: 2026-08-05

## 今回の完了範囲

Issue #18の意味的分解トラックについて、R0の再現可能な基準台帳と、R1の増加禁止ラチェットを
canonical build CLIへ追加した。これはEditor Coreを分割し終えたという意味ではなく、分割前の
結合を測定し、新しい結合の増加を止めるための基盤である。

- 実装: `tools/build/sakura_build_lib/semantic_inventory.py`
- CLI: `py -3 tools/build/sakura_build.py inventory semantic`
- baseline: `tools/build/baselines/editor-core-semantic.json`
- 実行証跡: `build/evidence/r0/editor-core-semantic.json`（生成物、通常はGit管理外）
- 単体テスト: `tools/build/tests/test_semantic_inventory.py`

## R0 baseline

2026-08-05に現在の作業ツリーを一度だけ明示的に受け入れ、次の値を保存した。

| 観測項目 | baseline |
|---|---:|
| source files | 2,087 |
| source lines | 783,549 |
| include directives | 11,541 |
| `GetDllShareData` | 749 |
| `GetEditDoc` | 8 |
| `GetEditWnd` | 293 |
| raw `new` | 929 |
| raw `delete` | 722 |
| `catch (...)` | 635 |
| Win32 parameter mentions (`HWND`/`WPARAM`/`LPARAM`) | 4,614 |
| private-include hints | 45 |
| tests1/monolith link hints | 2 |
| filtered-test hints | 53 |
| semantic hotspots | 23 |

source fingerprintは `sha256:c964abc16057692c9dd7e1991dfeaf0fabf2eec0b1359ba98f21894b65f0b957`
である。getterのファイル別内訳とCEditWnd/CEditView/CEditDoc/CEditApp/DLLSHAREDATAのhotspotも
同じJSONに保存する。

## R1 ratchet

通常の検証は次で行う。

```cmd
py -3 tools/build/sakura_build.py --format json inventory semantic --strict
```

この実行は成功（終了コード0）し、baseline比の増加は空集合だった。ラチェット対象は
global getter、生の`new`/`delete`、catch-all、Win32型、private include、monolith link hint、
filtered-test hintに限定している。source file/line/include数は診断値であり、ソース追加だけを
理由に分割作業を止めない。

台帳はAST、型の所有権、実行時の共有状態、プロトコル互換性を証明しない。正規表現による
保守的な観測であるため、R2以降では対象コンポーネントごとにtyped port、owner、thread/lifecycle、
fake交換、contract test、monolith非リンクを別の受入条件として追加する。

## 未完了と次のgraduation gate

- R0のraw pointer/public mutable field/`CEditApp` lifecycleの完全な型解析は未実施。
- R2の最初の縦切り（Selection/CaretまたはWorking Copy）は未着手。
- R3のWin32 typed event adapter、R4のDocument Core、R5のDLLSHAREDATA capability facadeは未着手。
- tests1の実体分割、CIでのstrict実行、既存global getterの減少は未完了。
- Issue #15のresource/package/runtimeとControl IPC transportのgraduationも別途継続中。

したがって、Issue #18は完了扱いにせず、今回のコミットはR0/R1基盤のgraduationとして扱う。

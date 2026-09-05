# #290 先行修正の検証記録

この文書は全37項目の完了報告ではない。Search の結果世代・preview、FileLoad の MIME option・設定寿命、対応する CI を実装した先行変更の記録である。全量対応表は [ledger.md](ledger.md)。未実装項目を FIXED_VERIFIED にしていない。

## A. 作業基準

- 調査・実装開始 SHA: `afaa395c46a3671420ef088905a3046c2966b3a5`。開始時の fork main と一致。
- fork: `tsuyoshi-otake/sakura-editor-next`、branch: `codex/audit-safety-followups`。
- 元 checkout のユーザー変更を避け、`C:/Users/developer/tmp/sakura-audit-safety` に隔離した。
- Windows、MSVC 2022 14.44、x64 Debug/Release、既存 Python/pytest、Microsoft Java 11 を使用。依存 package の更新なし。gitlink は既存ローカル clone から固定 SHA を取得。
- Issue #290 を作成し、論理単位で commit。main push、merge、release、upstream 書き込みは実施しない。
- 主要 commit: `cfddbcbb2` (FileLoad)、`89ed278ec` (Search)、`611f785d0` (TLC/CI)、`75b47745d` (台帳)、`dd40b6cf5` (確認済み semantic identity 更新)。`c7c12a788` (Explorer非同期終了test)。後続の証跡 commit は同じ branch に含む。

## B. 全量対応と再現

全 F01–F22 / H01–H15、対象 symbol、分類、commit、未完了理由は [対応台帳](ledger.md) に残した。共通 tracking Issue は [#290](https://github.com/tsuyoshi-otake/sakura-editor-next/issues/290)。

修正前製品コードと追加した native regression tests を canonical solution build でビルドし、次の5件が失敗した。[red log](evidence/native-red.log) を保存した。

| ID | 修正前の観測 | 実装・現在の検証範囲 |
|---|---|---|
| F07 | 空検索へ変更後、既に USER32 に通知された旧 completion が再採用された | 入力変更時点で世代更新し、受理と置換受付で root/query/generation を照合。実workerの通知後に空文字・debounce入力・root変更・closeを行うnative testが成功 |
| F09 | 遠方のhitがpreviewから消える。境界でsurrogate pairを切る | hitを含む最大250 UTF-16 code unit窓に変更。元のcolumn/lengthは保持。長いhitとsurrogate pairの試験成功 |
| F18 | MIME optionがconverter生成より後に反映され、ON/OFF/reopenで内容が不正 | converter生成前にoption反映。実ファイルをON/OFF/ON/OFFで再openする試験成功 |
| F21 (部分) | loader構築後に呼出元設定を変更すると自動判定も変わる | immutable設定snapshotを所有。Prepareへ設定とconverter寿命を共有。mappingは依然親の借用 |

F19はREJECTED。指定された1引数factoryは既に `unique_ptr` を返し、full-expression終了時に破棄される。2引数factoryの所有権とは混同していない。

## C. 設計判断・保証境界

Search は worker の実行と UI の受付を分ける。pending/mailbox は引き続き depth-one。debounce は次の実行を遅らせるだけで、旧結果の失効を遅らせない。結果は検索時のrequest snapshotを持ち、UI側で照合する。replacement文字列とpreserveCaseは検索patternの同一性から除外する。

FileLoad の設定は構築時snapshotとし、temporary参照を保持しない。converterの共有所有は寿命を保持するものであり、converterのthread safetyを証明するものではない。Prepareされたreaderがmappingを使う間は親loaderを閉じられないという制約が残る。

Search保存は既存 `ReadVersioned` / `ConditionalAtomicReplace` へまだ接続していない。外部writerに対する厳密CAS、publish成功後のfailure/unknown、dirty working copyとの整合、backup/metadataは未解決。これらの安全性をこのPRで保証しない。

Updaterの単一installation owner、admission/generation、cancel-before-join、実行許可の失効と再起動後の扱い、installer検証と起動handleの同一性は未実装。digestはpublisher署名ではない。署名運用は外部判断が必要だが、他のUpdater修正を妨げる依存ではない。

Clipboard旧形式はnative size_tで、D&Dの生成は終端NULを含まない。旧32/64bitの曖昧性、GlobalSizeの余剰、raw UTF-16/embedded NULの互換性を含む共有parserが必要。今回は形式の意味を変更していない。

## D. 検証結果

以下は隔離repo rootから実行。ログのパスはローカルの `.codex/goal-loop/audit-safety/` と `C:/Users/developer/tmp/`。共有用の限定ログを [evidence/](evidence/) に保存した。

### Native build / tests

```powershell
py -3 tools/build/sakura_build.py build solution x64 Debug --jobs 4
py -3 tools/build/sakura_build.py build solution x64 Release --jobs 4

pwsh -NoProfile -ExecutionPolicy Bypass -File C:/Users/developer/tmp/sakura-audit-run-tests.ps1 -Configuration Debug -Label debug-focused
pwsh -NoProfile -ExecutionPolicy Bypass -File C:/Users/developer/tmp/sakura-audit-run-tests.ps1 -Configuration Release -Label release-focused

pwsh -NoProfile -ExecutionPolicy Bypass -File C:/Users/developer/tmp/sakura-audit-run-tests.ps1 -Configuration Debug -Label debug-unattended-after-wait -Unattended -TimeoutSeconds 600
```

- Debug/Release solution build成功。`build-dev`だけを根拠にtest build成功と扱っていない。
- [Debug](evidence/debug-focused.log)、[Release](evidence/release-focused.log): 各13件成功。filterは `FileLoadOptionsTest.*:SearchRequestSafetyTest.*:SearchWorkbenchToolGeometry.*`。
- 初回unattendedは3410件中3408成功、既存benchmark 1 skip、Explorer test 1失敗。`Close()`直後にStoppedを要求していたが、productionは非同期retirementへ渡す契約だった。
- `ExplorerTool.ProductionWorkerDisplaysJunctionsAsLeaves` は同じ最終Stopped検査を維持し、既存のPumpMessagesUntilで最大2秒待機。再実行は **3410件中3409成功、既存benchmark 1 skip、失敗0** (235267ms)。既存disabled 15件もそのまま明記する。
- 広範囲再実行の後、同じtestの親windowをRAII化した。最終cohortは上記13件＋Explorer当該test。最終cohortは [Debug](evidence/debug-final-cohort.log) 14/14 (667ms)、[Release](evidence/release-final-cohort.log) 14/14 (541ms) 成功。両runnerのOwnedSurvivors=0。
- unattended filterは既存 `src/test/CLAUDE.md` の除外集合をそのまま使用。新たなskipや除外追加なし。これは対話UIを含む全suite完走とは異なる。
- runnerは全体timeout、own PID tree cleanup、終了後の該当repoのtests1/sakura再列挙を実施。実行済みnative試験のOwnedSurvivorsは0。

PBTは `mt19937` seed `0x290`、64個の実ファイル、prefix 0..1999、hit長1..400で元座標とpreview内hitを検査。反例なし。shrinkerは実装しておらず、縮小反例なしをshrinking実施の意味にしない。

### Python / architecture

```powershell
py -3 -m pytest src/test/py/test_ci_plan.py src/test/py/test_ci_plan_workflow.py tools/build/tests/test_architecture_gates_workflow_contracts.py tools/build/tests/test_search_lifecycle_gate.py -q
py -3 tools/build/sakura_build.py lint checkout-invariance
py -3 tools/build/sakura_build.py generate --check
py -3 tools/build/sakura_build.py graph check --all-contexts
py -3 tools/dependency_ledger.py check
py -3 tools/build/sakura_build.py inventory semantic --strict
pwsh -NoProfile -File src/main/ps1/check-encoding.ps1 -BaseSha afaa395c46a3671420ef088905a3046c2966b3a5
git diff --check
```

Pythonは33 tests / 44 subtests成功。[log](evidence/python-green.log)。モデルファイルをdocs-only扱いする旧classifierに対するredは2件。LF/CRLF checkout invariance、generated metadata、6 context graph、dependency ledgerは成功。

semantic ratchetでは実際の新規raw HWND/public mutable stateを最初に解消し、その後に残った既存getter/fieldのidentity移動2件を確認して公式accept-currentを使用した。基準SHA `75b47745d84bec9fd18680427739f15576e2cef9`、Issue290、理由とimmutable historyを記録。per-rule増加なし、raw delete/catch-allは各1減。scanner、閾値、touched-scope条件は変更していない。Explorer testを触った際のtouched-scope減少不足も隠さず、親windowのRAII化で対処した。最終strictはnew_findings/increases/missing_touched_reductionsすべて空、exit0。

### TLC / negative tests

```powershell
py -3 tools/verify-search-lifecycle.py --jar C:/Users/developer/tmp/sakura-tlc-pin/tla2tools.jar --output .codex/goal-loop/audit-safety/tlc-final
```

- 公式v1.7.4 asset (2024-08-05公開)、SHA256 `936a262061c914694dfd669a543be24573c45d5aa0ff20a8b96b23d01e050e88` を固定。新規release採用なし。
- model `docs/formal/SearchRequestLifecycle.tla`、3cfgのSHA256、実際のjava command、source HEADを [receipt](evidence/tlc-evidence.json) に保存。receiptのsource HEADは実行時点であり、後続のtest/doc-only commitを含まない。
- 正モデル: exit0、362 generated / 100 distinct / queue0 / depth13。
- `NoEmptyInvalidation` と `NoGenerationCheck`: 各exit12、意図した `CurrentResults` invariant violation。
- timeout、tool error、別invariant failure、探索未完了をnegative成功として認めない。Python gate testsで検査。
- 3世代、depth-oneの有界safetyモデル。liveness、公平性、Win32通知失敗、disk I/Oを検証したものではない。
- 必須architecture jobにTLC stepとartifactを接続。`.tla`/`.cfg`変更はdocs-only経路から除外。GitHub上のexact-head CIは別途確認が必要。

### 未実行・保証しないもの

runtime mutation campaign、ASan、allocation/Win32 failure injection、独立component runner、MinGW実ビルド、packaging、対話UI E2E、screen reader/UIA client、dual-captureによる実画面、性能A/B、全processのhandle/thread統計は未実行。TLC負モデルをC++ mutationの代わりとは扱わない。hidden HWND/LB_GETCOUNT検査は実際のpaintを証明しない。

## E. 未完了とmerge判断

本変更はDraftとして扱う。全37項目の完了条件を満たしておらず、今回のループ全体はPASSではない。Search/FileLoadの局所red→green、native build、限定モデルの結果はレビューできるが、必要なruntime mutation、ASan、実画面検証、exact-head CIの不足があるため、現時点でmerge可能とは判断しない。

次の実行可能な作業は、先行修正の不足検証を満たしたうえで、Clipboardの共有parser/所有権とCut成功契約、Search保存transaction、Updater protocolをそれぞれproduction経路と受入試験を伴う単位で進めること。未修正の安全性欠陥が残っているため、製品全体が安全になったとは報告しない。

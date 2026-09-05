# 安全性再検証・対応台帳 (#290)

## A. 作業基準

- 調査開始・実装基準: `afaa395c46a3671420ef088905a3046c2966b3a5`。
- 2026-09-05 にローカル main と `git ls-remote origin refs/heads/main` の一致を確認。
- fork: `tsuyoshi-otake/sakura-editor-next`。既定 branch は main。
- 依頼に明示された branch/commit/push/PR 権限を使う。main への push、merge、release は実施しない。
- 実装 branch: `codex/audit-safety-followups`、隔離 worktree: `C:/Users/developer/tmp/sakura-audit-safety`。
- 元 checkout の変更済み memory、ConPty test、vcpkg と未追跡ファイルは維持する。
- Windows/MSVC 2022 14.44、Python canonical build CLI。依存 gitlink は既存ローカル clone から固定 SHA を checkout。upstream fetch/push はしない。
- 全項目の tracking Issue: [#290](https://github.com/tsuyoshi-otake/sakura-editor-next/issues/290)。共通の [Draft PR #291](https://github.com/tsuyoshi-otake/sakura-editor-next/pull/291)。各行の実装commitと検証結果を evidence 文書に記録した。

## B. 全量対応表

SOURCE_CONFIRMED は実ソースでの観測であり、製品再現ではない。未検証の項目は完了ではない。
下表の Issue はすべて #290。個別 PR/commit がない行は未実装。H は既存基盤の不存在を主張しない。

| ID | 分類 | 実装commit | 対象 path / symbol | 観測・不足証拠、未完了理由・残余リスク |
|---|---|---|---|---|
| F01 | SOURCE_CONFIRMED | 未実装 | workbench/search/WorkspaceSearchEngine.cpp / ReplaceMatches | 位置・match再検査後の独自保存。versioned FileService未接続。外部writer race未対策。 |
| F02 | SOURCE_CONFIRMED | 未実装 | 同 / temporaryPath | fullPath + `.skrnew` を出力。既存物・同時操作の保護未実装。 |
| F03 | SOURCE_CONFIRMED | 未実装 | 同 / UnicodeToCode | 変換結果を確認せず保存。decode/encode loss故障注入未実行。 |
| F04 | NEEDS_REPRO | 未実装 | platform/filesystem/CWin32FileSystemProvider.cpp / ConditionalAtomicReplace; ReplaceMatches | DACL/ADS/属性/hardlink/reparse/UNCの実機マトリクス未実行。既存providerの能力と保存契約の照合が必要。 |
| F05 | SOURCE_CONFIRMED | 未実装 | WorkspaceSearchEngine.cpp / ReplaceMatches; io/CBinaryStream.cpp | 独自stream保存、close/flushの故障が伝播する契約を要再設計。publish後unknownを区別するテスト未実行。 |
| F06 | SOURCE_CONFIRMED | 未実装 | workbench/search/CSearchWorkbenchTool.cpp / RunReplace | UIから同期ReplaceMatches。filesChanged配線は存在。dirty/別process/reload/Saveの整合は未検証。 |
| F07 | REPRODUCED (修正済み・受入未完了) | 89ed278ec | 同 / StartSearch, ScheduleSearch, kResultMessage | 空検索が世代更新前にreturn。debounce前に無効化しない。修正前のUSER32待機中completion採用を再現。世代・root・検索条件照合を実装しDebug/Release回帰テスト成功。native Replace Allの正常／失効3例と即時失効を除去するruntime mutationを追加（evidence参照）。他guard・遅延発行・dual-captureは未完了。 |
| F08 | NEEDS_REPRO | 未実装 | WorkspaceSearchEngine.cpp / CollectLineMatches, ReplaceMatches | 現行Bregonigでzero-width/capture/EOL意味論の再現が必要。古いコメントだけで変更しない。 |
| F09 | REPRODUCED (修正済み・受入未完了) | 89ed278ec | 同 / BuildPreview | 行先頭250文字の切り出しで遠方hitのpreviewLengthが0。hitを含む250 code unit窓に修正。native red/green、surrogate境界、seed 0x290の64例がDebug/Releaseで成功。包括的mutationと視覚検証は未実行。 |
| F10 | SOURCE_CONFIRMED | 未実装 | _os/CClipboard.cpp / GetText; util/os.cpp / GlobalSakura::wstring | private payloadのnative size_tをヘッダ長検査前に読む。D&D生成は終端なし、GlobalSakuraは終端分を要求する差も確認。Paste/D&D共通parserと32/64bit fixture未実装。 |
| F11 | SOURCE_CONFIRMED | 未実装 | _os/CClipboard.cpp / SetText, SetHtmlText | allocation/lock/publishの失敗・所有権をすべて伝播しない。Cutまでの失敗経路未検証。 |
| F12 | SOURCE_CONFIRMED | 未実装 | update/UpdateService.cpp / TriggerCheck, CancelUpdate | resettable token、state検査とPublishの間にlock境界。並行API/listener再入の決定的試験未実行。 |
| F13 | SOURCE_CONFIRMED | 未実装 | update/UpdateExecutor.cpp / Post, Stop, Run | void受付、満杯/停止時drop、queued clear、catch空。通常操作でのqueue飽和は未確認。 |
| F14 | SOURCE_CONFIRMED | 未実装 | UpdateService.cpp / RunPendingUpdate; UpdateStagingStore.cpp / InstallerMatches | 起動前はsize検査。digest再計算・検証handle同一性・path containmentの修正未実装。 |
| F15 | SOURCE_CONFIRMED | 未実装 | UpdateStagingStore.cpp / WriteManifest, StoreInstaller, RemoveOtherStagedBuilds | 固定manifest temporary/共有stage、windowごとのservice。installation ownerと複数process試験未実装。 |
| F16 | SOURCE_CONFIRMED | 未実装 | UpdateService.cpp / AbortQuitAndInstall, RunPendingUpdate | WriteManifest失敗を無視しReadyへ戻る。disk booleanによる起動許可の失効・migration未実装。 |
| F17 | SOURCE_CONFIRMED | 未実装 | update/UpdateComposition.cpp / Shutdown; UpdateService::~UpdateService | timer.Stop → executor.Stop。cancelはservice destructor。製品上の停止時間/依存寿命影響は未再現。 |
| F18 | REPRODUCED (修正済み・受入未完了) | cfddbcbb2 | io/CFileLoad.cpp / FileOpen; io/CIoBridge.cpp / FileToImpl | converter生成後にnFlag代入。bridge引数は未使用。converter生成前にoptionを反映。実ファイルMIME ON/OFF/reopenがred→green、Debug/Release成功。ASan・runtime mutation未実行。 |
| F19 | REJECTED | 未実装 | charset/CCodeFactory.h / 1-argument CreateCodeBase; convert/CConvert_Code* | 指定3 callsiteの1引数factoryはunique_ptrでfull-expression終了時破棄。owning raw pointerを捨てるとの前提は誤り。2引数API全体の所有権改善は別途。 |
| F20 | SOURCE_CONFIRMED | 未実装 | _os/OleTypes.h / SysString::Get, operator= | ACP容量がUTF-16長×2、copy代入は旧BSTRを解放しない。macro callsitesは存在。UTF-8 ACP環境/実call到達性/heap再現未実行。 |
| F21 | REPRODUCED (部分修正) | cfddbcbb2 (部分) | io/CFileLoad.h, .cpp / constructor, Prepare | temporary設定をpointerで保持。Prepareは親のpointerへ上書き。設定snapshotを所有しPrepareへ共有、converterも共有所有。設定変更のred→greenあり。追加のPrepare状態修正はevidence.md参照（encoded EOL継承／UTF-7 offset初期化の2件red→green）。mappingは依然親の借用でF21全体は未完了。 |
| F22 | SOURCE_CONFIRMED | 未実装 | CSearchWorkbenchTool.cpp / Start, worker loop | event/thread/retirement/WAIT_FAILED/通知失敗のterminal契約不足を確認。故障注入と製品到達性未検証。 |
| H01 | IMPROVEMENT | 未実装 | window/CEditWnd.cpp / update and search projection | #18の完了範囲を全分割完了と解釈しない。縦切りcontroller抽出未着手。 |
| H02 | IMPROVEMENT | cfddbcbb2 (部分) | io/CFileLoad.cpp / configuration reads; env/DLLSHAREDATA.h | 設定snapshotの狭い修正から開始。worker全体の共有設定snapshot化は未完了。shared-memory ABIは維持。 |
| H03 | IMPROVEMENT | cfddbcbb2 (部分) | io/CFileLoad.cpp / Prepare, FileClose | 設定寿命とmapping/converter借用寿命を分ける。converter寿命のみ共有所有化。mapping owner/lease抽出は未着手。 |
| H04 | IMPROVEMENT | 未実装 | platform and workbench child-process boundaries | Git/Gh等の既存runner棚卸し、共通化境界と実process試験が必要。installer/ConPTYを万能runnerへ統合しない。 |
| H05 | IMPROVEMENT | 未実装 | util/os.h; _os/OleTypes.h; io/CFileLoad.cpp | 既存resource wrapperは存在。resource種別ごとの所有権棚卸しと不足修正未完了。 |
| H06 | IMPROVEMENT | 未実装 | include/sakura/filesystem/IFileService.h; platform/uri | Uri/FileService/long path基盤を再利用する予定。Recent/session/IPC end-to-end試験未実行。 |
| H07 | IMPROVEMENT | 未実装 | charset/CCodeFactory; _os/OleTypes.h | ACPと内部Unicode境界の全callsite分類未完了。raw UTF-16/CESU-8契約を無断変更しない。 |
| H08 | NEEDS_REPRO | 未実装 | util / LoadLibraryExedir and DLL-loading callers | CWD依存の到達性・plugin互換検証未完了。 |
| H09 | BLOCKED | 未実装 | update/UpdateDigest; Win32UpdateLauncher; release signing | 現行digestは完全性でありpublisher保証ではない。証明書・期待publisher・配布運用の外部決定が必要。鍵操作は行わない。 |
| H10 | IMPROVEMENT | 未実装 | CSearchWorkbenchTool / custom actions; CustomUiAutomationProvider | Search custom actionsのUIA契約・client実行未着手。他surfaceの未対応を推定しない。 |
| H11 | IMPROVEMENT | 611f785d0 | docs/formal; .github/workflows; tools/build | TLCモデルは既存。Search世代モデル・必須architecture job・docs分類修正を実装。固定TLC正/負3ケース成功。Updater/保存protocolモデルは未完了。 |
| H12 | IMPROVEMENT | 89ed278ec / c7c12a788 (部分) | src/test; existing fuzz/property infrastructure | repositoryにPBT/Fuzzがないと断定しない。Search preview seed 0x290の64実ファイル例を追加。Explorer非同期終了testの待機契約とRAIIを修正。runtime mutation/ASanは未完了。 |
| H13 | IMPROVEMENT | 未実装 | src/main/modules/modules.json; tools/build | canonical buildと既存graphを維持。private変更のrebuild閉包実測は未完了。 |
| H14 | IMPROVEMENT | cfddbcbb2 / 89ed278ec (部分) | scoped CLAUDE.md; legacy TODOs; diagnostics | 確認した誤記/契約のみ更新する。古いTODO全件分類やpayload-free診断整備は未完了。 |
| H15 | SOURCE_CONFIRMED | 未実装 | WorkspaceSearchEngine / CollectLineMatches, SearchFolder, ReplaceMatches | 全match収集後のbudget検査・matchごとの再走査。上限理由の説明・出力膨張・regex中断保証未完了。 |

## C. 設計判断と保証境界

Search保存は既存 `ReadVersioned` / `ConditionalAtomicReplace` へ接続する必要があるが、この初期変更ではまだ接続していない。hash再検査とReplaceFileWを任意外部writerに対する厳密CASと扱わない。commit後のfailure/unknown、復旧データ、dirty working copy、別window/processの扱いも未解決。

Updaterは単一installation owner、session/operation単位の実行許可、cancel-before-join、起動直前digest/handle同一性、launch outcome不明の再調停を一つのprotocolとして修正する必要がある。manifest書き戻しだけの局所修正ではF16完了にしない。旧Clipboard形式はbitness曖昧性を含め検証し、同じformat名の意味を変更しない。

## D. 検証証拠

実行コマンド、red/green、未実行検証は [evidence.md](evidence.md) に記録する。baseline取得中の失敗を回帰テスト失敗やPASSとして数えない。

## E. 未完了範囲

これは全体完了報告ではない。各表行で未実装/未検証とした作業を継続する必要がある。Issue作成だけでFIXED_VERIFIEDにしない。署名運用だけを理由に他の修正を止めない。今回のPRのmerge判断は、実際の変更範囲についてのみ evidence 文書のgate結果で行う。

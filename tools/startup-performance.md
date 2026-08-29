# 起動パフォーマンスの比較計測

[`measure-startup-performance.ps1`](measure-startup-performance.ps1) は、同じ
Sakura 実行ファイルと同じ Markdown 入力について、プロセス起動から画面表示・文書読込・
全文レイアウトと描画同期までの時点を繰り返し計測する開発者用ベンチマークです。変更前後やビルド構成間の
**比較**に用いるものであり、特定のミリ秒値や全環境での性能を保証するものではありません。

固定入力には [`startup-benchmark-sample.md`](startup-benchmark-sample.md) を使います。この
サンプルは再現可能な合成データであり、機密の社内文書を複製していません。2026-07-30 の
`Desktop\社内規定.md` に由来する比較値は、リポジトリ直下の
[`CLAUDE.md`](../CLAUDE.md) を参照してください。本サンプルへその内容をコピーしては
いけません。現在の固定サンプルは 1,249,037 bytes、5,604 行、SHA-256
`80156fc08b7c91988fd79d7230342862ea0fe534147089ac9e6f5b29461c61b6` です。計測前にこの値または
ファイルハッシュを確認し、入力が変わった結果を同列に比較しないでください。

## C++ / Rust GUI 起動のペア計測

Issue #274 の GUI 起動証拠には [`measure-output-startup.ps1`](measure-output-startup.ps1)
を使います。これは `tests1.exe` の Output provider マイクロベンチマークとは別の計測です。
後者の結果を GUI 起動時間の証拠として扱ってはいけません。

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File .\tools\measure-output-startup.ps1 `
  -CppSakuraExe .\build\evidence\output-startup\cpp\sakura.exe `
  -RustSakuraExe .\build\evidence\output-startup\rust\sakura.exe `
  -StartupSample .\tools\startup-benchmark-sample.md `
  -AffinityMask 1 `
  -CollectOnly -WarmupLaunches 1 -MeasuredLaunches 1
```

既定では各バックエンドについて 5 回の warmup と 30 回の measured launch を行います。
各スロットでは C++ / Rust を交互に先頭へ置く決定的な順序で起動し、同じ固定サンプル、
同じ fresh-per-launch プロファイル方針、同じ非ゼロ CPU affinity mask を使います。qualified mode では
各 backend の canonical runtime stage と `.sakura-runtime-stage.json` receipt が必須で、receipt が列挙した
全ファイルを計測用 campaign bundle にコピーし、サイズと SHA-256 を各起動前と campaign 終了後に
再検証します。bundle 直下へ UTF-16LE BOM/CRLF の `sakura.exe.ini` を生成して
`[Settings] MultiUser=0` を厳密に検証します。これにより `-PROF` は bundle 内だけへ解決され、
`%APPDATA%` や利用者の設定へフォールバックしません。sidecar が欠落・改変・再解析点の場合は
起動せず fail closed します。隣接 DLL を探索して暗黙にコピーすることはありません。exe と sidecar
だけの fallback は `-CollectOnly` に限定され、dependency closure の証拠にはなりません。
プロセスは suspended 状態で作成し、run-owned の kill-on-close Job Object へ割り当て、CPU affinity を
設定して read-back した後にだけ resume します。bundle を working directory とし、cleanup 後は Job
membership と exact bundle image path の両方で残存がないことを確認します。
最終の descendant affinity 検証は、完全な typed process census と PID / creation / image-path の exact identity
検証を通過した `Get-TrackedOwnedProcesses` の fresh current-live records だけを対象にします。current set に
存在しない historical record は終了済みの expected exit として扱い、affinity read-back の対象にしません。
current set が null / empty、未知または重複 PID、creation / path mismatch である場合、census / identity query / read-back
が失敗した場合は fail closed です。no-GUI self-test は 5 historical records から exact current 4 件だけを計画して read-back
し、expired である 1 件を read-back しないことも確認します。
Toolhelp の PID / PPID 閉包は候補集合に過ぎず、既存の run-owned parent から新しい descendant を `Owned` へ昇格する
前に、child の identity が parent の identity と一致し、child の creation time が parent より過去でないことを検証します。
Windows の FILETIME は観測時の分解能により同一 tick になり得るため、同時刻は許可し、parent より古い時刻だけを
stale PID reuse として除外します。拒否された候補を親とする descendant も昇格させず、creation の欠落・型不正は
fail closed です。`descendantCreationOrderSelfTestVerified` は、過去・同時刻・新しい実子、拒否候補の子、malformed
identity をこの境界で検証した no-GUI self-test の boolean です。

### paired runner の legacy path budget

通常の Win32 path-text 上限は 259 文字（終端 NUL は数えない）です。
paired runner は既存の `runId` に基づく sample/report のファイル名対応を維持しつつ、同じ
`runId` の SHA-256 先頭 16 桁を campaign-owned bundle、profile、trace 名に使います。
証拠の `runId` 自体は変わらず、C++ / Rust の path role token は同じ長さです。
検査は二段階です。phase 1 では全 schedule entry と backend ごとの実際の最大 ordinal を先に計画し、
artifact 解決前に profile の最悪パスへ `\.sakura-platform\profile-authority.v1.tmp.` と 32 桁の hex を
加えた通常 Win32 path を検査します。259 文字までは受け入れ、260 文字以上は artifact 解決・bundle 作成・
GUI 起動の前に `path-budget` として fail-closed します。qualified mode で build manifest と runtime-stage
receipt の検証が完了した後、phase 2 は各 backend の bundle 配下に bundle copier が作る全 canonical
receipt-relative destination と `sakura.exe.ini` sidecar を同じ計画へ追加して再検査します。collect-only は
`sakura.exe` と sidecar を追加します。phase 2 の assertion も sample copy / bundle 作成 / GUI 起動の前にあるため、
nested closure destination で初期検査を迂回できません。

失敗 envelope は raw path を含めず、`phase`（`generated` / `finalized`）、最大長・上限・margin、launch/ordinal
数、token 長、closure 件数と最大長だけを持つ単一の payload-free summary を保持します。`%TEMP%` への暗黙移動や
product manifest / LongPaths 設定の変更は行いません。これは旧来のフル timestamp/GUID 名が引き起こす MAX_PATH
超過を、所有する campaign 名だけ短縮して回避するための予算です。
result root 自体が極端に長く、互換維持している report filename さえ開けない場合は、fail-closed の終了は安全に
返しますが、ディスク上の envelope 保持までは保証できません。別の evidence 位置を発明せず、typed envelope を
保持する必要がある場合は report path が書き込める result root を指定してください。

各 paired launch では bundle 配下に一意な run-owned trace directory も作成します。共有 probe はそこへ
書かれた startup trace を process cleanup 後に読み取り、paired report には allowlist 済み event 名の件数、
`editor` / `control` / `unknown` ごとの role 件数、および最大 256 件の `orderedEvents` を残します。各 ordered
item は `ordinal`、allowlist 済み `event`、`role`、`value1`、`value2`、QPC から算出した `elapsedMs` だけを持ち、
上限到達時は `orderedEventsTruncated=true` になります。trace の path、directory 名、`detail`、payload、raw command
は report に出しません。QPC の launch clock と frequency が互換でない、空、または malformed な trace は
`trace-unavailable` として fail closed にします。elapsed の診断上限は launch timeout とは別の 120 秒です。なお、これらの trace は診断専用であり、event の順序や件数は
readiness 成功を証明しません。RAII の `editor_ready` event は early return 経路でも発行され得ます。trace directory
は non-reparse の所有パスであることを確認して削除し、削除を検証できなければ `traceCleanupVerified=false` として
launch を fail closed にします。

同じ paired report の各 launch には `startupDiagnostics` があり、`0.5s`、`2s`、`10s`、`timeout` の四つの
checkpoint ごとに、上限付き process metadata と root の exit state/code を記録します。root handle の
`STILL_ACTIVE` (259) は `active` であり、exit とは数えません。必要な診断観測が欠ける、または不正な場合は
`diagnostic-unavailable` として除外し、launch success を偽装しません。

既定の qualified mode では各 backend の warmup 5 回 / measured 30 回を下回る指定を拒否します。
GUI の疎通だけを確認する場合は `-CollectOnly -WarmupLaunches 1 -MeasuredLaunches 1` を明示的に
指定できますが、その結果は `acceptance.qualified=false` および `pass=false` となり、性能証拠には
なりません。

出力 JSON は payload-free です。commit、C++ / Rust artifact、host、sample、profile policy と
各 launch の profile の SHA-256、sidecar contract、bundle の source/copy ハッシュ前後と cleanup、
起動マイルストーンの中央値 / p95、失敗種別と cleanup の結果
だけを含み、絶対パス、本文、ウィンドウキャプション、コマンドライン、例外本文、環境変数の値は
含みません。失敗またはプロセス残存の launch は `excluded=true` の typed record として残りますが、
統計には入りません。親プロセスを先に identity 再照合して終了させる cleanup は全体で 3 秒に制限され、
残存があれば証拠全体を不合格にします。

いずれかの launch で process cleanup または profile cleanup を検証できなかった場合、後続の
backend を起動せず、その時点で campaign を停止します。JSON の `termination` に typed な
`cleanup-unverified`、完了数、抑止数を残し、`acceptance.qualified` と `pass` は false になります。
これにより残存プロセスや汚染された profile による後続 launch の増殖を防ぎます。

### Job-query / cleanup telemetry（診断専用）

paired runner の v1 optional telemetry（`launchJobQueryObservation`、各
`startupDiagnostics` checkpoint の `jobQueryObservation`、`cleanupObservation`）は、
payload-free な numeric observation としてだけ保持します。bounded な count / byte、boolean、
numeric Win32 error code と最大 8 件の attempt projection 以外は出力せず、PID、path、handle、
message、command、caption、document data、raw object は query / cleanup telemetry に含めません。
producer の JobObjectBasicProcessIdList 列挙は、最初の sizing call を含めて一つの列挙あたり最大
8 回の native query に制限します。再試行する Win32 error は `122`、`24`、`234` だけで、checked
arithmetic で 1 MiB 以下のより大きな buffer target を計算できる場合に限ります。例えば retained
evidence の `ERROR_MORE_DATA (234)` で `16 -> 40` bytes に拡張します。`capacityBytes`、
`requiredBytes`、`returnLengthBytes`、`assignedProcessCount`、`listedProcessCount` は別々の bounded
診断値で、各 attempt の resize と終端エラー（234 を含む）を保持します。

no-GUI の producer self-test は injected query invoker で本番と同じ retry loop を実行します。`16 -> 40` の
`234` correction と successful partial correction はそれぞれ正確に3回（sizing、retry、final success）を使い、
retry exhaustion は正確に8回で最後の native error を保持します。本番と self-test が共有する attempt predicate
は9回目を拒否します。self-test ではさらに architecture 依存の buffer growth と partial response 間の
membership 増加を同じ loop で検証し、0・負値・範囲外・重複 PID を拒否します。retry budget が native failure の直後に尽きた場合、top-level error と最後の attempt は
その実際の retryable error を保持し、`122` は successful partial の exhaustion にだけ使います。identity-gap
requery は別列挙で独自の budget を持ちます。

成功と扱うのは `listedProcessCount == assignedProcessCount` で、header/count/capacity が整合し、全 PID
が正の有効値かつ重複しない場合だけです。successful partial list は成功として報告せず、checked な
拡張を行って完全な list の再 query を要求します。overflow、stagnation、malformed count、重複 PID、
buffer cap 超過、または 8 回の budget 枯渇は fail closed です。cleanup の identity-gap requery は
別の列挙であり、独自の 8-call budget を持ちます。zero survivors でも query が失敗していれば cleanup
の証明にはなりません。
paired consumer は shared cleanup report の additive な固定 fields も保持します。cleanupObservation には
`processEnumerationAttempted` / `processEnumerationSucceeded` / `processEnumerationComplete` /
`processEnumerationErrorCode` / `processEnumerationRetryCount` / `processEnumerationCallCount` /
`processEnumerationCompletedCount` / `processEnumerationFailureCount` と、
`trackedSweepFailureType` / `trackedSweepFailureErrorCode` /
`trackedSweepIdentityAttemptCount` / `trackedSweepIdentityFailureCount` /
`trackedSweepDisappearedAfterSnapshotCount` / `trackedSweepStillPresentAfterFailureCount` /
`trackedSweepPassCount` を出力します。affinity には
`historicalOwnedCount` / `currentLiveCount` / `expiredHistoricalCount` /
`failureType` / `failureErrorCode` / `liveSetSource` を保持します。全 integer は bounded な
payload-free 値で、producer の enum allowlist、process enumeration の succeeded / complete / count
equations、`currentLiveCount <= historicalOwnedCount`、および
`expiredHistoricalCount = historicalOwnedCount - currentLiveCount` を検証します。attempted な
process enumeration で failure が 0 の場合は succeeded=true / complete=true / completedCount=callCount
を必須とし、failure が 1 以上の場合は succeeded=false（complete は true / false のいずれも許可）と
します。failure code は first-cause を保持します。

native process census の envelope は PowerShell の暗黙 cast を許可しません。`Attempted` / `Complete` /
`Succeeded` は実 bool、`ErrorCode` / `AttemptCount` / `RetryCount` は実 CLR integer、`Retried` は実 bool とし、
attempt は 1..3、retry は attempt より小さく、`Retried = (RetryCount > 0)` を必須とします。成功時だけ
`Attempted=true` / `Complete=true` / `Succeeded=true` / `ErrorCode=0` の entries を返し、typed failure や
partial / malformed envelope は必ず throw します。entries は bounded array、PID 0..Int32.MaxValue、親 PID
0..Int32.MaxValue、non-empty bounded image、unique PID を検証します（PID 0 は Toolhelp の valid entry です）。
Job query も `AttemptCount` / `AttemptNumber` / `ErrorCode` を実 Int32、byte fields を実 UInt64、process
counts を実 UInt32、bool fields を実 bool として検証し、bounded byte/count、attempt 配列長、連番、last
attempt との top-level 一致、`Resized` の OR、および成功時の listed/assigned/ProcessIds 整合を確認します。
不一致、fractional / floating、string、scalar、duplicate、out-of-range metadata は decision 前に fail closed
します。zero-capacity の初回 sizing attempt は index 0 のみで、assigned / listed は 0、`Resized=false`、
`requiredBytes == returnLengthBytes` を必須とします。後続 data attempt は `requiredBytes == capacityBytes` と
前回より大きい capacity を要求し、non-final は retryable failure + `Resized=true`、または
`listedProcessCount < assignedProcessCount` の successful partial + `Resized=true` だけを許可します。
final success は last attempt が `Succeeded=true` / `ErrorCode=0` / `Resized=false` であることを必須とし、
中間の complete success や non-retryable failure、last attempt と top-level status / error の不一致を拒否します。

### Job containment proof v2

`processCleanupVerified=true` の必要条件は、run-owned Job handle がまだ query 可能な間に得た
`containmentProof.version=2` です。cleanup は最初に parent-first の graceful close を要求し、最大 8 回の
bounded observation を行います。fresh で structurally valid な `JobObjectBasicProcessIdList` が empty を
示せば `mode=graceful-job-empty` です。member が残る、identity observation が失敗する、または graceful
observation が完了しない場合は PID 単位の terminate を行わず、exact Job handle に対して
`TerminateJobObject` を正確に 1 回だけ呼びます。成功後も handle を閉じず、同じ 3 秒 budget と最大 8 回の
outer membership poll の中で fresh query が zero member を示すまで待ちます。その後にだけ
`mode=explicit-job-termination` を確定し、finally で Job handle を閉じます。query failure / malformed shape、
member 残存、termination failure、CloseHandle failure はそれぞれ typed terminal rejection です。
`JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` は全 branch の fail-safe として維持しますが、close による暗黙 terminate
自体は proof ではありません。

`containmentProof` は payload-free で、`version`、`mode`、`terminalState`、
`terminationAttempted` / `terminationSucceeded` / `terminationErrorCode`、正規化した
`terminalJobQuery`、`terminalJobMemberCount`、`jobEmptyProven`、`identityReconciliation` だけを含みます。
PID、path、command line、handle、raw query object は含みません。`jobEmptyProven=true` は必ず successful な
terminal query、`terminalJobMemberCount=0`、assigned/listed count と zero の一致を要求します。
termination 未試行なら mode は `graceful-job-empty`、試行かつ成功なら
`explicit-job-termination` でなければなりません。`unavailable` は empty proof がない場合だけです。
verified terminal state は mode と一対一で、未知 enum、bool/integer の暗黙 cast、partial field、または
cross-field equation の不一致を拒否します。

`QueryProcessIdentity` failure telemetry は payload を持たない operation enum
`none` / `open-process` / `query-image-path` / `get-process-times` / `exception` と、明示的な observer role
`launch-job-member` / `graceful-job-member` / `post-close-tracked-history` / `exact-path` を保持します。
post-close tracked-history の identity failure は、先に `jobEmptyProven=true` が成立し、operation と role が
allowlist に入り、fresh typed census でも対象が消えている場合だけ reconcile できます。
`identityReconciliation` は attempted/accepted、operation、observerRole、reason のみを記録します。
exact-path observer failure は常に unreconciled terminal failure です。任意の error 5、PID/path の不在、
zero survivor、または CloseHandle success だけを cleanup proof へ昇格させません。

v2 より前の `jobQuerySucceeded`、`jobCloseSucceeded`、tracked/exact-path sweep、zero survivor、raw の
`processCleanupVerified=true` は診断互換性のため読み取れても、containment authority としては無効です。
present な v2 object が malformed、unknown、または equation 不一致なら fail closed です。v2 が valid でも、
post-close tracked sweep、exact-path sweep、zero survivor、zero cleanup error、closed Job handle の既存 gate は
すべて必要で、一つでも欠ければ `rejected-post-close-observation` になります。

paired converter は success 判定の前に launch と cleanup の nested object を照合します。旧 v1 のように
両方の raw object が欠落している場合は、両方を明示的な `not-observed` に正規化して neutral とします。
一方でも present なら、両方が valid な `observed`（failure のない正しい observed state を含む）に正規化され、
status と7 fields 全てが exact 一致しなければなりません。一方だけ present、unavailable / partial / malformed、
cross-field 不整合、または valid だが launch / cleanup 間で不一致の場合は typed `cleanup-unverified` として
run を success=false / excluded にし、termination が後続 launch を抑止します。valid な identity telemetry は
additive な診断であり、既存の Job close、最終 sweep、zero survivor、zero cleanup-error gate を弱めません。
schema version 1 の旧 report で追加 fields がすべて欠落している場合は、`unknown` / `not-observed` / `0` の
明示的な local fallback で読み込み可能です。object が存在するのに field が一部だけ、integer が範囲外、enum
が未知、または cross-field が不整合な新 report は local `unavailable` に fail closed し、run の qualify には
使いません。その他の query / cleanup telemetry は従来どおり diagnostic-only で、Job query が失敗したときに
zero survivors を cleanup proof へ昇格させず、raw payload も保持しません。optional fields がない旧 v1 report は
neutral な `not-attempted` とし、present だが malformed / mismatched な Job identity object だけは上記の契約に
より意図的に fail closed します。
graceful fallback の 3 fields は all-present だけを current schema として受理し、旧 report の all-absent は
`not-observed`、partial / malformed / unknown enum、または still-present counter を伴わない fallback は
`cleanup-unverified` にします。fallback は attempted cleanup、run-owned Job、成功した初回 Job query、Job close
attempt と組にならなければならず、非 attempted cleanup に graceful state が現れる report も拒否します。
valid fallback でも mandatory terminal gates は緩和されません。
launch result では正規化後の `cleanupObservation.status` 自体も `succeeded` でなければならず、旧 identity
shape や raw の `processCleanupVerified=true` でこの gate を迂回できません。したがって partial な graceful
fields や Job close failure は必ず `cleanup-unverified` になります。typed な pre-launch failure だけは、構造化済み
startup milestones が process 未開始を証明する場合に限り、明示的な `not-attempted` cleanup state を保持できます。

`acceptance.qualified` は必要な launch 数と cleanup がそろった収集判定です。qualified mode は
`-CppBuildManifest` / `-RustBuildManifest` と
`-CppRuntimeStageDirectory` / `-RustRuntimeStageDirectory` を必須とします。manifest は現在の source
state、artifact、Output/UTF-16 selector、Debug/Release、runtime receipt と dependency closure、Windows
image、power mode、parallelism、MSVC/Rust toolchain、Cargo lock、package plan、build command の identity
を含み、runner は二つの manifest の共通条件も照合します。build 後に手書きで補うものではありません。
`prepare-output-startup-artifact.ps1` が build、`dumpbin` と構成別の Output authority selector 証明、canonical
runtime stage、manifest の atomic publish を一つの bounded transaction として所有します。Debug は
`dumpbin` の未解決参照、Release LTCG は匿名 object 形式と `CL.command.1.tlog` の単一 provider source
compile command（`/GL` と selector define 数）で検証します。qualified mode
では clean checkout が必須です。runner は campaign 後、report serialization 直前、atomic write 直後に
source state と両 measurement script の hash を再確認し、drift があれば typed integrity failure として
HOLD を維持します。receipt の `artifact_id` / role / source / destination は canonical stage layout へ
厳密に結び付けられ、basename だけの一致、absolute path、traversal、Windows の曖昧な予約名は拒否されます。

producer の source-state failure envelope は payload-free のまま、`failure.stage` に大分類、
`failure.substage` に失敗した source-state substep、`failure.code` に安定した機械可読コードを記録します。
Rust toolchain identity の取得では、Windows PowerShell 5.1 の pipeline による `$LASTEXITCODE` の上書きを
避けるため native command の終了コードを出力変換より先に取得し、出力は bounded な単一行として検証します。
失敗時に Rust の version 出力、path、command、例外本文は manifest へコピーしません。

### qualified producer の最終イメージ証跡

Issue #274 の qualified producer は、既存の `build-dev.bat` を呼び出す経路とは別の
明示的な opt-in です。clean な exact source checkout で `-QualifiedFinalImage` と新規の
`-FinalImageStageRoot` を指定してください。qualified 実行は package plan の後に canonical
`inventory observe-product --rebuild --final-image-backend ... --final-image-stage-root ...`
を一度だけ実行します。observer 内の package closure と Rebuild は順番に実行されるため、
`PackageTimeoutSeconds + TimeoutSeconds + grace` が外側の所有 timeout になります。qualified
branch では `build-dev.bat` や別の二度目の build を実行しません。

qualified producer の `output-final-image-verify` barrier は `tools/build/sakura_build.py` の
no-build CLI を必ず呼びます。呼出しはちょうど三つで、(1) observer 完了直後、(2) manifest の
atomic write / publication 前、(3) transaction directory の move 後に、move 後の native evidence
path を使って実行します。これは同じ Rebuild を繰り返す三回の build ではなく、一回の observer
結果を三つの ownership 境界で再検証する順序です。barrier は graph や manifest を読みません。

barrier の成功 result は `ok=true` / `payloadFree=true` / `record=output-final-image-binding-validation`
と、`qualified` の `buildTarget=Rebuild` に対応する `boundNativeEvidenceSha256`、
`sourceNativeEvidenceSha256`、`stageId`、receipt path/hash、EXE/MAP の `path`・SHA-256・size、
provider summary を持ちます。PowerShell はこの canonical JSON、hash、receipt を実装せず、Python
validator の結果を manifest / summary に束ねます。producer manifest と成功 summary は
`qualifiedFinalImage=true`、`buildTarget=Rebuild`、bound/source native hash、receipt path/hash/stageId、
EXE/MAP identity、provider summary を保存します。

失敗時は producer が所有する transaction root と final-image stage root を削除し、残存数を含む
payload-free の typed envelope を返します。cleanup の検証不能は `cleanup-unverified` として主原因を
保持し、publication を完了扱いにせず adoption は常に `decision=HOLD` / `adoptionEligible=false` です。
qualified 成功時だけ final-image stage root を保持します。source drift、selector / platform /
configuration 不一致、receipt / MAP / artifact の mismatch、cleanup 検証不能はすべて fail closed です。
これは性能計測の `GO` や Rust 採用決定ではなく、後続の runtime-stage / paired GUI / ledger 検証に
渡す immutable な producer 証跡です。

Debug の qualified artifact pair は、clean checkout で backend ごとに新規 stage root を指定して次のように生成します。

```powershell
$artifactRoot = ".\build\evidence\output-startup-qualified\$(Get-Date -Format 'yyyyMMdd-HHmmss')"
$cppFinalImageRoot = ".\build\evidence\output-final-image\$(Get-Date -Format 'yyyyMMdd-HHmmss')-cpp"
pwsh -NoProfile -ExecutionPolicy Bypass -File .\tools\prepare-output-startup-artifact.ps1 `
  -Backend cpp -Platform x64 -Configuration Debug -BuildParallelism 1 -OutputDirectory $artifactRoot `
  -QualifiedFinalImage -FinalImageStageRoot $cppFinalImageRoot `
  -TimeoutSeconds 1800 -PackageTimeoutSeconds 1800
$rustFinalImageRoot = ".\build\evidence\output-final-image\$(Get-Date -Format 'yyyyMMdd-HHmmss')-rust"
pwsh -NoProfile -ExecutionPolicy Bypass -File .\tools\prepare-output-startup-artifact.ps1 `
  -Backend rust -Platform x64 -Configuration Debug -BuildParallelism 1 -OutputDirectory $artifactRoot `
  -QualifiedFinalImage -FinalImageStageRoot $rustFinalImageRoot `
  -TimeoutSeconds 1800 -PackageTimeoutSeconds 1800
```

各 backend の `Debug\<backend>\build-manifest.json`、`runtime-stage`、その中の `sakura.exe` に加えて、
producer の final-image receipt / native evidence を paired runner と ledger の入力に明示します。
Release cell は別の新規 output root と `-Configuration Release`、別の新規 final-image stage root で
同様に生成します。dirty checkout や manifest producer を通していない artifact は `-CollectOnly` に
限定し、qualified 証拠として扱いません。producer の `-SelfTest` は両方の PowerShell host で
MSBuild、Cargo、Python、runtime-stage コマンド、GUI を起動せずに transaction、cleanup、manifest、
Rust toolchain exit-code の契約だけを検証します。

`-QualifiedFinalImage` を指定しない既存経路は引き続き `build-dev.bat x64 <Configuration>` の
通常 Build であり、manifest / summary に `qualifiedFinalImage=false`、`buildTarget=Build`、
`qualification=non-qualified` を明示します。
この non-qualified 経路では final-image stage と三つの barrier を要求しません。dirty checkout や
stage root を伴わない既存の開発用 artifact は、qualified 証拠へ昇格させず従来どおり非 qualified として扱います。

これとは別に `performance` が measured `documentReadyMs` の C++ / Rust paired delta を集計し、Rust の
median は相対 2% かつ絶対 1 ms、p95 は相対 5% 以内というゲートを評価します。トップレベルの
`pass` は両方を満たした場合だけ true です。実行した二つの PowerShell スクリプト、CPU
manufacturer/model、OS version、physical/logical core 数も hash とともに記録されます。runner は
`startupGatePass` と明示的な `adoption.decision=HOLD` / `adoptionEligible=false` を別々に残します。
起動ゲートが通っても、Issue #274 の correctness、provider workload、build/package、Debug/Release、
MinGW、複数 hardware のゲートを代替せず、Rust default の採用を意味しません。

実機計測を行わずに順序、統計、スキーマ、affinity metadata、PID identity / parent-first cleanup
helper を確認するには次を使います。

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File .\tools\measure-output-startup.ps1 -SelfTest
```

`-SelfTest` は GUI を起動しません。通常の `measure-startup-performance.ps1` は単一の
`sakura.exe` のマイルストーン比較用であり、ペア証拠の既定 35 回 / backend の代用ではありません。
paired の self-test は旧 report fallback、bounded integer / enum、first-cause、process enumeration
equations、tracked 経路の identity-gap helper、および injected native-shaped invoker を使う本番
`Get-JobProcessRecords` 経路（fresh Job-membership による exact PID 不在だけを受理し、2 回目の Toolhelp
census を行わず、present / unavailable / malformed、invocation exception、strict conversion を拒否）を検証します。
launch と cleanup が同じ `jobIdentityObservation` を保持すること、affinity の historical/current/expired 整合も検証します。さらに 5 historical / 4 exact
current の affinity plan が current 4 件だけを read-back し、null / empty、unknown、duplicate、creation / path
mismatch を reject し、expired historical 1 件を失敗扱いにしないことを固定します。
同じ no-GUI contract は resolver が present member を拒否し続けること、graceful の exact counter transition
だけが Job-containment fallback を選ぶこと、first-cause telemetry が保持されること、および各 terminal gate
が独立に必須であることも固定します。

## 前提条件

- Windows 上で実行すること。対象の Release または Debug の `sakura.exe` が実在し、起動
  できること。
- 計測を始める前に、**同じ実行ファイル**を使う Sakura プロセスが存在しないこと。別ビルド
  や別パスのプロセスも、比較結果に影響し得るなら終了して条件を記録します。
- OS ファイルキャッシュの状態、電源モード、バックグラウンド負荷（ウイルス対策、同期、
  ビルド、ブラウザー等）を結果と一緒に記録すること。同一条件での変更前後比較を優先します。
- 入力は 31 物理行以上にします。本文レイアウト完了の外部判定が、Sakura の初期スクロール範囲
  （30 行）を誤って準備完了と見なさないためです。固定サンプルはこの条件を満たします。
- `CaptureScreenshot` を使うときは、対象ウィンドウを他のウィンドウで覆わないでください。実際に
  画面へ見えているウィンドウ矩形を保存するためです。

スクリプトは既定で試行ごとの一意なプロファイルを用います。通常の開発プロファイルを
ベンチマーク用に指定したり、結果だけを見てキャッシュ状態の異なる試行を混在させたりしないで
ください。

## 実行例

リポジトリのルートから実行します。パスは手元のビルド出力に合わせて置き換えてください。

```powershell
rtk proxy powershell -NoProfile -ExecutionPolicy Bypass -File tools\measure-startup-performance.ps1 `
  -SakuraExe .\x64\Release\sakura.exe `
  -SampleMarkdown .\tools\startup-benchmark-sample.md `
  -Iterations 5
```

既存プロファイルとの比較を追加する場合:

```powershell
rtk proxy powershell -NoProfile -ExecutionPolicy Bypass -File tools\measure-startup-performance.ps1 `
  -SakuraExe .\x64\Release\sakura.exe `
  -SampleMarkdown .\tools\startup-benchmark-sample.md `
  -Iterations 5 -CompareExistingProfile
```

表示結果を確認する画像も残す場合:

```powershell
rtk proxy powershell -NoProfile -ExecutionPolicy Bypass -File tools\measure-startup-performance.ps1 `
  -SakuraExe .\x64\Release\sakura.exe `
  -SampleMarkdown .\tools\startup-benchmark-sample.md `
  -Iterations 5 -CaptureScreenshot
```

PowerShell の計測補助だけを確認するセルフテストは次のように実行します。

```powershell
rtk proxy powershell -NoProfile -ExecutionPolicy Bypass -File tools\measure-startup-performance.ps1 -SelfTest
```

セルフテストは JSONL のスキーマ検査、QPC 差分からの `firstContentPaintedMs` の算出、破損行の
隔離、四つの process-diagnostic checkpoint、root exit state の `STILL_ACTIVE` 判定、paired trace の
allowlist 順序・role/value・elapsed 変換・256 件上限・malformed/clock 不整合の fail-closed も確認します。
さらに v2 containment state machine の全 terminal branch、operation/observer enum、payload-free shape、
query/time bound、および実際の non-GUI 2-member Job に対する TerminateJobObject 後の queryable handle と
zero membership before close を検証します。`-SelfTest` は Sakura、MSBuild、CMake、Cargo、Python、package
restore、runtime stage、GUI を起動せず、build artifact も生成しません。PowerShell 5.1
(`powershell.exe`) と PowerShell 7 (`pwsh`) の両方で
shared script と paired script の `-SelfTest` を実行してから実測へ進めます。

## アプリ内スタートアップトレース

計測スクリプトは、各 `condition` / `iteration` ごとに出力先の下へ専用の
`startup-trace-<runId>-iteration-<n>-<condition>` ディレクトリを作成し、対象プロセスへ
`SAKURA_STARTUP_TRACE_DIR` として渡します。アプリケーションは、この**既存のディレクトリ**が
明示的に指定されたときだけ、プロセスごとの `startup-trace-<pid>.jsonl` を書きます。エディタと
非表示コントロールプロセスは同じディレクトリに個別ファイルを出すため、プロセス間の待機や
ready 通知も同じ測定単位で相関できます。

この単体計測では、読み取った `startupTrace` の詳細を開発者向け結果へ残します。一方、Issue #274 の
paired runner は同じ trace directory を artifact bundle 配下に run-owned として作り、読み取り後に
allowlist 済み event 名の count、role count、最大 256 件の順序付き診断 projection へ変換してから削除します。
paired report の ordered item は `ordinal`、`event`、`role`、`value1`、`value2`、`elapsedMs` に限定され、trace の
path、directory、`detail`、payload、raw command は保持されません。trace が空または clock 不整合なら
`trace-unavailable` となり、trace cleanup の検証失敗も成功扱いになりません。この projection は診断専用で、
readiness や Rust 採用の証拠ではありません。

この環境変数が未設定、空、または既存ディレクトリを指さない通常起動では、アプリはトレース
ファイルやディレクトリを作成せず、イベント記録もしません。したがって通常の利用・配布ビルドの
起動経路にファイル I/O を追加しません。旧バイナリなど内部トレース未対応の実行ファイルをこの
スクリプトで測定した場合も、外形計測は有効で、`startupTrace.records` が空になります。

各 JSONL 行は schema version 1 の次の固定フィールドです。QPC は Windows 全体で単調な高分解能
カウンタであり、`qpc / frequency` の比較には同じ `frequency` を確認します。

| フィールド | 内容 |
|---|---|
| `schemaVersion` | 固定値 `1`。未知の値は集計対象にしません。 |
| `qpc`, `frequency` | イベント時刻と QPC 周波数。 |
| `pid`, `tid`, `role` | プロセス、スレッド、役割（`editor` / `control` など）。 |
| `event` | フェーズ境界・選択理由・同期状態を表す安定した ASCII 名。 |
| `value1`, `value2`, `detail` | 数値の補助値と短い分類文字列。レイアウト worker 数・判定理由などを格納します。 |

スクリプトは run-owned の全 Sakura プロセスが終了したことを確認してから JSONL を読みます。各 run の
`startupTrace` には `records` と `majorPhases`、既知の begin/end 組から算出した `phaseDurations`、解析
エラー、最初の `first_content_painted` レコード、および QPC 周波数が互換なときの
`firstContentPaintedMs` を統合します。`majorPhases` は将来のイベント名追加を取りこぼさないよう、現在は
記録された順序付きイベント集合そのものです。壊れた行は
`invalidLineCount` / `parseErrors` に隔離し、外形タイミングを黙って書き換えません。

`firstContentPaintedMs` は、起動描画トランザクションのコミット後、主本文ビュー（index 0）が実際の
レンダリングを完了して初めて記録する内部時点です。外部観測の `documentReadyMs` は全文レイアウトを
スクロール範囲で確認する完了時点なので、同じ指標ではありません。どちらも残し、初回の見え始めと全
レイアウトの回帰を別々に比較します。

### プロセス診断 checkpoint

各 launch の `startupDiagnostics` には `0.5s`、`2s`、`10s`、`timeout` の四つの固定 checkpoint が
あります。各項目は `observed` / `not-reached` / `unavailable` の終端を持ち、process tree は上限付きの
PID、親 PID、実行ファイル名、生成時刻、Job membership だけを記録します。root process の状態は、PID
再利用を避けるため launch 時に取得した process handle から `GetExitCodeProcess` で確認します。`STILL_ACTIVE`
（259）は `active` として扱い、終了コードとは解釈しません。観測 API が失敗した場合は
`unavailable` として残し、成功や終了を推測して補いません。

各 checkpoint には、観測に失敗した境界を bounded な
`failureSubstage` と数値の `failureErrorCode` として併記します。`failureSubstage` は
`root-exit`、`job-membership`、`process-census-first` / `process-identity-first`、
`process-census-second` / `process-identity-second`、`window-enumeration`、
`projection-finalize`（または `none`）の固定 allowlist だけを受け付け、未知の値、
型違い、範囲外の error code は `unavailable` として fail closed にします。この substage
の照合は大文字小文字を区別する ordinal 比較で行い、`process-identity` のような
総称値は受け付けません。
成功した後でも process census や window enumeration が失敗し得るため、この情報は
成功を推測するためではなく、`diagnostic-unavailable` の一次境界を payload-free に
切り分けるためのものです。
`failureSubstage` と `failureErrorCode` の組み合わせも checkpoint の状態と照合します。
`unavailable` は `none` 以外の substage と正の error code、`not-reached` は `none` と null
の error code を要求します。`observed` は `none`/null、または
`jobMembershipVerified=false`・bounded な Job query の failed・正の error code による
意図した Job-membership warning だけを許可し、その error code は
`jobQueryObservation.errorCode` と一致しなければなりません。両フィールドがともに欠落する場合は
legacy v1 として互換に扱いますが、片側だけの欠落やその他の矛盾は fail closed です。

paired runner の `startupDiagnostics` はこの schema を各 run に保ったまま、path・command line・caption・
本文などを含まない固定フィールドへ変換します。`startupTrace` も allowlist と固定 ordered item fields だけへ
変換し、最大件数を超えた場合は `orderedEventsTruncated` を設定します。到達した checkpoint が `unavailable` になる、または root
exit state の変換が不正な場合は `diagnostic-unavailable` / `failureStage=diagnostics` となり、その launch は
成功数や統計へ入りません。fast success で後続 checkpoint が `not-reached` のままなのは許容されます。
paired trace の `records` が空、malformed、または launch QPC clock と互換でない場合は `trace-unavailable` として
扱われ、成功数や統計へ入りません。trace が unavailable でも raw launch が timeout/survivor/startup failure なら、
その raw failure が primary のまま保持され、trace status は副次診断です。

### `phaseDurations` とイベントの補助値

`phaseDurations` は、同一 QPC 周波数で対応する begin/end レコードを結べた場合だけ算出します。通常の
同一プロセス phase は `factory`、`control_spawn`、`control_wait`、`control_ready_event`、`editor_spawn`、
`editor_wait`、`editor_ready_event`、`uipi_check`、`read`、`layout`、`startup_document`、
`startup_draw_commit` です。`read` は初期文書の同期読込、`layout` はその初期レイアウト、`uipi_check` は
エディタとコントロール間の UIPI 確認、`startup_draw_commit` は表示・一回のサイズ処理・再描画をまとめた
コミットを表します。`control_wait` / `editor_wait` は対応イベントを待った時間、`*_ready_event` は
`SetEvent` 呼出し自体の時間です。

さらに `control_ready_handoff` は control の `control_ready_event_end` から editor の
`control_wait_end`、`editor_ready_handoff` は editor の `editor_ready_event_end` から control の
`editor_wait_end` までを、PID ではなく役割で対応付けます。これは `SetEvent` が終わってから相手
プロセスが待機を抜けるまでのクロスプロセス handoff であり、個々の待機／通知 phase と混同しません。

`value1` / `value2` はイベント固有の結果を補います。生成成功などの真偽結果と
`*_ready_event_end`、`startup_draw_commit_end` では `value1` が成功 `1`、失敗 `0`、イベントハンドルまたは
IPC HWND が存在しない明示的な経路では `-1` です。後者の通知イベントの `value2` は Win32 error、コミットが
途中で中止された場合も `value2` は中止理由の Win32 error です。`*_wait_result` は `value1` に待機 API の
結果コードを持ちます。`uipi_check_end` は応答を得たかを `value1`（`1` / `0`、tray HWND なしは `-1`）に、
送信 error を `value2` に入れます。`layout_decision` は `value1` が行数、`value2` が worker 数、`detail` が
`below_minimum_lines` / `range_based_color` 等の固定理由です。未定義のイベント値を全イベント共通の成功値と
解釈してはいけません。

起動文書の内訳は、次の集計イベントで確認します。時間はすべて QPC tick であり、同じレコードの
`frequency` で換算します。`read` は読込全体、`decode` と `line_build` はその内訳なので、単純加算して
トランザクション全体と解釈してはいけません。

| イベント | `value1` | `value2` / `detail` |
|---|---|---|
| `startup_document_subphase_summary` | subphase の合計 QPC tick | 処理回数 / `pre_read_settings`, `read`, `decode`, `line_build`, `layout`, `post_load_finalize`, `workbench_ui`, `draw_commit` |
| `startup_read_decision_summary` | 入力 byte 数 | 有効な行境界 partition 数 |
| `startup_read_result_summary` | 公開した論理行数 | `EConvertResult` |
| `startup_read_worker_summary` | worker の合計 QPC tick | 完了した worker 計測数 |
| `startup_read_worker_lifecycle_summary` | 起動した worker 数 | 回収した worker 数 |
| `startup_read_transfer_summary` | 行 buffer の copy 回数 | move 回数 |
| `startup_layout_input_summary` | レイアウト対象行数 | 折返し幅 |
| `startup_minimap_cache_summary` | キャッシュ hit 数 | miss 数 |
| `startup_minimap_build_summary` | 再構築の合計 QPC tick | 走査したレイアウト行数 |
| `startup_make_one_line_summary` | `_MakeOneLine()` の合計 QPC tick | 呼出回数 |
| `startup_make_one_line_work_summary` | `_MakeOneLine()` の呼出回数 | 入力 UTF-16 code unit 数 |
| `startup_make_one_line_cost_summary` | 分類別の合計 QPC tick | 処理回数 / `kinsoku_and_word_inclusive`, `color_boundary`, `character_width`, `layout_allocation` |

`startup_read_worker_lifecycle_summary` の両値が一致しない結果は、速度比較に使わず読込終了経路を調査します。
`kinsoku_and_word_inclusive` は内側の文字幅・レイアウト生成を含み得るため、分類別 QPC tick も互いに排他的な
合計として扱いません。

`isa_dispatch` はプロセスごとに起動直後の一度だけ記録します。`value1` は選択した実装
（`1` = AVX、`2` = AVX2、`3` = AVX-512F/BW）、`value2` は CPUID・XGETBV と
ディスパッチ表初期化に要した QPC tick 数です。ミリ秒へ換算するときは、同じレコードの
`frequency` を使って `value2 * 1000 / frequency` とします。

トレースには文書本文、文書パス、ウィンドウキャプション、ユーザー設定値などの機密情報を入れては
いけません。`detail` は `range_based_color` のような固定分類値に限定します。保存された JSONL と
`report.json` を外部共有するときも、通常の出力に含まれる実行ファイル・サンプルの絶対パスを確認し、
機密入力を使った結果はリポジトリへコミットしません。

## 現状の起動ロジックと順序

通常起動の `sakura.exe -PROF=<name> <markdown>` は、必要なら同じプロファイル用の非表示
コントロールプロセスを先に起動し、その初期化を待ってからエディタウィンドウを作ります。現在の重要な
順序は、**子ビューと各バーの生成後に起動描画トランザクションを開始し、workbench の枠組み初期化と
初期文書の同期ロードを非表示・描画抑止のまま完了してから、一度だけ表示・描画をコミットする**ことです。
起動時の空文書に依存するアウトライン解析と拡張向け文書公開は実行せず、実文書の初回描画後に一つの
内部メッセージへまとめて完了します。

```mermaid
sequenceDiagram
    autonumber
    participant B as 外部ベンチマーク
    participant E as sakura.exe<br/>エディタプロセス
    participant F as CProcessFactory
    participant C as sakura.exe -NOWIN<br/>コントロールプロセス
    participant W as CEditApp / CEditWnd
    participant L as CDocFileOperation / CLoadAgent
    participant V as 主本文 CEditView
    participant Q as エディタのメッセージキュー

    B->>E: Process.Start("-PROF=&lt;name&gt; &lt;markdown&gt;")
    E-->>B: プロセス情報を返す
    Note right of B: processApiReturnMs<br/>子の初期化とは並行し得る
    E->>F: wWinMain → Create(command line)
    F->>F: コマンド解析・プロファイル確定
    F-->>E: CNormalProcess
    E->>E: Run → InitializeProcess

    alt 同じプロファイルのコントロールプロセスがない
        E->>C: CreateProcess("-NOWIN -PROF=&lt;name&gt;")
        C->>C: mutex・共有データ・tray window を初期化
        C->>C: SetEvent(control ready)
        C-->>E: control_ready_handoff<br/>待機解除
    else 既に存在する
        E->>E: コントロールプロセス起動を省略
    end

    E->>W: CEditApp::Create → CEditWnd::Create
    W->>W: CreateWindowEx("TextEditorWindow…")
    Note right of W: topLevelHwndMs
    W->>Q: SetTimer(IDT_FIRST_IDLE, 0)
    W->>W: view・bar・workbench を生成
    W->>W: BeginStartupDrawTransaction<br/>全初期 view の描画を抑止
    W->>C: UIPI check（tray HWND が無い場合も結果を記録）

    E->>W: OpenDocumentWhenStart
    W->>L: FileLoadWithoutAutoMacro
    L->>L: Check → Before → Load
    L->>L: 同期読込（read）→ SetLayoutInfo / 同期 layout
    L->>L: After → CLoadAgent::OnAfterLoad
    L->>W: UpdateCaption → SetWindowText
    Note right of W: captionReadyMs
    L->>L: Final → caret・scroll bar 更新<br/>再描画は抑止中

    E->>W: CommitStartupDrawTransaction
    W->>W: ShowWindow
    Note right of W: visibleMs
    W->>W: 保留した WM_SIZE / redraw を一回の OnSize2 に集約
    W->>W: 全 view の描画を復元<br/>scroll bar range を非描画で同期
    W->>W: startup document trace を complete
    W->>V: RedrawWindow(... RDW_UPDATENOW)
    V->>V: 実際の主本文レンダリング完了
    V-->>W: first_content_painted
    Note right of W: firstContentPaintedMs
    W->>W: 以前のタブはこの時点まで前面に残し、その後 hide
    B->>B: DwmFlush
    Note right of B: dwmFlushMs

    E->>Q: MainLoop
    Q->>W: WM_TIMER(IDT_FIRST_IDLE)
    W->>W: ready 設定・extension service 開始<br/>MYWM_FIRST_IDLE を post → KillTimer
    W->>Q: MYWM_COMPLETE_STARTUP_WORKBENCH を post
    Q->>W: 実文書を extension へ公開<br/>表示中なら outline を一度だけ解析
    B->>E: WaitForInputIdle
    Note right of B: inputIdleMs（任意診断）<br/>OS queue idle。hard readiness ではない
    B->>W: 縦 scrollbar range を poll
    Note right of B: documentReadyMs<br/>全物理行を含む layout range を確認
    B->>B: CopyFromScreen（指定時）
    Note right of B: スクリーンショットで最終表示を確認
```

根拠となる主な実装位置は、エントリが
[`WinMain.cpp:50`](../sakura_core/_main/WinMain.cpp#L50)、プロセス選択と control ready の待機が
[`CProcessFactory.cpp:49`](../sakura_core/_main/CProcessFactory.cpp#L49) と
[`CProcessFactory.cpp:234`](../sakura_core/_main/CProcessFactory.cpp#L234)、editor ready の待機が
[`CControlTray.cpp:1291`](../sakura_core/_main/CControlTray.cpp#L1291) です。初期化 mutex の
`WAIT_FAILED` はその場で失敗終端となり、`WAIT_ABANDONED` は mutex を保持したまま control 側へ
`MYWM_RECOVER_APPNODE` を同期送信して stale AppNode を復旧できた場合だけ続行します
([`CNormalProcess.cpp:166`](../sakura_core/_main/CNormalProcess.cpp#L166)、
[`CNormalProcess.cpp:593`](../sakura_core/_main/CNormalProcess.cpp#L593))。

`CEditWnd::Create` は子ビュー／bar／workbench の生成後に描画トランザクションを開始し、UIPI check は
tray HWND がない分岐でも明示的に終端イベントを残します
([`CEditWnd.cpp:1860`](../sakura_core/window/CEditWnd.cpp#L1860)、
[`CEditWnd.cpp:1931`](../sakura_core/window/CEditWnd.cpp#L1931))。起動ファイルの
`OpenDocumentWhenStart` は描画抑止中に `FileLoadWithoutAutoMacro` を同期実行します
([`CEditWnd.cpp:1978`](../sakura_core/window/CEditWnd.cpp#L1978))。実読込と `SetLayoutInfo`、タイトル更新、
Final 時の抑止判定は [`CLoadAgent.cpp:234`](../sakura_core/agent/CLoadAgent.cpp#L234) と
[`CLoadAgent.cpp:294`](../sakura_core/agent/CLoadAgent.cpp#L294) にあります。

通常の文書起動では [`CNormalProcess.cpp:433`](../sakura_core/_main/CNormalProcess.cpp#L433) の後に
[`CNormalProcess.cpp:517`](../sakura_core/_main/CNormalProcess.cpp#L517) が一回だけ
`CommitStartupDrawTransaction` を呼びます。コミットは `ShowWindow`、許可済みの一回の `OnSize2`、
描画スイッチ復元、全 view の scrollbar range 同期、startup document trace の complete、同期
`RedrawWindow` を順に実行します。抑止中の view `WM_SIZE` では `AdjustScrollBars` が何もしないため、
この明示的な同期を省くと全文 layout が完了していても外部の scrollbar maximum が初期値 `1` のまま
残ることがあります
([`CEditWnd.cpp:438`](../sakura_core/window/CEditWnd.cpp#L438))。主本文の実レンダリング後だけ
`first_content_painted` を発行し、前のタブを隠すのもその後です
([`CEditView_Paint.cpp:761`](../sakura_core/view/CEditView_Paint.cpp#L761)、
[`CEditWnd.cpp:422`](../sakura_core/window/CEditWnd.cpp#L422))。

workbench の初期化では、ロード直後に捨てる空文書のアウトライン解析と文書 snapshot を保留します。
`OnAfterLoad` は起動中の要求を重複させません。起動レイアウト中の `BlockingHook()` が 0 ms の
`IDT_FIRST_IDLE` を先に dispatch する場合もあるため、ready 状態だけでは完了処理を許可しません。
**first-idle、描画トランザクションの `Committed`、主本文の初回描画完了**がすべて成立した時だけ
`MYWM_COMPLETE_STARTUP_WORKBENCH` を一度 post します。各状態を確定する経路から同じ判定を再試行するため、
到達順序には依存しません。受信側は保留フラグを callback より先に消費し、実文書の公開と、右パネルが
表示中かつ outline が展開中の場合だけの解析を完了します。post に失敗した場合は、安全な三状態が成立
した時点で同じ完了処理を同期実行し、close 分岐では全保留・post済みフラグを破棄するため、保留状態が
暗黙の終端になりません
([`CEditWnd.cpp:803`](../sakura_core/window/CEditWnd.cpp#L803)、
[`CEditWnd.cpp:1680`](../sakura_core/window/CEditWnd.cpp#L1680)、
[`CEditWnd.cpp:4474`](../sakura_core/window/CEditWnd.cpp#L4474))。

したがって `visibleMs` はロード前の空の枠ではなく、コミット中の表示要求を観測する値です。ただし
`visibleMs`、`captionReadyMs`、`firstContentPaintedMs` はそれぞれ別の境界であり、全文レイアウトの外部確認を
表す主指標 `documentReadyMs` を置き換えません。起動表示を変更する際は、描画抑止・一回だけの commit・
実描画後までの前タブ保持を壊さず、各境界を分けて比較してください。

## 引数

| 引数 | 用途 |
|---|---|
| `SakuraExe` | 計測対象の `sakura.exe`。実在する Release または Debug 実行ファイルを指定します。 |
| `SampleMarkdown` | 開く固定 Markdown ファイル。通常は `tools/startup-benchmark-sample.md` を指定します。 |
| `Iterations` | 各条件の反復回数。比較の基本は 5 回です。 |
| `CompareExistingProfile` | 各反復で fresh 計測が生成した同じ一意プロファイルを再利用する条件を追加します。普段使いのプロファイルは使用しません。 |
| `CaptureScreenshot` | 各条件の第 1 反復について、全文レイアウト確定直後の表示確認用画像を画面から出力します。対象ウィンドウを覆わないでください。タイミング値の代替ではありません。 |
| `OutputDirectory` | 結果 JSON、必要に応じて画像を置くディレクトリ。未指定時は `~/tmp/sakuracode-startup-performance` です。 |
| `SelfTest` | 実アプリのベンチマークではなく、スクリプトの自己診断を実行します。 |

`-SelfTest` では通常の測定引数を組み合わせません。実測の前に自己診断を通し、実測では
`SakuraExe` と `SampleMarkdown` を明示指定する運用を推奨します。

## 計測時点

各試行は以下のマイルストーンをミリ秒で記録します。これらは「最初のコンテンツピクセル」の
直接計測ではなく、Windows API とアプリケーションの観測可能な状態を組み合わせた比較用の
proxy です。

| マイルストーン | 意味と注意点 |
|---|---|
| `processApiReturnMs` | Sakura 起動要求から、プロセス起動 API がプロセス情報を返すまで。 |
| `topLevelHwndMs` | 対象エディタのトップレベルウィンドウを検出した時点。 |
| `visibleMs` | そのウィンドウが可視状態になった時点。 |
| `dwmFlushMs` | DWM フラッシュ完了時点。合成へ反映を要求した指標であり、画面を人が見た時点そのものではありません。 |
| `captionReadyMs` | `CLoadAgent::OnLoad` のファイル読込と `SetLayoutInfo` の後、`OnAfterLoad` から更新される文書タイトルを外部観測する proxy。first content pixel ではありません。 |
| `inputIdleMs` | 任意診断。`WaitForInputIdle` が required readiness の成立前に完了した場合だけ記録します。process 内の任意の GUI thread が満たし得る一回限りの OS proxy であり、アプリ内部の `IDT_FIRST_IDLE`、main UI thread、layout、paint の証明ではありません。未観測時や query unavailable 時は `null` のままでも起動成功を妨げません。 |
| `documentReadyMs` | 本文ビューの縦スクロール範囲が入力の全物理行を含むまで拡張された時点。折返し後の layout 行数は入力の物理行数以上になるため、外部から全文レイアウト完了を確認する主指標です。 |

hard readiness は、対象 editor の top-level window、可視な対象文書 caption、全文を含む
scrollbar layout で判定します。`WaitForInputIdle` は初期化完了前に早く成立する場合と、同じ artifact でも
外部の caption/layout/paint 完了後まで成立しない場合があるため、比較用の任意診断に限定します。
`inputIdleMs` を `documentReadyMs` で補完・合成してはいけません。

## 結果 JSON の読み方

出力 JSON のトップレベルは `generatedAtUtc`、`scriptVersion`、`runId`、`reportPath`、
`configuration`、`environment`、`input`、`conditions`、`runs`、`summaries`、
`cleanupVerified` です。`input` は byte 数、行数、SHA-256 を含みます。`runs` の各要素には
`condition`、`iteration`、`processApiReturnMs`、`topLevelHwndMs`、`visibleMs`、`dwmFlushMs`、
`captionReadyMs`、`inputIdleMs`、`documentReadyMs`、`verticalScrollMaximum`、
`startupDiagnostics`、`startupTrace`、`inputIdleReached`、`success`、`error`、`screenshotPath`、
`containmentProof`、`processCleanupVerified`、`profileCleanupVerified`、`cleanupVerified` を記録します。
`containmentProof.version=2` がない、または `jobEmptyProven=false` の run は、他の cleanup field が clean でも
`processCleanupVerified=false` です。`summaries` は
7 マイルストーンごとに `count`、`medianMs`、`p95Ms`（nearest-rank ceiling）、`minMs`、`maxMs`、
`meanMs` を条件別に集計します。
`CaptureScreenshot` 時の画像名は
`startup-performance-<runId>-fresh-iteration-1.png` または
`startup-performance-<runId>-existing-profile-iteration-1.png` です。

`scriptVersion` をスキーマ上の版として扱い、キー名と全フィールドは生成された JSON を正本と
してください。未知の版では、旧版の集計スクリプトで機械的に解釈しません。

最終 `DwmFlush` と layout 後の 2 回目の `WaitForInputIdle` は採用していません。最初の
`WaitForInputIdle(0)` も任意診断であり hard gate ではありません。非アクティブな
ベンチマークウィンドウで、それぞれ約 23 秒ブロックする試行を再現したためです。どちらも
`documentReadyMs` で確認済みの本文レイアウト完了より後の compositor / workbench の静止待ちであり、
初回表示の回帰ゲートにすると結果を歪めます。

同じ理由で `PrintWindow` も使いません。大きな本文のウィンドウ 1 枚に約 23 秒かかる試行を再現した
ため、画像は `CopyFromScreen` で実際の可視ピクセルを取得します。スクリーンショット処理は全タイミング
値の記録後に実行されます。

読み取る際は次を守ります。

- 第一の比較値は各マイルストーンの**中央値**です。単発値はスケジューリングやキャッシュで
  大きく揺れます。
- 各試行の成功数、最小値・最大値（range）、失敗理由、プロセス／プロファイルの cleanup 結果も
  確認します。残存がある試行は成功として比較に混ぜません。
- `n=5` は軽量な比較のための標本です。`p95`、SLO、統計的な性能保証を主張する根拠には
  なりません。必要なら試行数を増やし、環境条件を固定して別途分析します。

## fresh と existing の意味

通常の **fresh** 条件では、入力した `sakura.exe` を計測専用の artifact bundle へコピーし、
bundle と同じ階層に UTF-16LE BOM/CRLF の `sakura.exe.ini`（`[Settings] MultiUser=0`）を
生成します。試行ごとに一意なプロファイルを bundle 内へ作って Sakura を起動するため、
`%APPDATA%` や利用者の設定へフォールバックしません。sidecar の欠落・改変・再解析点は
起動前に拒否されます。bundle は exe と sidecar だけで、DLL や隣接リソースを暗黙にコピーしません。
これは設定・履歴による差を抑え、再現性のある変更比較の基準にします。

`-CompareExistingProfile` を付けると、各反復の fresh 起動で作成・保存された同じ一意プロファイルを
もう一度使う **existingProfile** 条件も併記します。これは「設定ファイルが既にある 2 回目の起動」
との差を見るものであり、普段使いのプロファイルを読み込む機能ではありません。履歴や保存済み設定の
影響を含み得るため、結果では条件を区別し、existingProfile の値で fresh ベースラインを置き換えないで
ください。

一意なプロファイルと artifact bundle は試行終了後に削除されます。source/copy の SHA-256 は
前後で照合し、sidecar の contract と bundle cleanup も結果へ記録します。削除失敗、または
Sakura／その子プロセスの残存は失敗として扱い、原因を解消してから再計測します。

## 安全性と後始末

計測は全待機に timeout を設けます。timeout、ウィンドウ検出失敗、文書タイトル未到達、全文レイアウト未到達、
または cleanup 後の survivor は試行失敗です。入力待機の未観測や query unavailable は任意診断として保持し、
それ自体では試行を失敗にしません。スクリプトが終了できるのは、成功または明示的な失敗という終端状態に
到達した場合だけです。

root process の handle は `startupDiagnostics` の全 checkpoint と cleanup が終わるまで保持し、exit state の
観測に使います。process handle、thread handle、Job handle はそれぞれ独立した close 分岐で処理するため、
一つの close 失敗が別の所有資源の cleanup を省略しません。診断観測に失敗した場合も、観測不能を成功と置き換えず
typed な unavailable として結果へ残します。

cleanup の対象は当該試行が所有する Sakura プロセスと、その run-owned 子プロセスに限られます。
無関係な Sakura、他アプリケーション、ユーザーの作業中プロセスを kill しません。既定の出力先が
`~/tmp/` 配下なのも、リポジトリと通常のユーザーデータを汚さないためです。

終了は二段階です。まず可視の編集ウィンドウへ `WM_CLOSE` を送り、全ウィンドウの消滅を確認して
finalization を完了させます。次に、同じ試行が所有する非表示コントロールウィンドウへ終了要求を送り
ます。可視ウィンドウを所有していた PID がそのまま制御役として残る場合もあるため、PID の終了ではなく
可視ウィンドウの消滅を境界にします。非表示制御プロセスは編集ウィンドウがなくても常駐し得るため 1 秒の
猶予に限定します。期限内に残った場合だけ、親から先に強制終了し、最後に PID・生成時刻・実行パスを
再照合します。可視ウィンドウが応答しない場合も、測定専用プロファイルと未編集の固定入力に限った処理
なので、正常終了の総猶予は 3 秒です。

paired runner の trace directory も run-owned cleanup の対象です。削除前に所有 root、生成名、通常ディレクトリ、
reparse point でないことを検証し、削除後の不存在まで確認します。確認できない場合は `cleanup-unverified` として
後続 launch を抑止し、report の qualified/pass 判定を通しません。

## 再現性の記録テンプレート

比較結果を Issue、PR、または検証記録へ残すときは、少なくとも次を記録してください。

```text
commit:
SakuraExe: （絶対パス、Release/Debug、file version、計測日時）
sample: startup-benchmark-sample.md （bytes、lines、hash）
machine / OS:
power mode:
cache state: （cold / warm、前処理の有無）
background load: （実行中の主な負荷）
condition: fresh / existingProfile
per-run: （各マイルストーンの値、成功/失敗）
median / range: （条件・マイルストーン別）
cleanup: （run-owned process と一意プロファイルが残っていないこと）
screenshot: （有無と出力パス）
internal trace: （`firstContentPaintedMs`、主要 phase、worker 数/理由、read/layout/UIPI/control wait/SetEvent の時間、解析エラー）
```

## 既存の Workbench 計測との違い

[`measure-workbench-performance.ps1`](measure-workbench-performance.ps1) は terminal / workbench の
メモリ等を扱う別責務の計測です。本書の startup benchmark は、固定文書を開く Sakura の起動表示・
読込・入力待機の時点を比較するためのものです。目的、入力、指標、結果を相互に混在させないで
ください。

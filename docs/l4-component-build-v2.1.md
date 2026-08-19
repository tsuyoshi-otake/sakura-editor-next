# Issue #15 設計追補 v2.1 — L4 分割の graduation gate

## 再レビューの結論

2026-08-04 の第三者再レビューを、Issue #15 の既存設計、公開 `master`、作業ツリーと照合した。既存設計の方向性は維持するが、設計成熟度は **72/100**、実装・証拠は **17/100** と再評価する。仕様追補だけで到達できる上限は 94 点であり、残りは測定と互換証拠でのみ獲得できる。したがって R0 は開始可、R1 は R1a/R1b に分割し、葉抽出 R2 は R1a が green になるまで開始しない。

依存矢印は一貫して `consumer -> dependency/provider` とする。manifest、生成投影、MSBuild/CMake の参照、include/link/runtime 観測の向きが一致しない場合は、見た目上プロジェクトが分かれていても L4 不合格である。

## 判定の修正

| ID | 判定 | 採用する具体策 |
|---|---|---|
| B-01 | 条件付き採用 | consumer-owned port は採用する。signature 類似度と port 数は診断に留め、hard gate にしない。 |
| B-02 | 分割採用 | 製品所有の決定論的 runtime 依存は hard evidence とする。ambient OS access は診断扱い。 |
| B-03 | 修正採用 | SemanticGraph `S`、ContextProjection `G`、hard evidence `H`、diagnostic `D` を区別する。`S=G=H=D` は要求しない。 |
| B-04 | 修正採用 | 最小 role probe と、実際の `sakura.exe` を使う product activation probe を併用する。 |
| B-05 | 修正採用 | manifest policy、生成定数、C++ typed state machine、trace を採用する。汎用 lifecycle execution IR は作らない。 |
| B-06 | 分割採用 | C++ ABI、wire、永続形式、resource/message ID ごとの compatibility matrix を持つ。 |
| B-07 | 分割採用 | package-set root と restore 回数は hard gate。path-to-package 推定は diagnostic。 |
| B-08 | 修正採用 | `/m:P`、`/MP:C`、`--parallel J` を明示する。独自 weighted jobserver は作らない。ここで採用した `P*C<=J` の MSBuild 側は 2026-08-19 の実測で Issue #201 により置き換えた。`tests1` が `sakura` を `ProjectReference` するため支配的な局面では project 並列が効かず、`P*C<=J` は 16 論理 CPU を 4 並列に落としていた。現在は `P=C=J` とし、CMake 側の `--parallel J` はそのまま。 |
| B-09 | 修正採用 | migration seam を contract、adapter、legacy façade の三種に限定する。全依存を port 化しない。 |
| B-10 | 修正採用 | stable `test_id` と runtime selector を分離する。既存全テストへの手動 semantic ID は要求しない。 |
| B-11 | 分割採用 | `S -> ContextProjection/ProjectModel`、CLI は BuildIntent、実行は native backend scheduler とする。第二の build system は作らない。 |
| B-12 | 修正採用 | project compile、C++ contract edge、link/instrumentation の profile を分離する。巨大な単一 ABI hash は作らない。 |
| B-13 | 採用 | strict closed schema v3、generator version、graph hash、決定論的 `generate --check` を R1a hard gate とする。 |
| B-14 | 採用 | Visual Studio から直接ビルドした場合も stamp staleness を検出する。CLI を迂回した無検証生成は禁止する。 |
| B-15 | 採用 | 大文字小文字、Unicode、空白、長い path、junction、response file を path matrix で検証する。 |
| B-16 | 条件付き採用 | incremental state と debugger capability は R2 graduation で検証する。 |
| B-17 | 採用 | static archive、whole-archive、PDB、最終 link のスケールを R3 前に測る。 |
| B-18 | 採用 | product artifact/resource/distribution provenance を R10 gate に追加する。 |
| B-19 | 採用 | R1 の all-or-nothing を廃止して R1a pilot enabling proof と R1b scale/diagnostic に分割する。 |

## R0–R10 の gate

- **R0 — inventory:** 現行の include/link/generated/resource/package/runtime/test/state 依存と、同一保証範囲の性能基準を取得する。未観測を「依存なし」としない。
- **R1a — pilot enabling proof:** strict schema v3、condition grammar、generator/version/hash stamp、`generate --check`、Debug x64 ContextProjection、MSBuild/CMake 投影、canonical CLI/BuildIntent、`J=1`、旧 test inventory、compile profile 分離、IDE stamp check、基本 path matrix、standalone revert を green にする。
- **R1b — scale/diagnostic:** Release/MinGW、runtime diagnostic、package共有/GC、`J>1` 校正、IDE評価予算、coverage、全 path matrix、link/PDB scaling、distribution provenance を追加する。R2と並行可能だが、各葉の graduation に関係する項目は先に満たす。
- **R2 — first leaf:** URI候補について所有ファイル、public/private API、全分類辺、単独compile/link/test、rebuild closureを証明する。R1a green 前には開始しない。
- **R3 — foundation/platform:** fan-inとstatic link/PDB増加を測り、循環・global initializer・pragma linkをゼロにする。
- **R4 — protocol/control:** Control client/server、endpoint/profile identity、golden wire fixture、Editorからserver/storageへの禁止辺を検証する。
- **R5 — domain/editor:** config/workspace/document/editor/terminal/debugの状態所有者と全終端状態を検証する。
- **R6 — workbench:** 各葉を狭いcontractへ分離し、runtime/service locator/汎用event busを葉から排除する。
- **R7 — extension:** protocol/transport/host/registry/contributionsのwire互換とprocess cleanupを検証する。
- **R8 — legacy/resource/win32:** façade、shared memory、Function Code、resource/message ID、language DLLの互換証拠を固定する。
- **R9 — composition:** Control/Editor/Forwarderのfactoryとproduct activation probeを分離し、bootstrapだけが全roleを知る状態にする。
- **R10 — removal/distribution:** `tests1`、legacy monolith、allowlist、global include/object link/root restoreを削除し、配布物provenanceと全互換gateを通す。

### 2026-08-04 R0 inventory実測

canonical CLIに`inventory repository`を追加した。これは台帳の収集成功と卒業判定を分離する。
baseline modeは証拠を収集できればexit 0、`--strict`は未分類辺または未観測classが残ればexit 5とする。
同一入力ではroot非依存hard evidence hashとJSON内容が一致し、内容不変ならevidence fileのmtimeも更新しない。

現時点の実測は2,673 C/C++/resource候補、所有範囲1,373 C/C++ file、8,647 includeである。
未所有file 27、未所有providerへのinclude 250、未解決quoted include 17、source内link directive 17を観測した。
includeはlexical scanだけなのでpartialであり、native ownership/link/generated/resource/package/test fixtureもpartial、
runtime asset/state/protocolはnot-observedである。`sakura_app`は`UriIdentity.cpp`を直接compileし、生成`sakura_uri.vcxproj`を参照せず、manifestにも
`sakura_app -> sakura_uri` compile/link edgeがない。CMake本体は`GLOB_RECURSE`を使用している。したがって
`collection_ok=true`だが`graduation_ready=false`、URIは`embedded_in_product`かつ`independent=false`であり、R0/R2完了を宣言しない。

## R1a の実装契約

唯一の意味モデルは `src/main/modules/modules.json` とし、component、contract、artifact、typed edgeを別nodeとして持つ。strict parserは未知field、全node種別を横断する重複ID、repository外path、component間の所有path重複、未知owner/endpoint/context、列挙外tool IDを拒否する。生成物はgraph hash、schema version、generator versionを持ち、通常ビルド中には暗黙更新せず、`generate --check`でstaleなら規範終了コード4とする。

canonical build API は `py -3 tools/build/sakura_build.py`（Windowsでは `sakura-build.bat`）とする。既存 `.bat` は argv と正確な exit code を転送する互換shimに限定する。CLIは検証、tool discovery、BuildIntent、restore/staging方針、parallel budgetを所有するが、低レベルtask graphは再実装せずMSBuild/CMakeへ渡す。

MSBuild直呼び出しでは、通常buildだけがmanifest、schema、compile profile、generator、committed generated fileを入力とするread-only staleness checkを実行する。`DesignTimeBuild=true`はPython、restore、code generation、staging、nested build、runtime probeを一切起動せず、committed generated propsを読み、そのstampが存在し型として有効であることだけをMSBuild propertyで検査する。意味入力と生成物のhash再計算はactual build/CIの責務とする。診断用escape hatchを使ったbuildはuncertifiedでありgraduation evidenceに使用しない。

compile closureはroot ownerのprivate edgeを含めるが、依存先ownerのprivate compile edgeをさらにconsumerへ伝播しない。依存先から伝播できるのは`visibility=public`のcompile edgeだけとする。compile、link、runtime、lifecycleのSCCは別々に判定する。

B-12のprofileはproject compile、C++ contract edge、最終link/instrumentationの三層とする。C++ edgeは`consumer -> provider`ごとに契約が要求するfieldだけを比較し、未知fieldと不一致をgraph checkで拒否する。MSVCではedge名・field名ごとの`#pragma detect_mismatch`を両端へforce-includeしてlink時にも補助検査するが、一つの巨大なABI hashは作らない。opaque C handle契約はC++ value/type境界より狭いfield集合を要求し、無関係なpackやiterator設定を理由に拒否しない。

現時点のmanifestは、実在する `sakura_app` と `tests1` を `legacy` として記録する。未抽出のURIや葉を `independent` と宣言しない。最初の葉抽出はR1aの全hard gateが成功し、baseline evidenceが揃った後に別変更として行う。

## 性能受け入れ

局所性能は移行前後で同一機能・同一test保証を比較する。初期時間を `T0`、測定で除去可能と判断したcritical pathを `A`、`U=A/T0` とし、目標改善率は `min(50%, 0.8*U)` とする。計算結果が10%未満なら時間目標はdiagnosticとし、正確なrebuild closureをhard gateとする。full cleanは `min(15%, 0.8*U)`、full no-op/solution evaluation/DesignTimeBuildの悪化上限は `max(10%, 250ms)` とする。Release LTCG最終linkは局所指標へ混ぜない。

## Verify / Expect

- `Verify:` strict parser unit testと `manifest check`。`Expect:` 未知field、path escape、所有重複、重複ID、未知辺を決定論的error codeで拒否する。
- `Verify:` `generate`を二回実行してmtime/hashを比較し、続けて `generate --check`。`Expect:` 二回目は変更ゼロ、check成功。入力またはcommitted generated file変更時だけstale終了4。
- `Verify:` 全contextでSCCとrequired witnessを検査。`Expect:` consumer→dependency方向の循環ゼロ、必須辺の証拠漏れゼロ。
- `Verify:` MSVC native fixtureでprovider `/Zp8`対consumer `/Zp1`、`_ITERATOR_DEBUG_LEVEL=2`対`0`、およびpack差のあるopaque C境界をcompile/linkする。`Expect:` C++ value/type境界はそれぞれ`default_pack`、`iterator_debug_level`を名指ししてLNK2038となり、opaque C境界はlink・実行に成功する。
- `Verify:` `plan component`とDebug x64 dry-run。`Expect:` BuildIntentのclosureとbackend targetが決定論的で、`J=1`はMSBuild `/m:1`、MSVC `/MP1`、CMake/CTest `--parallel 1`を明示する。
- `Verify:` legacy `.bat` のargv/exit-code test。`Expect:` shim内にtool discovery、validation、build orchestrationが存在しない。
- `Verify:` 変更範囲に応じDebug/Release、MSBuild/CMake/MinGW、DesignTimeBuildを実行。`Expect:` 未検証経路はgreenと記録せず、次gateの残余riskとして残す。
- `Verify:` `verify rebuild-closure <leaf-test> --contexts <MSBuild,CMake> --samples 5`を隔離コピーで実行する。`Expect:` clean/no-op/private cpp/public contractごとのobject/archive/link集合が宣言閉包と完全一致し、生成projection書換え、package restore、余分な明示CMake configure、test失敗、workspace/process残存がゼロ。private header、PCH、resource、generated inputを所有しないpilotでは、それらを未検証として残す。

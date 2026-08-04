# Issue #15 — R0台帳・R1a基盤・URI candidate／ABI・rebuild gate ステータス（2026-08-04）

本書は、Issue #15 R1aの統合作業で確認できた事実を記録する。URIは独立候補のcandidate
vertical sliceとして扱い、L4独立性、`sakura_app`/`tests1`の置換、またはR1a完了は宣言しない。

## 統合した基盤

- SemanticGraphのpublic usage closureとfinal static-link closureを分離した。`propagation=none`は
  public propagationだけを止め、providerが必要とするprivate/transitive static archiveはfinal link
  closureに残す。
- compile-only edgeはMSBuild/CMakeのnative link dependencyを作らず、providerのpublic include rootだけを
  consumerへ渡す。link edgeだけがProjectReference/`target_link_libraries`になる。
- target-scoped stale checkは、committed stampのcontent hashと選択contextのsemantic native closureだけを
  検査する。全projectionの再描画、generated root全走査、別context専用providerの追跡は行わない。
  repository全体のunexpected/different outputはfull `generate --check`が引き続き検査する。
- 条件付きlink edgeのMSBuild `ProjectReference`はactiveな`Configuration|Platform`だけで有効化する。
  同一componentが大文字小文字違いを含む同じMSBuild物理キーを複数contextへ割り当てた場合はmanifestを拒否する。
- component link evidenceはMAP内のprovider名だけでは成功せず、期待するarchive memberが実際に選択された場合だけ
  成功する。
- `contract`/`aggregate`はsource、private header、private include root、state ownerを持てない。header-only
  projectionはMSBuild Utility、CMake INTERFACEとし、compile/archive/link成果物を作らない。
- component CMake childだけ、toolchain/package discoveryに使われる環境変数をcase-insensitiveに除去する。
  explicit toolchain environmentを構築した後に適用し、legacy/full commandの環境は変更しない。
- project compile profileとC++ contract edge profileを分離し、`consumer -> provider`の各edgeで
  contractが要求するfieldだけを比較する。C++ value/type境界は10 field、opaque C handle境界は
  `abi_family`と`arch`だけを要求し、巨大な全体ABI hashによる過剰拒否は行わない。
- MSVCではedge名とfield名を含む`#pragma detect_mismatch` headerをconsumer/provider双方へ
  force-includeする。graph checkが主検査であり、link guardは補助的な取りこぼし防止である。
- generator versionを0.3.4へ上げ、manifest minimum version、content-hash付きstamp、MSBuild props、
  CMake fragmentを整合させた。
- basic path matrixはnormal、ASCII space、日本語の3 checkoutでcanonical MSBuild/CMake component testと
  boundary evidenceを実行する。checkout rootを除外したhard evidence hashを導入し、3 caseのsemantic graph、
  declared/observed input、link provider/MAP member、package/import policyが同一であることを比較する。
- VS付属CMake 3.31.6が日本語source/build pathで`0xC0000409`終了する実測に対し、CMake childだけ短命ASCII
  directory junctionを使う。`os.path.samefile`で同一checkoutを確認し、build treeのidentity markerからevidence
  時に同じpath spellingを再生成する。success/failure/timeoutでrun-owned junctionを削除する。
- URI candidateをisolated manifestから除去して再生成するstandalone revert rehearsalを追加した。legacy project、
  filters、tests1 project、所有sourceのhash/参照を維持し、candidate生成projectだけが消えることを検査する。
- `verify rebuild-closure`は`~/tmp`配下の短い隔離コピーでclean、5回no-op、private cpp、public contract、
  MSBuild DesignTimeBuildを実行する。各phaseでobject/archive/executableのmtime・size・hashを比較し、期待した
  compile/archive/link閉包との過不足、生成projectionの書換え、package restore、component test終了値を検査する。
  元checkoutのsource/headerは変更しない。MSVC compiler probeがgenerated projectの
  `CMAKE_OBJECT_PATH_MAX`適用前にlegacy path長制限へ到達するため、既定workspace名は意図的に短くしている。
- component CMake commandは、有効なcache、native build file、source root、generator、configurationが一致する場合、
  明示configureを省略してnative buildへ直接渡す。CMake入力が変わった場合の再configureはNinja/Makefilesの
  build graphが所有し、moved checkout、cache欠落、generator/configuration変更では明示configureへ戻る。
- R0 repository inventoryは、manifest所有範囲のC/C++ include、未所有source/resource、ソース内
  `#pragma comment(lib)`、MSBuildの製品source/ProjectReference、CMake `GLOB_RECURSE`に加え、生成header/target、
  RC/RC2 include、MSBuild link dependency、testによる製品object取り込み、root vcpkg manifestとrestore triggerを
  一つのroot非依存evidenceへ記録する。依存方向は一貫して`consumer -> dependency/provider`である。通常収集の成功と
  graduation判定を分離し、`--strict`は未分類・未観測が残る間は終了コード5にする。全witnessはJSONへ保存し、
  CLIは有界な件数要約だけを返す。
- `inventory observe-product`は静的台帳と別schemaでMSBuild製品Build後のtrackerを収集する。compiler/PCH、
  source-controlled/生成input、RC input、link object/resource/libraryを記録し、graph/context/productと全観測入力の
  SHA-256でstalenessを検査する。欠落、変更、別context、Build未実行snapshotはgreenにしない。merge後の依存方向も
  `consumer -> dependency/provider`とし、MAPのないDebug linkではselected archive memberを未観測のまま残す。
- 同じ観測に属するMSBuild diagnostic logから、対象generator targetの開始、終端状態、`Exec` task、宣言outputを
  抽出する。通常Buildのup-to-date skipはschedulingだけ、`--rebuild`で確認した`Exec`だけをexecutionとして扱い、
  outputの厳密なpathがcompiler/RC/link inputへ現れた場合だけproducerとconsumerを相関する。diagnostic logは実行ごとの
  一意な一時fileで、収集後に削除する。生成規則の`Inputs`/`DEPENDS`欠落は実行観測とは別blockerにする。

## 今回のVerify / Expect

| Verify | Expect / 実結果 |
|---|---|
| `rtk proxy py -3 -m unittest discover -s tools/build/tests -p "test_*.py" -v` | exit 0、75 tests passed。敵対的台帳fixtureに加え、native trackerのcompiler/PCH/生成header/RC/link input、source変更によるstale、context不一致、Build未実行snapshot、tracker欠落、壊れたevidence JSON、generatorの実行/up-to-date/condition-false/無関係target混入、manifest link input、hard-evidence tamper、入力宣言欠落を検証した。MAPなしでselected archive memberを誤ってobservedにしない。 |
| `rtk proxy py -3 -m compileall -q tools/build` | exit 0。Python build CLI一式の構文検査に成功した。 |
| `rtk proxy py -3 tools/build/sakura_build.py manifest check --format json` | exit 0、schema 3、graph hash `sha256:323f9ab6f5e344a6e2a18c6ce9ab435d94b9c877b9a3c8a2cf45cc4ba75ec47c`。 |
| `rtk proxy python tools/build/sakura_build.py graph check --all-contexts --format json` | exit 0、5 context、SCC failure 0、witness failure 0。 |
| `rtk proxy python tools/build/sakura_build.py generate --format json` | exit 0。generator 0.3.4からMSBuild/CMake projectionとcontext別ABI headerを再生成した。生成rootの候補worktree全置換は行っていない。 |
| `rtk proxy python tools/build/sakura_build.py generate --check --format json` | exit 0、`stale=[]`。 |
| URI MSBuild/CMake Debug canonical component test | MSBuild exit 0（0 warning/0 error）、CMake build exit 0、CTest 1/1 passed。 |
| `verify component-boundary`（MSBuild/CMake） | 両方exit 0。ABI headerを含むdeclared/observed repo inputsは5件で一致、link provider/map memberは`sakura_uri.lib`、package restore false、root import suppression true、failures空。MSBuild external input 201、CMake 0。 |
| Debug限定edgeのMSBuild評価 | fixtureを実projectへ生成し、`-getItem:ProjectReference`をDebug/Releaseで評価。Debugは`provider.vcxproj`、Releaseは空。 |
| 生成URI test projectのDesignTimeBuild | `DesignTimeBuild=true;SkipCompilerExecution=true`、`ClCompile`がexit 0、0 warning/0 error。 |
| `build fixture abi-pack-mismatch` / `abi-iterator-mismatch` / `abi-opaque-compatible` | 3件ともCLI exit 0。最初の2件はnative linkがLNK2038となり、それぞれ`sakura.edge.abi-fixture.default_pack`、`sakura.edge.abi-fixture.iterator_debug_level`を診断して`expected_failure`へ終端した。opaque Cはpack差があってもlink/runtime exit 0。 |
| `path-matrix test component sakura_uri_tests --contexts msvc-x64-debug,cmake-msvc-x64-debug --jobs 1` | exit 0。normal/ASCII space/日本語の両backend test/evidenceが成功し、semantic graph hashとbackend別hard evidence hashが全case一致。standalone revertのlegacy hash/source/reference維持とcandidate生成project除去が成功。workspace cleanup true。 |
| `verify rebuild-closure sakura_uri_tests --contexts msvc-x64-debug,cmake-msvc-x64-debug --jobs 1 --samples 5` | exit 0、両backendでclean/no-op/private cpp/public contractの期待compile/archive/link集合と観測集合が完全一致。全phaseでprojection変更0、package restore 0、test exit 0、workspace cleanup true。no-opの明示CMake configureは5回とも0。evidence-mode native child中央値はMSBuild 363.451 ms、CMake/Ninja 73.755 ms、MSBuild DesignTimeBuildは267.125 ms。いずれも移行前比較を持たないbaseline-onlyで、canonical CLI全体の性能値や改善率とは扱わない。 |
| `inventory observe-product --context msvc-x64-debug --product sakura_app --jobs 1 --rebuild` | MSBuild製品Rebuildと収集はexit 0。568翻訳単位、source input 1,162、生成header 16、PCH create 1/use 558/none 9、RC unit 1、link object 568/resource 1、repository library 6/external library 35を観測した。13 generator targetの終端状態を記録し、実`Exec`を伴うproducerからFunccode define→RC、Funccode enum→compiler、manifest→link、version→compiler/RCの5消費を厳密pathで相関した。native evidence hashは`sha256:30a6e9f26059a94813f987ca6515dffca77b76f7cb0b78d761a163df65988bcd`。selected archive memberと実package restoreは`false`のまま。 |
| `inventory repository ... --native-evidence build/evidence/r0/native-msbuild-product.json` | 収集exit 0、`collection_ok=true`、`graduation_ready=false`。静的件数はrepository C/C++/resource候補2,678、scan済みC/C++ 1,378、include 8,709、未所有file 27、未所有provider include 250、未解決quoted include 17、source link directive 17。native compiler/PCH、生成input消費、generator scheduling/execution、5 producer-consumer相関、RC input、link input setをvalid evidenceとしてmergeし、`sakura_app -> sakura_uri`が`UriIdentity.obj`を直接linkする1 witnessを記録した。一方、CMake custom command等に入力宣言のない生成規則10件をblockerとして記録した。selected archive member、resource table、package restoreは未観測。hard evidence hashは`sha256:77db103cec56682ccbbe273d15db7e59496f6ba20b5ee305bee4e89ab2b92d6e`。 |
| 同inventoryの`--strict`と決定性再収集 | 規範終了コード5。46 blocker recordを要約し、10 dependency classすべてがpartial/not-observedのためR0卒業を拒否した。再収集前後のJSON SHA-256は`fd3bc579e7519242caa3ab367bd9d29ec980d7e1af6b9cd1da7df4662e0ba24c`で一致し、mtimeも不変だった。これは期待したred gateであり、収集失敗ではない。 |
| component `plan`/dry-run、legacy Release、MinGW | plan/dry-runはexit 0。legacy Release、MinGWは未実行。未検証をPASSとは扱わない。 |

生成器とmodelのunit fixtureでは、3-nodeの`consumer -> provider -> private-provider` final-link closure、
compile-only include/non-link投影、MSBuild Utility/CMake INTERFACE、MSBuild ProjectReference closureと
CMake root-only stale scopeを確認した。これはnative URI実行や製品consumerのsymbol解決を証明するものではない。

## 4候補の採否

- core candidate (`c943`): 採用。public/final closure、compile-only include境界、generator 0.3.2基盤（現0.3.4）、component
  evidenceのfinal-link入力を統合した。ただし候補報告に含まれるnative URI成功値と完了宣言は採用せず、mainで
  再実行したDebug結果だけを今回の実証として記録している。
- scoped staleness (`bcf3`): 補正して採用。MSBuild XMLをcontext無視で追跡する方式を廃止し、semantic
  final-link closureとcommitted content hashを使うtarget-scoped実装へ変更した。full checkの全生成物検査は維持した。
- contract interface (`c4e9`): 採用。source/private header/private include/state/runtime artifactの負例と、header-only
  Utility/INTERFACE投影を採用した。generic thread/OS handle IRは追加していない。
- CMake env isolation (`272c`): 採用。explicit overlay後のcase-insensitive discovery env除去と、child failure時のterminal
  exit stateテストを採用した。legacy/full envを保持するテストも追加した。

4候補の単独unit結果（core 34、stale 27、contract 29、env 28）は比較材料であり、単純なtest count合算ではない。
初回27件PASSは、target scopeだけでR1a完了とする根拠から差し戻し済みとして記録する。

## 未完了・残余risk

- URIのMSBuild/CMake/MSVC Debug実build・CTest、component-boundary evidence、basic path matrix、standalone
  revertは検証済みだが、Release/MinGW matrixは未検証である。
- 生成URI test projectのDesignTimeBuildは検証済み。legacy Debug/Release、MinGWは変更影響と利用可能toolchainを
  確認後に別gateとして実行する。
- URI candidateのclean/no-op/private cpp/public contract閉包はMSBuild/CMake Debugで検証済みだが、private header、
  PCH、resource、generated inputの各mutationと、product rootへのconsumer接続、legacy monolith到達性、repository全体の
  package/root provenanceは未完了である。今回のnative ABI link fixtureはMSVC Debug限定であり、MinGWには
  graph check以外の同等link guardをまだ実証していない。junction checkout、long path、response fileはR1b path matrixへ残る。
- URI実装`UriIdentity.cpp`は現在も`sakura_core/sakura.vcxproj`へ直接含まれ、製品から生成URI projectへの
  `ProjectReference`とmanifestの`consumer -> provider` compile/link edgeはない。CMake本体も`GLOB_RECURSE`でsourceを
  収集する。このためURIはpilot単独buildが成功していても製品上のL4独立componentではない。
- R0台帳はMSBuild Debug製品のcompiler/PCH/include、生成target scheduling/execution、5件の厳密なproducer-consumer相関、
  RC input、link input setまで観測したが、依然partialである。CMake custom command等の入力宣言欠落10件があり、変更時の
  正しい再生成閉包をまだ保証できない。selected archive member/link map、resource table/ID、実restore、他context、
  staging/file accessは未観測で、runtime asset/state/protocolはnot-observedである。実行されたこと、入力宣言が完全であること、
  互換性が保たれたことを混同しない。
  未観測を依存ゼロとして扱わず、残る27未所有file、250 include witness、1件の未分類quoted include
  (`cmigemo/migemo.h`)、17 source link directive、4 language resource、9 root package、global restore、tests1 object結合を
  後続gateで解消する。
- `docs/l4-component-build-v2.1.md`に記されたR1a hard gateを、このpilotのunit/graph結果だけでgreenへ繰り上げない。
- 本書に記載する実装一式は、GitHub Issue #15の追跡対象として統合する。Issueにはbasic path/revertの
  [`issuecomment-5174514088`](https://github.com/tsuyoshi-otake/sakura-editor-next/issues/15#issuecomment-5174514088)と、今回のABI gateの
   [`issuecomment-5174652528`](https://github.com/tsuyoshi-otake/sakura-editor-next/issues/15#issuecomment-5174652528)、rebuild closureの
   [`issuecomment-5175202617`](https://github.com/tsuyoshi-otake/sakura-editor-next/issues/15#issuecomment-5175202617)、R0 inventory baselineの
   [`issuecomment-5175424887`](https://github.com/tsuyoshi-otake/sakura-editor-next/issues/15#issuecomment-5175424887)、native product観測の
   [`issuecomment-5175817226`](https://github.com/tsuyoshi-otake/sakura-editor-next/issues/15#issuecomment-5175817226)、generator実行と実消費相関の
   [`issuecomment-5176160587`](https://github.com/tsuyoshi-otake/sakura-editor-next/issues/15#issuecomment-5176160587)を追記した。

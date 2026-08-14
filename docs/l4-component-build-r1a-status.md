# Issue #15 — R0台帳・R1a基盤・URI／serialization product-provider／独立test runner gate ステータス（2026-08-05）

本書は、Issue #15 R1aの統合作業で確認できた事実を記録する。URI vertical sliceは
`sakura_app`／`tests1`から直接source ownershipを除去し、宣言済みproviderをlinkするところまで進んだ。
これはURI sliceの製品接続を示すものであり、repository全体のL4独立性またはR1a完了は宣言しない。

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
- generator versionを0.3.5へ上げ、manifest minimum version、content-hash付きstamp、MSBuild props、
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
- `inventory observe-resources`はcurrentなnative product evidenceと製品hashを前提に、`sakura.exe`をdata/image
  resourceとして非実行で開く。top-level PE resourceのtype、name、language ID、size、content hashを有界に列挙する。
  table観測とcanonical/nested numeric resource-ID互換を別gateとし、後者はversion付きgolden baseline、
  `sakura_rc.h`、3言語のRC/RC2 source contract、3言語の実バイナリcontractが一致した場合だけgreenにする。
- `build/<platform>/CMakeTools`は本体と言語DLLが読む共有generator workspaceであり、leaf projectの
  `CoreClean`所有物ではない。通常のClean/Rebuildはworkspaceを保持し、canonical native observerの
  `--rebuild`だけが内部propertyで完全cleanを明示する。言語resource DLLから最終製品への不要かつ壊れた
  `ProjectReference`も除去し、依存方向を「resource leaf -> generated contracts」に限定した。
- CMake生成Visual Studio projectのphony outputによってcustom target自体が毎回要求される場合でも、重い処理を
  再実行しないcontent-aware observerへ変えた。version／manifest／runtime assetはcontent不変なら出力を更新せず、
  ctagsは親repositoryのgitlinkが示すexact commit、toolchain signature、helper hash、全outputを一つのlock内で検査する。
  archive展開とnested nmakeはstate不一致時だけ実行し、全output成功後にstateを発行する。runtime assetはsymlinkを
  実providerへ解決してSHA-256比較するため、`copy_if_different`だけに依存したmtime churnを起こさない。
- repository inventoryは`add_custom_command`だけでなく`add_custom_target`の`DEPENDS`／`BYPRODUCTS`／`COMMAND`も解析し、
  依存台帳へ統合する。今回対象のCMake generator入力宣言欠落は10件から0件になった。
- URIのlegacy consumer projectionをmanifestから生成する。MSBuildでは`UriIdentity.cpp`を製品／`tests1`の
  `ClCompile`から除去し、両consumerからgenerated `sakura_uri.vcxproj`へ直接`ProjectReference`を張る。
  CMakeではlegacy recursive source集合から同sourceを除外し、両consumerが一つの`sakura_uri` static archiveをlinkする。
  依存方向は一貫して`consumer -> dependency/provider`であり、providerから製品への逆参照は持たない。
- 最終製品に必要なgenerated provider projectは`sakura.sln`へ生成的に追加し、Debug／Release x64の
  `ActiveCfg`／`Build.0`とconsumer dependency blockを持たせる。手書きsolutionへの偶然の登録には依存せず、
  legacy productのlink closureとsolution membershipの一致をunit testで固定した。
- `inventory observe-product`のopt-in link MAPでprovider archive memberの実選択を観測する。Release LTCGでは
  provider libraryがlink command payloadへ現れずtracker inputにだけ残る場合があるため、command libraryと
  repository libraryの和集合から宣言providerを照合する。tracker rootはvcxprojの評価済み`IntDir`を優先し、
  `app-avx2`／`app-o1`等の兄弟variantを誤選択した場合はgreenにしない。
- freshなroot CMake build treeをNinja／MSVCでDebug／Releaseごとに構成し、生成されたnative graphでも
  `UriIdentity.cpp`を`sakura_uri`だけが所有し、`sakura`と`tests1`が`sakura_uri.lib`をlinkすることを確認した。
  VS付属CMake 3.31.6のVisual Studio 17 generatorは、repository非依存の最小`project(... LANGUAGES CXX)`でも
  compiler identification直後にchildless idleとなったため、CMake projection不具合とhost/tool blockerを分離した。

## URI product-provider統合の最新Verify / Expect

| Verify | Expect / 実結果 |
|---|---|
| build tool unit test | exit 0、114 tests passed。solution membership、legacy consumer projection、Release LTCGのtracker-only provider library、兄弟variantを持つ`IntDir`解決に加え、private-header rebuild scope、manifest-driven隔離コピー、URI consumerの狭い公開include root、mutation種別の適用可否分類、split runnerのexact discoveryと明示remap、旧URI test TUの非所有を回帰検査へ追加した。 |
| `manifest check`／`graph check --all-contexts`／`generate --check` | すべてexit 0。6 context、SCC／witness failure 0、generator 0.3.5、`stale=[]`。依存方向は`consumer -> dependency/provider`。 |
| MSBuild `sakura.sln` Debug／Release x64 | 両方成功。直後のno-opも成功した。solutionはgenerated `sakura_uri` projectを含み、`sakura_app`／`tests1`からのbuild dependencyを持つ。 |
| URI component contract test | MSBuild Debug／Releaseは0 warning／0 errorで実行終了値0。CMake MSVC Debug／Releaseはbuild成功、CTest 1/1 passed。 |
| 独立`sakura_uri_tests` | 旧`src/test/cpp/tests1/platform/UriIdentityTest.cpp`を削除し、同じ9 selectorをpackage-less runnerへ移した。MSBuild Debug／Releaseは各9/9、CMake／Ninja／MSVC Debug／ReleaseはCTest各1/1 passed。runnerは`sakura_uri`だけをlinkし、製品object、GoogleTest package、本体resource/test assetを要求しない。 |
| split test inventory | Debug基準で`sakura_uri_tests` 9件と`tests1` 2,790件、合計2,799件を実行時列挙し、missing／unexpected selector 0。URI 9件は既存stable `test_id`を維持してruntime runnerだけを変更した。古い台帳以降の83件は保証追加として登録し、既存2件の名称変更はstable IDを維持した明示remapとして記録した。Release `tests1`は`_DEBUG`条件の2件を列挙しないため、このDebug inventoryのexact照合対象にはしない。 |
| app／tests1 DesignTimeBuild | `DesignTimeBuild=true;SkipCompilerExecution=true`の`ClCompile` targetで両projectともexit 0。`Compile`はこのvcxprojの有効target名ではないため使用しない。 |
| product native evidence + repository inventory | Debug／ReleaseともMAPでprovider archive memberの実選択を観測し、`product_reachability.status=independent_provider`、`independent=true`、declared provider edge／native link closureを観測した。最終再採取したDebug native hashは`sha256:b29e188dc939065d00844c5baaffb1f611502c877cc27395b9a3ce99db4fd7e2`、inventory hashは`sha256:aac16792668d6341bf6a91c331e3e77814fbc0829a5f93e1a912fd4ccdb7bf44`。Release native hashは`sha256:126d010d0b31b4cdbe4ab406db1dfe8727e9576e0f5df6f24b674642938b1255`、inventory hashは`sha256:57e5f2ed5c4dd0f99b0e42a91a285e49d3602dce288c78e531cb02737409e5cc`。両方とも全体graduationはfalse、finding 42件。 |
| URI private cppの局所変更probe | `UriIdentity.cpp`へ一時的な非機能commentを加えたDebug `build dev`は6.55秒で、URIの1 TU compile、provider archive、app relinkだけを実行した。元内容とhashへ復元済み。これは現行sliceのbaselineであり、50%短縮を証明する移行前後比較ではない。 |
| URI private headerの局所変更probe | `UriIdentityInternal.h`を実private headerとして抽出し、manifestの`private_headers`／`private_include_roots`へ登録した。隔離rehearsalの一時変更では、MSBuild 1,232.144 ms、CMake/Ninja 678.568 msでURIの1 TU compile、`sakura_uri` archive、`sakura_uri_tests` linkだけを観測した。missing/unexpected action 0、package restore false、projection変更0、test exit 0、workspace cleanup true。 |
| URI public include rootの物理境界 | 公開契約を`sakura_core/include/sakura/uri/UriIdentity.h`へ移し、全consumerを`<sakura/uri/UriIdentity.h>`へ変更した。生成MSBuild/CMakeのproviderは公開rootとprivate rootを持つが、`sakura_uri_tests`は公開rootだけを受け取る。contract testは`__has_include`で`UriIdentityInternal.h`の到達不能を負側検査し、MSBuild/CMake MSVC Debug／Releaseで成功した。public-contract mutationの正確な閉包はMSBuild 1,756.564 ms、CMake/Ninja 962.716 msで、providerとtest consumerの各1 TU、archive、test linkだけだった。 |
| URI mutation適用可否 | evidence JSONへPCH／resource／component generated input／projection inputを必須分類として追加した。URIは小規模headless leafで、生成projectがPCHを明示無効化し、resourceとcomponent固有generatorを所有しないため前3種は理由・witness付き`not_applicable`である。manifest／compile-profileは`covered_by_staleness_gate`とし、通常component buildが暗黙再生成しない既存gateへ接続した。Debug隔離rehearsalはMSBuild／CMakeとも全phase成功、workspace cleanup true。 |
| manifest-driven隔離コピー | component source／public・private header／backend project、edge witness、artifact inputをmanifestからO(N)で収集する。存在しないpath、repository外へのescape、欠落witnessを明示失敗とし、空directoryを捏造しない。normal／ASCII space／日本語pathのMSBuild/CMake test/evidence hash一致とstandalone revertを再検証した。 |
| fresh root CMake／Ninja／MSVC Debug | configure 16.824秒、`sakura` build 53.161秒、直後no-op 0.499秒、`tests1` build 32.151秒、URI focused test 9/9 passed、直後no-op 0.749秒。native graph上の`UriIdentity.cpp` compile ruleは`sakura_uri`の1件だけで、`sakura`／`tests1` link ruleはいずれも`sakura_uri.lib`を含む。 |
| fresh root CMake／Ninja／MSVC Release | configure 14.399秒、`sakura` build 95.527秒、直後no-op 0.623秒、`tests1` build 106.816秒、URI focused test 9/9 passed、直後no-op 0.990秒。Debugと同じ単一ownership／consumer link edgeをnative graphで確認した。 |
| URI公開header移動後の製品再検証 | MSBuild solutionはDebug 151.26秒／Release 251.12秒で全再compile・linkに成功し、focused URI testは各9/9、直後のapp no-opは0 warning／0 errorで約1秒だった。既存Ninja treeのCMake rootはDebug `sakura` 163.864秒、`tests1` 112.137秒、Release `sakura` 172.350秒、`tests1` 168.494秒で成功し、focused URI testは各9/9、app no-opはDebug 0.669秒／Release 0.634秒だった。公開headerの物理移動による一度限りのmonolith再構築と、通常のleaf局所変更を混同しない。 |
| root CMakeの負側観測 | configureはroot `vcpkg.json`の全依存をrestoreし、URIに無関係なpackageを局所化できていない。`tests1` targetは言語DLL、PPA stub、DLL plugin、test ZIP、本体EXEを起動する。URI provider接続のPASSとpackage／asset／test集約のred gateを分離して扱う。Visual Studio generator停止は最小projectでも再現し、run-owned processを親から停止してsurvivor 0を確認した。 |
| product no-opの負側観測 | root CMakeの`sakura` no-opはDebug 0.429秒、Release 0.426秒だが、runtime DLL／manifest／version observerを各回起動する。MSBuild `tests1`の旧観測では再linkが報告されていたが、2026-08-04の現作業ツリー再測定ではDebug solution no-op 1.39秒で`RemoveTests1Exe`とLinkはいずれもup-to-dateだった。再現しない旧赤ゲートを完了根拠にも未完了根拠にも使わず、入力mutationを含む回帰証拠で判定する。 |
| process cleanup | test／build後にrepository path付きrunner、`sakura.exe`、MSBuild、compiler、linkerを再列挙し、survivor 0。 |

## serialization product-provider／独立test runner統合の最新Verify / Expect

URI sliceの次の低結合な葉として、JSONC document parserを`sakura_serialization`へ抽出した。これは標準ライブラリだけを使う1 TUのproviderで、resource、language DLL、PPA、test plugin、製品objectを単独runnerへ持ち込まない閉包を選んだ。依存方向は引き続き`consumer -> dependency/provider`である。

| Verify | Expect / 実結果 |
|---|---|
| ownership／公開面 | `JsoncDocument.cpp`は旧`sakura_app` source ownershipから除外し、generated `sakura_serialization`が単独所有。公開headerは`sakura_core/include/sakura/serialization/JsoncDocument.h`、旧private header pathは削除。`sakura_app`／`tests1`はgenerated provider projectをlinkする。 |
| 独立runner | `sakura_serialization_tests`はpackage-lessで4 selectorを公開し、`sakura_serialization`だけをlinkする。MSBuild Debug／Release、CMake/Ninja/MSVC Debug／Releaseの全4構成で4/4 pass。CMakeでは既存の長い隔離build pathに関する`CMAKE_OBJECT_PATH_MAX`警告が出るため、path短縮は後続のbuild-speed gateとして残す。 |
| full solution | `sakura.sln` x64 Debug／Releaseとも成功、0 error。serialization archiveがconsumer link commandへ現れ、旧`JsoncDocument.cpp`はtlogから削除された。Releaseは4分18秒。既存legacy warningは残るが、今回の変更によるerrorはない。 |
| split test inventory | `sakura_serialization_tests` 4件、`sakura_uri_tests` 9件、`tests1` 2,786件、合計2,799件。3 runnerの実行時selectorをexact照合し、missing／unexpectedとも0。guarantee fingerprintは`sha256:cd17b07e61f94a2f687ab4b78b1c4e16189e37ef269d162afefc461965ee7f81`。 |
| static／generated regression | 追加したMSBuild ownership／狭い公開include root回帰テストを含むbuild-tool unit 116件が全pass。`manifest check`（graph hash `sha256:868c2bb87997e02fd088c958b8551b8978ccdb6c5740c887a6595acd13803bd2`）、全6 contextの`graph check`、`generate --check`（stale 0）、compileall、`git diff --check`が全てexit 0。 |
| process cleanup | build／test後にrepository path付きrunner、compiler、linker、MSBuild等を再列挙し、survivorは`[]`。 |

### serialization sliceの残余risk／次gate

- `sakura_serialization`自身は標準ライブラリだけの小さなproviderだが、JSONCを利用するconfiguration、workspace、theme、extension等はまだlegacy monolith／tests1へ残る。providerを分けたことを利用機能全体の独立性とは扱わない。
- product／tests1の公開include rootは移行中のlegacy viewを保持しており、repository全体でprivate pathを名前解決不能にする境界検査は未完了である。
- root vcpkg manifestの全依存restore、tests1の本体object・resource・language DLL・PPA／plugin／ZIP集約、runtime asset／state／protocol境界は未解消である。
- 4構成のCMake成功はnative MSVC/Ninja経路だけであり、MinGW native buildはtoolchain不在のため未実行。Visual Studio generatorのhost blockerも別gateに残す。
- 同等保証範囲の移行前後5回warm中央値、private/public/generated/resource mutationごとの正確な再ビルド閉包、CMake object path短縮、L4全体graduationは未完了。次はR0 dependency inventoryからresource／package／test assetを含まない別の葉を選び、同じexact runtime／link closure gateを適用する。

### 既存基盤の累積証跡

以下はURI製品接続前から積み上げた基盤の実行時証跡である。個々のhash／finding数はその実行時点の値であり、上表の最新URI製品証跡が優先する。

| Verify | Expect / 実結果 |
|---|---|
| `rtk proxy py -3 -m unittest discover -s tools/build/tests -p "test_*.py"` | exit 0、104 tests passed。敵対的台帳fixtureに加え、native trackerのcompiler/PCH/生成header/RC/link input、source/product変更によるstale、context不一致、Build未実行snapshot、tracker欠落、壊れたevidence JSON、generatorの実行/up-to-date/condition-false/無関係target混入、manifest link input、hard-evidence tamper、入力宣言欠落、resource tableの正規化/改ざん/checkout独立hashを検証した。さらにstandard/extended dialog/menu、accelerator、string block、header/source contract、baseline version advance、入力staleness、共有CMakeTools cleanの明示所有権、言語DLLの製品非参照、content-stable runtime copy／archive state／gitlink観測／ctags cache、custom-target BYPRODUCTSと入力解析、solution membership、legacy consumer projection、tracker-only provider libraryを検証する。 |
| `rtk proxy py -3 -m compileall -q tools/build` | exit 0。Python build CLI一式の構文検査に成功した。 |
| `rtk proxy py -3 tools/build/sakura_build.py manifest check --format json` | exit 0、schema 3、graph hash `sha256:323f9ab6f5e344a6e2a18c6ce9ab435d94b9c877b9a3c8a2cf45cc4ba75ec47c`。 |
| `rtk proxy python tools/build/sakura_build.py graph check --all-contexts --format json` | exit 0、6 context、SCC failure 0、witness failure 0。 |
| `rtk proxy python tools/build/sakura_build.py generate --format json` | exit 0。generator 0.3.5からMSBuild/CMake projectionとcontext別ABI headerを再生成した。生成rootの候補worktree全置換は行っていない。 |
| `rtk proxy python tools/build/sakura_build.py generate --check --format json` | exit 0、`stale=[]`。 |
| URI MSBuild/CMake Debug canonical component test | MSBuild exit 0（0 warning/0 error）、CMake build exit 0、CTest 1/1 passed。 |
| `verify component-boundary`（MSBuild/CMake） | 両方exit 0。ABI headerを含むdeclared/observed repo inputsは5件で一致、link provider/map memberは`sakura_uri.lib`、package restore false、root import suppression true、failures空。MSBuild external input 201、CMake 0。 |
| Debug限定edgeのMSBuild評価 | fixtureを実projectへ生成し、`-getItem:ProjectReference`をDebug/Releaseで評価。Debugは`provider.vcxproj`、Releaseは空。 |
| 生成URI test projectのDesignTimeBuild | `DesignTimeBuild=true;SkipCompilerExecution=true`、`ClCompile`がexit 0、0 warning/0 error。 |
| `build fixture abi-pack-mismatch` / `abi-iterator-mismatch` / `abi-opaque-compatible` | 3件ともCLI exit 0。最初の2件はnative linkがLNK2038となり、それぞれ`sakura.edge.abi-fixture.default_pack`、`sakura.edge.abi-fixture.iterator_debug_level`を診断して`expected_failure`へ終端した。opaque Cはpack差があってもlink/runtime exit 0。 |
| `path-matrix test component sakura_uri_tests --contexts msvc-x64-debug,cmake-msvc-x64-debug --jobs 1` | exit 0。normal/ASCII space/日本語の両backend test/evidenceが成功し、semantic graph hashとbackend別hard evidence hashが全case一致。standalone revertのlegacy hash/source/reference維持とcandidate生成project除去が成功。workspace cleanup true。 |
| `verify rebuild-closure sakura_uri_tests --contexts msvc-x64-debug,cmake-msvc-x64-debug --jobs 1 --samples 5` | exit 0、両backendでclean/no-op/private cpp/private header/public contractの期待compile/archive/link集合と観測集合が完全一致。全phaseでprojection変更0、package restore 0、test exit 0、workspace cleanup true。private header phaseはMSBuild 1,232.144 ms、CMake/Ninja 678.568 ms。no-op native child中央値はMSBuild 369.179 ms、CMake/Ninja 35.213 ms、MSBuild DesignTimeBuildは280.259 ms。いずれも移行前比較を持たないbaseline-onlyで、canonical CLI全体の性能値や改善率とは扱わない。 |
| CMake Visual Studio generator targets（Debug / Release） | Debugはstate形式更新を一度反映した初回39.206秒、直後3.396秒、Releaseは初回3.777秒、直後3.105秒。収束後はいずれもversion、manifest、runtime DLL、ctags/diff、ctags archive/state等12 outputのhash/size/mtimeがすべて不変で、2回目にctags展開／nmake／submodule更新／archive一時発行を実行しなかった。direct targetの軽量observer実行と重いmaterializationを区別した。 |
| canonical `build dev x64 Release --jobs 4` | 初回製品buildは成功（674既存warning、0 error、1分39.83秒）、直後は0.66秒、0 warning/0 errorだった。その後、明示的native `--rebuild` observerが共有CMakeToolsをcleanしたため復旧buildが1分38.82秒で一度だけ発生し、直後の通常buildは0.74秒、0 warning/0 errorでcompiler/linkerともup-to-dateになった。改善率のbaselineではなく、明示cleanの影響と日常no-opを分離した実証である。 |
| `inventory observe-product --context msvc-x64-debug --product sakura_app --jobs 4 --rebuild` | MSBuild製品Rebuildと収集はexit 0。observerだけが共有CMakeTools cleanを明示し、568翻訳単位、source input 1,162、生成header 16、PCH create 1/use 558/none 9、RC unit 1、link object 568/resource 1、repository library 6/external library 35を観測した。13 generator targetの終端状態を記録し、実`Exec`を伴うproducerからFunccode define→RC、Funccode enum→compiler、manifest→link、version→compiler/RCの5消費を厳密pathで相関した。native evidence hashは`sha256:8f76a4693be70c4316283994f26db9783aa8aa01e0569e2af548cd25d1ee9303`。selected archive memberと実package restoreは`false`のまま。 |
| `inventory snapshot-resource-ids --image ja-JP=... --image en-US=... --image zh-CN=... --accept-current` | exit 0。compatibility version 1、header定義1,874件と3言語source contractを固定した。実バイナリではja-JP 260 / en-US 257 / zh-CN 257 top-level resource、各62 dialog、923–926 control ID、1,364–1,390 string IDを記録した。baseline hashは`sha256:9b546dbb42414037278296b736f09b19e1fdcf45f65b265d9cbfd2a39daf4219`。 |
| `inventory observe-resources ... --resource-id-baseline ... --compat-image en-US=... --compat-image zh-CN=...` | exit 0。current native productとの一致を確認し、top-level PE resource 260 entry、12 type、language ID 1033/1041、合計414,505 bytesを観測した。table hashは`sha256:1bcd9acee829e42f0ca092211b98589b3e8d932cad53beb9d26e8e184e0ee4de`、resource hard evidence hashは`sha256:a8b02ad25a2090463f685c28af3f1158ea9b499d68dcba6424cfced138cf2404`、canonical/nested resource-ID compatibilityは`true`。entry pointは実行していない。 |
| `sakura_lang.sln /t:Rebuild`（x64 Debug / Release） | Debug 2.08秒、Release 2.03秒、双方0 warning/0 error。製品ProjectReferenceは0件。前後でCMake cache、Funccode define/enum、version header、manifestのSHA-256とmtimeがすべて不変で、製品を再buildせずnative evidenceを再検証して`valid=true`、failure 0。 |
| language DLL DesignTimeBuild | en-US／zh-CNを`DesignTimeBuild=true;SkipCompilerExecution=true`で評価し、各0.08秒、0 warning/0 error。製品projectの評価・buildを要求しない。 |
| `inventory repository ... --native-evidence ... --resource-evidence ...` | 収集exit 0、`collection_ok=true`、`graduation_ready=false`。native compiler/PCH、生成input消費、generator scheduling/execution、5 producer-consumer相関、RC input、top-level PE resource table、numeric resource-ID compatibility、link input setをvalid evidenceとしてmergeした。findingは44件、CMake generator入力宣言欠落は0件、hard evidence hashは`sha256:5fb7df8b170639584e703c1b2cc146e518a1f3ef55fdb4b0822c98d32a37ba23`。selected archive member、package restore、未分類所有権等のred gateは維持した。 |
| 同inventoryの`--strict` | 44 findingと10 dependency classのpartial/not-observedが残るためR0卒業を拒否した。入力宣言gateだけをgreenにし、未観測gateをまとめて成功扱いにはしていない。 |
| component `plan`/dry-run、legacy Release、MinGW | plan/dry-runとlegacy x64 Releaseはexit 0。MinGWはこのhostにgcc/g++/iconv toolchainが存在せず実行不能で、静的contract/unit検査だけを実施した。未実行をPASSとは扱わない。 |

生成器とmodelのunit fixtureでは、3-nodeの`consumer -> provider -> private-provider` final-link closure、
compile-only include/non-link投影、MSBuild Utility/CMake INTERFACE、MSBuild ProjectReference closureと
CMake root-only stale scopeを確認した。これはnative URI実行や製品consumerのsymbol解決を証明するものではない。

## 4候補の採否

- core candidate (`c943`): 採用。public/final closure、compile-only include境界、generator 0.3.2基盤（現0.3.5）、component
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

- URIのMSBuild／CMake MSVC Debug／Release component build・独立test runner、MSBuild製品／残存`tests1` consumer接続、
  split runtime inventory、DesignTimeBuild、MAP archive-member選択は検証済みである。MinGWはこのhostに`gcc.exe`、
  `g++.exe`、`mingw32-make.exe`がないためnative buildを実行できず、利用可能な環境で別gateとして残す。
- generated CMake ownership／consumer projection、isolated URI Debug／Release buildに加え、fresh root
  CMake／Ninja／MSVCの`sakura`／`tests1` Debug／Release build、focused test、no-opは成功した。これにより
  CMake製品接続のURI ownership／link edgeはPASSとする。一方、VS付属CMake 3.31.6のVisual Studio 17 generatorは
  repository非依存の最小projectでもcompiler identification直後にchildless idleとなるhost/tool blockerが残る。
  Visual Studio generator固有経路をgreenとせず、利用可能な別hostで再検証する。
- URIのprivate cpp／private header mutationでは、URI 1 TU、provider archive、対象consumer relinkへ局所化したことを観測した。
  PCH、resource、component generated inputをURIへ機械的に足すことはせず、所有入力がない小規模headless leafとして
  evidenceへ理由付き`not_applicable`を記録した。これらの実mutationは、実際に所有する後続UI/resource/generated componentの
  graduation gateとして残る。同等保証範囲による移行前後5回warm中央値と50%短縮判定も未完了である。
- `UriIdentityInternal.h`はmanifest上privateで、実装上のincludeはprovider実装だけである。公開契約は
  `sakura_core/include/sakura/uri/`へ移し、isolated consumerにはその公開rootだけを渡すため、URI leafの物理include viewは
  狭小化した。一方、legacy monolithの製品／`tests1`は移行中の広い`sakura_core` include rootをまだ持つ。したがって
  repository全体でprivate pathを物理的に名前解決不能にする作業は未完了であり、URI leafのgreenを全体へ外挿しない。
- URIはMSBuild製品と残存URI consumerを持つ`tests1`で独立providerになり、生成CMake projectionでもsource ownershipとlink edgeを分離した。
  URI自身の9 testは独立runnerへ移したが、`tests1`全体は依然として他の製品object manifestを集約し、repository全体のlegacy monolith／resource／package／runtime／
  state／protocol境界は未分離であるため、URI一件のgreenをL4全体のgreenへ外挿しない。
- fresh root CMake configureはURIだけのbuild要求でもroot vcpkg manifestの全packageをrestoreした。また`tests1` buildは
  言語DLL、PPA stub、DLL plugin、test ZIP、本体EXEを明示的に起動した。これはpackage manifest分割とtest asset／runner分割が
  まだ実装されていない実証であり、no-opが高速でもclean buildの局所性を満たしたとは判定しない。
- 最新のDebug／Release URI inventoryはいずれも`independent_provider`を観測したが、全体graduationはfalseでfinding 42件である。
  resource evidenceは今回のinventoryへ再結合していないためnot-providedであり、実package restore、staging/file access、
  runtime asset/state/protocolはpartialまたはnot-observedである。未観測を依存ゼロとして扱わず、17 source link directive、
  root vcpkg manifest／global restore、未所有source/resource/include、tests1 object結合を後続gateで解消する。
- `docs/l4-component-build-v2.1.md`に記されたR1a hard gateを、このpilotのunit/graph結果だけでgreenへ繰り上げない。
- 本書に記載する実装一式は、GitHub Issue #15の追跡対象として統合する。Issueにはbasic path/revertの
  [`issuecomment-5174514088`](https://github.com/tsuyoshi-otake/sakura-editor-next/issues/15#issuecomment-5174514088)と、今回のABI gateの
   [`issuecomment-5174652528`](https://github.com/tsuyoshi-otake/sakura-editor-next/issues/15#issuecomment-5174652528)、rebuild closureの
   [`issuecomment-5175202617`](https://github.com/tsuyoshi-otake/sakura-editor-next/issues/15#issuecomment-5175202617)、R0 inventory baselineの
   [`issuecomment-5175424887`](https://github.com/tsuyoshi-otake/sakura-editor-next/issues/15#issuecomment-5175424887)、native product観測の
   [`issuecomment-5175817226`](https://github.com/tsuyoshi-otake/sakura-editor-next/issues/15#issuecomment-5175817226)、generator実行と実消費相関の
   [`issuecomment-5176160587`](https://github.com/tsuyoshi-otake/sakura-editor-next/issues/15#issuecomment-5176160587)、native PE resource table観測の
   [`issuecomment-5176365272`](https://github.com/tsuyoshi-otake/sakura-editor-next/issues/15#issuecomment-5176365272)、共有生成物所有権の
   [`issuecomment-5177272586`](https://github.com/tsuyoshi-otake/sakura-editor-next/issues/15#issuecomment-5177272586)、生成入力契約とcontent-aware observerの
   [`issuecomment-5178007507`](https://github.com/tsuyoshi-otake/sakura-editor-next/issues/15#issuecomment-5178007507)、URI製品provider接続の
   [`issuecomment-5178948312`](https://github.com/tsuyoshi-otake/sakura-editor-next/issues/15#issuecomment-5178948312)と、fresh root CMake製品接続の
   [`issuecomment-5179243788`](https://github.com/tsuyoshi-otake/sakura-editor-next/issues/15#issuecomment-5179243788)、private-header境界と正確なrebuild closureの
   [`issuecomment-5179427392`](https://github.com/tsuyoshi-otake/sakura-editor-next/issues/15#issuecomment-5179427392)、狭い公開include rootと物理consumer境界の
   [`issuecomment-5179710741`](https://github.com/tsuyoshi-otake/sakura-editor-next/issues/15#issuecomment-5179710741)、mutation適用可否とno-op再測定の
   [`issuecomment-5179809797`](https://github.com/tsuyoshi-otake/sakura-editor-next/issues/15#issuecomment-5179809797)、URI test runner分離とexact runtime inventoryの
   [`issuecomment-5180031460`](https://github.com/tsuyoshi-otake/sakura-editor-next/issues/15#issuecomment-5180031460)を追記した。

## filesystem product-provider統合の最新Verify / Expect（2026-08-05）

filesystem葉は、URI／serializationと同じく「実装を持つ最小の責務」と「独立runner」を分けた。
この節のgreenはfilesystemの宣言済み閉包だけを意味し、Issue #15全体のL4 graduationではない。

| Verify | Expect / 実測結果 |
|---|---|
| ownership・公開面 | `sakura_filesystem`が`CFileService.cpp`、`CWin32FileSystemProvider.cpp`、`FileSystemFactory.cpp`と3つの公開contractを所有する。公開型は`sakura_core/include/sakura/filesystem/`、具象型はfilesystem private rootに限定し、product consumerは公開include viewだけを受け取る。 |
| 独立runner | `sakura_filesystem_tests`はpackage/resource-lessな13 selector runner。登録、bounded read、conditional atomic replace、version bound、local-file URI、reparse point、watch terminal cleanupを対象にする。旧tests1のFileService／Win32 provider test sourceは所有から除去した。 |
| MSVC component test | x64 Debug／Releaseのcanonical component build後、各13/13 pass。`verify component-boundary`はfailures空、filesystem／URIの宣言済みarchiveだけを観測し、package restoreはfalse、root build import suppressionはtrue。 |
| CMake/Ninja component test | x64 Debug／Releaseで各13/13 pass。初回の`DECLARED_INPUT_NOT_OBSERVED`は、MSVC `/showIncludes`のCMake自動probeが日本語出力を文字化けさせ、`ninja -t deps`が`#deps 0`になったことが原因だった。生成CMakeでこのtoolchainのUTF-8 prefixを固定し、CMake child環境を隔離してclean rebuildした後、depsとboundaryは正常化した。 |
| 全体ビルド | `sakura.sln` x64 Releaseがexit 0、0 error（既存warning 675）。生成物・legacy warningは残るがfilesystem変更によるerrorはない。 |
| static／generated regression | build-tool unit 118件、`compileall`、`manifest check`、全context `graph check`、`generate --check`（stale 0）、`git diff --check`がexit 0。最新graph hashは`sha256:ee319fb0b9b66c5435d4ac8ba3c46ab6293c4c8c81708828283ded4172a52994`。 |
| split test inventory | `tests1` 2,773、URI 9、serialization 4、filesystem 13、合計2,799。missing／unexpectedとも空で、guarantee fingerprintは`sha256:cd17b07e61f94a2f687ab4b78b1c4e16189e37ef269d162afefc461965ee7f81`のまま。 |
| process cleanup | build／test／boundary後のrepository path付きrunner、compiler、linker、MSBuild等を再列挙し、process auditは`[]`。 |

### filesystem sliceの残余risk／次gate

- CMake生成projectへ`メモ: インクルード ファイル:`のMSVC prefixを直接設定している。これは現hostのUTF-8出力とCMake probeの実測に基づく修正であり、他言語Visual Studio toolchainでの自動適応は未検証である。将来のtoolchain設定化を別gateに残す。
- 隔離CMake buildではobject pathが161–169文字となり、`CMAKE_OBJECT_PATH_MAX=220`の警告が出る。buildは成功したが、path短縮と再ビルド性能は未完了である。
- MinGW native build、Visual Studio generatorのCMake child execution、移行前後の同等保証5回warm性能比較、private/public/generated/resource mutation別の厳密な再ビルド閉包は未検証である。
- root vcpkg manifest全体restore、tests1の本体object/resource/language DLL/PPA/plugin/ZIP、legacy monolith、runtime asset/state/protocol、全体resource境界は残る。filesystemのgreenを機能ファミリーやL4全体へ外挿しない。
- 次の葉でも`consumer -> dependency/provider`の方向、contracts-only consumer、declared link closure、package-less runner、exact inventory、terminal process cleanupを必須にする。次候補はstorageまたはrequestだが、requestはWinHTTP／非同期／credential境界が大きいため、まずstorageの依存閉包を台帳化して安全性を判定する。

## storage model contract leafの最新Verify / Expect（2026-08-05）

storageは一つの大きな責務として一括抽出せず、まず不変な型契約・検証ロジックだけを葉にした。この節のgreenは `StorageTypes` modelの独立性を意味し、`CInMemoryStorageService`、`CAtomicFileStorageService`、Control RPC、永続化、stateful lifecycleの独立性を意味しない。

| Verify | Expect / 実測結果 |
|---|---|
| ownership・公開面 | `sakura_storage`が `StorageTypes.cpp` と `sakura_core/include/sakura/storage/StorageTypes.h` を所有する。consumerは `sakura/storage/StorageTypes.h` の公開include rootだけを受け、旧 `platform/storage/StorageTypes.h` は削除した。 |
| 参照方向 | 依存矢印は `consumer -> dependency/provider`。製品consumerは `sakura_storage`の公開契約へ向かい、独立runnerは `sakura_storage`だけをリンクする。stateful providerやControl server/storage実装への逆到達はこのsliceに含めない。 |
| 独立runner | `sakura_storage_tests`はpackage/resource-lessな2 selector runner。`StorageAddress.RequiresScopeIdentityOnlyForProfileAndWorkspace` と `StorageAddress.RejectsUnknownScopesOversizedPartsAndMalformedUtf8` を移し、旧tests1 sourceから同じ2 selectorを削除した。 |
| MSVC component test | x64 Debug／Releaseでcanonical build後、2/2 pass。boundaryのclosureは `[sakura_storage, sakura_storage_tests]`、failures空、package restore false、root import suppression true。 |
| CMake/Ninja component test | x64 Debug／Releaseで2/2 pass。boundary failures空、external input 0、package restore false。object path 161--169文字の既存警告は残余riskとして記録する。 |
| 全体ビルド | `sakura.sln` x64 Debug／Releaseが0 errorで成功し、app/tests1のリンク閉包に`sakura_storage.lib`が含まれる。Releaseは既存warning 675、経過3分53.08秒。 |
| generated / DAG | `generate --check`は`stale=[]`、`graph check --all-contexts`は6 context・failures空。最新graph hashは`sha256:951025aff305e27e394b63e20150bb71ce77e46609293f74551bb974651ad6dc`。 |
| split inventory | filesystem 13、serialization 4、storage 2、URI 9、tests1 2,771、合計2,799。missing／unexpected空、guarantee fingerprintは`sha256:cd17b07e61f94a2f687ab4b78b1c4e16189e37ef269d162afefc461965ee7f81`のまま。 |
| static／cleanup | build-tool unittest 120件、`compileall`、runtime inventory verify、process audit `[]`、`git diff --check`を実行する。 |

### storage model sliceの残余risk／次gate

- `CInMemoryStorageService`、`CAtomicFileStorageService`、`IStorageService`のstateful実装、filesystem／registry／Control IPC／永続化との結合は既存経路に残る。StorageService全体のL4独立性は未判定であり、次のgateでowner、thread/lifecycle、callback、revision、retry、終端状態を別々に台帳化する。
- root vcpkg manifest、tests1の本体object/resource/language DLL/PPA/plugin/ZIP、legacy monolith、runtime asset/state/protocolは未分離である。model-only runnerのpackage-less実行を、製品全体のhermetic性へ外挿しない。
- MinGW native build、Visual Studio CMake generator child execution、同等保証範囲の移行前後5回warm性能比較、private/public/generated/resource mutation別の厳密な再ビルド閉包は未検証である。
- CMakeのMSVC `/showIncludes` prefix固定は現hostのUTF-8出力に対する実測対応であり、他言語toolchainの自動適応は未検証である。object path警告も短縮ゲートに残す。
- 次の実装はstorage service全体へ無条件に広げず、まずstate ownershipとControl/Editor参照方向を `consumer -> dependency/provider` で確定し、model contractと同じexact inventory／boundary／terminal cleanupを満たす最小のstateful portから進める。

## `IStorageService` 公開契約の最新Verify / Expect（2026-08-05）

StorageTypes modelに続き、stateful implementationを移さずにstorageの公開契約面をprivate headerから切り離した。この節のgreenは `IStorageService` の公開面とconsumer include境界だけを意味する。

| Verify | Expect / 実測結果 |
|---|---|
| private include遮断 | `sakura_core/platform/storage/IStorageService.h`を削除し、CAtomic／CInMemory／ControlStorageRpc／DurableProfileのconsumerは `<sakura/storage/IStorageService.h>` だけをincludeする。scan上の旧private includeは0。 |
| public manifest | `sakura_storage`は `IStorageService.h` と `StorageTypes.h` をpublic headersとして所有し、generated MSBuild/CMakeとhandwritten projectへ反映した。 |
| component | MSVC/CMake-Ninja x64 Debug／Releaseでstorage runner各2/2。boundary closureは `[sakura_storage, sakura_storage_tests]`、failures空、package restore false。 |
| solution | `sakura.sln` Debugは3 warning/0 error、41.82秒、Releaseは3 warning/0 error、50.73秒。警告は既存の`wcsncpy`と未使用parameterで今回の変更由来ではない。 |
| generated / DAG | `generate --check` stale空、6 context graph failures空、graph hash `sha256:a5a0bdb1dd4eed13971641ef081d1f69a68ec6c496addc1097bc16022975637d`。 |
| inventory/static/cleanup | storage 2、tests1 2,771、合計2,799、missing／unexpected空、fingerprint `sha256:cd17b07e61f94a2f687ab4b78b1c4e16189e37ef269d162afefc461965ee7f81` 不変。static 120件、compileall、process audit `[]`、diff check pass。 |

### この契約sliceの残余risk

- `CInMemoryStorageService.cpp`、`CAtomicFileStorageService.cpp`はなお`sakura_app`のlegacy compilation/link closureにあり、`sakura_storage`がstateful implementationを所有すると主張しない。
- Controlだけがauthority/durable stateを所有し、Editorがserver/storageへ直接到達しない参照方向、subscription queueのboundedness、callbackのclose/Stop終端、fake port交換性は未検証である。
- root vcpkg、tests1、本体resource/runtime asset、IPC/persistence compatibility、MinGW native、performance closureは未分離であり、公開契約のgreenをL4全体へ外挿しない。

## `sakura_security` Win32 security leafの最新Verify / Expect（2026-08-05）

storageの公開契約境界に続き、既存のcurrent-user ACL生成処理を `sakura_security` へ切り出した。この節のgreenはACL/SECURITY_ATTRIBUTESの公開契約と単独リンク性を意味し、Control authority、secret vault、profile registry、共有メモリー、プロセス境界のL4完了を意味しない。

| Verify | Expect / 実測結果 |
|---|---|
| ownership・公開面 | `sakura_security`が `CurrentUserSecurityAttributes.cpp` と `sakura_core/include/sakura/security/CurrentUserSecurityAttributes.h` を所有する。旧 `sakura_core/platform/security/CurrentUserSecurityAttributes.h` は削除し、storage／secrets／profiles／Control IPCのconsumerは公開includeだけを使用する。旧private include scanは0。 |
| 依存分類 | Win32 security APIのsystem library `advapi32`をmanifestの`system_libraries`へ明示し、MSBuildのfinal link closureとCMakeのPUBLIC linkへ投影した。ソース内`#pragma comment(lib)`には依存していない。 |
| 独立runner | `sakura_security_tests`はresource/package-lessな1 selector runner。公開headerの単独include、non-copyable契約、ACL初期化、descriptor属性、再初期化、失敗診断の空性を検査する。private header到達はcompile-time `__has_include` guardで拒否する。 |
| MSVC component test / boundary | x64 Debug／Releaseのcanonical build後、各1/1 pass。`verify component-boundary sakura_security_tests`も各`ok=true`で、closureは`sakura_security_tests -> sakura_security`、declared system libraryは`advapi32.lib`、package restore false、failures空。Release executableの実行も`[==========] 1 tests ran; 0 failed.`で終了した。 |
| CMake/Ninja component test / boundary | x64 Debug／Releaseで各1/1 pass。CMake側にも`advapi32`のPUBLIC linkを生成し、MAPの`sakura_security` archive member選択とboundary `ok=true`を確認した。object path 151--165文字の既存警告は残余riskである。 |
| 全体ビルド | `sakura.sln` x64 Debug／Releaseを再実行し、Debugは0 warning/0 error（50.09秒）、Releaseは0 warning/0 error（1.37秒のno-op寄り再実行）で成功した。security projectはsolutionのDebug/Release x64構成へ明示登録した。 |
| generated / DAG | `manifest check`、全6 contextの`graph check`、`generate --check`が成功。graph hashは`sha256:8df7bf3b74eece6501eb50eed4a4dea778177e229a5397a87f82ad909580a8e2`。 |
| split inventory | `tests1` 2,766、URI 9、serialization 4、filesystem 13、storage 7、security 1、合計2,800。6 runnerすべてでmissing／unexpectedとも空。guarantee fingerprintは`sha256:88eb51eac6ef15f21946040912423d5b5b8387fc8db8a0b16bee371ae22a7d61`。新規selectorの安定IDは`sakura_security_tests:`名前空間で追加し、既存2,799件のIDは保持した。 |
| static／cleanup | build-tool unittest 122件、`compileall`、runtime inventory verify、`git diff --check`が成功。全build/test/boundary後のrepository path付きprocess auditは`[]`。 |

### security leafの残余risk／次gate

- `advapi32`の宣言はmanifestから生成されるが、handwritten `sakura.sln`のsolution登録は現状generatorが所有していないため、solution projectionをmanifestから完全生成する作業を残す。今回の手動登録はDebug/Releaseの実リンクを通すための明示的な移行処置である。
- MinGW native component buildは、このhostでCMakeがMinGW MakefilesとCXX compilerを見つけられず開始前に終了した。未実行をPASSとは扱わず、利用可能なtoolchainで再検証する。
- `CurrentUserSecurityAttributes`はWindows APIを含むため、公開契約はWin32実行環境へ依存する。Control/Editorのauthority所有、secret/profileのlifecycle、ACL互換golden fixture、resource/package/runtime hermetic性は未検証である。
- root vcpkg manifest、tests1の本体object/resource/language DLL/PPA/plugin/ZIP、legacy monolith、全体resource／protocol／state境界、同等保証範囲の5回warm性能比較、mutation別の厳密な再ビルド閉包は残る。security leafのgreenをL4全体へ外挿しない。

## `IStorageAuthority` Control lifecycle port／durable providerの最新Verify / Expect（2026-08-05）

authority portを導入した後、既定のdurable実装を実際に `sakura_storage` へ移設した。Control runtimeは公開factoryだけを知り、EditorはControl IPC client／endpoint reader／snapshot cache方向だけを保持する。この節のgreenはauthorityとdurable providerの縦切りに限り、repository全体のL4 graduationを意味しない。

| Verify | Expect / 実測結果 |
|---|---|
| 公開port／factory | `IStorageAuthority.h` は `Open`／`Close`／`IsOpen` と型付き終端結果だけを公開する。`StorageAuthorityFactory.h` を追加し、Control runtimeの公開ヘッダー／実装から `CAtomicFileStorageService` のprivate header依存を除去した。 |
| ownership | `CAtomicFileStorageService.cpp`／`.h`、公開factory、snapshot cache、storage public headersはmanifest上 `sakura_storage` が所有する。`CInMemoryStorageService` はtests1/legacy用途として `sakura_app` closureに残る。 |
| 参照方向／lifecycle | Controlだけがauthorityの `Open`／`Close` と停止順を所有する。Editorはserver、authority store、durable storage実装へ直接到達しない。RPC adapter／hostは `shared_ptr<IStorageAuthority>`、同期sessionはprotocol面として基底 `IStorageService`だけを受け取る。 |
| final-link依存 | `sakura_storage -> sakura_security` と `bcrypt` をmanifestへ宣言した。generatorはMSBuild/CMakeの各final-link rootへ `graph.final_link_closure` の静的providerを明示投影する。4構成boundaryのclosureは `sakura_security`、`sakura_storage`、`sakura_storage_tests`、MAPにはCAtomic／security memberを確認した。 |
| fake／実provider | fake authorityのOpen→Apply/Snapshot/Subscribe→Close→`IsOpen=false`を検査し、公開factoryでは隔離temp storeへ書込み、close、再open、再読出しを検査する。resource/package/本体EXEなしでstorage runner各9/9 pass。 |
| MSVC component／boundary | x64 Debug／Releaseでbuild/test pass。4つのboundary checkは全て `ok=true`、failures空、package restore false。宣言system libraryは `advapi32.lib`／`bcrypt.lib`。 |
| CMake/Ninja component／boundary | x64 Debug／ReleaseでCTest 1/1、runner各9/9、boundary `ok=true`。隔離build pathの既存 `CMAKE_OBJECT_PATH_MAX` 警告は残余riskとして記録する。 |
| transport／integration | Named Pipe transport filter 9/9、storage/control focused filter 72/72。active Snapshot frameを排出してから `BeginStopping` が完了する。 |
| 全体接続 | `sakura.sln` x64 Debugは0 warning/0 error、Releaseは0 errorで成功（既存legacy warning 215件）。Release `/GL` 最終リンクはcomponent局所指標と分離して扱う。 |
| generated／DAG／inventory／cleanup | manifest check、全6 context graph、`generate --check`、compileall、build-tool unittest 122件、`git diff --check`が成功。graph hash `sha256:4dd673562e752e33daa28e00d0baa9b16c6294fef28b289f5c4d9a4ad2346898`。Debug inventoryは2803、missing／unexpected 0、fingerprint `sha256:1d5c1541732ffaef0165bd6959b63f6eac45fa3454116186d8278cf6ffb1b127`。process auditは`[]`。 |

### authority／durable sliceの残余risk／次gate

- `CInMemoryStorageService`、legacy monolith、resource/package/runtime hermetic、protocol/persistence golden、MinGW native、全葉の交換可能性、同等保証範囲の性能比較、mutation別の厳密な再ビルド閉包は未完了である。
- CMake object path警告とReleaseの既存legacy warningは今回のgreenに含めず、build-speed/quality gateで別途扱う。
- 最終 `sakura.exe` の静的リンクは許容された全体結合であり、leaf単独link closureの証拠と混同しない。

## request product-provider と公開契約統合の最新 Verify / Expect（2026-08-05）

Request は transport/proxy/retry/cancellation/cache の契約と WinHTTP adapter を `sakura_request` に固定し、利用側から private header へ到達できない公開 include view に移した。依存方向は `consumer -> dependency/provider` として、`sakura_app -> sakura_request`、`tests1 -> sakura_request`、`sakura_request_tests -> sakura_request` を manifest witness 付きで宣言している。`sakura.sln` には generated request project と x64 Debug/Release mapping も登録した。

| Verify | Expect / 実結果 |
|---|---|
| ownership / public contract | `sakura_core/include/sakura/request/` の3公開契約、3実装 source、`winhttp` system library を `sakura_request` が所有。旧 `platform/request/*.h` と source-level link directive は残していない。 |
| standalone MSVC | x64 Debug/Release の `sakura_request_tests` が各5/5 pass。 |
| standalone CMake/Ninja | MSVC x64 Debug/Release が各6/6 test、CTest 1/1 pass。既知の長い object path warning のみ。 |
| full solution | `sakura.sln` x64 Debug/Release が exit 0。Release は既存 warning 675、error 0。 |
| compatibility tests | tests1 の RequestService focused filter は 22/22 pass。これは統合互換性の証拠で、standalone ownership の証明とは別に扱う。 |
| boundary evidence | `verify component-boundary sakura_request_tests` Debug/Release は `failures: []`、closure `[sakura_request, sakura_request_tests]`、`winhttp.lib` observed、package restore false。静的 archive の provider 単体は MAP が無いため、この checker の直接対象にしない。 |
| generated graph | manifest check、`generate --check`、6 context の graph check が pass。graph hash は `sha256:232dbff4ed9ed11f2de9ded7388f9eac752352a8b5ba907a80766daca3a4f8b5`。 |
| split inventory | `tests1` 2767、filesystem 13、request 5、security 1、serialization 4、storage 9、URI 9、合計 2808。refresh/verify とも missing/unexpected 0、fingerprint は `sha256:ef200b9707968c3b61a4d564341a6c1fbd10980cc742ee0ab720f13af350df05`。 |
| MinGW | CMake が MinGW Makefiles generator と CXX compiler を検出できず開始前終了。MSVC/CMake-MSVC の green と混同せず、環境 blocker として記録する。 |
| process cleanup | build/test/discovery 後に repository path を持つ runner/compiler/linker/MSBuild を再列挙し、最終 audit は `[]`。 |

### request slice の残余リスク / 次の gate

- `sakura_request` は L4 の明示的な static-link closure として最終 `sakura.exe` に入る。L5 DLL 化は今回の範囲外である。
- 既存の WinHTTP/RequestService tests1 source は process/UI compatibility の統合証拠として残る。`tests1` を削除する前に、同等保証を別 runner へ移す必要がある。
- 次は resource/package/runtime asset、protocol golden、hermetic staging、同一保証範囲の性能・rebuild closure を検証する。Request slice の成功を Issue #15 全体の L4 graduation と取り違えない。
### request rebuild-closure の追加検証（2026-08-05）

`verify rebuild-closure sakura_request_tests` は MSVC x64 Debug と CMake/Ninja MSVC x64 Debug で `ok: true` になった。cleanだけが明示configure 1で、no-op/private-cpp/public-contractはconfigure 0。期待したcompile/archive/link閉包と観測が一致し、package restore 0、projection変更0、test exit 0、workspace cleanup trueだった。MSVCのno-op/private-cpp/public-contractは372.138/2,062.177/6,065.066 ms（DesignTimeBuild 272.671 ms）、CMakeは42.261/1,221.141/2,359.064 ms。private-headerはproviderにprivate headerがないため型付き`not_applicable`である。

この検証で、直接subprocess経路のCMakeは`VSLANG=1033`を固定し、clean後のphaseでは明示configureを抑止する必要が判明した。通常component buildの自動reconfigureは維持している。MSVC/CMake-Ninja x64 Releaseも同じ閉包でgreenになった（MSVCのno-op/private-cpp/public-contractは339.350/2,408.493/7,098.779 ms、CMakeは42.030/1,760.059/3,272.606 ms）。MinGWと他のleafは未検証であり、Issue #15全体のL4 graduationとは分離する。

## Control IPC protocol leafの最新 Verify / Expect（2026-08-05）

Control IPC全体を一度に移動せず、最初に wire framing／bounded field codec だけを
`sakura_controlipc_protocol` として抽出した。この節のgreenは protocol leaf と v1
compatibility fixture の独立性だけを意味し、transport、security、endpoint discovery、
RPC、authority、process composition のL4完了を意味しない。依存矢印は常に
`consumer -> dependency/provider`で記録している。

| Verify | Expect / 実測結果 |
|---|---|
| ownership / public contract | `sakura_controlipc_protocol`が `ControlIpcProtocol.cpp` と `sakura_core/include/sakura/controlipc/ControlIpcProtocol.h` を所有する。旧 `sakura_core/platform/controlipc/ControlIpcProtocol.h` は削除し、transport／runtime／RPC／tests1は公開includeへ移行した。production includeの旧private path scanは0（pilotの負側 `__has_include` guardだけが旧名を文字列として保持）。 |
| 参照方向 | `sakura_app -> sakura_controlipc_protocol`、`tests1 -> sakura_controlipc_protocol`、`sakura_controlipc_protocol_tests -> sakura_controlipc_protocol`。protocol leafはControl transport、storage、UI、resource、process runtimeへ逆依存しない。 |
| golden fixture / protocol boundary | `sakura-controlipc-v1-wire-contract` と `controlipc-protocol-v1-golden-fixture` をmanifestへ追加。fixtureはproducer/consumer、major/minor、Hello requestとterminal CancelAckのwire bytesを固定し、Python self-consistency testがlength、little-endian header、direction、terminal flag、boundsを検査する。wire shapeを型移動だけで変更しない。 |
| standalone runner | `sakura_controlipc_protocol_tests` は package/resource-less な5 selector runner。fragmented/coalesced decode、minor version/UTF-8、invalid direction、sticky failure/reset terminalを検査し、既存tests1の `ControlIpcProtocol.*` 12 selectorは互換性証拠として残す。 |
| MSVC component / boundary | x64 Debug／Releaseでcomponent buildと5/5 testが成功。boundaryは両構成 `ok=true`、closure `[sakura_controlipc_protocol, sakura_controlipc_protocol_tests]`、package restore false、failures空。 |
| CMake/Ninja component / boundary | MSVC x64 Debug／Releaseでbuild、CTest 1/1、5/5、boundary `ok=true`。静的archiveのprovider memberとfixture witnessを確認し、無関係なresource/package/test assetは閉包に入らない。隔離buildの長いobject path警告は既存の残余riskとして扱う。 |
| rebuild closure | `verify rebuild-closure sakura_controlipc_protocol_tests` を MSVC Debug／Release と CMake/Ninja MSVC Debug／Releaseの4 contextで、短いworkspace rootから実行し `ok=true`。cleanだけconfigure 1、後続no-op/private-cpp/public-contractはconfigure 0、宣言済みcompile/archive/link閉包、package restore false、projection変更0、test exit 0、workspace cleanup true。private header phaseはprivate headerを所有しないため理由付き `not_applicable`。 |
| compatibility integration | `tests1.exe --gtest_filter=ControlIpcProtocol.*` は12/12 pass。全MSVC solution x64 Debug／Releaseもprotocol projectを含めexit 0（Releaseは既存warning 675、error 0）。 |
| generated / DAG / static | `manifest check`、`generate --check`（stale 0）、全6 context `graph check`（failures 0）、`compileall`、build-tool unittest 127件が成功。graph hashは `sha256:9c912a52e0bbd320756369f126622131b66acfe4398275d08f6e561ff5ddfa97`。 |
| MinGW / cleanup | MinGW x64 DebugはCMakeが `MinGW Makefiles` のbuild programとCXX compilerを検出できず、configure開始前にexit 1。未実行をPASSとは扱わない。全build/test後のrepository path付きprocess auditは `[]`。 |

### Control IPC protocol sliceの残余リスク / 次のgate

- C++ pilotとJSON fixtureは同じwire constantsを検査するが、pilotはfixture JSONを実行時parseしない。fixtureの二重記述を解消するschema/codegenまたはgolden loaderは次のcompatibility gateで判断する。
- 4-context rebuild closureはWindowsの長い一時パスでMSVC compilerがPDB/objectを生成できずC1083になるため、短いworkspace rootで再実行した。これはコード失敗ではないが、CIと開発CLIのpath-length headroomを別gateに残す。
- MinGW toolchainはこのhostにない。MinGWを完了判定するには実際のgenerator/compilerでcomponent build、CTest、boundary、rebuild closureを再実行する。
- 次はresource／package／runtime assetの明示閉包と、Control protocol compatibility fixtureを利用するtransport/endpoint integrationの最小縦切りを、同じ contracts-only・hermetic・terminal-state条件で進める。`tests1`削除、legacy monolith削除、全体L4 graduationはまだ開始可能な完了条件ではない。

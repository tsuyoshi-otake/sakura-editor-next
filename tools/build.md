# ビルド方法

<!-- TOC -->

- [ビルド方法](#ビルド方法)
  - [必要なツール](#必要なツール)
    - [サクラエディタ本体のビルド](#サクラエディタ本体のビルド)
    - [インストーラのビルド](#インストーラのビルド)
  - [ビルド手順](#ビルド手順)
    - [実行ファイルのみ](#実行ファイルのみ)
      - [GUI でビルド](#gui-でビルド)
      - [コマンドラインでビルド](#コマンドラインでビルド)
    - [すべてビルド](#すべてビルド)
  - [開発者向け情報](#開発者向け情報)
    - [ビルドで使用する環境変数](#ビルドで使用する環境変数)
    - [ビルドに使用されるバッチファイル](#ビルドに使用されるバッチファイル)
    - [x64 ビルドの増分検証](#x64-ビルドの増分検証)
    - [単体テストの実行](#単体テストの実行)
    - [カバレッジマップによる影響テスト選択](#カバレッジマップによる影響テスト選択)
    - [デバッグ方法](#デバッグ方法)
    - [アセンブリ一覧の生成](#アセンブリ一覧の生成)
    - [githash.h の更新をスキップ](#githashh-の更新をスキップ)
    - [PowerShellによるZIPファイル処理の強制](#powershellによるzipファイル処理の強制)
    - [CIビルドのスキップ](#ciビルドのスキップ)
    - [Editor Core意味的負債台帳](#editor-core意味的負債台帳)
  - [MinGWビルド (実験的)](#mingwビルド-実験的)
    - [MinGWでのビルド方法](#mingwでのビルド方法)
  - [参考情報](#参考情報)
    - [Chocolatey関連](#chocolatey関連)
      - [Chocolateyのインストール](#chocolateyのインストール)
    - [Msys2関連](#msys2関連)
      - [Msys2のインストール](#msys2のインストール)
      - [Msys2コンソールを開く方法](#msys2コンソールを開く方法)
    - [MinGW w64関連](#mingw-w64関連)
      - [MinGW w64のインストール](#mingw-w64のインストール)

<!-- /TOC -->

## 必要なツール

### サクラエディタ本体のビルド

C++20をサポートするC++コンパイラーが必要です。

- [最新のVisual Studio][Visual Studio 最新版] (推奨)
- [以前のバージョンのVisual Studio][Visual Studio 以前のバージョン]
  - Visual Studio 2019 以降
- MinGW64 GCCコンパイラー

**補助ツールとして以下が必須です。**
|ツール名|exe名|説明|
|--|--|--|
|HTML Help Workshop|hhc.exe|Visual Studio同梱のもの|
|CMake|cmake.exe|Visual Studio同梱のもので可|
|PowerShell Core|pwsh.exe|Microsoft Storeなどからインストール|
|[7-Zip](https://7-zip.opensource.jp/)|7z.exe|外部依存ファイルの解凍に使用します。|
|Locale Emulator|LEProc.exe|日本語環境以外でHTMLヘルプをビルドする場合に利用します。|
|Auto HotKey|AutoHotKey.exe|日本語環境以外でHTMLヘルプをビルドする場合にソースに腹持ちしたLocale Emulatorを展開する際に利用します。|

### インストーラのビルド

インストーラをビルドする場合のみ必要です。

- [Inno Setup](https://jrsoftware.org/isdl.php) (ISCC.exe)
  - 必須バージョン: 6.7.0 以降
  - 推奨バージョン: [innosetup-6.7.3.exe](https://files.jrsoftware.org/is/6/)

```pwsh
choco install InnoSetup -y
```

詳細は [インストーラビルドの仕組み](../installer/readme.md) を参照してください。

## ビルド手順

### 実行ファイルのみ

#### GUI でビルド

Visual Studio で `sakura.sln` を開いてビルドします。

#### コマンドラインでビルド

用途に応じて次のコマンドを使い分けます。

ビルドのcanonical APIは、Python 3.10以上の標準ライブラリだけで動く
`py -3 tools/build/sakura_build.py`です。`sakura-build.bat`だけが全引数と終了コードを
無変更で転送する正規shimです。既存のnamed `build-*.bat`は旧構文を変換する暫定compatibility
adapterであり、厳密な無判断shimではありません。新規のCI、IDE、文書からは直接Python CLIを使い、
named batchはR10で削除します。

```cmd
sakura-build.bat manifest check
sakura-build.bat generate --check
sakura-build.bat graph check --all-contexts
sakura-build.bat build dev x64 Debug --jobs 8
```

`--jobs` は MSBuild/CMake へ渡す全体予算です。MSBuild では `/m:P` と `/MP:C` を
`P*C <= jobs` となるよう設定し、`--jobs 1` では `/m:1` と `/MP1` を明示します。
既存 batch shim の既定予算は論理 CPU 数で、`SAKURA_BUILD_JOBS` で上書きできます。
低レベルの task scheduling は MSBuild/CMake が担当し、CLI は独自 jobserver を実装しません。

| 用途 | コマンド | ビルドする範囲 |
|--|--|--|
| 普段の編集・動作確認 | `build-dev.bat` | `sakura_core\sakura.vcxproj` のみ |
| 本体と単体テストの確認 | `build-sln.bat` | `sakura.sln`（本体と `tests1`） |
| 配布成果物の作成 | `build-all.bat` | 本体、単体テスト、HTML ヘルプ、インストーラ、ZIP |

通常の編集では `build-dev.bat` を使用すると、`tests1` プロジェクトの評価・コンパイル・リンクを省略できます。単体テストのビルドや配布準備には `build-sln.bat` または `build-all.bat` を使用してください。

component graph の唯一の定義は `src/main/modules/modules.json` です。manifest変更後は
`sakura-build.bat generate` を明示的に実行し、CIや通常ビルドでは
`sakura-build.bat generate --check` により生成物のstalenessを検査します。通常ビルドが
生成済みproject fragmentを暗黙に書き換えることはありません。

R0の製品実行証跡は、静的なrepository台帳とは別に収集してから明示的にmergeします。

```cmd
sakura-build.bat inventory observe-product ^
  --context msvc-x64-debug --product sakura_app --jobs 1 ^
  --output build/evidence/r0/native-msbuild-product.json

rem generatorのExec実行まで観測する場合だけ、clean rebuildを明示する
sakura-build.bat inventory observe-product ^
  --context msvc-x64-debug --product sakura_app --jobs 1 --rebuild ^
  --output build/evidence/r0/native-msbuild-product.json

sakura-build.bat inventory snapshot-resource-ids ^
  --image ja-JP=x64/Debug/sakura.exe ^
  --image en-US=x64/Debug/sakura_lang_en_US.dll ^
  --image zh-CN=x64/Debug/sakura_lang_zh_CN.dll ^
  --accept-current

sakura-build.bat inventory observe-resources ^
  --context msvc-x64-debug --product sakura_app ^
  --native-evidence build/evidence/r0/native-msbuild-product.json ^
  --resource-id-baseline tools/build/baselines/sakura_resource_ids.json ^
  --compat-image en-US=x64/Debug/sakura_lang_en_US.dll ^
  --compat-image zh-CN=x64/Debug/sakura_lang_zh_CN.dll ^
  --output build/evidence/r0/native-resource-table.json

sakura-build.bat inventory repository ^
  --context msvc-x64-debug --product sakura_app --provider sakura_uri ^
  --native-evidence build/evidence/r0/native-msbuild-product.json ^
  --resource-evidence build/evidence/r0/native-resource-table.json ^
  --output build/evidence/r0/repository-inventory.json --strict
```

`observe-product`はMSBuildの製品Buildが成功した後、`CL.read`/`CL.command`、`rc.read`、
`link.read`/`link.command` trackerと、今回の実行だけに属するdiagnostic logを解析します。diagnostic logは
PIDとUUIDで一意な一時pathへ出力し、成否にかかわらず収集後に削除します。翻訳単位、source-controlled
input、生成header/PCH、RC input、link object/resource/library、対象generator targetの終端状態を
repository-relativeな証跡として保存します。証跡にはgraph、context、product、観測source/生成物/trackerの
SHA-256を保持し、欠落、変更、別context、Build未実行のsnapshotはrepository台帳でblockerになります。
製品trackerはvcxprojで宣言された`IntDir`を評価して解決し、`app-avx2`や`app-o1`等の兄弟variantを
名前の類似だけで選びません。observerは内部propertyでそのBuildだけlink MAPを有効化し、宣言providerの
archive memberが最終製品へ実際に選択されたことを記録します。Release LTCGでprovider libraryがlink command
payloadへ現れない場合も、trackerのrepository library inputとの和集合で照合します。MAP生成propertyは証跡収集専用で、
通常buildの出力契約へは追加しません。

通常のBuildでgeneratorがup-to-date skipになった場合は、target schedulingの観測であってExec実行の
観測ではありません。`--rebuild`は製品をclean rebuildして実際の`Exec` taskを観測するための証跡専用optionです。
全翻訳単位を再構築し、外部package処理も起動し得るため、通常の局所開発ループでは使用しません。generator
実行をgreenにするには、対象targetの実行だけでなく、その厳密なoutput pathが同じ観測のcompiler/RC/link
inputに現れ、producerとconsumerを相関できなければなりません。diagnostic logの時刻や所要時間、一時pathは
hard evidence hashへ含めません。

`build/<Platform>/CMakeTools`は本体と言語DLLが共有するgenerator workspaceです。通常のproject／言語DLLの
Clean・Rebuildはこのdirectoryを削除せず、各generator targetの`Inputs`／`Outputs`で必要な更新だけを行います。
完全なgenerator実行を採取する`inventory observe-product --rebuild`だけが、内部MSBuild propertyを明示して
共有workspaceを先にcleanします。このpropertyは通常のビルド操作用の公開環境変数ではありません。

CMakeがVisual Studio向けに生成する`add_custom_target`は、phony outputのため親buildごとに要求される場合があります。
この場合もobserverは実inputと実outputのcontent hashを比較し、変更がなければ展開、nested build、copy、state更新を
行いません。state fileは全outputの生成成功後だけ発行します。ctagsは親repositoryのgitlinkが指すexact commitを
lock内でarchive/buildし、submodule worktreeのdirty/untracked fileを入力にしません。runtime assetはsymlinkを
実providerへ解決してSHA-256で比較するため、同一内容のstageによってmtimeを更新しません。

`observe-resources`は、先に収集したnative product evidenceと現在の製品EXEのhashが一致する場合だけ、
そのEXEをWindowsのdata/image resourceとして開きます。アプリケーションのentry pointは実行しません。
PE resource tableのtype、name、language ID、size、content SHA-256を正規化し、100,000 entry、1 entry
64 MiB、合計512 MiBを上限として収集します。生成済み製品を再利用するため、このcommand自体はcompile、
link、package restoreを起動しません。

`snapshot-resource-ids`は、`sakura_rc.h`の数値mapping、各言語RC/RC2内で参照されるsymbol、実バイナリの
top-level resource IDとdialog/menu/accelerator/string block内部の数値IDを、content非依存のgolden baselineへ
保存します。出力先の既定値は`tools/build/baselines/sakura_resource_ids.json`です。既存baselineの内容を変更する
場合は`--compatibility-version`を増加させる必要があり、`--accept-current`の明示なしではsnapshotを作成できません。

`observe-resources`へbaselineと全言語imageを渡すと、現在のheader/source/image contractをbaselineと比較し、
一致した場合だけcanonical/nested numeric resource-ID compatibilityをobservedにします。日本語imageには検証済み
native productを必ず使用し、`--compat-image`では残りの言語roleを指定します。baseline、header、RC/RC2、imageの
SHA-256をresource evidenceへ保持するため、採取後の入力変更はvalidationでstaleになります。baselineを指定しない
従来のtop-level table観測も可能ですが、その場合は`RESOURCE_ID_COMPATIBILITY_UNOBSERVED`をblockerとして残します。

Debug製品にはMAPがないため、link input setの観測だけでstatic archive内の採用memberを証明したとは
扱いません。またgeneratorのExecと消費先を相関できても、生成規則の`Inputs`/`DEPENDS`が完全であること、
RC inputの観測がresource table/ID互換であること、vcpkg targetの実行やtrackerの存在がrestore内容であることは
別の証明です。これらの未宣言・未観測gateが残る間、`--strict`は終了コード5を返します。

### Editor Core意味的負債台帳

Issue #18のR0/R1ゲートは、ビルド依存グラフとは別に、Editor Coreの意味的な結合を
再現可能なソース観測として記録します。canonical CLIから次を実行します。

```cmd
py -3 tools/build/sakura_build.py inventory semantic ^
  --baseline tools/build/baselines/editor-core-semantic.json ^
  --output build/evidence/r0/editor-core-semantic.json --strict
```

`--strict`の終了コードは、baselineのスキーマ不一致・読み取り失敗が2、ラチェット対象の
増加が11、成功が0です。`--accept-current`は、レビュー済みの基準更新時だけ使用します。
baselineはソース相対パスと内容から決定的なfingerprintを作り、出力は一時ファイルから
atomic replaceします。同じ入力で再実行しても不要なmtime更新は発生しません。

台帳はASTや所有権解析の代替ではありません。現在は、`GetDllShareData`/
`GetEditWnd`/`GetEditDoc`、生の`new`/`delete`、`catch (...)`、Win32型の言及、private
include、tests1全体リンク・filtered testのヒントを明示的なラチェット対象にしています。
source file/line/include数とCEditWnd/CEditView/CEditDoc/CEditApp/DLLSHAREDATAのhotspotは
診断情報です。R2以降のSelection/CaretまたはWorking Copyの縦切りでは、台帳を更新してから
型付きport、ライフサイクル、テストの独立性を別ゲートで証明します。

`tests1`分割前の保証集合は`src/test/test-inventory.json`で凍結します。`test_id`は
source-controlledな安定ID、`runtime.runner_id`と`runtime.selector`は実行時mappingです。
runner分割時は安定IDを維持し、runtime mappingだけを変更します。discovery失敗、0件取得、
重複ID/selector、fingerprint不一致は終了コード7で失敗します。

```cmd
py -3 tools/build/sakura_build.py test inventory collect --runner-id tests1 ^
  --executable x64/Debug/tests1.exe ^
  --output build/evidence/tests-current.json

py -3 tools/build/sakura_build.py test inventory compare ^
  src/test/test-inventory.json build/evidence/tests-current.json

py -3 tools/build/sakura_build.py test inventory verify-runtime ^
  src/test/test-inventory.json ^
  --runner tests1=x64/Debug/tests1.exe ^
  --runner sakura_uri_tests=build/components/msvc-x64-debug/sakura_uri_tests/bin/sakura_uri_tests.exe
```

比較のhard gateはstable ID集合とenabled/disabled状態です。runner/selectorのremapは
明示的に報告しますが、stable IDが維持されていれば保証差分とは扱いません。baselineには
収集元revision、dirty状態、test executableのSHA-256を保存します。

複数runnerの現在値で台帳を更新する場合は`refresh-runtime`を使います。既存selectorが消えた場合は
silent deleteを拒否し、意味を維持した名称変更だけを`--remap stable-test-id=runner-id::selector`で
明示します。新規selectorは保証追加としてstable ID集合へ加わります。更新後は必ず同じrunner集合で
`verify-runtime`を実行し、missing／unexpectedがともに0であることを確認します。

```cmd
build-dev.bat <Platform> <Configuration>
```

既定のユーザープロファイルは実行ファイルの場所によらず、Windows の
%APPDATA%\sakura に保存されます。Debug/Release の切替、配布版の更新、
再インストール後も設定・拡張機能・プロファイルを共有できます。
実行ファイル隣接のポータブルプロファイルが必要な場合だけ、実行ファイルと
同じフォルダーに sakura.exe.ini を置き、[Settings] の MultiUser=0 を
明示してください。

**例: x64 の Debug 本体ビルド**
```cmd
build-dev.bat x64 Debug
```

本体と単体テストをまとめてビルドする場合は `build-sln.bat` を使用します。

```cmd
build-sln.bat <Platform> <Configuration>
```

**例: x64 の Release ビルド**
```cmd
build-sln.bat x64 Release
```

**Visual Studio 2019を指定してビルド**
```cmd
set NUM_VSVERSION=16
build-sln.bat x64 Release
```

参考: [MSBuildの検索について](./find-tools.md#msbuild) で `NUM_VSVERSION` の詳細を説明しています。

### すべてビルド

実行ファイル、ヘルプファイル、インストーラをすべてビルドします。

```cmd
build-all.bat <Platform> <Configuration>
```

**例: x64 の Release ビルド**
```cmd
build-all.bat x64 Release
```

x64 Release は、最小要件を AVX とする単一の `x64\Release\sakura.exe` を生成します。
AVX2 および AVX-512F/BW を使う処理はソース単位で分離してコンパイルし、プロセス起動時に
CPUID と XGETBV で CPU・OS の保存状態を一度だけ確認して、利用可能な最上位実装へ
関数ポインターを固定します。追加の ISA 別バイナリは生成しません。

### R1 candidate component pilot (URI)

Issue #15のR1基盤では、URIをcandidate vertical sliceとして検証します。これはL4独立性の完成、
`sakura_app`/`tests1`の置換、またはR1a完了の宣言ではありません。semantic graphと生成projectionの
確認は次のコマンドで行えます。

```cmd
py -3 tools/build/sakura_build.py manifest check
py -3 tools/build/sakura_build.py generate --check
py -3 tools/build/sakura_build.py graph check --all-contexts
py -3 tools/build/sakura_build.py inventory repository --context msvc-x64-debug --product sakura_app --provider sakura_uri
py -3 tools/build/sakura_build.py inventory repository --context msvc-x64-debug --product sakura_app --provider sakura_uri --strict
py -3 tools/build/sakura_build.py plan component sakura_uri_tests --context msvc-x64-debug --phases compile,link
py -3 tools/build/sakura_build.py plan component sakura_uri_tests --context cmake-msvc-x64-debug --phases compile,link
py -3 tools/build/sakura_build.py test component sakura_uri_tests --context msvc-x64-debug --jobs 1
py -3 tools/build/sakura_build.py test component sakura_uri_tests --context cmake-msvc-x64-debug --jobs 1
py -3 tools/build/sakura_build.py build fixture abi-pack-mismatch --context msvc-x64-debug
py -3 tools/build/sakura_build.py build fixture abi-iterator-mismatch --context msvc-x64-debug
py -3 tools/build/sakura_build.py build fixture abi-opaque-compatible --context msvc-x64-debug
py -3 tools/build/sakura_build.py path-matrix test component sakura_uri_tests --contexts msvc-x64-debug,cmake-msvc-x64-debug --jobs 1
py -3 tools/build/sakura_build.py verify rebuild-closure sakura_uri_tests --contexts msvc-x64-debug,cmake-msvc-x64-debug --jobs 1 --samples 5
```

`inventory repository`はR0 dependency/provenance台帳を
`build/evidence/r0/repository-inventory.json`へ保存します。baseline modeは収集に成功すればexit 0ですが、
それはL4独立性の合格を意味しません。`--strict`は未所有file、未申告include、未解決quoted include、
source内link directive、製品へのprovider source埋込み、CMake source glob、未分類package、global restore、
testからの製品object取り込み、共有resource ID header、partial/not-observed classが一つでも残ればexit 5です。
台帳はMSBuild/CMakeの生成出力・入力・tool witness、RC/RC2 include、AdditionalDependencies/ProjectReference、
vcpkg manifest install triggerも構造化します。ただし、build定義の静的観測はnative compiler/linker/resource compiler/
package restoreの実行観測を代替しません。全witnessはevidence fileに保持し、端末出力は件数とhashの有界な要約にします。

2026-08-04のbaselineでは`collection_ok=true`、`graduation_ready=false`でした。URIは`sakura_app`に
`UriIdentity.cpp`が直接compileされる`embedded_in_product`状態であり、生成provider projectへの参照もmanifest edgeもありません。
したがってcomponent pilotの単独build成功だけをもって製品上の独立componentとは扱いません。

public usage closureとfinal static-link closureは別の情報です。`propagation=none`はpublic propagationを
止めますが、providerのprivate/transitive static archiveをfinal linkから削除しません。compile-only edgeは
providerのpublic include rootだけを渡し、MSBuild ProjectReferenceやCMake link targetを追加しません。
component stale checkはcommitted stampのcontent hashを使い、選択contextのnative closureだけを検査します。
全projectionの再描画、generated root全走査、別context専用provider追跡は行いません。repository全体の
stale/unexpected outputはfull `generate --check`が検査します。条件付きlink edgeのMSBuild
`ProjectReference`はactiveな`Configuration|Platform`だけで有効になり、同じMSBuild物理キーを複数contextへ
割り当てるmanifestは拒否されます。

public-type-onlyの`contract`/`aggregate`はsource/private state/runtime assetを持たないheader-only interfaceです。
MSBuildではUtility、CMakeではINTERFACEへ投影し、空archiveを作りません。component CMake childではtoolchain/package
discovery環境を除去しますが、legacy/full commandの環境は変更しません。

C++ contract edgeは、consumerとproviderのproject compile profileから契約種別が要求するfieldだけを比較します。
MSVC projectionはedge/field単位の`#pragma detect_mismatch`をforce-includeし、`default_pack`や
`iterator_debug_level`の不一致をLNK2038で名前付き拒否します。これはgraph checkを置き換えるものではありません。
opaque C handle契約は`abi_family`と`arch`だけを要求するため、C++内部のpack差を理由に過剰拒否しません。
上記3 fixtureは「期待したlink失敗」を成功として型付きterminal stateへ変換し、opaque fixtureは実行まで検証します。

`path-matrix`はnormal、ASCII space、日本語の3 checkoutを実際に作り、canonical component testとboundary
evidenceをMSBuild/CMakeで実行します。semantic graph hashとroot非依存hard evidence hashが3 caseで一致し、
最後にcandidate componentだけをmanifestから戻すstandalone revert rehearsalが成功した場合だけgreenです。
VS付属CMakeが日本語pathで異常終了する環境では、CMake childの間だけ同一checkoutを指す短いASCII junctionを
使用します。`os.path.samefile`で同一性を確認し、build treeに記録したidentityからevidence収集時のpath spellingを
再生成します。junctionはsuccess、failure、timeoutの終了経路で削除され、MSBuildとlegacy/full CMakeには適用しません。

`verify rebuild-closure`は元checkoutを変更せず、`~/tmp`配下の短い隔離コピーでclean、反復no-op、
private cpp、private header、public contract、MSBuild DesignTimeBuildを実行します。object、archive、executableの
mtime・size・SHA-256をphase間で比較し、期待したcompile/archive/link集合との過不足、生成projectionの
書換え、package restore、component test終了値をJSONへ保存します。既定出力は
`build/evidence/r1a/rebuild-closure/evidence.json`です。URI pilotはprivate headerを明示所有し、providerだけの
compile/archiveと対象test linkへ閉じることを検査します。PCH、resource、generated inputは所有しないため、
それらのmutationを検証したことにはなりません。

有効なCMake/Ninja component build treeでは、canonical CLIは毎回の明示configureを省略し、native build graphへ
直接渡します。source root、generator、configuration、cache、native build fileのいずれかが一致しない場合は
configureを実行します。CMakeLists等が変更された場合の自動再configureはNinja/Makefilesが所有します。

この作業時点で、manifest/graph check、generator再生成、tools/buildの69 unit tests、URIのMSBuild Debug
component test、MSBuild `/t:Rebuild`、CMake/Ninja `--clean-first` + CTest Debug、component-boundary evidenceを
実行済みです。MSBuild rebuildは`/MP1`でcl.exe/link.exeを実行し、0 warning/0 errorでした。CMakeは明示的な
MSVC toolchain environmentを構築した後、component childだけの環境分離を有効にして4 build stepsとCTest
1/1 passを確認しました。生成URI projectのDesignTimeBuild、ABI mismatch/opaque C fixture、基本path matrix/standalone revert、
両backendのclean/no-op/private cpp/private header/public contract rebuild closureは確認済みです。5回no-opのevidence-mode native child中央値は
MSBuild 363.451 ms、CMake/Ninja 73.755 msで、CMakeの明示configureは0回でした。MSBuild DesignTimeBuild中央値は
267.125 msです。これらは移行前比較のないbaseline-onlyであり、canonical CLI全体の改善率とは扱いません。
R0 repository inventoryは2,673 C/C++/resource候補と8,647 includeを走査し、未所有file 27、
未所有providerへのinclude 250、未解決quoted include 17、source link directive 17を記録しました。
さらにgenerated/resource/product-link/packageの静的provenanceを記録し、global vcpkg restore、`tests1`の
製品object取り込み、language resourceの未所有、共有resource ID headerをblockerにしました。MSBuild x64 Debugの
native product observationでは566翻訳単位、PCH create 1/use 556/none 9、source input 1,151、生成header 16、
link object 566、resource 1を観測し、`sakura_app -> sakura_uri`が`UriIdentity.obj`を直接linkするwitnessを得ました。
一方、selected archive member、generator実行、resource table、package restoreは未観測のままです。同一入力の再収集で
hard evidence hash、JSON SHA-256、mtimeが不変で、strictは規範終了コード5でした。
legacy Release、MinGW、R1bのjunction/long-path/response-file matrixは別gateとして未実行であり、未実行の結果を
passとは扱いません。詳細な採否と残余riskは[`docs/l4-component-build-r1a-status.md`](../docs/l4-component-build-r1a-status.md)
に記録します。

## 開発者向け情報

### ビルドで使用する環境変数

[ビルドで使用する環境変数](./build-envvars.md) を参照してください。

### ビルドに使用されるバッチファイル

[ビルドに使用されるバッチファイル](./build-batchfiles.md) を参照してください。

### x64 ビルドの増分検証

Windows Terminal の取り込みファイルの SHA-256 を検証するには、以下を実行します。

```pwsh
.\tools\verify-build-incremental.ps1 -ValidateImportedFiles
```

既にビルド済みの x64 構成について、ベースラインビルドと続く no-op ビルドの binlog を保存し、`sakura.exe` の更新時刻が変わらず、2 回目の診断ログに `cl.exe`、`link.exe`、`CL`、`Link` タスクがないことを確認するには、以下を実行します。Visual Studio の開発者コマンド プロンプト、または `msbuild.exe` が `PATH` にある PowerShell から実行してください。

```pwsh
.\tools\verify-build-incremental.ps1 -VerifyNoOpBuild -Platform x64 -Configuration Release
```

binlog と診断ログは `artifacts/build-verification/` に保存されます。個別の非公開 `.cpp` を確認する場合は `-ProbeCpp <path>` を加えます。対象が含まれる `.vcxproj` を表示し、一時的に更新時刻を進めてビルドした後、元の更新時刻へ復元します。テスト後にこのリポジトリに関連するプロセスだけを報告するには、`-ListSurvivors` を使用します。このスクリプトはプロセスを終了しません。

### 単体テストの実行

`build-sln.bat` でビルドした構成の `tests1.exe` を実行します。

```cmd
x64\Debug\tests1.exe
```

`tests1.exe` には、実際のエディタ画面やダイアログを起動する UI・連携テストも含まれます。特に `WinMain/WinMainTest.*` は既定設定のテストプロファイルを使用するため、普段の設定とは異なる外観でエディタが起動します。

UI を起動しないローカル確認では、対象を確認して `--gtest_filter` で除外してください。現在のヘッドレス確認例は次のとおりです。

```cmd
x64\Debug\tests1.exe --gtest_filter=-MacroMgrTest.*:CPpaTest.*:SelectFileTest.*:FileDialog/FileDialogTest.*:CDlgProfileMgrTest.*:TrayWndTest.*:EditWndTest.*:WinMainFuncTest.*:WinMain/WinMainTest.*
```

このフィルターは UI・外部連携を含むテストを省くため、最終確認では必要なテストを別途実行してください。

GitHub-hosted runner は非対話セッションのため、標準 CI でも同じフィルターを環境変数 `GTEST_FILTER` から適用します。CI では CTest と Actions の両方に上限時間を設け、失敗時を含めてリポジトリ配下の `tests1.exe`、`sakura.exe`、および関連するカバレッジプロセスを検査・終了します。完全な UI・連携テストは、対話可能な Windows セッションで別途実行してください。

### カバレッジマップによる影響テスト選択

Issue #47 の feature PR 用選択器は、develop の全件テストから作った OpenCppCoverage
Cobertura 断片をスイート単位で統合し、変更ファイルを GoogleTest selector へ変換します。
マップのスキーマは [`tools/build/coverage-map.schema.json`](build/coverage-map.schema.json)
です。マップには develop の `base_sha`、`tests1.exe` の SHA-256、テスト inventory の
guarantee fingerprint を保存するため、別の develop 成果物を誤って再利用できません。
Actions cache は `tia-map-windows-x64-<base-sha>-<schema-version>`（CLI の
`coverage_cache_key` と同じ形式）を使い、base SHA を省略した共有キーを作ってはいけません。

```cmd
py -3 tools/build/sakura_build.py test coverage-map merge ^
  --base-sha <develop-sha> ^
  --test-binary x64/Debug/tests1.exe ^
  --inventory src/test/test-inventory.json ^
  --fragment FooTest.*::coverage/FooTest.xml ^
  --output coverage/coverage-map.json

py -3 tools/build/sakura_build.py --format json test coverage-map select ^
  --map coverage/coverage-map.json ^
  --inventory src/test/test-inventory.json ^
  --base-sha <develop-sha> ^
  --modules-json src/main/modules/modules.json ^
  --changed-file M::sakura_core/CEditView.cpp ^
  --smoke-selector RequestService.CancellationIsTerminalBeforeTransport
```

スイート単位の収集を複数 runner に分割する場合は、同じ inventory・runner・除外 selector
から `plan` を作ります。各 shard は通常の `merge` で小さな partial map を作り、最後に
`merge-partials` で provenance が一致するものだけを統合します。巨大な Cobertura XML を
job 間で渡してはいけません。

```cmd
py -3 tools/build/sakura_build.py --format json test coverage-map plan ^
  --inventory src/test/test-inventory.json ^
  --runner-id tests1 ^
  --shard-index 0 --shard-count 8 ^
  --exclude-selector MacroMgrTest.* ^
  --output coverage/plan-0.json

py -3 tools/build/sakura_build.py --format json test coverage-map merge-partials ^
  --base-sha <develop-sha> ^
  --inventory src/test/test-inventory.json ^
  --partial-map coverage/partial-0.json ^
  --partial-map coverage/partial-1.json ^
  --output coverage/coverage-map.json
```

`select` の `mode` は `selected`、`smoke`、`full` のいずれかです。マップ不在・SHA／inventory
不一致・Cobertura に存在しない production file・削除／rename・CMake/MSBuild/vcpkg/modules/
test-runner/PCH の変更・選択テストが全体の 65% 以上、または smoke selector の不整合は、
`full_fallback: true` と空の positive filter を返します。呼び出し側はこの結果を全件実行へ
渡し、0件成功として扱ってはいけません。Markdown等の文書だけが変更された場合は
`mode: smoke` となり、影響テスト数が0でも指定した smoke selector を必ず実行します。

`modules.json` の source/header/include root は module 単位の展開に使われるため、header や
inline/template の変更も同じ module の coverage suite を選べます。modules の所有範囲に
入らない production file は安全側に全件へフォールバックします。

Actions では `build-sakura.yml` が full Debug/Release 成功後の trusted `develop` push だけで
`coverage-map.yml` を呼びます。Debug の `tests1.exe` と PDB は retention 1 日の入力 artifact
として渡し、8 shard が XML を各 runner 内で検証して compact partial map だけを集約します。
最終 map は正確な develop SHA を含む cache key で保存されます。map 作成は required check では
なく、失敗時には cache を保存しません。

同一リポジトリの `feature/*` → `develop` PR はその PR の `base.sha` と完全一致する map だけを
restore し、selector の結果と `CNativeW.*` の smoke を `tests1` の `GTEST_FILTER` に渡します。
coverage map は `tests1` 専用なので、別々に CTest へ登録された component test executable と
pytest は、この選択時にも従来の full headless filter で必ず実行します。map 不在・provenance
不一致・不明な差分・選択器の例外は、従来の full headless suite へ戻ります。fork、`fix/*`、
`hotfix/*`、Dependabot、`develop` → `main` は常に full suite です。したがって map は高速化の
ためのヒントであり、テスト0件の成功や cache 書き込み権限を与えるものではありません。

各構成の選択決定は Actions artifact `test-selection-x64-Debug`／
`test-selection-x64-Release` 内の `tia-test-selection.json` に保存されます。`mode` と
`full_fallback` を確認すると、feature PR が選択実行したか、安全側の full fallback に戻ったかを
後から判別できます。

### デバッグ方法

- [タスクトレイのメニュー項目をデバッグする方法](./debug-tasktray-menu.md)
- [大きなファイルの作成方法](./create-big-file.md)

### アセンブリ一覧の生成

通常の Visual Studio / MSBuild ビルドでは、コンパイル時間とディスク書き込みを抑えるため `.asm` 一覧を生成しません。調査目的で必要な場合は、環境変数 `SAKURA_GENERATE_ASSEMBLY_LISTINGS` を `1` に設定します。

```cmd
set SAKURA_GENERATE_ASSEMBLY_LISTINGS=1
build-dev.bat x64 Release
set SAKURA_GENERATE_ASSEMBLY_LISTINGS=
```

設定を切り替えた直後はコンパイラーオプションが変わるため、次のビルドで一度だけ再コンパイルが発生する場合があります。`build-all.bat` はスクリプト内だけで、配布 CI は Release の MSBuild ステップだけで、この設定を自動的に有効化します。CMake からビルドする場合は `-DSAKURA_GENERATE_ASSEMBLY_LISTINGS=ON` を指定します。

一覧ファイル名はソースファイル名から決まりますが、Release の全プログラム最適化ではコード生成がリンカーへ遅延され、翻訳単位ではなく関数単位で分割されます。同じソース由来の関数が複数のコード生成スレッドに割り当てられると、同一の `.asm` を同時に開こうとして Win32 の共有チェックに失敗し、`C1083 ... Permission denied` と `LNK1257` として観測されます。そのため一覧生成が有効なときだけリンカーに `/CGTHREADS:1` を渡してコード生成を直列化しています。一覧を生成しない通常ビルドの並列度は変わりません。

### MSBuild の実行観測（バイナリログ／PerformanceSummary）

MSBuild ステップがどこで時間を使っているかを調べたいだけで、最適化・アセンブリ一覧・LTCG・ビルドターゲット・CI トリガーは何も変えたくない場合は、診断専用の環境変数を使います。`build-dev.bat`/`build-sln.bat`/`build-all.bat` のいずれで実行しても、内部で組み立てるすべての MSBuild コマンドラインに反映されます。

```cmd
set SAKURA_MSBUILD_BINLOG=%CD%\build\logs\msbuild.binlog
set SAKURA_MSBUILD_PERFORMANCE_SUMMARY=1
build-sln.bat x64 Release
set SAKURA_MSBUILD_BINLOG=
set SAKURA_MSBUILD_PERFORMANCE_SUMMARY=
```

`SAKURA_MSBUILD_BINLOG` はパスを設定すると `/bl:<パス>` を追加し、[MSBuild Structured Log Viewer](https://msbuildlog.com/) 等で開けるバイナリログを出力します。空文字列（空白のみを含む）を設定するのは明示的エラーです。`SAKURA_MSBUILD_PERFORMANCE_SUMMARY` は `1`/`true` で `/clp:PerformanceSummary` を追加してコンソール末尾にタスク別実行時間の要約を出力し、`0`/`false` は未設定と同じ無効な状態になります。それ以外の値はどちらの変数も明示的エラーとして失敗します。計測が終わったら両方とも解除してください。

### githash.h の更新をスキップ

ビルド時に git の commit hash を `githash.h` に出力します。これによりバイナリが commit hash から特定できますが、バイナリが変化しないリファクタリングでもバイナリが異なってしまいます。

検証を容易にするため、環境変数 `SKIP_CREATE_GITHASH` を `1` に設定することで commit hash の更新をスキップできます。

**注意:** `githash.h` が存在しない場合は、この環境変数に関係なく生成されます。

**実行例:**
```cmd
set SKIP_CREATE_GITHASH=1
build-sln.bat x64 Release
build-sln.bat x64 Debug
```

### PowerShellによるZIPファイル処理の強制

通常、`7z.exe` が利用可能な場合は自動的に使用されます（高速）。デバッグ目的で [PowerShellスクリプト](./zip/readme.md) を強制的に使用する場合:

```cmd
set FORCE_POWERSHELL_ZIP=1
build-sln.bat x64 Release
```

### Pull RequestではCIビルドをスキップしない

`main` と `develop` は required checks で保護されています。ドキュメントのみの Pull Request でも必要な Workflow が起動して成功結果を報告するため、コミットメッセージに `[ci skip]` または `[skip ci]` を含めないでください。Workflow 自体が起動しないと required check が未報告のままになり、Pull Request をマージできません。

## MinGWビルド (実験的)

**警告:** 生成されるバイナリは正しく動作しません。

### MinGWでのビルド方法

[MinGW w64のインストール](#mingw-w64のインストール) を完了後、以下の方法でビルドできます。

MINGW64コンソールで以下を実行。
```bash
cmake -S . -B build/MinGW -DCMAKE_BUILD_TYPE=Debug -DBUILD_PLATFORM=MinGW
cmake --build build/MinGW
ctest --test-dir build/MinGW --output-on-failure
```

または、コマンドプロンプトで以下を実行。
```cmd
build-gnu.bat MinGW Debug
build-gnu.bat MinGW Release
```

## 参考情報

### Chocolatey関連

#### Chocolateyのインストール

1. PowerShell管理者コンソールを開く:
   - Windowsタスクバーの検索窓に `powershell` と入力
   - `Windows PowerShell` を右クリックして「管理者として実行」

2. 以下のコマンドを実行:
   ```powershell
   Set-ExecutionPolicy Bypass -Scope Process -Force; [System.Net.ServicePointManager]::SecurityProtocol = [System.Net.ServicePointManager]::SecurityProtocol -bor 3072; iex ((New-Object System.Net.WebClient).DownloadString('https://chocolatey.org/install.ps1'))
   ```

3. インストール確認:
   ```powershell
   choco
   ```

詳細: [Chocolatey公式サイト](https://chocolatey.org/install)

### Msys2関連

#### Msys2のインストール

[Chocolatey](#chocolateyのインストール) をインストール後、PowerShell管理者コンソールで実行:

```powershell
choco install msys2 --params "/InstallDir:C:\msys64"
```

#### Msys2コンソールを開く方法

`C:\msys64\msys2.exe` を実行します。

### MinGW w64関連

#### MinGW w64のインストール

1. [Chocolatey](#chocolateyのインストール) をインストール
2. [Msys2](#msys2のインストール) をインストール
3. [Msys2コンソール](#msys2コンソールを開く方法) を開く
4. pacmanパッケージを最新化:
   ```bash
   pacman -Syuu
   ```
5. MinGW-w64をインストール:
   ```bash
   pacman -S --noconfirm mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake mingw-w64-x86_64-make mingw-w64-x86_64-7zip
   ```

<!-- リンク定義 -->
[Visual Studio 以前のバージョン]: https://visualstudio.microsoft.com/ja/vs/older-downloads/ "Visual Studio 以前のバージョン"
[Visual Studio 最新版]: https://visualstudio.microsoft.com/ja/downloads/ "Visual Studio 最新版"

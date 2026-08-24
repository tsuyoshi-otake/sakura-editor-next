# SENP パッケージ仕様（v1）

この文書は、Sakura Editor NEXT の `.senp` パッケージを作成・検証・
インストールするための正規仕様書です。実装上の正本は
[`rust/sakura_senp/src/lib.rs`](../rust/sakura_senp/src/lib.rs) と
[`sakura-senp-tool`](../rust/sakura_senp_tool/src/main.rs) です。
この文書はその実装が受け付ける形式を説明します。VSIX、Open VSX、または
VS Code Extension API の仕様ではありません。

## まず作る

SENP には、実行コードを持たない宣言型パッケージと、WebAssembly
Component を持つ runtime パッケージの 2 種類があります。

### 宣言型 language/grammar パッケージ

最小のソースツリーは次の形です。

```text
my-language/
├── senp.json
├── README.md
├── LICENSE
└── assets/
    └── syntaxes/
        └── my-language.tmLanguage.json
```

`senp.json` の `contributes.languages` と `contributes.grammars` は必ず対に
します。grammar の `path` は `assets/` から始まり、実際のファイルを指す
必要があります。この形式では `runtime` と `module/extension.wasm` を
置きません。したがって、言語・grammar だけを提供するパッケージに
`module/extension.wasm` は不要であり、置くと拒否されます。

リポジトリ内の実例は次のとおりです。

- [Shell Language Basics](../rust/extensions/sakura_shell_language_basics/)
- [Core Language Basics](../rust/extensions/sakura_core_language_basics/)
- [Database Language Basics](../rust/extensions/sakura_database_language_basics/)
- [Infrastructure Language Basics](../rust/extensions/sakura_infrastructure_language_basics/)
- [Configuration and Build Language Basics](../rust/extensions/sakura_configuration_language_basics/)
- [Legacy Language Basics](../rust/extensions/sakura_legacy_language_basics/)

### runtime パッケージ

runtime パッケージは `runtime` を宣言し、宣言された ABI の Component を
`module/extension.wasm` に格納します。最小の staging ツリーは次の形です。

```text
my-runtime/
├── senp.json
├── README.md
├── LICENSE
└── module/
    └── extension.wasm       # componentize 済みの Component
```

現在の runtime 実例は
[Indent Rainbow](../rust/extensions/sakura_indent_rainbow/) です。リポジトリに
置かれている実例のソースツリーには生成前の Rust ソースしかなく、
`module/extension.wasm` はビルド時の staging 出力です。

runtime の ABI は `sakura:senp/extension@1.0.0` です。現在の WIT world は
[`rust/wit/senp-extension.wit`](../rust/wit/senp-extension.wit) に定義され、
`editor-decorations` を export します。host は WASI やその他の ambient
import を linker に追加しません。Wasm 側の機能はこの WIT の境界だけを
使用してください。

## `.senp` は deterministic ZIP

`.senp` は ZIP アーカイブですが、任意の ZIP を受け入れるわけではありません。
`sakura-senp-tool pack` が生成するアーカイブは、同じソースバイト列からは
同じバイト列・同じ archive SHA-256 になります。

`pack` は次の方法で出力します。

- アーカイブ内パスを UTF-8 の `/` 区切りで扱い、`BTreeMap` の順序で書く。
- ZIP compression は `Stored`（無圧縮）を使う。
- ZIP の更新時刻は `DateTime::default()` に固定する。
- Unix permission は `0o100644` に固定する。
- payload の SHA-256 を同じパス順で `integrity/SHA256SUMS` に書く。
- signing key が指定されたときだけ `signature/ed25519.sig` を追加する。

検証側は `Stored` と `Deflated` のみを許可します。`Deflated` の入力を
検証できても、`pack` が生成する形式は `Stored` です。

## アーカイブの構成

アーカイブで使用できるパスは次のものだけです。ZIP のディレクトリ
エントリーは使用できません。

| パス | 必須性 | 内容 |
| --- | --- | --- |
| `senp.json` | 必須 | v1 manifest。UTF-8 の strict JSON。 |
| `README.md` | 必須 | パッケージの説明。UTF-8。 |
| `LICENSE` | 必須 | パッケージのライセンス文。UTF-8。 |
| `module/extension.wasm` | `runtime` 宣言時だけ必須 | runtime host が Component として読み込む bytes。宣言型パッケージには存在してはならない。`sakura_senp` の ZIP validator 自身は Wasm の Component 形式までは解析しない。 |
| `integrity/SHA256SUMS` | 必須 | payload 全体の canonical SHA-256 coverage。 |
| `CHANGELOG.md` | 任意 | `pack` の source directory では UTF-8。既存 archive の verify では 4 MiB 制限と checksum coverage を検査するが、本文の UTF-8 decode は行わない。 |
| `assets/**` | 任意 | manifest が参照する grammar/configuration などの payload。 |
| `signature/ed25519.sig` | 任意 | Ed25519 署名の canonical な 1 行。signing key を指定した `pack` が生成する。 |

`SHA256SUMS` と署名自身は checksum coverage に含めません。それ以外の
アーカイブエントリー（manifest、README、LICENSE、runtime module、
CHANGELOG、assets）はすべて coverage に含めます。

ソースディレクトリを `pack` するときは、上表以外のトップレベルファイルは
アーカイブへコピーされません。`CHANGELOG.md` と `assets/` は存在するとき
だけ取り込まれます。`assets/` 内のシンボリックリンク、ディレクトリ以外の
特殊ファイル、16 段を超えるネストは拒否されます。

### パス規則

ZIP entry name、checksum 文書のパス、manifest が参照する asset path は
次の規則に従います。

- 空でない UTF-8 である。
- `/` 区切りだけを使用する。`\`、NUL、`:` を含めない。
- `/` または `\` で始めず、`/` で終わらない。
- 各 path component は通常の名前であり、`.`、`..`、root、prefix を含めない。
- 大文字小文字を畳み込んだ同一 path も重複として拒否する。
- grammar の `path` と language の `configuration` は `assets/` で始まり、1,024 バイト以下でなければならない。
- archive の `assets/**` entry 名には、上記の manifest 参照 path 以外の個別の長さ上限はありません（entry・archive 全体のサイズ上限は適用されます）。

アーカイブ検証では ZIP directory entry、許可リスト外の path、非 regular
entry、許可されない compression も拒否します。ソースまたはインストール
先の symlink/reparse point も拒否します。

### サイズと個数の上限

上限値は実装の定数そのものです。

| 対象 | 上限 |
| --- | ---: |
| ZIP ファイル（compressed archive bytes） | 64 MiB |
| 展開後の全 entry 合計 | 64 MiB |
| 1 entry | 32 MiB |
| document（manifest、README、LICENSE、CHANGELOG、checksum、signature） | 4 MiB |
| ZIP entry 数 | 256 |
| compression ratio | `uncompressed_size / max(compressed_size, 1)` の整数商が 100 以下 |
| `assets/` のディレクトリ深さ（pack 時） | 16 以下 |

`pack` は checksum と署名のために 2 entry 分を予約するため、payload は
最大 254 entry です。署名なしの完成アーカイブは最大 255 entry、署名あり
なら最大 256 entry になります。検証側の一般上限は 256 entry です。

## Manifest (`senp.json`)

JSON の object member は重複してはならず、末尾に余分な JSON を続けては
なりません。manifest とすべての nested object は unknown field を拒否します。
JSON のキーは Rust struct の `camelCase` 形です。

### 共通フィールド

| JSON field | 型 | 必須性 | 検証内容 |
| --- | --- | --- | --- |
| `schemaVersion` | integer | 必須 | `1` だけを許可。 |
| `id` | string | 必須 | 1–128 バイト。ASCII 小文字、数字、`.`、`-` のみ。先頭末尾を `.`/`-` にしない。 |
| `displayName` | string | 必須 | `trim()` 後が空でなく、160 バイト以下。 |
| `version` | string | 必須 | `trim()` 後が空でない。形式の semantic version 検証はしない。 |
| `publisher` | string | 必須 | `id` と同じ identifier 規則。publisher trust の key lookup に使う。 |
| `description` | string | 必須 | 512 バイト以下。 |
| `engines` | object | 必須 | unknown field 不可。`sakura` string が必須で、`trim()` 後が空でない。値の version 解釈はしない。 |
| `runtime` | object | 任意 | runtime パッケージでのみ指定する。下記参照。 |
| `activationEvents` | array of string | 任意 | runtime は `["onStartupFinished"]` だけ。宣言型は空配列（省略可）。重複不可。 |
| `capabilities` | array of string | 任意 | `editor.visibleText` と `editor.decorations` だけ。重複不可。宣言型は空配列（省略可）。 |
| `contributes` | object | 必須 | unknown field 不可。下記の 3 contribution 配列だけを受け付ける。 |

`displayName`、`description` などの長さは Rust の `str::len()` に相当する
バイト数です。`id` と `publisher` は大文字や `_` を含められません。

### Runtime と trust に関わるフィールド

`runtime` を指定する場合、object は次の 2 member だけを持ちます。

```text
"runtime": {
  "abi": "sakura:senp/extension@1.0.0",
  "module": "module/extension.wasm"
}
```

`abi` と `module` は上記の文字列に完全一致しなければなりません。
runtime package は `onStartupFinished` を 1 件だけ宣言し、
`module/extension.wasm` を含めなければなりません。宣言型 package は
`runtime` を省略し、activation event と capability を空にし、module を
含めてはなりません。

`sakura_senp` の `pack`/`verify` は module の存在、サイズ、checksum、path を
検査しますが、Wasm の Component model を解釈しません。runtime 用の bytes は
`sakura-senp-tool componentize` で作成し、実行時には
[`sakura-senp-host`](../rust/sakura_senp_host/src/main.rs) が Component として
ロードします。したがって、path が正しいだけの任意 bytes は archive の
形式検証を通っても runtime として実行できるとは限りません。

runtime package の capability は次の 2 つだけです。

- `editor.visibleText`
- `editor.decorations`

`editorDecorations` contribution を使う場合は runtime package であり、かつ
両方の capability を宣言する必要があります。runtime package であっても
capability を 0 件にすること自体は manifest validator では禁止されませんが、
`editorDecorations` を使う場合は上記の条件が必要です。

### `contributes`

`contributes` は次の配列だけを受け付けます。省略された配列は空配列として
扱います。全体として、`editorDecorations` も language/grammar も 0 件の
manifest は拒否されます。

#### `editorDecorations`

各要素は次の形です。

```json
{
  "id": "sakura.indent-rainbow",
  "kind": "indent"
}
```

`id` は共通 identifier 規則、`kind` は `indent` に完全一致しなければ
なりません。同一 manifest 内で id を重複させられません。この contribution
を宣言できるのは、前述の runtime と capability 条件を満たす package だけです。

#### `languages`

最大 64 要素です。各要素は次の fields を持ちます。

| JSON field | 型 | 必須性 | 制約 |
| --- | --- | --- | --- |
| `id` | string | 必須 | identifier 規則。language 配列内で一意。 |
| `aliases` | string array | 任意 | 最大 32 件。一意、空でなく各 1,024 バイト以下。 |
| `extensions` | string array | 任意 | 最大 128 件。一意、空でなく各 1,024 バイト以下。各値は `.` で始まり、`/`、`\`, `:`, NUL を含まない。 |
| `filenames` | string array | 任意 | 最大 128 件。一意、空でなく各 1,024 バイト以下。`/`、`\`, `:`, NUL を含まない。 |
| `filenamePatterns` | string array | 任意 | 最大 128 件。一意、空でなく各 1,024 バイト以下。 |
| `mimetypes` | string array | 任意 | 最大 32 件。一意、空でなく各 1,024 バイト以下。 |
| `firstLine` | string | 任意 | 空でなく 4,096 バイト以下。NUL を含まない。 |
| `configuration` | string | 任意 | `assets/` で始まる正規化 path、1,024 バイト以下。参照先の file が必須。 |

各 language には、`extensions`、`filenames`、`filenamePatterns`、
`firstLine` のうち少なくとも 1 つの selector が必要です。`aliases` や
`mimetypes` だけでは selector になりません。

#### `grammars`

最大 128 要素です。各要素は次の fields を持ちます。

| JSON field | 型 | 必須性 | 制約 |
| --- | --- | --- | --- |
| `language` | string | 任意 | 指定時は同じ manifest の language `id` に一致。 |
| `scopeName` | string | 必須 | 1–256 バイト。ASCII 英数字、`.`、`-`、`_`、`+` のみ。先頭末尾を `.`/`-` にしない。grammar 間で一意。 |
| `path` | string | 必須 | `assets/` で始まる正規化 path、1,024 バイト以下。参照先の file が必須。 |
| `injectTo` | string array | 任意 | 最大 64 件。一意、空でなく各 1,024 バイト以下。各要素は scope name 規則。 |
| `embeddedLanguages` | object | 任意 | key は scope name 規則、value は language identifier 規則。 |
| `balancedBracketScopes` | string array | 任意 | 最大 64 件。一意、空でなく各 1,024 バイト以下。 |
| `unbalancedBracketScopes` | string array | 任意 | 最大 64 件。一意、空でなく各 1,024 バイト以下。 |

`languages` と `grammars` は、両方空であるか、両方が空でない必要が
あります。language を宣言しただけ、または grammar だけを宣言した
manifest は拒否されます。grammar の `language` は省略できますが、指定
する場合は存在する language id でなければなりません。

validator は `path` の存在、archive の許可 path、scope/name の形式を
検証します。grammar ファイルや `configuration` ファイルの内部 JSON schema
そのものは `sakura_senp` では検証しません。

### 完全な runtime manifest 例

これはリポジトリの Indent Rainbow が使用する形です。

```json
{
  "schemaVersion": 1,
  "id": "sakura-indent-rainbow",
  "displayName": "Indent Rainbow",
  "version": "0.1.1",
  "publisher": "sakura.builtin",
  "description": "Colors indentation levels in the active editor.",
  "engines": {
    "sakura": ">=0.1.0"
  },
  "runtime": {
    "abi": "sakura:senp/extension@1.0.0",
    "module": "module/extension.wasm"
  },
  "activationEvents": [
    "onStartupFinished"
  ],
  "capabilities": [
    "editor.visibleText",
    "editor.decorations"
  ],
  "contributes": {
    "editorDecorations": [
      {
        "id": "sakura.indent-rainbow",
        "kind": "indent"
      }
    ]
  }
}
```

### 完全な宣言型 manifest 例

次は 1 language と 1 grammar だけを持つ最小の宣言型例です。

```json
{
  "schemaVersion": 1,
  "id": "example-toml-language",
  "displayName": "Example TOML Language",
  "version": "0.1.0",
  "publisher": "example.publisher",
  "description": "Adds TOML language selection and TextMate grammar.",
  "engines": {
    "sakura": ">=0.1.0"
  },
  "contributes": {
    "languages": [
      {
        "id": "toml",
        "aliases": ["TOML", "toml"],
        "extensions": [".toml"],
        "filenames": ["Cargo.lock"]
      }
    ],
    "grammars": [
      {
        "language": "toml",
        "scopeName": "source.toml",
        "path": "assets/syntaxes/toml.tmLanguage.json"
      }
    ]
  }
}
```

この例では省略された `runtime`、`activationEvents`、`capabilities`、
`editorDecorations` はそれぞれ `None` または空配列になります。実際の
grammar ファイルを `assets/syntaxes/toml.tmLanguage.json` に置かない限り、
pack/verify は `MissingRequiredEntry` で失敗します。

## Checksum の canonical form

`integrity/SHA256SUMS` は UTF-8 のテキストで、次の形式を厳密に守ります。

```text
<64 桁の小文字 sha256>␠␠<archive path>\n
```

具体的な規則は次のとおりです。

- 空でなく、最後に LF（`\n`）を 1 つ持つ。
- CR（`\r`）を含めない。CRLF は不可。
- digest は 64 文字の小文字 hexadecimal（`0-9a-f`）だけ。
- digest と path の間は ASCII space 2 個。
- path は上記の path 規則に従う。
- 行は path の文字列順に strictly ascending で、重複しない。
- checksum 自身と署名自身を除く全 archive entry を、過不足なく 1 回ずつ含む。
- 各 digest は、対応する entry の uncompressed bytes の SHA-256 と一致する。

`pack` は payload ごとにこの形式を生成します。展開済みデータの digest で
あるため、同じ payload でも ZIP compression の違いは checksum の値を変え
ません。一方、archive SHA-256 は ZIP 全体の bytes の digest です。

## Ed25519 署名と trust policy

署名は `signature/ed25519.sig` に UTF-8 の 1 行として格納します。

- 署名データは 64 バイトの Ed25519 signature を lowercase/uppercase どちら
  でも読める hexadecimal 128 文字で表し、末尾に LF を 1 つ付ける。
- `pack` の signing key 引数は 32 バイトの Ed25519 signing key を表す
  hexadecimal 64 文字です。`decode_hex` は入力 key の大文字も受け付けます。
- 署名対象は `integrity/SHA256SUMS` の canonical bytes だけです。manifest、
  ZIP local header、archive SHA-256 を別途署名する形式ではありません。
- verify 時は manifest の `publisher` と一致する公開鍵を使い、署名を
  `verify_strict` します。publisher key 引数は
  `publisher=public-key-hex` の 1 組です。公開鍵は 32 バイト（64 桁 hex）です。

`TrustPolicy` と CLI の対応は次のとおりです。

| policy / command | 署名 | archive pin | 初期 enabled | 用途 |
| --- | --- | --- | --- | --- |
| `PublisherKeys`（`verify`、`install`） | 必須。key がなくても、署名があっても key 不一致なら拒否。 | なし | `true`（既存の同 trust の状態を保持） | publisher package |
| `BuiltIn`（`inspect-builtin`、`install-builtin`） | 署名なしを許可。署名付きは publisher key がないため拒否。 | 必須。archive SHA-256 完全一致。 | `true`（既存の同 trust の状態を保持） | built-in package |
| `DeveloperUnsigned`（`install-dev`） | 署名なしだけを許可。unsigned でインストールし、disabled。 | なし | 常に `false` | ローカル開発 package |

つまり、built-in の trust は embedded package と期待 archive SHA-256 の
pin であり、`BuiltIn` policy で Ed25519 publisher signature を検証する
仕組みではありません。また `install-dev` に署名付き package を渡しても、
publisher key が policy にないため成功しません。publisher package の検証に
は、manifest の publisher と対応する公開鍵を明示してください。

署名用秘密鍵はリポジトリや package に含めず、外部の安全な秘密管理から
渡してください。この CLI は key をコマンドライン引数で受け取るため、
プロセス一覧や shell history に残る環境では取り扱いに注意が必要です。

インストール時は検証済み archive の全 bytes を profile root の
`content/<archive-sha256>/` に staging し、entry と
`.senp-archive-sha256` marker を検査してから公開します。profile の選択は
`profiles/<id>.json` に別途 atomic に公開されます。既存の同じ archive digest
の content があれば、内容が完全一致する場合だけ再利用します。

`list` はインストール済み content の path、サイズ、checksum coverage、
manifest、署名の形式を再検査しますが、publisher 公開鍵を持たないため、
既存 content の一覧処理で publisher signature を再検証しません。署名で
守られるのは検証時に読み取った archive の checksum document です。
content、checksum、marker を同一ユーザー権限で書き換えられる場合の追加の
改ざん耐性を、この format だけから主張してはいけません。

## インストールと有効化

tool の install root は次の profile-scoped layout です。

```text
<root>/
├── content/
│   └── <archive-sha256>/       # 検証済み payload と marker
├── profiles/
│   └── <id>.json               # active digest、trust、enabled など
└── uninstalled/
    └── <id>.json               # built-in の tombstone（存在する場合）
```

profile state は package の `id`、archive SHA-256、`signed`、trust class、
enabled state を持ちます。publisher/built-in の再インストールは、同じ id
かつ同じ trust class の既存 enabled state を保持します。developer unsigned
package は常に disabled です。

組み込み package を `uninstall-builtin` すると、content を削除せずに
`uninstalled/<id>.json` tombstone を先に公開します。これにより製品更新時も
active profile から隠れた状態を維持します。`install-builtin` による明示的な
再インストールは tombstone を消し、同じ embedded content を offline で
再利用できます。tombstone がある extension は `set-enabled` できません。

archive format 自体はインストール UI や marketplace protocol を定義しません。
Sakura Editor NEXT はローカル開発 package の導入 UI として、Extensions
ViewContainer の `SENP パッケージからインストール...` と、Extensions View
への drag-and-drop を提供します。drop は通常の `.senp` file を拡張子の
大文字・小文字を区別せず受け付け、1 回につき 16 files までを指定順に
`install-dev` へ渡します。処理前に batch 全体を検査するため、`.senp` 以外、
directory、存在しない file、または 17 files 以上を含む drop は package を
1 件もインストールせず拒否します。各 package では picker と同じ trust
確認を表示し、enable して install、disabled で install、または cancel を
明示的に選択します。途中で cancel または install failure になった場合、
後続 package は処理しませんが、それ以前に成功した install は維持されます。

marketplace、gallery search、network install は引き続き未実装です。tool を
直接使う場合は下記のコマンドを使用してください。

## `sakura-senp-tool` の使い方

以下では PowerShell の `$tool` を、ビルド済みの
`sakura-senp-tool.exe` に置き換えます。workspace の release build では
通常、次のように作成できます。

```powershell
cargo build --manifest-path rust/Cargo.toml `
  --package sakura-senp-tool --release --target x86_64-pc-windows-msvc
$tool = Resolve-Path rust/target/x86_64-pc-windows-msvc/release/sakura-senp-tool.exe
```

### unsigned 開発 package

```powershell
$source = (Resolve-Path .\my-language)
$package = (Join-Path $PWD 'out\my-language.senp')
$root = (Join-Path $PWD 'out\profile')

& $tool pack $source $package
& $tool install-dev $package $root
& $tool list $root
& $tool set-enabled $root my-language true
```

`pack` の標準出力は生成 archive の lowercase SHA-256 です。`install-dev` の
標準出力は `VerifiedPackage` の JSON です。unsigned developer package は
インストール直後 disabled なので、実行コードを使う場合も明示的に
`set-enabled ... true` を行います。`list` の JSON は `enabled`、`trust`、
`modulePath`、`moduleSha256` などを含みます。宣言型 package では module の
値は null/省略相当になります。

### publisher 署名付き package

`pack` の 3 番目の引数は、秘密の 32-byte signing key を 64 桁 hex で渡す
形式です。ここでは秘密鍵そのものを例示しません。

```powershell
$signingKeyHex = '<64桁のEd25519 signing key hex>'
$publisherKey = 'example.publisher=<64桁のEd25519 public key hex>'
$source = (Resolve-Path .\my-language)
$package = (Join-Path $PWD 'out\my-language.senp')
$root = (Join-Path $PWD 'out\publisher-profile')

& $tool pack $source $package $signingKeyHex
& $tool verify $package $publisherKey
& $tool install $package $root $publisherKey
```

`senp.json` の `publisher` が `example.publisher` でない場合、上記 key map
にはその値を指定してください。署名がない package を `verify` または
`install`（publisher policy）に渡すと `UnsignedPackage`、publisher key が
一致しない場合は `UntrustedPublisher` または `InvalidSignature` になります。

### built-in package と archive pin

built-in package は署名を付けず、pack 時に出力した archive hash を別ファイル
へ保存し、その値で検証・インストールします。

```powershell
$source = (Resolve-Path .\my-language)
$package = (Join-Path $PWD 'out\my-language.senp')
$hashFile = (Join-Path $PWD 'out\my-language.sha256')
$root = (Join-Path $PWD 'out\builtin-profile')

& $tool pack-builtin $source $package $hashFile
$hash = (Get-Content $hashFile -Raw).Trim()
& $tool inspect-builtin $package $hash
& $tool install-builtin $package $root $hash
& $tool list $root
& $tool uninstall-builtin $root my-language $hash
& $tool list-uninstalled $root
```

`pack-builtin` は signing key を受け取らず、`pack` を unsigned で実行して
hash file に hash と LF を書きます。`inspect-builtin` は `verify` と異なり、
publisher 署名を要求せず archive pin を検証します。

### runtime Component の作成

runtime の guest は `rust/wit/senp-extension.wit` を `wit-bindgen` で参照し、
`wasm32-unknown-unknown` 向けに build します。たとえばリポジトリの
Indent Rainbow の staging を手動で再現する場合は次の流れです。

```powershell
cargo build --manifest-path rust/Cargo.toml `
  --package sakura-indent-rainbow --release --target wasm32-unknown-unknown
cargo build --manifest-path rust/Cargo.toml `
  --package sakura-senp-tool --release --target x86_64-pc-windows-msvc

$stage = Join-Path $PWD 'out\sakura-indent-rainbow'
New-Item -ItemType Directory -Force (Join-Path $stage 'module') | Out-Null
Copy-Item rust/extensions/sakura_indent_rainbow/senp.json, `
  rust/extensions/sakura_indent_rainbow/README.md, `
  rust/extensions/sakura_indent_rainbow/LICENSE $stage

$guest = Resolve-Path rust/target/wasm32-unknown-unknown/release/sakura_indent_rainbow.wasm
$component = Join-Path $stage 'module\extension.wasm'
& $tool componentize $guest $component

$package = Join-Path $PWD 'out\sakura-indent-rainbow.senp'
& $tool pack-builtin $stage $package (Join-Path $PWD 'out\sakura-indent-rainbow.sha256')
```

`componentize` は core Wasm module を読み、WIT component model の validation
を有効にして Component を出力します。package validator は manifest の
`runtime.module` が `module/extension.wasm` と完全一致することだけを要求
するため、生成物をその場所に置いてください。host 側でも Component を
読み込む前に 32 MiB 上限と module SHA-256 を検査します。

## 検証・失敗時の見方

CLI は成功時に結果を stdout へ出し、失敗時は `ErrorCode: detail` を stderr
へ出して終了コード 1 を返します。引数不足・余分な引数は usage を stderr
へ出して終了コード 2 です。

よくある失敗と確認箇所は次のとおりです。

| error code | 確認すること |
| --- | --- |
| `MissingRequiredEntry` | `senp.json`、`README.md`、`LICENSE`、`integrity/SHA256SUMS`、runtime module、または manifest が参照する asset が存在するか。 |
| `InvalidManifest` / `UnsupportedSchema` / `AbiMismatch` | camelCase field、重複/unknown member、schema `1`、identifier、runtime ABI、activation/capability の条件を確認する。 |
| `InvalidChecksumDocument` | LF 終端、CR の不存在、space 2 個、lowercase digest、strictly sorted path、全 payload coverage を確認する。通常は `pack` で再生成する。 |
| `ChecksumMismatch` | package bytes、entry bytes、built-in archive pin、または既存 content が変わっていないか。 |
| `UnsupportedCompression` / `CompressionRatioLimit` | ZIP を `Stored` または `Deflated` にする。過剰な圧縮率や破損した compressed size を避ける。 |
| `UnsafePath` / `UnsupportedEntry` / `DuplicateEntry` | `/` path、`.`/`..`、backslash、case-fold 重複、directory/symlink、許可リスト外 entry を確認する。 |
| `UnsignedPackage` / `UntrustedPublisher` / `InvalidSignature` | policy と package 種別を揃える。publisher package は対応公開鍵、built-in は archive hash、developer は unsigned を使う。 |
| `EntryTooLarge` / `ExpandedSizeLimit` / `TooManyEntries` | document 4 MiB、entry 32 MiB、全体 64 MiB、entry 256 件、pack payload 254 件以内か。 |
| `ReparsePoint` | source、install root、content、profile、state 配下の symlink/reparse point を除去する。 |
| `PublicationFailed` | install root の staging、profile の atomic publication、既存 content の競合を確認する。 |

最初に `pack` が作った archive を同じ key/pin で `verify` または
`inspect-builtin` し、その後 `install-*`、`list` の順に切り分けると、
package 内容の失敗と profile state の失敗を分離できます。`list` は installed
content を再走査するため、インストール後に payload や checksum を直接編集
しないでください。

## 実装との対応

この仕様の主な実装入口は次のとおりです。

- format constants、manifest 型、validator、checksum、署名、pack、install：
  [`rust/sakura_senp/src/lib.rs`](../rust/sakura_senp/src/lib.rs)
- CLI の全 command と引数形式：
  [`rust/sakura_senp_tool/src/main.rs`](../rust/sakura_senp_tool/src/main.rs)
- runtime の WIT 境界：[`rust/wit/senp-extension.wit`](../rust/wit/senp-extension.wit)
- runtime host の component、hash、fuel、memory、frame 制限：
  [`rust/sakura_senp_host/src/main.rs`](../rust/sakura_senp_host/src/main.rs)
- built-in package の staging/pack 手順：
  [`src/main/msbuild/sakura-senp.targets`](../src/main/msbuild/sakura-senp.targets)

実装とこの文書が食い違う場合は、まず `lib.rs` の validator と CLI を確認し、
仕様変更として両方を同時に更新してください。

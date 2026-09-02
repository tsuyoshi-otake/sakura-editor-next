<!-- TOC -->

- [インストーラ作成に必要なもの](#インストーラ作成に必要なもの)
- [インストーラ作成手順](#インストーラ作成手順)
  - [すべて一括でビルドする場合](#すべて一括でビルドする場合)
    - [具体例 (x64 の全構成をビルドする場合)](#具体例-x64-の全構成をビルドする場合)
  - [個別にビルドする場合](#個別にビルドする場合)
    - [具体例 (x64 の Release)](#具体例-x64-の-release)
- [インストーラの設定ファイル](#インストーラの設定ファイル)
- [インストーラのビルドに必要なファイル](#インストーラのビルドに必要なファイル)
- [インストーラのビルド](#インストーラのビルド)
  - [x64](#x64)
- [コード署名](#コード署名)
- [インストーラのテスト](#インストーラのテスト)
  - [インストーラーのデバッグ](#インストーラーのデバッグ)
  - [英語版インストーラーの動作確認について](#英語版インストーラーの動作確認について)

<!-- /TOC -->

## インストーラ作成に必要なもの

[こちら](../tools/build.md#必要なツール) を参照

配布対象は Windows 11 build 22000 以降の x64 環境のみです。`sakura-x64.iss` は AMD64 のみを許可し、`MinVersion=10.0.22000` を設定しています。インストール方式は管理者権限を要求しない現在のユーザー専用です。全ユーザー向けインストールへの切り替えは提供しません。Release インストーラーには AVX を最小要件とする単一の本体バイナリを同梱し、セットアップ開始時に Windows が報告する AVX 対応を確認します。AVX2 および AVX-512F/BW の選択は、インストール後に本体がプロセス起動時の CPUID と XGETBV で一度だけ行います。Intel / AMD のベンダー名では分岐しません。AVX を利用できない環境ではインストールを開始しません。

## インストーラ作成手順

### すべて一括でビルドする場合

以下のコマンドを実行する

```
build-all.bat <Platform> <Configuration>
```


| 引数 | 名前 | 値 |
----|----|----
|第一引数 | platform      | "x64" |
|第二引数 | configuration | "Debug" または "Release" |

#### 具体例 (x64 の全構成をビルドする場合)

```
build-all.bat x64   Release
build-all.bat x64   Debug
```


### 個別にビルドする場合

以下のコマンドを実行する

```
build-sln.bat <Platform> <Configuration>
build-chm.bat
build-installer.bat <Platform> <Configuration>
```


#### 具体例 (x64 の Release)

```
build-sln.bat x64 Release
build-chm.bat
build-installer.bat x64 Release
```

## インストーラの設定ファイル

Inno Setup の設定ファイルは拡張子が iss のファイルです。Sakura Editor NEXT の
ダークテーマ、背景色、および高 DPI 対応画像には Inno Setup 6.7.0 以降が必要です。

| iss ファイル | 意味 |
----|----
|[sakura-common.iss](sakura-common.iss) |共通ファイル。以下のファイルからインクルードされます。 |
|[sakura-x64.iss](sakura-x64.iss)       |x64   用の iss ファイル |

`sakura-common.iss` は純黒を使わない `#2B2B2B` のチャコール背景を指定し、
セットアップ EXE に埋め込まれた Sakura Editor NEXT アイコンを Welcome / 完了ページの
左側へ縦横比を維持して表示します。Windows の高コントラストテーマでは Inno Setup が
カスタムスタイルを自動的に無効化します。セットアップ言語は Windows の UI 言語から
自動検出し、言語選択ダイアログは表示しません。

追加オプションでは、スタートメニューとデスクトップのショートカットを既定で選択します。
Windows の「アプリで開く」登録、右クリックメニュー、`送る` メニュー、サインイン時の
バックグラウンド起動は既定で選択しません。

## インストーラのビルドに必要なファイル

事前に以下にファイルを配置する。(build-installer.bat を実行すると以下のファイルの配置～インストーラのビルドまで行う。)

- installer/
    - sakura/
        - sakura.exe
        - sakura-senp-tool.exe（SENP パッケージ管理ツール）
        - sakura-senp-host.exe（SENP 拡張機能ランタイムホスト）
        - sakura_lang_en_US.dll
        - bregonig.dll （製品ビルドがステージした DLL。`bron420.zip` からは展開しない）
        - sakura.exe.manifest.x
        - sakura.exe.manifest.v
        - sakura.chm
        - macro.chm
        - plugin.chm
        - sakura.exe.ini
        - license/
            - bregonig/
                - bsd_license.txt
                - perl_license.txt
                - perl_license_jp.txt
            - windows-terminal/
                - LICENSE
                - UPSTREAM.md
                - IMPORTED_FILES.md
            - codicons/
                - CODICONS-ATTRIBUTION.md
            - seti/
                - SETI-ATTRIBUTION.md
                - SETI-LICENSE
            - fmt/
                - LICENSE
            - ms-gsl/
                - LICENSE
            - wil/
                - LICENSE
        - keyword/
            - *.col
            - *.dic
            - *.hkn
            - *.khp
            - *.kwd
            - *.otl
            - *.rkw
            - *.rl
            - *.rule
            - *.txt

Release と Debug の `sakura/sakura.exe`、`sakura/sakura-senp-tool.exe`、および
`sakura/sakura-senp-host.exe` は、それぞれの構成の `x64/<Configuration>/` から
ステージングされます。ショートカット、
ファイル関連付け、および既存の起動方法は従来どおり `sakura.exe` を参照します。
`bregonig.dll` と `migemo.dll` も製品ビルドが所有するステージ済み payload です。
同じファイルバージョンでも内容が更新されることがあるため、再インストール時は
Inno Setup の既存ファイルを残さず、ステージ済み DLL へ置き換えます。

## インストーラのビルド

以下のコマンドでインストーラをビルドします。(build-installer.bat に含まれます。)

### x64

"C:\Program Files (x86)\Inno Setup 6\ISCC.exe" installer\sakura-x64.iss

→ installer\Output-x64\ にインストーラが生成されます。

## コード署名

ローカルの `build-installer.bat` および Inno Setup のコンパイル処理は、生成した
セットアップ EXE にコード署名を付与しません。署名状態は Windows SDK の SignTool で
確認できます。

```pwsh
signtool verify /pa /v installer\Output-x64\sakura_install<version>-x64.exe
```

ローカル成果物では `SignTool Error: No signature found.` になるのが現在の仕様です。
配布用成果物は、公開前に信頼できる証明書とタイムスタンプを使って別途署名する必要が
あります。

## インストーラのテスト

### インストーラーのデバッグ

インストーラーのデバッグについては、Inno Script Studioを利用してステップ実行できます。ただしデバッカーから実行した場合のインストーラーの挙動が不安定の為実際の動作確認については、生成されたインストーラーそのもののexeを実行し動作確認したほうが確実です。
ですのでInno Script Studioを使うのは主にPascalのスクリプトの挙動を確認する場合に用います。

Inno Script Studioは、Inno Setupのサイトより、innosetup-qsp-5.6.1-unicode.exeのインストーラーにてインストールするか、Inno Script Studioのサイトより直接ダウンロードしてインストールしてください

https://www.kymoto.org/products/inno-script-studio/downloads

(有志で[日本語化のファイル](https://www42.atwiki.jp/jfactory/pages/75.html)も公開されています)

ただし現在、issファイルをインクルードしているとうまくブレイクポイントが有効にならないようなので、sakura-x64.issの先頭２行をsakura-common.issの最初に挿入してからsakura-common.issにブレイクポイントを設定して実行してください。

### 英語版インストーラーの動作確認について

英語版のインストーラーの挙動を確認する場合には、実行環境を英語モードにする必要があります。
お使いのPCの言語設定を英語に変更するか([こちら](https://www.google.co.jp/search?q=%E8%A8%80%E8%AA%9E+%E6%97%A5%E6%9C%AC%E8%AA%9E+%E8%8B%B1%E8%AA%9E+Windows&oq=%E8%A8%80%E8%AA%9E%E3%80%80%E6%97%A5%E6%9C%AC%E8%AA%9E%E3%80%80%E8%8B%B1%E8%AA%9E%E3%80%80Windows&aqs=chrome..69i57j0l2.5435j0j4&sourceid=chrome&ie=UTF-8)参考)、お使いのPCのリソースに余裕があれば、[VirtualBOX](https://www.virtualbox.org/)等の仮想化ソフトウエアにて、[開発用Windowsマシン](https://developer.microsoft.com/en-us/microsoft-edge/tools/vms/)を利用する方法もあります。
（覚書：この仮想PCのユーザーのパスワードは「Passw0rd!」です)

# ビルドに使用するバッチファイル

<!-- TOC -->

- [ビルドに使用するバッチファイル](#ビルドに使用するバッチファイル)
  - [使用するバッチファイルの一覧](#使用するバッチファイルの一覧)
  - [呼び出し構造](#呼び出し構造)
  - [ビルドに使用するバッチファイルの引数](#ビルドに使用するバッチファイルの引数)
  - [バッチファイルの仕組み](#バッチファイルの仕組み)
    - [githash.bat の構造](#githashbat-の構造)
      - [処理の流れ](#処理の流れ)
    - [zipArtifacts.bat の構造](#zipartifactsbat-の構造)
      - [処理の流れ](#処理の流れ-1)

<!-- /TOC -->

## 使用するバッチファイルの一覧

`sakura-build.bat` が canonical build CLI です。`build-dev.bat`、`build-sln.bat`、
`build-all.bat`、`build-gnu.bat` は argv と exit code だけを中継する互換shimです。
引数検証、tool discovery、実行計画、並列予算、staging は
`tools/build/sakura_build.py` が所有します。

| ファイル名 | 説明 |
|----|----|
|[sakura-build.bat](../sakura-build.bat)| canonical CLIへのWindows入口 |
|[tools\githash.bat](./githash.bat) | Git や CI の環境変数から githash.h を生成する |
|[tools\find-tools.bat](./find-tools.md) | ビルド関連ツールのパスを探す |
|[build-all.bat](../build-all.bat)| 配布に必要な本体、ヘルプ、インストーラ、ZIP をビルドする |
|[build-dev.bat](../build-dev.bat) | `sakura.vcxproj` だけを高速にビルドする |
|[build-sln.bat](../build-sln.bat) | `sakura.sln` の本体と単体テストをビルドする |
|[build-gnu.bat](../build-gnu.bat) | Makefile をビルドする |
|[build-chm.bat](../build-chm.bat) | compiled HTML ファイルをビルドする |
|[build-installer.bat](../build-installer.bat) | インストーラをビルドする |
|[zipArtifacts.bat](../zipArtifacts.bat) | 成果物を zip ファイルにまとめる |

## 呼び出し構造

- [build-dev.bat](../build-dev.bat) / [build-sln.bat](../build-sln.bat) / [build-all.bat](../build-all.bat) / [build-gnu.bat](../build-gnu.bat)
    - `py -3 tools/build/sakura_build.py compat ...`（薄い互換shim）
- [sakura-build.bat](../sakura-build.bat)
    - `py -3 tools/build/sakura_build.py`
        - strict manifest / ContextProjection / BuildIntent を検証
        - MSBuildまたはCMakeのnative schedulerへ実行を委譲
- canonical CLIによる `build dev`
    - MSBuild.exe sakura_core\sakura.vcxproj (`/nr:false`)
- canonical CLIによる `build solution`
    - MSBuild.exe sakura.sln (`/nr:false`)
        - cmake.exe Gitリポジトリ情報を version.h に書き出す。
            - git.exe
        - python.exe [src/main/py/header_make.py](../src/main/py/header_make.py) : Funccode_define.h, Funccode_enum.h を生成する
        - cmake.exe 外部ソースからツールをビルドする、または、配布zipから展開する
            - cmake.exe
            - 7z.exe
- [build-all.bat](../build-all.bat)
    - パッケージング環境では `SAKURA_GENERATE_ASSEMBLY_LISTINGS=1` を保持する
    - 最初に一覧を明示的に無効にして solution（本体と tests1）をビルドし、続いて `sakura_core\sakura.vcxproj` だけを一覧有効・`/m:1` で再ビルドする
    - [build-chm.bat](../build-chm.bat)
        - cmake.exe
            - ChmSourceConverter.exe : ヘルプファイルの文字コードを UTF-8 から Shift_JIS に変換する
            - pwsh.exe
                - [help\CompileChm.ps1](../help/CompileChm.ps1)
                    - hhc.exe (Visual Studio に同梱) : compiled HTML をビルドするコンパイラ。かなり古いツールであり、日本語 HTML をビルドするためには Windows のシステムロケールを日本語に変更する必要がある。
    - [build-installer.bat](../build-installer.bat)
        - ISCC.exe : [InnoSetup](https://www.jrsoftware.org/isinfo.php) でインストーラをビルドする
        - innounp.exe : リポジトリ同梱の `tools/innounp/innounp.exe` で生成インストーラから payload を展開し、ステージ済み `bregonig.dll` / `migemo.dll` の SHA-256 と照合する。7-Zip は Inno Setup 6 の exe を開けない。`bron420.zip` から DLL を展開しない
    - [zipArtifacts.bat](../zipArtifacts.bat)
        - [tools\githash.bat](./githash.bat)
            - git.exe
        - [tools\zip\zip.bat](./zip/zip.bat) : 成果物を ZIP ファイルにまとめる
            - 7z.exe または [tools\zip\zip.ps1](./zip/zip.ps1)
- [build-gnu.bat](../build-gnu.bat)
    - cmake -S . -B build/MinGW -DCMAKE_BUILD_TYPE=Debug -DBUILD_PLATFORM=MinGW
    - cmake --build build/MinGW --config Debug --target sakura
    - cmake --build build/MinGW --config Debug --target sakura_lang_en_US
    - cmake --build build/MinGW --config Debug --target tests1
    - ctest --test-dir build/MinGW --build-config Debug --output-on-failure

canonical CLIはMSBuildのノード再利用を無効にするため、正常終了後に再利用待ちのMSBuildプロセスを残しません。

## ビルドに使用するバッチファイルの引数

| バッチファイル | 第一引数 | 第二引数 |
|----|----|----|
|build-all.bat       | platform ("x64") | configuration ("Debug" または "Release")  |
|build-dev.bat       | platform ("x64") | configuration ("Debug" または "Release")  |
|build-sln.bat       | platform ("x64") | configuration ("Debug" または "Release")  |
|build-gnu.bat       | platform ("MinGW") | configuration ("Debug" または "Release")  |
|build-chm.bat       | なし | なし |
|build-installer.bat | platform ("x64") | configuration ("Debug" または "Release")  |
|zipArtifacts.bat    | platform ("x64") | configuration ("Debug" または "Release")  |

## バッチファイルの仕組み

### githash.bat の構造

#### 処理の流れ

- Git や CI の環境変数を元に githash.h を生成する
    - 設定される環境変数については [こちら](build-envvars.md) を参照してください。

### zipArtifacts.bat の構造

#### 処理の流れ

* if 文の条件判定を元に、成果物のファイル名、フォルダー名を構築して環境変数に設定する
    - 設定される環境変数については [こちら](build-envvars.md#zipartifactsbat-で設定する環境変数) を参照してください。
* 作業用フォルダーに必要なファイルをコピーする
* [tools\zip\zip.bat](./zip/zip.bat) を使用して作業用フォルダーの中身を zip ファイルにまとめる
    - [7-Zip](https://7-zip.opensource.jp/) が利用できる場合は 7z.exe を、利用できない場合は PowerShell を利用してファイルを作成します。

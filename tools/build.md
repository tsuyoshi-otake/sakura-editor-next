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
    - [デバッグ方法](#デバッグ方法)
    - [アセンブリ一覧の生成](#アセンブリ一覧の生成)
    - [githash.h の更新をスキップ](#githashh-の更新をスキップ)
    - [PowerShellによるZIPファイル処理の強制](#powershellによるzipファイル処理の強制)
    - [CIビルドのスキップ](#ciビルドのスキップ)
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

| 用途 | コマンド | ビルドする範囲 |
|--|--|--|
| 普段の編集・動作確認 | `build-dev.bat` | `sakura_core\sakura.vcxproj` のみ |
| 本体と単体テストの確認 | `build-sln.bat` | `sakura.sln`（本体と `tests1`） |
| 配布成果物の作成 | `build-all.bat` | 本体、単体テスト、HTML ヘルプ、インストーラ、ZIP |

通常の編集では `build-dev.bat` を使用すると、`tests1` プロジェクトの評価・コンパイル・リンクを省略できます。単体テストのビルドや配布準備には `build-sln.bat` または `build-all.bat` を使用してください。

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

### CIビルドのスキップ

ドキュメント修正など、ビルドが不要な変更の場合、コミットメッセージに `[ci skip]` または `[skip ci]` を含めることでCIビルドをスキップできます。

**注意:** PRマージ時は実行されます。

**参考:**
- https://qiita.com/vmmhypervisor/items/f10c77a375c2a663b300
- https://github.blog/changelog/2021-02-08-github-actions-skip-pull-request-and-push-workflows-with-skip-ci/

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

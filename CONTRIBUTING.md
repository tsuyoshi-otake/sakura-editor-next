# Sakura Editor NEXT の開発にご協力ください
Sakura Editor NEXT はオープンソースのテキストエディタで、多くの方の協力で支えられています。

まずは簡単なこと、小さなことからで構いません。ぜひ開発にご協力ください。

* Sakura Editor NEXT の感想や要望を投稿する
* ヘルプやテキストの誤字脱字を直す
* 不具合を直したり、機能を追加するプログラムを書く

# ビルド

## 必要なツール

C++20 に対応した Visual Studio 2019 以降が必要です。`NUM_VSVERSION=16` で Visual Studio 2019、`NUM_VSVERSION=17` で Visual Studio 2022 を選べます。必要なコンポーネントは [`.vsconfig`](./.vsconfig) で導入できます。

次の補助ツールも必須です。

| ツール | 実行ファイル | 備考 |
| --- | --- | --- |
| Python 3.10 以上 | `py` / `python` | ビルドの正規ドライバー `tools/build/sakura_build.py` が使います。標準ライブラリだけで動きます |
| Rust toolchain | `cargo.exe` / `rustc.exe` | `rust/native/rust-toolchain.toml` と `rust/senp/rust-toolchain.toml` が 1.96.0 に固定しています |
| CMake | `cmake.exe` | Visual Studio 同梱のもので構いません |
| PowerShell Core | `pwsh.exe` | |
| 7-Zip | `7z.exe` | 外部依存ファイルの解凍に使います |
| HTML Help Workshop | `hhc.exe` | Visual Studio 同梱のものです |

次のツールは、該当する場合だけ必要です。

- インストーラをビルドする場合: [Inno Setup](https://jrsoftware.org/isdl.php) 6.7.0 以降（`ISCC.exe`）
- 日本語環境以外で HTML ヘルプをビルドする場合: Locale Emulator（`LEProc.exe`）と Auto HotKey（`AutoHotKey.exe`）

## ビルド手順

リポジトリのルートで実行します。

```cmd
git clone --recursive https://github.com/tsuyoshi-otake/sakura-editor-next.git
cd sakura-editor-next
build-dev.bat x64 Debug
```

| 目的 | コマンド | ビルドする範囲 |
| --- | --- | --- |
| 普段の編集・動作確認 | `build-dev.bat x64 Debug` | アプリ本体（`sakura_core\sakura.vcxproj`）のみ。`tests1` は評価もビルドもしません |
| 本体と単体テストの確認 | `build-sln.bat x64 Debug` | `sakura.sln`（本体と `tests1`） |
| 配布成果物の作成 | `build-all.bat x64 Release` | 本体、テスト、HTML ヘルプ、インストーラ、ZIP |
| 実験的な MinGW ビルド | `build-gnu.bat MinGW Debug` | CMake／MinGW。生成されるバイナリは正しく動作しません |

- Visual Studio で [`sakura.sln`](./sakura.sln) を開いてビルドすることもできます。
- `build-dev.bat` は本体のビルドだけを確認します。`tests1.exe` を実行する前や、テストがビルドできると確認するときは `build-sln.bat` を使ってください。
- x64 Release は、AVX を最小要件とする単一の `x64\Release\sakura.exe` を生成します。AVX2 と AVX-512F/BW の実装はソース単位で分離してコンパイルされ、起動時に CPUID と XGETBV の結果から一度だけ選ばれます。ISA ごとの実行ファイルは作りません。
- 新しい C++ ソースを MSBuild のプロジェクトへ加えるときは、`.vcxproj` と対応する `.vcxproj.filters` の両方を更新してください。CMake ビルドはソースを別に探索するため、必要に応じて両方のビルドで確認します。

ビルドの詳細は次のドキュメントを参照してください。ここには重複して書きません。

- [ビルド方法](./tools/build.md) — 手順、package restore、MinGW、デバッグ、アセンブリ一覧の生成
- [ビルドに使用されるバッチファイル](./tools/build-batchfiles.md) — 各スクリプトの責務と呼び出し関係
- [ビルドで使用する環境変数](./tools/build-envvars.md) — 公開している環境変数と既定値
- [ツールの検出](./tools/find-tools.md) — Visual Studio の選択とツールの探索

# テストの実行

1. `build-sln.bat x64 Debug` で本体と `tests1` をビルドします。
2. `x64\Debug\tests1.exe` を実行します。

```cmd
x64\Debug\tests1.exe
```

`tests1.exe` には、実際のエディタ画面やダイアログを起動する UI・連携テストも含まれます。特に `WinMain/WinMainTest.*` は既定設定のテストプロファイルを使うため、普段の設定とは異なる外観でエディタが起動します。

UI を起動しないローカル確認では、次のフィルターで UI・外部連携のテストを除外できます。

```cmd
x64\Debug\tests1.exe --gtest_filter=-MacroMgrTest.*:CPpaTest.*:SelectFileTest.*:FileDialog/FileDialogTest.*:CDlgProfileMgrTest.*:TrayWndTest.*:EditWndTest.*:WinMainFuncTest.*:WinMain/WinMainTest.*
```

- このフィルターは最終確認の代わりになりません。除外した UI・連携テストは、対話できる Windows セッションで別途実行してください。標準の CI も、非対話セッションのため同じフィルターを使います。
- フィルターを変える前に `--gtest_list_tests` でスイート名を確認してください。
- 実行後は、`tests1.exe` とテストが起動した `sakura.exe` が終了していることを確認してください。
- テストの追加や名前の変更をしたときは、テスト台帳の更新が必要です。手順は [src/test/CLAUDE.md](./src/test/CLAUDE.md) の Running Tests を参照してください。

# 起動パフォーマンスの計測

[`tools/measure-startup-performance.ps1`](./tools/measure-startup-performance.ps1) は、同じ `sakura.exe` と同じ Markdown 入力について、プロセス起動から画面表示・文書読込・全文レイアウトと描画同期までの時点を繰り返し計測します。変更前後やビルド構成間を**比較**するための開発者用ベンチマークで、特定のミリ秒値や他の環境での性能を保証しません。

固定入力は [`tools/startup-benchmark-sample.md`](./tools/startup-benchmark-sample.md) です。

1. 計測する構成をビルドします（例: `build-sln.bat x64 Release`）。
2. 同じ実行ファイルを使う Sakura プロセスが残っていないことを確認します。
3. 次のコマンドを実行します。

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\measure-startup-performance.ps1 `
  -SakuraExe .\x64\Release\sakura.exe `
  -SampleMarkdown .\tools\startup-benchmark-sample.md `
  -Iterations 5
```

- 作成済みプロファイルとの比較を加えるには `-CompareExistingProfile` を付けます。
- 表示結果の画像も残すには `-CaptureScreenshot` を付けます。このとき対象ウィンドウを他のウィンドウで覆わないでください。
- 計測用の補助だけを確認するセルフテストは `-SelfTest` で実行できます。GUI は起動しません。
- 入力は 31 物理行以上にしてください。固定サンプルは満たしています。
- キャッシュ状態や電源モード、バックグラウンド負荷が異なる試行を混在させないでください。
- 各指標の定義、結果 JSON の読み方、C++／Rust のペア計測は [起動パフォーマンスの比較計測](./tools/startup-performance.md) を参照してください。

## 参考値

2026-07-30 に Windows 11 Pro build 26200、AMD Ryzen 7 9700X、61.6 GiB RAM、x64 Release 版を使い、1,249,037 bytes の Markdown ファイルを各条件 5 回起動した結果は次のとおりです。

| 条件 | ウィンドウ表示の中央値 | 文書準備完了の中央値 |
| --- | ---: | ---: |
| 新規プロファイル | 448.883 ms | 295.653 ms |
| 作成済みプロファイル | 420.607 ms | 274.413 ms |

この環境では全試行で AVX-512F/BW が選択されました。CPU・OS 判定と関数ディスパッチの初期化時間は、プロセス当たり中央値 0.0025 ms、範囲 0.0019–0.0098 ms でした。

上記は同一マシンのウォームファイルキャッシュ条件で得た開発時の参考値であり、性能保証ではありません。「文書準備完了」は、全文レイアウトがスクロール範囲へ反映されたことを外部から確認した時点です。

# Pull Requestのガイドライン
Pull Request は `main` を base branch にしてください。

Pull Request のコミットに `[ci skip]` または `[skip ci]` を含めないでください。

Pull Request を出す前に、次を確認してください。

1. リポジトリのルートで `py -3 tools/build/sakura_build.py --format json lint checkout-invariance` を実行し、終了コードが 0 であることを確認します。
2. 変更した `.cpp`／`.h` が ASCII のみ、または BOM 付き UTF-8 であることを確認します。BOM なしの UTF-8 は CI の `src/main/ps1/check-encoding.ps1` が拒否します。`.rc`／`.rc2` は BOM 付き UTF-16 にしてください。
3. 動作に関わる変更は、前の節のとおりビルドとテストを実行します。

# ライセンス
Sakura Editor NEXT は誰もが自由にプログラムを使用できるように [zlib License](LICENSE) で提供されています。

みなさんからいただいたPull Requestやデータの著作権はそれぞれの作者の方が保有しますが、Sakura Editor NEXT プロジェクトが zlib License でそれを公開することに同意したものとして扱います。

zlib License以外のライセンス(GPLやApacheライセンスなど)が適用された著作物をPull Request等で提供する場合や、不安なことがありましたらGitHub Issuesや掲示板でご一報ください。

[掲示板一覧](https://sakura-editor.github.io/#bbs)

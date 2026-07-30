<h1 align="center">
  <img src=".github/assets/sakura-editor-next-hero.png" alt="Sakura Editor NEXT — VS Code級の機能を、ネイティブの速さで。">
</h1>

<p align="center">
  サクラエディタを基盤に、VS Code スタイルのワークベンチを Windows ネイティブで実現するテキストエディタ。
</p>

<p align="center">
  <a href="https://github.com/tsuyoshi-otake/sakura-editor-next/actions/workflows/build-sakura.yml"><img src="https://github.com/tsuyoshi-otake/sakura-editor-next/actions/workflows/build-sakura.yml/badge.svg" alt="Build"></a>
  <img src="https://img.shields.io/badge/Windows%2011-x64-0078D4.svg" alt="Platform: Windows 11 x64">
  <a href="./LICENSE"><img src="https://img.shields.io/badge/License-Zlib-lightgrey.svg" alt="License: Zlib"></a>
</p>

<p align="center">
  <a href="#ダウンロード">ダウンロード</a> ・
  <a href="#主な特徴">主な特徴</a> ・
  <a href="#基本操作">基本操作</a> ・
  <a href="#ビルド">ビルド</a> ・
  <a href="#プロジェクト情報">プロジェクト情報</a>
</p>

## ダウンロード

> [!IMPORTANT]
> Sakura Editor NEXT は **Windows 11 build 22000 以降の x64 環境専用**です。x86 版および Windows 10 はサポートしません。

| 種類 | 入手先 | 内容 |
| --- | --- | --- |
| 開発版 | [GitHub Actions](https://github.com/tsuyoshi-otake/sakura-editor-next/actions/workflows/build-sakura.yml?query=branch%3Amaster) | `master` の最新ビルド |
| リリース版 | [Releases](https://github.com/tsuyoshi-otake/sakura-editor-next/releases) | 公開済みの配布パッケージ |

## 主な特徴

- **ネイティブワークベンチ** — 左の Explorer、右の Outline、下の統合 Terminal を 1 つのウィンドウにまとめています。
- **必要なときだけ起動する Terminal** — 新規プロファイルでは Explorer と Outline を表示し、Terminal は初めて使うまで起動しません。
- **Open VSX 拡張機能管理** — Open VSX の検索、VSIX のダウンロード、インストール、削除に対応しています。
- **サクラエディタ由来の編集基盤** — 実績ある編集機能を引き継ぎながら、Windows 11 向けの x64 ネイティブアプリとして発展させています。

> [!NOTE]
> Open VSX 拡張機能は管理機能までの対応です。拡張機能を実行するランタイムホストは、現在まだ搭載していません。

## 基本操作

| 操作 | ショートカット |
| --- | --- |
| 左パネルの表示／非表示 | <kbd>Ctrl</kbd>+<kbd>B</kbd> |
| 下パネルの表示／非表示 | <kbd>Ctrl</kbd>+<kbd>J</kbd> |
| Terminal の表示／フォーカス | <kbd>Ctrl</kbd>+<kbd>@</kbd>（英語配列では <kbd>Ctrl</kbd>+<kbd>&#96;</kbd>） |

統合 Terminal は、ユーザーが指定したプロファイルを最優先します。指定がない場合はインストール済みの安定版 PowerShell から最も新しいものを選び、PowerShell 7 がない環境では Windows PowerShell 5.1 にフォールバックします。ネットワークへの問い合わせや自動インストールは行いません。

## ビルド

C++20 に対応した Visual Studio 2019 以降が必要です。Visual Studio 2022 では [`.vsconfig`](./.vsconfig) を使って必要なコンポーネントを導入できます。

```cmd
git clone --recursive https://github.com/tsuyoshi-otake/sakura-editor-next.git
cd sakura-editor-next
build-dev.bat x64 Debug
```

| 目的 | コマンド | 対象 |
| --- | --- | --- |
| 普段の編集・動作確認 | `build-dev.bat x64 Debug` | アプリ本体 |
| 本体と単体テストの確認 | `build-sln.bat x64 Release` | アプリ本体と `tests1` |
| 配布成果物の作成 | `build-all.bat x64 Release` | 本体、テスト、ヘルプ、インストーラ、ZIP |

Visual Studio で [`sakura.sln`](./sakura.sln) を開くこともできます。環境構築、MinGW の実験的サポート、テスト手順などは [ビルド方法](./tools/build.md) を参照してください。

## プロジェクト情報

| 項目 | リンク |
| --- | --- |
| コントリビュート | [CONTRIBUTING.md](./CONTRIBUTING.md) |
| セキュリティ | [GitHub Security Advisories](https://github.com/tsuyoshi-otake/sakura-editor-next/security/advisories) |
| 最新の変更 | [コミット履歴](https://github.com/tsuyoshi-otake/sakura-editor-next/commits/master/) ・ [Releases](https://github.com/tsuyoshi-otake/sakura-editor-next/releases) |
| 引き継いだ変更履歴 | [CHANGELOG.md](./CHANGELOG.md) |
| ライセンス | [zlib License](./LICENSE) |

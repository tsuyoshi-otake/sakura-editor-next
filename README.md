# Sakura Editor NEXT

<p align="center">
  <img src="src/main/resources/images/sakura_editor_next.png" alt="Sakura Editor NEXT" width="180">
</p>

**VS Code級の機能を、ネイティブの速さで。**

A fast, native Windows editor with a VS Code-style workbench.
[![License: Zlib](https://img.shields.io/badge/License-Zlib-lightgrey.svg)](https://opensource.org/licenses/Zlib)
[![build sakura](https://github.com/tsuyoshi-otake/sakura-editor-next/actions/workflows/build-sakura.yml/badge.svg)](https://github.com/tsuyoshi-otake/sakura-editor-next/actions/workflows/build-sakura.yml)
[![Github Releases All](https://img.shields.io/github/downloads/tsuyoshi-otake/sakura-editor-next/total.svg)](https://github.com/tsuyoshi-otake/sakura-editor-next/releases "All Releases")
[![Star History](https://img.shields.io/badge/star-history-yellow.svg)](https://star-history.com/#tsuyoshi-otake/sakura-editor-next)

<!-- 以下は Markdownの参照形式によるリンク の定義です。 -->
<!-- 参照 https://hail2u.net/blog/coding/markdown-reference-style-links.html -->

[Visual Studio 以前のバージョン]: https://visualstudio.microsoft.com/ja/vs/older-downloads/ "Visual Studio 以前のバージョン"
[Visual Studio 最新版]: https://visualstudio.microsoft.com/ja/downloads/ "Visual Studio 最新版"
[ライセンスの OSI のページ]: https://opensource.org/license/zlib
[Visual Studio Community ライセンス]: https://visualstudio.microsoft.com/ja/license-terms/vs2022-ga-community/
[How to extract currently installed Visual Studio component IDs?]: https://stackoverflow.com/questions/52946333/how-to-extract-currently-installed-visual-studio-component-ids
[Configure Visual Studio across your organization with .vsconfig]: https://devblogs.microsoft.com/setup/configure-visual-studio-across-your-organization-with-vsconfig/
[インストール構成をインポートまたはエクスポートする]: https://docs.microsoft.com/ja-jp/visualstudio/install/import-export-installation-configurations?view=vs-2019
[コマンド ライン パラメーターを使用して Visual Studio をインストールする]: https://docs.microsoft.com/ja-jp/visualstudio/install/use-command-line-parameters-to-install-visual-studio?view=vs-2019
[不足しているコンポーネントを自動的にインストールする]: https://docs.microsoft.com/ja-jp/visualstudio/install/import-export-installation-configurations?view=vs-2019#automatically-install-missing-components

<!-- TOC -->

- [Sakura Editor NEXT](#sakura-editor-next)
  - [脆弱性の報告方法](#脆弱性の報告方法)
  - [ダウンロード](#ダウンロード)
  - [開発情報](#開発情報)
    - [How to build](#how-to-build)
  - [変更履歴](#変更履歴)

<!-- /TOC -->

## 脆弱性の報告方法

https://github.com/tsuyoshi-otake/sakura-editor-next/security/advisories
から報告を行ってください。


## ダウンロード

リリース版は [Sakura Editor NEXT Releases](https://github.com/tsuyoshi-otake/sakura-editor-next/releases) に置いてあります。

開発中の最新版は [GitHub Actionsのビルドページ](https://github.com/tsuyoshi-otake/sakura-editor-next/actions/workflows/build-sakura.yml?query=branch%3Amaster) から取得できます。

### Sakura Editor NEXT の動作環境

Sakura Editor NEXT は **Windows 11 build 22000 以降の x64 環境専用**です。x86 版および Windows 10 はサポートしません。

左の Explorer、右の Outline、下の統合 Terminal を備えます。新規プロファイルでは Explorer と Outline を表示し、Terminal は必要になるまで起動しません。`Ctrl+B` で左パネル、`Ctrl+J` で下パネルを表示／非表示にできます。Terminal は `Ctrl+@`（英語配列では `Ctrl+\``）で表示またはフォーカスします。

統合 Terminal は、ユーザーが指定したプロファイルを優先し、次にインストール済みの安定版 PowerShell のうち最も新しいものを選びます。PowerShell 7 がない場合は Windows PowerShell 5.1 にフォールバックします。ネットワークへの問い合わせや自動インストールは行いません。

## 開発情報

### How to build

`sakura.sln`を開いてビルドできます。  
詳細は [ビルド方法](./tools/build.md) を参照。

## 変更履歴

- Sakura Editor NEXT の最新変更は [コミット履歴](https://github.com/tsuyoshi-otake/sakura-editor-next/commits/master/) と [Releases](https://github.com/tsuyoshi-otake/sakura-editor-next/releases) を参照してください。
- 基盤となったサクラエディタから引き継いだ履歴は [CHANGELOG.md](./CHANGELOG.md) に保存しています。

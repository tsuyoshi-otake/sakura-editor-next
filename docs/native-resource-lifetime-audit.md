# ネイティブリソースの寿命・リーク検査

2026-09-22 / [Issue #308](https://github.com/tsuyoshi-otake/sakura-editor-next/issues/308)

## 結果

リソースの解放漏れを2件、修正前の実行で再現し、修正後に解消を確認した。
加えて、フォント選択を戻す際のDC指定を修正した。フォントの持続的なリークは
今回の反復検査では再現していない。プロセス全体・全機能でリークが存在しないと
保証する結果ではない。

| 所有者・契約 | 修正前の観測 | 修正と修正後の観測 |
|---|---|---|
| `CMenuDrawer`: 生成した各HBITMAPを保持し、再生成・縮小・破棄時に解放する | 4アイコンで`Create`を20回呼ぶと80個になり、破棄後も76個残った。6→2→0への変更でも8個残った | DIBが`ResourceHolder<DeleteObject>`で所有し、再生成前に旧配列を解放。保持中は現在のアイコン数だけ、破棄後は開始時のGDI数に戻る |
| `CSelectLang::SSelLangInfo`: 検証済み言語DLLだけを所有し、失敗時の一時マッピングを解放する | 不一致の言語IDで32回読み込みを拒否した後、DLLへの排他的な読み書きオープンがエラー32で失敗 | 読み込み直後からローカルRAIIで保持し、検証成功時だけメンバーへ移譲。例外後と正常な再読み込み・Unload後の両方で排他的に開ける |
| `CDCFont`: 選択を元に戻す→フォント削除→DC解放の順序を守る | 選択復元用RAIIがDC取得前のnullを値として保持していた。反復でGDI数の増加は観測しなかった | DC取得後に復元先を設定。ウィンドウを指定したスコープ終了後にフォント取得が失敗し、128回のスコープ反復後もGDI数が増えない |

メニューの再生成はAPIを直接反復する回帰検査であり、同じ回数のユーザー操作や
長時間の実アプリ操作を計測した結果ではない。アイコンの描画内容には依存せず、
実際の`CreateDIBSection`で確保したGDIオブジェクト数を観測する。

## 検証結果

| Verify | Expect | 実績 |
|---|---|---|
| x64 Debugのアプリ・tests1をsolutionビルド | コンパイル・リンク成功 | 成功、警告0・エラー0 |
| 下記31テストを同一プロセス内で10回、順序を変えて実行 | すべて成功、反復しても残存資源が増えない | 310/310成功、seed 308から317 |
| CRTチェックポイントで`CMemory`のコピー・移動・拡張・縮小・Resetを128回反復 | 通常ヒープの残存ブロック数・バイト数が増えない | 両方とも差分0（上記反復にも含む） |
| Debugバイナリからテスト台帳をrefresh-runtime後、verify-runtime | 既存IDを保持し、欠落・予期しないselectorがない | 4,161→4,168件、追加7件、欠落0・余剰0 |
| 修正対象3翻訳単位のCppcheck | lifetime errorがない | 完走、リーク・二重解放の指摘0、その他の指摘319件 |

変更したC++ 6ファイルのASCIIまたはUTF-8 BOMを検証し、`git diff --check`も成功した。
テスト後にtests1、sakura、Cppcheck、MSBuild、cl、linkのプロセスを再列挙し、
該当プロセスが残っていないことを確認した。

反復検査のselector:

```text
MenuResourceLifetime.*:NativeResourceLifetime.*:CSelectLang.*:CMemory.*:CFontAutoDeleter.*:TerminalBuiltinGlyphRenderer.*
```

実行引数は`--gtest_repeat=10 --gtest_shuffle --gtest_random_seed=308`。
テストは180秒、solutionビルドは600秒で打ち切るラッパー内で実行した。
この環境で既に有効なtests1のFull Page Heap設定を維持した。
既存の`CMemory`異常サイズ検査が出力する「メモリ確保に失敗しました」は
期待された失敗経路であり、各テストの成功も確認した。

### 静的解析の範囲と未完了項目

Cppcheck 2.21.0をMSBuildプロジェクトの`Debug|x64`設定、
`--enable=warning,performance,portability --force --platform=win64`で実行。
全体の探索は600秒で打ち切りとなり、XML末尾も未完了だった。途中までの完全な
error要素958件を候補調査に使用したが、全体解析の合格とは扱わない。
修正後は`CSelectLang.cpp`、`CMenuDrawer.cpp`、`util/window.cpp`だけを
`--file-filter`で選択し、完全なXMLの出力まで確認した。
こちらの319件はヘッダー由来を含むwarning 315、portability 3、performance 1であり、
すべてを検証・解消したという意味ではない。

全体探索で得た寿命に関係する指摘8件は、次のように判定した。

- 言語DLLの`resourceLeak` 3件: 同一所有者の例外経路。1件の根本原因として修正。
- `ProfileAuthorityStore`の`resourceLeak` 2件: 直前の`HandleCloser`の
  デストラクタで各return経路のHANDLEが閉じられるため、所有権を追跡できていない指摘。
- `CTerminalWnd::DestroyBackBuffer`の`doubleFree` 1件: 1回目の削除が
  失敗した場合だけ、DCを破棄してから再試行する分岐。二重解放ではない。
- Terminal/Markdownのクリップボードの`doubleFree` 2件:
  `SetClipboardData`成功時はOSへ移譲し、失敗時だけ`GlobalFree`する。
  相互排他的な分岐・成功時の所有権移譲が解析に反映されていない指摘。

フォントについては、最初のテストが`GetObjectType`を有効性判定に使っていた。
このWindowsでは削除後も種類を返すことを小さなWin32再現コードで確認したため、
その失敗をリークの証拠とする判断を撤回した。最終テストではスコープ内の
`GetObjectW`成功、スコープ外の失敗、およびGDI数を確認する。
解放済みDCを検査しない。Win32の契約は
[DeleteObject](https://learn.microsoft.com/en-us/windows/win32/api/wingdi/nf-wingdi-deleteobject)と
[GetWindowDC](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getwindowdc)を参照した。

Release・MinGW・全4,168件の実行、UI画面の描画比較、長時間の編集・検索・端末・
拡張機能操作でのヒープ推移は今回の検証には含まない。

## 調査範囲・設計への影響（IRV）

- **A. 実際の範囲**: メニューDIB、言語DLL、DC/フォント、`CMemory`、
  共通`ResourceHolder`の所有権契約を読んだ。実装変更は前記3所有者の4ファイル。
  回帰テストは`test-native-resource-lifetime.cpp`と`test-loadstring.cpp`、
  登録はtests1のproject/filtersとtest-inventory。既存のフォント・端末描画検査も実行した。
- **B. 拡大理由**: 障害箇所が指定されていない品質調査のため、静的解析の候補から
  ProfileAuthorityStoreとTerminal/Markdownの解放分岐まで確認した。
  フォントの誤判定を解決するためWin32 APIの小さな再現検査を追加した。
  これらを理由とした無関係な改修は行っていない。
- **C. 設計影響**: 画像はDIB、DLLはSSelLangInfo、DCとフォントはCDCFontが
  引き続き唯一の所有者。新しい責務や公開APIは追加していない。メニューの画像保持に
  既存の`ResourceHolder`を使用し、言語DLLの検証完了時を所有権移譲点にした。
  Workbenchの配置・コマンド・操作仕様に変更はない。
- **D. 次回のIRV**: `ResourceLifetime`または`ReleasesResourceMapping`の検索で
  寿命検査に直接到達でき、全GUIを立ち上げずに同じ不具合を検証できる。
  次回はGDI数、CRT通常ブロック、DLLの排他的オープンという観測契約と、
  `GetObjectType`では解放を判定できなかった環境上の注意を理解する必要がある。

## ローカル検証証跡

### リリース準備での追加検証

同日、変更ファイルのsemantic ratchetに対応するため、メニューアクセスキー探索の
未読の`MENUITEMINFO`コピーを削除し、必要な位置インデックスだけを保持するよう整理した。
一致なし・単独実行・複数一致の巡回と折り返しを実メニューで検証した。
`SGetTextResult`の成功状態は既存のbool変換からのみ読み取り、ロケール期待値の言語IDはconstとした。
公開操作・所有者・Workbenchの仕様に変更はない。

- Debug solutionの再ビルド成功。ヘッダー変更を含むビルドは既存の警告394件・エラー0、
  最終の差分ビルドは警告0・エラー0。
- 上記selectorに`ApiWrap.*`を加えた37件をseed 308から317で10回実行し、370/370成功。
- Debug台帳は4,169件。既存IDを保持し、欠落・余剰selectorとも0。
- tests1、MSBuild、cl、link、Cppcheck、および今回のPython検査の終了を確認。
- インデント付きconstフィールドを誤検出する解析側の不具合は[Issue #309](https://github.com/tsuyoshi-otake/sakura-editor-next/issues/309)で追跡。

追加証跡は`~/tmp/sakura-release-20260922/`に保存した。
最終ビルドrequestは`7778a90849d74fbb992ea873a877012a`、37件×10回は
`7778a91249d74fbb992ea873a877012a`、台帳照合は`7778a91449d74fbb992ea873a877012a`。
上記のRelease/MinGW未実行という制限はローカル調査時点の記録であり、公開の可否は
リリース元コミットに対するCIと配布物の検査結果で別途判断する。

一時証跡は`~/tmp/sakura-resource-audit-20260922/`に保存した。
再現前後のGoogleTest XML、反復実行の全stdout、静的解析XMLとその集計が含まれる。
TKWの保存出力は次のrequest IDから回収できる（検査対象の再実行は不要）。

| 内容 | Request ID |
|---|---|
| 最終Debug solutionビルド | `308b6ee944984877a8939dd4b0f819fb` |
| 最終31件×10回 | `308c69fe01b3443abbf2749fd778ceab` |
| 修正前GDI・ヒープ検査 | `308ca142d391400298dca363d9c3b19c` |
| 修正前言語DLL検査 | `308c3063e965450882d88121804b80fb` |
| 修正後の限定Cppcheck | `308ad1fbe54a44a0810d830992c617af` |

```powershell
& "$env:LOCALAPPDATA/Programs/TKW/tkw.exe" request <Request-ID>
```

# Git・GitHub 機能のトラブルシューティング

GitHub 関連の画面に表示される `Unsupported` だけでは、GitHub CLI の有無や原因を断定できません。次の順に画面で確認してください。

## まず画面で確認する

1. 左側の［ソース管理］に「Git が見つかりません」と出る場合は、その画面の［詳細］を開き、下の「Git が見つからない」を確認します。
2. 左下の［アカウント］を開き、［GitHub CLI］の項目に「未検出」と表示されるか確認します。表示される場合は、下の「GitHub CLI が見つからない」へ進みます。
3. 左側の［拡張機能］を開き、問題のビューを提供する拡張の状態を確認します。拡張や機能が利用できない場合は、下の「`Unsupported` と表示される」を確認します。
4. Git と GitHub CLI、拡張に問題が見当たらなければ、開いているフォルダー、GitHub remote、サインイン状態について、該当する項目を確認します。

## `Unsupported` と表示される

`Unsupported` は、現在のビューを拡張が提供できないことを示します。GitHub CLI の未検出そのものを表す表示ではありませんが、GitHub CLI が見つからない環境でも拡張がビューを提供できず、この表示に至る場合があります。画面上の表示だけで原因を決めず、上記の［アカウント］と［拡張機能］を確認してください。

GitHub CLI が見つかっていても、拡張や機能自体が未対応なら再インストールでは解決しません。拡張名と操作名を確認し、その拡張の対応範囲を確認してください。

## GitHub CLI が見つからない

GitHub CLI（実行ファイル名 `gh.exe`）が未導入、またはアプリから見つけられない状態です。GitHub CLI の[公式インストール手順](https://github.com/cli/cli#installation)に従って導入し、Sakura Editor NEXT を再起動してから画面をもう一度確認してください。

## Git が見つからない

SCM 画面で「Git が見つかりません」と表示された場合、アプリから `git.exe` が見つけられません。Git for Windows の[公式ダウンロードページ](https://git-scm.com/download/win)から導入してください。すでに導入済みなら、`git.exe` のあるフォルダーが Windows の `PATH` に含まれるか確認します。設定後は Sakura Editor NEXT を再起動してください。

「Git リポジトリがありません」は別の状態です。Git が見つかる環境で開いているフォルダーにリポジトリがないときに表示されます。

## フォルダーまたは GitHub リポジトリが見つからない

GitHub 機能は、開いているフォルダーを手掛かりにローカルの Git リポジトリと GitHub remote を確認します。

1. 対象のプロジェクトフォルダーを開きます。
2. そのフォルダーが Git リポジトリか確認します。まだ Git リポジトリでない場合、GitHub 上のリポジトリをクローンするか、Git で初期化して GitHub remote を設定します。
3. GitHub の URL を remote として設定しているか確認します。GitHub 以外の remote しかない場合、GitHub のリポジトリとして選択できません。
4. 複数のリポジトリや remote がある場合、対象を選択します。`origin` が自動で優先されるとは限りません。

作業フォルダーが空、Git リポジトリがない、GitHub remote がない場合は、GitHub の対象を決められません。SSH のホスト別名など、URLだけでは接続先を確定できない remote もあります。その場合は、実際の接続先が分かる HTTPS URL または GitHub ホスト名を使う設定を確認してください。

## GitHub にサインインしていない

GitHub の非公開情報を読むには、GitHub CLI のアカウント接続が必要です。画面に接続・サインイン操作が表示される場合はそれを選び、ブラウザーで GitHub の認証を完了してください。アプリは GitHub CLI の設定領域を指定して接続状態を管理するため、通常のターミナルで `gh auth login` を実行しても、アプリ側の接続状態には反映されないことがあります。認証コマンドの意味は GitHub CLI の[公式認証手順](https://cli.github.com/manual/gh_auth_login)で確認できます。

アプリからの切断は、GitHub CLI が共有する認証情報を削除する操作ではありません。認証情報の削除や切り替えは GitHub CLI の認証手順で行ってください。

## 公式手順

- [GitHub CLI のインストール](https://github.com/cli/cli#installation)
- [Git for Windows のダウンロード](https://git-scm.com/download/win)
- [GitHub CLI の認証（`gh auth login`）](https://cli.github.com/manual/gh_auth_login)

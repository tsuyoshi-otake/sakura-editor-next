# Git・GitHub 機能のトラブルシューティング

［ソース管理］や GitHub のビュー（Pull Requests、Issues、GitHub Actions）が期待どおりに動かないときの確認手順です。画面の［詳細］ボタンからもこのページを開けます。

## まず画面で確認する

画面に表示されている文言を下の表で探し、該当する節を確認してください。

| 表示される場所 | 表示 | 確認する節 |
|---|---|---|
| ［ソース管理］ | 「Git が見つかりません。Git for Windows をインストールし、git.exe を PATH に追加してから Sakura Editor NEXT を再起動してください。」 | [Git が見つからない](#git-が見つからない) |
| ［ソース管理］ | 「開いているフォルダーに Git リポジトリがありません。」など | [フォルダーまたは GitHub リポジトリが見つからない](#フォルダーまたは-github-リポジトリが見つからない) |
| GitHub のビュー | 「このビューを表示できません。［拡張機能］で拡張機能の状態を確認してください。」 | [「このビューを表示できません」と表示される](#このビューを表示できませんと表示される) |
| GitHub のビュー | 「GitHub CLI が見つかりません。導入または PATH を確認してください。」 | [GitHub CLI が見つからない](#github-cli-が見つからない) |
| ［アカウント］ | 「GitHub CLI 未検出（導入・PATH を確認）」 | [GitHub CLI が見つからない](#github-cli-が見つからない) |
| GitHub のビュー | 「GitHub ツールがサポートされていないバージョンで応答しました。」 | [GitHub CLI のバージョンが合わない](#github-cli-のバージョンが合わない) |
| ［アカウント］ | 「GitHub CLI にアカウントがありません」 | [GitHub にサインインしていない](#github-にサインインしていない) |

［アカウント］は、既定では Activity Bar（画面の端に縦に並ぶアイコン）の一番下にあるアイコンです。`workbench.activityBar.location` を `top` または `bottom` にしている場合は、タイトルバーにあります。［アカウント］のメニューは状態を表示するだけで、項目を選んでも操作は実行されません。メニューを開くたびに状態をバックグラウンドで確認し、その結果は次にメニューを開いたときに表示されます。Git や GitHub CLI の状態を変えたあとは、メニューを 2 回開いて確認してください。

## 「このビューを表示できません」と表示される

GitHub のビューを提供する拡張機能が、現在そのビューを提供できない状態です。原因は 1 つではないため、次の順に確認します。

1. 同じビューに「GitHub CLI が見つかりません。導入または PATH を確認してください。」も表示されている場合は、[GitHub CLI が見つからない](#github-cli-が見つからない)へ進みます。
2. ［拡張機能］を開き、そのビューを提供する拡張機能の状態を確認します。
3. GitHub CLI が見つかっている場合は、[GitHub CLI のバージョンが合わない](#github-cli-のバージョンが合わない)を確認します。

このビューの［詳細］ボタンはこのページを開きます。ブラウザーを開けなかった場合は「ガイドを開けませんでした。ブラウザーで次の URL を開いてください:」と URL が表示されるので、その URL をブラウザーで開いてください。

## GitHub CLI が見つからない

［アカウント］の「GitHub CLI 未検出（導入・PATH を確認）」は、次の 2 つの場合に表示されます。画面上の表示は同じです。

- GitHub CLI（`gh.exe`）が導入されていない、または `PATH` に含まれていない。
- `PATH` 上の `gh.exe` に GitHub, Inc. の署名がない。Scoop や Chocolatey のシム、自分でビルドした `gh.exe` には署名がないため、導入済みでも使われません。

対処:

1. [GitHub CLI 2.93.0 のリリースページ](https://github.com/cli/cli/releases/tag/v2.93.0)から、Windows 用インストーラー（64 ビット版 Windows では `gh_2.93.0_windows_amd64.msi`）を入手して実行します。GitHub のビューはこのバージョンだけを使います。理由は[GitHub CLI のバージョンが合わない](#github-cli-のバージョンが合わない)を参照してください。
2. Sakura Editor NEXT を再起動します。
3. ［アカウント］を 2 回開き、「GitHub CLI 未検出」が消えたことを確認します。

［アカウント］と GitHub のビューは、`gh.exe` を別の方法で探します。

- ［アカウント］は `PATH` 上の `gh.exe` だけを確認します。
- GitHub のビューは、Program Files フォルダー（通常は `C:\Program Files`）の `GitHub CLI\gh.exe` を先に探し、見つからなければ `PATH` を探します。

どちらも GitHub, Inc. の署名を確認します。探す場所が違うため、［アカウント］とビューで結果が食い違うことがあります。

## GitHub CLI のバージョンが合わない

GitHub のビューが使う GitHub CLI は、バージョン 2.93.0 だけです。それより新しいバージョンも古いバージョンも使われません。［アカウント］はバージョンを確認しないため、［アカウント］にアカウントが表示されていても、ビューだけが失敗することがあります。

1. ターミナルで次のコマンドを実行します。

   ```powershell
   gh --version
   ```

2. 1 行目が `gh version 2.93.0` で始まるか確認します。
3. 違う場合は、[GitHub CLI 2.93.0 のリリースページ](https://github.com/cli/cli/releases/tag/v2.93.0)から Windows 用インストーラーを入手して導入し直します。
4. Sakura Editor NEXT を再起動します。

`winget upgrade` などで GitHub CLI を自動更新すると、2.93.0 以外のバージョンになり、GitHub のビューが使えなくなります。

## Git が見つからない

［ソース管理］の「Git が見つかりません。…」は、アプリが `git.exe` を見つけられない状態です。アプリは `PATH` に含まれるフォルダーだけを探し、Git の既定のインストール先などは探しません。

1. Git for Windows の[公式ダウンロードページ](https://git-scm.com/download/win)から導入します。導入済みの場合は、`git.exe` のあるフォルダーが Windows の `PATH` に含まれるか確認します。
2. 新しく開いたターミナルで `where.exe git` を実行し、`git.exe` のパスが表示されることを確認します。
3. Sakura Editor NEXT を再起動します。

この画面の［詳細］ボタンはこのページを開きます。

## フォルダーまたは GitHub リポジトリが見つからない

### ［ソース管理］の表示

Git が見つかっていても、開いているものに Git リポジトリがない場合は次のように表示されます。これらの画面には［詳細］ボタンはありません。

| 状態 | 表示 | 使えるボタン |
|---|---|---|
| フォルダーを 1 つ開いている | 「開いているフォルダーに Git リポジトリがありません。」 | ［リポジトリを初期化］ |
| 複数のフォルダーを含むワークスペースを開いている | 「このワークスペースに Git リポジトリはありません。」 | ［リポジトリを初期化］ |
| フォルダーのないワークスペースを開いている | 「ワークスペースにフォルダーがありません。」 | ［ワークスペースにフォルダーを追加］ |
| 何も開いていない | 「Git の機能を使用するには、Git リポジトリを含むフォルダーを開くか、URL から複製してください。」 | ［フォルダーを開く］、［リポジトリを複製］ |

### GitHub のビューが対象にするリポジトリ

GitHub のビューは、開いているフォルダーから対象の GitHub リポジトリを自動で 1 つ決めます。対象を手動で選ぶ画面はありません。次のどれかに当てはまると対象が決まらず、ビューにデータが表示されません。

- フォルダーを開いていない。または、開いているフォルダーに Git リポジトリがない。
- ワークスペース内に Git リポジトリが 2 つ以上ある。
- github.com のリポジトリを指す remote がない。remote が 1 つもない場合も含みます。
- github.com 以外のホストを指す SSH の remote が 1 つでもある。SSH のホスト別名（例: `git@my-alias:owner/repo.git`）もこれに当たります。`git@github.com:owner/repo.git` は使えます。
- 別々のリポジトリを指す remote が 2 つ以上ある。たとえば、フォーク元を指す `upstream` とフォークを指す `origin` の両方がある場合です。`origin` が優先されることはありません。

GitHub のビューが扱うホストは github.com だけです。GitHub Enterprise Server など、github.com 以外のホストのリポジトリは表示できません。

remote の一覧は、リポジトリのフォルダーで次のコマンドを実行すると確認できます。

```powershell
git remote -v
```

## GitHub にサインインしていない

Sakura Editor NEXT には、GitHub へのサインインやサインアウトの画面はありません。アプリが `gh auth login` を実行することもありません。GitHub CLI に保存されているサインイン情報をそのまま使います。

1. 通常のターミナル（PowerShell など）で次のコマンドを実行し、github.com にサインインします。

   ```powershell
   gh auth login
   ```

2. ［アカウント］を 2 回開き、「<ログイン名> (GitHub CLI)」と表示されることを確認します。
3. GitHub のビューに反映されない場合は、Sakura Editor NEXT を再起動します。

GitHub のビューが読む GitHub CLI の設定は、環境変数 `GH_CONFIG_DIR` に絶対パスが設定されていればその場所、それ以外（未設定または相対パス）の場合は `%APPDATA%\GitHub CLI` です。ターミナルでも同じ設定を使っていれば、ターミナルでのサインインがビューに反映されます。

GitHub のビューは、`GH_TOKEN` や `GITHUB_TOKEN` などの環境変数のトークンを使いません。GitHub CLI のサインイン情報を使ってください。一方、［アカウント］は環境変数をそのまま引き継いで `gh auth status` を実行します。これらの環境変数を設定していると、［アカウント］とビューで結果が食い違うことがあります。

［アカウント］の「状態」に「注意が必要」と表示される場合は、ターミナルで `gh auth status` を実行し、表示される指示に従ってください。サインアウトやアカウントの切り替えも、ターミナルで GitHub CLI のコマンド（`gh auth logout`、`gh auth switch`）を使います。

## 公式手順

- [GitHub CLI 2.93.0 のリリース](https://github.com/cli/cli/releases/tag/v2.93.0)
- [GitHub CLI のインストール](https://github.com/cli/cli#installation)
- [Git for Windows のダウンロード](https://git-scm.com/download/win)
- [GitHub CLI の認証（`gh auth login`）](https://cli.github.com/manual/gh_auth_login)

# Sakura Editor NEXT の開発にご協力ください
Sakura Editor NEXT はオープンソースのテキストエディタで、多くの方の協力で支えられています。

まずは簡単なこと、小さなことからで構いません。ぜひ開発にご協力ください。

* Sakura Editor NEXT の感想や要望を投稿する
* ヘルプやテキストの誤字脱字を直す
* 不具合を直したり、機能を追加するプログラムを書く

# Pull Requestのガイドライン
WikiのPull Requestガイドラインをごらんください。

https://github.com/tsuyoshi-otake/sakura-editor-next/pulls

通常の機能追加、修正、ドキュメント更新、依存関係更新の Pull Request は `develop` を base branch にしてください。GitHub CLI では `gh pr create --base develop` を明示します。`main` は同一リポジトリの `develop` または `hotfix/*` から作成するリリース用 Pull Request のみを受け付けます。

`develop` 向け Pull Request は squash merge、`develop` から `main` へのリリース Pull Request は merge commit を使用します。保護ブランチの required checks はドキュメントのみの変更でも必要なため、Pull Request のコミットに `[ci skip]` または `[skip ci]` を含めないでください。

# ライセンス
Sakura Editor NEXT は誰もが自由にプログラムを使用できるように [zlib License](LICENSE) で提供されています。

みなさんからいただいたPull Requestやデータの著作権はそれぞれの作者の方が保有しますが、Sakura Editor NEXT プロジェクトが zlib License でそれを公開することに同意したものとして扱います。

zlib License以外のライセンス(GPLやApacheライセンスなど)が適用された著作物をPull Request等で提供する場合や、不安なことがありましたらGitHub Issuesや掲示板でご一報ください。

[掲示板一覧](https://sakura-editor.github.io/#bbs) 

# ビルドで使用する環境変数

## GitHub Actions が定義する環境変数

|環境変数名|説明|値の例|
|--|--|--|
|GITHUB_ACTIONS|GitHub Actionsで実行中かどうか|`true`|
|GITHUB_EVENT_NAME|ワークフローをトリガしたイベント名|`pull_request`, `push`|
|GITHUB_REF_TYPE|リファレンスの種類|`branch`, `tag`|
|GITHUB_SERVER_URL|GitHubサーバーのURL|`https://github.com`|
|GITHUB_REPOSITORY|リポジトリのフルパス|`tsuyoshi-otake/sakura-editor-next`|
|GITHUB_SHA|コミットハッシュ (PRではマージコミット)|40桁のハッシュ値|

## CI が定義する環境変数

|変数名|定義条件|定義仕様|
|--|--|--|
|CI_ACCOUNT_NAME|常に|`github.actor`|
|CI_REPO_NAME|常に|`github.repository`|
|CI_BUILD_VERSION|常に|`github.run_id`|
|CI_BUILD_NUMBER|常に|`github.run_number`|
|CI_BUILD_URL|常に|`github.server_url`/`github.repository`/actions/runs/`github.run_id`|
|SAKURA_BUILD_SOURCE_SHA|常に|実際に checkout する不変のソースSHA (`inputs.source_sha`、なければ `github.sha`)。再利用Workflowでは `github.sha` が呼出元のWorkflow SHAを指すため、成果物の版情報にはこの値を使う。旧タグの `version.cmake` はこの変数をまだ読まないため、release promotion は MSBuild の子プロセス内だけ `GITHUB_SHA` を同じ値へ差し替える。GitHub Actions のジョブ環境自体は変更しない。|
|GITHUB_COMMIT_URL|常に|`github.server_url`/`github.repository`/commit/`SAKURA_BUILD_SOURCE_SHA`|
|GITHUB_PR_NUMBER|pull_request のみ|`github.event.pull_request.number`|
|GITHUB_PR_HEAD_SHORT_COMMIT|pull_request のみ|`github.event.pull_request.head.sha` の先頭8文字|
|GITHUB_PR_HEAD_COMMIT|pull_request のみ|`github.event.pull_request.head.sha`|
|GITHUB_PR_HEAD_URL|pull_request のみ|`github.server_url`/`github.repository`/pull/`github.event.pull_request.number`/commits/`github.event.pull_request.head.sha`|

## version.cmake が CI ビルドで生成するマクロ

|変数名|定義条件|定義仕様|
|--|--|--|
|BUILD_ENV_NAME|常に|"GHA"|
|GIT_REMOTE_ORIGIN_URL|常に|`GITHUB_SERVER_URL`/`GITHUB_REPOSITORY`|
|GIT_COMMIT_HASH|常に|`SAKURA_BUILD_SOURCE_SHA` が定義されていればその値、なければ `GITHUB_SHA`|
|BUILD_VERSION|.git がある|カレントブランチの累積コミット数|
|DEV_VERSION|32bit以外|(値なし)|
|CI_BUILD_NUMBER_INT|pull_request のみ|CI_BUILD_NUMBERを数値化。|
|CI_BUILD_NUMBER_LABEL|pull_request のみ|"Build %CI_BUILD_NUMBER%"|
|GITHUB_PR_NUMBER_INT|pull_request のみ|GITHUB_PR_NUMBERを数値化。|
|GITHUB_PR_NUMBER_LABEL|pull_request のみ|"PR %GITHUB_PR_NUMBER%"|
|GITHUB_TAG_NAME|タグビルドのみ|`GITHUB_REF_NAME`|

[CI が定義した環境変数](#ci-が定義する環境変数)はそのまま定義されます。

## version.cmake がローカルビルドで生成するマクロ

|変数名|定義条件|定義仕様|
|--|--|--|
|BUILD_ENV_NAME|常に|"Local"|
|BUILD_VERSION|.git がある|カレントブランチの累積コミット数|
|GIT_COMMIT_HASH|.git がある|`git show -s --format=%H`|
|GIT_SHORT_COMMIT_HASH|.git がある|GIT_COMMIT_HASH の先頭8文字|
|GIT_REMOTE_ORIGIN_URL|.git がある|`git config --get remote.origin.url`|

## ローカルビルドで使用する環境変数

|変数名|既定値|説明|
|--|--|--|
|NUM_VSVERSION|インストール済みの最新バージョン|使用する Visual Studio のメジャーバージョン。例: `16` は Visual Studio 2019、`17` は Visual Studio 2022。詳細は [MSBuild の検索について](./find-tools.md#msbuild) を参照。|
|CMD_MSBUILD|自動検出|canonical CLIが使用する `MSBuild.exe` の明示パス。未設定時はPATHとvswhereで探索する。|
|SAKURA_BUILD_JOBS|論理CPU数|既存 `build-*.bat` 互換shimがcanonical CLIへ渡す全体並列予算。正の整数を指定する。canonical CLIを直接使う場合は `--jobs` を優先する。|
|SakuraSkipModulesCheck|`false`|MSBuild内部用の緊急診断escape hatch。`true`でread-only manifest stamp検査を省略する。通常開発・CI・配布では設定せず、この値を使った成果物を検証済みと扱わない。|

CMakeには同じ目的のcache option `SAKURA_SKIP_MODULES_CHECK=ON` がある。これも診断専用であり、通常のMinGW/CIビルドでは使用しない。
|SAKURA_GENERATE_ASSEMBLY_LISTINGS|未設定（無効）|`1` または `true` の場合、MSVC の `/FAsu` を有効にして `.asm` 一覧を生成する。Release 製品 listing pass は `cl.exe` の中間 `.asm` を Link 直前に削除し、LTCG が同じ出力先へ最終 listing を書く。`tests1` は製品 `/GL` object を archive 経由で再リンクし、object に記録された `/Fa` 出力先を LTCG で再利用する。そのため `build-sln.bat` / `build-all.bat` は solution と tests1 をこの値を明示的に `false` にした第一段階で完成させ、製品 `sakura.vcxproj` だけを `true`・`/m:1` にした第二段階で再ビルドする。製品 listing branch は `/MP` を無効にし、リンカーには `/CGTHREADS:1` を渡す。値を切り替えた次のビルドでは再コンパイルが発生する場合がある。`build-all.bat` は配布成果物のためパッケージング段階では常に `1` を保持する。CI (`build-sakura.yml`) の Release ビルドは `pull_request` 以外のトリガー（`push`/`workflow_dispatch`）でのみ `1` を設定し、`pull_request` では `0` のままにする。`zipArtifacts.bat` は値が `1`/`true` でない場合、`.asm` の収集と `Asm` zip の生成を必須とせず skip する。|
|SAKURA_DEV_BUILD_TARGET|`Build`|診断専用。`build-dev.bat` が実行する MSBuild ターゲットを上書きする。`Build` 以外では成果物が完成しない場合があるため、通常は設定せず、使用後は解除する。|
|SAKURA_MSBUILD_BINLOG|未設定（無効）|診断専用。`msbuild_command` が構築するすべての MSBuild 呼び出し（`build-dev.bat`/`build-sln.bat`/`build-all.bat`）に `/bl:<パス>` を追加し、MSBuild バイナリログを指定パスへ出力する。値は空文字列であってはならず、設定した場合は空白のみの値も含めて明示的エラーになる。ビルド最適化・アセンブリ一覧・LTCG・ビルドターゲットには影響しない。計測が終わったら解除する。|
|SAKURA_MSBUILD_PERFORMANCE_SUMMARY|未設定（無効）|診断専用。`1` または `true` の場合、`msbuild_command` が構築するすべての MSBuild 呼び出しに `/clp:PerformanceSummary` を追加し、コンソールログの末尾にタスク別実行時間の要約を出力する。`0` または `false` は明示的に無効（未設定と同じ挙動）。それ以外の値は明示的エラーになる（無効値を暗黙に無効側へ倒さない）。ビルド最適化・アセンブリ一覧・LTCG・ビルドターゲットには影響しない。|

## zipArtifacts.bat で設定する環境変数

### 生成する環境変数

|生成する環境変数|説明|有効性|
----|----|----
|ALPHA|alphaバージョンの場合1|x64ビルドの場合|
|BUILD_ACCOUNT|CIのビルドアカウント名|sakuraeditor用のアカウントの場合空|
|TAG_NAME|"tag_"+tag名|tagが有効な場合|
|BUILD_NUMBER|"build"+ビルド番号|CIビルド以外の場合"buildLocal"|
|PR_NAME|"PR"+PR番号|CIでのPRのビルドのみ有効|
|SHORTHASH|commithashの先頭8文字|実体はGIT_SHORT_COMMIT_HASH|
|RELEASE_PHASE|"alpha"または空|x64ビルドの場合のみ有効|
|BASENAME|成果物のzipファイル名(拡張子含まない部分)|常に有効|
|WORKDIR|作業用フォルダー|常に有効|
|WORKDIR_LOG|ログファイル用の作業用フォルダー|常に有効|
|WORKDIR_EXE|実行ファイル(一般向け)用の作業用フォルダー|常に有効|
|WORKDIR_DEV|開発者向け成果物用の作業用フォルダー|常に有効|
|WORKDIR_INST|インストーラ用の作業用フォルダー|常に有効|
|WORKDIR_ASM|アセンブラ出力用の作業用フォルダー|常に有効|
|OUTFILE|成果物のzipファイル名|常に有効|
|OUTFILE_LOG|ログファイルの成果物のzipファイル名|常に有効|
|OUTFILE_EXE|実行ファイル(一般向け)の成果物のzipファイル名|常に有効|
|OUTFILE_DEV|開発者向け成果物のzipファイル名|常に有効|
|OUTFILE_INST|インストーラの成果物のzipファイル名|常に有効|
|OUTFILE_ASM|アセンブラ出力の成果物のzipファイル名|常に有効|

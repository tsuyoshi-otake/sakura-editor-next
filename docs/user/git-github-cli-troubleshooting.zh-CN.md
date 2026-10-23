# Git 与 GitHub 功能故障排查

当“源代码管理”或 GitHub 视图（Pull Requests、Issues、GitHub Actions）无法按预期工作时，请按本页步骤检查。也可以通过界面上的“详细信息”按钮打开本页。

## 先在界面上确认

在下表中查找界面上显示的文字，然后查看对应章节。

| 显示位置 | 显示内容 | 对应章节 |
|---|---|---|
| “源代码管理” | 「未找到 Git。请安装 Git for Windows，将 git.exe 添加到 PATH，然后重新启动 Sakura Editor NEXT。」 | [找不到 Git](#找不到-git) |
| “源代码管理” | 「打开的文件夹不是 Git 存储库。」等 | [找不到文件夹或 GitHub 存储库](#找不到文件夹或-github-存储库) |
| GitHub 视图 | 「无法显示此视图。请在“扩展”中检查该扩展的状态。」 | [显示“无法显示此视图”](#显示无法显示此视图) |
| GitHub 视图 | 「未检测到 GitHub CLI。请安装或检查 PATH。」 | [找不到 GitHub CLI](#找不到-github-cli) |
| “帐户” | 「未检测到 GitHub CLI（请检查安装和 PATH）。」 | [找不到 GitHub CLI](#找不到-github-cli) |
| GitHub 视图 | 「GitHub 工具以不受支持的版本响应。」 | [GitHub CLI 版本不匹配](#github-cli-版本不匹配) |
| “帐户” | 「GitHub CLI 没有帐户」 | [未登录 GitHub](#未登录-github) |

“帐户”默认是活动栏（Activity Bar，界面边缘竖排的图标）最下方的图标。如果将 `workbench.activityBar.location` 设置为 `top` 或 `bottom`，它位于标题栏中。“帐户”菜单只显示状态，选择其中的项目不会执行任何操作。每次打开菜单时，应用会在后台检查状态，检查结果在下一次打开菜单时显示。更改 Git 或 GitHub CLI 的状态后，请打开菜单 2 次进行确认。

## 显示“无法显示此视图”

提供 GitHub 视图的扩展目前无法提供该视图。原因不止一个，请按以下顺序检查。

1. 如果同一个视图中还显示「未检测到 GitHub CLI。请安装或检查 PATH。」，请前往[找不到 GitHub CLI](#找不到-github-cli)。
2. 打开“扩展”，检查提供该视图的扩展的状态。
3. 如果能找到 GitHub CLI，请检查[GitHub CLI 版本不匹配](#github-cli-版本不匹配)。

此视图中的“详细信息”按钮会打开本页。如果无法打开浏览器，会显示「无法打开指南。请在浏览器中打开以下 URL：」和一个 URL，请在浏览器中打开该 URL。

## 找不到 GitHub CLI

“帐户”中的「未检测到 GitHub CLI（请检查安装和 PATH）。」会在以下 2 种情况下显示。界面上的显示相同。

- GitHub CLI（`gh.exe`）未安装，或不在 `PATH` 中。
- `PATH` 中的 `gh.exe` 没有 GitHub, Inc. 的签名。Scoop 或 Chocolatey 的 shim 以及自行构建的 `gh.exe` 没有签名，因此即使已安装也不会被使用。

处理方法：

1. 从 [GitHub CLI 2.93.0 发布页面](https://github.com/cli/cli/releases/tag/v2.93.0)获取 Windows 安装程序（64 位 Windows 使用 `gh_2.93.0_windows_amd64.msi`）并运行。GitHub 视图只使用此版本。原因请参阅[GitHub CLI 版本不匹配](#github-cli-版本不匹配)。
2. 重新启动 Sakura Editor NEXT。
3. 打开“帐户”2 次，确认「未检测到 GitHub CLI」已消失。

“帐户”和 GitHub 视图用不同的方式查找 `gh.exe`。

- “帐户”只检查 `PATH` 中的 `gh.exe`。
- GitHub 视图先查找 Program Files 文件夹（通常是 `C:\Program Files`）下的 `GitHub CLI\gh.exe`，找不到时再查找 `PATH`。

两者都会检查 GitHub, Inc. 的签名。由于查找位置不同，“帐户”和视图的结果可能不一致。

## GitHub CLI 版本不匹配

GitHub 视图使用的 GitHub CLI 只有 2.93.0 版本。比它新或旧的版本都不会被使用。“帐户”不检查版本，因此即使“帐户”中显示了帐户，也可能只有视图失败。

1. 在终端中运行以下命令。

   ```powershell
   gh --version
   ```

2. 确认第 1 行是否以 `gh version 2.93.0` 开头。
3. 如果不是，请从 [GitHub CLI 2.93.0 发布页面](https://github.com/cli/cli/releases/tag/v2.93.0)获取 Windows 安装程序并重新安装。
4. 重新启动 Sakura Editor NEXT。

如果用 `winget upgrade` 等方式自动更新 GitHub CLI，版本会变成 2.93.0 以外的版本，GitHub 视图将无法使用。

## 找不到 Git

“源代码管理”中的「未找到 Git。…」表示应用找不到 `git.exe`。应用只在 `PATH` 包含的文件夹中查找，不会查找 Git 的默认安装位置等。

1. 从 Git for Windows 的[官方下载页面](https://git-scm.com/download/win)安装。如果已经安装，请确认 `git.exe` 所在的文件夹包含在 Windows 的 `PATH` 中。
2. 在新打开的终端中运行 `where.exe git`，确认会显示 `git.exe` 的路径。
3. 重新启动 Sakura Editor NEXT。

此界面中的“详细信息”按钮会打开本页。

## 找不到文件夹或 GitHub 存储库

### “源代码管理”中的显示

即使找到了 Git，如果打开的内容中没有 Git 存储库，也会显示如下内容。这些界面没有“详细信息”按钮。

| 状态 | 显示内容 | 可用按钮 |
|---|---|---|
| 打开了 1 个文件夹 | 「打开的文件夹不是 Git 存储库。」 | “初始化存储库” |
| 打开了包含多个文件夹的工作区 | 「此工作区没有 Git 存储库。」 | “初始化存储库” |
| 打开了没有文件夹的工作区 | 「工作区中没有文件夹。」 | “将文件夹添加到工作区” |
| 什么都没打开 | 「若要使用 Git 功能，可以打开包含 Git 存储库的文件夹或从 URL 克隆。」 | “打开文件夹”、“克隆存储库” |

### GitHub 视图的目标存储库

GitHub 视图会根据打开的文件夹自动确定 1 个目标 GitHub 存储库。没有手动选择目标的界面。满足以下任一条件时，目标无法确定，视图中不会显示数据。

- 没有打开文件夹。或者，打开的文件夹中没有 Git 存储库。
- 工作区内有 2 个或更多 Git 存储库。
- 没有指向 github.com 存储库的 remote。一个 remote 都没有的情况也包括在内。
- 存在任何 1 个指向 github.com 以外主机的 SSH remote。SSH 主机别名（例如 `git@my-alias:owner/repo.git`）也属于这种情况。`git@github.com:owner/repo.git` 可以使用。
- 有 2 个或更多指向不同存储库的 remote。例如，同时存在指向 fork 来源的 `upstream` 和指向 fork 的 `origin`。`origin` 不会被优先选择。

GitHub 视图只支持 github.com 这一个主机。GitHub Enterprise Server 等 github.com 以外主机上的存储库无法显示。

要查看 remote 列表，请在存储库所在的文件夹中运行以下命令。

```powershell
git remote -v
```

## 未登录 GitHub

Sakura Editor NEXT 没有登录或退出登录 GitHub 的界面。应用也不会运行 `gh auth login`。应用直接使用 GitHub CLI 中保存的登录信息。

1. 在普通终端（PowerShell 等）中运行以下命令，登录 github.com。

   ```powershell
   gh auth login
   ```

2. 打开“帐户”2 次，确认显示「<登录名> (GitHub CLI)」。
3. 如果 GitHub 视图中没有体现，请重新启动 Sakura Editor NEXT。

GitHub 视图读取的 GitHub CLI 配置位置如下：如果环境变量 `GH_CONFIG_DIR` 设置为绝对路径，则为该位置；否则（未设置或为相对路径）为 `%APPDATA%\GitHub CLI`。如果终端也使用相同的配置，在终端中的登录会反映到 GitHub 视图中。

GitHub 视图不使用 `GH_TOKEN` 或 `GITHUB_TOKEN` 等环境变量中的令牌。请使用 GitHub CLI 的登录信息。而“帐户”会原样继承环境变量来运行 `gh auth status`。如果设置了这些环境变量，“帐户”和 GitHub 视图的结果可能不一致。

如果“帐户”的「状态」中显示「需要注意」，请在终端中运行 `gh auth status`，并按显示的提示操作。退出登录和切换帐户也使用终端中的 GitHub CLI 命令（`gh auth logout`、`gh auth switch`）。

## 官方说明

- [GitHub CLI 2.93.0 发布](https://github.com/cli/cli/releases/tag/v2.93.0)
- [安装 GitHub CLI](https://github.com/cli/cli#installation)
- [下载 Git for Windows](https://git-scm.com/download/win)
- [GitHub CLI 认证（`gh auth login`）](https://cli.github.com/manual/gh_auth_login)

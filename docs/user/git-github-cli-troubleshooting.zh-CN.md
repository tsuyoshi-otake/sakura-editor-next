# Git 与 GitHub 功能故障排查

仅凭视图中的 `Unsupported`，无法判断是否安装了 GitHub CLI，也无法确定具体原因。请先在应用中依次检查以下位置。

## 先在应用中检查

1. 如果左侧“源代码管理”显示找不到 Git，请打开其中的“详细信息”，并阅读下文“找不到 Git”。
2. 打开左下角的“帐户”，查看“GitHub CLI”项目是否显示“未检测到”。如果是，请阅读下文“找不到 GitHub CLI”。
3. 打开左侧的“扩展”，检查提供该视图的扩展状态。如果扩展或功能不可用，请阅读下文“视图显示 `Unsupported`”。
4. 如果 Git、GitHub CLI 和扩展都可用，请按下文对应章节检查已打开的文件夹、GitHub 远程地址和登录状态。

## 视图显示 `Unsupported`

`Unsupported` 表示扩展无法提供当前视图。它并非直接提示“缺少 GitHub CLI”，但在找不到 GitHub CLI 的环境中，扩展也可能因此无法提供视图并显示该状态。不要只凭此视图判断原因；请按上文检查“帐户”和“扩展”。

即使 GitHub CLI 可用，如果扩展本身不支持该功能，重新安装也无法启用它。请确认扩展名称和操作，并核对该扩展的功能支持范围。

## 找不到 GitHub CLI

应用找不到 `gh.exe`。它可能尚未安装，或者应用无法找到它。请按照 [GitHub CLI 官方安装说明](https://github.com/cli/cli#installation)安装，重启 Sakura Editor NEXT，然后再次查看该视图。

## 找不到 Git

如果“源代码管理”显示找不到 Git，表示应用无法找到 `git.exe`。请从 [Git for Windows 官方下载页面](https://git-scm.com/download/win)安装。如果已经安装，请确认包含 `git.exe` 的目录位于 Windows 的 `PATH` 中。完成安装或修改 `PATH` 后，重启 Sakura Editor NEXT。

“没有 Git 仓库”是另一种状态：Git 可以使用，但当前打开的文件夹中没有仓库。

## 找不到文件夹或 GitHub 仓库

GitHub 功能会根据打开的文件夹查找本地 Git 仓库及其 GitHub 远程仓库。

1. 打开目标项目文件夹。
2. 确认该文件夹是 Git 仓库。如果不是，请从 GitHub 克隆仓库，或使用 Git 初始化仓库并配置 GitHub 远程地址。
3. 确认至少有一个远程地址指向 GitHub。只有非 GitHub 远程地址的仓库无法作为 GitHub 仓库选中。
4. 如果存在多个仓库或远程地址，请选择目标。不要假定应用会自动选择 `origin`。

工作区为空、没有 Git 仓库或没有 GitHub 远程地址时，应用无法确定 GitHub 操作目标。使用 SSH 主机别名的远程地址也可能无法确定实际主机；此时请检查远程配置，改用能明确目标的 HTTPS 地址或 GitHub 主机名。

## 尚未登录 GitHub

访问 GitHub 私有信息需要连接 GitHub CLI 账户。如果应用显示“连接”或“登录”操作，请使用该操作并在浏览器中完成认证。Sakura Editor NEXT 使用应用专用的 GitHub CLI 配置位置，因此在普通终端运行 `gh auth login` 不一定会让应用进入已连接状态。有关该命令，请参阅 [GitHub CLI 官方认证说明](https://cli.github.com/manual/gh_auth_login)。

在应用中断开连接不会删除 GitHub CLI 共享的认证信息。要删除或切换这些凭据，请使用 GitHub CLI 的认证命令。

## 官方说明

- [安装 GitHub CLI](https://github.com/cli/cli#installation)
- [下载 Git for Windows](https://git-scm.com/download/win)
- [使用 GitHub CLI 认证（`gh auth login`）](https://cli.github.com/manual/gh_auth_login)

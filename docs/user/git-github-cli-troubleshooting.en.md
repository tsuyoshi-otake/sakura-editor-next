# Troubleshooting Git and GitHub features

Use these steps when **Source Control** or the GitHub views (Pull Requests, Issues, GitHub Actions) do not work as expected. You can also open this page from the **Details** button on the screen.

## Check the screen first

Find the message on your screen in the table below, then go to the matching section.

| Where it appears | What is shown | Section to check |
|---|---|---|
| **Source Control** | "Git was not found. Install Git for Windows, add git.exe to PATH, then restart Sakura Editor NEXT." | [Git cannot be found](#git-cannot-be-found) |
| **Source Control** | "The open folder is not a Git repository." and similar messages | [Folder or GitHub repository cannot be found](#folder-or-github-repository-cannot-be-found) |
| GitHub view | "This view cannot be displayed. Check the extension status in Extensions." | [The view says "This view cannot be displayed"](#the-view-says-this-view-cannot-be-displayed) |
| GitHub view | "GitHub CLI was not found. Install it or check PATH." | [GitHub CLI cannot be found](#github-cli-cannot-be-found) |
| **Accounts** | "GitHub CLI not found (install or check PATH)." | [GitHub CLI cannot be found](#github-cli-cannot-be-found) |
| GitHub view | "The GitHub tool answered with an unsupported version." | [GitHub CLI version does not match](#github-cli-version-does-not-match) |
| **Accounts** | "No GitHub CLI accounts" | [Not signed in to GitHub](#not-signed-in-to-github) |

By default, **Accounts** is the icon at the bottom of the Activity Bar (the vertical row of icons at the edge of the window). If `workbench.activityBar.location` is `top` or `bottom`, it is in the title bar. The **Accounts** menu only shows status. Selecting an item does nothing. Each time you open the menu, the app checks the status in the background. The result appears the next time you open the menu. After you change Git or GitHub CLI, open the menu twice to check.

## The view says "This view cannot be displayed"

The extension that provides the GitHub view cannot provide that view right now. There is more than one possible cause, so check in this order.

1. If the same view also shows "GitHub CLI was not found. Install it or check PATH.", go to [GitHub CLI cannot be found](#github-cli-cannot-be-found).
2. Open **Extensions** and check the status of the extension that provides the view.
3. If GitHub CLI is found, check [GitHub CLI version does not match](#github-cli-version-does-not-match).

The **Details** button on this view opens this page. If the browser could not be opened, the app shows "Could not open the guide. Open this URL in a browser:" followed by a URL. Open that URL in your browser.

## GitHub CLI cannot be found

**Accounts** shows "GitHub CLI not found (install or check PATH)." in the following two cases. The message is the same in both.

- GitHub CLI (`gh.exe`) is not installed, or is not in `PATH`.
- The `gh.exe` on `PATH` has no signature from GitHub, Inc. Scoop and Chocolatey shims and `gh.exe` files you built yourself have no signature. They are not used, even if installed.

What to do:

1. From the [GitHub CLI 2.93.0 release page](https://github.com/cli/cli/releases/tag/v2.93.0), get the Windows installer (`gh_2.93.0_windows_amd64.msi` on 64-bit Windows) and run it. The GitHub views use only this version. For the reason, see [GitHub CLI version does not match](#github-cli-version-does-not-match).
2. Restart Sakura Editor NEXT.
3. Open **Accounts** twice and check that "GitHub CLI not found" is gone.

**Accounts** and the GitHub views look for `gh.exe` in different ways.

- **Accounts** checks only the `gh.exe` on `PATH`.
- The GitHub views first look for `GitHub CLI\gh.exe` in the Program Files folder (usually `C:\Program Files`). If they do not find it, they search `PATH`.

Both check for the signature from GitHub, Inc. Because they look in different places, **Accounts** and the views can show different results.

## GitHub CLI version does not match

The GitHub views use only GitHub CLI version 2.93.0. Neither newer nor older versions are used. **Accounts** does not check the version. So **Accounts** can show an account while only the views fail.

1. Run the following command in a terminal.

   ```powershell
   gh --version
   ```

2. Check whether the first line starts with `gh version 2.93.0`.
3. If it does not, get the Windows installer from the [GitHub CLI 2.93.0 release page](https://github.com/cli/cli/releases/tag/v2.93.0) and install it again.
4. Restart Sakura Editor NEXT.

If GitHub CLI is updated automatically, for example by `winget upgrade`, it moves to a version other than 2.93.0. The GitHub views then stop working.

## Git cannot be found

"Git was not found. …" in **Source Control** means the app cannot find `git.exe`. The app searches only the folders in `PATH`. It does not search Git's default install location or other places.

1. Install Git from the [official Git for Windows download page](https://git-scm.com/download/win). If it is already installed, check that the folder containing `git.exe` is in the Windows `PATH`.
2. In a newly opened terminal, run `where.exe git` and check that the path to `git.exe` is shown.
3. Restart Sakura Editor NEXT.

The **Details** button on this screen opens this page.

## Folder or GitHub repository cannot be found

### What Source Control shows

If Git is found but what you have open has no Git repository, **Source Control** shows the messages below. These screens have no **Details** button.

| State | What is shown | Available buttons |
|---|---|---|
| One folder is open | "The open folder is not a Git repository." | **Initialize Repository** |
| A workspace with several folders is open | "This workspace has no Git repository." | **Initialize Repository** |
| A workspace with no folders is open | "The workspace has no folders." | **Add Folder to Workspace** |
| Nothing is open | "In order to use Git features, you can open a folder containing a Git repository or clone from a URL." | **Open Folder**, **Clone Repository** |

### Which repository the GitHub views use

The GitHub views choose one target GitHub repository automatically from the open folder. There is no screen to choose it by hand. If any of the following applies, no target is chosen and the views show no data.

- No folder is open, or the open folder has no Git repository.
- The workspace has two or more Git repositories.
- No remote points to a repository on github.com. This includes the case where there is no remote at all.
- At least one SSH remote points to a host other than github.com. An SSH host alias (for example `git@my-alias:owner/repo.git`) counts as well. `git@github.com:owner/repo.git` works.
- Two or more remotes point to different repositories. For example, both `upstream` (the original of a fork) and `origin` (the fork) exist. `origin` is not given priority.

The GitHub views handle only github.com. Repositories on other hosts, such as GitHub Enterprise Server, cannot be shown.

To see the list of remotes, run the following command in the repository folder.

```powershell
git remote -v
```

## Not signed in to GitHub

Sakura Editor NEXT has no screen to sign in to or sign out of GitHub. The app also does not run `gh auth login`. It uses the sign-in information that GitHub CLI has saved, as it is.

1. In a regular terminal (such as PowerShell), run the following command and sign in to github.com.

   ```powershell
   gh auth login
   ```

2. Open **Accounts** twice and check that a row like `<login> (GitHub CLI)` is shown.
3. If the GitHub views do not show the change, restart Sakura Editor NEXT.

The GitHub views read the GitHub CLI configuration from the folder in the environment variable `GH_CONFIG_DIR` if it is set to an absolute path. Otherwise (not set, or a relative path), they read it from `%APPDATA%\GitHub CLI`. If your terminal uses the same configuration, signing in from the terminal also applies to the views.

The GitHub views do not use tokens in environment variables such as `GH_TOKEN` and `GITHUB_TOKEN`. Use the sign-in information of GitHub CLI. **Accounts**, on the other hand, runs `gh auth status` with the environment variables inherited as they are. If you set these variables, **Accounts** and the views can show different results.

If the "Status" row of **Accounts** shows "Needs attention", run `gh auth status` in a terminal and follow the instructions it shows. To sign out or switch accounts, also use the GitHub CLI commands in a terminal (`gh auth logout`, `gh auth switch`).

## Official instructions

- [GitHub CLI 2.93.0 release](https://github.com/cli/cli/releases/tag/v2.93.0)
- [Install GitHub CLI](https://github.com/cli/cli#installation)
- [Download Git for Windows](https://git-scm.com/download/win)
- [Authenticate with GitHub CLI (`gh auth login`)](https://cli.github.com/manual/gh_auth_login)

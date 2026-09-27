# Troubleshooting Git and GitHub features

An `Unsupported` view alone does not establish whether GitHub CLI is installed or what caused the problem. Check these areas in the app first.

## Check the app first

1. If **Source Control** says Git cannot be found, open **Details** there and see “Git cannot be found” below.
2. Open **Accounts** at the bottom left and check whether **GitHub CLI** says it cannot be found. If so, see “GitHub CLI cannot be found” below.
3. Open **Extensions** on the left and check the status of the extension that provides the affected view. If the extension or feature is unavailable, see “The view says `Unsupported`” below.
4. If Git, GitHub CLI, and the extension look available, check the relevant sections below for the open folder, GitHub remote, and sign-in state.

## The view says `Unsupported`

`Unsupported` means the extension cannot provide the current view. It is not a direct “GitHub CLI is missing” message, but an extension may also reach this state in an environment where GitHub CLI cannot be found. Do not infer the cause from this view alone; check **Accounts** and **Extensions** as described above.

Even when GitHub CLI is available, installing it again will not enable a feature the extension does not support. Check the extension name and operation, then confirm that extension's supported features.

## GitHub CLI cannot be found

The app cannot find `gh.exe`. It may not be installed or may not be available to the app. Follow the [official GitHub CLI installation instructions](https://github.com/cli/cli#installation), restart Sakura Editor NEXT, and check the view again.

## Git cannot be found

When **Source Control** says Git cannot be found, the app cannot locate `git.exe`. Install Git from the [official Git for Windows download page](https://git-scm.com/download/win). If it is already installed, check whether the directory containing `git.exe` is on the Windows `PATH`. Restart Sakura Editor NEXT after changing the installation or `PATH`.

“No Git repository” is a different state: Git is available, but the open folder does not contain a repository.

## No folder or GitHub repository is found

GitHub features use the open folder to find a local Git repository and its GitHub remote.

1. Open the project folder you want to use.
2. Make sure it is a Git repository. If it is not, clone a repository from GitHub, or initialize it with Git and configure a GitHub remote.
3. Make sure a remote points to GitHub. A repository with only non-GitHub remotes cannot be selected as a GitHub repository.
4. If there are multiple repositories or remotes, select the one you want. Do not assume `origin` is selected automatically.

An empty workspace, no Git repository, or no GitHub remote leaves the app without a GitHub target. A remote URL using an SSH host alias may not identify its real host. In that case, check the remote configuration and use an HTTPS URL or a GitHub hostname that identifies the destination.

## You are not signed in to GitHub

Access to private GitHub information requires a connected GitHub CLI account. If the app offers a connect or sign-in action, use it and complete authentication in the browser. Sakura Editor NEXT uses an app-specific GitHub CLI configuration location, so running `gh auth login` in a regular terminal may not connect the account inside the app. See the [official GitHub CLI authentication instructions](https://cli.github.com/manual/gh_auth_login) for details about the command.

Disconnecting in the app does not delete credentials shared by GitHub CLI. Use GitHub CLI's authentication commands to remove or switch those credentials.

## Official instructions

- [Install GitHub CLI](https://github.com/cli/cli#installation)
- [Download Git for Windows](https://git-scm.com/download/win)
- [Authenticate with GitHub CLI (`gh auth login`)](https://cli.github.com/manual/gh_auth_login)

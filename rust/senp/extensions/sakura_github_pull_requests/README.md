# GitHub Issues and Pull Requests

This built-in SENP extension contributes the `github-pull-requests` ViewContainer and the
upstream-compatible `pr:github` and `issues:github` View IDs. It requests fixed, repository-scoped
GitHub reads from the native tool broker and has no WASI, network, filesystem, process, or token access.

The current implementation publishes the paged Issues list. Pull request rows and read-only details
are added by the following implementation stages.

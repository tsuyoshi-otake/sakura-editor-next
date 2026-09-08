# GitHub Issues and Pull Requests

This built-in SENP extension contributes the `github-pull-requests` ViewContainer and the
upstream-compatible `pr:github` and `issues:github` View IDs. It requests fixed, repository-scoped
GitHub reads from the native tool broker and has no WASI, network, filesystem, process, or token access.

The extension publishes separate paged Issues and Pull Requests trees. Selecting an Issue or pull
request opens a bounded read-only document; expanding either row pages its ordinary conversation
comments. Pull request documents show draft or merged state and preserve base and head repository,
branch, and commit identities. Those identities are display data only: every detail and comment read
remains anchored to the repository selected by the native broker. Empty bodies, empty comment pages,
partial pages, and failed reads remain distinct terminal presentations.

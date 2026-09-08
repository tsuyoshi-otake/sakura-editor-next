# GitHub Issues and Pull Requests

This built-in SENP extension contributes the `github-pull-requests` ViewContainer and the
upstream-compatible `pr:github` and `issues:github` View IDs. It requests fixed, repository-scoped
GitHub reads from the native tool broker and has no WASI, network, filesystem, process, or token access.

The current implementation publishes the paged Issues list, opens a selected Issue as a bounded
read-only document, and pages its comments as child rows. Selecting a comment opens its own
read-only document. Empty bodies, empty comment pages, partial pages, and failed reads remain
distinct terminal presentations. Pull request rows are added by the following implementation stage.

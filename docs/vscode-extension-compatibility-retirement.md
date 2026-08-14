# VS Code extension compatibility retirement

Sakura Editor NEXT has removed its VS Code extension compatibility capability.
This includes extension discovery and installation, Open VSX access, VSIX
handling, the extension host, contributed workbench surfaces, extension secret
storage, and the associated build and packaging paths.

The capability was retired as a precaution against software supply-chain
attacks. No dormant implementation is retained in the source tree or release
artifacts. Existing installations are cleaned during an installer upgrade by
removing the former `exthost` payload directory.

The native workbench, built-in editor features, Sakura Editor plug-ins, and
macros are unaffected. They do not execute VS Code extensions.

If compatibility is reconsidered in the future, recover the prior design from
Git history into a new change and perform a fresh security review before any
code or package-execution path is restored. Do not reintroduce the removed
implementation as disabled or unreachable source.

# Workbench icon guidance

This directory owns the bundled Codicon font and built-in icon resolution used
by native Sakura Editor NEXT workbench surfaces.

- Keep icon lookup restricted to the bundled, versioned Codicon vocabulary.
- Unknown icon identifiers must fall back safely without loading external font
  files or package metadata.
- Keep glyph lookup allocation-free on paint paths where practical.
- Preserve the existing C++ encoding rules when editing sources in this tree.

VS Code extension compatibility and externally supplied icon themes were
retired for supply-chain safety. See
[`../../../docs/vscode-extension-compatibility-retirement.md`](../../../docs/vscode-extension-compatibility-retirement.md).

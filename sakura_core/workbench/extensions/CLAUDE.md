# Extensions Workbench Guidance

## Stable Boundary

- The Extensions ViewContainer is a native workbench projection. Package
  discovery, validation, installation, activation, and persistence remain in
  the SENP service layer; the HWND never owns those operations.
- `CWorkbenchPanelHost` owns ViewContainer-title actions such as the overflow
  menu. `CExtensionsWorkbenchTool` exposes the install command but must not draw
  a second title bar or a large install call-to-action inside a View.
- The first-party SENP surface is one flat 72-DIP extension list. Do not split
  the small bounded catalog into Built-in and Installed sections, and do not
  add a permanent search control without a real scale or discovery need.
- A bundled package that is not installed remains a management-service catalog
  entry and gets a real row-level `Install` command. The HWND calls the service
  by stable extension ID; it never installs embedded resources itself.
- An installed bundled package gets a row-level `Uninstall` command. Successful
  uninstall keeps the catalog row and changes the action back to `Install`;
  runtime and language consumers observe the management revision and stop using
  the removed package immediately.
- Extension rows contain identity and compact status metadata. `README.md`
  belongs to an extension-details editor and must not be rendered as an inline
  card in the list. Until that editor exists, rows remain non-activating rather
  than opening an unrelated or fabricated surface.
- Overflow uses the shared workbench overlay scrollbar over the View body. Do
  not restore a native `WS_VSCROLL` gutter.

## Intentional SENP Divergence

SENP is a local Sakura Editor NEXT package format, not VSIX and not an Open VSX
client. There is no extension gallery. Marketplace search, recommendations,
ratings, download counts, update controls, and publisher verification remain
unsupported and must not be approximated. Local package installation is exposed
through the Extensions ViewContainer title menu as `Install from .senp...`.

Unlike VS Code's product-owned built-in extensions, SENP uses `builtIn` only for
the embedded source and integrity trust class. Sakura Editor NEXT intentionally
allows a user to uninstall a SENP built-in from the active profile. The embedded
package remains in the executable so the same catalog row can reinstall it
without a marketplace or network connection.

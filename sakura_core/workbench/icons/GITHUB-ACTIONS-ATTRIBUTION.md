# GitHub Actions status icon attribution

Sakura Editor NEXT draws the run, job and step status icons of the GitHub
Actions extension for Visual Studio Code in its Tree Views. The geometry and
colours are imported from:

- Project: [github/vscode-github-actions](https://github.com/github/vscode-github-actions)
- Pinned commit: `45e962b4439e6476d67937206566c0470743c660`
- License: MIT (reproduced below)

Nothing is loaded from that package at run time. The SVG path data is compiled
into `GitHubActionsStatusIcons.h` and drawn with GDI paths.

## Imported icons

Upstream ships one SVG per colour theme kind. Sakura names each icon by the
theme-independent path the extension's tree items use, and picks the light or
dark colours from the active colour theme kind.

| Local icon path | Upstream light SVG | Upstream dark SVG |
| --- | --- | --- |
| `resources/icons/workflowruns/wr_success.svg` | [light](https://github.com/github/vscode-github-actions/blob/45e962b4439e6476d67937206566c0470743c660/resources/icons/light/workflowruns/wr_success.svg) | [dark](https://github.com/github/vscode-github-actions/blob/45e962b4439e6476d67937206566c0470743c660/resources/icons/dark/workflowruns/wr_success.svg) |
| `resources/icons/workflowruns/wr_failure.svg` | [light](https://github.com/github/vscode-github-actions/blob/45e962b4439e6476d67937206566c0470743c660/resources/icons/light/workflowruns/wr_failure.svg) | [dark](https://github.com/github/vscode-github-actions/blob/45e962b4439e6476d67937206566c0470743c660/resources/icons/dark/workflowruns/wr_failure.svg) |
| `resources/icons/workflowruns/wr_skipped.svg` | [light](https://github.com/github/vscode-github-actions/blob/45e962b4439e6476d67937206566c0470743c660/resources/icons/light/workflowruns/wr_skipped.svg) | [dark](https://github.com/github/vscode-github-actions/blob/45e962b4439e6476d67937206566c0470743c660/resources/icons/dark/workflowruns/wr_skipped.svg) |
| `resources/icons/workflowruns/wr_cancelled.svg` | [light](https://github.com/github/vscode-github-actions/blob/45e962b4439e6476d67937206566c0470743c660/resources/icons/light/workflowruns/wr_cancelled.svg) | [dark](https://github.com/github/vscode-github-actions/blob/45e962b4439e6476d67937206566c0470743c660/resources/icons/dark/workflowruns/wr_cancelled.svg) |
| `resources/icons/workflowruns/wr_pending.svg` | [light](https://github.com/github/vscode-github-actions/blob/45e962b4439e6476d67937206566c0470743c660/resources/icons/light/workflowruns/wr_pending.svg) | [dark](https://github.com/github/vscode-github-actions/blob/45e962b4439e6476d67937206566c0470743c660/resources/icons/dark/workflowruns/wr_pending.svg) |
| `resources/icons/workflowruns/wr_queued.svg` | [light](https://github.com/github/vscode-github-actions/blob/45e962b4439e6476d67937206566c0470743c660/resources/icons/light/workflowruns/wr_queued.svg) | [dark](https://github.com/github/vscode-github-actions/blob/45e962b4439e6476d67937206566c0470743c660/resources/icons/dark/workflowruns/wr_queued.svg) |
| `resources/icons/workflowruns/wr_waiting.svg` | [light](https://github.com/github/vscode-github-actions/blob/45e962b4439e6476d67937206566c0470743c660/resources/icons/light/workflowruns/wr_waiting.svg) | [dark](https://github.com/github/vscode-github-actions/blob/45e962b4439e6476d67937206566c0470743c660/resources/icons/dark/workflowruns/wr_waiting.svg) |
| `resources/icons/workflowruns/wr_inprogress.svg` | [light](https://github.com/github/vscode-github-actions/blob/45e962b4439e6476d67937206566c0470743c660/resources/icons/light/workflowruns/wr_inprogress.svg) | [dark](https://github.com/github/vscode-github-actions/blob/45e962b4439e6476d67937206566c0470743c660/resources/icons/dark/workflowruns/wr_inprogress.svg) |
| `resources/icons/steps/step_success.svg` | [light](https://github.com/github/vscode-github-actions/blob/45e962b4439e6476d67937206566c0470743c660/resources/icons/light/steps/step_success.svg) | [dark](https://github.com/github/vscode-github-actions/blob/45e962b4439e6476d67937206566c0470743c660/resources/icons/dark/steps/step_success.svg) |
| `resources/icons/steps/step_failure.svg` | [light](https://github.com/github/vscode-github-actions/blob/45e962b4439e6476d67937206566c0470743c660/resources/icons/light/steps/step_failure.svg) | [dark](https://github.com/github/vscode-github-actions/blob/45e962b4439e6476d67937206566c0470743c660/resources/icons/dark/steps/step_failure.svg) |
| `resources/icons/steps/step_skipped.svg` | [light](https://github.com/github/vscode-github-actions/blob/45e962b4439e6476d67937206566c0470743c660/resources/icons/light/steps/step_skipped.svg) | [dark](https://github.com/github/vscode-github-actions/blob/45e962b4439e6476d67937206566c0470743c660/resources/icons/dark/steps/step_skipped.svg) |
| `resources/icons/steps/step_cancelled.svg` | [light](https://github.com/github/vscode-github-actions/blob/45e962b4439e6476d67937206566c0470743c660/resources/icons/light/steps/step_cancelled.svg) | [dark](https://github.com/github/vscode-github-actions/blob/45e962b4439e6476d67937206566c0470743c660/resources/icons/dark/steps/step_cancelled.svg) |
| `resources/icons/steps/step_queued.svg` | [light](https://github.com/github/vscode-github-actions/blob/45e962b4439e6476d67937206566c0470743c660/resources/icons/light/steps/step_queued.svg) | [dark](https://github.com/github/vscode-github-actions/blob/45e962b4439e6476d67937206566c0470743c660/resources/icons/dark/steps/step_queued.svg) |
| `resources/icons/steps/step_inprogress.svg` | [light](https://github.com/github/vscode-github-actions/blob/45e962b4439e6476d67937206566c0470743c660/resources/icons/light/steps/step_inprogress.svg) | [dark](https://github.com/github/vscode-github-actions/blob/45e962b4439e6476d67937206566c0470743c660/resources/icons/dark/steps/step_inprogress.svg) |

The Activity Bar icon of the `github-actions` ViewContainer is compiled into
`GitHubActionsContainerIcon.h`. Its local name is the path upstream's
`package.json` gives as the container icon; the light and dark files are
identical.

| Local icon path | Upstream light SVG | Upstream dark SVG |
| --- | --- | --- |
| `resources/icons/light/explorer.svg` | [light](https://github.com/github/vscode-github-actions/blob/45e962b4439e6476d67937206566c0470743c660/resources/icons/light/explorer.svg) | [dark](https://github.com/github/vscode-github-actions/blob/45e962b4439e6476d67937206566c0470743c660/resources/icons/dark/explorer.svg) |

The upstream `wr_warning.svg`, `step_warning.svg` and light-only
`step_pending.svg` are not imported because no upstream tree node resolves to
them.

## Local adaptations

- Paths are copied verbatim, except that each `<circle>` element is rewritten
  as four cubic Bezier segments (the path reader accepts absolute
  `M`/`L`/`H`/`V`/`C`/`Z` commands only), and every coordinate is scaled by
  2000 into GDI's integer path space.
- Upstream `clipPath` wrappers that clip to the full view box are dropped.
- The in-progress spinner is drawn at rest. Upstream rotates its arc with a
  CSS animation, and a Win32 Tree View row has no animation clock; animating it
  would need a per-row repaint timer that the Tree View rules forbid.
- The spinner ring's `fill-opacity="0.5"` is drawn with a constant-alpha
  `AlphaBlend` of 128 over the existing row background.
- One local icon path stands for the upstream light and dark pair. Layers that
  exist in only one of them (the two `step_queued` rings of different radii)
  are drawn only for that colour theme kind.
- Colours are copied from each SVG's `fill` attributes. They are not remapped
  to theme tokens, matching upstream, where the SVGs are fixed per theme kind.
- `explorer.svg` has no fill. Its elliptical-arc corners are rewritten as
  cubic Bezier segments, and it is filled with the Activity Bar's icon
  foreground, matching VS Code, which uses an extension's container icon as a
  mask over that colour.

## MIT License

```text
MIT License

Copyright GitHub

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

# Beer CSS (vendored, no source file here)

The UI's layout, components and Material 3 tokens are
[Beer CSS](https://www.beercss.com), used unmodified.

| | |
|---|---|
| version | 5.0.3 |
| licence | MIT (`LICENSE.txt` here) |
| source | https://github.com/beercss/beercss |
| sha256 of the script | `992306080b2ad5d0f21a812b473d3d8b26fe5b9a731e29035853d79a766f640f` |
| sha256 of the stylesheet | `ec87eb0a3a8e8fd3430ce6303474af902d094521cf6edd0ae9744358be7be9b4` |

**Why there is no code in this directory.** The device serves one file. The
page build splices Beer CSS's script and stylesheet directly into `ui.html`,
so the shipped artifact is the only copy and there is no second one to keep in
step. The stylesheet is the upstream sheet with unused component rules
dropped; no rule is rewritten, and nothing is added to it.

The licence text sits here rather than beside a file because there is no file
to sit beside. MIT asks that the notice travel with the software, and the
software travels inside `ui.html`.

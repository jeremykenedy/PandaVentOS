# Heroicons (vendored as SVG paths, no source file here)

Every interface icon is [Heroicons](https://heroicons.com) outline, 24px grid,
1.5 stroke, by Tailwind Labs. The path data is used unmodified.

| | |
|---|---|
| licence | MIT (`LICENSE.txt` here) |
| source | https://github.com/tailwindlabs/heroicons |
| set | outline, 24x24 |
| sha256 of the sprite | `5e77138bd61fb3a0c697adcd20f820a5ea47e38d69086fcc172e53bc6884469e` |

**What is and is not Heroicons.** `sprite.svg` carries 1 symbols. The
interface icons are Heroicons paths, copied as-is into `<symbol>` elements so
one inline sprite replaces 1 network requests the device could not serve
anyway. The 3D-printing icons in the same sprite -- the nozzle, the spool, the
vent -- are **not** Heroicons: they were drawn for this project on a 96 grid
with a 4 stroke, and they are covered by this repository's own licence.

The sprite is spliced into `ui.html` by the page build, which is why no SVG
file is stored in this directory.

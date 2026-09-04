# Heroicons (vendored as SVG paths, no source file here)

Every interface icon is [Heroicons](https://heroicons.com) outline, 24px grid,
1.5 stroke, by Tailwind Labs. The path data is used unmodified.

| | |
|---|---|
| licence | MIT (`LICENSE.txt` here) |
| source | https://github.com/tailwindlabs/heroicons |
| set | outline, 24x24 |
| sha256 of the sprite | `5e77138bd61fb3a0c697adcd20f820a5ea47e38d69086fcc172e53bc6884469e` |

**What is and is not Heroicons.** The sprite carries 51 symbols. 43 of them
are Heroicons outline paths on the 24x24 grid at 1.5 stroke, copied as-is into
`<symbol>` elements so one inline sprite replaces 51 network requests the
device could not serve anyway.

The other 8 are **not** Heroicons. They were drawn for this project on a 96
grid with a 6 stroke, and they are covered by this repository's own licence:

    i-bed-hot   i-material   i-materials         i-nozzle
    i-nozzle-temp   i-printer-enclosed   i-printer-open   i-spool-end

There is no `sprite.svg` file in the published tree: the sprite is assembled
by the page build and spliced into `ui.html`, which is why the hash above
fingerprints the block inside that page rather than a file of its own.

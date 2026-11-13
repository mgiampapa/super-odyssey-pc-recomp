# MacNativeGames extraction scripts

Copies from https://github.com/ly05010419/MacNativeGames at `b75be8cfa704df3701e86130c8ced268e79d6b11`, MIT (see
`LICENSE`), by way of TotK PC Recomp (https://github.com/valentderah/totk-pc-recomp, MIT).

| File | Upstream path |
|---|---|
| `extract_exefs.py` | `games/totk/scripts/extract_exefs.py` |
| `extract_romfs.py` | `games/totk/scripts/extract_romfs.py` |
| `extract_update_romfs.py` | `games/totk/scripts/extract_update_romfs.py` |
| `extract_shared_fonts.py` | `games/totk/scripts/extract_shared_fonts.py` |

The one change: `extract_exefs.py` checks Super Mario Odyssey's title IDs (base `0100000000010000`, update
`0100000000010800`) instead of Tears of the Kingdom's.

They're kept as the reference `runtime/nxpkg.c` was ported from, and to compare its output with
`build/tests/test_nxpkg.exe`'s (`tools/test_nxpkg.c`) during development.

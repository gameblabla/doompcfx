# Doom PC-FX port

This is the source code for Doom PC-FX port. Game assets, disc images, music, all
generated folders and payloads, build output, emulator binaries, and tool
binaries are intentionally omitted.

## Requirements

- V810 GCC toolchain (the Makefile defaults to `/opt/v810-gcc`)
- `libpcfx` built beside this directory (`../libpcfx`), or `LIBPCFX` set to its location
- Python 3 and a native C compiler for the included CD linker
- A legally obtained Doom IWAD and any optional art/music supplied separately

## Build

Build with an externally supplied IWAD, for example:

```sh
make DOOM1WAD=/path/to/doom1.wad
```

The Makefile's asset-generation rules create the omitted generated source and
binary payloads from the supplied IWAD/assets. Build products are not kept here.

## Notes

I should note that this port was heavily AI-asssisted.

For context, a previous "human" made port was attempted here :

https://github.com/gameblabla/doom-pcfx

This attempt did not even boot ingame, it was quickly abandoned as i realized the difficulty of it.
As it turns out, even the initial C-based build only ran at 5 fps. Yikes.

I understand now why 3DO Doom was so difficult, the walls in particular destroy performance (they consume more than 30ms of CPU work), followed by the floors.
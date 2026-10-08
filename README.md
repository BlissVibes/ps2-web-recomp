# PS2 Web Recomp

PlayStation 2 games running in the browser without an emulator. The game's Emotion Engine machine code is translated ahead of time into C++, compiled to WebAssembly, and linked against a reimplementation of the PS2's operating system and hardware.

This is the sibling of [psp-web-recomp](https://github.com/snuri00/psp-web-recomp) and follows the same method. The first title it targets is Gran Turismo 4, chosen because it is one of the most demanding games on the system: it leans on both vector units, on exact floating point behaviour for its replays and license tests, and on graphics tricks that read rendered images back as palettes.

The project is at an early stage. What exists so far is listed below and grows with every step.

## Layout

`scripts/` holds the tools that prepare a game: `disc.py` reads PS2 DVD images, including the second layer of dual layer discs, and `gt4_core.py` turns Gran Turismo 4's packed main program into an ordinary ELF file.

## Your game files

No game data is included here, and none may be added. You bring a disc image of a game you own and the scripts turn it into a build on your machine. Everything derived from a game, including extracted files, generated C++ and the compiled output, stays local; `.gitignore` keeps `games/`, `generated/` and `build/` out of the repository.

Gran Turismo and PlayStation are trademarks of Sony Interactive Entertainment. This project is not affiliated with or endorsed by Sony, Polyphony Digital or any game publisher.

## License

MIT, see [LICENSE](LICENSE).

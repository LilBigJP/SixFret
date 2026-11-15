# Six Fret

**Six Fret** is an unofficial native PC port of **Guitar Hero Live** (PlayStation 4, Activision / FreeStyleGames,
2015) for Windows 10 and 11. It is a personal project, for playing a copy of the game you own.

> **No game files are included.** You need a dump of your own PS4 copy: the folder with `eboot.bin`, `sce_module`
> and `sce_sys` (tested: CUSA02410, version 01.08). This project is not affiliated with or endorsed by Activision,
> FreeStyleGames or Sony Interactive Entertainment. Guitar Hero is a trademark of Activision Publishing, Inc.

## How it works

The PS4 runs x86-64 code, so the game's own executable runs directly on the PC's processor: nothing is emulated on
the CPU side. A runtime replaces the PS4 system libraries the game calls (memory, threads, files, audio, video
decoding, the guitar, saves, dialogs, network, PSN), and the PS4 graphics commands and shaders are translated to
Vulkan by the [shadPS4](https://github.com/shadps4-emu/shadPS4) video core.

The port is built on [Minus Two](https://github.com/gminustwo/minustwo), an earlier native PS4 port, which is built on
[SpartanPort](https://github.com/FelipePrado0/SpartanPort), the God of War III port (see [Credits](#credits)).

| Part | Where | What it does |
|---|---|---|
| Preparation | `scripts/` | Reads `eboot.bin`, applies its relocations and links the game's own `libc`, `libSceFios2` and `sku.prx` into one image |
| Loader | `src/probe.c` | Loads the image, binds its imports to the runtime and starts the game's original code |
| Runtime | `src/runtime_*.c` | The PS4 system functions |
| GPU | `gpu/shadps4/` | The shadPS4 video core: PS4 GPU commands and shaders to Vulkan |
| In-game menu | `gpu/shim/` | Dear ImGui menu (Insert, or R3 + L2) with display, performance and overlay settings |
| Launcher | `launcher/sixfret_launcher.py` | Game folder, settings, controls, achievements and the Play button |

### What this port adds for Guitar Hero Live

- **The guitar** (`src/runtime_pad.c`): the game opens its wireless receiver as a "special pad" by USB id
  (1430:07BB), requires the device to report the guitar class, and reads the six frets, the strum bar, the direction
  knob and the Hero Power and GHTV buttons from fixed bits of the pad data. The port provides that device, played
  from the keyboard or any gamepad. This is what the game waits for on its title screen.
- **libSceVideodec2** on FFmpeg (`src/runtime_videodec2.c`): the logo movies and the live-action footage behind
  the highway.
- **BSD sockets** on the host's network (`src/runtime_socket.c`): the game's Demonware layer opens a UDP socket
  while it starts and does not go on without one.
- **A start-up fix**: the game's PSN toolkit thread signals "ready" to a creator that, on a PC, is not yet waiting
  for it; new threads start a moment after they are created (`src/runtime_thread.c`).
- A silent microphone port (the game otherwise retries opening one without a pause), `sku.prx` linked into the
  image, and offline answers for the PSN, dialog and store functions the game imports (`src/runtime_ghl.c`).
- **Achievements**: the game's 50 trophies with their names and descriptions, in the launcher and in-game.

## What works

Tested on Windows 11 with a Vulkan 1.3 GPU (GeForce RTX 5060 Ti), game version 01.08:

- Start-up, the logo movies with sound, the title screen, the first-time setup and saving.
- The tutorial: the highway, notes, countdown and live-action video, at a steady 60 FPS.

### Not done yet

- **Guitar Hero TV**: the servers closed in 2018. Sockets work; HTTP requests are not sent yet, so GHTV reports
  that it cannot connect.
- A real Guitar Hero Live guitar and receiver are not read yet: the guitar is the keyboard or a gamepad.
- Only the tutorial has been played so far; whole songs, scoring and audio sync are not verified.
- On the first screen ("Connect your guitar controller") the shadow layer the game draws under each card and
  caption shows as a grey, offset copy. It is on that screen only and does not affect play.
- Linux and macOS builds are inherited from Minus Two and untested with this game.

## Playing

1. Build the port (below) and run `python launcher/sixfret_launcher.py`. A packaged build has `SixFret.exe`,
   the same launcher (`bash packaging/windows/package.sh` makes it in `dist/`).
2. Choose your Guitar Hero Live folder (the one with `eboot.bin`). The dump must be complete: the `video` and
   `tracks` folders hold the movies and songs.
3. **PLAY**. The first start prepares the game image; shaders are compiled the first time they are needed.

Saves and the shader cache are in the `user` folder next to the port.

### Controls

| Key | Guitar |
|---|---|
| A S D | black frets 1-3 |
| Z X C | white frets 1-3 |
| Up / Down, Right Shift | strum |
| Left / Right | whammy bar |
| I J K L | direction knob (menus) |
| Return | pause |
| Space | Hero Power |
| G | GHTV button |
| Insert | the port's menu |

On a gamepad: Cross, Circle, Triangle are the black frets, Square, L1, R1 the white ones, the left stick strums.

## Building from source

Windows, with [MSYS2](https://www.msys2.org) and its **CLANG64** environment:

```bash
pacman -S --needed git patch \
  mingw-w64-clang-x86_64-{toolchain,cmake,ninja,pkgconf,python} \
  mingw-w64-clang-x86_64-{vulkan-headers,vulkan-loader,vulkan-memory-allocator,glslang} \
  mingw-w64-clang-x86_64-{sdl3,fmt,boost,robin-map,xxhash,ffmpeg,zydis,freetype} \
  mingw-w64-clang-x86_64-{spirv-tools,spirv-headers,spirv-cross}
git submodule update --init --recursive
bash build.sh                       # out/sixfret.exe
```

Then, from a plain Windows Python:

```powershell
$env:SF_GAME_DIR = 'D:\Games\CUSA02410'
python run.py
```

## Tools for developers

- `python tools/testrun.py --game <folder> --seconds 20` starts the game and ends it after the given time
  (the loader has no watchdog on Windows); `--trace sceNp,scePad` logs those imports with a number per thread.
- `python tools/autoplay.py --game <folder> --seconds 120 --press 42:cross --hold 60-61:ly=0` plays unattended
  with scripted input and a contact sheet of screenshots.
- `python tools/re_imports.py scePadOpenExt` shows where the game calls a system function, with the code around it.
- `SF_FILE_TRACE=1` logs every file the game opens or looks for (it checks an `Override` folder before each data
  file), `SF_SOCKET_TRACE=1` its sockets, `SF_VIDEODEC_TRACE=1` the video decoder, `SF_GUITAR=0` starts without a
  guitar, `SF_FRAME_STATS=1` prints frame rate and pacing.
- `python tools/gen_trophies.py` writes the trophy texts from `tools/trophies.txt` into the runtime and launcher.

## Credits

- **Minus Two** — the PS4 port this one is built from: the runtime, the launcher and the tools.
- **SpartanPort** by Felipe Prado — the God of War III port Six Fret is built from; itself a fork of
  **Supermedo**'s Windows port of **deadinside28**'s [bloodborne_pc](https://github.com/deadinside28/bloodborne_pc).
- **shadPS4 team** — the [shadPS4](https://github.com/shadps4-emu/shadPS4) video core, and the reference for the
  PS4 libraries' behaviour (NID tables, Videodec2).
- **cuesta4** — the shadPS4 rendering fixes SpartanPort carries.
- [LibAtrac9](https://github.com/Thealexbarney/LibAtrac9), [FreeType](https://freetype.org),
  [FFmpeg](https://ffmpeg.org), [SDL3](https://github.com/libsdl-org/SDL), [Dear ImGui](https://github.com/ocornut/imgui),
  [sirit](https://github.com/shadps4-emu/sirit), [magic_enum](https://github.com/Neargye/magic_enum),
  [miniz](https://github.com/richgel999/miniz), [xbyak](https://github.com/herumi/xbyak),
  [Vulkan Memory Allocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator),
  [FSR-Vulkan](https://github.com/FireBurn/FSR-Vulkan), [MSYS2](https://www.msys2.org) and [LLVM](https://llvm.org).

## License

GNU GPL v2 or later ([LICENSE](LICENSE)), as Minus Two and SpartanPort. Third-party components keep their own
licenses.

## AI disclosure

This port was made with an AI coding assistant (Claude), working from Minus Two's source, shadPS4's library
references and the game's own behaviour observed in test runs.

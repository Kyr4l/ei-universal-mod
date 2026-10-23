# Rebuilding game.exe from the reversed source (`game-src/`)

Goal: recreate the Russian `game.exe` (md5 `6b6db6a470eed909b5e26018653cc07e`) from source that comes out of the reverse-engineering work in this folder.
Two stages exist; both are reproducible with the scripts in `game-src/tools/`.

## Stage A - exact source (asm + data), byte-identical
`tools/gen_asm.py` + `tools/make_game.py` turn the original binary into GNU-as source (`game-src/asm/a*.s`, 49 files, one `.org` per instruction, branch targets as
`T_xxxxxxxx` labels, every function a `f_xxxxxxxx` global label) plus data blobs (`game-src/data/*.bin`: PE header, `.rdata`, `.data`, `.rsrc`, overlay).
`make_game.py` assembles, links with a linker script that pins each section part to its original offset, and re-creates the PE.
Result: `build/game_rebuilt.exe`, **md5-identical** to the original, runs to the Universal Mod main menu under Wine.

## Stage B - hybrid: lifted C replaces asm bodies
`tools/lift.py` / `liblift.py` turn the Ghidra decompiles (`_cpr/claude-re/dec2`) into compilable C (`game-src/src/m*.c`, 7,383 functions have C source).
`tools/hybrid.py` compiles the selected C functions (MinGW, `-O1 -m32 -mno-sse -mfpmath=387`), emits the asm source with the selected functions reduced to a
`jmp f_X` stub, links everything (C objects go to a new `.rbld` section at 0x7D8000) and writes `build/game_hybrid.exe`.

### How a function is accepted
1. random-input differential test against the original asm (`tools/gen_tests.py`, `runtests.py`): categories verified / weak / mismatch / crash / untestable
2. in-situ test: the exe with the function replaced must boot under Wine to the main menu and render the menu signpost identically to the original
   (`tools/trial.sh`, driven by `tools/findbad.py` which bisects the set of replaced functions and denies the culprit)

### Lift bug classes found by the in-situ bisect (each has a fix pass over `src/m*.c`)
| class | symptom | fix |
|---|---|---|
| float expression passed to an `undefined4` parameter | C converts the value instead of passing the bits | `fixcalls.py` wraps call arguments in `EI_ARG()` |
| `void`-typed function whose asm returns `this` in eax | callers read garbage | `fixvoid.py` returns `this` (507 functions) |
| hidden register parameter (edx / ecx not in Ghidra's prototype) | callee reads garbage | `regs.py` finds them, callers and callee are denied |
| one buffer split into several locals | gcc reorders them, a callee overruns | `fixlocals.py` puts the frame locals into one packed struct at the original offsets, `fixtail.py` pads it |
| indirect call through a register keeps ecx | `this` lost (`CallN`) | denied |
| Ghidra value casts `(int)f` / `(float)i` that are really raw dword moves | wrong numbers | `fixcasts.py`: bit reinterpretation where the asm has no fild/fistp, or in bit-operator statements |
| globals typed `undefined4` but only used as x87 floats | integer conversion of a float | `globtypes.py` + `build.py ctype()` declare them float/double, `fixglobals.py` fixes integer-style accesses |
| imports named `_AIL_xxx_8` have no header | C assumes cdecl, stack popped twice | declared `stdcall` in the generated `proto.h` |
| replaced function contains a referenced interior entry point | the entry is wiped | `interior.py` |
| the Universal Mod patches bytes inside a replaced function | `PatchVerify` fails, the mod misbehaves, loading hangs | functions containing patch sites are denied (`build/deny_modpatch.txt`) |

### Running it (do not run heavy jobs bare)
`tools/safe.sh <cmd>` runs a command in a systemd scope with a memory cap, CPU quota, low priority and a task limit. The build is capped at 2-3 workers.
Needs a hidden X server on `:97` (`Xvfb :97`), a private clone of the Wine prefix (`tools/smoke.sh` creates `/tmp/eipfx`, nothing is written to the game install),
and a silent audio sink (created automatically: `pactl load-module module-null-sink sink_name=eitest_null`).

## Status
See the numbers at the end of `game-src/STATUS.md` (kept up to date by hand after each bisect run).

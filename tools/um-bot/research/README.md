# um-bot research harnesses

Offline replays of captured server streams (`UM_BOT_DUMP=<dir> um-bot --connect-test` writes every decompressed stream message as a file). They include `../net.hpp` with
`private` made public and run its decoder over the files:

- `streamtest.cpp`  every unit creation record (0x40) found: id, kind, figure, name, position, side, enemy mask (`DUMPF=1` also dumps the decoded fields)
- `endcheck.cpp`    how many creation records end exactly where the next sub-message starts (`SHOW=1` lists the ones that do not)
- `coverage.cpp`    units that got 0x41 updates but no decoded creation
- `watchid.cpp`     replays the files and prints every change of one unit's decoded fields

Build (from tools/um-bot): `g++ -std=c++17 -O1 -I. -I../um-multitool research/streamtest.cpp -o /tmp/streamtest`. The live test rig that produces ground truth (um.dll's UNITS) is in `game-src/tools/`.

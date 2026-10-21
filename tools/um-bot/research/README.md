# um-bot research harnesses

Offline replays of captured server streams (`UM_BOT_DUMP=<dir> um-bot --connect-test` writes every decompressed stream message as a file). They include `../net.hpp` with
`private` made public and run its decoder over the files:

- `streamtest.cpp`  every unit creation record (0x40) found: id, kind, figure, name, position, side, enemy mask (`DUMPF=1` also dumps the decoded fields)
- `endcheck.cpp`    how many creation records end exactly where the next sub-message starts (`SHOW=1` lists the ones that do not)
- `coverage.cpp`    units that got 0x41 updates but no decoded creation
- `endstats.cpp`    what follows each decoded 0x41 update (known unit / unknown id): the basis of the update acceptance rule in `ReadUnits`
- `poscheck.cpp`   every raw decoded position against the true ones (`gt.txt` lines `time id x y` sampled from um.dll's UNITS while `UM_BOT_DUMP` was written), with the section it came from
- `watchid.cpp`     replays the files and prints every change of one unit's decoded fields

Build (from tools/um-bot): `g++ -std=c++17 -O1 -I. -I../um-multitool research/streamtest.cpp -o /tmp/streamtest`. The live test rig that produces ground truth (um.dll's UNITS) is in `game-src/tools/`.

## Research switches of `um-bot --connect-test`

Environment variables, read only by the test mode (a normal run never sets them): `UM_BOT_FOLLOW=1` follow the host, `UM_BOT_FIGHT=<0|1|2>` fight (engagement), `UM_BOT_SIGHT=<units>` pretend a longer sight,
`UM_BOT_ATTACK=<distance>` walk to that distance of the nearest hostile and attack it (retries), `UM_BOT_MOVE="x,y;x,y"` walk there (one per 8 s), `UM_BOT_USE="slot,secs"` use a belt slot on itself,
`UM_BOT_CASTON="spell,secs,self|host"` cast a spell object id, `UM_BOT_HEAL=<%>` heal-below threshold, `UM_BOT_LEAVE="secs[,rejoin]"` leave the zone (enter again later), `UM_BOT_MAP="secs,name,entry"` send
a change-map message, `UM_BOT_STAY=<secs>` how long to stay, `UM_BOT_DUMP=<dir>` write every stream message, `UM_BOT_RXLOG=1` log the received chunks (noisy: set only when needed).

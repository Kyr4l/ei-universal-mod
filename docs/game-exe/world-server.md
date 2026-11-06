# The world (server side) and the game session in game.exe (CWorld, CWorldServer, Session_*, MpFile, resources)

Written 2026-10-27 from the typed Ghidra decompiles of the Russian `EIStarter OBT-1/Engine/game.exe`: the CWorld base
(0x55E000-0x560200), the session driver and save/load glue (0x579000-0x58A000), CWorldServer (0x58A000-0x598000 plus the
container instantiations up to 0x59F000), the resource readers (0x674000-0x676000) and the per-frame loop of `main` (0x456440).
The rendering half (CWorldClient 0x5612F0-0x579000, CBridgeManager, CWeather, the sky and day-light tables) and the players
(CPlayer / CPlayerServer / CPlayerClient 0x664000-0x674000) are read on their own pages; here they appear only where the server
calls into them (the message names of `CPlayerServer::SendMsgXX`).

Tags: **[V]** read directly in the decompile, **[I]** inferred from several functions (strong), **[?]** guess / not resolved.
Addresses are VAs; offsets are `this+off`; "tick" = one game tick (55 ms at normal speed); g_ = a global.

## How to re-implement it (short version)

1. **Two world objects, one process.** `CWorldServer g_0x7AFF60` simulates, `CWorldClient g_0x7AF5B8` renders; in single
   player they talk through the same in-memory connection as a network game (`CWorldClient::PlayRecordedStream` 0x566CE0 gives the
   local client a `FileMsgStream` connection; the server treats it like a TCP peer). Both derive from `CWorld` (0x84 bytes: the
   nine managers, the clock, the wind, the water levels). [V]
2. **Fixed-step ticks, decoupled from frames.** `main` accumulates real ms (at most 5 ticks per frame) and runs
   `CWorldServer::Tick` 0x58DFF0 once per `g_0x78FF34` ms (55; 27 at x2), then `CWorldClient::Update` 0x5641F0 on its own
   accumulator, then `Session_ProcessRequest` 0x57C700 (pending session requests only), then UI + render. `Tick` replicates every
   slice; the game clock (`CWorldServer +0x64`, `g_0x7AFFC4`) and the simulation (`CWorldServer::Update` 0x58DCD0) advance
   only while `+0x6c paused == 0`. [V]
3. **One flat object world.** Every object is in the id hash `+0x12d70` (`FindObjectById`); the ones that tick, save and
   replicate are in `+0x12da0` (dynamic); the static scenery moved out by `SeparateStaticObjects` 0x5951A0 sits in `+0x12d88`;
   named objects in `+0x12dd0`; effects in `+0x12de8`. In a playable zone (`+0x12d64 zoneKind == 1`, the global `g_0x7C2CC4`)
   only the "active set" `+0x104` ticks: units are woken by `ActivateObjectsAround` (every tick around the player's units,
   sight-based radius) and fall asleep when their countdown `+0x174` (128..143 ticks) runs out with an idle brain; in the menu,
   camp and scripted zones every dynamic object ticks. [V]
4. **Objects come from descriptors.** The `.mob` parser (`MobFile_Load` 0x481660) builds `CObjectDesc` / `CUnitDesc` / lever /
   trap descriptors and `CreateObjectFromDesc` 0x5927C0 turns the `kind +0x48` (1 object, 2 light, 3 sound / particle, 4 effect,
   5 unit, 6 campfire, 7 trap, 8 lever, 9 active object) into a manager creation (`type +0x50` = the 0x3xxx key) plus the hash
   registrations. The same function spawns the party's heroes (`SpawnPartyHeroes` builds a `CUnitDesc` per `HeroRecord`). [V]
5. **Zone change = unload, load, spawn.** `Game_EnterZone` 0x57C460 -> `Session_EnterZoneFromParty` 0x57A060: first visit ->
   `LoadZone(mpr, mob)` (CAIMap from the `.mpr`, objects from the `.mob`, clock 4:00); return visit -> `saves\current\<zone>.sav`
   through `CWorldServer::LoadSave` 0x596E70 and the zone is **simulated for 100 ticks per game hour away** (at most 1000)
   around the entrance before the player appears. The heroes enter through `CWorldServer::PlayerIn` 0x590700 when the
   client reports ready (msg 0x09) and leave through `PlayerOut` 0x590A40 (items and stats written back to the `HeroRecord`s, the
   units stay in the zone ownerless or are removed when dead). Leaving writes `<zone>.sav` (`Session_SaveZoneToCurrent` 0x57A780). [V]
6. **A save slot is a folder copy.** `saves\current\` holds one `<zone>.sav` per visited zone (party variable `zs.<zone>` = 1,
   `zt.<zone>` = last-visit gtime); `Session_SaveToSlot` 0x57B3B0 copies those of the current allod (plus bz7g) into
   `saves\<name>\`, writes `scenario.sav` (party store + UI + camera) and `mission.sav` (the live zone, `WriteSave` 0x597BD0);
   `Session_LoadSaveSlot` 0x57ABA0 does the reverse. All compressed files share the `MpFile` container (0x579830 / 0x579B80: head word, version 0x74, checksum, LZ). [V]
7. **Replication is view-point and senses based.** Per tick and per player `ReplicateToPlayer` 0x58B750 sends, in a playable
   zone, the non-unit dynamic objects within 100 m of the player's view point (`CPlayerServer +0x38`, the client's msg 0x40), the
   player's visible units, the named objects and the units within twice the sight / life-sense range (+5) of each party unit
   (other zones: by distance only), that the player does not know yet (msg 0x40 creation, 0x41
   figure update, 0x42 untrack), and the effects within their radius (0x50 full / 0x51 delta / 0x52 remove); world state goes by
   flags on `CPlayer +0x130` (0x02 zone state blob, 0x70 rain, 0x71 time, 0x72 wind, 0x73 water levels). [V]
8. **Stats are XOR-scrambled only while the window is inactive** (`World_SetStatScramble` 0x5790E0, key `g_0x7AFECC`, flag
   `g_0x79026C`): an in-process tool reads plain values while the game is focused. [V]

## 1. Conventions

| Thing | Value |
|---|---|
| the server world | `CWorldServer g_0x7AFF60` (vtable 0x73F928, 42 slots); the client world `CWorldClient g_0x7AF5B8` (0x73F41C) |
| game tick | `CWorldServer +0x64` = `g_0x7AFFC4` (client copy `g_0x7AF61C`), ++ per `Update`; 55 ms (`g_0x78FF34` = 0x37; 0x1B at x2 speed, set by the UI 0x63A560 / 0x648D70 ..) [V] |
| frame tick | `CWorldServer +0x68` = `g_0x7AFFC8`: ++ per `Tick` even while paused; `CEffect::lastChangeTick` uses it [V] |
| paused | `CWorldServer +0x6c` = `g_0x7AFFCC` (byte; `SetPaused` 0x5942C0, `Game_SetServerPaused` 0x578D10); client `+0x6c` = `g_0x7AF624` (the Space pause the memory notes found) [V] |
| hour of day | `fmod(tick * +0x20 + +0x1c, 24)` [V] (x87: `fild +0x64; fmul +0x20; fadd +0x1c; fld 24.0; call _CIfmod`, `Game_GetDayHour` 0x5786E0 on the globals `g_0x7AFFC4 / 0x7AFF80 / 0x7AFF7C` = `g_0x7AFF60 +0x64 / +0x20 / +0x1c`; `SetTime(h)` stores `h - fmod(tick * +0x20, 24)`); `+0x20 = 1 / (15 * rate)`, rate = 60 in every save -> 900 ticks (49.5 s) per game hour, 19.8 min per day [V] |
| gtime | the campaign clock in game hours, a party variable (`CPlayer +0xc4` table) advanced by `BroadcastGameTime` 0x58D280 = `(tick - CPlayer +0x110) * +0x20` [V] |
| positions | metres, x / y / z floats as everywhere; the replication radius 100 m is `dx^2 + dy^2 < 10000` [V] |
| zone kind | `g_0x7C2CC4` (see 2.3); server mode `CWorldServer +0x138` = `g_0x7B0098` (see 2.3) |
| id hash | `{3 bytes, buckets begin / end / cap, count}` = 0x14 bytes, nodes `{next, id, obj}` (STLport hashtable, `IdHash_*` 0x59Bxxx); `*MaxId` fields keep the largest id ever inserted [V] |

## 2. The session

### 2.1 The frame loop (`main` 0x456440, lines 485-700) [V]

```
PeekMessage loop (messages -> App_WndProc / App_HandleInputMessage)
  dt = timeGetTime delta;  window inactive (render globals +0x10 bit 0 clear): World_SetStatScramble(0), no ticking, render only
  active: World_SetStatScramble(1); lobby bookkeeping (Lobby_Create / LobbySession_ReleaseActive2 when CWorldClient::NetObjectIdle)
          Net_TickAll; dt = min(dt, 5 * tickMs); (debug fixed step g_0x79BC54: dt = 1000 / g_0x79BCF8)
          g_0x79BC64 (server accumulator) += dt; g_0x79BC68 (client accumulator) from the client lag estimate
          loop:
            while server accumulator >= tickMs: CWorldServer::Tick (0 -> stop), accumulator -= tickMs, Net_TickAll
            if client accumulator >= tickMs and (CWorldClient::TickNetAndQuery6 or not connection state 2):
                 CWorldClient::Tick (0 -> stop), accumulator -= tickMs, CCamera::Update(tickMs / 55), Net_TickAll; repeat
          Session_ProcessRequest(g_0x7AFE20)
          client speed factor g_0x784758 (0.1 .. 10) from the lag estimate (CWorldClient::TickNetAndQuery7)
          CCamera::Update per remaining tickMs (+ keyframe / fly-path when not paused)
  CInterface::ProcessPendingScreens; App_RenderFrame
```

`CWorldServer::Tick` 0x58DFF0 (slot 6) does, in order: (1) per player: drop the player when its net stream `+0x1c` is null, the
stream's `vt[4]()` is false or `CPlayerServer vt[23]()` returns 0 (`CPlayer::ClearNetStream`, released through `vt[1]`, log
"Disconnecting player N"), else `ActivateObjectsAround(unit pos +0x1c, max(sight +0x904 * +0x294, life-sense +0x2a4) * 2 + 20 m)`
for every party unit (`CPlayer +0x40` hash; fields of the unit's `CUnitServer +0x240`); (2) the local holder `+0x5c`
(`vt[5]()` + `AcceptPlayers`) and the TCP driver `+0x60` (`CTcpNetDriver::SyncPeers(g_0x79B5DC)` only while the lobby server is
alive, then `AcceptPlayers`); (3) not paused: `CPlayer::PurgeSyncedItems` per player and `Update(0)`; (4)
`CPlayerServer::UpdateVisibleSet` per player; then the zone VM's "Heroes" group (`+0x1298c` groups, names `+0x129a0`, count
`+0x129a8`) is united with the visible array `+0x510c` of every observer (`CPlayerServer +0x24 < 0`), accumulating, and every
observer gets the whole union (all heroes plus what any observer sees); (5) `ReplicateToPlayer` per player (0 -> the same
disconnect); `+0x68` ++; returns 1. [V deep read 2026-11-06: the radius is the larger of the two ranges, the branch-free select
`(a < b) ? b : a` built from `fcomp` + `sar` at 0x58E0A5..0x58E0D0]

`CWorldServer::Update(force)` 0x58DCD0 = the game tick: `+0x64` ++; `(tick & 63) == 3` -> `UpdateEnvironment` 0x58CCD0; effects
`+0x12de8` whose `vt[6] Update` returns 0 are released; `StimulusMap::ExpireBucket(+0x2838, tick)`; `WaterLevels::Tick(+0x70)`
still animating -> flag 0x20 on every player; `CSpellEffect::Update` over the cast list `+0x12e2c` (finished casts freed);
`GSWorld::Tick(+0x12874)` when `+0x160` (the world script runs) or `force`; `GSWorld::Tick(+0x12a6c)` always;
`TickActiveObjects` 0x58D670 (see 4.3). [V]

### 2.2 Session requests (`SessionRequest g_0x7AFE20`, `Session_ProcessRequest` 0x57C700) [V]

`Session_ProcessRequest` is **not** a per-frame tick: it returns at once when the request kind `+0` is 0. The UI and the lobby
fill the request (`SessionRequest::SetLoadSave` 0x5816D0 kind 2, `SetEnterZone` 0x581700 kind 4, `SetLeaveToLobby` 0x581730
kind 5, `SetHostZone` 0x581760 kind 6, `Clear` 0x5816C0; `RequestMpMapChange` 0x57EE10 builds 5 / 6 from the lobby state) and the
next frame handles it:

| kind | what happens (after the common prologue) |
|---|---|
| prologue | server mode 1 with zone kind 1 / 3 and kind 4 (zone change): the party store's party is fetched, `Session_SaveZoneToCurrent(party)` (the zone is written before leaving, `zs.` / `zt.` set) and stored back; mode 2 with zone kind 1 and kind 4: the same save into a **fresh empty `CParty`** (the file is written, the two variables are thrown away: `Session_HostMpZone` only checks the file); `CInterface::CloseAllScreens`; `CWorldServer::UnloadZone`; database swap (`g_0x7AFDEC` "database.res" / "databaseLMP.res" by `Lobby_IsAlive`: `Databases_LoadAll` 0x57CB50 + the balance index `g_0x79BEC0` / `C4` swap) |
| 1 | main menu: `g_0x7C7D94 = 1`, `Game_EnterZone("MainMenu", 2)` |
| 2 | load a save slot `+4`: `CWorldClient::PlayRecordedStream(g_0x7AFFBC)`, connection `+0x85 = 0`, `SetMode(1)`, `CWorldClient::Update` + `CWorldServer::Tick` twice, `Session_LoadSaveSlot`; `+0x78` set -> `CPlayerClient::SendMsg02_RequestParty`, `g_0x7B007C = 1` |
| 3 | new game: `Console_ExecuteLine`, `Session_ResetPlayers_Single(g_0x7AA5A0)`, clear `saves\current`, `Game_EnterZone(party store zone, 0)` |
| 4 | enter the zone `+4` (`Game_EnterZone(name, 0)`) |
| 5 | leave: `CPlayer::SetParty(local player, +8)`, `UnloadZone`, `CWorldClient::ConnectToLobbyHost`; only when that fails: no lobby -> `Game_EnterZone("MainMenu", 2)`, else `Lobby_ResetMpGameState`; connection `+0x85 = 1` |
| 6 | host a multiplayer zone: `Session_HostMpZone(+4)`, `SetParty(+8)`, `PlayRecordedStream`, `+0x85 = 1` |
| end | kind `+0 = 0`, `Net_TickAll` [V deep read 2026-11-06] |

### 2.3 Zone kinds, server modes, zone records [V]

`Game_EnterZone(name, kind)` 0x57C460: "endofgame" -> kind 8; else the zone record (`ZoneRecord` 0x5c bytes in the MapInfo
`g_0x79BF00`, loaded from `map.txt` + `map_LMP.txt` by `MapInfo_Load` 0x45EA20); kind 0 is derived from the record's `+0x28`:
0 -> 1 (playable zone), 1 -> 4 (camp / map screen), 2 -> 3 (scripted zone); unknown zone -> FatalError "Zone %s not found".
Then `SetTimeScale(g_0x79BE5C)`; kind 2: `LoadZone("ZoneMainMenuNew.mpr", "ZoneMainMenuNew.mob", 0)` with the real wall clock as
the hour, server mode 4; kind 8: mode 1; else `Session_EnterZoneFromParty(party store party)`, `SetTime("gtime")`,
`SetWeatherMode(record +0x2c)`, `SetCaveFlag(record +0x30)` (0x58CC70; once called SetRestrictFlag), mode 1. `g_0x7C2CC4 = kind`, `SetMode(mode)`,
`CWorldClient::PlayRecordedStream(g_0x7AFFBC)`.

Which zones are which [I, from the map.txt copy in `extra-assets/unmade-maps/res/texts_res/`: 87 zones]: the 16 `brief` zones are
the allod bases `bz1g..bz7g` (Gipat), `bz8k..bz11k` (Ingos), `bz13h..bz18h` (Suslanger), e.g. `#res basegipat basecam`: the
scripted base scenes (zone kind 3); the 23 `game` zones (`gz1g`, ... with `#maps`) are the playable quest zones (kind 1); the
48 `edge` zones (`gz1g_bz1g`, ...: no `#res`, only `#position` and `#exit` records) are the world-map links between them (kind 4).

| global | values |
|---|---|
| zone kind `g_0x7C2CC4` = `CWorldServer +0x12d64` | 1 playable zone, 2 main menu, 3 scripted zone (record kind 2), 4 camp / map screen (record kind 1), 7 editor map (`App_LoadMapAndScript` 0x456140), 8 endofgame; written through the global after the zone is loaded, zeroed by `UnloadZone`; the decompiles show the field (`f_0x12D64`) only as a reader, so earlier reads took it for a dead "net mode" |
| server mode `CWorldServer +0x138` (`SetMode` 0x58DC90, once per zone) | 1 single-player zone, 2 hosted multiplayer zone (`Session_HostMpZone`), 4 main-menu zone; 3 never set; modes 2 / 3 also `CWorld::ResetRefListPair` |
| `ZoneRecord` | `+0` name, `+4` allod (gipat / ingos / suslanger: loading video), `+8` mob base, `+0xc` mpr base (empty = no world), `+0x28` kind, `+0x2c` weather (0 none, 1 rain or any other word, 2 snow, 3 tornado), `+0x30` cave flag (`#sky cave`; `#restrict` fills `+0x34` byte + floats `+0x38..+0x40`) [V], `+0x44` cage byte, `+0x48` CArray of `ZoneExit` (0x3c: `+4` rect, `+0x28` target zone, `+0x2c` entrance, `+0x30` view angle, `+0x34` has angle, `+0x38` angle) |

### 2.4 Zone transition and the saves\current folder [V]

```
Game_EnterZone(zone, 0)
  Session_EnterZoneFromParty(party)            0x57A060
    zone record of party +0x68 (none -> return 0)
    "movie" == 1 and zone == "bz3g": play Movies\dcity.bik, remove "movie"; "movie" == 1 elsewhere: kept for later; else removed
    record mpr base +0xc empty: only SetZoneExits
    mpr = <+0xc>.mpr; zone "gz6g": "zone6_2.mpr" when "z.gz6g.frozen" != 0 else "zone6.mpr" (also the LoadSave mpr override)
    first visit  ("zs.<zone>" == 0): CWorldServer::LoadZone(mpr, <+8>.mob, zone); SetTime(4.0)
    return visit: saves\current\<zone>.sav -> MpFile::ReadContainer -> CWorldServer::LoadSave(stream, mpr override)   ("Save file %s is corrupt")
                  entrance +0x6c in 0 .. exit count - 1: n = min(1000, (gtime - "zt.<zone>") * 100) ticks (none when n < 1) of
                                         ActivateObjectsAround(exit rect centre, 70) + CWorldServer::Update(force)    <- the zone lives on while away
    record kind 2: LoadWorldScript("<mob>.mob")  else GSWorld::ResetAndRestart(g_0x7C27D4)
    SetZoneExits(record +0x48)
  SetTime("gtime"); SetWeatherMode; SetCaveFlag; SetMode(1); PlayRecordedStream
client: msg 0x09 READY -> CPlayerServer::EnterWorldOnce 0x671060 -> CWorldServer::PlayerIn(player, 0, runScript = 1)
leaving (request 4 with mode 1): Session_SaveZoneToCurrent 0x57A780: CCamera::SaveView (7 in zone kind 1, 6 in kind 3),
         ResetUnitEffects, WriteSave -> saves\current\<zone>.sav (MpFile head 0x102; "Can't write to %s"); "zs.<zone>" = 1; "zt.<zone>" = gtime
```

The catch-up multiplier was checked in the disassembly (0x57A4FE: `fld gtime; fsub zt; fmul 100.0; _ftol; cmp 1000`): one game
hour away = 100 ticks = 5.5 s of simulated time, so a zone left for 10 game hours or more gets the full 1000 ticks (55 s) [V].

`PlayerIn` 0x590700: `GivePartyItemsToPlayer` (the backpack items become loose `CPlayer +0xe4` items, quest items filtered by
`FilterQuestItems` in single player), then, in zone kinds 1 / 3 (not in the camp or the menu), the hero spawn: arrival point = the centre of zone exit
`CParty +0x6c` (view angle from the exit record or the direction to the map centre), `SpawnPartyHeroes` 0x58EF20 (4.10: a `CUnitDesc` per
`HeroRecord`, positions `pos + i * 1.2` along the direction, items re-created from the four record arrays with a
`PartyItemLink +0x1c` back to the record item, stats from the record, a `WorldScript(... AddObject(Heroes, ...))` text compiled
into `+0x12874`), `SendMsg04_Position`, `+0x110 = tick`, `+0x160 = 1` and five `GSWorld::Tick`. `PlayerOut` 0x590A40:
`BroadcastGameTime(0)`, `MoveTrackedItemsToParty` + `SendMsg83_Party` (not in the camp, kind 4), kinds 1 / 3: dead records
dropped, `DetachHeroes` 0x58FD90 (records refilled from the units, `CUnit::SetPlayer(0)` or `RemoveObject` when dead). The
client learns the heroes through the ordinary 0x40 creation records (the server's `ReplicateToPlayer` always includes the
player's own units); `CWorldClient::CreateHeroUnits` 0x5699C0 is the camp / menu side (the other page).

### 2.5 Save slots [V]

| step | `Session_SaveToSlot` 0x57B3B0 (server mode 1 only) | `Session_LoadSaveSlot` 0x57ABA0 (not in modes 2 / 3) |
|---|---|---|
| folder | busy cursor; `saves\<name>\` created and emptied; zone kind 4 (camp): the local player's (`FindPlayerById(0)`) `CParty` stored into the party store first | busy cursor, `Console_ExecuteLine`; `saves\current\` emptied; `scenario.sav` read (failure -> `Game_EnterZone("MainMenu", 2)`): `ObjPartyRecord_Read` -> party store `g_0x7AA5A0`, then version > 0x46 `CInterface::ReadSaveState` + the embedded UI stream `g_0x7AFDF8` (version `g_0x7AFE00`), > 0x51 camera |
| zones | every zone record with `"zs.<zone>"` set **and** (zone `bz7g` or the same allod `ZoneRecord +4` as the party's current zone): `saves\current\<zone>.sav` copied to the slot; otherwise (other allod, or the copy failed) `"zs.<zone>" = 0` in the stored party | the same filter and copy from the slot into `saves\current\` |
| campaign | `scenario.sav` = party store (`ObjPartyRecord_Write`), `CInterface::WriteSaveState`, the embedded UI stream (`CInterface::OnSaveToSlot` fills `g_0x7AFDF8`), `CCamera::Write`, in one `MpFile` (head 0xDEAD, verified at 0x57B974) | - |
| zone | record kind 0 / 2: `mission.sav` = `WriteSave` inline (head 0x102 at 0x57BB05) **without** `ResetUnitEffects`; log "Saved <name>" | kind not 0 / 2: `Game_EnterZone(zone, 0)` (camp); else `Session_ReadMissionSave` + `SetZoneExits`; log "Loaded <name>" (no `mission.sav` -> return 0) |

Consequences [V]: a slot keeps only the zone files of the current allod (and `bz7g`); the zones of an allod left behind are
forgotten at the next save or load (their `zs.` flag is cleared, so a later visit would be a first visit). A slot's `mission.sav`
keeps the temporary spell effects of the live zone (the cast list, save chunk 11), while a `<zone>.sav` written on a zone change
has them stripped by `ResetUnitEffects` first.

`Session_ReadMissionSave` 0x579E60: `MpFile::ReadContainer`, version < 0x52 `CCamera::Read`, < 0x47 the UI stream, then
`CWorldServer::LoadSave(stream, 0)`. The version written is `g_0x790260` (0x74); the reader takes the compressed path when the
version is above `g_0x790264`, and checks the checksum only above 0x6b.

### 2.6 Multiplayer glue (lobby) [V]

The lobby objects are `LobbyClient g_0x79B5D8` and `LobbyServer g_0x79B5DC` (alive when `+4` bit 31 is clear;
`Lobby_IsAlive` 0x57F440 tests the client). `Lobby_StartServerOrClient` 0x57E740 (lobby screen) creates them;
`Session_SetMapName` 0x57ED60 (from `LobbyServer::SetMap` / `UnloadMap` / `OnNamedEvent`) stores the map at `SessionRequest +0x7c`
and, when this process hosts, calls `Session_HostMpZone` 0x57BDF0 at once: zone record kind 0 only, `UnloadZone`,
`saves\current\<zone>.sav` if present (`LoadSave(stream, 0)`; a corrupt file returns 0, no fallback, and the saved clock is kept)
else `LoadZone` + `SetTime(4.0)`; the zone exits are a copy of the record's with **every target zone replaced by "MPGame1"**;
`SetWeatherMode(record +0x2c)`, `SetCaveFlag(record +0x30)`, zone kind 1, `SetMode(2)`, `g_0x79BE6C = 1`; then, only with a live
`LobbyServer`: the current quest line (`LobbyServer +0x25a0 questHostLine`, a `StringPairMessage` {gethostname `ip:8000`, zone})
announced with `PostSystemChatToNonHosts(8, zone)`, and `LoadMob("<zone>.mob")` - the quest's own objects over the base map (the
record name is the quest, z3xq1) [V deep read 2026-11-06]. `SessionRequest::RequestMpMapChange` 0x57EE10 (from `Session_RequestMapChange` 0x578AC0, dialog screens) is
two-phase: first call = party from the host player (`StorePartyFromUnits` + `ClearPartyItems`) or `g_0x7C82B0`, kind 5 (client,
map from `LobbyClient +0x25ec`) or 6 (host, `+0x7c`), `+0x80 = 1`; second call = `SendMsg06_String(skirmish map)`,
`PostSystemChatToNonHosts(9, ..)`. `AcceptPlayers` 0x58DAF0 turns accepted joins into `CPlayer` + `CPlayerServer`
(`CPlayerManager::CreateServerPlayer`, `+0x130 = -1`, start position `+0x164`, "Accepted player N"). `Campaign_*` / `Briefing_*` /
`Lobby_*` (0x57F560-0x580900) keep their names: mission file scan (`*.mq`), named floats (`constr.current`, `b.smith.*`),
briefing text keys.

`Campaign_AdvanceMission` 0x5800C0 (deep read 2026-11-03 [V]; the multiplayer client, called when a lobby variable message 10
arrives, net-transport.md) **rebuilds the player's named floats from the lobby's campaign state**. It does nothing without a live
`LobbyClient`. It unloads the text archives queued in `g_0x7AFDD0` and clears the player's named float map `CPlayer +0xC4`, keeping only
`constr.current`. With a current map (`+0x25F4`) it queues that map's file transfer. By the starting allod `+0x261C` (0..3) it
sets the first built camp building (`b.smith.constr_1`, `b.Shopper.constr_3`, `b.kuzn.constr_4`, `b.golem.constr_5`) and the intro
name `intro_1..4`; the briefing keys `<intro>q` / `<intro>k` (`Briefing_MakeKey`) are raised to at least 1. Every briefing of the list
`+0x25F8` goes through `Briefing_SetNamedFloat`, and every lobby variable of `+0x25FC` named `q.<x>.<y>` triggers `QuestFiles_Scan(x)`.
The variables `+0x25FC` are records {name +0xC, value +0x18, serial +0x1C}. In a playable world (`CWorldClient` kind 1) they are replayed
in serial order: the journal counters `g_0x7AFED0` (highest serial seen) and `g_0x7AFED4` (next to show) pick the new ones, which are
sorted (`Sort_IntroLoop`) and shown through `CampScreen_ShowNamedState`, then every variable up to the shown serial is written into
the named floats. Elsewhere they are all written at once and the counter resets to 0. `Lobby_SetMpGameFlags` (`z.MPGame1`) ends it.

### 2.7 Databases and the balance index [V]

`Databases_LoadAll` 0x57CB50 loads units / perks / spells / items from the database path (`g_0x79BC94`, file `g_0x7AFDEC`);
`Session_ProcessRequest` reloads them when the file name changes between `database.res` (single player) and `databaseLMP.res`
(lobby alive), and swaps `g_0x79BEC0` (the balance index into the stat tables `g_0x7C54A0`, forced 0 in lobby games) with its
saved copy `g_0x79BEC4`. `Databases_Reload` 0x57DEE0 is the explicit variant.
Both paths destroy the unit tables first (`UnitsDb_Dtor` / the table dtors), so `UnitsDb::Load` 0x53A200 always starts from empty
name maps (data-sound.md 3) [V deep read 2026-11-06].

`QuestsDb::ParseQuestReg` 0x6AE8E0 (each map's `quest.reg`, from `QuestFiles_Scan`) [V deep read 2026-11-06, disassembly
0x6AEA09..0x6AEA6E]: `[quest]` `name` -> `Quest +0` std::string, `exp` -> `+0xc` **float**, `money` -> `+0x14` float, `zone index`
-> `+0x18`, `order` -> `+0x1c` (`+0x10`, qdb tag 2, stays 0), `questplaces` "{x;y}" entries (braces trimmed, split on ';', exactly two
parts) -> `+0x20` float pairs; the quest is appended only when its name is not in the map `+0xc` yet, and the whole map is rebuilt
after the push (the vector may move). `[briefing receive]` / `[briefing reject]` / `[briefing complete]`: `name`, `exp` `+0xc` and
`money` `+0x10` (floats), then the lists `give items` `+0x14`, `remove items` `+0x20`, `give quests` `+0x2c`, `complete quests`
`+0x38`, `available zones` `+0x44`, `complete zones` `+0x50` (empty entries skipped); receive is added when its name is new,
reject and complete through `BriefingTable_AddUnique` 0x6B0600. The quest log reads exp `+0xc` and money `+0x14` as floats
(`CQuestLogWin::FillObjectives` 0x64DC60), and `OnNamedEvent` applies the briefings (4.14).

## 3. CWorld (0x84 bytes, vtable 0x73F18C)

| off | field | notes |
|---|---|---|
| +0x1c / +0x20 | timeBase / timeScale | `SetTime` 0x55FB30 (slot 31), `SetTimeScale` 0x55FB50 (slot 32: `1 / (15 f)`, 0 = stopped, f < 0 = default `g_0x79BE5C`), `GetHourAndRate` 0x55FBD0, `IsNight` 0x55FC10 (outside 6..22), `NightFactor` 0x55FC50 (1 before 8 h, `(h - 20) / 4` after 20 h) |
| +0x24 / +0x30 | windDir / windStrength | `SetWindFields` 0x55FCB0; server `SetWind` 0x58EE20 clamps 0..1 and flags 8 |
| +0x34 | manager34 | slot 23; client only (`CWorldClient::InitClientResources`) |
| +0x38 .. +0x58 | the managers | created in this order by `CreateManagers` 0x55EE40 (`SetWorld(this)` each): `+0x38` CPlayerManager (0xb0), `+0x40` CMapObjectManager (0xbc), `+0x3c` CFigureManager (0xd0), `+0x48` CEffectManager (0xb0), `+0x50` CSpellConstructor (0x24), `+0x4c` CSpellManager (0xd4), `+0x58` CItemConstructor (0x34), `+0x54` CItemManager (0xbc); slots 20 / 19 / 15 / 14 / 17 / 22 / 16 / 18 return them; `Done` 0x55F8E0 releases them in reverse |
| +0x5c | refHolder | the local "loopback" net holder (`RefHolder`, vt 0x73F240, 12 bytes, built by `CreateManagers`): pumped every frame (slot 5, empty here) by `CWorldClient::PumpNet` and the server tick, whose `AcceptPlayers(this, +0x5c)` takes the local player's stream from it (slot 6 `MoveRefToList`) [I, 2026-11-03] |
| +0x60 | refListPair | `RefListPair` (vt 0x73F3A8, two lists) created by `CWorld::ResetNetDriver` 0x55F410 in modes 2 / 3 and `ConnectToLobbyHost`; released by `UnloadZone`; it is the live `CTcpNetDriver` of the remote peers (net-transport.md) [V] |
| +0x64 / +0x68 / +0x6c | tick / frameTick / paused | see 1. |
| +0x70 | waterLevels | MFC CArray (vt 0x73F234, `WaterLevels::Serialize` 0x560200) of 16-byte `{id, stepsLeft, level, step}`; script `SetWaterLevel` 0x593FA0 -> `WaterLevels::SetTarget` 0x6D4410, `WaterLevels::Tick` 0x6D44E0 in `Update`, replicated by msg 0x73, saved |

Shared slots (both worlds): 13 GetWorld, 24 `CloneItem` 0x55E940 (`CItemManager::CreateItem(kind, proto, sub, -1)` +
`vt[15] CloneInto`; the server override 0x594CE0 copies the mana and lists regenerating items in `+0x12e44`), 26 IsServer,
27 GetHeight, 28 terrain cell, 31 / 32 time, 40 FindObjectById; `FindUnitById` 0x55FB00 filters type 0x50. Id spaces: object ids
are assigned by the managers (`CBaseManager::RegisterWithNewId`); the `.mob` NID (`CObjectDesc +0x18`) is kept, -1 = allocate;
effects get ids from the effect manager; `CObject +0x18 objectId` (prototype << 16 | sequence for spells / items) is written in
the save's id table. Paths: `World_MapPath` 0x55E990 = `g_0x7AF460 + "maps\"`, `World_MapFilePath` 0x595BA0 (absolute names pass
through), `World_InitWorkDir` 0x55EA10 = `g_0x7AF454`.

## 4. CWorldServer (0x12e7c bytes, vtable 0x73F928)

### 4.1 Layout (own fields; the CWorld part is above, CAIMap at +0x170 is pathfinding.md) [V]

| off | field | notes |
|---|---|---|
| +0x84 / +0x88 | nightFactor / weatherSight | `UpdateEnvironment`: 0.9 while it rains |
| +0x8c | alarmSlots[5] | `{byte active, int tick, float x, float y}`; `ActivateLogicSlot` 0x593FC0 (script InvokeAlarm) |
| +0xdc / +0xe0 | weatherMode / caveFlag | from the zone record (`+0x30` = the map.txt `#sky cave` flag, not `#restrict`: MapInfo_Load 0x45EA20; `#restrict` fills ZoneRecord +0x34 byte + floats +0x38..+0x40); saved; `+0xe0` also in the zone state blob |
| +0xe4 / +0xe8 | rainKind / rainStrength | `SetRain` 0x58EE00 (script), msg 0x70 (byte, short) |
| +0xec .. +0xfc | zone exits | CArray of `ZoneExit` (0x3c) from `SetZoneExits` 0x5915E0; `GetZoneExit` 0x58E4C0; sent in the zone state blob (`CEffectKey::Write` 0x4E58A0 is really `ZoneExit::Write`) |
| +0x100 / +0x172 | aiMapReady / navGridOn | 0 while loading (`BeginAIMapBatch` 0x595A70 / `EndAIMapBatch` 0x595A90) |
| +0x104 .. +0x118 | activeObjects hash, activeMaxId | see 4.3 |
| +0x11c | sendPositionOnJoin | `SendZoneState` sends `SendMsg04_Position` when set |
| +0x11d / +0x11e | createUnits / createObjects | `NewZone` sets them: a map re-entered from a save loads only the static objects (`+0x11d = 0`), a zone change inside the same `.mob` only the units (`+0x11e = 0`) |
| +0x120 | progressZoneName | the zone record name (loading video, quest item filter, save) |
| +0x124 .. +0x12c | mobNames | the extra `.mob` files (`LoadMob` 0x595AB0); saved (version >= 0x60) |
| +0x138 | mode | see 2.3 |
| +0x13c | lastThunderTick | |
| +0x140 | hoover | `CHoover` (the loot collector) |
| +0x144 | smessRegistry | `CFileRegistry` over `SMessBase.reg` |
| +0x15c / +0x160 | mapLoaded / worldScriptRunning | |
| +0x164 | startPosition | copied into joining players |
| +0x170 | aiMap | `CAIMap` (0x12704; CollisionMap `+0x294`, NavGrid `+0x2468`, terrain rows `+0x280c`, StimulusMap `+0x2838`) |
| +0x12874 / +0x12a6c | worldScript / systemScript | (script-vm.md 2026-11-01: SpawnPartyHeroes, OnNamedEvent, CTrapObject::Update, MobFile_Load and LoadWorldScript all compile into the ZONE VM +0x12874; only the console WorldScript_Execute and World_ScriptSpawnFlash use +0x12A6C) two `GSWorld` VMs (0x1f8): the zone script (saved) and a second one ticked unconditionally (one-shot `WorldScript(...)` texts from `SpawnPartyHeroes` / `OnNamedEvent` 0x592090 / `World_ScriptSpawnFlash`) [I] |
| +0x12c64 | relations | `PlayerRelations` (0x100) |
| +0x12d64 | zoneKind | = `g_0x7C2CC4` (2.3); the branch selector of the tick set, the replication set, the random weather, the hero spawn and the camp behaviour |
| +0x12d68 / +0x12d6c | mapName (.mob) / terrainName (.mpr) | |
| +0x12d70 .. +0x12dfc | the six id hashes | all (+MaxId `+0x12d84`), static (`+0x12d9c`), dynamic (`+0x12db4`), dynamic non-units (`+0x12dcc`), named (`+0x12de4`), effects (count `+0x12df8`, max `+0x12dfc`) |
| +0x12e00 | staticObjectStream | `CMemStream` of 0x40 records filled by `SeparateStaticObjects`; read by `CWorldClient::LoadZone` through `Game_GetGlobal7C2D60` 0x578750 (= `&g_0x7AFF60 +0x12e00`) and appended to the local player's packet stream (`CPlayerClient::AppendStream`): the static objects reach the client without crossing the network [V] |
| +0x12e28 .. +0x12e40 | cast list | `CSpellEffect` refs (vt 0x73F9D0) |
| +0x12e44 .. +0x12e5c | item update list | regenerating items (`AddItemToUpdateList` 0x594F30) |
| +0x12e60 .. +0x12e78 | loot records | `LootRecord` (0x20: unit id, lobby index, money, item RefArray) nodes; `LootUnit` 0x5942D0, `HasLootRecordFor` 0x5949D0 |

Lifecycle: `CWorldServer::ctor` 0x589ED0 (the static initializer 0x589BE0 builds `g_0x7AFF60`), `Init` 0x58AE10 (managers,
registry, `CServerSpecificManager` at `+0x44`, `CHoover`, wind (1, 0, 0) / 0.4, `+0x11d = +0x11e = 1`), `Reset` 0x58B600,
`ClearObjects` 0x58B250, `Done` 0x58B020, `dtor` 0x58AAD0.

### 4.2 Object containers and ids [V]

| hash | who is in it | written by | read by |
|---|---|---|---|
| `+0x12d70` all | every created object | `CreateObjectFromDesc`, `ReadSaveObjects` | `FindObjectById` (slot 40), `RemoveObject`, `UpdateEnvironment` (vt[57] on each), `ActivateLogicSlot`, `LootAllDeadUnits` |
| `+0x12da0` dynamic | the same minus the static scenery | the same; `SeparateStaticObjects` removes | `TickActiveObjects` (zone kinds 2 / 3 / 4), `ReplicateToPlayer`, `WriteObjectIds` / `WriteObjectNames`, `RemoveAllObjects`, `ResetUnitEffects`, `WriteUnitHashes` |
| `+0x12db8` dynamic non-units | dynamic objects whose type is not 0x50 | the same | `ReplicateToPlayer` (zone kind 1 with `g_0x79BE6C`) |
| `+0x12d88` static | objects not dynamic (`vt[29]() == 0`) after `LoadMapObjects` of a base map | `SeparateStaticObjects` | `RemoveObject` only (they are never ticked, saved or re-sent; the client loads them from the same `.mob`) |
| `+0x12dd0` named | objects with a non-empty name `+0x1a8` | creation | `ReplicateToPlayer` (zone kind 1), scripts through `GetObjectByName` |
| `+0x104` active | kinds 6 / 7 / 8 / 9 at creation, units activated by `ActivateObject` 0x58D5D0 (`TakeHit` too) / `ActivateObjectsAround` 0x58D3D0 / `ActivateLogicSlot`, non-units and busy-brain units from a save | | `TickActiveObjects` in zone kind 1 (the playable zones) |
| `+0x12de8` effects | `CEffect*` (lights, sounds, particles, lightning ..) | `AddEffectObject` 0x58E520 (18 callers), `CreateObjectFromDesc` kinds 2 / 3 / 4, `LoadSave` | `Update` (drop when `vt[6]` returns 0), `ReplicateToPlayer`, `RemoveEffectObject` 0x58E5D0 (80 callers), `ClearEffects` 0x58B150 |

`RemoveObject(id)` 0x5935C0 (12 callers) is the only removal path [V deep read 2026-11-06]: the object is looked up in `+0x12d70`
(absent -> nothing); type not 0x61 -> `CollisionMap::RemoveObjectAndUnstamp`; erased from the active hash `+0x104`, `+0x12d70`,
the named hash, and either the static hash `+0x12d88` or (not static) the dynamic `+0x12da0` and dynamic non-unit `+0x12db8`
hashes; `vt[5] Done`; held once more (refcount + 1); units `CUnit::SetPlayer(0)`; flag bit 0; `GSWorld::PurgeDeadObjects` on both
VMs; `vt[56](obj)` on every object still in `+0x12d70`; every cast of `+0x12e28` whose `CSpellEffect::OnObjectRemoved` returns 0
is unlinked; `CPlayerServer::RemoveTrackedUnit` per player; two releases (net: the hash's reference is dropped). `RemoveAllObjects` 0x596C60 /
`RemoveAllUnits` 0x5939E0 iterate `+0x12da0` / `+0x12d70`.

### 4.3 The activation model [V]

`TickActiveObjects` 0x58D670: in a playable zone (`+0x12d64 == 1`) it walks the active hash `+0x104`: `vt[6] Update` each;
units decrement `+0x174` (armed to `(ptr >> 5 & 15) + 128` = 128..143 ticks by every activation) and leave the hash at 0 -
they fall asleep - unless the brain is busy (`CUnitServer +0x174` / `+0x9b7` / `+0xb0c` set), in which case the countdown is
re-armed. Who activates: `CreateObjectFromDesc` for kinds 6 / 7 / 8 / 9 (fires, traps, levers, active objects: they never
sleep because they are not units), `ReadSaveObjects` for non-units and units whose brain flag `+0x9b7` is set, `ActivateObject`
0x58D5D0 (`CUnitServer::TakeHit` and the AI: a unit that is hit or ordered wakes up), `ActivateObjectsAround` 0x58D3D0 (every
`Tick` around every party unit of every player with `max(sight +0x904 * +0x294, life-sense +0x2a4) * 2 + 20` m, and 70 m around the
entrance during the zone catch-up), `ActivateLogicSlot` 0x593FC0 (script `InvokeAlarm`: the player-less units whose brain selects
the slot). A sleeping unit does not run `CUnitServer::Update` at all (no AI, no regeneration, no movement); an object asleep
keeps its last replicated state. In the menu (2), the scripted zones (3) and the camp (4) every object of `+0x12da0` ticks. The
removed-object sweep (flag bit 0 -> `RemoveObject`) and the item update list (`+0x12e48`, `vt[6]` = `RegenMana`) run in both
branches. For the bot: a unit far from every hero is frozen until a hero's sight circle (x2 + 20 m) reaches its 16 m sector.

### 4.4 From descriptors to objects (`CreateObjectFromDesc` 0x5927C0, 10 callers) [V]

Descriptors are the parsed `.mob` nodes. `MobFile_ParseChunks` 0x481F00 (chunk -> class, size, ctor; verified in the
disassembly 0x481FA8..0x482113 [V deep read 2026-11-06]):

| chunk | descriptor | bytes | ctor | kind +0x48 |
|---|---|---|---|---|
| 0xB000 | the object section; the sub-chunks below | | | |
| 0xB001 | `CObjectDesc` (plain objects; kind 9 is only built in code by `SpawnConsumableUnit`) | 0x8c | 0x46A5C0 | 1 |
| 0xAA01 | light (`CEffectDesc` vt 0x73C788) | 0x68 | 0x46D960 | 2 |
| 0xCC01 | sound (`CEffectDesc` vt 0x73C7B8) | 0x88 | 0x46DE70 | 3 |
| 0xDD01 | particle (`CEffectDesc` vt 0x73C758) | 0x5c | 0x46D520 | 4 |
| 0xBBBF0000 | torch / campfire (`CTorchDesc`) | 0xa0 | 0x46B210 | 6 |
| 0xBBAB0000 | magic trap (`CMagicTrapDesc` vt 0x73C650) | 0xd0 | 0x46B4A0 | 7 |
| 0xBBAC0000 | lever (`CLeverDesc` vt 0x73C6B0) | 0xa4 | 0x46BD40 | 8 |
| 0xBBBB0000 | unit (`CUnitDesc`) | 0x324 | 0x46C010 | 5 |
| 0xDDDDDDD1 | diplomacy: 0x1000-byte 32 x 32 int matrix + a CStringArray of side names | | | |
| 0xACCEECCA / 0xACCEECCB | the zone script, plain / scrambled (LCG `k = k * 0x343FD + 0x269EC3`, byte `k >> 16`, `ChunkFile::ReadBytesToString` 0x42C650) | | | |
| 0xABD0 | wind: vec3 direction (default 0, 1, 0), strength -> `SetWind(dir, min(strength, AppSettings DefaultWindForce g_0x79BE58))`, hour (default 14) -> `SetTime`, two more floats (0.2 / 0.6) read and unused | | | |
| 0xC000 / 0xD000 | markers: the parse returns 1 / 0 after them (default 1); `MobFile_Load` applies the diplomacy matrix only on 1 | | | |

Each descriptor reads its own sub-chunks (`ForEachChunk`) and is stored in a hash keyed by its NID `+0x18`; a second descriptor
with the same NID silently replaces the first (the first is leaked, never created).

`MobFile_Load` 0x481660 (called by `LoadMapObjects(mob, noScript)`): parse; result 1 -> `PlayerRelations::Set(g_0x7C2BC4, a, b,
m[a][b], applyToUnits = 1)` for all 32 x 32 pairs. With the registry option `AutomaticQuantization` (`g_0x79BCF4` = AppSettings
`+0x84`, default 0, read nowhere else) the complection (`+0x60` morph) of every kind-1 object (types other than 0x32..0x34) is
grouped by model name `+0x54` and snapped to the nearest of at most two centroids (`PointSet_ClusterCentroids(points, out, 2)`,
`Vec3s_FindNearest`): fewer distinct morphed meshes. Then per descriptor, in hash order: kind 7 -> `+0x8c` = the trap's hostile
mask (bit b set when `m[side +0x6e][b] == 2`) and `+0x94..+0x9c` = the position; `CreateObjectFromDesc`; kind 5 with a created
object `+0x1c` -> `Brain::SetMobLogics(unit brain, +0x90 logic slots, 5)`, except the descriptor with NID 2 and model `unmoco2`;
the descriptor is released; `Movie_PlayIntro` (progress) every `n / 5 + 1` descriptors. Last, unless `noScript`, the script text
is compiled into the zone VM `g_0x7C27D4` (= `+0x12874`) [V]. `CObjectDesc` (0x8c; ctor 0x46A5C0, chunks 0x46AB20): `+0x18` id
(NID, -1 = allocate), `+0x1c` object, `+0x20` pos, `+0x2c` quaternion, `+0x3c` name, `+0x48` kind, `+0x4c` object name,
`+0x50` type (the 0x3xxx creation key), `+0x54` model, `+0x58` prototype, `+0x60` morph x3, `+0x6c` flag 0x20000, `+0x6d` dynamic,
`+0x6e` side, `+0x70` / `+0x74` / `+0x78` specific strings. `CUnitDesc` (0x324): `+0x90` five 0x4c logic slots, `+0x20c` prototype,
`+0x210` UnitProto (0xac), `+0x2bc` five item name lists (`+0x2d0` = quick items).

| kind | creates | registered in |
|---|---|---|
| 1 object, 5 unit, 8 lever, 9 | map object manager `vt[23](type, id)`; needs `+0x11d` (units) / `+0x11e` (objects); names, model, `vt[34](proto)`, `PackFullNetState`; units: `SetPlayer(0)`, `SetSide`, forced alarm mask from `PlayerRelations`, `Unit_ApplyPrototype`, `Unit_GiveProtoEquipment`, quick items, `MobUnitStats_ApplyTo`, the six animation speeds from the units database (`+0x4b0`); lever: `CLeverObject::SetParams(+0x8c)`; morph, position, rotation, `+0x6d == 0` -> flag 8 (static), `BuildFromAnimation(model)`, units `ResetOrdersAndStance` | all, dynamic, non-units (not 0x50), CollisionMap; kinds 8 / 9 also active; named when the .mob name (`CObjectDesc +0x4c` copied to `+0x1a8`) is non-empty; flag 0x20000 when `+0x6c` is set or the object has a name [V deep read 2026-11-03] |
| 2 light | effect manager type 0x4b (pos, `+0x14c`.. colour, `+0x158`) | effects |
| 3 sound / particle | type 0x48 (wave name `+0x58`; "Adding sound without wave name") or type 0x49 when `+0x84` | effects |
| 4 | type 0x4a | effects |
| 6 fire / campfire | like 1 plus a "campfire" spell cast (`CastSpellMulti`) at `+0x90` when the normal is upright, light emitter `vt[16..18]`, flags 2 / 4 | all, dynamic, non-units, active, CollisionMap |
| 7 magic trap | like 1 (static flag 8) + `CMagicTrapObject::SetParams(+0x8c)` | all, dynamic, non-units, active |

Object kind 0x39 is `CFakeUnit` (0x580, a `CUnit` with an old-style animation system at `+0x4E0`): its model comes from a 3ds
scene file through `CFakeUnit::LoadModelFromScene` 0x446C10 (path under `scenes\`, "Can't open file", "Scene %s have more then one
top ..."), one client figure per scene part; it is the only user of the 3ds importer (`Scene3ds_*`). No shipped map places one
[V, data 2026-11-03]: the object type chunks (0xB003) of the 99 campaign maps and the 130 LostInAstral maps hold only 0x32 (4857),
0x33 (5599), 0x34 (4715) units, 0x35 (93300), 0x36 (27613), 0x37 (2867), 0x3A (565), 0x3B (15) and 0x3C (2025); no code asks the
factory for kind 0x39 either, so `CFakeUnit` and the 3ds importer never run in the shipped game.

The same function spawns heroes (`SpawnPartyHeroes` 0x58EF20, `SpawnSingleHero` 0x5910A0) and the console unit
(`CWorldServer::SpawnConsumableUnit` 0x58ECE0: proto `st.tr.130`, model `unhuma`, type 0x3010). `CObjectDesc::Spawn` 0x46AFB0 / `CUnitDesc::Spawn`
0x46C4D0 (the editor / script path) call it when `+0x1c` is 0.

### 4.5 Loading a zone, step by step [V]

```
CWorldServer::LoadZone(mpr, mob, zone)        0x595480      (called by Game_EnterZone, Session_EnterZoneFromParty / HostMpZone, SetZone, App_LoadMapAndScript)
  +0xe0 = 0; World_SelectProgressVideo(zone) 0x595080 (progres[1-4].bik by allod); Movie_PlayIntro (the loading screen)
  both files must exist (cwd or maps\) else 0
  World_LoadAIGraphStream(mpr, mob, stream) 0x596140: the AIGRAPH chunk (0x31415926) of the .mob, else "<mpr>.grp"   ("Can't load AIGrid")
  mob names cleared; DirtyRect::Flush; +0x100 = 0; +0x172 = 0; alarm slots cleared
  NewZone(mpr, mob, stream, fromSave = 0)     0x595C70
    mpr != +0x12d6c: CAIMap::Dtor + CAIMap::Load(maps\mpr) (terrain grid, materials, water, heights; pathfinding.md) ; +0x12d6c = mpr, +0x12d68 = ""
    stream non-empty: NavGrid_Read(+0x2468)   (the AIGRAPH)
    mob != +0x12d68: both GSWorld::Reset, PlayerRelations::Reset, RemoveAllObjects, cast list + effects cleared, +0x11e = 0 (objects come next), +0x11d = !fromSave
    mob == +0x12d68: ClearObjects, +0x11e = 1, +0x11d = !fromSave
    every CPlayer +0x130 = -1; +0x15c = 1; rain cleared (+flag 2)
    +0x11e or +0x11d: +0x12d68 = mob; LoadMapObjects(mob, +0x11d == 0) 0x596030   -> MobFile_Load 0x481660 -> CreateObjectFromDesc each, the .mob script compiled into +0x12874, diplomacy g_0x7C2BC4
                      not +0x11d: GSWorld::Reset(+0x12874) + PlayerRelations::Reset (the save brings them)
                      +0x11e: SeparateStaticObjects 0x5951A0
    fromSave: RemoveAllObjects + effects cleared (ReadSaveObjects re-creates the dynamic ones)
    +0x11d = +0x11e = 1
  +0x100 = 1; Flush; +0x172 = 1; +0x120 = zone; +0x160 = 0
```

`UnloadZone` 0x5957E0 (every session transition): players detached and removed, `+0x15c = 0`, loot records, mob names,
`+0x172 = 0`, both VMs, relations, `RemoveAllObjects`, casts, effects, `ReleaseRef60`, `+0x138 = 0`, `+0x12d64 = 0`. `SetZone`
0x595B50 (script `Game_SetZone` 0x578730) is `LoadZone` immediately followed by `UnloadZone` (a validation / warm-up only).
`LoadWorldScript` 0x596350 reads the script section (0xACCEECCB scrambled -> `CString_Unscramble`, 0xACCEECCA plain) of a
`.mob` into `+0x12874` for zone records of kind 2.

### 4.6 Weather, time and the environment [V]

`UpdateEnvironment` 0x58CCD0 (every 64 ticks and from `SetTime`): `+0x84 = NightFactor`; in a playable zone (`+0x12d64 == 1`)
the random weather: mode `+0xdc` 1 / 2 -> every 10 ticks (`g_0x7C2DFC`) a 1/101 chance to start rain (`+0xe4 = 1 / 2`, strength
20 / 40, flag 2) and a 1/31 chance to stop it; mode 3 (tornado) -> a `CEffectMoshka` lightning (effect kind 0x65) from a random
grass / earth tile (`CAIMap::GetMaterialIndex` 1 / 3, not water) to a point 30..70 m away when the height step along the path is
below 1 m. Also: rain kind 1 -> a thunder flash every 20 ticks (`World_ScriptSpawnFlash` 0x578E30, `+0x13c`); `+0x88` = 0.9
while raining; every object's `vt[57]()`; `BroadcastGameTime` in zone kinds 1 / 3. Scripts use `SetRain` 0x58EE00 / `SetWind`
directly. `SetTime` 0x58EEA0 / `SetTimeScale` 0x58EEE0
wrap the CWorld versions and flag 4 (msg 0x71). The client renders the day-light tables from the hour it receives
(`CWorldClient::LoadDayLightTable`, the other page).

### 4.7 Replication [V]

Join: `AcceptPlayers` creates the player with `+0x130 = -1`; the first `ReplicateToPlayer` sends `SendZoneState` 0x58C890 =
msg 0x02 blob `{u32 tick - 1, mpr name\0, mob name\0, u32, ZoneExit records (count +0xf4), u32 +0xe0, the unit equipment hashes
(CUnit::BuildEquipmentTables per unit, UnitHashes_WriteSave: one record per distinct look = prototype + complection (3 x 10 bits) + the 7 worn and 4 weapon-slot item ids + active weapon slot, or the texture names of non-humanoids, with the unit ids sharing it)}`, then
`CPlayerServer::SetState4` (player state 4 = waiting for the client's 0x09), `ResendItemValues` 0x66F080 (message 0x14 for every
synced party item id of `CPlayer +0x114`) and `FlushSendStream`, and returns (nothing else that tick) [V, deep read 2026-11-03].
Nothing runs unless `CPlayerServer +0x20 state == 1` and the map is loaded. Per tick afterwards:

| flag / set | message | content |
|---|---|---|
| `+0x130 & 2` | 0x70 | rain kind byte, strength short |
| `& 4` | 0x71 | hour, rate (`GetHourAndRate`) |
| `& 8` | 0x72 | wind vec3 + strength |
| `& 0x20` | 0x73 | the water level records (`Vec16Array_Write` 0x6D4570) |
| `& 0x10` | 0x07 | two dwords (`SendMsg07` 0x66F180) |
| objects | 0x42 / 0x40 / 0x41 | `PtrArray_Diff(+0x50d8 known, wanted)`: untrack ids / creation records (`SendMsg40_Units`: id, kind, objectId, type == 0x50, position `+0xc4`, name `+0x1a8`, model, prototype, then the delta-coded state) / figure updates |
| effects | 0x52 / 0x50 / 0x51 | effects of `+0x12de8` whose `vt[19]` radius squared covers the view point: remove / full state (`+0xf8` stream) / delta (`+0x120` stream) |
| states 1 / 2 | 0x60 | `SendMsg60_PartyStatus` (low health / in combat) |

The wanted object set (static array `g_0x7AFF48`) by zone kind [V, deep read 2026-11-03]:
- 1 (playable zone), the normal case: `g_0x79BE6C` is set by `Game_EnterZone` for every playable zone and by `Session_HostMpZone`,
  and never cleared. Then: the dynamic **non-unit** objects `+0x12db8` (type != 0x3b) within 100 m (squared distance < 10000) of
  the player's view point (`CPlayerServer +0x38 / +0x3c`, updated by the client's 0x40 view-point message, `PlayerIn` and
  `SendMsg04_Position`), the player's `visibleUnits` (`CPlayerServer +0x510C`), every named object `+0x12dd0`, and for every unit
  of the player's `CPlayer +0x40 partyUnits` the units within `max(sight +0x294 x sightMult +0x904, life-sense range +0x2A4) x 2 + 5`
  of it (`CollisionMap::FindUnitsNear`; fields of the unit's `CUnitServer`). Units are therefore replicated by what the party
  can sense, not by distance to the camera.
- 1 without `g_0x79BE6C` (only before any `Game_EnterZone`): every dynamic object `+0x12da0` (type != 0x3b) within 100 m or a unit
  owned by the player (`CUnit +0x24C == player`), and none of the extras.
- 3 (scripted zone): every dynamic object except type 0x3b and flag 0x10000. 2 / 4 (menu, camp): the dynamic objects within 100 m.

Effects: every `+0x12de8` effect whose `vt[19]` radius r gives `distance^2 < r^2` to the view point (diffed against
`+0x50E8`). In the playable zone with a lobby alive and `partyMode == 0`, the party is re-sent when the countdown `+0x5124`
reaches 0 (`ResendParty`). The tail runs even when the early checks fail: `SendMsg60_PartyStatus` for player states 1 / 2 with a
map loaded, then (state 3: return 1 without flushing) `SendMsg07_GameMode` when dirty bit 0x10 and state not 0 / 4, and
`FlushSendStream`; a failed send returns 0 (the caller drops the player).
`BroadcastMsg0B` 0x58D340 (to the players tracking an object), `SendMsg81_NamedValue` (named floats: `gtime`, the mercenary
flags of `UpdateMercenaryFlags` 0x591800 / `OnMercenaryEvent` 0x591DE0, `World_SetConstrCurrent` 0x578560) and
`SendMsg80 / 83_Party` (the party blob) complete the server -> client traffic; the inside of the senders is the players page.

### 4.8 The save body (`WriteSave` 0x597BD0 / `LoadSave` 0x596E70; mission.sav and `<zone>.sav`) [V]

All strings are NUL-terminated, lists end with -1 unless noted; the container is `MpFile` (head 0x102). Version checks refer to
the container version (`g_0x790260` = 0x74 today).

| # | content | notes |
|---|---|---|
| 1 | mob name, mpr name, zone name | `+0x12d68`, `+0x12d6c`, `+0x120` (sav-format.md saw them first); the names are relative to `maps\`; `LoadSave(stream, mprOverride)` replaces the mpr name when given (gz6g frozen) and starts the loading video of the zone |
| 2 | wind 12 B, strength, nightFactor, 5 alarm slots x 16 B | = 100 bytes (the "128-byte header" of sav-format.md starts here) |
| 3 | tick, rainKind, rainStrength, hour, rate | `+0x64`, `+0xe4`, `+0xe8`; read side: version < 0x4c an int converted to the float rate (`fild`, 0x597164) |
| 4 | `+0x13c` lastThunderTick (version > 0x73), `+0x12d64` zone kind | = the two words sav-format.md found at +120 / +124 (+124: 3 in bz* / cz*, 1 in gz* zones = scripted / playable zone kind) [V] (disassembly 0x597DA5..0x597DCA; the reader stores them straight into `+0x13c` / `+0x12d64`, 0x59714A / 0x59716E) |
| 5 | AIGRAPH patch | `World_LoadAIGraphStream` of the files patched against the live `NavGrid_Write` (`Stream_WritePatch` 0x4A4DA0: the long run of 0x00FF u16 of sav-format.md) |
| 6 | object id table | `{id +0xc, kind +0x10, objectId +0x18}` per dynamic object `+0x12da0`, -1 (written inline by `WriteSave`; the same loop as `WriteObjectIds` 0x5965F0, which has no caller); `LoadSave` creates each through the map object manager `vt[23]` with `+0x18` restored |
| 7 | effect, spell, item manager headers | `vt[10] Write` of `+0x48`, `+0x4c`, `+0x54` |
| 8 | objects | `WriteObjectNames` 0x596730: `{id, prototype / model name, the object's Write (vt[10]), u32 dirty mask}`, -1; read by `ReadSaveObjects` 0x596900 (names from version 0x59) |
| 9 | manager objects | `CBaseManager::WriteObjects` x3 (effects, spells, items): `{id (+ byte when id == -0x521), vt[10]}`, 0 |
| 10 | effect ids | the `+0x12de8` ids, -1 (`LoadSave` re-adds them with `MarkDirty`) |
| 11 | spell casts | `{1, CSpellEffect::Write}`*, 0 |
| 12 | StimulusMap, GSWorld (+0x12874), PlayerRelations | `StimulusMap::Write` 0x5CAFA0, `GSWorld::Write` 0x4C0F90, `PlayerRelations::Write` 0x4557F0 |
| 13 | weatherMode, caveFlag, water levels | `+0xdc`, `+0xe0`, count + 16-byte records (version > 0x4d) |
| 14 | item update ids | -1 |
| 15 | per player (manager order) | `CPlayer +0xfc` partyId, the party (`CParty::Write(+0x70, stream, 1)` 0x661C10 = the `.mp` body), dword `+0x100` (zoneKindCopy), the loose item ids of the CList `+0xe0` (looked up in the world's item manager) -1, byte `+0x108` godMode, the synced item ids of the CList `+0x114` (count `+0x120` + ids; read only for version > 0x54); read side: version < 0x49 one extra byte, < 0x46 an `ObjRefCArray` read and dropped; `+0x110 = tick`; then `SendMsg80_Party` + `CPlayerServer::SetInWorld(1)` |
| 16 | paused byte | `+0x6c` |
| 17 | item -> record links | `{item id, PartyItemLink id}` pairs, -1 (version > 0x54) |
| 18 | mob names | count + names (version >= 0x60) |
| 19 | loot records | count + `{unit id, lobby index, money, item count + ids}` (version > 0x6d; `LootRecord::ReadItems` 0x589CA0) |

Before reading, `LoadSave` zeroes `+0x138 mode` and `+0x160`, and when a player manager exists sets both to 1 and calls
`CPlayer::ClearAll` on every player (then every player's `+0x130 = -1`); `CAIMap::BeginBatch`, `+0x100 = +0x172 = 0`. The map is
rebuilt by `NewZone(mpr, mob, AIGRAPH stream, fromSave = 1)` ("CWorldServer::LoadSave() Can't load map" -> FatalError), and
`Movie_PlayIntro` pumps the loading video between the blocks. After reading, it sets the zone name, applies `vt[32]` / `vt[31]`
(rate, hour), `+0x100 = 1`, `CAIMap::EndBatch`, `+0x172 = 1`, collects the units standing on cells without a passable neighbour
(`CAIMap::HasPassableNeighbour`; the list is built and not used [?]) and calls `ReleaseAllObjects` on the four managers;
`Session_EnterZoneFromParty` runs the catch-up afterwards. `WriteSave` starts with `BroadcastGameTime` in zone kinds 1 / 3 and
`ReleaseAllObjects` on the managers. Temporary spell effects are stripped by `ResetUnitEffects` 0x5956E0 only in
`Session_SaveZoneToCurrent` (zone change), not in `WriteSave` itself nor in `Session_SaveToSlot` (2.5). The zone kind **is** saved
(chunk 4) and restored into `+0x12d64`; `Game_EnterZone` writes the same value again through `g_0x7C2CC4` after the load.

### 4.9 The stat scramble (`World_SetStatScramble` 0x5790E0, `World_XorStats` 0x5791C0) [V]

`main` calls it with 1 while the window is active and 0 when it is not; it acts only on a change of `g_0x79026C`. On 1 -> 0
(deactivation) a fresh key is drawn from the ISAAC pool `g_0x7A9D40` into `g_0x7AFECC` and XORed over: `CPlayer +0xc0`
(money) and every hero record's dwords `+0x20..+0x724` of the client player manager `g_0x7AF5F0`; the same plus every tracked
unit's `UnitStats +0x1e0..+0x8e4` (`CUnitServer`) of the server manager `g_0x7AFF98`; `g_0x7C8300`; the party store records
`g_0x7C82E0` (count `g_0x7C82E4`). On 0 -> 1 the same key is applied again (undo). So the live stats are plain while the game is
focused and scrambled with a random key while it is not (an anti-ArtMoney measure). An external tool must XOR with
`g_0x7AFECC` whenever `g_0x79026C == 0`; um.dll running inside the process while the game has focus sees plain values.

### 4.10 Spawning the party's heroes (`SpawnPartyHeroes` 0x58EF20) [V deep read 2026-11-06]

Arguments: `(CPlayer* or 0, HeroRecord* CArray, arrival pos, arrival quaternion x / y / z / w, previous-state CArray or 0)`
(`PlayerIn` passes the exit centre and angle; the previous states are the 0x28-byte records `DetachHeroes` left on a zone change:
`{id, pos[3], quat[4], stance +0x20, weapon +0x24}` [I from the reads]). One script text is built:
`WorldScript(` + one `AddObject( Heroes, GetObjectByID( "<id>" ) )` per hero + `)`. Per record `i`:

1. A `CUnitDesc` is filled: kind 5, proto `+0x14`, model = record `+8` (the race figure mask, "unhuma"), morph = `+0x724`,
   dynamic, side 0, specific strings `+0xc` / `+0x10`, desc dwords `+0x90 = 4`, `+0xb8 = 0`, `+0xc8 = 1` (logic slot 0; the brain is rebuilt in step 3 anyway); for prototype type 0x32 (humanoids,
   `Units_GetProtoTypeByName`) the part list is the 16 body parts `hd bd hp hr lh1 lh2 lh3 rh1 rh2 rh3 ll1 ll2 ll3 rl1 rl2 rl3`
   with `hr` replaced by `hr.%.2d` of the prototype's hair index (`UnitProto +0x10`, 1 when the prototype is missing, part
   dropped when -1). NID = record id `+0x1c`; an existing object with that id is removed first (`RemoveObject`).
2. Position: the previous state with the same id, if any (position and rotation kept); else rotation = the arrival quaternion,
   angle `a = 2 acos(..)` of it (negated when `w / |v| < 0`) and `pos = arrival + i * 1.2 * (cos(a + pi), sin(a + pi), 0)`: the
   heroes stand in a line 1.2 m apart, behind the arrival point.
3. `CreateObjectFromDesc`; then on the unit: `Brain::ClearLogics`, a new `AiLogicPlayer` (0x24), `Brain::ClearOrder`; out of the
   collision map, `CAIMap::FindNearestReachableCell(unit, arrival pos)`, back in; `CUnit::SetPlayer(player)`; `vt[61]`(display
   name `+0`); `SetField438(+0x780 battle button)`; `ResetServerSpecificSlots`; `vt[35]`(stats `+0x20`); `DropAllItems`.
4. Items, each cloned through the world's `vt[24] CloneItem` with a `PartyItemLink` (`+8` = the record item's id) at clone
   `+0x1c`: weapons `+0x730` -> `AddWeaponSlotItem`; senses `vt[25](0)`; armour `+0x750` -> `EquipArmor`; belt `+0x740` ->
   `AddQuickSlotItem` with the count `+0x48` copied and **`+0x28 = +0x2c`** (the clone's charge refilled to its maximum);
   spells `+0x760` -> `AddSpellSlotItem`. Then `vt[37]()`, `vt[33](0)`, `SetProto2ByName(+0x18)`.
5. Stance / weapon: zone kind 1 (and not the intro below): `stanceWanted = +0x770`; a previous state: its stance (set at once with
   `SetStanceBoth` when the cell is passable for class 1, else only wanted); `ResetOrdersAndStance`; zone kinds 1 / 3:
   `weaponWanted = +0x774`; previous state and zone kind != 3: its weapon.

After the loop, in zone kind 1: every hero whose record has a follow target `+0x77c > 0` gets `AiLogicPlayer::SetLeader(that
unit)`. **The intro**: when the terrain name starts with "zone1" (case-insensitive) and the arrival point is within
`sqrt(300)` = 17.3 m of (54, 138) (z counted from 0), and the server mode is not 2, the script also gets
`PlayAnimation(GetObjectByName( "hero" ),"uspecial25")` (the new-game wake-up) and the record stance is not applied. The text is
compiled into the zone VM `+0x12874` only when a player was given.

### 4.11 The multi-target cast (`CastSpellMulti` 0x58E850, 12 callers) [V deep read 2026-11-06, disassembly 0x58E874..0x58E954]

`(CSpell*, caster or 0, caster pos, target object or 0, target point, quiet)`; the spell side is spells.md 4.3.

1. Nothing at all happens when the spell is null, when there is a caster and the zone kind `+0x12d64` is not 1 (camp, menu and
   scripted zones only take caster-less casts: scripts, traps), or when the spell kind (`CSpell +0x18 >> 16`) is 0x15
   possession, 0x1B link or 0x24 charm - those three are refused for every caller, caster or not (the shipped engine's lock on
   the three spells; SpellAddon's `possessioncharmlink=1` lifts it, docs/SpellAddonInfo.en.md).
2. `flags +0x3c & 0xffff == 0` (no target filter) and `targets +0x38 < 2`: one `CastSpell(spell, caster, pos, target, point,
   quiet)`.
3. Otherwise: the caster's `CPlayerServer` (`caster +0x24c -> +0x20`, 0 for monsters); a point-target spell (`0x20000000`)
   without `ic` (`0x20000`) is first cast once on the given target / point (count - 1). The candidates are
   `CollisionMap::FindUnitsNear(centre, range +0x34)` with the centre = the caster's position (or the given position without a
   caster); a given unit target (type 0x50) is moved to the head of the list.
4. While the count is above 0: dead units (`vt[55]() != 0`) are dropped from the list; the given target is taken at once when it
   is still there, else the unit nearest to the target point; none left -> stop. The pick leaves the list. With a caster it must be
   known: player caster -> in its visible array `+0x510c`; monster -> in its `CUnitServer +0xa94` list, or else bit
   `caster side +0x250` set in the relation row of the candidate's side (`PlayerRelations +0x12c64`); without a caster no test.
   Then `CUnitServer::ComputeCastTarget` and `CSpell::IsValidTarget` must pass, and `CastSpell` on it (count - 1). A refused
   candidate costs nothing: the next nearest is tried.

### 4.12 Experience (`GiveExperience` 0x5911C0) [V deep read 2026-11-06]

`(CPlayer* or 0, xp)`; xp 0 does nothing. Callers: `CUnitServer::TakeHit` (kills, combat.md 4), the script `QuestComplete`, the
lobby player events.

- **A player given**: the live (`vt[55]() == 0`) units of kind 0x32 (humanoids) in the player's party-unit hash `CPlayer +0x40`
  share it equally: `share = xp / n`; each gets `CUnitServer::UpdateStatsFromPlacement(share)` on the live unit and
  `UnitStats_AddExperienceScaled(share)` on the `HeroRecord` with the same id (`CParty` records `+0xa0`, count `+0xa4`); then
  `SendMsg15_PartyExperience(share)` once. Dead heroes and non-humanoid party members get nothing.
- **No player** (the online credit): only players that have a connection `+0x20` and exactly one hero (`+0xa4 == 1`) whose unit is
  alive take part. Weight `w = sqrt(UnitStats +0x700 accumulator + +4 experience)` of the hero, `total = max(sum w, 0.01)`, share
  `= xp * w / total` (the shares add up to xp; a more experienced hero gets more). Live unit and record are updated as above, and
  every connected player receives `SendMsg18_UnitExperience(share, unit id)` for every hero.

### 4.13 Mercenary briefing flags (`UpdateMercenaryFlags` 0x591800, script `RecalcMercBriefings`) [V deep read 2026-11-06]

For every player and every mercenary `i = 1..10` the function computes the briefing switches the camp dialogs test and sends
them with `SendMsg81_NamedValue` (names formatted in the disassembly 0x5918D5..0x5919B8: `%s` is the string `merc<i>`):
`b.merc<i>.n1_<i>`, `n2`, `n3`, `n8`, `n9`, `n10`, `n11` (all `b.merc<i>.nK_<i>`) and `amerc_<i>`. Inputs: the player's named
floats (`CPlayer +0xc4`), whether a party record has the id `String_HashCI("merc<i>")` (the record id is the hash of the
hero's key name, so a mercenary's `HeroRecord +4` is "merc<i>"), `others` = the player's live heroes - 1, and `home` = the
current zone `+0x120` equals merc i's base (table `g_0x7907AC`: 1 bz1g, 2 bz1g, 3 bz2g, 4 bz6g, 5 bz8k, 6 bz8k, 7 bz14h,
8 bz14h, 9 bz9k, 10 bz14h).

| state | sent |
|---|---|
| always first | n11 = 0 |
| in the party, n10 == 0 | n8 = n1 = n3 = n9 = 0, n2 = home |
| in the party, n10 != 0 | n8 = n1 = n2 = n3 = n9 = 0 |
| not in the party, n10 != 0 | n8 = n1 = n2 = n3 = n9 = n10 = 0 and amerc = 0 (n10 is a one-shot reset) |
| not in the party, amerc != 0, n8 == 1 | n1 = n2 = n3 = n9 = n10 = 0 |
| not in the party, amerc != 0, others < 2 | amerc == 1: n1 = 1, else n3 = 1; the other of n1 / n3, n8, n2, n9 = 0 |
| not in the party, amerc != 0, others >= 2 | n9 = 1 (party full: 3 live heroes), n1 = n3 = n8 = n2 = 0 |
| not in the party, amerc == 0, n8 != 1 | n11 = 1 |

### 4.14 Briefing completion (`OnNamedEvent` 0x592090) [V deep read 2026-11-06]

`(player index, "a.b.c")`, from `DialogScreen::StartDialogLine` and `CPlayerServer::OnMsg_NamedEvent` (the client finished a
briefing / dialog line). With a lobby alive: only `Lobby_SendNamedEvent(b)` when the name has three dot-separated parts. Else:

1. Three parts -> the `Briefing` record of `b` (`StringHash_GetOrInsert(g_0x7C87D4)`, data-sound.md 4.2; an unknown name gives
   no record).
2. The player with that index: `OnMercenaryEvent(player, name)` 0x591DE0 first; then, with a record: `GiveExperience(player,
   exp +0xc)` when non-zero (4.12: split among the live humanoid heroes), `CParty +0x50 money += money +0x10`; the item names of
   `+0x14` created (`CItemManager::CreateItemByName`) and given in one `CPlayer::AddItems`; the names of `+0x20` created and,
   **only when the item is a quest item (type 0x14)**, `CPlayer::RemoveQuestItemById(player, item +0x4c)`, then released: other
   item kinds in a "remove" list remove nothing; the names of `+0x2c` and `+0x44` -> `SendMsg81_MaxValue(name, 1)` (raised to at
   least 1, a 2 stays), the names of `+0x38` and `+0x50` -> `SendMsg81_NamedValue(name, 2)`.
3. Always (player found or not): the text `WorldScript( GSSetVar( <index>,"<a.b.c>",2 ) #OnBriefingComplete( <index>, "<a.b.c>" ) )`
   is compiled into the zone VM `+0x12874`; in zone kind 3 (bases) the VM is then ticked five times at once.

### 4.15 Looting a body (`LootUnit` 0x5942D0) [V deep read 2026-11-06]

`(unit, CPlayer*) -> bool` (items.md 6 has the drop rolls). No player, or a unit that still has a player (`CUnit +0x24c`) and is
alive -> 0. Unit flag `+8 & 0x20000000` clear: `CHoover::CollectUnitLoot(world, unit, items, ..., money)` (0 -> return 0); with a
lobby alive the quest items (type 0x14) of the haul go to player index 0 (the host) through their own `AddItems`; the looter gets
`+0xc0 money += money` and `AddItems(rest)`; returns 1 when an item or money changed hands. Flag set (the body was looted for
someone else): the first loot record `+0x12e60` with the unit id `+0xc` is handed over whole (money + items) to **whoever loots
now** - the record's lobby index is not compared - and freed; returns 1; no record -> 0.

## 5. Resource files (CResourceBase / CResourceFile / CMappedResource / CMappedFile / CObjectManager) [V]

The `.res` layout is res-format.md (16-byte header `{magic 0x019CE23C, entry count, table offset, names length}`, 0x16-byte
entries, a names block). Three readers share the base part (vt 0x743324, 0x24 bytes: the header at `+4..+0x10`, `+0x14` entries,
`+0x18` names, `+0x1c / +0x20` the acceleration table): `FindEntry` 0x674850 hashes `sum(tolower(c)) % entryCount` and walks the
`+0` chain comparing length + name case-insensitively; `ListFiles` 0x674AC0 (wildcards); `CreateAccelTable` 0x674910 (fixed
index -> entry, "Index out of range / No entry"), `Reset` 0x674810.

- `CResourceFile` (vt 0x743320, 0x34; the functions are still named `CResourceBase::Ctor / Close / OpenFile / CloseFile / Read /
  Seek`): a Win32 `mmio` reader; `OpenFile(name, out size)` 0x6744C0 prefers a **loose file of that name on disk** (mmioOpen into
  `+0x30`) and otherwise seeks the archive handle `+0x24` to the entry; `Read` / `Seek` are entry-relative. Used by the sound
  system (`CSoundManager::Init`, `CMusicSystem::StartStream`, `Mp3Stream_ReadCallback`).
- `CMappedResource` (vt 0x7432E0, 0x58; 0x671280..0x672190, net module): `Open(path, writable)` reads the header and the tables
  with CRT file I/O (`+0x24` handle), `Attach(data)` works on a memory image (`+0x28` base); `OpenFile(name, out size)` returns a
  cached buffer (name hash `+0x44`, pointer hash `+0x30`, refcount `+0x14` of the entry) read from the file or pointing into the
  image; `CloseFile(buffer)` drops the refcount and writes the buffer back when `+0x2c` writable; `FreeEntries` 0x672040. Slot order
  0 dtor, 1 Open, 2 Attach, 3 Release, 4 nop, 5 OpenFile, 6 CloseFile, 7 FreeEntries [I]. `CMappedResource2` (vt 0x743300, 0x6c)
  adds a `CMappedFile` at `+0x58` and `FindEntry` 0x6725E0 returning `view + offset` (zero-copy): the memory-mapped variant used
  for `database.res`, the registries and `aiinfo.res`.
- `CMappedFile` (0x10: file, mapping, view, size): `Open` 0x675770 (`FILE_FLAG_NO_BUFFERING` when the sector size divides the
  page size, "Too large" over 2 GB), `Close` 0x6756D0, `Flush` 0x675A50 (sets the write time).
- `CObjectManager` (vt 0x743338, 0xbc; base of CMapObjectManager, CItemManager, CTextureManager, CMaterialManager) keeps a vector
  of opened archives at `+0xb0..+0xb8` (`{std::string name, CMappedResource*}`): slot 16 `OpenProtoResource(name)` 0x675050
  appends and opens a new archive (1 when already open), 17 `CloseProtoResource` 0x675200, 18 `ClearProtos` 0x675340,
  19 `OpenFile(name, out size)` 0x675440 (first archive that has it; `CFigureManager` loads `.fig` / `.ani` / `.lnk` through it),
  20 `CloseFile(buffer)` 0x675490, 21 `HasFile(name)` 0x6754E0, 22 `FreeCachedFiles` 0x675530. [I slot roles]

## 6. What is not known

Open after the T5 deep read (2026-11-06):

- `LoadSave` builds a list of the units standing on a cell without a passable neighbour and nothing visible uses it [?]
  (Ghidra lost the array); a fix-up of stuck units may hide behind it.
- The 0x28-byte previous-state records `SpawnPartyHeroes` reuses (`{id, pos, quat, stance +0x20, weapon +0x24}`) are inferred from
  the reads only; `DetachHeroes` 0x58FD90 writes them [I].
- `Quest +0x10` (qdb tag 2) is never written by the quest.reg parser and not read by the quest log [?].
- The meaning of the mercenary switches n1 / n2 / n3 / n8 / n9 / n10 / n11 is only what the dialogs test; the dialog side is not read.
- A stored loot record goes to whoever loots the flagged body (4.15): design or a multiplayer bug, not tested [?].
- `Session_LoadSaveSlot` / `Session_SaveToSlot` keep `bz7g` across allods; why that zone is special is not known.

Settled in the T3 pass (2026-11-02; log in `_cpr/claude-re/full/t3-settled.tsv`):

- Zone record kinds come from the `#zone` type word (`MapInfo_Load` 0x45EA20: "game" 0, "edge" 1, "brief" 2) [V]. Zone kind 3 (record
  kind 2) = the 20 "brief" zones of map.txt / map-lmp.txt: bz1g..bz7g, bz8k..bz11k, bz13h..bz18h and bz1mpg..bz4mpg (the villages /
  bases); zone kind 4 (record kind 1) = the 43 "edge" zones (`gz1g_bz1g` ...: the travel screens between zones); the 21 "game" zones
  (gz*) are kind 1 [V] (data: `_cpr/ru-texts/texts-RU/map.txt`, `textslmp-RU/map-lmp.txt`). The saves agree: header +124 = 3 in bz* /
  cz*, 1 in gz* (sav-format.md).
- Hour = `fmod(tick * timeScale + timeBase, 24)` (section 1 row) [V]. The save dwords at +120 / +124 are `+0x13c` lastThunderTick and the
  zone kind `+0x12d64` (save chunk 4) [V].
- `+0x12e00 staticObjectStream` is read: `CWorldClient::LoadZone` appends it to its player stream through the absolute address
  0x7C2D60 (`Game_GetGlobal7C2D60`) [V]. A remote client's own process builds the same stream with its own `CWorldServer::NewZone`
  [I] (the "client loads them from the same `.mob`" of section 4).
- `CWorld +0x5c` is a `RefHolder` (vt 0x73F240) created empty by `CWorld::Init` (`+8` ref = 0); nothing sets its ref, its slot 5
  ("pump") is empty and `AcceptPlayers` on it moves a null ref: a dead placeholder of a removed net driver [I]. `+0x60` = the
  `RefListPair` of the game streams (`CTcpNetDriver::SyncPeers`, net-transport.md 4.9) [V].
- `CObjectDesc`: `+0x3c` = chunk 0xB004 OBJNAME (the editor name, e.g. "Human Kania Hunter F7-1000324"), `+0x4c` = chunk 0xB016 =
  the object's script name (empty in 56527 of 56805 objects of the 200 shipped .mob files; set examples "q1r.5", "z0jq1.1"): it is
  what `CreateObjectFromDesc` gives the object as its name (named hash `+0x12dd0`) [V]. `+0x5c` = chunk 0xB012 (-1 in 7843 objects,
  another object's NID in 4959, any number otherwise): only copied and written back, never used at run time [V for the data, I for
  the use]. `ZoneRecord +0x30` = the `#sky cave` flag (section 3, `caveFlag`) [V]. The `CUnitDesc` item lists: 0xBBBB0005 quest
  items +0x2bc, 06 quick +0x2d0, 07 spells +0x2e4, 08 weapons +0x2f8, 09 armors +0x30c (structs.tsv) [V].
- `World_SelectProgressVideo`: `g_0x78B0EC` = "gz1g", `g_0x7905CC` = "bz7g" get progres1.bik [V].
- `MpFile::WriteContainer` 0x579B80 = (ecx out IStream, edx head word 0x102, stack: u32 version `g_0x790260` (0x74), CMemStream* body),
  `ret 8`; `MpFile::ReadContainer` 0x579830 = (ecx out CMemStream, edx expected head word 0x102, stack: u32 newest version
  `g_0x790260` 0x74, u32 oldest version `g_0x790268` 0x42, source stream), `ret 0xc`; the compressed form starts above `g_0x790264`
  (0x64) [V] (call sites 0x57A01D..0x57A028, 0x579EC2..0x579EDD). The "same as previous image" shortcut has no caller-side buffer in
  these signatures: [?] left as read in the decompile.

## 7. Corrections to earlier notes

| file | wrong | right |
|---|---|---|
| flows.md "Session driver" | `Game_SessionTick` is the per-frame session driver | it only handles a pending `SessionRequest` (kind != 0) once per frame; the per-frame work is in `main` + `CWorldServer::Tick` / `CWorldClient::Tick` (2.1) |
| functions.tsv 0x589ED0 | `GSWorld_Ctor` "construct the full game world object" | it is `CWorldServer::ctor` (vtable `vt_CWorldServer`, `CWorldServer::Reset`); `GSWorld` is the script VM embedded at +0x12874 / +0x12a6c |
| functions.tsv 0x58DCD0 | "day change (0x6D44E0 on +0x70) -> flag 0x20" | `WaterLevels::Tick`: flag 0x20 (msg 0x73) while a water level animates |
| functions.tsv 0x58D670, 0x58B750, 0x58CCD0 .. | "+0x12d64 = the game / net mode (1 / 3 / 4)" | +0x12d64 is the zone kind `g_0x7C2CC4` (1 playable zone, 3 scripted zone, 4 camp): the branches are live, not multiplayer leftovers (2.3, 4.3) |
| game-memory.md "Game speed and time" | +18.4 ticks/s vs "15 ticks a second measured with spell durations: another clock" | one clock: `CWorldServer +0x64` at 55 ms; the game hour is 900 ticks (rate 60), the day 19.8 min; 0x7AF624 is `CWorldClient +0x6c paused`, 0x7AFFC4 the server tick |
| game-memory.md "The current map" | 0x7C2CC8 / 0x7C2CCC are "game strings rewritten in place" | they are `CWorldServer +0x12d68` (the .mob name) and `+0x12d6c` (the .mpr name) of the global world `g_0x7AFF60`; 0x7C2CC4 just before them is the zone kind (`+0x12d64`); 0x7AFE90 is `SessionRequest +0x70` = the request party's zone name (`CParty +0x68`) |
| sav-format.md "The 128-byte header" | +100 "a game time (when the zone was last left?)", +104 "0 or 2", +108 "0 or 40" | +100 = `CWorldServer +0x64` tick, +104 rain kind, +108 rain strength, +112 hour, +116 rate; the "variable part starting with 0x00FF" = the AIGRAPH patch stream (4.8) |
| sav-format.md | "the party's units" are re-created from mission.sav | yes, and `scenario.sav`'s copy is only re-applied when the zone record kind is 1 (camp); confirmed by `Session_LoadSaveSlot` |
| structs.tsv CWorldServer +0x2838 stimulusMap | a separate field | it lies inside the embedded `CAIMap +0x170` (+0x26c8); drop it when the +0x170 row is merged |
| multiplayer-protocol.md 0x02 (server -> client) "unknown" | | the zone state blob (4.7); 0x70 / 0x71 / 0x72 / 0x73 = rain / time / wind / water levels |
| items.md / spells.md "CWorldServer::CloneItem" | | unchanged; note that the regenerating clone goes to `+0x12e44` whose entries tick in `TickActiveObjects` |
| world-server.md 2.4 / summary (T5 deep read 2026-11-06) | visited flag `z.<zone>`, last visit `<prefix><zone>`; catch-up `clamp(gtime - last, 0, 1000)` ticks | the variables are `zs.<zone>` (= 1) and `zt.<zone>` (= gtime); catch-up `min(1000, (gtime - zt) * 100)` ticks: 100 ticks per game hour away (x87 `fmul 100.0` at 0x57A506) |
| world-server.md 2.1 / 4.3 | activation radius `min(sight x mult, life-sense) * 2 + 20` | `max(...)`: the branch-free select at 0x58E0A5..0x58E0D0 keeps the larger value (as the replication radius already said) |
| world-server.md 2.5 | `scenario.sav` = UI stream, camera, party store; every visited zone file is copied | order = party store, `CInterface` save state, embedded UI stream, camera (writer and reader agree); only zones of the current allod (and `bz7g`) are copied, the others lose their `zs.` flag |
| world-server.md 4.8 | `ResetUnitEffects` runs before every save; "the zone kind itself is not saved" | `ResetUnitEffects` is called only by `Session_SaveZoneToCurrent` (a slot's `mission.sav` keeps the temporary effects); the zone kind is written and read back into `+0x12d64` (0x59716E) |
| world-server.md 4.8 row 15 | per player: `+0xfc`, party, tracked items, byte `+0x108`, (`+0x120`), list `+0x118` | `+0xfc`, party, dword `+0x100`, loose item ids of the CList `+0xe0`, byte `+0x108` godMode, the synced id list `+0x114` (count `+0x120` + ids) |
| world-server.md 4.4 | 0xAA01 = objects (0x68) | 0xAA01 = light descriptors (0x68, kind 2); objects are 0xB001 (`CObjectDesc` 0x8c); 0xCC01 sound 0x88, 0xDD01 particle 0x5c, 0xBBBF0000 torch 0xa0 |
| world-server.md 2.3 / 2.6, functions.tsv 0x57BDF0 | `SetRestrictFlag(record +0x30)` | `SetCaveFlag` 0x58CC70 (`#sky cave`); `Session_HostMpZone` also renames every exit target to "MPGame1" and loads the quest `.mob` only with a live lobby server |
| spells.md 4.3 | possession 0x15 / link 0x1B / charm 0x24 "need zone kind 1 or no caster"; "an explicit unit target gets the first cast" | `CastSpellMulti` refuses those three kinds for every caller, and the zone-kind-1-or-no-caster rule applies to every spell (disassembly 0x58E874..0x58E8B2); the extra first cast is for point-target spells without `ic`, at the point; the given unit is only put first in the nearest-first list (4.11) |
| data-sound.md 4.1 (Quest) | exp `+0xc` int, money `+0x10`, zone index `+0x14`, order `+0x18` | `ParseQuestReg` stores exp `+0xc` and money `+0x14` as floats, zone index `+0x18`, order `+0x1c`; `+0x10` (tag 2) stays 0; the quest log reads `+0xc` / `+0x14` as floats (2.7) |
| data-sound.md 4.2 (Briefing) | exp / money int; available zones set `z.<zone>` = 2; remove items "released from the party" | exp / money floats; `+0x2c` and `+0x44` names are raised to 1 (`SendMsg81_MaxValue`), `+0x38` / `+0x50` set to 2; the remove list removes only quest items (type 0x14) (4.14) |
| gs-functions.md 0xD2, modules.md 0x591800 | merc flags `b.<zone>.nK_%i` | `b.merc<i>.nK_<i>`: the `%s` is the string `merc<i>` (4.13) |
| items.md 5 / structs.tsv HeroRecord +8 | `+8` class ("Hero") | `+8` = the race model's Mask Name (figure stem "unhuma"; `InitHeroFromProto` copies `UdbRaceModel +0xe8`), used as the hero unit's model by `SpawnPartyHeroes`; `+4` is the hero key name hashed into the id (mercenaries: "merc<i>", 4.13) |
| structs.tsv UnitProto +0x108 / +0x114 | bloodKind / hitEffectFlag | the end-of-storage words of the spells (`+0x100`) and wears (`+0x10c`) vectors; blood / cast type are `UdbRaceModel +0x108 / +0x114` |
| world-client-players.md day light | a third section "(sky / fog)" | the section is named "sky"; keys `hourNN` must be ascending, each interpolated from the previous key, hours after the last key keep old values |
| world-client-players.md Update | "print colours re-lit from the lights" | only the blood prints are relit (grey of the max luminance: the sun at the camera ground point, then every non-sun light whose radius covers the print) |

## 8. Function index (addresses)

Session: `Session_ProcessRequest` 0x57C700, `Game_EnterZone` 0x57C460, `Session_EnterZoneFromParty` 0x57A060,
`Session_SaveZoneToCurrent` 0x57A780, `Session_LoadSaveSlot` 0x57ABA0, `Session_SaveToSlot` 0x57B3B0 (wrapper 0x581610),
`Session_ReadMissionSave` 0x579E60, `Session_WriteMissionSave` 0x579FB0, `Session_HostMpZone` 0x57BDF0, `Session_SetMapName`
0x57ED60, `SessionRequest::RequestMpMapChange` 0x57EE10, `Session_RequestMapChange` 0x578AC0, `SessionRequest::ctor` 0x578460,
`SetLoadSave` 0x5816D0, `SetEnterZone` 0x581700, `SetLeaveToLobby` 0x581730, `SetHostZone` 0x581760, `Clear` 0x5816C0,
`Session_StartNewGame` 0x57BD10, `Session_LeaveGameToMenu` 0x579420, `Session_PlayRecordedStream` 0x5793E0,
`MpFile::ReadContainer` 0x579830, `MpFile::WriteContainer` 0x579B80, `Dir_CreateAndClear` 0x57AAB0, `Databases_LoadAll` 0x57CB50,
`Databases_Reload` 0x57DEE0, `Lobby_StartServerOrClient` 0x57E740, `Lobby_ResetMpGameState` 0x57F280, `Lobby_IsAlive` 0x57F440,
`Campaign_AdvanceMission` 0x5800C0, `World_SetStatScramble` 0x5790E0, `World_XorStats` 0x5791C0, `Game_SetServerPaused` 0x578D10.

CWorld: `ctor` 0x55EAB0, `CreateManagers` 0x55EE40, `Done` 0x55F8E0, `dtor` 0x55EC80, `SetTime` 0x55FB30, `SetTimeScale`
0x55FB50, `GetHourAndRate` 0x55FBD0, `IsNight` 0x55FC10, `NightFactor` 0x55FC50, `SetWindFields` 0x55FCB0, `CloneItem` 0x55E940,
`FindUnitById` 0x55FB00, `CWorld::ResetNetDriver` 0x55F410, `ReleaseRef60` 0x55F890, `World_MapPath` 0x55E990, `World_MapFilePath`
0x595BA0, `WaterLevels::Serialize` 0x560200, `WaterLevels::SetTarget` 0x6D4410, `WaterLevels::Tick` 0x6D44E0.

CWorldServer: `ctor` 0x589ED0, `Init` 0x58AE10, `Done` 0x58B020, `Reset` 0x58B600, `ClearObjects` 0x58B250, `ClearEffects`
0x58B150, `dtor` 0x58AAD0, `Tick` 0x58DFF0, `Update` 0x58DCD0, `TickActiveObjects` 0x58D670, `ActivateObject` 0x58D5D0,
`ActivateObjectsAround` 0x58D3D0, `ActivateLogicSlot` 0x593FC0, `AcceptPlayers` 0x58DAF0, `SetMode` 0x58DC90, `SetPaused`
0x5942C0, `ReplicateToPlayer` 0x58B750, `SendZoneState` 0x58C890, `SendPlayerPosition` 0x58CBB0, `WriteUnitHashes` 0x58C4F0,
`FlagAllPlayers` 0x58B6D0, `BroadcastGameTime` 0x58D280, `BroadcastMsg0B` 0x58D340, `UpdateEnvironment` 0x58CCD0,
`SetWeatherMode` 0x58CC80, `CWorldServer::SetCaveFlag` 0x58CC70, `SetRain` 0x58EE00, `SetWind` 0x58EE20, `SetTime` 0x58EEA0,
`SetTimeScale` 0x58EEE0, `GetHeight` 0x594220, `GetTerrainCell` 0x58A390, `FindObjectById` 0x593AA0, `FindPlayerById` 0x594180,
`CreateObjectFromDesc` 0x5927C0, `RemoveObject` 0x5935C0, `RemoveAllObjects` 0x596C60, `RemoveAllUnits` 0x5939E0,
`AddEffectObject` 0x58E520, `RemoveEffectObject` 0x58E5D0, `CastSpell` 0x58E670, `CastSpellMulti` 0x58E850, `LoadZone` 0x595480,
`NewZone` 0x595C70, `LoadMapObjects` 0x596030, `World_LoadAIGraphStream` 0x596140, `SeparateStaticObjects` 0x5951A0, `LoadMob`
0x595AB0, `SetZone` 0x595B50, `LoadWorldScript` 0x596350, `UnloadZone` 0x5957E0, `BeginAIMapBatch` 0x595A70, `EndAIMapBatch`
0x595A90, `World_SelectProgressVideo` 0x595080, `SetZoneExits` 0x5915E0, `GetZoneExit` 0x58E4C0, `WriteSave` 0x597BD0,
`LoadSave` 0x596E70, `WriteObjectIds` 0x5965F0, `WriteObjectNames` 0x596730, `ReadSaveObjects` 0x596900, `ResetUnitEffects`
0x5956E0, `LootRecord::ReadItems` 0x589CA0, `PlayerIn` 0x590700, `PlayerOut` 0x590A40, `SpawnPartyHeroes` 0x58EF20,
`SpawnSingleHero` 0x5910A0, `DetachHeroes` 0x58FD90, `GivePartyItemsToPlayer` 0x590460, `GiveItemsToPlayer` 0x58FBF0,
`MoveTrackedItemsToParty` 0x590160, `FilterQuestItems` 0x5901E0, `StorePartyFromUnits` 0x590C70, `ClearPartyItems` 0x591070,
`GiveExperience` 0x5911C0, `CountOtherLiveHeroes` 0x591740, `UpdateMercenaryFlags` 0x591800, `OnMercenaryEvent` 0x591DE0,
`OnNamedEvent` 0x592090, `LootUnit` 0x5942D0, `LootAllDeadUnits` 0x593D60, `HasLootRecordFor` 0x5949D0, `CollectLootUnitsFor`
0x594A30, `CheckUnitsPlacement` 0x593B80, `OnUnitDied` 0x593AF0, `CloneItem` 0x594CE0, `AddItemToUpdateList` 0x594F30,
`CreateItemByName` 0x594AB0, `CreateSpellItemByName` 0x594BC0, `Unit_ConsumeItemCharge` 0x595020, `CWorldServer::SpawnConsumableUnit` 0x58ECE0,
`CopyHero` 0x5947E0, `SetWaterLevel` 0x593FA0.

Databases and records read with this page (their main text is data-sound.md / items.md / world-client-players.md):
`UnitsDb::Load` 0x53A200, `UnitsDb::CountSoundVariants` 0x53A8E0, the units-database STL helpers 0x53F530 / 0x53FCC0 / 0x541960 /
0x542830 / 0x542FC0, `QuestsDb::ParseQuestReg` 0x6AE8E0, `HeroRecord::Read` 0x65F9B0, `CWorldClient::Init` 0x561E50 / `Done`
0x563290 / `Update` 0x5641F0 / `LoadDayLightTable` 0x563930, `CBridgeManager::AddBoxFaces` 0x560820.

Descriptors and the mob loader: `MobFile_Load` 0x481660, `MobFile_ParseChunks` 0x481F00, `CObjectDesc::ctor` 0x46A5C0 / `OnChunk`
0x46AB20 / `Spawn` 0x46AFB0, `CUnitDesc::ctor` 0x46C010 / `OnChunk` 0x46D080 / `Spawn` 0x46C4D0, `CMagicTrapDesc::ctor` 0x46B4A0,
`CLeverDesc::ctor` 0x46BD40, `CTorchDesc` 0x46B210.

Resources: `CResourceBase::InitBase` 0x674790, `Reset` 0x674810, `FindEntry` 0x674850, `AccelIndex` 0x6748F0, `CreateAccelTable`
0x674910, `FreeAccelTable` 0x674A90, `ListFiles` 0x674AC0, `WildcardMatch` 0x674CE0; `CResourceFile` (named CResourceBase::)
`Ctor` 0x674280, `dtor` 0x6742C0, `Close` 0x674430, `OpenFile` 0x6744C0, `CloseFile` 0x674670, `Read` 0x6746B0, `Seek` 0x6746F0;
`CMappedResource::ctor` 0x671280, `Open` 0x671730, `Attach` 0x671A40, `Release` 0x671AC0, `OpenFile` 0x671B40, `CloseFile`
0x671E00, `FreeEntries` 0x672040, `CMappedResource2::ctor` 0x6721D0, `FindEntry` 0x6725E0; `CMappedFile::Open` 0x675770, `Close`
0x6756D0, `Flush` 0x675A50; `CObjectManager::CObjectManager` 0x674D90, `dtor` 0x674DF0, `OpenProtoResource` 0x675050,
`CloseProtoResource` 0x675200, `ClearProtos` 0x675340, `OpenFile` 0x675440, `CloseFile` 0x675490, `HasFile` 0x6754E0,
`FreeCachedFiles` 0x675530, `CFigureManager::Done` 0x674F40.

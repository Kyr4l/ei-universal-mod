# The client world and the players in game.exe (CWorldClient, CPlayer, CPlayerClient, CPlayerServer: the in-game protocol)

Written 2026-10-27 from the typed Ghidra decompiles of the Russian `EIStarter OBT-1/Engine/game.exe` (modules 0x560000-0x579000
CWorldClient / CBridgeManager / CWeather, 0x664000-0x672000 CPlayer / CPlayerClient / CPlayerServer / CPlayerManager; the handful of
label-only handlers were read with `objdump`). The server world (CWorldServer, ReplicateToPlayer, PlayerIn / PlayerOut, SendZoneState),
the game session and the resource files are documented by the parallel server page; here they appear only as entry points. The unit side
of the orders is in `combat.md` (section 3) and `unit-ai.md` (AiLogicPlayer); the delta codec, the 0x40 record and the 0x41 sections are in
`docs/multiplayer-protocol.md` (sections 4 and "Unit creation"). Batch files: `_cpr/claude-re/full/batch-players.tsv` (271 rows) and
`batch-players-structs.tsv` (147 rows).

Tags: **[V]** read directly in the decompile / disassembly, **[I]** inferred from several functions, **[?]** guess.
Addresses are VAs; offsets are bytes; "tick" = `CWorld +0x64` (18/s on the server at normal speed).

## How to re-implement it (short version)

1. **One link, two LZ streams.** Each direction is a sequence of *messages* `{u32 compressed, u32 raw, LZ bytes}` (the .mp LZ with one
   window kept per direction). A message is a run of *sub-messages*: one opcode byte + a payload whose length only the handler knows.
   Client: `CPlayerClient::Update` 0x667A30 pops a packet from the net stream, inflates it into `+0x34` and `DispatchPackets` 0x667970 walks
   the 256-entry table built by `Init` 0x667230. Server: `CPlayerServer::Update` 0x66D270 does the same into the global packet stream
   `g_0x7C83D0` with the table of `Init` 0x66C9A0. Outgoing bytes are appended to a send `CMemStream` (`+0x5c` client, `+0x48` server) and
   `FlushSendStream` (0x667810 / 0x66D100) compresses and ships it. [V]
2. **Join:** client `0x08` (optional party upload) + `0x00` identity -> server `0x00` {player id} + `0x80` party -> state 1; the server's
   replication then sends `0x02` zone state (the client runs `LoadZone` and answers `0x09`), `0x40` for every unit near the view point,
   `0x50` effects, `0x60` party status, `0x07` game mode, `0x71` time, `0x72` wind, `0x70` weather, `0x73` db. `0x09` makes the server spawn the
   hero (`World::PlayerIn`). From then on the client sends `0x40` (view point) every frame while in world (state 2). [V]
3. **Per tick (server):** `CWorldServer::ReplicateToPlayer` 0x58B750 diffs the non-unit objects within 100 m of the view point (plus the visible units,
   the named objects and the units the party sees) against the player's tracked array: `0x42` remove, `0x40` create, `0x41` delta; the same
   for effects (`0x52` / `0x50` / `0x51`); then `0x60` party status, the periodic `0x80` party resend (every 900..1800 ticks) and whatever
   dirty bits of `CPlayer +0x130` are set (`0x02`, `0x70`, `0x71`, `0x72`, `0x07`, `0x73`). [V]
4. **Orders** are plain client sub-messages carrying unit id lists: `0x30` move, `0x3A` move in formation, `0x31` attack, `0x32` / `0x33` cast,
   `0x36` interact (talk / use / loot), `0x34` weapon, `0x35` stance, `0x39` leader, `0x3B` field 0x438. The server resolves the units, skips dead
   ones (flag 0x800000) and calls the `AiLogicPlayer` entry (`MoveToNow` 0x5D39E0, `MoveTo`, `AttackTarget` 0x5D3C30, `CastOn` 0x5D3D20,
   `CastAt`, `Interact`, `SetLeader`) or `CUnitServer::RequestWeapon` 0x5552E0 / `RequestStance` 0x555270. Feedback comes back as `0x0B`
   {unit, code} and, for moves, `0x0E` with the computed path. [V]
5. **The client world** (`CWorldClient`, the static instance `g_0x7AF5B8`) owns the object list `+0xb8`, two 16-unit grids, the hero list,
   the picking records, the bridge quads, the print lists, the day-light tables and the local `CPlayer +0x20c` whose `+0x20` is the
   `CPlayerClient`. `LoadZone` 0x567AB0 consumes the `0x02` payload; `Update` 0x5641F0 pumps the net and ticks the visible objects; `Render`
   0x569E40 -> `RenderObjects` 0x56AFE0 -> `ClassifyCellObjects` 0x56A990 culls the grid cells around the camera into draw / shadow lists. [V]

## 1. Conventions

| Thing | Value |
|---|---|
| player object | `CPlayer` (0x134, kind 0x300a, typeId 0xf, vtable 0x743104, 13 slots) made by `CPlayerManager::CreatePlayerWithConnection` 0x66C180 together with its *connection* (kind 0x49): `CPlayerClient` (0x50f8, vt 0x743180) on the client, `CPlayerServer` (0x5128, vt 0x743280) on the server [V] |
| connection slots (callbacks from CPlayer) | 2 Init, 3 Done, 6 SendClientState (client) / unused (server), 13 OnTrackedCleared, 14 OnUnitTracked (client `OnUnitAdded`), 15 Reset (net stream set), 16 Disconnect, 17 SendItems, 18 ClearItems, 19 RemoveItem, 20 Respawn (server), 21 ClearKnownObjects (server), 22 SendItemValue (server), 23 Update [V] |
| net stream | `CPlayer +0x1c`: an `IMsgStream` (TcpMsgStream 0x73CA10, PeerMsgStream 0x73C990, FileMsgStream 0x73C8F8 for recorded packets); slots 4 ready, 6 HasPacketHeader, 7 CountPackets, 8 SendBuffered, 9 PopPacket (net-notes.md) [V] |
| message framing | `MsgStream_Compress` 0x47D7F0 writes `{u32 raw length}` + LZ bytes and `MsgStreamBase::v8_SendBuffered` 0x47BAD0 adds the u32 frame length; `MsgStream_Decompress` 0x47D970 inflates with the receive `LzCompressor` (corrected by net-transport.md 7; nothing is at 0x4A5790); empty keep-alive = `04 00 00 00 00 00 00 00` (multiplayer-protocol.md) [V] |
| strings in payloads | C strings with their NUL (`SendMsgString`, `Stream_ReadCString` 0x4A48F0); "embedded stream" = `Stream_WriteEmbeddedStream` 0x4A5250: u16 `len*2` or u32 `len*2+1` (>= 0x7531) then the bytes; "length-prefixed blob" = `MsgStream_WriteLenPrefixed` / `ReadLenPrefixed` 0x47DAE0: 1 or 4 byte length (bit 0 = long), max 100000 [V] |
| ids | object ids (`CObject +0xc`); heroes 0x3B9ACA0D.. (1 000 000 013); `IsHeroId` 0x5F8B20 marks them; a respawned hero gets the first free id >= old + 250000 [V] |
| flags used here (`CObject +8`) | 1 deleted, 2 net dirty (PackDeltaNetState before sending), 0x10 selected by the local player (`SelectObject` 0x566030; the UI iterates units with it), 0x20 inside the view frustum, 0x100 ghost render mode (message 0x19), 0x200 relit periodically (lighting cache expires), 0x400 lighting cache valid (`Draw` sets it with render specific `+0xEC` = `timeGetTime`), 0x400000 visible to the local party (the unit list of message 0x60: party units and what they see; corrected 2026-11-06, was "party unit"), 0x800000 dead (orders skip it) [V] |
| type ids (`vt[12]`) seen in the client lists | 0x50 CUnit, 0x52 CTree, 0x54 CBuilding (`GetTypeId` 0x529820) and 0x5d CLeverObject (0x52B2F0) = the objects with walkable parts (`ActivateObjectBridges`; no other vtable reuses these two slots), 0x44 CFigure, 0x4f CSpell [V] |
| the game-state globals | `g_0x7C83D0` server packet stream (+5 ok byte), `g_0x7AFF60` CWorldServer (its +0x64 = `g_0x7AFFC4` tick), `g_0x7AF5B8` CWorldClient (+0x64 = `g_0x7AF61C`), `g_0x79B2A8` CCamera, `g_0x7C7D50` CInterface, `g_0x79B5D8` lobby client, `g_0x7B0098` party store mode (1 local store, 2 session hash, 3, 4 none) [V] |

## 2. The link

**Client side** (`CPlayerClient`): `SendClientState` 0x667680 (slot 6, every frame from `CWorldClient::Update`): state 0 and the net stream
ready -> when `+0x85` (upload pending) writes `0x08` + `MpFile::Write(party +0x70, full)`, then `0x00` + the identity block, state = 1; state 2
-> `0x40` + the active camera position (x, y as two floats, `CObject3D::GetActiveTransformPtr(g_0x7AF7C8)`); both end with `FlushSendStream`.
`Update` 0x667A30 (slot 23): `netStream vt[6]`, `vt[9](packet)`, inflate into `+0x34`, `DispatchPackets`; returns 0 on a parse error and
`CWorldClient::Update` then `FatalError("CWorldClient::Segment... Disconnected")`. The receive stream's ok byte `+0x39` is cleared by a short
read: every handler that returns 0 ends the session with `NetFatalError` 0x664B70 ("Error during parsing update prevID %x curID %x"). [V]

**Server side** (`CPlayerServer`): `Update` 0x66D270 (slot 23, every tick) pops packets while the state is not 3 (disconnected) and dispatches
them through `g_0x7C83D0`; the handlers read with `g_0x7C83D0->vt[1](buf, n)`. `FlushSendStream` is called by `ReplicateToPlayer` at the end of
the tick and by `ChangeMap` / `Disconnect`. [V]

States: client `+0x20` 0 handshake, 1 identity sent, 2 in world (after server `0x00`), 3 leaving (after `0x04` until `0x05`); server `+0x20` 0 new,
1 in game, 2 left the world (after client `0x04`), 3 disconnected / refused, 4 hero uploaded (awaiting `0x09`). [V]

## 3. Sequences

**Join** [V, `OnMsg00_Identity` 0x66F890, `OnMsg08_HeroUpload` 0x670F50, `OnMsg09_Ready` 0x671060, `LoadZone`]:

```
C: [08 + party (MpFile, zone := quest)]  00 {u32 100, name[64], password[64], u32 partyId}
S: version != 100 -> 11 "Wrong Client Version..." (state 3, link dead)
S: 00 {u32 player object id}  80 {party}            state 1 (party from the store / session hash / cleared)
S: (ReplicateToPlayer) 02 {zone state}  40.. 50.. 60  07 {mode, tick}  71 {hour, rate}  72 {wind}  70 {weather}  73 {db}
C: (LoadZone done) 09                                 -> server state 4 -> 1, World::PlayerIn: 04 {camera}, 40 {hero}, 60, 80 ...
C: 40 {x, y} every frame
```

**Leave / zone change** [V, `OnMsg04_Leave` 0x670CE0, `SendMsg03_ChangeZoneById` 0x66B140, `ChangeMap` 0x66F310, `OnMsg05_EnterWorld` 0x670DA0]:
client `04 {mode}` (state 3) -> server `World::PlayerOut` (sends `83 {party}`), `80 {party}`, `07 {mode, tick}`, state 2. The next client order
that needs the world (`SendMsg03` / `SendMsg06`) first uploads `50 {party}` and sends `05` (server: state 1, `World::PlayerIn` with the spawn
records `+0x74`), then `03 {exit index}` or `06 {map, entry}`: the server `ChangeMap` sets state 3, stores the party, and answers
`08 {map name, entry}`; the client `MpCommand::SetKind4` loads the map and connects again. A hero standing in a zone exit rectangle
(`CWorldClient::FindZoneExitAt` 0x567440) is what the UI uses to send `03`.

**Respawn** [V, `CPlayerServer::Respawn` 0x66F4C0 = slot 20, from `ChangeMap` when the hero is dead or missing]: outside a session `09` then
`0B {hero id, 0xFF}`; in a session the money is cut by `g_0x7C5490`, `0B {old id, 0xFF}`, the hero is copied (`World::CopyHero`) under a free id
>= old + 250000, healed (`UnitStats_HealAllParts`), its experience reduced by `g_0x7C5494 * (exp - prototype +0x54)`, `80 {party}`, PlayerOut /
PlayerIn, and the old unit's seen list is copied to the new one.

**Disconnect** [V]: the server's `Disconnect` 0x66D020 (slot 16, also client opcode `01`) writes the single byte `01` and flushes; the client
`OnMsg01_ClearNetStream` 0x667DE0 drops its net stream. Party saves: every `0x80` the client receives rewrites the character file
(`Lobby_SavePartyToFile` 0x57F0F0) and rebuilds the synced-item list (`+0x114`).

## 4. Client -> server (server table `CPlayerServer::Init` 0x66C9A0, handlers read `g_0x7C83D0`)

| op | name | payload | sender (client) | handler (server) | effect |
|---|---|---|---|---|---|
| 00 | Identity | u32 version (100), char name[64], char password[64], u32 party id | `SendClientState` 0x667680 | `OnMsg00_Identity` 0x66F890 | state 0 only; bad version -> `11` + text, state 3; the party is fetched (`PartyHash_GetParty` with name / password / `+0xfc`) -> `00 {id}`, `80`, state 1 |
| 01 | Disconnect | - | (`Disconnect` on the server side writes it too) | `OnMsg01_Disconnect` 0x66FA90 | `vt[16]` Disconnect: PlayerOut, `80` in session mode, byte `01` back, state 3 |
| 02 | RequestParty | - | `SendMsg02_RequestParty` 0x669710 (also starts the packet recording file) | `OnMsg02_RequestParty` 0x66FAA0 | `CPlayer::ResetReplication` (dirty bits = -1) + `80` |
| 03 | ChangeZoneById | u32 zone exit index | `SendMsg03_ChangeZoneById` 0x66B140 (`Session_RequestMapChange` 0x578AC0) | `OnMsg03_ChangeZoneById` 0x670C20 | `CWorldServer::GetZoneExit` -> `ChangeMap(name, entrance)` -> `08` |
| 04 | Leave | u32 game mode | `SendMsg04_Leave` 0x66B280 (`Game_SendLeave` 0x578B00) | `OnMsg04_Leave` 0x670CE0 | state 1 only: `+0x100` = mode, `World::PlayerOut` (-> `83`), `80`, `07`, state 2 |
| 05 | EnterWorld | - | `SendMsg05_EnterWorld` 0x66B2C0 (after a leave, preceded by `50`) | `OnMsg05_EnterWorld` 0x670DA0 | state 2 only: state 1, `World::PlayerIn(spawn records +0x74)`, dirty bit 0x10 (`07` goes out) |
| 06 | ChangeMapByName | C string map, u32 entry | `SendMsg06_ChangeMapByName` 0x66B1B0 (`MpState::LoadMap`, `Game_SendString`) | `OnMsg06_ChangeMapByName` 0x670E00 | `ChangeMap` |
| 07 | NamedEvent | C string | (scripts / UI through the connection) | `OnMsg07_NamedEvent` 0x670EB0 | `Script::NamedEventSource(world, player index, name)` |
| 08 | HeroUpload | the party (`MpFile::Read` full) | `SendClientState` when `+0x85` | `OnMsg08_HeroUpload` 0x670F50 | exactly one hero: `NextUniqueId(+0xfc)`, hero unit id = next free world id, `PartyHash_SetParty`; else 0 (link dead) |
| 09 | Ready | - | `SendMsg09_Ready` 0x66B300 (end of `LoadZone`) | `OnMsg09_Ready` 0x671060 | state 4 only: state 1, first time `World::PlayerIn(player, 0, 1)` |
| 10 | Pause | u8 paused | `SendMsg10_Pause` 0x66B040 (`CWorldClient::SendPause` 0x566330, clock button `CBattleBar2`) | `OnMsg10_Pause` 0x66FAC0 | `CWorldServer +0x6c` = paused; in a session the host's owner posts chat line 0xC |
| 30 | Move | u32 n, n x u32 unit ids, u32 flag, f32 x, y, z | `SendMsg30_Move` 0x66A7D0 (ground click) | `OnMsg30_Move` 0x66FFF0 | `Formation_ComputeOffsets` 0x66FD70 (centroid + unit direction, zero for one unit), `AiLogicPlayer::MoveToNow(brain, pos, run = n > 1, flag, offset)` per live unit |
| 31 | Attack | u32 n, n x u32 ids, u32 a, u32 target id, u32 hit location | `SendMsg31_Attack` 0x66A880 (called-shot menu `ApplyChoice`, click) | `OnMsg31_Attack` 0x670190 | `CWorldServer::FindObjectById(target)`, `AiLogicPlayer::AttackTarget(brain, target, hitLocation, 1)` per unit; refused when the target is not in the unit's known list |
| 32 | CastAt | u32 spell slot, u32 caster id, f32 x, y, z, u8 flag (21 B) | `SendMsg32_CastAt` 0x66ACD0 / `SendMsg32_UseWeaponAt` 0x66ADD0 (slot \| 0x80000000: a weapon-slot scroll / charged item, its charge is decremented locally) | `OnMsg32_CastAt` 0x6708D0 | live caster, `vt[55]` dead == 0: `AiLogicPlayer::CastAt -> QueueCastAt` (combat.md 3) |
| 33 | CastOn | u32 spell slot, u32 caster id, u32 target id, u8 flag (13 B) | `SendMsg33_CastOn` 0x66AC70 / `SendMsg33_UseWeaponOn` 0x66AD40 | `OnMsg33_CastOn` 0x670820 | `AiLogicPlayer::CastOn -> QueueCastOn` |
| 34 | SelectWeapon | u32 unit id, u8 slot | `SendMsg34_SelectWeapon` 0x66AE70 (weapon buttons, keys; also `CUnit +0x270` locally) | `OnMsg34_SelectWeapon` 0x670960 | `CUnitServer::RequestWeapon(slot)` |
| 35 | SetStance | u32 unit id, u8 stance | `SendMsg35_SetStance` 0x66AED0 (keys, `CUIWidget7424FC`; `CUnit +0x268` locally) | `OnMsg35_SetStance` 0x6709D0 | `CUnitServer::RequestStance(stance)` (walk / run / sneak / battle) |
| 36 | Interact | u32 n, n x u32 ids, u32 a, u32 interaction id, u32 b, u32 target id | `SendMsg36_Interact` 0x66AB80 (selection; b = -1), `SendMsg36_InteractOne` 0x66AA20 (one unit: talk / use / loot from `IssueCommandAt`) | `OnMsg36_Interact` 0x670310 | with several units the nearest that can reach the target (`CanReachOrderTarget`, `CAIMap::FindPath`); `AiLogicPlayer::Interact(brain, target, interaction, b, a != 0)`; 0x18A95 = loot |
| 37 | - | - | - | `OnMsg37_Nop` 0x670810 | nothing |
| 38 | unused | u32 unit id, u32 value | `SendMsg38` 0x66AF90 (not a Ghidra function; no call or pointer anywhere in the exe) | `OnMsg38_Unused` 0x670A40 | reads both words (from the packet stream `g_0x7C83D0`), resolves the unit, nothing: a removed feature at both ends [V] |
| 39 | SetLeader | u32 unit id, u32 leader id | `SendMsg39_SetLeader` 0x66AFE0 (`IssueCommandAt` on the selected units) | `OnMsg39_SetLeader` 0x670A90 | `AiLogicPlayer::SetLeader(brain, leader or 0)` (follow / regroup) |
| 3A | MoveFormation | as 30 | `SendMsg3A_MoveFormation` 0x66A720 (`SendOrder` with the formation option) | `OnMsg3A_MoveFormation` 0x66FBD0 | `AiLogicPlayer::MoveTo(logic 6, ...)` (the queued variant) |
| 3B | SetField438 | u32 unit id, u8 v | `SendMsg3B_SetField438` 0x66AF30 (`CUIWindow742530::OnSelect`) | `OnMsg3B_SetField438` 0x66FB70 | `CUnit::SetField438` (net section 13) |
| 40 | ViewPoint | f32 x, f32 y | `SendClientState` every frame in state 2 | `OnMsg40_ViewPoint` 0x670B20 | `+0x38 / +0x3c` = the replication centre (100 units) |
| 50 | PartyUpload | embedded stream (`MpFile::Write(party, 0)`) | `SendMsg50_UploadParty` 0x66B080 | `OnMsg50_PartyUpload` 0x670B60 | `MpFile::Read(player party, 0)` |

Unknown opcodes, a missing handler or a handler returning 0 kill the link (`NetFatalError` "Serv wrong packet ID / packet has no handler /
error during parsing command"). [V]

## 5. Server -> client (client table `CPlayerClient::Init` 0x667230)

| op | name | payload | sender (server) | handler (client) | effect |
|---|---|---|---|---|---|
| 00 | PlayerId | u32 player object id | `OnMsg00_Identity` | `OnMsg00_PlayerId` 0x667DA0 | state = 2, `CPlayerManager::SetPlayerId` |
| 01 | ClearNetStream | - | `Disconnect` | `OnMsg01_ClearNetStream` 0x667DE0 | `CPlayer::ClearNetStream` |
| 02 | ZoneState | length-prefixed blob: u32 tick - 1, zone name, map name, u32 n + n zone exit records (0x3c: 9 x u32 with the rect at +0x14..+0x20, name, u32 entrance, u32, u8, u32, f32 rotation), u32 indoor, the unit save records (`UnitHashes_WriteSave`) | `SendMsg02_ZoneState` 0x66EBD0 (`CWorldServer::SendZoneState` 0x58C890, dirty bit 1) | `OnMsg02_ZoneState` 0x669010 | `CPlayer::ClearAll`, `CWorldClient::LoadZone` (section 8), the rest of the message is re-queued, `09` sent |
| 03 | SpellsAndItems | length-prefixed blob | (server save / zone path) | `OnMsg03_SpellsAndItems` 0x6690F0 | the whole spell and item managers read (`ReadSpellsAndItems` 0x567A60) |
| 04 | CameraAt | f32 x, y, z, u32 unit id, u32 flag (20 B) | `SendMsg04_CameraAt` 0x66ED30 (`PlayerIn`, `SendZoneState`, `SendPlayerPosition` 0x58CBB0, `StartDialogLine`) | `OnMsg04_CameraAt` 0x667C20 | `CameraLookAt(xy)`, preset view 6 / 7 (`CCamera::LoadView`) or the "gz20g" reset, yaw 4 |
| 05 | ItemObjects | length-prefixed blob: object lists {u8 1, u32 id, u32 type, u32 +0x18, object `vt[10]` data}... u8 0, first the spells then the items | `SendMsg05_06_Items` 0x66E7D0 (slot 17, from `CPlayer::AddItems`: loot, shop) | `OnMsg05_ItemObjects` 0x6691C0 | `ReadItemObjects` 0x567950: `CBaseManager::ReadObjectsFromStream` 0x5679A0 creates the objects in the spell then the item manager |
| 06 | AddItems | u32 money, u8 flag, u32 item ids ... u32 -1 | same, right after `05` | `OnMsg06_AddItems` 0x669490 | money `+0xc0`, `CPlayer::AddItems(list, flag)`, inventory UI `FwdTopVt70(list, money delta, flag)` |
| 07 | GameMode | u32 game mode, u32 world tick | `SendMsg07_GameMode` 0x66F180 (dirty bit 0x10), inline in `OnMsg04_Leave` | `OnMsg07_GameMode` 0x669D50 | `+0x104 = +0x100`, `+0x100` = mode, `CInterface::CreateScreenById(mode)`, `CWorldClient +0x64` = tick, mode 3 reloads the mouth textures, party units re-announced to the UI |
| 08 | ChangeMap | C string map, u32 entry | `ChangeMap` 0x66F310 | `OnMsg08_ChangeMap` 0x669EB0 | `MpCommand::SetKind4(g_0x7AFE20, name, entry)`: the client loads the map |
| 09 | TopScreenVt7C | - | `Respawn` (no session) | `OnMsg09_TopScreenVt7C` 0x669F30 | `CInterface::FwdTopVt7C(0)` (respawn screen) |
| 0A | RemoveItem | u32 item id | `SendMsg0A_RemoveItem` 0x66EA30 (slot 19) | `OnMsg0A_RemoveItem` 0x669290 | `CPlayer::RemoveItemById`, inventory UI refresh |
| 0B | UnitEvent | u32 unit id, u32 code | `SendMsg0B_UnitEvent` 0x66D6E0 (20 CUnitServer callers, `BroadcastMsg0B` 0x58D340) | `OnMsg0B_UnitEvent` 0x667DF0 | `CInterface::FwdTopVt80(record, 8)`: order feedback (codes in combat.md 3; 0xFF = respawned hero id) |
| 0C | ItemValues | (u32 item id, f32 value)* then u32 -1 | `SendMsg0C_ItemValues` 0x66EAC0 (item CList: id `+0xc`, `+0x28`): not a Ghidra function, no call or pointer anywhere in the exe: dead [V] | `OnMsg0C_ItemValues` 0x6696A0 | item `+0x28` (durability) of each item of the item manager |
| 0D | MusicTrack | C string | `SendMsg0D_MusicTrack` 0x66EB90 (scripts), `BroadcastMsgString` | `OnMsg0D_MusicTrack` 0x669F40 | `CMusicZone::SetTrackIfBattle` |
| 0E | MovePath | u32 kind (3 = no path: nothing follows), u32 flag (0 / 1), f32 x, f32 y (the request's target point, mover state `+0x60`), u32 unit id, f32 value (1 000 000.0 from `ReportMoveResult`, a local float in `ValidateQueuedOrder`), [embedded delta stream: `MoveState_WriteNet` unless kind == 3] (order read from the sender's disassembly) [V] | `SendMsg0E_MovePath` 0x66D490 (`ReportMoveResult` 0x54EB30, `ValidateQueuedOrder` 0x54E600) | `OnMsg0E_MovePath` 0x669FA0 | decoded into a temporary unit; only drawn with the debug flag `g_0x79BE70` (particles 0x2039 / 0x203a): a move acknowledgement |
| 0F | CameraFromTo | f32[3] eye, f32[3] target, u8 | `SendMsg0F_CameraFromTo` 0x66EDA0 (dialog camera) | `OnMsg0F_CameraFromTo` 0x667D10 | `CCamera::LookFromTo` + flag 0x40 |
| 11 | Error text | C string | `OnMsg00_Identity` (wrong version) | none in the table (the link is dead) | - |
| 12 | WorldByte | 5 bytes: 1 uninitialised stack byte, u32 round(b) | `SendMsg12_WorldByte` 0x66D730 (`BroadcastMsg12`, script `SendEvent(a, b)` op 0x39: `a` is passed but never written) | `OnMsg12_WorldByte` 0x667E20 | `CWorldClient +0x7c0` = the u32 (bytes 1..4); nothing in the exe reads `+0x7c0`: no effect [V] |
| 13 | ScreenCommand | C string | `SendMsgString(0x13)` (`BroadcastMsgString`, script op 0x3a) | `OnMsg13_ScreenCommand` 0x667E60 | `CInterface::HandleScreenCommand` ("say_block", "tutorial", "briefing") |
| 14 | ItemValue | u32 item id, f32 value | `SendMsg14_ItemValue` 0x66EB40 (slot 22, `CPlayer::TrackItemId`, `ResendItemValues` 0x66F080) | `OnMsg14_ItemValue` 0x66A420 | party item `+0x28` = value, `TrackItemId` |
| 15 | PartyExperience | f32 xp | `SendMsg15_PartyExperience` 0x66F280 (`GiveExperience` 0x5911C0, no session) | `OnMsg15_PartyExperience` 0x66A480 | floating number on every party unit |
| 16 | WeaponBroken | u32 hero unit id, u32 weapon index | `SendMsg16_WeaponBroken` 0x66EA70 (`CUnit::DropBrokenItems` 0x532AC0) | `OnMsg16_WeaponBroken` 0x669400 | `HeroRecord::RemoveWeapon`, `CUnitClientSpecific vt[25] SetWeaponFigure(-1)` |
| 17 | ClearItems | - | `SendMsg17_ClearItems` 0x66EA10 (slot 18) | `OnMsg17_ClearItems` 0x669350 | `CPlayer::ClearItems`, UI refresh |
| 18 | UnitExperience | u32 unit id, f32 xp | `SendMsg18_UnitExperience` 0x66F2C0 (`GiveExperience`, session share) | `OnMsg18_UnitExperience` 0x66A5A0 | floating number (kind 8) |
| 19 | UnitVisibility | u8 mode, u32 unit id | `SendMsg19_Dialog` 0x66F840, only from the server `DialogScreen` and only to player 0: `AddSpeaker` (1), `RemoveSpeakerByName` (2), `Update` talk markers (1 / 2), `StartBriefing` (0, 3), `StartDialogLine` end (2 per speaker, then (0, 0)) [V] | `OnMsg19_UnitVisibility` 0x66A620 | 0 `SelectObject` (the speaker gets the selection marker; was read as "HideObject"), 1 ghost flag 0x100 + render specific `vt[26](1)`, 2 clear, 3 `DeselectObject(all)` |
| 40 | UnitCreate | unit record + full net state (multiplayer-protocol.md "Unit creation") | `SendMsg40_UnitCreate` 0x66D7F0 | `OnMsg40_UnitCreate` 0x667EC0 | the unit is created (figure manager), `CMapObject::ReadNetState(0)`, `CWorldClient::AddObject`, player unit hash `+0x28` |
| 41 | UnitUpdate | u32 id + delta net state | `SendMsg41_UnitUpdate` 0x66DBA0 (objects changed this tick) | `OnMsg41_UnitUpdate` 0x6687B0 | `UnpackNetState(1)`; types 0x54 (CBuilding) / 0x5d (CLeverObject) re-register their bridge quads |
| 42 | UnitRemove | u32 id | `SendMsg42_UnitRemove` 0x66DAD0, `RemoveTrackedUnit` 0x66DC90 | `OnMsg42_UnitRemove` 0x668850 | `SetPlayer(0)` for party units, hash `+0x28` erase, `RemoveObject` |
| 43 | UnitDied | u32 id | `SendMsg43_UnitDied` 0x66D6A0 (`CWorldServer::OnUnitDied` 0x593AF0, players that see it) | `OnMsg43_UnitDied` 0x66A3B0 | `CUnitClientSpecific vt[48]` (death blood) |
| 50 | EffectCreate | u32 id, u32 type id + length-prefixed full state | `SendMsg50_EffectCreate` 0x66DDE0 | `OnMsg50_EffectCreate` 0x668940 | effect manager `vt[23](type, id)`, `CEffect::ReadDelta(0)` (spells.md), hash `+0x58` |
| 51 | EffectUpdate | u32 id + delta | `SendMsg51_EffectUpdate` 0x66E020 | `OnMsg51_EffectUpdate` 0x668A30 | `CEffect::ReadDelta(1)` |
| 52 | EffectRemove | u32 id | `SendMsg52_EffectRemove` 0x66DF40 | `OnMsg52_EffectRemove` 0x668AA0 | hash erase, release |
| 60 | PartyStatus | u8 paused, u32 sync marker, u32 player index, u32 n, n x u32 visible unit ids, u32 party mode, u32 m, m x u32 loot-owner unit ids | `SendMsg60_PartyStatus` 0x66E430 (every replication in states 1 / 2; the ids are `CPlayerServer +0x510c`, the visible set) | `OnMsg60_PartyStatus` 0x668C50 | `CWorldClient +0x6c` paused, flag 0x400000 ("visible to the party") moved to the listed units (`+0x50d8`), `+0x6e4` party mode, `+0x50e8` loot ids; in a playable zone only these units are drawn (section 8, RenderObjects) |
| 70 | Weather | u8, i16 (3 B) | `SendMsg70_Weather` 0x66EC10 (dirty bit 2: `CWorldServer +0xe4 / +0xe8`) | `OnMsg70_Weather` 0x669760 | `CWeatherSound::SetRain(g_0x7AFCAC, 3, 0)` |
| 71 | GameTime | f32 hour, f32 rate | `SendMsg71_GameTime` 0x66EC70 (dirty bit 4, `CWorld::GetHourAndRate`) | `OnMsg71_GameTime` 0x6697B0 | `SetTimeScale(rate)`, `SetTime(hour)`, `UpdateDayLight` |
| 72 | Wind | f32[3], u32 | `SendMsg72_Wind` 0x66ECD0 (dirty bit 8) | `OnMsg72_Wind` 0x669800 | `CWorldClient::SetWind` (terrain + weather) |
| 73 | WaterLevels | u32 count + 16-byte records `{material id, steps left, f32 level, f32 step}` | `SendMsg73_DbArray` 0x66F240 (dirty bit 0x20, `WaterLevels::Write`) | `OnMsg73_DbArray` 0x66A400 | `WaterLevels::Read` into `CWorld +0x70`: the script water levels, applied with `CTerrain::SetMaterialHeight` (terrain-lighting.md, world-server.md) [V] |
| 80 | Party | embedded stream (`CParty::Write` full) | `SendMsg80_Party` 0x66EF80 (identity, respawn, map change, disconnect, every 900..1800 ticks, scripts, `Console_Give`) | `OnMsg80_Party` 0x669A30 | `MpFile::Read(party, 1)`, synced item list rebuilt (items with `+0x28 < +0x2c`), named values kept in a session, `ApplyObjectVariables`, the .mp file rewritten |
| 81 | NamedValue | C string, f32 | `SendMsg81_NamedValue` 0x66EE10 / `SendMsg81_MaxValue` 0x66EE80 (`BroadcastGameTime` "gtime", scripts, mercenaries, `um.speed`) | `OnMsg81_NamedValue` 0x669850 | `StringFloatMap_Set(+0xc4)`; "q." names also `NotifyScreenName` + `SetNamedObjectState` |
| 82 | RemoveNamedValue | C string | `SendMsg82_RemoveNamedValue` 0x66EF30 (GSDelVar) | `OnMsg82_RemoveNamedValue` 0x669960 | remove; "q." names -> state 0 |
| 83 | PartyFinal | embedded stream (`MpFile::Write` 0) | `SendMsg83_PartyFinal` 0x66F0C0 (`World::PlayerOut` 0x590A40) | `OnMsg83_PartyFinal` 0x669C90 | `MpFile::Read(party, 0)` |

## 6. Replication, from the client's point of view

`CWorldServer::ReplicateToPlayer` 0x58B750 (server page) runs per tick per player: the dirty bits of `CPlayer +0x130` (section 1 of the
structs batch) are flushed first (`02`, `70`, `71`, `72`, `73`), then `PtrArray_Diff(+0x50d8, candidates)` -> `42` / `40` / `41`, the effects
(`+0x50e8`) -> `52` / `50` / `51`, the party resend countdown `+0x5124` -> `ResendParty` 0x66F450 (`StorePartyFromUnits` + `80` +
`ClearPartyItems`), `60` in states 1 / 2, `07` with bit 0x10, `FlushSendStream`. The candidates are the units within 100 units of the view point
`+0x38` (non-units only, in a playable zone), the visible units, the named objects and the units near every party unit (world-server.md 4.7); `UpdateVisibleSet` 0x66E130 keeps the *known* (`+0x50f8`)
and *visible* (`+0x510c`) arrays from the party units' `AiMemory` seen lists (`+0xa90` / `+0xaac` of the unit server object); `IsUnitVisible`
0x66E110 gates attack orders and the `43` death message. [V]

**Creating an object from a `40` record** (`CPlayerClient::OnMsg40_UnitCreate` 0x667EC0, deep read 2026-11-03 [V]; wire layout in
`docs/multiplayer-protocol.md` "Unit creation"): after the fixed fields and the morph, the first CString is the object's **.mob name**
(`CMapObject_SetName` -> `+0x1A8`; empty for unnamed monsters), then the model, the two texture-set names and the part list, the display
name and a u32. The object comes from the world's map object manager `vt[23](kind, id)`; then:
- units (type 0x50): the u32 prototype index selects the 0x120 `UnitProto` record (`g_0x7AF358`); its units-database row gives the
  six animation speeds (`+0x4B0..`) and the model height offset (`modelOffset[1]` = row `+0x104`), `CUnit::SetProtoIndex`;
- record flag 8 -> static flag 8; models `unmoco1` / `unmoco2` get flag 0x10000000;
- heroes (kind 0x32) whose model is neither `unhuma` nor `unhufe` (the transformed / monster heroes): the part list's `hr` part is
  dropped when the prototype's `+0x10` is -1, else renamed `hr.NN`;
- `CObject3D::LoadModel(model, parts)` (failure = message rejected); model `unmogo` loses the parts `rh3.axeth00`, `rh3.sword00` and
  `sling` (or `rh3.pike00` when the prototype's weapon kind `+0x78` is 5); model `unmowi` gets particle system 0x204D attached
  (scale 0.05); non-units get parts flag 8 and object flag 0x200;
- materials registered, the model's own animation set, texture names: heroes `mo%dp` / `mo%ds`, others the record's two names (the
  second dropped when it is `none`); units `vt[61]`;
- registered in `CPlayer +0x28` (id hash), `vt[34](prototype)` (units: `CUnit::GetProto`, others the model name),
  `PackFullNetState`, then `UnpackNetState(0)` reads the length-prefixed full state; on success `CWorldClient::AddObject`, a named
  object takes its saved named value (`World_BuildObjectVarName` + the player's named floats `+0xC4` -> `SetNamedState`), and in a
  world of kind 3 a unit loads its mouth texture.

What the client keeps: `CPlayer +0x28` every replicated unit, `+0x40` the party units (`CUnit::SetPlayer` -> `TrackUnit`), `+0x58` the effects,
`+0xe0` the loose items of the last `06` batch, `+0xc4` the named values, `+0x114` the synced item ids, `CPlayerClient +0x50d8` the
visible unit ids of the last `60` (party units plus every unit they see), `+0x50e8` the loot-owner ids, `CWorldClient +0x6e4` the party
mode. [V]

## 7. CPlayer / CPlayerClient / CPlayerServer layouts (details in `batch-players-structs.tsv`)

`CPlayer` (0x134): `+0x1c` net stream, `+0x20` connection, `+0x24` player index, `+0x28 / +0x40 / +0x58` IdHash (0x18: buckets +4, count +0x10,
max id +0x14) units / party units / effects, `+0x70` CParty (0x70: heroes `+0xa0` / count `+0xa4`, items `+0xb0`, money `+0xc0`), `+0xc4`
named values, `+0xd8` map name, `+0xdc` entry, `+0xe0` item CList, `+0xfc` party id, `+0x100 / +0x104` game mode / previous, `+0x10c` net log,
`+0x110` last gtime tick, `+0x114` synced item CList, `+0x130` dirty bits. [V]

`CPlayerClient` (0x50f8): `+0x20` state, `+0x24` handlers (256), `+0x34` receive CMemStream (ok `+0x39`), `+0x5c` send CMemStream, `+0x84`
lobby mode, `+0x85` upload pending, `+0x88` send LzCompressor (0x2828), `+0x28b0` receive LzCompressor, `+0x50d8` visible unit ids (count
`+0x50dc`), `+0x50e8` loot-owner ids (count `+0x50ec`). There is no DeltaCodec member: the one the earlier notes placed at `+0x2898` is the
bit writer inside the send codec; `OnMsg0E_MovePath` builds a DeltaCodec on its stack. [V]

`CPlayerServer` (0x5128): `+0x20` state, `+0x24` sync marker, `+0x28` handlers, `+0x38` view point, `+0x48` send CMemStream, `+0x70` in world,
`+0x74` spawn records CArray (0x28 each; filled by PlayerOut, consumed by PlayerIn after `05`), `+0x88 / +0x28b0` codecs, `+0x50d8` tracked
units, `+0x50e8` tracked effects, `+0x50f8` known objects (ObjRefCArray: data +4, count +8), `+0x510c` visible units, `+0x5120` party mode
(0 calm, 1 a party unit has a body-part stat `+0x4c8..+0x4d8 <= 2.0`, 2 a visible unit casts an offensive spell or a hostile brain targets a
party unit: the "+0x5120 modes" open question of unit-ai.md), `+0x5124` party resend countdown. [V]

`CPlayerManager` (a CBaseManager): `CreateByKind` 0x66C110, `CreatePlayerWithConnection` 0x66C180 (kind 0x300a + specific kind 0x49),
`SetPlayerId` 0x66C1E0, `FindPlayerByIndex` 0x66C2B0, `BroadcastMsg12` 0x66C340, `BroadcastMsgString` 0x66C3C0. The local client player is
`CWorldClient +0x20c` (created in `Init`); `CWorldClient::GetConnection` 0x5699F0 (69 callers) returns its `CPlayerClient`. [V]

## 8. CWorldClient

**Instance and containers** (`g_0x7AF5B8`, 0x7e4 bytes; ctor 0x5612F0, `Init` 0x561E50, `Done` 0x563290): `+0xb8` object list (every object
in the world), `+0xb4` hero list, `+0xbc / +0xd8` visible CList + id hash (the objects `Update` ticks), `+0x120` grid A and `+0x150` grid B
(16-unit cells, `terrain size / 16` after `LoadZone`; `ObjGrid_MoveObject` 0x570860 through slots 41 / 42 when an object moves), `+0x8c` zone
exits, `+0xa0` zone exit effects, `+0xf0` screen rectangles of the drawn objects and `+0xf4` the pick records, `+0x108` debug polylines,
`+0x1b8 / +0x1bc / +0x1c0` blood / foot / scorch prints, `+0x1d4` CBridgeManager, `+0x7bc` selected object ids; managers `+0x180` terrain,
`+0x188` materials, `+0x18c` textures, `+0x190` lights, `+0x19c` particles, `+0x1a0` lightning, `+0x1a4` vis fog, `+0x1a8` music, `+0x1ac` sky,
`+0x1b0` moshka bird, `+0x1b4` floating numbers, `+0x1cc` sound, `+0x1d0` texts; `+0x20c` the local player, `+0x210` the camera. [V]

**AddObject** 0x5647E0 / **RemoveObject** 0x564BE0: object list, grid A cell, hero list (kind 0x32 + `IsHeroId`), visible list when `vt[29]`,
and for types 0x54 CBuilding / 0x5d CLeverObject the walkable part faces (flag 0x400 parts) into the bridge manager; `ActivateObjectBridges` 0x5651A0 refreshes them
on every `41`. `SelectObject` 0x566030 (was `HideObject`: flag 0x10, render specific `vt[26]` unless ghost then `vt[25]`, the id
appended to the selected list `+0x7bc`) / `DeselectObject` 0x566100 (was `UnhideObject`; 0 = all): called by `CBattleHud` (click, keys,
`SelectUnitIndex`, `DeselectAll`), `UIOrderClickHandler`, `DialogScreen::OnPartyChanged` and message 0x19 modes 0 / 3 [V].
`CollectObjectsNear` 0x56DAF0 walks the grid cells of a square. `FindNearestPartyUnitFor` 0x5640A0 returns the nearest live unit of
the visible list `+0x50d8` whose relation masks `+0x244 & +0x248` contain the given unit's side bit `1 << +0x250` (a friend of it). [V]

**CreateHeroUnits** 0x566F70 (`Game_CreateHeroUnits` from `CCampScreen::Build` / `CCampScreen2::InitHero`: the party of the camp
screens, no server) [V]: per hero record a local `CUnit` (kind 0x32, id = record `+0x1C`) from the map object manager; part list
`hd bd hp hr lh1 lh2 lh3 rh1 rh2 rh3 ll1 ll2 ll3 rl1 rl2 rl3` where `hr` becomes `hr.%.2d` of the unit-database row (`+0x14` name)
`+0x10`, or is dropped when that is -1; proto index from the same row, `SetProto2ByName(+0x18)`, flag 0x4000000, `LoadModel(+8, parts)`
(failure ends the loop), materials, the model's animation set, `SetTextureNames("none", +0x10 unless "none")`, the stats block `+0x20`
(`vt[35]`), morph `+0x724`, weapons `+0x730` (count `+0x734`), armour `+0x750` / `+0x754` (pieces of armour type 0 only when the second
argument is set), quick items `+0x740` / `+0x744`, `vt[61](+0)`, `AddObject`.

**LoadZone** 0x567AB0 (the `02` payload) [V]: `Lobby_FinishPendingAction`; effects volume 0 and `StopAll`; `+0x6e4` party mode = 0;
`+0x64` tick; zone name, map name; zone exits (`ZoneExitArray_SetSize`, `CEffectKey::WriteAll` 0x4E5750 reads each); u32 indoor (stored
in `+0x6e0` after the next step); the unit save records (`UnitHashes_ReadSave`: 0x3c unit records with the equipment, 0x24 model records);
`Game_SetZone`; the print lists, the exit effects, grid B, every effect, `ClearAll`, `UnpinItemsAndSpells`, unreferenced particle systems,
`+0x20c -> +0x100 = 0`; the map path gets `World_MapPath()` in front unless it is absolute (`X:`); `CTerrain::Unload`, texture caches
dropped (`vt[34]` texture manager `DeleteCachedFiles` + `DeleteAll`); world name `+0x184` from the map record (`MapZoneRecords_FindByName`
on `g_0x79BF00`, default "gipat", upper case); `CTerrain::Load(map path, world, indoor)` or `FatalError("Can't load terrain or map
file")`; both grids resized to terrain size / 16; terrain flag 1 (water waves) cleared without `EnableWaterWaves` (AppSettings `+0xC8`)
and forced on indoors, flag 0x10 cleared without `EnableSelfShadowing` (`+0xCC`), relight budget = the second `LightingFreq` copy
(`+0xD4`, `g_0x79BD44`) / 66; sky texture (slot 0x3d) "Sky01" with sky `+0xc` = 0 indoors, "Sky00" with 1 outdoors;
`LoadDayLightTable("Lights[Cave]<WORLD>.ini")` (indoors with sound: reverb `(0.3, 1.0, 3.0)`, outdoors sound manager `+0x8a8 = 0`);
`g_0x78FF38 = -1` so the first `UpdateDayLight` pushes the sun to the terrain; `CreateZoneExitEffects` 0x567500 (one particle 0x2038 per named exit, enabled when the player's value "<prefix><exit>" is
1.0, `UpdateZoneExitEffectStates` 0x567680); `UpdateDayLight`; music zone (`"final"` for gz20g, only with `Audio` and a music volume); the
remaining bytes of the message are re-queued (`CPlayerClient::AppendStream`); `+0x6ec = 0`; `PrebuildUnitSkinsAndModels` 0x568800 composes the
heroes' skin textures ("<prefix>skin_NN.mmp" + armor / spell overlays -> "prot.dp" / "mo.dp" 256 and "prot.ds" / "mo.ds" 128) and
assembles the figures; wind strength = `DefaultWindForce` (`+0x1E8`) when not 0; `SendMsg09_Ready`; `Bink_Close`. `Movie_PlayIntro` is
called between the long steps so the loading video keeps playing. A terrain failure returns `FatalError`'s value (the process exits).

Skin composition in detail (`PrebuildUnitSkinsAndModels` 0x568800, deep read 2026-11-03 [V]). For every look record of the zone state
(prototype index -> 0x120 `UnitProto` -> units-database row; the row's `+0xE8` is the texture prefix) with a non-empty armor id list,
two layered textures are composed with `CTextureManager::ComposeAndSaveLayers` 0x693180 (format code 0x5551) and registered with
`FindOrCreateByName`. Every layer name is `<prefix><Armor_FormatModelName(id >> 16, id & 0xFFFF)>.mmp`. The armor ids are
`proto << 16 | material`, and -1 means an empty slot. Look record: u16 prototype index at +0, armor id list +4 / +8 (begin / end),
weapon id list +0x10 / +0x14, active weapon byte +0x1C, `+0x20`.
- **primary** (`prot%dp`, or `mo%dp` when the record's `+0x20` is not -1; 256 x 256, texture kind 1): the base skin
  `<prefix>skin_00.mmp` (`skin_%.2d` when the record has a skin index), then armor slots 3..6 of the list, then slots 1 and 2. When
  both of these are worn, the one whose armor row (200-byte table `g_0x7C8718`) has the smaller `+0xB8` is drawn first, so the other
  one covers it.
- **secondary** (`prot%ds` / `mo%ds`; 128 x 128, kind 2): armor slot 0, then the active weapon (record byte `+0x1C` indexes the
  weapon id list `+0x10`; `WeaponProto_FormatModelName` from the 0xB4 table `g_0x7C86F8` + material).
`ComposeAndSaveLayers` also writes the result to `textures\<name>.mmp` (the skin cache). The second loop assembles the figures:
`UnitProto::BuildModelPartNames` 0x535CB0 (section 8.1) and the render manager's `vt[3]`.

**Update** 0x5641F0 (slot 6, every frame) [V]: frame counter `+0x6ec` (10 = "autosave" command + volumes), terrain / music / sound / vis fog
`vt[6]`, net pumps (`+0x5c`, `+0x1c8`, `CTcpNetDriver::SyncPeers(+0x60)`), the player: `PurgeSyncedItems`, connection `vt[23] Update` (0 ->
`FatalError` "Disconnected"), `vt[6] SendClientState`; unpaused (`+0x6c == 0`): `+0x64 ++`, every visible object `vt[6]` (dropped when it
returns 0), every effect, the moshka; in a session the objects also get `vt[25](tick + +0x6f0)`; `CParticleManager::RemoveFinished`, expired
prints erased, print colours re-lit from the lights when `EnableBloodprintsRelight` (`g_0x79BD58`) and the countdown `+0x1c4` (minus the
tick length `g_0x78FF34` = 55 ms per unpaused update) runs out, reloaded with `LightingFreq` (`g_0x79BD24` = AppSettings `+0xB4`, ms),
texture cache trimmed every 32 ticks.

**Frame** `Render` 0x569E40 [V]: `UpdateDayLight`; view matrix (`CCamera::BuildViewMatrix` of `g_0x79B2A8`) pushed on the view and world
stacks of `g_0x7AF068`; flag 0x1000: the sky (render states alpha test 15 = 0, z-write 14 = 0, cull 22 = none, 29 = 1; post-process
off); `Net_TickAll`; flag 0x100: `CTerrain::SetVisibleArea(camera xy, 100)` + `RenderOpaque`; `RenderObjects`; `Net_TickAll`; flag 0x100:
`RenderWater`; `Net_TickAll`; flag 0x400: vis fog, particles sorted by depth (`vt[36]` particle manager), lightning, weather (blend
SRCALPHA / INVSRCALPHA, alpha blend on, z-write off); `Net_TickAll`; `RenderDebugPolylines`; stacks popped; flag 0x2000:
`CFlyingHitPoints::Draw` with ZENABLE 0. The network is pumped four times per frame so a slow frame does not starve the link [V]. The render pass flags live in `CWorld +8`
(`0x3dc0` set by the ctor; 0x40 / 0x80 object / unit passes, 0x200 shadows, 0x8000 wireframe `ToggleWireframe` 0x569970).

**RenderObjects** 0x56AFE0 (deep read 2026-11-06) [V]: nothing without a loaded terrain. The 11 global lists reset: draw lists units
0x7AF578, trees (0x52) 0x7AF568, buildings (0x54) 0x7AF5A8, levers (0x5d) 0x7AF588, others 0x7AF598; shadow lists units 0x7AF528, trees
0x7AF518, buildings 0x7AF558, levers 0x7AF538, others 0x7AF548; the relight list 0x7AF508; the deferred units 0x7AF5B4. The moshka flight is
advanced (`tick + g_0x7AFCA8`). Classification (`ClassifyCellObjects` 0x56A990, `now = timeGetTime()`):
1. the grid A cells of the camera position +- 100 units (clamped to the terrain) in rows: non-units are classified, units (type 0x50) are
   only appended to the deferred list, heroes (kind 0x32 + `IsHeroId`) skipped;
2. the deferred units, then the hero list `+0xb4`: each unit gets `vt[25] UpdateClientPresence`, and is classified only when the world is
   not a playable zone (`g_0x79BE6C` and the local player's mode `+0x100 == 1`) or the unit has flag 0x400000 (visible to the party,
   message 0x60): **the client-side fog of war**;
3. classifying one object (with a render specific `+0x94`): world matrix `+0x54` = `Quat_ToMatrix4(+0x34)` with the position `+0x1c` in
   column 3; `CD3DLibrary::CullBox(+0x124)` under it. Outside (2) -> flag 0x20 cleared and, when `IsShadowVisible`, the animation
   advanced and the object pushed to the shadow list of its type. Inside -> flag 0x20, `+0x28..+0x30` = the position rotated into view
   axes (rows 0..2 of the view top, no translation; `+0x30` is the depth used by the sort), the draw list and the shadow list of its type;
   flag 0x200 objects lit longer ago than `LightingFreq` (`g_0x79BD24`, ms: `now - render specific +0xEC`) also go to 0x7AF508. The
   return value is the count of those stale objects.
Relight budget: the stale list is sorted by `+0xEC` newest first (`Unit_GreaterByStatEC`) and the first `max(10, stale / 8)` lose flag
0x400, so their next `Draw` relights them. Drawing: flags 0xC0 -> state block, the screen rects `+0xf0` cleared, alpha test on (state 15);
flag 0x40 -> the four object lists sorted by `RenderObj_Less` 0x574300 (material key `*(render specific +0x20)`, then `+0xf0`, then
depth `+0x30`) and drawn others (slot 22 `Draw` = render specific slot 19 without a rect), levers (slot 23 = `Draw` into the rect `+0x100`,
then `0x56A370` appends `{type, id, rect}` to `+0xf0` for objects with flag 0x20 and without 0x40), flush, buildings and trees (slot 22);
flag 0x80 -> units sorted and drawn with slot 23 + rect records. So only units and levers are pickable on screen. Prints when a list is
not empty (`EnableBloodprints` `g_0x79BD50`, `EnableFootprints` `4C`, `EnableFireprints` `54`; `CPrintDecal::Render`). Flag 0x200 ->
the shadow lists of buildings, others, levers, trees sorted, a **translation by `-0.02 *` the camera up axis** (`Quat_GetUpAxis` of
`g_0x79B2DC` = `CCamera +0x34`, the view axis also used for the particle depth sort) pushed on stacks 2 and 1 as a depth bias, then
`CMapObject` slot 24 `RenderShadow` on levers, others, buildings, trees and the units (unsorted). Last, with `SelectionType` 1
(`g_0x79BE64` = AppSettings `+0x1F4`) and the unit pass on: a `DrawArrow3D` (size 0.5, the object's forward / right axes) at the part whose
name starts with "hp" (else the position) of every live object of the selected list `+0x7bc`; a dead one (`vt[55]`) is deselected
(flags 0x110 cleared, render specific slots 25 / 26) and removed. The draw lists are emptied for the next frame.

`BuildShadowReceiverMesh` 0x56C750 (was RenderShadowVolume; deep read 2026-11-06) builds the receiver geometry of one caster:
`ShadowProjector::Build` (the global `g_0x7AF488`); the terrain cells in the prism (`CTerrain::BuildShadowReceiverMesh`) unless the
bridge check is on and the caster's xy is on a bridge quad; then, with the objects flag, every node of the bridge quad list (`+0x1f4` =
CBridgeManager `+0x20`): its owner (`vt[40]` by id) is tested with a coarse sphere (position `+0x1c`, radius `|bounds centre +0x128| +
+0x134`) and a fine one (world bounds centre, `+0x134`); on a hit every part `+0xd0` (count `+0xd4`) is put on the matrix stack `+0x84`,
its mesh fetched by part name (`+0xf0` slot 6), transformed by the device into the 16-byte vertex buffer `g_0x7AF478` and its triangles
clipped with `ClipTriangleB`; `ClipBuf::Finish`. Callers: `CPrintDecal::Render` (bridge check 1, objects 1: a print on a bridge is laid on
the bridge parts instead of the terrain) and `CObject3DClientSpecific::RenderShadow` (no bridge check; objects unless one of the caster's parts has bounds flag 0x400, i.e. the caster is itself a bridge owner).
The walk is per quad, so an owner with several walkable quads has its parts clipped once per quad (duplicate receiver triangles) [I].

**Picking** [V]: the object renderers append `{RECT, object id}` to `+0xf0`; `PickObjectsAt` 0x565790 / `PickObjectsAtPrecise` 0x5659D0
(`ScreenRects_CollectAt` 0x571800 / 0x5718B0 with the stats `vt[22]` / `vt[24]` point test) fill `+0xf4` (0x18-byte records), sort by view depth
(live units first, `UnitRecord_LessByDepth` 0x573A60) and iterate with `NextPickedObject` 0x565C30 / `FirstPickedUnit` 0x565D00 /
`NextPickedUnit` 0x565F60 / `PickUnitAt` 0x565E40. `PickTerrainPoint` 0x56C350: the screen ray from the inverted view-projection and the
terrain height (slot 29: terrain + bridges) by iteration and bisection; `PickTerrainPointRaw` 0x56BF30 (only caller
`UIOrderClickHandler::OnLButtonDown`) samples the terrain only (`CTerrain::GetHeightAt(x, y, 0)`). Its algorithm (deep read 2026-11-06):
the view top is pushed on stack `+0x14`, the ray is solved by Cramer's rule as `x(z) = a z + b`, `y(z) = c z + d` for the integer screen
point; `z0 = (H(ray(m)) + m) / 2` with `m` = terrain `+0x114` / 2; fixed-point `z = H(ray(z))` until `|dz| <= 0.01` (at most 100 steps);
when that does not converge, a damped iteration `z = H(ray((z + H) / 2))` (100 steps). Ray coordinates are clamped to >= 0 only. With an
empty view stack the result is `g_0x7AFDB0`. [V]

**CBridgeManager** (`+0x1d4`, 0x38) [V]: a CList of 0x4c-byte walkable quads {owner id, centre, radius, origin, u / v axes, normal, lengths}
built by `AddBoxFaces` 0x560820 (the six faces of a part box, normals with z >= 0.8 kept, `AddQuad` 0x560650) for objects of types 0x54 / 0x5d;
`GetHeightAt` 0x560F00 (highest quad under a point, else the terrain), `IsOnQuad` 0x561050, `GetNormalAt` 0x561150, `RemoveQuadsOf`
0x560E70. Slots 29 / 30 of the world (`HeightWithBridges` 0x5618E0 / 0x561930) combine it with the terrain height: this is why units walk on
bridges and stairs on the client.

**Day light** [V]: `LoadDayLightTable` 0x563930 reads `config/lights[cave]<world>.ini` sections "sunlight", "ambient" and "sky" (the
string at 0x790064; its table `+0x52c` feeds the sky / fog colour): keys "hourNN" (characters 4..5 are the hour), values r g b / 255,
alpha 1, filled linearly from the previous listed hour to this one into 24 rgba entries at `+0x22c / +0x3ac / +0x52c`. The hours
must be listed in ascending order: the fill only runs while the previous hour is below the new one, so a key out of order sets its
own entry and leaves the gap before it unfilled (deep read 2026-11-06, ws round). `UpdateDayLight` 0x566360 (every
`Render`; deep read 2026-11-06): nothing when render globals flag 1 is clear; `UpdateZoneExitEffectStates`; hour `h` = fmod of the world
hour. Indoor: the hour-0 entries, light direction (0.5, 0.5, -0.7071), fog density `g_0x79BED4`, the sky recoloured black. Outdoor: fog
density `g_0x79BED4` (day) over 6..18 h, `g_0x79BED0` (night) over 22..2 h, linear in between (2..6 h, 18..22 h); `+0x6dc = 0x80`; the
three tables interpolated between entry `floor(h)` and `(floor(h) + 1) % 24`; **elevation**: 4..20 h `30 + 30 * (1 - |h - 12| / 8)`
degrees (30 at 4 h and 20 h, 60 at noon), otherwise `30 + 45 * d / 4` with `d` the hours past 20 h or before 4 h (75 at midnight: the
night light comes from high up); azimuth `h / 24 * 2 pi`; direction `(cos az cos el, sin az cos el, -sin el)` into the sun light
`+0xd4..+0xdc`, its colour `+0x98..`. Writes `+0x6cc` sun, `+0x6ac` ambient (0.8 * sun when the ambient entry of `floor(h)` is zero),
`+0x6bc` = a copy of the ambient colour (not the fog), the fog-table colour as the D3D fog colour `0xFFRRGGBB` (render globals `+0x14`),
`CSky::Recolour(ambient copy, sun, fog colour)`, and when the hour passed a whole hour since `g_0x78FF38` (reset to -1 when `h` goes back)
`CTerrain::SetSunLight`; returns 1 on that hourly update. `GetEnvironmentMask` 0x56D9F0 (stance bits, world name, day / night, in / outdoor, weather
`+0x7a8`) selects ambient sounds and music.

### 8.1 The client unit: sounds, hit effects, skins, part names (`CUnitClientSpecific`, deep read 2026-11-06) [V]

The rest of the client unit (net sections, animation playback, move replay) is `combat.md` 8.

**`UpdateAnimSounds` 0x534B60** (from `Update` every frame): the clip entry is `animSystem +0x40`, its time `+0x60`; a new entry
restarts `lastAnimTime +0x2c` at 0; nothing when the time did not move. The clip class is `entry +0x14 & 0x3C0000`:
- `0x040000` plain and `0x200000`: the clip's sound event between the last and the current time (`GetAnimSoundName`, priority 1000 for
  0x040000, 0 for 0x200000);
- `0x080000` swing / hit (skipped when the local player's mode is 3): the loop sound `+0x88` stopped; when the time crosses the clip's hit
  frame `entry +0x40` a camera shake of strength `UnitProto +0x30` (the "Detonation" column, read as a float; 5.0, 0.25) at the unit;
  heroes (kind 0x32) with `CUnit +0x256` set also play the hit / miss event sound; then the clip's sound as the **voice** `+0x90`, not
  restarted while the previous voice is still playing or pending;
- `0x100000` / `0x140000` steps: up to four step frames `entry +0x30..+0x3c` (stored + 1, 1 ends the list). When one is crossed: the step
  sound for the terrain material under the unit (`GetStepSoundName`) unless the unit has buff 0x10 (silence); then, unless the stance
  `+0x264` is 0 (crawl): the camera shake as above, and a **footprint** when the race (`RaceModel` = `CUnit::GetProto` 0x530970) has a
  Footprint Type `+0x10c` >= 0 and Leg Segments `+0x110` in 1..8: the part `ll%d` for even step events, `rl%d` for odd ones (swapped by
  First Step Right `+0x11c`), placed at the part minus half its size along the facing, rotated to the facing, strength `UnitProto +0xb0`
  times 0.75 when sneaking (stance 1), visible when not in a playable zone or the unit has flag 0x400000;
  `CWorldClient::AddFootPrint(.., (left << 16) | type, ..)`; prints are chained 4 + rand(0..3) at a time (`+0x80` / `+0x84`);
- `0x180000`: the sound becomes the loop sound `+0x88`; `0x1C0000`: loop and voice stopped, then the sound (priority 1000).
Every sound is `Play3D(name, priority, unit position, 8, 40)`.

**`v46_PlayHitEffect` 0x537500** (was `PlayAnimation`; slot 46, called by `CUnit::ReadNetState` section 4 when a body part's HP ratio
drops: (part 0..5, drop, packed-stats byte `+0x107`)): nothing with flag 8 or a drop <= 0.0001. The sound
`weapons\<set>\<material>\1.wav` (priority 1000): the set by the u16 `CUnit +0x350` (a weapon kind below 0x10, else
`UnitProto[+0x350 - 0x10] +0x7c`): 0, 1, 13 slash; 2, 3, 5, 6, 10 pierce; 4, 11 crash; 9 hands1; 12 hands4; 14 hands6; 15 hands7; 7 / 8
none. The material is the race's Skin Type (`RaceModel +0x118`); for heroes the first worn armour covering the part (table 0x73EC78 per
part: head slot 0; torso 1 then 3; arms 1, 3, 6; legs 2, 4, 5) gives the material record (`g_0x7C86D8`, 0x50 bytes, index = armour id
& 0xFFFF) whose name `+4` maps Crystal / Bone / Stone -> `stone`, Metal -> `Metal`, anything else -> `leather`. With unit flag 0x80, a blood
particle at the part (`hd bd lh2 rh2 ll2 rl2`, attach modes 1 / 7 / 3 / 4 / 5 / 6) by Blood Type `RaceModel +0x108` (1 / 5 0x200F,
2 / 6 0x2031, 3 / 7 0x2032, 4 / 8 0x2033), scale `part +0x198 * 2.5`, intensity = the drop clamped to 0.1..1; for blood types 1..4 also
a blood print (`AddBloodPrint`) under the part, offset randomly by up to `min(+0x144, +0x148, 1)` in 0.02 steps, size
`(part +0x1a8 + +0x1ac) / 2 * 0.3` by three times that, kind `(type - 1) << 16 | rand(0..1)`.

**`BuildSkinTextures` 0x536670** (callers `UpdateBodyTexture`, `UpdateHeadTexture`): the run-time twin of the skin composition of
`PrebuildUnitSkinsAndModels` (section 8 above), on the replicated equipment `armorIds +0xa0` / `weaponIds +0xbc`. Mode 0: `mo%ds` /
`prot%ds`, 128, armour slot 0 then the active weapon (`CUnit +0x26c` <= 3). Mode 1: `mo%dp` / `prot%dp`, 256, the base skin
`<prefix>skin_%.2d.mmp` (`skin_00` when the proto index `+0x18` is out of range), armour slots 3..6, then slots 1 and 2 with the smaller
`ArmorProto_GetWearOrder` first. Every layer name is `<CUnit +0xa4 prefix><model name>.mmp`; `BuildEquipmentList` gives the 12-byte
layer records; `ComposeAndSaveLayers(out, records, names, 3, 0, 0x5551)`.

**`UnitProto::BuildModelPartNames` 0x535CB0** (was `CUnitClientSpecific::BuildArmorModelNames`; `this` is the 0x120 `UnitProto` row;
only caller `PrebuildUnitSkinsAndModels`, when the look record has no part list of its own): the part table 0x73EC24 is three rows of
seven inline 4-byte names: head `hr hd`, body `bd rh1 rh2 rh3 lh1 lh2 lh3`, legs `hp rl1 rl2 rl3 ll1 ll2 ll3`. For every armour id (row
`g_0x7C8718 + (id >> 16) * 200`) the row chosen by the armour's slot kind `+0xc` (0 head, 1 body, 2 legs; others skipped) yields
`hr.%.2d.armor%.2d` (proto `+0x10`, armour `+0x18`; skipped when the proto has no hair, -1) or `%s.armor%.2d` (part, armour `+0x18`); then
`Weapon_ListModelParts` for each weapon id. The second output is the 21 base parts with `hr` -> `hr.%.2d` (dropped without hair).

**CWeather** (`+0x6f4`, 0xc8, vt 0x73F4C8) [I]: rain / snow particles and sounds; `CWeatherSound::SetRain` 0x4E3FC0 from message 0x70,
`SetWind` 0x564190 from 0x72, updated with the camera position in `Render` (`CEffectSoundObj::Update`); its own module (0x4E3000) is not
in this round.

**Camera** [V]: `+0x210` (`SetCamera` 0x565670) is the CCamera whose position bounds the terrain window and the grid window; `CameraLookAt`
0x5662B0 (message 04) moves it to a point; the view-point message `40` carries the active transform of `g_0x7AF7C8` (the camera controller).

## 9. What is not known

Settled in the T3 pass (2026-11-02; log in `_cpr/claude-re/full/t3-settled.tsv`):

* Senders: `0x0C` is written by `0x66EAC0` (`SendMsg0C_ItemValues`: the item CList, id `+0xc` and `+0x28` per item, then -1), a body
  Ghidra never made a function; an exe-wide scan finds no call, jump or pointer to it: dead code, the client handler is never fed [V].
  `0x19` comes from `CPlayerServer::SendMsg19_Dialog` 0x66F840, called only by the server-side `DialogScreen` (speakers, talk
  markers, briefing start / end) for player 0 [V]. `0x12` = script `SendEvent(a, b)`: the payload is one uninitialised byte then
  `u32 round(b)`; `a` is dropped by `SendMsg12` (disassembly of 0x66D730 / 0x66C340 / 0x4B6EB6) [V].
* `CWorldClient +0x7c0` (msg 0x12, -1 after the ctor) and `+0x6dc` (0 in `Init`, 0x80 in `UpdateDayLight`) are write-only: an
  exe-wide disassembly scan of `0x7c0(` / `0x6dc(` finds only these stores [V]. `+0x228` (10 in the ctor) has no other access in
  0x55E000-0x579000 [I]. `vt[37] / vt[38]` return `+0x194 / +0x198`, written only by the ctor (0): null manager slots, and the server
  versions return 0 too (`CWorldServer::v37_Ret0 / v38_Ret0`) [I].
* Type id 0x54 = `CBuilding`, 0x5d = `CLeverObject` (the only `GetTypeId` bodies returning them) [V]. The 16-byte records of `0x73`
  are the `WaterLevels` of `CWorld +0x70` [V].
* `0x38` is dead at both ends: the client writer `0x66AF90` (`{u32 unit id, u32 value}`) has no reference anywhere, and the server
  handler reads the two words and does nothing [V].
* The `0x40` tail [V] (disassembly 0x66D971..0x66DA01): for units only (`vt[12] == 0x50`) the display name `vt[7]()` with its NUL and
  `u32 CUnit +0x23c` (protoIndex); any other object writes "" and 0; then `BitStream_FlushByte` and the raw full net-state bytes of
  `+0x1e8` (no length prefix). The `0x0E` words are `{kind, flag, f32 x, f32 y, unit id, f32}` (section 5 row): words 3..4 are the
  target point, not unit ids [V].
* `CPlayerServer +0x44` is only ever set to -1 (ctor, Init, Done, Reset, ClearLists): write-only [V by disassembly of 0x66C540-0x671280];
  `CPlayer +0x108` = `GodMode` level (combat.md 4.3) [V].

Open after the T5 deep read (2026-11-06):

* `UnitProto +0xb0` (the first "Detections" float of data-sound.md 3.1) is the footprint strength in `UpdateAnimSounds`; whether the
  column order of data-sound.md is off by one there or the designers reused the detectability is not settled [?].
* `CUnit +0x256` (anim flags byte) gates the hero hit / miss event sound of class-0x080000 clips; its bit meanings beyond "miss" are [?].
* The duplicate receiver triangles of a bridge owner with several walkable quads (8, `BuildShadowReceiverMesh`) should darken a unit
  shadow / a print twice where they overlap: **[live]** (`live-t5-wcp-net.tsv`).

## 10. Corrections to earlier notes

| file | wrong | right |
|---|---|---|
| multiplayer-protocol.md 5 | `0x00` server -> client = "the world snapshot / tick" | `0x00` = `{u32 player object id}` (`OnMsg00_PlayerId`); the snapshot is `0x02` (zone state) and the periodic "tick" bytes are `0x60` / `0x07` / `0x81 gtime` |
| multiplayer-protocol.md 5 | `0x05` = "the players' arrival points" | `0x05` = the object lists of a loot / shop batch (`SendMsg05_06_Items`), always followed by `0x06` |
| multiplayer-protocol.md 5 | `0x04` = "u32 id, u32 tick, u8 1: an event on a unit" | `0x04` = `{f32 x, y, z, u32, u32}` camera jump (`SendMsg04_CameraAt`); the unit event is `0x0B` |
| multiplayer-protocol.md 5 | `0x42` "47 bytes, unknown", `0x43` "body gone" | `0x42` = `{u32 id}` unit removed from the view; `0x43` = `{u32 id}` unit died (death blood on the client) |
| multiplayer-protocol.md 5 | `0x0B` = "the new hero's unit id" | `0x0B` = the generic unit event / order feedback; `{id, 0xFF}` is only the respawn case |
| multiplayer-protocol.md 4 | `0x34` = `SetOwnerField270`, `0x35` = `SetAttackMode` | `0x34` -> `CUnitServer::RequestWeapon`, `0x35` -> `RequestStance` (combat.md already renamed the unit side) |
| multiplayer-protocol.md 4 | `0x10`, `0x02`, `0x03-0x07`, `0x3B`, `0x50` unknown | see section 4 |
| net-notes.md | CPlayerClient 0x5128 bytes, `+0x2898 DeltaCodec` | 0x50f8 bytes (0x5128 is the server); no DeltaCodec member |
| net-notes.md | CPlayerServer `+0x5110 / +0x5114 party unit array`, `+0x5120 danger level (1 low stats, 2 combat)` | `+0x510c` ObjRefCArray of the *visible* units (data `+0x5110`); `+0x5120` = party mode 1 hurt / 2 fighting as defined in `SendMsg60_PartyStatus` |
| net-notes.md | CPlayer `+0x118 id CList of object ids to resend` | the synced party item ids (durability message 0x14) |
| net-notes.md | server -> client `0x82 name`, `0x12`, `0x15`, `0x16`, `0x18`, `0x19` unexplained | section 5 |
| functions.tsv | `CWorldClient::SendLeaveFlag`, `Game_SendLeaveFlag`, `SendMsg10_Flag` | the pause flag (clock button) |
| functions.tsv | `WorldClient::OnTick`, `OnNewHeroId`, `CPlayer::CallSlot2WithKind9`, `CObject3D::GetPartBounds` at 0x5647E0, `CWorld::IsNight` at 0x56D9F0, `CBridgeManager::AddBoxFaces` at 0x5651A0, `CUnit::SetProto2ByName` at 0x566F70, `CPlayer::ClearAll` at 0x564E70 | mis-attributed names: see the batch (`OnMsg00_PlayerId`, `OnMsg0B_UnitEvent`, `SendMsg09_Ready`, `CWorldClient::AddObject`, `GetEnvironmentMask`, `ActivateObjectBridges`, `CreateHeroUnits`, `CWorldClient::ClearAll`) |
| flows.md 236 | `CWorldClient::v6` "slot 6 default" | the per-frame `Update` |
| pathfinding.md 320 | "`OnMsg_UnitMoveState` stores the path" | it only decodes it into a temporary unit; nothing is kept unless the debug flag `g_0x79BE70` draws it |
| this page 1, 5, 6, 7 (until 2026-11-06) | flag 0x400000 = "party unit"; `CPlayerClient +0x50d8` = "party unit ids" | the message-0x60 list is the server's VISIBLE set (`+0x510c`: party units and what they see); flag 0x400000 = visible to the party; in a playable zone the client draws only those units (fog of war) |
| this page 1 / 8 RenderObjects | flags 0x200 / 0x400 = "animation-budget candidate / animation enabled"; "at most `max(10, visible / 8)` ... (the animation budget)"; `g_0x79BD24` "frames" | 0x200 = cacheable lighting, 0x400 = lighting cache valid (`Draw` sets it with `+0xEC` = `timeGetTime`); a **relight** budget of `max(10, stale / 8)` newest-lit stale objects; `g_0x79BD24` = AppSettings `LightingFreq` in ms |
| this page 8 RenderObjects | shadows under "a shear matrix `sun up * -0.02`"; `+0x28` "world position" | a translation by `-0.02 *` the CAMERA up axis (`g_0x79B2DC` = `CCamera +0x34`), a depth bias; `+0x28..+0x30` = the position rotated into view axes (sort depth `+0x30`) |
| this page 8, 5 (0x19), `structs.tsv` | `HideObject` / `UnhideObject`, `+0x7bc` "hidden object ids" | `SelectObject` / `DeselectObject`: flag 0x10 and the selected list `+0x7bc` (the SelectionType 1 arrows); message 0x19 mode 0 selects the speaker, mode 3 deselects all |
| this page 8 Day light | "sun elevation `30 + 45 * bell(4..20 h)`"; `+0x6bc` "fog" | 4..20 h: `30 + 30 * (1 - abs(h - 12) / 8)` (60 at noon); night up to 75 at midnight; `+0x6bc` is a copy of the ambient colour, the fog colour goes to the renderer |
| this page 8 Frame | `Net_TickAll` once per frame | four times per `Render` |
| `terrain-lighting.md` 10.3 | "the object-receiver branch (`ClipTriangleB`, list `CWorldClient +0x1F4`) is dead code"; "A print centred on a bridge quad gets no mesh" | `+0x1F4` is the CBridgeManager quad list (filled by `AddQuad`); prints (objects = 1) and unit shadows (objects unless the caster has walkable parts) are clipped onto the bridge-owner parts; a print on a bridge gets no TERRAIN mesh but does get the bridge mesh (page not owned by this round: reported) |
| `data-sound.md` 3.1 row 33 / `functions.tsv` | "`+0x118` also selects the animation set (`CUnitClientSpecific::PlayAnimation` 0x537500)" | 0x537500 is slot 46 `PlayHitEffect`; the field it reads is `RaceModel +0x118` Skin Type (hit-sound material), not `UnitProto +0x118` Weapon 1 (reported) |
| `functions.tsv` | `CUnitClientSpecific::BuildArmorModelNames` 0x535CB0; `CMapObject::PickTest_v22` / `GetScreenRect_v23` | `UnitProto::BuildModelPartNames` (`this` = the 0x120 row); slot 22 = `Draw` without a rect, slot 23 = `Draw` recording the screen rect (batch-t5-wcp-net) |

## 11. Function index

Players (0x664000): `CPlayer::ctor` 0x664BF0, `dtor` 0x664E20, `Reset` 0x665380 (= `Init` 0x665010 / `Done` 0x6650E0), `ClearTracked` 0x6651A0,
`ResetReplication` 0x6651D0, `ClearAll` 0x665200, `OwnsUnit` 0x665440, `AddItems` 0x665470, `ClearItems` 0x665550, `HasQuestItemById` 0x665640,
`RemoveQuestItemById` 0x665780, `RemoveItemById` 0x665870, `SetConnection` 0x665950, `SetNetStream` 0x665990, `ClearNetStream` 0x665A10,
`SetPlayerIndex` 0x665A60, `TrackUnit` 0x665A70, `UntrackUnit` 0x665B50, `TrackItemId` 0x665BE0, `SetParty` 0x665CC0, `DetachDeadUnits` 0x665D20,
`PurgeSyncedItems` 0x665DE0; `NetFatalError` 0x664B70; `HeroRecordArray` 0x664160 / 0x6641D0; `HeroRecord::Assign` 0x664950.
`CPlayerClient`: ctor 0x666D10, dtor 0x666FD0, `Init` 0x667230, `Done` 0x667510, `ResetStreams` 0x667580, `Reset` 0x667600, `SendClientState`
0x667680, `FlushSendStream` 0x667810, `DispatchPackets` 0x667970, `Update` 0x667A30, `AppendStream` 0x667B40, `OnUnitAdded` 0x667BA0, the
handlers `OnMsgNN_*` 0x667C20..0x66A620 (section 5), the senders `SendMsgNN_*` 0x66A720..0x66B300 (section 4), `StopRecording` 0x669750.
`CPlayerManager` 0x66C0F0..0x66C3C0. `CPlayerServer`: ctor 0x66C540, dtor 0x66C770, `Init` 0x66C9A0, `Done` 0x66CC50, `ClearLists` 0x66CE50,
`EnterWorld` 0x66CED0, `LeaveWorld` 0x66CF00, `Reset` 0x66CF20, `Disconnect` 0x66D020, `ClearKnownObjects` 0x66D090, `FlushSendStream` 0x66D100,
`Update` 0x66D270, the senders 0x66D490..0x66F2C0 and 0x66F840, `RemoveTrackedUnit` 0x66DC90, `IsUnitVisible` 0x66E110, `UpdateVisibleSet`
0x66E130, `ResendItemValues` 0x66F080, `ChangeMap` 0x66F310, `ResendParty` 0x66F450, `Respawn` 0x66F4C0, the handlers 0x66F890..0x671060
(section 4), `Formation_ComputeOffsets` 0x66FD70. `CMappedResource` 0x671280..0x6725E0 (the .res mapping; net-notes.md layout, unchanged).

World (0x560000): `CBridgeManager` 0x560650..0x561150, 0x561800 / 0x561840; `CWorldClient` ctor 0x5612F0, dtor 0x561B00, `Init` 0x561E50,
getters 0x561890..0x5619D0, `CWeather::dtor` 0x5616C0, `CreateSky` 0x562F70, `PinItemsAndSpells` 0x5630B0 / 0x563190, `Done` 0x563290,
`InitClientResources` 0x563710, `LoadDayLightTable` 0x563930, `TerrainHeightClamped` 0x563F70, `GetHeight` 0x564070, `FindNearestPartyUnitFor`
0x5640A0, `SetWind` 0x564190, `Update` 0x5641F0, `AddObject` 0x5647E0, `RemoveObject` 0x564BE0, `ClearAll` 0x564E70, `ClearVisibleEffects`
0x565090, `ActivateObjectBridges` 0x5651A0, `ApplyObjectVariables` 0x5652E0, `SetNamedObjectState` 0x565380, `World_BuildObjectVarName`
0x5654F0, `FindTexture` / `CWorldClient::FindTexture` 0x5655D0 / 0x5655F0, `SetCamera` 0x565670, picking 0x5656A0..0x565F60, `SelectObject` / `DeselectObject`
0x566030 / 0x566100, `CameraLookAt` 0x5662B0, `SendPause` 0x566330, `SetPaused` 0x566350, `UpdateDayLight` 0x566360, `CreateMoshkaEffect`
0x566C20, net stream attach 0x566CB0..0x566F30, `CreateHeroUnits` 0x566F70, `RemoveHeroUnits` 0x567400, zone exits 0x567440..0x5677F0,
`ReadItemObjects` 0x567950, `ReadSpellsAndItems` 0x567A60, `LoadZone` 0x567AB0, `Hero_BuildEquipmentEntries` 0x5683D0,
`PrebuildUnitSkinsAndModels` 0x568800, `ToggleWireframe` 0x569970, `GetConnection` 0x5699F0, stats flags 0x569A10..0x569B20,
`RenderDebugPolylines` 0x569D60, `Render` 0x569E40, `ClassifyCellObjects` 0x56A990, `RenderObjects` 0x56AFE0, `PickTerrainPointRaw` /
`PickTerrainPoint` 0x56BF30 / 0x56C350, `BuildShadowReceiverMesh` 0x56C750, `ShowFloatingNumber` 0x56CE40, prints 0x56CE90..0x56D740,
`ResetDisabledShadowBuffers` 0x56D7A0, `AddVisibleObject` 0x56D7E0, `GetEnvironmentMask` 0x56D9F0, `CollectObjectsNear` 0x56DAF0,
`ReloadMouthTextures` 0x56DE80; containers / sorts 0x56DEC0..0x578400 (`ObjGrid_*` 0x570730..0x5728D0, `ScreenRects_*` 0x5717B0 / 0x571800 /
0x5736D0, `UnitRecord_*` / `RenderObj_*` STL sorts); glue `Game_* / World_* / Session_*` 0x578560..0x578E30 (section 11 of the batch).

Client unit (section 8.1; the rest of `CUnitClientSpecific` is combat.md 8): `UpdateAnimSounds` 0x534B60, `UnitProto::BuildModelPartNames`
0x535CB0, `BuildSkinTextures` 0x536670, `v46_PlayHitEffect` 0x537500; slot thunks `CMapObject::Draw_v22` 0x516460 / `DrawWithScreenRect_v23`
0x516420 / `RenderShadow_v24` 0x516490.

Left to the server / session page (read only as entry points): `CWorld::ctor / CreateManagers / Done / dtor` 0x55EAB0..0x55F8E0 and the
CWorld getters 0x55EB30..0x55EC50, `CWorld::CloneItem` 0x55E940, `FindUnitById` 0x55FB00, `SetTime / SetTimeScale / GetHourAndRate / IsNight /
NightFactor` 0x55FB30..0x55FC50, the RefHolder / RefListPair helpers 0x55F190..0x55F8E0, `ObjList_WriteStream` 0x55E8A0, the record copies
0x55E1C0..0x55E510, `World_MapPath / World_InitWorkDir` 0x55E990 / 0x55EA10, `CWorldServer::ReplicateToPlayer`, `SendZoneState`,
`SendPlayerPosition`, `PlayerIn / PlayerOut`, `GetZoneExit`, `CollectLootUnitsFor`, `StorePartyFromUnits`, `ClearPartyItems`, `BroadcastGameTime`,
`GiveExperience`, `OnUnitDied`.

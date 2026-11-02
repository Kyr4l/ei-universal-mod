# game.exe (Russian build, EIStarter OBT-1) - reverse engineering map

Target: `EIStarter OBT-1/Engine/game.exe`, md5 6b6db6a470eed909b5e26018653cc07e, MSVC 6.0, STLport statically linked, no RTTI;
`.text` 0x401000-0x73B000 (the CRT / STLport tail starts at 0x6E0000). Only this exe is covered; the western build is not.

How the map was made: Ghidra 12 with Decompiler Parameter ID, the decompiles typed with the struct fields as they were found
(`_cpr/claude-re/full/`: `functions.tsv`, `structs.tsv`, the per-round `batch-*.tsv`). Every one of the 13797 functions carries a
name (13731 named, 66 still tagged `guess`), about 3400 struct fields are typed by hand, and 15 subsystem pages (2026-10-25 to
2026-11-01) read one subsystem each from the decompiles. Every page tags its claims **[V]** read in the decompile, **[I]** inferred
from several functions, **[?]** guess, and ends with "What is not known" and "Corrections to earlier notes". The network transport
and lobby page (net-transport.md: transport, lobby, ChunkFile, LZ, delta codec) completes the set (2026-11-01).

**The one rule:** when two pages disagree, the newer page wins and lists the correction in its corrections section. The oldest
notes (findings.md, hierarchy.md, classes.md, net-notes.md, item-tables.md) are working notes kept for their layouts; the subsystem
pages supersede them wherever they overlap.

## The engine in one page

**Shell and frame loop** (ui-framework.md 2, world-server.md 2.1). `main` 0x456440 is WinMain: it registers the `EIGAME` class,
creates one borderless `WS_POPUP` window of the configured resolution, reads the settings from
`HKCU\Software\Nival Interactive\EvilIslands` (`AppSettings` g_0x79BC70, every key listed in ui-framework.md 2.5), creates the
D3D7 device and runs a PeekMessage loop. One frame: `Net_TickAll` 0x4122D0, then `CWorldServer::Tick` 0x58DFF0 once per 55 ms
(`g_0x78FF34` = 0x37; 0x1B at double speed, at most 5 ticks per frame), `CWorldClient::Tick` 0x5641F0 on its own accumulator,
`Session_ProcessRequest` 0x57C700 (only when a zone change / load / menu request is pending), `CInterface::ProcessPendingScreens`
0x5F57C0, `App_RenderFrame` 0x458010. Input is Win32 messages (`App_HandleInputMessage` 0x457970); DirectInput serves only the
mouse delta of the drag-look. The cursor is drawn in software (`CSoftCursor`).

**Client and server in one process** (world-server.md, world-client-players.md). Two world objects derive from `CWorld` (0x84
bytes: nine managers, clock, wind, water levels): `CWorldServer` g_0x7AFF60 simulates, `CWorldClient` g_0x7AF5B8 renders. In
single player they talk through an in-memory `FileMsgStream` (`CWorldClient::AttachFileStream` 0x566CE0); the server treats it
like a TCP peer. Each player is a `CPlayer` (0x134) plus a connection object: `CPlayerServer` (0x5128) on the server,
`CPlayerClient` (0x50F8) on the client. Per server tick `ReplicateToPlayer` 0x58B750 sends the non-unit objects within 100 m of the
player's view point and the units the party can sense (msg 0x40 create / 0x41 delta / 0x42 remove), the effects (0x50 / 0x51 / 0x52) and the world state (0x70 rain,
0x71 time, 0x72 wind, 0x73 water levels); orders go the other way as plain sub-messages (0x30 move, 0x31 attack, 0x32 / 0x33 cast,
0x36 interact). The protocol is two LZ streams of sub-messages (25 client -> server and 41 server -> client opcodes); the lobby and the UDP / TCP
transport under them are net-transport.md.

**Object model** (hierarchy.md, classes.md, renderer.md 3.1). `CObject` (0x1C: vtable, refcount +4, flags +8, id +0xC, typeId +0x10,
manager +0x14) -> `CObject3D` (0x170: position +0x1C, quaternion +0x34, parts +0xD0, complection +0xC4, render scene +0xF0) ->
`CMapObject` (buffs +0x194, the net sections, a "specific" at +0x170) -> `CUnit` (0x4DC; its `CUnitServer` 0xB20 at +0x240 on the
server, `CUnitClientSpecific` on the client), `CTree`, `CBuilding`, `CStone`, `CStuffObject`, `CTrapObject`, `CMagicTrapObject`,
`CLeverObject`, `CTorchObject`. Objects live in managers (`CBaseManager` vtable 0x73B870, 25 slots: id map, pools; created by
`CWorld::CreateManagers` 0x55EE40) and come from `.mob` descriptors: `MobFile_Load` 0x481660 builds `CObjectDesc` / `CUnitDesc` /
lever / trap / light / sound / particle records, `CWorldServer::CreateObjectFromDesc` 0x5927C0 turns their kind (+0x48) into an
object. The server keeps six id hashes (all +0x12D70, dynamic, static, named, effects, active +0x104); only the active set ticks in a
playable zone (world-server.md 4.3).

**How a zone loads** (world-server.md 4.5, world-client-players.md 8). `Game_EnterZone` 0x57C460 -> `Session_EnterZoneFromParty`
0x57A060: a first visit runs `CWorldServer::LoadZone` 0x595480 (`CAIMap::Load` 0x5B43C0 of the `.mpr`, the AIGRAPH of the `.mob`,
`LoadMapObjects` 0x596030, the zone script compiled into the zone VM); a return visit reads `saves\current\<zone>.sav`
(`LoadSave` 0x596E70) and simulates up to 1000 ticks of the elapsed campaign time. The client receives msg 0x02 and runs
`CWorldClient::LoadZone` 0x567AB0 (`CTerrain::Load` 0x6C68B0, day-light tables, composed hero skins), answers 0x09, and
`PlayerIn` 0x590700 spawns the party heroes. Leaving writes `<zone>.sav`; a save slot is a copy of `saves\current\` plus
`scenario.sav` and `mission.sav` (`MpFile` container, head word, version 0x74, LZ).

**How a frame renders** (renderer.md, terrain-lighting.md, world-client-players.md 8). `App_RenderFrame` -> `BeginFrame` 0x503F40
-> `CInterface::Render3D` 0x5F5220 -> the top screen's Draw -> `CWorldClient::Render` 0x569E40: sky, `CTerrain::RenderOpaque`
0x6C6230, `RenderObjects` 0x56AFE0 (`ClassifyCellObjects` 0x56A990 culls the 16-unit grid cells around the camera into draw and
shadow lists sorted by material), `RenderWater` 0x6C6330, fog of war, particles, lightning; then `EndFrame`, `Draw2D` (GDI blit
surfaces), the cursor, the flip. Everything is CPU-transformed: `3DfpFPU.dll` (`InitRenderer` 0x500B70) feeds pre-transformed
`VertexTL` (FVF 0x1C4) to `DrawIndexedPrimitive`; the vertex-buffer path is dead. A model is a `CRenderScene` per (model,
complection), blended from the eight `.fig` corners (`FindOrCreateScene` 0x4F7150); a part is drawn by
`CFigureClientSpecific::RenderLit` 0x5A7A70 with the pipeline's light kernel (slot 40). The terrain is 32 x 32 unit sectors in an
MRU cache (`CTerrain::GetSector` 0x6C8140), lit per vertex on the CPU (`TerrainSector_Relight` 0x6C3A00); the sun is rewritten
every frame by `CWorldClient::UpdateDayLight` 0x566360. Animation is one key per world tick (`CAnimationSystem2::SetTime`
0x50D600). The vanilla shadow is a 64 x 64 silhouette draped on the terrain (`RenderShadow` 0x51CE60).

**How a unit acts** (combat.md, unit-ai.md, pathfinding.md). `CUnitServer::Update` 0x552300 runs once per tick for every active
unit: regeneration, the senses (`AiMemory::Tick` inside `Brain::Tick` 0x5D53B0: vision cone + `TraceVisibility`, event-driven
hearing with a 26-tick delay), every motivation's `Evaluate` (Guard / Patrol / Sentry 50, Suspection = interest, Aggression 10000,
Fear 15000 / 3000, Player 10, Tamed 1e8), the winner's `Update`, then the order machine. Orders are a two-slot queue
(queued +0x1B4 -> current +0x190, `DispatchQueuedOrder` 0x54A900) over seven states; a combat order plans the swing
(`PlanActionApproach` 0x54CA40), rolls the hit at the swing start (`ComputeHitChance` 0x555BD0 on the defender) and applies it at
the action end tick `+0xE0` (`TakeHit` 0x553F40 -> `UnitStats_ApplyHit` 0x5254B0: seven damage types, per body part, armor tiers; the unit HP is
derived from the parts). Movement: `CAIMap::FindPath` 0x5BB350 is Dijkstra on 0.5 m tiles in a 32 x 32 window, with the `.mob`
AIGRAPH for long hauls; units are stamped into byte grids, not looked up; the result is a `MoveRequest` turned into a Hermite
`MoveCurve` that both sides evaluate per tick (`TickMovement` 0x547890 on the server, `ReplayMove` 0x535850 on the client). Stats
are `UnitStats` (0x704 at `CUnitServer +0x1E0`), packed into `CUnit +0x330` for the client.

**Spells, items, scripts** (spells.md, items.md, script-vm.md, gs-functions.md). A spell is a `spells.sdb` prototype row plus up to
eight runes (`CSpellContainer::Finalize` 0x68BF60); casting is order 4 / 5 -> `CastOrderSpell` 0x54EE10 -> `CWorldServer::CastSpellMulti`
0x58E850 -> one `CSpellEffect` (0xA8) state machine per target; damage goes through the ordinary hit record; buffs are 0x14-byte
records on `CMapObject +0x198`, ticked down by `CMapObjectServer::TickBuffs` 0x515820; the visuals are server-owned `CEffect*` objects
the client draws. An item is a `CItem` (0x4C, id = proto row << 16 | material id) built once from an `items.idb` row and a material row
(`CItemConstructor::ApplyProto` 0x6A5D60); heroes keep their items in a `HeroRecord` (0x784) inside `CParty` (the `.mp` file).
Scripts: `CWorldServer` embeds two `GSWorld` VMs (+0x12874 zone script, saved; +0x12A6C console and one-shot texts).
`Compiler_CompileText` 0x4D23A0 compiles WorldScript text into `SSCCmd` trees, `GSWorld::Tick` 0x4BF000 schedules them,
`SSCScript::Run` 0x4B6760 is the switch over the 225-entry function table g_0x78BCA0. Scripts run on the server only.

**The UI** (ui-framework.md, ui-screens.md). `CInterface` g_0x7C7D50 owns a stack of `CUIWindow` screens; the server picks the
screen through msg 0x07 (`CreateScreenById` 0x5F5BA0: 1 battle HUD, 2 main menu, 3 dialog, 4 world map, 5 camp, 7 free camera,
8 credits, 9 exchange). Every widget is a `CObject3D` (`CUIWidget` 0x148) whose quads sit at depth z inside the scene, laid out in a
virtual 800 x 600 space; text is GDI on `CUIBlitSurface`s blitted after the 3D pass; events reach the `IUIEvents` sub-object at
+0x94 (handlers first, then children by rect). The camera `CCamera` g_0x79B2A8 is a target / yaw / pitch / distance model eased
per tick. The console (scancode 0x29) runs `@expr` lines through the console VM.

**Data files** (data-sound.md, items.md, spells.md, terrain-lighting.md, docs/file-formats/). `database.res` holds the tagged
archives (`Archive_ReadTagged` 0x409070: `[tag][len*2 | wide bit][bytes]`, fields found by tag, none mandatory): `items.idb`,
`spells.sdb`, `perks.pdb`, `units.udb`, `Levers.ldb`, `prints.db`, `acks.db`, `quests.qdb`. `.res` archives are read through
`CMappedResource` (world-server.md 5): `figures.res` (`.fig` meshes, `.bon` / `.lnk` skeletons, `.anm` clips, `.adb` clip tables),
`textures.res` (`.mmp`), the zone `.mpr` (`.mp` header + one `.sec` per sector), the `.mob` (ChunkFile nodes), `aiinfo.res`,
`sounds*.res`. Saves and party files share `MpFile` / `CParty::Write` 0x661C10. Particles are one `CParticle` (0x1F0) per
effect with a constant table per type (`CParticle::Init` 0x485540); sound is Miles with 15 channels (`CSoundManager` g_0x7AF784)
and an mp3 music thread directed by `CMusicSystem` from `music.reg`. File-level layouts: docs/file-formats/ (adb, aigraph,
database, figure, mmp, mob, mp, reg, res, sav, umtexts).

**The network** (world-client-players.md, net-transport.md). The in-game protocol (opcodes, payloads, handlers, join / leave /
respawn sequences) is world-client-players.md 3-6; the transport, the lobby, ChunkFile, LZ and the delta codec are the network
transport and lobby page (net-transport.md), with the raw layouts of net-notes.md behind it.

## Which page for what

| Question | Page | Section |
|---|---|---|
| Startup order, command line switches, what `main` creates | ui-framework.md | 2.1 |
| Every registry settings key and its `AppSettings` offset | ui-framework.md | 2.5 |
| The frame loop, the 55 ms accumulators, the client speed factor | world-server.md; ui-framework.md | 2.1; 2.3 |
| Zone kinds, server modes, `map.txt` zone records | world-server.md; ui-framework.md | 2.3; 2.6 |
| What a zone load does, step by step (server) | world-server.md | 4.5 |
| What a zone load does (client: terrain, skins, day light) | world-client-players.md | 8 |
| Which objects tick (the active set, sleeping units) | world-server.md | 4.3 |
| `.mob` descriptor kinds -> objects | world-server.md; data-sound.md | 4.4; 6 |
| `.mob` chunk ids and fields (OBJECT, UNIT, UNIT_LOGIC, LIGHT, SOUND ...) | data-sound.md | 6.1-6.3 |
| The save file chunks (`mission.sav`, `<zone>.sav`) | world-server.md | 4.8 |
| Save slots, `saves\current`, `scenario.sav`, `info.sav`, `shot.sav` | world-server.md; ui-screens.md | 2.5; 7 |
| What the script VM saves (chunk 12) | script-vm.md | 8 |
| Replication: what a client is told and when | world-server.md; world-client-players.md | 4.7; 6 |
| The in-game opcodes (client -> server, server -> client) | world-client-players.md | 4, 5 |
| Join, leave, zone change, respawn, disconnect sequences | world-client-players.md | 3 |
| `CPlayer` / `CPlayerClient` / `CPlayerServer` layouts | world-client-players.md | 7 |
| Lobby, transport, ChunkFile, LZ, delta codec | net-transport.md; net-notes.md | 3 transport (packet types, handshake, reliability, 220-byte budget), 4 lobby catalogue, 5 primitives, 7 corrections |
| The stat scramble (anti-tamper) | world-server.md | 4.9 |
| `.res` archive readers, `CMappedResource`, `CObjectManager` | world-server.md | 5 |
| The server unit tick, order of operations | combat.md; unit-ai.md | 2; 2.1 |
| Order kinds, the two-slot queue, the entry points | combat.md | 3.1 |
| The hit chance formula and the called-shot penalties | combat.md | 4.2 |
| Damage per body part, armor tiers, crippling, death | combat.md | 4.4 |
| Max HP, level, action speed, attack period, regeneration | combat.md | 5 |
| Attack animation selection and clip keys | combat.md | 4.5 |
| Arrows and ranged hits | combat.md; spells.md | 4.4; 7.2 |
| What the client computes for a unit (replicated sections) | combat.md | 8 |
| How the AI picks a target (the score) | unit-ai.md | 7 |
| The motivations as state machines and their priorities | unit-ai.md | 3.5, 6 |
| Vision, peripheral sight, sense life, hearing | unit-ai.md | 4 |
| Relations masks, call for help, alarm propagation, alarm slots | unit-ai.md | 5 |
| The `.mob` logic slot fields (Model, AggrMode, Help, Wait ...) | unit-ai.md; data-sound.md | 3.6; 6.2 |
| Party follow, regroup, the battle button | unit-ai.md; pathfinding.md | 6.9, 8.3; 5.5 |
| Passability nibbles, slope tables, object volumes | pathfinding.md | 3.1-3.2 |
| Unit stamps and unit-vs-unit collision | pathfinding.md | 3.3 |
| The tile and nav-level path search | pathfinding.md | 4 |
| `MoveRequest` wire format, `MoveCurve`, speeds | pathfinding.md | 5.1-5.3 |
| Blocking, waiting, re-pathing, follow chains | pathfinding.md | 5.4 |
| `spells.sdb` prototype fields and the rune formulas | spells.md | 2, 3.1 |
| The cast path (checks, animation, target selection) | spells.md; combat.md | 4; 7 |
| The spell kind table and what each spell does | spells.md | 2.2, 5.2 |
| The buff kind table (record, readers, visuals) | spells.md | 6 |
| The `CEffect*` object classes and their replication | spells.md | 7 |
| `items.idb` fields by tag | items.md | 2 |
| The item price / weight / durability formulas (`ApplyProto`) | items.md | 3 |
| `HeroRecord`, `CParty`, the `.mp` file | items.md | 5 |
| Equipment slots, durability wear, loot | items.md; combat.md | 6; 6 |
| The shop stock generator | items.md | 7 |
| Camp prices, constructor rules, tabs | ui-screens.md | 4 |
| The script grammar as parsed, lexer quirks, compile errors | script-vm.md | 2-4 |
| Script scheduling (`if/then` pairs, waits, `For`) | script-vm.md | 6 |
| The 225 script functions, parameters, handlers | gs-functions.md | 3, 4 |
| Where the code differs from the vendor scripting reference | gs-functions.md | 6 |
| The console: commands, cheat mode, `@expr` | ui-framework.md; script-vm.md | 4.7; 7 |
| `keyboard.ini` actions and key names | ui-framework.md; ui-screens.md | 3.1; 10 |
| The camera model, controllers, `.cam` files | ui-framework.md | 3.5-3.6 |
| Screen ids, the screen stack, draw order | ui-framework.md; ui-screens.md | 4; 2 |
| Widget vtable slots, event dispatch, quad builders | ui-framework.md | 5 |
| How a HUD click becomes an order message | ui-screens.md | 12 |
| The dialog screen and briefing scripts | ui-screens.md | 3 |
| The options table (globals per option) | ui-screens.md | 10 |
| The D3D7 device, render states, state blocks | renderer.md | 2 |
| The figure morph blend (`.fig` corners -> mesh) | renderer.md | 3.3 |
| Drawing an object and a part (lights, textures, materials) | renderer.md | 3.4-3.5 |
| Animation loading and playback (`.bon`, `.anm`, `.adb`) | renderer.md | 4 |
| Shadows (vanilla silhouette, terrain self-shadow, receiver mesh) | renderer.md; terrain-lighting.md | 3.6; 4.3, 10.3 |
| The `.mp` header and `.sec` sector layout | terrain-lighting.md | 2 |
| Terrain lighting formulas (sun, ambient, point lights, water) | terrain-lighting.md | 4 |
| Water levels, waves, the stripes | terrain-lighting.md | 5 |
| Textures (`.mmp` load path, formats, cache budget) | terrain-lighting.md | 6 |
| Sky, fog of war, lightning, prints | terrain-lighting.md | 7-10 |
| The tagged database archive format | data-sound.md | 2 |
| `units.udb` fields (Monsters, RaceModels, HitLocations, NPC) | data-sound.md | 3 |
| `quests.qdb` and `quest.reg` | data-sound.md; ui-screens.md | 4; 9 |
| Particle types and the particle loop | data-sound.md | 7 |
| Sound channels, attenuation, the sample cache | data-sound.md | 8.2 |
| Music tracks and the director | data-sound.md | 8.3 |

## Module map

The linker kept each source file contiguous, so `.text` splits into themed ranges (modules.tsv, generated; modules.md lists the
largest functions of each). "Page" is where the range is read; "named only" means the functions carry names and one-line
purposes in functions.tsv but no page reads the range as a whole.

| range | what | group | page |
|---|---|---|---|
| 0x401000-0x402000 | math: vec3/quat helpers (CObject3D transform) | leaf | named only |
| 0x402000-0x405000 | CObject root + CBaseManager (id map, pools, RegisterInstant) | core | hierarchy.md, classes.md |
| 0x405000-0x40C000 | STLport containers / node allocator (no strings) | leaf | CRT / STLport (stlport.md) |
| 0x40C000-0x411000 | CCamera (+ camera .cam files) | client | ui-framework.md 3.6 |
| 0x411000-0x414000 | CCamera helpers, STLport deque, Net_TickAll, ServerInfo / JoinMessage / ChatMessage / NamedEventMessage, LobbyServer::ctor | net | ui-framework.md 3.6; net-transport.md |
| 0x414000-0x41A000 | net message classes (NetMessage subclasses), LobbyServer map list, variables, file transfers, player events | net | net-transport.md |
| 0x41A000-0x42D000 | net lobby: LobbyServer / LobbyClient (players, chat, maps, variables, file transfers, pairing/party records), ChunkFile fields | net | net-transport.md |
| 0x42D000-0x42E000 | CClientSpecificManager; ChunkFile (tag+length chunk reader-writer over CFile), vector<byte>, polygon clipping | core | net-transport.md (ChunkFile); rest named only |
| 0x42E000-0x433000 | GameBuffer; Vertex32_Lerp, OBB tests, Mat3_Solve, LZ compressor, BigNum_ModExp + RIPEMD-128 key hash, NetPeer::ctor | leaf | net-transport.md (LZ, NetPeer); rest named only |
| 0x433000-0x444000 | net transport: NetConnection / NetServer / NetClient | net | net-transport.md |
| 0x444000-0x446000 | debug text (Courier New) + misc | client | named only |
| 0x446000-0x447000 | CFakeUnit | units | named only |
| 0x447000-0x453000 | 3ds model import / figure export (editor leftover): Scene3ds_*, WeldVertices, AnimNode_Write .ani / .ast / .bon | figures | named only (editor leftover) |
| 0x453000-0x455000 | CFigure base in-place constructors, CFakeFigure dtor, hash / CList / aligned vector helpers | figures | renderer.md 3.3 |
| 0x455000-0x456000 | CObject helpers (GetWorld, SetManager), PlayerRelations, disk / capture | core | unit-ai.md 5.1 (PlayerRelations) |
| 0x456000-0x461000 | application: main window, settings sections, DX7 check, sysmsg.txt, map.txt | app | ui-framework.md 2 |
| 0x461000-0x463000 | CHoover (loot collector) + CInventory + keyboard.ini | client | items.md 6; ui-framework.md 3.1 |
| 0x463000-0x466000 | Levers.ldb loader | data | data-sound.md 5 |
| 0x466000-0x469000 | CLightning (lightning.dat) | effects | terrain-lighting.md 9 |
| 0x469000-0x470000 | .mob object descriptors (CObjectDesc / CUnitDesc / CLightDesc / CSoundDesc / CParticleDesc / torch / lever / trap) + 3DS figure builder | app | data-sound.md 6; world-server.md 4.4 |
| 0x470000-0x474000 | AI state names + DirectX error text | misc | named only |
| 0x474000-0x47E000 | MFC CArray/CMap instantiations, IStream/CMemStream/CFileStream, MsgStream family, CTcpNetDriver, Huffman coder, unit prototype apply | net | net-transport.md; net-notes.md |
| 0x47E000-0x481000 | net delta codec: DeltaWriter/DeltaReader, DeltaCodecSet, GameException, FatalError | net | net-transport.md; net-notes.md |
| 0x481000-0x484000 | mission/zone file loader (MobFile) + std::map instantiations | renderer | world-server.md 4.4; data-sound.md 6 |
| 0x484000-0x499000 | particle effects (CParticle) | effects | data-sound.md 7 |
| 0x499000-0x4B7000 | script API (GS* functions) + colour/percentage parsers | script | gs-functions.md 3; script-vm.md 7 |
| 0x4B7000-0x4BB000 | script API (cont.) | script | gs-functions.md 3; script-vm.md 7 |
| 0x4BB000-0x4D6000 | WorldScript compiler (lexer, #define, GSWorld) + spell name table | script | script-vm.md 2-6 |
| 0x4D6000-0x4D7000 | CServerSpecificManager + CTorchObjectSpecific; ClipBuf / ShadowProjector | core | terrain-lighting.md 10.3 |
| 0x4D7000-0x4DA000 | CSkyObject / CSky | client | terrain-lighting.md 7 |
| 0x4DA000-0x4DB000 | GameBuffer (stream) | leaf | named only |
| 0x4DB000-0x4E1000 | CSysInfo + CTextManager | app | named only |
| 0x4E1000-0x4E3000 | CVisibilityFog / CVisFogManager | client | terrain-lighting.md 8 |
| 0x4E3000-0x501000 | renderer: D3D7 device, state blocks, vertex buffers (dead), MMX | renderer | renderer.md 2 |
| 0x501000-0x50A000 | renderer: I3DFigureImpl (frame, draw entries) + old animation code | renderer | renderer.md 2, 4.4 |
| 0x50A000-0x50E000 | CAnimationTrack / CAnimationNode / CAnimationSystem2 | figures | renderer.md 4 |
| 0x50E000-0x51A000 | CMapObject (net state, buffs, specifics) + CMapObjectClientSpecific + CMapObjectServer + CObject3D (parts, LoadModel) + unit prototype lookup | objects | renderer.md 3.1, 5; spells.md 6 |
| 0x51A000-0x520000 | CObject3DClientSpecific (texture sets, materials, skins, wounds, shadows, invisibility) + CObject3DServer + perks.pdb | objects | renderer.md 3.4-3.6; combat.md 5 |
| 0x520000-0x529000 | unit stats & damage model (UnitStats, damage types, hit locations) | units | combat.md 4-5 |
| 0x529000-0x52E000 | CTree / CBuilding / CStone / CStuffObject / CTrapObject / CMagicTrapObject / CLeverObject / CTorchObject + udb record serializers | objects | world-server.md 4.4; pathfinding.md 3.4; data-sound.md 3 |
| 0x52E000-0x533000 | CUnit (+ CFakeUnit overrides) | units | combat.md; items.md 6 |
| 0x533000-0x53A000 | CUnitClientSpecific | units | combat.md 8 |
| 0x53A000-0x545000 | units database (units.udb) | data | data-sound.md 3 |
| 0x545000-0x557000 | CUnitServer (update, combat, orders) | units | combat.md 2-4 |
| 0x557000-0x55E000 | CUnitServer helpers (no strings) | units | combat.md 2-4 |
| 0x55E000-0x565000 | CWorld base (managers, time of day) + CWorldClient start (grids, CBridgeManager, sky, day-light tables, CWeather) | world | world-server.md 3; world-client-players.md 8 |
| 0x565000-0x579000 | CWorldClient: LoadZone, Render / RenderObjects, picking, prints, zone exits, hero units, net attach | world | world-client-players.md 8 |
| 0x579000-0x57A000 | game session helpers: stat scramble, MpFile, MainMenu entry | ui | world-server.md 2.5, 4.9 |
| 0x57A000-0x58A000 | game session: saves, zone entry, lobby glue, databases, scenario / mission.sav | app | world-server.md 2 |
| 0x58A000-0x595000 | CWorldServer: object creation, hashes, replication, weather / time, spells, hero spawning, loot, experience | world | world-server.md 4 |
| 0x595000-0x59F000 | CWorldServer::LoadZone / NewZone / LoadMapObjects / LoadSave / WriteSave / UnloadZone + container instantiations | world | world-server.md 4.5, 4.8 |
| 0x59F000-0x5AD000 | CFigure (.fig mesh, morph deltas, bounds) + CFigureClientSpecific (Render / RenderLit) + CFigureManager (figures.res, skeletons, clips) | figures | renderer.md 3-4 |
| 0x5AD000-0x5C6000 | CAIMap: grids, collision map, path finders, nav grid, stamps; AiAcknowledge | ai | pathfinding.md 3-4; unit-ai.md 5.5 |
| 0x5C6000-0x5C8000 | MoveRequest / CMover / MoveCurve | units | pathfinding.md 5 |
| 0x5C8000-0x5D3000 | unit AI: perception (StimulusMap, AiMemory), AiInfo, spell scoring | ai | unit-ai.md 3-4, 7 |
| 0x5D3000-0x5E2000 | unit AI: the motivations (AiLogic*), Brain | ai | unit-ai.md 6, 8 |
| 0x5E2000-0x5E3000 | IUIEvents defaults + CUIMessageLog | ui | ui-framework.md 4.6 |
| 0x5E3000-0x5E7000 | console (CUIConsole), software cursor (CSoftCursor), ScreenFade, CI3DFigure | renderer | ui-framework.md 3.3, 4.7; renderer.md 3.5 |
| 0x5E7000-0x5EE000 | UI: camera controllers, UIQuadMesh, unit previews / portraits, camp frame widgets | ui | ui-framework.md 3.5, 5.5, 5.8 |
| 0x5EE000-0x5FD000 | UI: CInterface screen manager, item / spell / perk info panels, STL leaves | ui | ui-framework.md 4, 5.9 |
| 0x5FD000-0x602000 | UI: CUIWidget / CUIWindow (arrays, dispatch, draw), CUIBlitSurface, UIButton | ui | ui-framework.md 5 |
| 0x602000-0x60E000 | UI: dialog screen (speakers, topics, briefing), lobby status bar | ui | ui-screens.md 3 |
| 0x60E000-0x645000 | UI: camp, party screen, lobby, exchange, esc menu, credits, world map, main menu, boxes, battle HUD | ui | ui-screens.md 4-8, 12 |
| 0x645000-0x65F000 | UI: HUD strips, quest log, options, save / load, tutorial, generic controls, DirectInput mouse, RichEdit | ui | ui-screens.md 7, 9-13 |
| 0x65F000-0x664000 | CParty (HeroRecord 0x784, party items, variables), PartyStore (*.mp), credits scroller | world | items.md 5 |
| 0x664000-0x674000 | CPlayer / CPlayerClient / CPlayerServer: in-game protocol, party sync, tracked objects, CPlayerManager, CMappedResource | net | world-client-players.md 2-7; world-server.md 5 |
| 0x674000-0x676000 | resources: CResourceBase / CMappedFile + CObjectManager | data | world-server.md 5 |
| 0x676000-0x683000 | CEffect* (sounds, lights, particles, arrows, walls, lightning, tornado, moshka) | effects | spells.md 7 |
| 0x683000-0x68C000 | CSpell / CSpellContainer / CSpellConstructor / CSpellManager, spells.sdb | spells | spells.md 2-3 |
| 0x68C000-0x68F000 | CLight / CLightManager / CMaterial / CMaterialManager | renderer | terrain-lighting.md 4.5-4.6 |
| 0x68F000-0x69F000 | CTexture / CTextureManager (.mmp), CFileRegistry, CIniFile, CWinRegistry, ShopStock (0x699000), STL leaves | renderer | terrain-lighting.md 6; items.md 7 |
| 0x69F000-0x6AE000 | CItem* + CItemConstructor + CItemManager + items.idb | items | items.md 2-4 |
| 0x6AE000-0x6B4000 | quests database (quests.qdb, quest.reg) | data | data-sound.md 4 |
| 0x6B4000-0x6C3000 | CMusicSystem / CSoundManager / mp3 (Miles) | sound | data-sound.md 8 |
| 0x6C3000-0x6CC000 | CTerrain: .mp / .sec, sector cache, lighting, water, textures, render passes, lights | world | terrain-lighting.md 2-5 |
| 0x6CC000-0x6E0000 | terrain prints (decals), CBitmapDib, prints.db / acks.db, WaterLevels | renderer | terrain-lighting.md 10-11 |
| 0x6E0000-0x73B000 | MSVC runtime, MFC 6 pieces, STLport | crt | CRT / STLport (library names where recognisable) |

## Files

Generated tables (from `_cpr/claude-re/full/`, the working database of the reverse):

- [functions.tsv](functions.tsv): every function (addr, size, name, status, one-line purpose), 13797 rows. Written by
  `tools/export_docs.py` from the full table (which `tools/name.py` updates and `tools/sync_funcs.py` keeps in step with Ghidra).
  Generic names such as `GetField_8_xxxxxx`, `InitFields4_xxxxxx`, `SmartPtr_CallSlot4_X` or `HeapArray_Free_X` come from a shape
  classifier (`tools/trivcls.py`, `tools/autostl.py`); the number or letter only keeps them unique.
- [structs.tsv](structs.tsv): the typed struct fields (class, parent, size, off, type, name, note), 3386 rows over 307 classes.
  Merged by hand from the per-round `batch-*-structs.tsv` with `tools/merge_structs.py`; a copy of the full table.
- [layouts.tsv](layouts.tsv): the raw field harvest of 2026-10-18 (class, offset, C types seen, method count, example methods) made by
  `tools/layouts.py` from the decompiles; frozen since 2026-10-27 (do not re-run on the typed decompiles). structs.tsv supersedes it.
- [modules.tsv](modules.tsv) / [modules.md](modules.md): the address ranges above with their function counts (`tools/export_docs.py`),
  and per module the largest functions with purposes (`tools/modules_doc.py`).
- [flows.md](flows.md): pruned call trees from the key entry points (main loop, session, zone load, render, unit tick, orders,
  network receive, script tick, save / load), two levels deep (`tools/flows.py`).
- [gs-functions.md](gs-functions.md) section 4 and `_cpr/claude-re/full/gsfuncs.tsv`: the 225-entry script function table dumped
  from the exe by `tools/gsfuncs.py`.

Hand-written pages: the 14 subsystem pages listed in the table above plus net-transport.md; [findings.md](findings.md) (the
chronological log of the reverse with the first layouts), [hierarchy.md](hierarchy.md) / [classes.md](classes.md) (the root
classes, vtable slot tables, kinds and typeIds), [net-notes.md](net-notes.md) (stream, codec, lobby and party layouts),
[stlport.md](stlport.md) (STLport container layouts to recognise in memory), [item-tables.md](item-tables.md) (what an item id
outside the tables does), [rebuild.md](rebuild.md) (rebuilding the exe from the lifted source: byte-identical asm stage and the
hybrid C stage).

Conventions shared by all pages:

- Tags: **[V]** read directly in the decompile, **[I]** inferred from several functions, **[?]** guess / not resolved. gs-functions.md
  uses the letters V / I / G in a column instead.
- Addresses are virtual addresses in game.exe (image base 0x400000; `.data` VA 0x783000 = file 0x383000). `g_0x...` is a global at
  that address; `vt 0x...` a vtable; "slot N" / `vt[N]` the virtual function at offset N*4 of the vtable.
- Offsets are `this+off` in bytes, hexadecimal. Struct sizes are in bytes.
- "tick" = one server world tick, `CWorldServer +0x64` = g_0x7AFFC4, 55 ms at normal speed (g_0x78FF34 = 0x37; 0x1B at double
  speed; about 18 per second). The client tick is `CWorldClient +0x64` = g_0x7AF61C. One `GSWorld::Tick` = one server tick. The
  game hour is 900 ticks (49.5 s) at the default rate 60; "gtime" is the campaign clock in game hours.
- Distances are metres = world units = the terrain vertex spacing; the AI grid tile is 0.5 m; a terrain sector is 32 x 32 units;
  the path and mover code works in tile units (metres x 2).
- The UI is laid out in a virtual 800 x 600 space (`UiScale::ScaleRect` 0x5F7CA0 maps it to the screen); colours in UI text are
  GDI `COLORREF` BGR, in the renderer D3DCOLOR 0xAARRGGBB.
- Every page ends with "What is not known", "Corrections to earlier notes" and a function index; the batch rows behind a page are
  `_cpr/claude-re/full/batch-<page>.tsv` and `batch-<page>-structs.tsv`.

## Known gaps

Per page, the open items that matter most for the mod work:

- world-server.md: `+0x12E00 staticObjectStream` is filled and never read. (Settled: zone kind 3 = the 16 `bz*` allod bases,
  the hour formula, the save header dwords +120 / +124, `CWorld +0x5C` / `+0x60`.)
- world-client-players.md: nothing open: 0x0C's sender is dead code, 0x12 / 0x19 have senders (script SendEvent, the dialog
  screen). (Settled: 0x54 CBuilding / 0x5D CLeverObject, the 0x40 record read in full 2026-11-03, the 0x73 water levels.)
- combat.md: nothing open for the mod work: order kind 10 is dead code (no reference), the player `+0x108` cheat level only
  replaces the hit record. (Settled: the full 0x0B unit event code list, the max HP floor `max(1, x)`.)
- unit-ai.md: brain +0x44 / +0x48 / +0x4C and `FollowLimit` / `normal.dat` are written, saved and loaded only (no reader); the
  .mob Wait (`MobLogicSlot +0x30`) is unused, the alarm wait is always 75 (a live check confirms it). Sleeping-unit activation:
  world-server.md 4.3; the `+0x5120` party modes: world-client-players.md 7.
- pathfinding.md: nothing open except live checks (`FindUnitsNear` corner miss); `Vec2Q64`, `RotateSpeedMult`, the portal tables
  (the corridor re-plan), `CUnit +0x274` and the wait shift are settled.
- spells.md: the exact `CEffectSoundSeq::Activate` modes; the buff `+0x10` flag byte is never read; shapechange and comprehend
  have no server script. (Settled: no fresh sequence numbers are generated, a copy keeps its id; `SpellProto +0x48 / +0x54 / +0x60`.)
- items.md: nothing open (the proto runtime `CStringArray`s are never filled). Dead / unread: `CItemWeapon +0x4C`,
  the material Skill column, several proto byte fields, `CInventory`.
- renderer.md: pipeline slots 5 and 35 and the clipper live inside `3DfpFPU.dll` (only the call shapes are known);
  the bodies of `BuildFigureMeshB / C` were not read (`LoadFigFile2` is a live path; which objects load the alternate node tracks
  is a live check).
- terrain-lighting.md: `TraceSunRay`'s double slope on diagonal suns is "as decompiled"; which periodic water change the 0.8 s
  stripes are (test with `EnableWaterWaves = 0`); the water pass blend mode (inherited from `RenderObjects`: a live check). (The
  MMP extra block is never read; the sky figure is nask0sky.fig: settled.)
- ui-framework.md: `CamCtlClassic` / `CamCtlSimple` are dead; several static-init globals are unexplained. (`InitQuadUV`, the
  console handlers 0x5DD920-0x5E0A10 (now defined) and the map.txt zone record are settled.)
- ui-screens.md: the options ini writer was not found; `CCampScreen +0x168..+0x18F` and `CCampScreen2 +0x1A0..+0x220` are
  unmapped. (Tooltip ids = text keys `tip N`, `GetMagicTotals` = a quotient: settled.)
- script-vm.md: whether a failed compile of a `.mob` script is reported anywhere (the map runs with a partial script); the second
  auto-id counter `+0x6C` has no reader; `GetBSZValue` reads a stale vtable slot (undefined on the server).
- gs-functions.md: the sound-source respawn loop serves `CreateFXSource` records and skips `CreateRandomizedFXSource` ones (inverted
  test or dead command); `CreateFXSource` vs `CreateFX` differ only in `CEffectSound +0x254`; `HideObject` / `BlockUnit` /
  `ActivateTrap` flip flags without `MarkNetDirty`; the hit record fields of `KillUnit` / `InflictDamage` beyond [8..14].
- data-sound.md: about 40 of the 60 particle callbacks are `LAB_` code without a function symbol; RaceModels `+0x158..` (Unk47..50)
  have no reader. (Settled: the loot fields have no gameplay reader, the 3D pan is not zeroed (decompiler artefact), the XMIDI
  player is dead code, the reverb is set per zone by `LoadZone`, the Levers fields.)

Global:

- 66 functions in functions.tsv are still tagged `guess` (a `?` in the name or purpose); the `Protection_*` functions near the stat
  scramble are among them.
- The CRT, MFC 6 and STLport region 0x6E0000-0x73B000 (1657 functions) and the STLport leaf module 0x405000-0x40C000 carry library
  names only where they were recognised; the rest are generic.
- Editor leftovers are named but not read as code paths: the 3ds import / figure export 0x447000-0x453000, `Scene3ds_BuildFigures`
  0x46F1F0, the old `.ani` animation loaders 0x505410-0x508D30, the terrain editing and `.sec` writer functions in 0x6C3000
  (`TerrainSector_Pack`, `SaveModifiedSectors`, `SetVertexHeight` ...), the script decompiler `GSWorld::Decompile` 0x4D53C0 and the
  five one-list `Compiler_*` entry points, the dead vertex-buffer path of the renderer, `CamCtlClassic` / `CamCtlSimple`,
  `OnMsg38_Unused` and the ten script ids with no `Run` case.
- `net-transport.md` is being written; until it lands, net-notes.md and flows.md "Network receive" are the only map of
  0x411000-0x444000 and 0x474000-0x481000.

## Coverage

Coverage (13820 functions): named 13768, guess 52.

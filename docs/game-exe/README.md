# game.exe (Russian build, EIStarter OBT-1) - reverse engineering map

Generated 2026-10-18 from the full reverse (`_cpr/claude-re/full/`, phase 3 complete). Target: `Engine/game.exe`, md5 6b6db6a470eed909b5e26018653cc07e, MSVC 6.0, STLport statically linked, no RTTI; `.text` 0x401000-0x73B000.

## Files
- [functions.tsv](functions.tsv): every function (CRT, MFC and STLport included) (address, size, name, status, one-line purpose). Status is `named` for all rows. Generic names such as GetField_8_xxxxxx, InitFields4_xxxxxx, SmartPtr_CallSlot4_X or HeapArray_Free_X come from a shape classifier (tiny getters, constructors, thunks, template leaves); the number or letter only keeps names unique. CRT, MFC 6 and STLport functions (0x6E0000-0x73B000) carry their library names where recognisable.
- [modules.md](modules.md): per-module summary with the largest functions and their purposes.
- [modules.tsv](modules.tsv): address ranges with a theme (the linker kept each source file contiguous).
- [gs-functions.md](gs-functions.md): the 225 WorldScript commands and functions (id, name, parameter letters).
- [layouts.tsv](layouts.tsv): derived field layouts per class (class, offset, C types seen, number of methods touching it, example methods); built by `_cpr/claude-re/full/tools/layouts.py` from the decompiles of the class's own methods (inherited base fields are listed under the base class).
- [flows.md](flows.md): call trees (in call order) from the key entry points: startup/main loop, session driver, zone load, rendering, unit tick, orders, network, script, save/load.
- [findings.md](findings.md): chronological notes of the reverse with the discovered class layouts (GSWorld, CTerrain, CObject3D, CMapObject, animation, script runtime...).
- [hierarchy.md](hierarchy.md): class hierarchy and vtable slot tables. [net-notes.md](net-notes.md): network classes and message layouts. [stlport.md](stlport.md): STLport container layouts and node allocator.
- [classes.md](classes.md): class hierarchy draft (CObject / CObject3D / CMapObject slot tables).

## Module map

| range | what | group |
|---|---|---|
| 0x401000-0x402000 | math: vec3/quat helpers (CObject3D transform) | leaf |
| 0x402000-0x405000 | CObject root + CBaseManager (id map, pools, RegisterInstant) | core |
| 0x405000-0x40C000 | STLport containers / node allocator (no strings) | leaf |
| 0x40C000-0x411000 | CCamera (+ camera .cam files) | client |
| 0x411000-0x414000 | CCamera helpers (LookFromTo, SetDirection, AddShake), STLport deque<0x24>, Net_TickAll, ServerInfo / JoinMessage / ChatMessage / NamedEventMessage, LobbyServer::ctor | net |
| 0x414000-0x41A000 | net message classes (NetMessage subclasses: strings / refs / blobs), LobbyServer map list, variables, file transfers, player events | net |
| 0x41A000-0x42D000 | net lobby: LobbyServer / LobbyClient (players, chat, maps, variables, file transfers, pairing/party records), ChunkFile fields, STLport/MFC helpers | net |
| 0x42D000-0x42E000 | CClientSpecificManager; ChunkFile (tag+length chunk reader-writer over CFile), vector<byte>, polygon clipping (ClipTriangle / ClipPoly_*) | core |
| 0x42E000-0x433000 | GameBuffer; Vertex32_Lerp, OBB tests, Mat3_Solve, LZ compressor (LzCompress / LzDecompress), BigNum_ModExp + RIPEMD-128 key hash, NetPeer::ctor | leaf |
| 0x433000-0x444000 | net transport: NetConnection / NetServer / NetClient (A) | net |
| 0x444000-0x446000 | debug text (Courier New) + misc | client |
| 0x446000-0x447000 | CFakeUnit | units |
| 0x447000-0x453000 | 3ds model import / figure export (editor leftover): Scene3ds_ResolveNodesAMF, WeldVertices, ComputeNormals, BuildIndexedComponents, CorrectMappings (.reg atlas), Material_ParseName, AnimNode_Write .ani / .ast / .bon, old CAnimationNode tree building; container serializers | figures |
| 0x453000-0x455000 | CFigure base in-place constructors (CFigures_ConstructN 0x220, CFakeFigures_ConstructN 0x18c = the 3ds mesh record), CFakeFigure dtor, hash / CList / aligned vector helpers | figures |
| 0x455000-0x456000 | CObject helpers (GetWorld, SetManager) + disk / capture | core |
| 0x456000-0x461000 | application: main window, settings sections, DX7 check, sysmsg.txt | app |
| 0x461000-0x463000 | CHoover + CInventory + keyboard.ini | client |
| 0x463000-0x466000 | database.res loader (databases) | data |
| 0x466000-0x469000 | CLight + CLightning (lightning.dat) | effects |
| 0x469000-0x470000 | .mob object descriptors (CObjectDesc / CUnitDesc / CLightDesc / CSoundDesc / CParticleDesc / torch / lever / trap) + 3DS figure builder | app |
| 0x470000-0x474000 | AI state names + DirectX error text | misc |
| 0x474000-0x47E000 | MFC CArray/CMap instantiations, IStream/CMemStream/CFileStream, MsgStream family (file / peer / TCP), CTcpNetDriver, Huffman coder, unit prototype apply | net |
| 0x47E000-0x481000 | net delta codec: DeltaWriter/DeltaReader (16-bit quantized fields vs two base snapshots), DeltaCodecSet, GameException, FatalError, DDraw error text, AnimKeyToString | net |
| 0x481000-0x484000 | mission/zone file loader (MobFile) + std::map<string,list> instantiations | renderer |
| 0x484000-0x499000 | particle effects (CParticle, 'Particle effect ...') | effects |
| 0x499000-0x4B7000 | script API (GS* functions: GetObject*, KillScript) + colour/percentage parsers | script |
| 0x4B7000-0x4BB000 | script API (cont.) | script |
| 0x4BB000-0x4D6000 | WorldScript compiler (lexer, #define, Duplicate script) + spell name table | script |
| 0x4D6000-0x4D7000 | CServerSpecificManager + CTorchObjectSpecific | core |
| 0x4D7000-0x4DA000 | CSkyObject | client |
| 0x4DA000-0x4DB000 | GameBuffer (stream) | leaf |
| 0x4DB000-0x4E1000 | CSysInfo + CTextManager | app |
| 0x4E1000-0x4E3000 | CVisibilityFog / VisionFog | client |
| 0x4E3000-0x501000 | renderer: D3D7 device, state blocks, vertex buffers, MMX | renderer |
| 0x501000-0x50A000 | renderer: I3DFigure / C3DFigureImpl (scene, figure drawing) | renderer |
| 0x50A000-0x50E000 | CAnimationTrack (.anm keys: positions, rotations, morph frames; Evaluate) / CAnimationNode (hierarchy, bound figure part, morph interpolation, UpdateTransform) / CAnimationSystem2 (nodes, animation queue, SetTime / SetAnimation / SetMorphParams / BindPart) | figures |
| 0x50E000-0x51A000 | CMapObject (net state pack / unpack, buffs, specifics) + CMapObjectClientSpecific (buff visual effects + sounds, light) + CMapObjectServer + CObject3D (parts, figure / .adb animation database, LoadModel / LoadFigures / CreatePart, scale, bounds, effects list) + units prototype lookup (Units_FindProtoByName, Unit_InitStatsFromProto, Unit_ApplyPrototype, Unit_GiveProtoEquipment) | objects |
| 0x51A000-0x520000 | CObject3DClientSpecific (texture sets +0x20/+0x24, materials +0x38, skins SetTextureNames, wounds texture + TextureBlend, shadows, invisibility / stun material tweaks, material colour push/pop) + CObject3DServer + perks.pdb database (PerksDb_FindByName / Save) + aligned dword vectors / CString vector helpers | objects |
| 0x520000-0x529000 | unit stats & damage model (perks.pdb, damage types, hit locations) | units |
| 0x529000-0x52E000 | CTree / CBuilding / CStone / CStuffObject / CTrapObject / CMagicTrapObject (areas, cast points, targets, CastAtTargets) / CLeverObject (SwitchState: AI map blocking, animation phase) / CTorchObject + specifics (light, effect, looping sound) + units.udb / prototype / hero record field serializers + UnitStats_BoostMax* | objects |
| 0x52E000-0x533000 | CUnit (+ CFakeUnit overrides) | units |
| 0x533000-0x53A000 | CUnitClientSpecific | units |
| 0x53A000-0x545000 | units database (units.udb prototypes) | data |
| 0x545000-0x557000 | CUnitServer (update, combat, skills, orders) | units |
| 0x557000-0x55E000 | CUnitServer helpers (no strings) | units |
| 0x55E000-0x565000 | CWorld base (managers, time of day, RefListPair +0x60) + CWorldClient (object lists / grids, CBridgeManager walkable quads, sky, day-light tables, CWeather at +0x6f4), CTerrain / CTextManager / CParticleManager / CVisFogManager dtors | world |
| 0x565000-0x579000 | CWorldClient: LoadZone (terrain / sky / day-light tables / skins), Render / RenderObjects / ClassifyCellObjects / shadows, object picking (screen rects), terrain prints (prints.db), zone exits, hero unit creation, net attach (file / TCP streams), session helpers; STL sort / container instantiations | world |
| 0x579000-0x57A000 | game session helpers: stat scramble (anti-tamper), save writer/reader, MainMenu entry, CUIWindow 73F7F8 | ui |
| 0x57A000-0x58A000 | game session: saves, MainMenu, autoexec, scenario/mission.sav, MpState | app |
| 0x58A000-0x595000 | CWorldServer: object creation from descriptions, object hashes (+0x12d70 all, +0x104 active, +0x12de8 effects, +0x12dd0 named), per-player replication (ReplicateToPlayer), weather / time / day-night, spell casting, hero spawning from CParty records, mercenaries, loot, experience, script VM dtor, CAIMap dtor | world |
| 0x595000-0x59F000 | CWorldServer::LoadZone / NewZone / LoadMapObjects / LoadSave / WriteSave / UnloadZone, world script loading, progress videos; the server object hash / list / array container instantiations (IdHash_*, RefList / RefArray / LootRecordList serializers) | world |
| 0x59F000-0x5AD000 | CFigure (.fig mesh: raw SoA buffers +0x1c0.., morph deltas +0x128, bounds FIG8 / FIG6 interpolation, SetMorphDeltas) + CFigureClientSpecific (Render / RenderLit, texture frame animation, screen rect) + CFigureManager (figures.res models: FigureModel records, LoadSkeleton (.lnk), BuildAnimationNode (.bon), LoadNodeTrack, SetupAnimation, LoadPartBounds, CreatePart; old .ani / .bon loaders) | figures |
| 0x5AD000-0x5C6000 | CAIMap: terrain / passability grids, collision map, path finders, nav grid, stamps; Alertness | ai |
| 0x5C6000-0x5C8000 | Mover (unit movement state) | units |
| 0x5C8000-0x5D3000 | unit AI: perception (aiinfo.res, hearing, vision, smell) | ai |
| 0x5D3000-0x5E2000 | unit AI: behaviours (Guard / Patrol / Sentry / CorpseWatcher / Tamed) + #say | ai |
| 0x5E2000-0x5E3000 | IUIEvents defaults + CUIMessageLog (timed chat/message lines) | ui |
| 0x5E3000-0x5E7000 | chat/console panels (CUIMessageLog edit, CUIConsole), software cursor (CSoftCursor), ScreenFade, CI3DFigure | renderer |
| 0x5E7000-0x5EE000 | UI: camera input handlers, UIQuadMesh, unit preview/portraits, camp screen widgets | ui |
| 0x5EE000-0x5FD000 | UI: CInterface screen manager, item/spell/perk info panels, CUIBlitSurface, string-pair arrays, STL leaves | ui |
| 0x5FD000-0x602000 | UI: CUIWindow (element/quad/button/blit-surface arrays, event dispatch), CUIBlitSurface (DirectDraw backing surface) | ui |
| 0x602000-0x60E000 | UI: dialog screen (speakers, topics, briefing), status-lamp screen, STL leaves | ui |
| 0x60E000-0x645000 | UI: camp/inventory/trade/spell+item constructor, hero stats, main menu, save/load, lobby, battle HUD, credits | ui |
| 0x645000-0x65F000 | UI: battle HUD strips, scroll bars, edit/list controls, save/load, options, tutorial, key config; DirectInput mouse; RichEdit | ui |
| 0x65F000-0x664000 | CParty (hero records 0x784 bytes with four item arrays: equipped / B / inventory / spells, party items, named variables, map name), HeroRecord save reader, PartyStore (*.mp folder), PartySlot::SaveToFile, RichEdit credits scroller | world |
| 0x664000-0x674000 | CPlayer / CPlayerClient / CPlayerServer: in-game protocol opcodes (client SendMsgXX / OnMsg_*, server SendMsgXX / OnMsg_*), party sync (MpFile), tracked units and objects, CPlayerManager, CMappedResource (resource file cache), hero record array | net |
| 0x674000-0x676000 | resources: CMappedResource / CResourceBase / CMappedFile + CObjectManager | data |
| 0x676000-0x683000 | CEffect* (sounds, lights, particles, arrows, tornado) | effects |
| 0x683000-0x68C000 | CSpell / CSpellContainer / CSpellConstructor / CSpellManager | spells |
| 0x68C000-0x68F000 | CLight / CLightManager / CMaterial / CMaterialManager | renderer |
| 0x68F000-0x69F000 | CTexture / CTextureManager (.mmp), CFileRegistry (binary key/value registry), CIniFile, CWinRegistry, CItemConstructor, STL leaves | renderer |
| 0x69F000-0x6AE000 | CItem* + CItemManager + items.idb | items |
| 0x6AE000-0x6B4000 | quests database (quests.qdb) | data |
| 0x6B4000-0x6C3000 | CMusicSystem / CSoundManager / mp3 (Miles) | sound |
| 0x6C3000-0x6CC000 | CTerrain: Load (.mp header, materials, tile types, textures + lightmap pages, UV tables), sector LRU cache (GetSector), per-vertex sun / dynamic lighting with shadow ray tracing, water waves + animated tiles, rendering passes, lights link lists, height / material queries, ray to ground, ambient sounds, modified-sector writer | world |
| 0x6CC000-0x6E0000 | terrain prints (foot/blood/fire decals), terrain tile helpers, CBitmapDib (BMP/mip loading), database.res loaders (CDatabase/CDatabase2/CDatabaseTables) | renderer |

## Coverage

Coverage (13785 functions): named 13785.

## Reading guide (how a frame / a map / a save works, as found)
- **Startup and main loop:** application in 0x456000 (main window, settings, DX7 check); per-frame session driver `Game_SessionTick` 0x57C700 (Net_TickAll, loads/saves, MainMenu fallback); renderer `CRenderManager`/`CRenderDevice` in 0x4E3000-0x501000, `I3DFigureImpl::InitRenderer` 0x500B70.
- **Map load:** `Game_EnterZone` 0x57C460 -> `CWorldServer::LoadZone` (0x595000) / `CWorldClient::LoadZone` (0x565000), terrain `CTerrain` (0x6C3000, `.mp`), objects from `.mob` (`Game_LoadZoneMob` 0x57BDF0, MobFile 0x481000), scripts compiled by the WorldScript compiler (0x4BB000).
- **Saves:** `saves\current\` is the working save: `Game_SaveCurrent` 0x57A780, `Game_LoadSavedGame` 0x57ABA0, `Game_SaveToSlot` 0x57B3B0; the UI side is `CSaveLoadWin` (0x645000: ScanSaves 0x655D40, DoSave 0x655550, QuickSave 0x6570E0).
- **Objects:** CObject -> CObject3D -> CMapObject -> CUnit / CTree / CBuilding ... (see classes.md); managers `CBaseManager` (id map, pools); units: `CUnit` 0x52E000, `CUnitServer` 0x545000 (combat, orders), AI 0x5AD000-0x5E2000 (CAIMap, perception, behaviours).
- **Network:** lobby `LobbyServer`/`LobbyClient` 0x41A000 (singletons g_0x79B5D8 client, g_0x79B5DC server), transport 0x433000, delta codec 0x47E000, in-game protocol opcodes `CPlayerClient/Server` 0x664000.
- **Data:** `database.res` loaders 0x463000/0x6CC000, `units.udb` 0x53A000, `items.idb` 0x69F000, `quests.qdb` 0x6AE000, `perks.pdb` 0x520000.
- **UI:** `CInterface` screen manager + `CUIWindow` 0x5EE000-0x602000, screens 0x60E000-0x65F000; vtable slot convention for windows is in the module notes (slots 1 Deactivate, 2-8 mouse, 9-11 keys, 12 wheel, 18 Build, 20 Draw, 24 Update, 25 OnOpen, 26 OnClose, 27 OnCommand).
- **Anti-tamper:** `World_SetStatScramble` 0x5790E0 (XOR scramble with an ISAAC key) and `Protection_*` functions flagged `?`.

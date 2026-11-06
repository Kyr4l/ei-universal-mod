# Game executable bug list (verified 2026-11-06)

The bug candidates found by the 2026-11-06 deep read of the 302 big functions, each verified a second time against the machine
code and the shipped data (paths below are relative to `_cpr/claude-re/`, the RE work folder). Earlier settled bug: the sound-source
respawn test (gs-functions.md section 5, fixed by um-engine `GSWorld::RandomizedSoundSources`).

Offline read of the Russian game.exe (decompiles in `dec2/`, machine code of `navmesh/game_engine.exe` disassembled with capstone).
Verdicts: CONFIRMED (the code does it), REFUTED, UNSURE. Effect: none / cosmetic / gameplay / crash / security.
Data checks used the game data in `drive_c/Evil Islands/res` and `maps` (the data the Russian exe loads) and `Universal-Mod/res`
(UM ships its own items.idb / spells.sdb in databaselmp.res, no quests.qdb, no ambient sounds). None of the 23 is covered by a
SpellAddon setting (docs/SpellAddonInfo.en.md); SpellAddon only widens #17 (`possessioncharmlink`, `newspells`).

| # | verdict | reachable in the shipped game | effect | one line |
|---|---|---|---|---|
| 1 | CONFIRMED, dead code | no (client never sends 1 / 2) | none | tame arm unreachable; steal / tame are cut features |
| 2 | CONFIRMED | yes, every smoothed path | cosmetic | straightness = 2·frac(x) twice, y ignored |
| 3 | CONFIRMED, moot | no (bad hero proto only) | crash either way | the caller derefs the null proto first (0x66225D) |
| 4 | REFUTED | - | none | the owner check is in ExecuteCombatOrder (0x54D9E9); side question on respawned hero ids |
| 5 | CONFIRMED, design | no effect | none | every vanilla remove-list name is a quest item |
| 6 | CONFIRMED | no (no duplicate NID in 210 .mob files) | gameplay (broken mob only) | later desc overwrites, earlier leaked |
| 7 | REFUTED (correct omission) | - | none | ResetUnitEffects = limb-HP refill for a zone being left |
| 8 | CONFIRMED | yes, every MP packet / server list | security (remote crash) | LZ raw length and input end unchecked; output side safe |
| 9 | CONFIRMED | yes, shadows / prints on multi-part bridges | cosmetic | owner geometry emitted once per walkable quad |
| 10 | CONFIRMED | rare (light below the caster) | cosmetic | upward light sum kept; shadow most likely vanishes |
| 11 | partly REFUTED | no | none (crash only if the "" party was removed) | missing "::" / unknown party resolve to the default party, not null |
| 12 | CONFIRMED | no (malformed script only) | crash | Script / DeclareScript at EOF deref a null cursor |
| 13 | CONFIRMED (condition: one message > 12 lines) | modified MP client only | security (heap corruption) | trim erases the sentinel |
| 14 | CONFIRMED | yes, a repeated dialog voice line | cosmetic (sound) | decoded mp3 cache hit skips AIL_set_sample_file |
| 15 | CONFIRMED | yes, every ambient one-shot | cosmetic (sound) | pan 0..100 = centre to one side |
| 16 | CONFIRMED, design | yes (map fires) | none | permanent burn area of fire objects |
| 17 | CONFIRMED | no (SpellAddon unlock / scripts) | none (stuck cast record) | charm failure jumps past phase = 3 |
| 18 | REFUTED | - | none | the limit is a real float (30.0 / 100.0) |
| 19 | CONFIRMED | yes, Ingos night / Gipat morning + night | cosmetic (sound) | count from day folder; 80 % of Ingos night one-shots silent |
| 20 | CONFIRMED | no (loose .bmp textures only) | crash / garbled texture | rows = width, row length = height; byteSize unset |
| 21 | CONFIRMED | no (mod data / UM save without UM) | crash | null weapon / armour row read at +0xC |
| 22 | CONFIRMED | no (no weapon / armour with M.Type none) | crash | FinishItem null then `+4 += 1` |
| 23 | CONFIRMED, dead code | no (no caller) | none | own vs parent +0xFC record |

## 1. CUnitServer::ExecuteInteraction 0x54F000: tame branch unreachable

CONFIRMED (dead code), effect none, a cut feature rather than a bug.

- Outer test in machine code: `0x54F0BC mov eax,[esi+0x194]` (orderParam), `0x54F0C4 cmp eax,1 / je 0x54F0D1`, `0x54F0C9 cmp eax,ebx(0) / jne 0x54F431` (exit). Only params 0 and 1 enter the
  block, so the later `== 2` arm (CUnitServer::OnProvoked, player message 0x16) can never run.
- Server side, param 2 orders do exist: Brain::OrderInteract 0x5D7CE0 mode 2 -> CUnitServer::QueueOrderInteract2 0x555480 queues order kind 6
  param 2 (only caller chain: CPlayerServer::OnMsg36_Interact 0x670310 -> AiLogicPlayer::Interact 0x5D3CF0). Such an order walks to the
  target and then ExecuteInteraction does nothing (no message, no aggro).
- Client side the shipped game never sends 1 or 2: CHudAbilityList::SetUnit 0x63F800 has the three rows compiled
  (`0x63F91F` skill0009 id 0 = science, `0x63F90A` skill0008 id 1 = steal, `0x63F8F5` skill0010 id 2 = tame) but the loop bound is 1
  (`0x63FAC3 inc edi / 0x63FAC4 cmp edi,1 / jb 0x63F8C7`), so only the science row is built. The command-mode setter that would store
  another interaction id (code at 0x637FC0, writes +0x184) has no call and no pointer anywhere in the exe.
- Reachable only by a modified client sending msg 0x36 with interaction 2 (or 1, which does run the steal code). Stealing and taming are
  cut features of the shipped game.

## 2. CAIMap::FindPath 0x5BB350: straightness from x twice

CONFIRMED, reachable on every path of 3+ cells (all unit movement), effect cosmetic (path shape only). A source copy-paste slip, not design.

- `0x5BB807..0x5BB821`: a = |goal.x - round(goal.x)| from `[esi+0x58]`; `0x5BB824..0x5BB841`: b = the same expression again from
  `[esi+0x58]` (mover goal x). `[esi+0x5C]` (goal y, stored at 0x5BB6xx `pfVar2[0x17]`) is never read here. `0x5BB844..0x5BB86E`
  picks min(2a, 2b) = 2a, pushed at `0x5BB8B4` as the last argument (straightness) of CAIMap::SmoothPathStart 0x5C46C0.
- Intended was almost certainly min(2|dx|, 2|dy|) (how far the snapped goal is from the cell grid). With y ignored, SmoothPathStart's
  diagonal-arrival penalty (1 - straightness) * 2048 is computed from the x fraction only, so the last turn of a smoothed path can differ
  from what the author meant. No failure, no stuck units: the path is still valid, only its final bend changes.

## 3. Hero_InitStatsFromProto 0x513DA0: proto +8 read before the null check

CONFIRMED, but the check is moot: the only caller already dereferences the same null pointer first. Effect: crash in both cases (an
access violation instead of the intended FatalError text); not reachable with vanilla data.

- Units_FindProtoByName 0x5144D0 returns 0 for an unknown name (StringHash_FindOrInsert inserts an empty entry).
- `0x513DB2 call 0x5144D0`, `0x513DC3 mov esi,eax`, `0x513DD1 mov edi,[esi+8]` (row index read) ... the null test is only at
  `0x513E0A test esi,esi` (-> FatalError "Can't find prototype (%s) for new HERO").
- The single caller CParty::InitHeroFromProto 0x6621F0 (`0x66227E call 0x513DA0`) does its own `0x662251 call 0x5144D0` and reads
  `0x66225D mov eax,[esi+8]` with no check, so a bad name faults at 0x66225D before Hero_InitStatsFromProto runs. That function's only
  caller is CParty::AddHero 0x6628D0 (from PartyStore::ResetToDefault, the mission script op 0x86 in SSCScript::Run, Console_Join,
  CWorldServer::OnMercenaryEvent, SpawnSingleHero, camp screen).
- Reachable only with a mod / mission script / save naming a hero prototype that is not in units data. Vanilla: no. Fix value: a clean
  error message for modders, nothing more.

## 4. CWorldServer::LootUnit 0x5942D0: loot record given to any looter

REFUTED as a bug: LootUnit itself does not compare the lobby index (true), but the owner check is done earlier, when the loot order runs.
Effect none in normal play.

- Records are made only by CWorldServer::CopyHero 0x5947E0, called from CPlayerServer::Respawn 0x66F4C0 (`0x66F5AC`) in a session: the dead
  hero keeps its old id, the record gets {old hero id, lobby index of LobbyServer::FindPlayerById(old hero id) +0x30 (or -1), the lost money,
  the player's inventory +0xE0}. (LoadSave restores saved ones.)
- LootUnit's record loop matches only `record[+8] == unit id` (unit id), right: the +0xC lobby index is never read there.
- But every pick-up goes through CUnitServer::ExecuteCombatOrder 0x54D960 first: `0x54D9D2 cmp [edi],6 / 0x54D9D7 cmp [edi+4],0x18A95` ->
  `0x54D9E9 call CUnitServer::IsAggroTarget 0x54C760` -> `0x54D9F0 jne 0x54DBFD` (order dropped, 0x545F40). For a body flagged 0x20000000,
  IsAggroTarget returns !CWorldServer::HasLootRecordFor 0x5949D0(body id, looter id), and HasLootRecordFor requires the record's lobby index
  == the looter's lobby index or -1. So another player's loot order on your body is dropped before ExecuteInteraction / LootUnit runs.
  CollectLootUnitsFor 0x594A30 (msg 0x60) uses the same rule to show each player only his own bodies.
- Paths that skip the gate: Console_LootAll 0x5DF5B0 -> LootAllDeadUnits 0x593D60 (host cheat console: the host collects every record,
  other players' dropped inventories included, by design of a cheat) and the steal branch of ExecuteInteraction (`0x54F36B`, not sent by the
  shipped client, see #1).
- Side question, UNSURE: the lobby lookup is by hero unit id (LobbyServer player +0x2C). Respawn gives the hero a new id (old + 250000,
  0x66F4C0 lines 61-80), and the lobby +0x2C is only rewritten by LobbyServer::OnJoinUpdate 0x41AD80. If the client does not re-send its join
  with the new id, a second death files the record with lobby index -1 (anyone may loot it) and the owner's own lookup also falls back to -1.
  Live test: two-instance rig, die twice with items, check whether the other player can loot the second body.

## 5. CWorldServer::OnNamedEvent 0x592090: briefing "remove items" only takes quest items

CONFIRMED in code, but no effect with the game's data: every name in every vanilla "remove items" list is a quest item. Design (the list is
a quest-item list), not a bug.

- Single player only (Lobby_IsAlive == 0). For each name of the briefing record +0x20: CItemManager::CreateItemByName, refcount +1,
  `vt[0x30]` type == 0x14 -> CPlayer::RemoveQuestItemById(player, item +0x4C), then release (vt+0x10). Any other type does nothing.
- Data check: quests.qdb from `drive_c/Evil Islands/res/database.res` (the data the Russian exe loads; parsed offline, tagged archive,
  briefing block = tag 2, field 5). 32 briefings have a remove list (s3, m4, z1, gogo1, z2, z11, s8_2, s12, z13_1, z14, z15, z17, z17_2,
  v19_1, s24, s25_3, z39, Sh41, Yc45, Yc47_2, Yc50, G53, D55_3, Gl64, jf67, Sn69_1, R72, pdm2, pdm4, pds0, v3, s12_5). Every one of the
  41 names (White Wolf Hide, Awl, Knife, SackofGold, Map00, BrokenToy, Ogrekey1/2, ...) is in the quest-item block (block 5) of items.idb.
- Only a mod that lists a weapon / armour / potion name there would see "nothing removed". The created item is released again, no leak.

## 6. MobFile_ParseChunks 0x481F00: duplicate NID replaces and leaks

CONFIRMED in code, not reachable with vanilla or Universal-Mod data. Effect with a broken .mob: the earlier object silently missing +
a small leak (gameplay for that map, no crash).

- Hash insert: bucket walk `0x482183 cmp [eax+4],ecx / je 0x4821AC`; on a hit it jumps straight to `0x4821B5 mov [eax+4],esi` (node value =
  the new descriptor) with no release of the old value, which is then never created (CreateObjectFromDesc runs per hash entry) and never
  freed. The hash is local to one MobFile_Load 0x481660 call (`HashMap_Ctor_4833B0` on its frame), so only duplicates inside ONE file count.
- Data check (offline parse of every .mob, NID = 0xB002 for objects / units / levers / traps / torches, 0xDD02 particles, 0xAA05 lights,
  0xCC02 sounds): 99 game maps in `drive_c/Evil Islands/maps` (52670 descriptors) and the 111 .mob files under the repo `resources/`
  (57468 descriptors): no duplicate NID in any file, no descriptor without a NID.
- Only a hand-edited / tool-merged mob could hit it. Worth a warning in a mob editor (um-multitool), not an exe patch.

## 7. Session_SaveToSlot 0x57B3B0: mission.sav written without ResetUnitEffects

REFUTED as a bug (the fact is right, the omission is correct). Design; effect none.

- CWorldServer::ResetUnitEffects 0x5956E0 has a single caller, `0x57A8E4` in Session_SaveZoneToCurrent 0x57A780. The slot save writes
  mission.sav through its own inline CWorldServer::WriteSave call (`0x57BAF1`); Session_WriteMissionSave 0x579FB0 (`0x57A00F`) does not
  call it either.
- What the function really does (the old "temporary spell effects dropped" label is wrong): for every living unit of the dynamic hash
  +0x12DA0, UnitStats_FillParts 0x525AF0 (the 6 body parts of UnitStats +0x110, 0xF4 each: when state +0xE0 > 1, hp +0xE4 = max +0xE8,
  crippled parts are left alone) then CUnitServer slot 19 RecomputeFromPlacement 0x5533B0 with refreshMax 0. It heals the limbs of the
  units of a zone the party is leaving, so they are rested when the party comes back. Spell lists are untouched.
- Applying that in a slot save would change the LIVE world the player keeps playing (every monster's wounds healed at each save) and
  would make the reloaded game differ from the saved moment. Saving the wounds as they are is the right behaviour.
- Doc fix wanted: the ResetUnitEffects purpose in functions.tsv / world-server.md 4.8 ("temporary spell effects") should read "limb HP
  refill + recompute". (Not edited here: other agents own the pages.)

## 8. LzDecompress 0x430960 and its callers: untrusted raw length, no input bound

CONFIRMED. Reachable in every multiplayer game (each packet a peer sends) and from the master-server list; effect security (remote crash
/ denial of service). No out-of-bounds WRITE found.

- The raw length is the first u32 of the input (`iVar5 = *(int*)ptr`, decompile line "iVar5 = *(int *)local_24.ptr"), taken as is.
  NetCursor::ctor 0x4DA490 then grows the output GameBuffer for that many bytes (vt[1]): a value like 0x7FFFFFF0 asks for a ~2 GB
  allocation (failure -> MFC memory exception / crash). A negative value just produces nothing.
- The input pointer (`pbVar7`, every `*pbVar7++` refill and the inline LzBitReader loops) is never compared to the end of the input
  view (MsgStream_Decompress 0x47D970 builds the view with its end at +0x18 + size, but LzDecompress never reads that end). The loop runs
  until `iVar5` bytes are produced, so a truncated or hostile frame keeps reading heap memory past the packet: garbage output, or an
  access violation when it reaches an unmapped page.
- The output side is safe: each match length is clamped to the remaining count (`if (iVar5 <= iVar6) iVar6 = iVar5`) and the output was
  reserved for exactly iVar5 bytes; window reads/writes are masked `& 0x3FF`.
- Callers and who controls the bytes:
  - CPlayerServer::Update 0x66D270 (`MsgStream_Decompress` at decompile line 56): every packet from every connected client, on the host.
  - CPlayerClient::Update 0x667A30: every packet from the host, on each client.
  - LobbyClient::OnServerInfoPacket 0x41D600: the master-server game list. The u32 checked there (`0x41D6DC cmp eax,edx / je`) is the TCP
    frame length, not the LZ raw length, so it does not bound LzDecompress.
  - MpFile::ReadContainer 0x579830: local .sav / mp files above version 0x64 (checksum tested first): only a crafted file.
- Not a design choice. A fix would pass the input end and a raw-length cap (the sender never compresses more than its send stream, a
  few KB) into LzDecompress.

## 9. CWorldClient::BuildShadowReceiverMesh 0x56C750: bridge owners clipped once per quad

CONFIRMED in code. Reachable whenever a unit shadow or a footprint decal touches a bridge / platform object (types 0x54 / 0x5D) that has
more than one walkable part. Effect cosmetic (over-dark shadow / print on those objects; wasted clipping work). UNSURE on how visible
it is: that depends on the blend state of the shadow pass (multiplicative or alpha blend shows the doubling).

- Loop: `0x56C87B mov eax,[esi+0x1F4]` (bridges.quadHead) ... `0x56C892 mov ecx,[eax]` (next) / `0x56C894 mov eax,[eax+8]` (quad
  owner id) / `0x56C8A3 call [edx+0xA0]` (find object by id). No "same owner as before" test anywhere in the loop: every quad re-runs
  SphereVsPrism and clips ALL parts of its owner (`puVar11[0x34]` part array, ShadowProjector::ClipTriangleB) into the same output.
- Quads come from CWorldClient::ActivateObjectBridges 0x5651A0 -> CBridgeManager::AddBoxFaces 0x560820: one AddQuad per box face of each
  part flagged 0x400, faces with normal z < 0.8 rejected, so about one quad per walkable part. An object with N walkable parts gets its
  whole geometry emitted N times.
- Callers: CMapObjectClientSpecific::RenderShadow (`0x51E08B`, withObjects = 1 when its local [ebp-0x18] is 0, onBridge 0) and
  CPrintDecal::Render (`0x6CF07E`, onBridge 1, withObjects 1, pushed at 0x6CEFFA / 0x6CF003).
- Live check: a unit standing on a multi-plank bridge (Gipat / Ingos bridges) with um-engine shadows off; compare the shadow darkness on
  the bridge vs on the terrain.

## 10. CMapObjectClientSpecific::RenderShadow 0x51CE60: 20 deg clamp tests abs(z)

CONFIRMED (an upward light sum passes through unclamped). Reachable only when nearby point lights BELOW the caster outweigh the sun
(night, a unit standing above a fire / torch light, e.g. on a ledge or bridge). Effect cosmetic: that unit's shadow most likely
disappears for those frames (UNSURE of the exact picture without a live look).

- Light sum: point lights (kind 0x31) add normalize(object - light) * weight, so a light below the object adds +z; the sun adds its
  travel direction (z < 0). Elevation test: `0x51D220..0x51D235` copies z and `and eax,0x7FFFFFFF` (abs), `0x51D23E fpatan(|z|, horiz)`,
  `0x51D254 fcompp` against 20 deg; only below 20 deg is z rewritten (`0x51D260 fmul [0x73DF80]` = -0.36397 * horiz, always downward).
  A sum with z > 0 and more than 20 deg of elevation keeps its upward z.
- What the sign changes: Shadow_BuildLightProjectionMatrix 0x4F1440 uses vx/vz and vy/vz (sign-free), and ShadowProjector::Build 0x4D8000
  uses L / |L.z| for the projection, so the silhouette is the same; but the prism cap `-((pos - L * 0.33 a) . L)` is sign-dependent:
  with L pointing up the kept half-space is ABOVE the caster, so the terrain under the unit is clipped out and no shadow is drawn
  (a bridge part above the unit could receive it instead).
- Intended was clearly "at least 20 deg below the horizon" (the rewrite always gives a downward z). Fix: test the signed z (z > -tan20 *
  horiz -> rewrite).
- Live check: night, a hero on a slope / bridge right above a campfire light; watch the blob shadow.

## 11. SSCScript::Run CopyStats 0x4B8A0B / CopyItems 0x4B8922: unchecked ScriptParty_Resolve

Partly REFUTED. The missing null check is real (AddUnitToParty / RemoveUnitFromParty check, these two do not), but the two cases
named in the claim do not produce null. Effect: none with vanilla scripts; a crash only after a script removed the default party.

- CParty::FindHeroById 0x6621B0 starts with `mov edx,[ecx+0x34]`, so a null party is an access violation at 0x34.
- ScriptParty_Resolve 0x4BAEC0 returns null only for a null player object (already excluded: both cases test FindPlayerById first)
  or when PartyStore::FindByName 0x4A5E10 returns 0:
  - Name without "::": the store name stays "" and is compared with the current store name g_0x7AA5D4; equal (the normal case) -> the
    player's own party +0x70; else FindByName("").
  - Unknown store name: FindByName falls back to the entry named g_0x79B298 (the shared "" literal) (`0x4A5E71 mov eax,ebx` returns that
    default). PartyStore::ResetToDefault 0x4A5BD0 always creates that ""-named party (single and multiplayer session start).
  - So 0 only when the store has no ""-named entry: after a script `RemoveParty(player, "")` (op 0x9B -> PartyStore::RemoveByName,
    `0x4B8719`), or a save whose store lacks it.
- Vanilla use (plain-text scripts in `drive_c/Evil Islands/maps`): bz13h `CopyStats(0, "HeroAlone::Hero", "Hero")` and back, bz7g
  `CopyStats` / `CopyItems(0,"Hero","JunParty::JunBoy")`, zone15 `CopyStats(0,"HeroAlone::Hero","Hero")`; all resolve (HeroAlone /
  JunParty are made by CreateParty). The real pitfall for modders is the opposite: a misspelt party silently resolves to the default
  party (copies onto / from the wrong hero, or FindHeroById fails and nothing happens).
- Doc fix wanted: gs-functions.md CopyItems / CopyStats rows and section 7 say "missing :: or unknown party crashes"; it does not.
  SpellAddon's `copystatscut` only changes what CopyStats copies, not this path.

## 12. Compiler_CompileText 0x4D23A0: Script / DeclareScript as the last token

CONFIRMED. Reachable only with a malformed script text (a mod's .mob script or world script that ends right after the keyword).
Effect crash (access violation at address 0 while loading the map) instead of the compiler error "Unexpected end of file".

- The top-level loop has already advanced the cursor `[esp+0x18]` (local_158) to the next token node when it classifies a keyword.
- WorldScript (0x74) tests it: `0x4D291E cmp [esp+0x18],ebx / jne` -> else error 1 "Unexpected end of file" (`0x4D2928 push 0x78D740`); the
  0x77 keyword path does the same.
- Script (0x75): `0x4D2B77 call GSTokenCList_GetNext 0x4D6580` with no test; GetNext does `mov` of `*cursor` -> null deref.
- DeclareScript (0x76): inline `0x4D2A88 mov eax,[esp+0x18]` / `0x4D2A8C mov ecx,[eax]` (eax = 0) -> access violation.
- Other truncations (e.g. `Script name` then EOF) go through Compiler_ExpectToken, which reports errors normally (not re-checked here).
- Vanilla scripts compile, so no effect in the shipped game. A modder-facing robustness bug.

## 13. CUIMessageLog::AddMessage 0x5E2A30: trim loop can erase the list sentinel

CONFIRMED, with a corrected condition: the sentinel is hit whenever ONE message wraps into more than 12 lines (whatever the number of
existing lines). Not reachable from the stock client (chat input is capped); reachable by a modified multiplayer client. Effect crash /
memory corruption (security, remote, lobby or in-game chat).

- Count of existing lines: loop at the top (local_80). Trim: `0x5E2E8A lea eax,[edx+edi-0xC]` (new + old - 12), loop `0x5E2E92..0x5E2EBA`
  always erases the current head `[ebp+0x148]` (`0x5E2EA1 mov edx,[edx]`) with UIMessageLines_Erase 0x5E3AD0. Erasing old + new - 12 nodes
  from a list of old nodes passes the end as soon as new > 12; the head is then the sentinel itself (+0x148 inside the CUIMessageLog):
  UIMessageLines_Erase unlinks it, runs UIMessageLine::Destroy on the widget's own fields and pushes the sentinel into the node free list
  g_0x79B22C, so a later node allocation hands out memory inside the UI object.
- Inputs: the only caller is CInterface::AddChatLine 0x5F82F0, only called by LobbyClient::OnChatLine 0x41E5D0 (replicated IdStringMessage
  chat lines and system lines). Local typing is capped by CUIMessageLog::OnChar 0x5E3550 (200 chars and 0x30C px = one line), and the
  system lmp_* lines are short, so the stock game stays at 1-2 lines per message. A client that sends a long chat string (more than ~12 x
  780 px of text, roughly 1300+ characters) makes every other player's game corrupt its heap (assuming the lobby server relays the
  string uncut; its chat relay was not re-read here).
- Not design. Fix: clamp the erase count to the existing count, and drop the excess new lines (keep the last 12).

## 14. CSoundManager::LoadSample 0x6B89F0: decoded-mp3 cache hit leaves the sample empty

CONFIRMED and reachable in the shipped game: decoded mp3s ARE put in the cache, so playing the same briefing / dialog voice line again
while its entry is still cached gives silence. Effect cosmetic (sound): a missing voice line.

- Hit path: `0x6B8A67 call StrHashMap_Find 0x6BC0C0`, `0x6B8A7F je 0x6B8C3E` (miss), then `0x6B8A85 mov ecx,[esp+0xA8]` (decompressMp3) /
  `0x6B8A8E jne 0x6B9043`: straight to the epilogue that sets name, loaded = 1 and a new id. No AIL_set_sample_file, no LRU move, no
  refcount ++, duration +0x2C not set.
- Miss path with decompressMp3: AIL_decompress_ASI, then the else branch appends a cache node at the TAIL (refcount 1) and the map entry
  (decompile "else { ... SampleCacheList_NewNode(.., cacheTail, 0) ... _AIL_set_sample_file_12 }"). So the next request of the same name
  is a hit. (The eviction loop is skipped for mp3 loads; mp3 nodes are evicted by a later WAV miss once 60 s old and unreferenced, or when
  the 8 MB budget is exceeded.)
- Who passes decompressMp3 = 1: only CSoundManager::Play2D 0x6B8250 forwards its 8th argument (`0x6B84DB mov eax,[ebp+0x24]` ->
  `0x6B84E7 call LoadSample`); Play3D (`0x6B8832 push 0`) and Update (`0x6B9D5A push 0`) pass 0. The one Play2D caller with 1 is
  DialogScreen::ParseDialogScript 0x604C50 (`0x60521D push 1 / push 1`: decode, category voice) for `%s\briefing\%s\%d.mp3`.
- Play2D calls AIL_init_sample on the channel before LoadSample (decompile line 126), so the hit leaves an empty HSAMPLE: AIL_start_sample
  plays nothing. UNSURE side effect: no data means no end-of-sample callback, so that channel may stay "loaded" until stolen by priority.
- Reach: hear a briefing / NPC dialog line, then trigger the same line again within about a minute (re-ask the same topic, reopen the
  dialog): the second time is silent. Live test: talk to a quest giver twice in a row and listen. Not design.

## 15. CSoundManager::Update 0x6B9B10, ambient one-shots: pan = rand % 101

CONFIRMED, reachable on every map with ambient one-shot lists (step 4 of Update, the random ambient sounds). Effect cosmetic: those
sounds are never on the left half of the stereo field (on the right half with the default stereo; mirrored when panSign = 0).

- `0x6BA09F mov edi,0x65 / 0x6BA0A6 div edi` -> remainder 0..100 pushed as the pan argument (`0x6BA0BA push edx`) of Play2D (`0x6BA0D1`);
  volume is `rand % 0x33 + 0x32` = 50..100 (`0x6BA0B5..0x6BA0C5`).
- Play2D's pan scale (decompile line 175) is `(pan + 100) * 127 / 200` over -100..100, after `pan = -pan` when CSoundManager +panSign == 0:
  0..100 maps to 63..127 = centre to full right. Every other Play2D user passes -100..100 (the CSoundManager channel code in Update step 1
  also clamps to 0..127 around the centre).
- Intended was surely `rand % 201 - 100`. Not design.

## 16. CSpellEffect::TickActive 0x682650, campfire 0x27: the countdown counts up, never ends

CONFIRMED as code, judged DESIGN (not a bug). Reachable on every map with a fire / torch object. Effect: none beyond the intended
permanent burn area.

- `case 0x27`: `countdown = old + 1`, every 4th tick (`(old & 3) == 0`) BuildHitRecord + ApplyHitTo on the live units within the radius;
  no path to `phase = 3`.
- Spell index 0x27 is "Campfire" (row 39 of the SpellPrototypes block in spells.sdb, offline parse). No item / scroll / wand in items.idb
  carries it. Its user is the map fire object: CTorchDesc (TORCH chunk 0xBBBF0000, kind 6) -> CTorchDesc::Spawn 0x46B310 ->
  CreateObjectFromDesc (fire object with a light and the campfire spell). Those fires are permanent scenery, so a burn area that lasts
  for the life of the zone (and blocks the passability map through UpdateVolumes 0x67EFC0) is what the design needs.
- Only a script / mod that casts Campfire as a normal timed spell would get a fire that never goes out. The tick counter overflows after
  2^31 ticks (years), harmless.

## 17. CSpellEffect::Start 0x67F870, charm 0x24: a failed check leaves phase 0

CONFIRMED. Not reachable in vanilla; reachable with SpellAddon `possessioncharmlink=1` (unlocks charm) or a script-cast charm with no
caster. Effect none visible (a dead cast record re-running Start every server tick for the rest of the zone: a tiny CPU / memory leak).

- `case 0x24`: target vt[0x30] != 0x50 (not a unit), or caster null, or caster not a unit -> `goto LAB_00681458` (UpdateVolumes and
  return). That label is AFTER the `switchD_..._caseD_16: phase = 3` line, so the phase stays 0. The normal paths do end: success ->
  start sound, `break` -> phase 3; resisted -> NotifyPlayer 0x1A -> phase 3.
- CSpellEffect::Update 0x67E6D0 calls Start for phase 0 on every tick, so the same checks run again forever (nothing ever sets a caster
  afterwards); the cast stays in the world cast list +0x12E2C and in the zone save.
- Vanilla: charm is in no shop / monster list and CastSpellMulti refuses it (spells.md). SpellAddon `possessioncharmlink=1` and its
  `newspells` "Ensorcell" (charm, ID 42) make it castable; with a normal caster and unit target the bad branch still needs the caster to
  be gone before the next tick (caster removed on the cast tick) or a non-unit target.
- Not design (other kinds jump to the phase-3 line on failure). Fix: point the failure jump at `phase = 3`.

## 18. CTerrain::UpdateAmbientSounds 0x6CB390 (0x6CB804): loop pan limit "int read as float"

REFUTED. The limit is stored as a float and read as a float; the loops are panned normally (limited to +-30 or +-100). Effect none.

- `0x6CB801 mov eax,[ecx*8+0x7969E0]` / `0x6CB808 mov [ebp-0xC],eax` / `0x6CB80B fld dword [ebp-0xC]` / `fchs` -> clamp of the pan value
  `[ebp-0x2C]` to [-limit, +limit] (`0x6CB81D..0x6CB835`).
- The table 0x7969D0 (16 records of 0x18: name, ?, weight float +8, redirect +0xC, limit +0x10, one-shot chance +0x14) holds at +0x10 the
  dwords 0x41F00000 (= 30.0f) or 0x42C80000 (= 100.0f) in the exe image: real floats. A raw-byte search for the addresses 0x7969D0 /
  0x7969D4 / 0x7969DC / 0x7969E0 finds only reads (0x6C7300 count loop, 0x6CB57B, 0x6CB804, 0x6CB811, 0x6CBB4D); nothing rewrites +0x10.
- The "always 0" note in functions.tsv / the terrain page should be removed.

## 19. Terrain ambient one-shots: file count from the `day` folder only

CONFIRMED with the game's sfx.res. Reachable on Gipat grass / field / dry-grass maps in the morning and at night, and on Ingos ice / snow
maps at night. Effect cosmetic (sound): some random ambient one-shots are silent (missing file) and some night files never play.

- Count, in CTerrain::Load 0x6C68B0: for each table record with redirect < 0, `0x6C730D mov [esi],0`, then loop `0x6C7313..0x6C734E`:
  sprintf("ambient\%s\%s\day\%i.wav", world, type, count + 1) (`0x6C731A inc edx`, format 0x796BD0) and CSoundManager 0x6B8960 (file
  exists in a sound archive) -> count++. Only `day` is counted, from 1 until the first gap.
- Use, in UpdateAmbientSounds: `0x6CB97F call Terrain_DayTimeName 0x6CBC00` (day 6-18 h, morning 2-6, evening 18-22, else night),
  `0x6CB98C div [ebp-0x2C]` (the day count), `0x6CB994 inc edx`, sprintf("ambient\%s\%s\%s\%i.wav") at `0x6CB9B3` -> Play3D.
- Data (sfx.res of `drive_c/Evil Islands/res`, 558 ambient files; the mod ships none):
  - Ingos Ice, Snow, Snowball: day 1-15, night only 1-3 -> 12 of 15 night picks name a missing file (80 % of night one-shots silent).
  - Gipat GRASS, FIELD, DRYGRASS: day 1-16; morning has 1-11, 13, 14 -> picks 12, 15, 16 missing (3 of 16); night has 1, 3-20 -> pick 2
    missing and 17-20 never used.
  - Every other world / type / time folder matches its day folder.
- A missing file just fails in LoadSample (Play3D returns -1), no crash. Fix options: count each daytime folder separately (exe), or
  fill the gaps with copies in a mod's sfx (data-only fix, the user's DDS/resource rules apply).

## 20. CTexture::LoadFromBmp 0x68FBF0: width / height loop counts swapped; byteSize never set

CONFIRMED, both parts. Reachable only with a loose `<root>\textures\<name>.bmp` file (CTextureManager::LoadTexture 0x6922A0 prefers it
over the .mmp in the archives); the game's `textures` folder is empty, so vanilla never takes this path. Effect for a modder's non-square
BMP: garbled texture, and when width > height a write past the end of the staging surface (crash / memory corruption).

- Size: `0x68FD2D call Bitmap_GetSize`, `0x68FD32 mov ebx,[eax]` (width), `0x68FD3D mov esi,[eax+4]` (height), saved as
  `[esp+0x28]` = width / `[esp+0x2C]` = height (`0x68FD45 / 0x68FD49`, 3 pushes earlier). Surfaces are created right: dwHeight = esi
  (`0x68FDB1`), dwWidth = ebx (`0x68FDD4`), this->width +0x20 = ebx (`0x68FDDE`), height +0x24 = esi (`0x68FDAC`).
- Fill loop: OUTER count = `[esp+0x28]` = width (`0x690010 / 0x69003C`), each outer step adds the pitch (`0x6900C5 add ecx,edx`, edx =
  the locked pitch) = one ROW; INNER count = `[esp+0x2C]` = height (`0x690040 / 0x69004A`) pixels of 2 bytes. So it writes `width` rows
  of `height` pixels into a surface of `height` rows of `width` pixels. Square textures (all EI textures are powers of two, mostly square)
  are unaffected; W > H overruns the locked memory by (W - H) rows.
- Also: the source is read as one continuous B G R stream (`[esi+edi]`, esi += 3 per pixel), ignoring the 4-byte BMP row padding, so
  widths with W*3 not a multiple of 4 shear as well.
- byteSize +0x28 (summed by CTextureManager into bytesInUse for the purge budget) is never written in 0x68FBF0..0x690260 (no
  `[ebp+0x28]` store), so BMP textures count as 0 bytes (or whatever the slot held) in the budget.
- Not design. Relevant only if the multitool / a mod ever relies on loose BMP textures (Universal-Mod uses .mmp / DDS, so no).

## 21. CItemLoot::GetIconName 0x6A4B90: blueprint with an out-of-range weapon / armour row

CONFIRMED. Not reachable with vanilla data; reachable with modded data / old saves. Effect crash (read of address 0xC whenever the item's
icon is drawn: inventory, shop, trade, camp).

- Category 2, table 0: `refIndex < (end - begin) / 0xB4` ? row : 0, then `switch (*(row + 0xC))` (decompile lines 79-89); table 1 the
  same with stride 200 (lines 122-132). No null test in between. (GetPrice 0x6A5470 returns 0 for a bad row; GetInitIconName 0x6A5030
  has the same bounded lookup as GetIconName and was not re-checked.)
- Where refIndex +0x54 comes from: CItemLoot::InitFromProto 0x6A4AB0 (proto +0x74; the generic rows "instruction weapon" / "instruction
  armor" in items.idb have -3 there), the item-name constructor ("instruction weapon.<name>": unknown name -> -1, items.md 4.x),
  CCampScreen::DeconstructItem (valid row), shop generation (valid rows), CItemLoot::Read 0x6A5630 (saves, party files, the network).
- Vanilla: no quest give list (quests.qdb), map script or shop makes a blueprint with a bad row (no "instruction ..." name in any
  shipped script or briefing). Bad rows appear when: a mod script / briefing gives "instruction weapon" with a wrong or missing second
  part; a save or .mp party made with a bigger weapons / armours database is loaded with a smaller one (rows are stored as indexes, so
  any Universal-Mod database change that removes or reorders rows can turn old blueprints into crash items); a hostile multiplayer
  peer sends one.
- Concrete case: Universal-Mod's items.idb (in `Universal-Mod/res/databaselmp.res`) has 69 weapon rows against 37 in vanilla
  (armours 193 vs 192). A UM save holding a blueprint for weapon row >= 37, loaded without the mod, crashes as soon as the item is
  shown.
- Not design. A guard (return "" for a null row) in GetIconName (and GetInitIconName if it has the same gap) would make it harmless.

## 22. ShopStock::GenerateItems 0x69AE20: weapon / armour proto with material type "none"

CONFIRMED in code; not reachable with vanilla, Universal-Mod or LostInAstral data. Effect with such data: crash when a shop restocks.

- Material type test: `_stricoll(proto +0x10, "none")` (decompile line 133); equal -> `CItemConstructor::SetMaterial(0)` (LAB_0069b283),
  then `param_3 = CItemConstructor::FinishItem` (line 277) and at once `*(param_3 + 4) += 1` (line 279) with no null test. FinishItem
  0x6A5CF0 releases the item and returns 0 for a weapon / armour without a material, so that line writes to address 4.
- Data (offline, field 3 = M.Type of every record): vanilla items.idb (database.res and databaselmp.res): weapons Stone / Metal /
  Crystal / Bone, armours Leather / Fur / Cloth / Hide / Metal / Crystal; Universal-Mod databaselmp.res items.idb: the same type names
  (69 weapons, 193 armours). Only the quick items use "None" (16 potions / sensor / spell container), and quick items need no material,
  so FinishItem succeeds for them.
- A modder who adds a weapon or armour row with M.Type `none` to a shop list (S_Weapons / S_Armors) gets a crash on the shop's next
  generation. Worth a check in the multitool's database editor. Not design.

## 23. Node3ds_StepFrame 0x49F890: own +0xFC record on the key frame, the parent's on other frames

CONFIRMED in code, and the whole path is dead: effect none.

- Key-frame branch (a new rotation key starts at this frame, decompile "if ((iVar13 < count - 1) && (key.frame <= frame))"): the axis
  is rotated by the quaternion `*(this + 0xFC) + frame * 0x28 + 0x18..0x24` (code at 0x49FA3B per functions.tsv); the other branch uses
  `*(param_1 + 0xFC) + ...` (the parent node, 0x49FB7C). Same formula (q v q* expanded with Vec3_Cross / Vec3_Scale2), different
  node. Almost certainly a typo for the parent in the first branch.
- Reachability: the only caller is AnimNodes_ComposeWorldTransforms 0x448720 (`0x44875A`); that function's only call site is itself
  (`0x44899A`, inside its own 673 bytes: recursion over the child nodes). A raw-byte search finds no pointer to 0x448720 or 0x49F890
  anywhere in the image (no vtable / table use). So this 3DS keyframe stepping never runs in the shipped game.
- Nothing to fix; only matters if someone revives the 3DS animation code.

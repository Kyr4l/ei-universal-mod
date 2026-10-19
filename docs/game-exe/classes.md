# game.exe classes (draft from the full reverse, phase 1; source of truth: _cpr/claude-re/full/functions.tsv + classes.tsv)

Generated 2026-10-17 from `_cpr/claude-re/full/hierarchy.md`. Addresses are for the Russian game.exe (EIStarter OBT-1).


Conventions: `Class::vN` = slot N of the class's vtable not yet understood. Root base class names in quotes are guesses
(no class-name stub: the roots are abstract); the stub names (CUnit, CTree, ...) are the real ones. Offsets are `this+off`.

## "CObject" — the root of every game class (13 virtual slots; 150+ vtables)
Fields: +0 vtable, +4 refcount, +8 flags (bit 16 / bit 17 = per-class booleans, see slots 21 / 29 of CMapObject), +0x10 typeId
(the value slot 12 returns, stored at construction), +0x14 owning manager (CManagerBase*), +0x18 ?. Size 0x1c.

| slot | name | what it does (default) |
|---|---|---|
| 0 | ~dtor(flags) | deleting destructor: body, then `operator delete` if flags&1 |
| 1 | CopyBase(other) | copies +8, +0x10, +0x18 (0x402D40) |
| 2 | Init() -> bool | per class |
| 3 | Done() | per class |
| 4 | Release() | --refcount; at 0: manager(+0x14)->vt[24](this) (0x455980) |
| 5 | v5 | empty default (0x402CC0) |
| 6 | v6 | returns 0 (0x402CD0) |
| 7 | GetName() | "noname" (0x402CE0) |
| 8 | GetDesc() | "nodesc" (0x402CF0) |
| 9 | Read(stream) | default empty; stream: vt[1](ptr,size) read, +8 version |
| 10 | Write(stream) | default empty; stream: vt[2](ptr,size) write |
| 11 | ClassName() | the class-name stub |
| 12 | GetTypeId() | small constant (CCamera 0x43, CTree 0x52, CSpell 0x4f, CMapObjectServer? 0x47 = the "specific" type CMapObject expects, 0x39 = a figure-holding specific) |

## "CObject3D" — CObject + transform (slots 13..17 shared by CCamera, CParticle, CLight, the CEffect*, CTree.., CUnit..)
Fields: +0x1c local pos (3f), +0x28 world pos (3f), +0x34 local rot quat (4f), +0x44 world rot quat (4f) -> size 0x54.
| 13 | CopyTransform(other) | 0x401490 |
| 14 | UpdateWorldRot(parentQuat) | 0x40CB80 |
| 15 | UpdateWorldTransform(parent) | 0x40CC30: slot 14 then world pos |
| 16 | WriteTransform(buf) | buf->vt[14] (0x401510) |
| 17 | ReadTransform(buf) | buf->vt[13] (0x401540) |

## "CMapObject" — the placed world objects (CTree, CBuilding, CStone, CStuffObject, CTrapObject, CMagicTrapObject(43 slots), CLeverObject, CTorchObject: 61 slots; CUnit 62; CFakeUnit 63)
Fields so far: +0xa4 name (char*), +0xc4 offset (3f, reset from +0x1ac), +0xd0 array (ptr, count +0xd4, cap +0xd8, step +0xdc;
32-byte aligned), +0x100 RECT, +0x128 offset (3f, slot 48), +0x170 "specific" (CMapObjectServer / ClientSpecific, typeId 0x47:
its +0x1c = owner back pointer, +4 refcount), +0x174 = 5, +0x178 = 1.0f, +0x17c struct (0x5233B0 init / 0x522C50 copy),
+0x194 (0x50FE70 init), +0x198/+0x19c array of 0x14-byte items (0x688550 read, 0x6885A0 write), +0x1a8 string, +0x1ac 3f, +0x1c0 = -1, +0x1c4 (0x50FA30/0x47FB70).
Slots 18..61: see functions.tsv (CMapObject::*). Known: 19 SetSpecific, 20 Assign, 21/29 flag bits, 30 ResetOffset, 48 PlaceOnTerrain,
51 SnapZ, 52/53 GetLocalPos, 55 -> specific vt[17]. The world object: 0x455950(this) -> its vt[27](x,y) = terrain height.

## The root fields, settled
+4 refcount, +8 flags, +0xc id (set by the manager, -1 before), +0x10 **kind** = the creation key passed to
CBaseManager::RegisterInstant(kind, id) (NOT GetTypeId: the server and client factories map the same kind to different
classes), +0x14 manager, +0x18 (read from the save stream by CBaseManager::Read). Objects attached to another one keep the
owner at +0x1c (the "specific" objects: CSpecificBase, vtable 0x73DC54).

## Kinds (the factory keys; obj+0x10)
| kind | CServerSpecificManager (0x4D6970) | CClientSpecificManager (0x42D700) | CMapObjectManager (0x512730) |
|---|---|---|---|
| 0x32-0x34 | | | CUnit (0x4dc bytes) |
| 0x35 | | | CTree (0x238) |
| 0x36 / 0x37 / 0x38 | | | ctors 0x5297C0 / 0x5298A0 / 0x529980 (0x238 each: CBuilding / CStone / CStuffObject, see ctor.py) |
| 0x39 | | | CFakeUnit (0x580) |
| 0x3a / 0x3b / 0x3c | | | ctors 0x52BEE0 (0x23c) / 0x529DD0 (0x290) / 0x52B1F0 (0x264) |
| 0x300d / 0x3010 | | | ctors 0x50C5F0 (0x78) / 0x529A80 (0x23c) |
| 0x45 | | CFigureClientSpecific (0x3c) | |
| 0x46 | CObject3DServer (0x20) | CObject3DClientSpecific (0xfc) | -> attached at CMapObject +0x94 |
| 0x47 | CMapObjectServer (0x94) | CMapObjectClientSpecific (0x58) | -> attached at CMapObject +0x170 ("specific") |
| 0x48 | CUnitServer (0xb20) | CUnitClientSpecific (0xdc) | -> attached at CUnit +0x240 |
| 0x49 | CPlayerServer (0x5128) | CPlayerClient (0x50f8) | |
| 0x4b | CTorchObjectSpecific (0x58) | CTorchObjectClientSpecific (0x64) | |

## Managers — "CBaseManager" (vtable 0x73B870, 25 slots; the name comes from the string "CBaseManager::RegisterInstant")
Fields: +0x1c id->object map, +0x30 max id, +0x38/+0x3c list head/tail (+0x40 count, +0x44 free nodes, +0x48 pool),
+0x50 id->node hash, +0x68 kind->pooled objects, +0x80 kind->pool count, +0x98/+0x9c id range (0, 2000000000), +0xa8 last id,
+0xac the world (set by slot 13 SetWorld). Slots: 2 Init, 3 Done, 9 Read, 10 Write, 13 SetWorld(world), 14 CreateByKind(kind)
(pure virtual in the base), 15 DoneObjects, 23 RegisterInstant(kind, id or -1 = random), 24 DestroyObject(obj) (from
CObject::Release). 16-22: defaults returning 0 / empty (per-manager hooks).

## CUnitServer (kind 0x48 server; vtable 0x73EF38, 54 slots; 0xb20 bytes) — +0x1c = the owning CUnit
+0x3c move state (slot 16 returns it; +0 mode, 2 = moving), +0x1e0 stats block (0x523xxx / 0x524xxx / 0x525xxx helpers,
+0x1e4 experience -> slot 24 GetLevel), +0x8e4 attack (short), +0x980 (0x5D5080), +0xa24 (0x5CEED0), +0xaf8 (0x5CCBF0).
CUnit fields seen from here: +0x24c player (0 = monster), +0x26c equipped weapon id, +0x330 struct (net section 4),
+0x340, +0x480..+0x488 / +0x48c..+0x498 (slots 37 / 36 check them), +0x1dc net dirty mask (MarkNetDirty), +0x1c0 last change time.

## CWorld (vtable 0x73F18C, 42 slots) -> CWorldClient (0x73F41C, 43) / CWorldServer (0x73F928, 42); CParty (0x743000) implements the same container slots 13..26
Managers (CWorld::CreateManagers 0x55EE40): +0x38 CPlayerManager, +0x3c CFigureManager, +0x40 CMapObjectManager, +0x44 the specific manager (CClientSpecificManager / CServerSpecificManager; NOT the terrain),
+0x48 CEffectManager, +0x4c CSpellManager, +0x50 CSpellConstructor? (0x683FF0), +0x54 CItemManager, +0x58 CItemConstructor? (0x6A5AB0),
+0x5c handle (vtable 0x73F240). +0x1c time base, +0x20 time scale/15, +0x64 world time (CMapObject reads world+0x68 as "now"),
+0x170 passability map (server; 0x5BA830 / 0x5BA9D0 / 0x5BBB20), +0x180 terrain object (client), +0x280c height map (server),
+0x12c64 aggro table, +0x12d74 id hash (server FindObjectById), +0x12e44 timed item list (server).
Slots: 13 GetWorld(self), 14 GetEffectManager, 15 GetFigureManager, 16 GetItemConstructor?, 17 GetSpellConstructor?, 18 GetItemManager,
19 GetMapObjectManager, 20 GetPlayerManager, 21 GetSpecificManager, 22 GetSpellManager, 23 Get34, 24 CreateItem(desc), 25 Get224 / 0,
26 IsServer, 27 GetHeight (server) / ClampToTerrain (client), 28 Passability (server) / GetHeight (client), 29/30 wrappers,
31 SetTime, 32 SetTimeScale, 33..38 client getters +0x188..+0x19c, 40 FindObjectById, 41/42 spatial queries (client).

## The "specific" objects (CSpecificBase 0x73DC54, 20 slots): +0x1c = owner object, created by kind through the server / client specific manager
- CObject3DServer (0x20 bytes) / CObject3DClientSpecific (0xfc): kind 0x46, at CMapObject +0x94. Client = the renderable: parts lists +0x20/+0x24/+0x28,
  scale +0xf0..+0xf8 (slot 18), slots 13..15 attach/detach parts, 19 (435 lines), 20 Render, 21 RebuildMaterials, 22..24 hit tests, FirstPart/NextPart iterators.
- CMapObjectServer (0x94) / CMapObjectClientSpecific (0x58): kind 0x47, at CMapObject +0x170. Server +0x20 = placement (floats: +8 HP, +0xc max HP,
  ...; packed to int16 at CMapObject +0x17c: +2 HP, +4 max HP); slots 17 IsDead, 18 IsDestroyed, 20 TakeHit (server).
- CUnitServer (0xb20) / CUnitClientSpecific (0xdc): kind 0x48, at CUnit +0x240 (see above); CUnitSpecific base 0x73EDBC.
- CPlayerServer (0x5128) / CPlayerClient (0x50f8): kind 0x49; +0x48 (server) = the connection (vt[2] = write bytes: messages 0x17, 0x0a, 0x14, 1);
  +0x20 state (1 in game, 3 leaving). CPlayer (13 slots, typeId 0xf) is the owner; CPlayerClient slot 14 OnUnitAdded.

## CFigure (vtable 0x73C1C4, 24 slots; typeId 0x44; three vtables share the stub: 0x73C1C4 full, 0x73C224 (20), 0x73FBF0 (22)); CI3DFigure (0x73FC48, 24, typeId 0x46) shares 15..23
+0x120 name, +0x158/+0x15c header counts, +0x1c0[]/+0x1d0[] rotation tables (0x40-byte entries). Slots: 7 GetName, 15 UpdateWorldTransform,
16 collect parts, 17 read parts, 18 CopyFrom, 19 Load(data) ("FIG" magic), 20 ParseFIG, 21 BuildMorphed(data, s, d, h), 22/23 negate rotations.

## Items — "CItem" base (vtable 0x743DA4, 22 slots; CItemWeapon 0x5c bytes, CItemArmor 0x8c, CItemQuick, CItemQuest, CItemLoot, CItemSpell; CInventory shares the base)
+0x18 proto ids (lo = material index into g_0x7C86D8 (0x50-byte entries), hi = weapon index into g_0x7C86F8 (0xb4-byte entries)),
+0x1c refcounted holder, +0x20/+0x24 count, +0x28/+0x2c durability cur/max, +0x30 regen, +0x34, +0x38 weight, +0x3c, +0x40, +0x44 object,
+0x48; weapon +0x4c..+0x58 ranges. Slots: 6 Update (regen), 9/10 Read/Write (save), 13 InitFromProto, 14 Equals, 15 FillInfo,
16 GetWeight, 17..20 text getters (name, description, ...), 21 Zero3. Created by CItemManager::CreateByKind (kinds 0x3004.. ).

## Effects — CObject3D + slots 18..23 for the 24-slot ones (CEffectSound, SoundSeq, AmbientSound, ArrowFigure, ShowVisibility, Moshka, Tornado):
18 Reset, 19 GetParam (float), 20 GetPos (this+0x1c), 21 ReadNet(stream, reader) (DeltaReader), 22 WriteNet(writer), 23 Play(flag).
CEffectPointLight / Particle / Wall / Lightning / Arrow have only the 18 CObject3D slots.

## Managers: CBaseManager (25) -> CObjectManager (proto list +0xb0..+0xb4, slots 16..22: FindProtoByName, .., ClearProtos; CMapObjectManager, CItemManager 26 slots)
Every manager's slot 14 = CreateByKind (the factory), slot 25 (26-slot managers) = a name getter (see functions.tsv).

## The UI framework (unnamed vtables 0x740000..0x742FFF; no class-name stubs: ClassName returns "Sorry, no other way!", typeId 0x45)
CObject3D-derived widgets with a secondary 13-slot event interface at this+0x94 (IUIEvents, vtable 0x7401D0 base; each class has
its own copy: slots 1..6 / 9..12 empty defaults, 7 / 8 forward to its slots 2 / 5). Primary vtables:
- CUIWidget (0x740F60, 25 slots; ctor 0x5FE700 41 callers): +0x98 parent, +0x34..+0x40 colour, +0xec/+0xf0 materials (kind 0x16),
  +0x108[] figures (+0x10c), +0x118[] element callbacks (+0x11c), +0x128[] buttons (+0x12c), +0x138[] sprites/texts (+0x13c).
  Slots 19 SetVisible, 20 Draw(renderer), 21 UpdateHover, 22 UpdateElements, 23 UpdateHoverAndButtons.
- CUIWindow (0x741008, 42 slots; ctor 0x600B20 31 callers; base CUIWindowBase? 0x73F7F8): + children windows +0x148[] (+0x14c);
  slots 19..24 recurse into the children, 25/26 pure virtual, 27..41 empty event handlers.
- 55-slot screens (0x74091C, 0x740A2C, 0x740D18, 0x74214C) and 26/27-slot widgets: the actual menus (strings: MainMenu, campinfo,
  saveload, globalmap, battle00, infoitem, option_difficulty, quit_msg, lmp_status_*) — read per module in phase 3.

## Other hierarchies (not CObject-derived; phase 3 per module)
- Net: NetConnection (0x73BF5C, 15 slots; seed names from net/README.md), NetServer 0x73BB70/0x73BBB4 (17), NetClient 0x73BDEC/0x73BE30 (17).
- Renderer: I3DFigure? (0x73D8A0, 25 slots, abstract) / C3DFigureImpl? (0x73D93C, 26: Load 2566 lines, Render 3211/2524; CI3DFigure::SetTexture at 11).
- Resource readers: IResReader? (0x73C8A4, 13, abstract) and the 13/14-slot classes at 0x73C8F8..0x73CA10 (ctors 0x4782E0..0x47CD40).
- Small 13-slot helper classes 0x73D128..0x73D294 (ctors 0x4BA1C0..0x4BA950), 0x73F0C4/0x73F0F8 (0x551AE0), 0x73FF40/0x73FF8C (AI/orders 0x5D0F60..).

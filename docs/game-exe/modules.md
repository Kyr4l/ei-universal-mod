# game.exe modules: what each address range contains

Generated from `functions.tsv` / `modules.tsv`. For every module: the theme and the 14 largest functions with their one-line purpose (generic suffixes only make names unique). The full list per range is in functions.tsv.

## 0x401000 - 0x402000: math: vec3/quat helpers (CObject3D transform)

25 functions, group `leaf`.

| addr | size | name | purpose |
|---|---|---|---|
| 0x00401F00 | 766 | CObject3D::AlignToDirectionSmooth | (dir, slow): same axes and projection as AlignToDirection (own -Y toward dir about own Z); angle > pi branch (2pi - angle, axis negated) cannot trigger after acos; slow != 0 scales the angle by 0.4 (gradual turn); then RotateAxisA... |
| 0x00401B50 | 643 | CObject3D::AlignToDirection | (dir float[3]): from localRot +0x34 builds the object's -Y axis (column 1) and Z axis (column 2), projects dir onto the plane normal to Z (Z x dir x Z, normalised), axis = (-Y) x proj; if /axis/ >= 1e-5 angle = acos(clamp(dot(-Y, ... |
| 0x00401680 | 535 | CObject3D::LookAtPlanar | rotation delta between local axis and target vector (xy), via Quat_FromAngleAxis, applied with Quat_MulAssign |
| 0x00401940 | 527 | CObject3D::RotateTowardDirection_B | rotation of the object's quaternion (+0x34) that turns the other local axis onto a target direction (sibling of 0x401680) |
| 0x00401170 | 414 | NetPlayerInfo::Read | GameBuffer counterpart of 0x401000, reads the same fields in the same order |
| 0x00401000 | 367 | NetPlayerInfo::Write | serialize into GameBuffer: dwords +0x1c,+0x20, two CStrings (+0,+0xc), bytes +0x18,+0x19,+0x1b,+0x1a |
| 0x00401570 | 258 | CObject3D::BuildWorldMatrix | (float[16] out): identity, then rotation part from worldRot +0x44 (w,x,y,z) in the D3D row-vector layout, translation row out[12..14] = worldPos +0x28; CORRECTION: was 'rotation matrix only' (it also writes the translation) |
| 0x00401E50 | 176 | CObject3D::RotateAxisAngle | (axis, angle): q = Quat_FromAngleAxis(angle, axis); localRot +0x34 = q * localRot (q on the left); CORRECTION: was 'post-multiply' |
| 0x004018A0 | 148 | Quat_PreMulAssign | (this quat, q): this = q * this (Hamilton, w first); checked term z = q1*a2 - q2*a1 + .. |
| 0x00401490 | 115 | CObject3D::CopyTransform | CObject3D slot 13: copies +0x1c..+0x50 (local pos 3f @0x1c, world pos 3f @0x28, local rot quat 4f @0x34, world rot quat 4f @0x44) from the other object |
| 0x00401340 | 97 | GameBuffer::ReadBytes | (dst, n): if cursor +8 + n > +0x10 -> GameBuffer::Read(dst, n); else if it fits under +0xc copies inline and advances +8; else vt[3](dst, n) refill path |
| 0x00401310 | 46 | GameBuffer::ReadInline | (dst, n): copies n bytes from the cursor +8 (dwords then tail bytes), +8 += n; no bounds check |
| 0x00401460 | 41 | Quat_StaticInit_Identity | static init: global quaternion 0x79B1C0 = (1,0,0,0) (w first) |
| 0x00401510 | 39 | CObject3D::WriteTransform | CObject3D slot 16: stream->vt[14](ptr,size) writes local pos (+0x1c, 12 bytes) and local rot (+0x34, 16 bytes) |

## 0x402000 - 0x405000: CObject root + CBaseManager (id map, pools, RegisterInstant)

73 functions, group `core`.

| addr | size | name | purpose |
|---|---|---|---|
| 0x00402780 | 887 | RadixSortU32 | fastcall (uint *data, n): 4-pass LSD radix sort (bytes 0..3, ping-pong data <-> scratch g_0x79B1E0, result back in data); scratch grown like 0x402310 (AlignedAlloc32_406B50, guard 0x79B1F0, atexit 0x402B00); no caller or pointer; ... |
| 0x004031E0 | 761 | CBaseManager::Done | slot 3: walks idMap +0x1c three times: (1) refcount +4 += 1 on every object, (2) records each id +0xc in a temp std::list and calls obj vt[3] (Done), (3) for each recorded id still in idMap calls vt[4] (Release); frees the temp li... |
| 0x00403730 | 636 | CBaseManager::RegisterInstant | slot 23 (kind, id): poolCountByKind +0x80[kind] > 0 and a pooled list in poolsByKind +0x68 -> PopPooled reuses an object, else vt[14](kind) creates one (0 -> returns 0); obj flags +8 = 0, kind +0x10 = kind; id != -1: appends a lis... |
| 0x00402500 | 599 | RadixSortU16 | fastcall (ushort *data, n): 2-pass LSD radix sort (low byte into scratch g_0x79B1F8, high byte back into data); scratch grown like 0x402310 (AlignedAlloc32_406B30, guard 0x79B208, atexit 0x402760); no caller or pointer; CORRECTION... |
| 0x004034E0 | 568 | CBaseManager::DoneObjects | slot 15: three passes over idMap +0x1c: refcount +4 += 1 on every object, then vt[3] (Done) on each, then vt[4] (Release) on each; idMap maxId +0x30 = 0 and its entries freed; object list +0x38 emptied (CPlex chain freed); nodeMap... |
| 0x00402DF0 | 539 | CBaseManager::DestroyBody | resets vt 0x73B870, calls Done (0x4031E0), frees the nodes and bucket vectors of poolCountByKind +0x80, poolsByKind +0x68, nodeMap +0x50, empties the object list +0x34 (vt reset to 0x73B8D4, head/tail/free/count 0, CPlex chain +0x... |
| 0x00404040 | 529 | CBaseManager::PoolObject | (obj): if idMap +0x1c[obj id +0xc] is obj, removes it from idMap, nodeMap +0x50 and the object list (CList12_RemoveNode); then appends obj to the kind's CList in poolsByKind +0x68 (creates the list when missing: operator new 0x1c,... |
| 0x00404410 | 490 | CBaseManager::FreeTypeGroups | iterates the std::map at +0x68, for each group deletes all objects of its list (vt[0](1)), clears it and frees; then empties hash table +0x6c |
| 0x00402310 | 452 | RadixSortU8 | fastcall (byte *data, n): in-place counting sort of n bytes: grows the 32-byte-aligned scratch g_0x79B210 (cap +0x79B218, growth = last n/8 clamped 4..0x400; once-guard 0x79B220 + atexit 0x4024E0), counts 256 buckets, scatters int... |
| 0x00404A70 | 426 | List16_Ctor | (this list, alloc byte): allocates the sentinel from the 0x10-byte pool g_0x79B228 (refill of 20 entries inlined: chunk g_0x79B268..g_0x79B26C, heap total g_0x79B264, then stlport_node_alloc_chunk_alloc), sentinel next = prev = it... |
| 0x00402B20 | 416 | CBaseManager::CBaseManager | CBaseManager constructor body (the managers' ctors call it before storing their vtable) |
| 0x00403020 | 390 | CBaseManager::Init | slot 2: idMin +0x98 = 0, idMax +0x9c = 2000000000; idMap +0x1c cleared and resized to 0x400 buckets; nodeMap +0x50 nodes freed, 0x400 buckets; poolsByKind +0x68 nodes freed (PoolFree_N12_407DA0, the pooled lists themselves are not... |
| 0x00403BD0 | 349 | CBaseManager::RegisterWithNewId | (obj): id = idMap maxId +0x30 + 1 when in [idMin, idMax), else up to 100000 random tries for an unused id (FatalError 'CBaseManager::RegisterInstant..' on failure); new id -> list entry appended +0x38 and nodeMap +0x50 entry (+0x6... |
| 0x00403E10 | 296 | dead_403E10 | unregistered code with no call, jump or pointer anywhere in the image (gaps.py 2026-11-06) |

## 0x405000 - 0x40C000: STLport containers / node allocator (no strings)

194 functions, group `leaf`.

| addr | size | name | purpose |
|---|---|---|---|
| 0x00409AE0 | 928 | Archive_Open | (archive, file stream, loading, version): frees the old section nodes (PoolFree_N24), clears the object stream +0 and the data stream +0x28 (vt+0x18), pushes a root section, +0x58 = loading; storing: +0x54 = version, done. Loading... |
| 0x0040A000 | 919 | Archive_Close | (archive, file stream): storing: cuts the data stream +0x28 to the root, writes it to the file as chunk tag 1 (tag byte, length*2 in 1 or 4 bytes /1, bytes), clears it, resets the root; then for every object queued in list +0x70 (... |
| 0x00409640 | 651 | Archive_SerializeStream | (archive, tag, CMemStream*): opens a section with that tag (storing: pushed after the parent's data, offset = parent offset + size + 5; loading: the n-th tag entry, nothing done when absent); inside it tag 1 = the stream length (4... |
| 0x0040AF00 | 580 | DwordVector_Reserve_40AF00 | STLport vector<4-byte>::reserve(n): nothing when capacity >= n; else n dwords from node_alloc (inlined refill / chunk_alloc) or operator new, old contents copied (DwordVector_AllocFill_40B9F0 path when non-empty), old buffer freed... |
| 0x00405EB0 | 560 | HashTable_InitBuckets_405EB0 | STLport hashtable::_M_initialize_buckets(n): n -> first prime >= n of __stl_prime_list 0x73B914 (inline lower_bound, 0xFFFFFFFB past the end), bucket vector +4..+0xC reserve (node_alloc / PoolAlloc_Refill / operator new, old buffe... |
| 0x00406300 | 560 | HashTable_InitBuckets_406300 | STLport hashtable::_M_initialize_buckets(n), same code as 0x405EB0 (other callee copies); CBaseManager::CBaseManager. CORRECTION: was "rehash" |
| 0x0040AA80 | 560 | HashTable_InitBuckets_40AA80 | STLport hashtable::_M_initialize_buckets(n), same code as 0x405EB0; FactoryMap_Ctor_408EA0 (n = 100). CORRECTION: was "Rehash2" |
| 0x00407740 | 515 | HashTable_ConstructBuckets_407740 | STLport vector<void*>(n, alloc) storage ctor: n dwords from node_alloc (free list 0x79B224, inlined _S_refill / chunk_alloc) or operator new when > 0x80 bytes; begin = finish = buffer, end_of_storage = buffer + n; the caller (Resi... |
| 0x00407960 | 515 | HashTable_ConstructBuckets_407960 | same code as 0x407740 (vector<void*> storage of n, no fill); HashTable_Resize_406D10 |
| 0x00407B80 | 515 | HashTable_ConstructBuckets_407B80 | same code as 0x407740; HashTable_Resize_406EE0 |
| 0x00407E00 | 515 | HashTable_ConstructBuckets_407E00 | same code as 0x407740; HashTable_Resize_4070B0 |
| 0x0040BE50 | 515 | HashTable_ConstructBuckets_40BE50 | STLport vector<void*> storage of n (no fill), same code as 0x407740; HashTable_Resize_40BAC0 |
| 0x00409470 | 452 | Archive_SerializeStdString | (archive, tag, std::string* {begin +0, end +4}): as Archive_SerializeCString for an STLport string: storing writes strlen + 1 bytes; loading assigns the entry's C string (or "" 0x79B298 when absent) by overwrite + StlString_EraseR... |
| 0x00405950 | 398 | PoolAlloc | STLport __node_alloc::allocate(n) (fastcall): n > 0x80 -> operator new; else pop free list 0x79B224[(n-1)>>3]; empty -> _S_refill inlined (chunk 0x79B268..0x79B26C, heap size 0x79B264, 20 objects, grow by 2*20*n + heap/16, falls b... |

## 0x40C000 - 0x411000: CCamera (+ camera .cam files)

99 functions, group `client`.

| addr | size | name | purpose |
|---|---|---|---|
| 0x0040FB30 | 3103 | CCamera::Update | (dt in ticks, 1.0 = 55 ms): listener for CSoundManager (position = target or camera when recording/playing (flags 0x60), up axis); flags 0x60 -> return (fly path / recording own the transform); follow: carrier unit (FindById +0x11... |
| 0x0040DB50 | 2162 | CCamera::SampleSmoothHeight | (xy*, cellW, cellH): terrain height under a point smoothed with a bicubic uniform B-spline over a 4x4 grid of CWorldClient::GetHeight samples spaced cell/3 apart (clamped to the terrain size g_0x7AF738 +0x10C/+0x110); the 12 cubic... |
| 0x0040E4C0 | 814 | CCamera::AvoidObstacles | (): Terrain_SampleHeightBox at the camera position (step 1, +-2 m) -> max terrain height; CWorldClient::CollectObjectsNear within 16 m: for every object with bounds <= 100 the 'top' z + bounds max z (+0x14C) + radius (+0x130) weig... |
| 0x0040ECD0 | 730 | CCamera::LoadCamFile | (name, cutIndex): reads <resources>\camera\<name>: size / 0x24 keyframes {float time, pos xyz, quaternion} into +0x2F0 (count +0x2F4); when cutIndex > 1 the first keyframe whose next key is > 500 time units later ends the prefix a... |
| 0x0040F8B0 | 619 | CCamera::UpdateFromTarget | view quaternion +0x34 = yaw quaternion +0xF4 rotated by -(90 deg + pitch +0x104) about its right axis; position +0x1C = target +0xE0 - up axis * distance +0xF0 (z = target height - up.z * distance); AvoidObstacles; when it pushed ... |
| 0x0040C8B0 | 582 | CCamera::CCamera | (0x308 bytes, vt_CCamera, CObject3D transform at +0x1C/+0x34): shake list +0x108 (0x18 nodes), 8 view presets +0x120 (0x18 each: distance 30, identity yaw quaternion, pitch 45 deg), keyframe deque +0x1E0, .cam path +0x2EC, keyfram... |
| 0x0040E910 | 581 | CCamera::StartRecording | (index): in <work>\camera\: renames camera.cam to camera%.4d.cam (next free number) and opens a fresh camera\<index>.cam for recording: path +0x2EC, flags /= 0x10 (main then calls RecordKeyframe every frame), g_0x7AF5C0 /= 0x10. W... |
| 0x0040D470 | 562 | CCamera::TurnTowardsCarrier | (immediate): rotates the view quaternion +0x34 towards the carrier unit (CBaseManager::FindById g_0x7AF5F8, +0x11C) in 10% steps (full step when immediate) then LevelRoll; no callers in this build |
| 0x0040D710 | 555 | CCamera::LevelRoll | derives the roll of the view quaternion +0x34 (angle between the right axis and the horizontal of -up); when >= 1 deg rotates 10% of it back about the cross axis, then AvoidObstacles. Was ClampPitch (it does not clamp the pitch: U... |
| 0x0040C060 | 515 | DwordVector_ctorN8 | STL: vector<node*>(n) bucket vector ctor with the pool chunk refill inlined (g_0x79B224 lists, chunk_alloc); HashTable_Resize_40BC30 = rehash of the archive's written-object id set (Archive_Open / HashSetU32_Insert) |
| 0x0040EFB0 | 506 | CCamera::PlayCamFile | (time): flag 0x20 playback of the keyframes +0x2F0: finds the segment (current index +0x304, time offset +0x300), lerps the position +0x1C and slerps the quaternion +0x34; past the last key copies it and clears flag 0x20; main cal... |
| 0x00410E70 | 484 | dead_410E70 | unregistered code with no call, jump or pointer anywhere in the image (gaps.py 2026-11-06) |
| 0x0040D9B0 | 404 | Terrain_SampleHeightBox | (centre xy*, unused, unused, step, halfW, halfH, out avg, out min, out max): CWorldClient::GetHeight on a grid of points centre +- half extents at the given step (inside the terrain g_0x7AF738 +0x10C/+0x110), returns the average /... |
| 0x0040CC30 | 387 | CObject3D::UpdateWorldTransform | CObject3D slot 15: calls slot 14 (world rot) then world pos (+0x28) = parent pos + local pos (+0x1c) rotated by the parent quat (arg = parent's 7 floats: pos? no: arg[0..3] quat, used as rotation); helpers 0x401DE0 vec3 set, 0x40C... |

## 0x411000 - 0x414000: CCamera helpers (LookFromTo, SetDirection, AddShake), STLport deque<0x24>, Net_TickAll, ServerInfo / JoinMessage / ChatMessage / NamedEventMessage, LobbyServer::ctor

89 functions, group `net`.

| addr | size | name | purpose |
|---|---|---|---|
| 0x00412D80 | 1225 | LobbyServer::ctor | (this 0x2600, gameKind): NetConnection::ctor, pending-accept list +0x2568 (0x28 nodes {addr 16, linkId, rate, age}), vt 0x73BBB4 then 0x73BB70, NetSocket::Open(8888 = g_0x783E30), lists +0x2590..+0x25c4, strings +0x25c8 name / +0x... |
| 0x004110D0 | 645 | CCamera::SetDirection | (dx, dy, dz): yaw quaternion +0xF4 from atan2 of the horizontal direction (identity when straight down), pitch = asin(dz / len), view quaternion +0x34 = yaw rotated by -(90 deg + pitch) about x; false for a zero vector |
| 0x00412370 | 521 | ServerInfo::ctor | 0x48-byte ServerInfo (vtable 0x73BB40 base NetPeerInfo -> 0x73BB28): NetPeerInfo::Ctor, name +0x28 and map +0x38 = default string g_0x783D6C, +0x34 = 4, bytes +0x44..+0x46 = 0 |
| 0x00411660 | 432 | List18_CtorE | (this, alloc&): STLport list ctor: header node = 0x18 bytes from the free list g_0x79B22C (empty -> inlined _S_refill: 20 nodes from the chunk g_0x79B268..0x79B26C, heap size g_0x79B264), header next = prev = itself; caller CCamer... |
| 0x00411850 | 426 | List10_CtorB | (this, alloc&): same STLport list ctor as 0x411660 with 0x10-byte nodes (free list g_0x79B228); callers CWorldClient::ctor, CCamera::AvoidObstacles, CBattleHud::Update / OnLButtonUp |
| 0x00411F70 | 424 | Deque24_ReserveMapAtBack | (this, nodesToAdd, atFront): STLport _M_reallocate_map: if the map +0x20 (size +0x24) has room for 2 * needed, recentres the used node pointers inside it (memmove forwards or backwards), else allocates max(size, add) + size + 2 po... |
| 0x00412590 | 359 | ServerInfo::Clone | slot 3 of 0x73BB28: new 0x48 bytes, refCount +4 = 0, copies the NetPeerInfo part (address +8..+0x14, timeout +0x18, ping +0x1C, versionMismatch +0x20, source +0x24), name string +0x28, gameKind +0x34, map string +0x38, players / m... |
| 0x00412750 | 344 | ServerInfo::Reset | slot 2 of 0x73BB28: frees the map +0x38 and name +0x28 strings, re-runs NetPeerInfo::Ctor, re-creates both strings (8-byte buffers) and assigns "?" (0x783D6C) to name and map, gameKind +0x34 = 4, players / maxPlayers / hasPassword... |
| 0x00413570 | 306 | JoinMessage::Reset | slot 2 of 0x73BBF8: releases peerRecord +0x30 (refCount +4 --, 0 -> vt[0](1)), frees unitName and name, re-constructs: version 1, two empty strings (8-byte buffers), peerRecord 0, hostFlag 0, state +0x28 = 4, heroId +0x2C = -1 |
| 0x00413440 | 299 | JoinMessage::Clone | slot 3 of 0x73BBF8: new 0x34, refCount 0, version +8 copied, name +0xC and unitName +0x18 strings copied, hostFlag +0x24, state +0x28, heroId +0x2C, peerRecord +0x30 shared (its short refCount +4 ++); vt 0x73BBF8 |
| 0x00413D20 | 285 | NamedEventMessage::Clone | slot 3 of 0x73BC40: new 0x1C, refCount 0, version and kind +0xC copied, name +0x10 copied (length_error on overflow), vt 0x73BC40 |
| 0x00413930 | 279 | ChatMessage::Clone | slot 3 of 0x73BC28: new 0x18, refCount 0, version copied, text +0xC copied (length + 1 from PoolAlloc, length_error on overflow), vt 0x73BC28 |
| 0x00411C00 | 278 | Deque24_InitializeMap | (this, n): STLport _M_initialize_map: nodes = n/14 + 1, map size +0x24 = max(8, n/14 + 3) pointers (pool or operator new), buffers 0x1F8 bytes (14 elements) allocated for the centred node range (Deque24_CreateNodes), start = {firs... |
| 0x00412B70 | 259 | ObjListMessage::Read | slot 5 of 0x73BDD0 (cursor*): count, RefVector_Resize +0xc, every ref via NetPeer::ReadObjectRef(g_0x79B924), +0x18, u8 +0x1c, +0x20, +0x24, refs +0x28 / +0x2c |

## 0x414000 - 0x41A000: net message classes (NetMessage subclasses: strings / refs / blobs), LobbyServer map list, variables, file transfers, player events

153 functions, group `net`.

| addr | size | name | purpose |
|---|---|---|---|
| 0x004186F0 | 1262 | LobbyServer::SetMap | (quest name): called by OnNamedEvent kind 1 "<quest>_1"; clears the map list +0x25ac and variables +0x25b0, appends the quest state StringMessage "<quest>_2" (kind 8) to +0x25ac, creates the current-map StringMessage2 +0x25a8 (kin... |
| 0x00417EB0 | 1062 | LobbyServer::GatherReplicated | slot 13 of 0x73BB70 (out vector of {object, group byte}): what the peer g_0x79B924 must see, group 2 unless noted: all player records +0x2590, variables +0x25b0, its own peer record of +0x25a4 (+0x10 == peer), chat lines +0x2594 w... |
| 0x00418ED0 | 748 | LobbyServer::OnMapRecord | called by OnNamedEvent kind 1 "<quest>_3" (quest completed): chat line 7; record g_0x7C87D4["<quest>_3"]: for every peer posts player events kind 0 (record +0xc / peers: experience) and kind 1 (+0x10 / peers: money) and the map ev... |
| 0x00419A00 | 699 | LobbyServer::AddFileTransfer | (name string*, GameBuffer): unless a transfer with that name exists in the list +0x2598: creates a ListStringMessage {chunk count +0xc, size +0x10, name +0x14} and splits the buffer into 64-byte RefBlobMessage chunks (0x54: owner ... |
| 0x00417BC0 | 551 | LobbyServer::dtor | destructor body: strings +0x25d4 / +0x25c8, hash tables / lists +0x25c4..+0x2590, refs +0x25a8 / +0x25a0, the list +0x2568, then NetConnection::dtor |
| 0x00418CC0 | 520 | LobbyServer::UnloadMap | called by OnNamedEvent kind 1 "<quest>_2": chat line 6, PostMapEvents(record "<quest>_1", withStart 1), releases the current map +0x25a8, clears +0x25ac / +0x25b0 / +0x2598, RefreshMapList, Session_SetMapName(g_0x7AFE20, 0) |
| 0x004191C0 | 498 | LobbyServer::PostMapEvents | (mapRecord, withStart): for the first loaded player (list +0x25a4, +0x28 set): posts a kind-2 player event (0x419D90) for every string of the record's vector +0x14 and, when withStart, a kind-4 event with the string g_0x79B298 |
| 0x00419D90 | 435 | LobbyServer::PostPlayerEvent | (player, kind, value, strBegin, strEnd): creates a RefDwordsStringMessage {player ref, kind, value, string} and appends it to the timed list +0x25b4 with a 60 s timeout |
| 0x00418530 | 428 | LobbyServer::RefreshMapList | 0x580F10(); enumerates the maps (0x57F850 with +0x25e4 and the current name +0x25d4) and appends a StringMessage "<map><suffix 0x783D70>" per map to the list +0x25ac (0x5809D0 per map name) |
| 0x004193F0 | 379 | LobbyServer::OnQuestMapFinished | (name string*): when the current map message +0x25A8 carries that name: releases it, clears the map list +0x25AC and appends a StringMessage "<name>_3" (map progression); true when it matched. Was LobbyServer::OnMapUnloaded |
| 0x00416220 | 355 | ListStringMessage::Clone | slot 3 of 0x73BD18: new 0x2C, refCount 0, version, chunkCount +0xC, size +0x10, name +0x14 copied, chunk ObjRef vector +0x20 copied into a PoolAlloc block (ObjRefs_UninitCopy), vt 0x73BD18 |
| 0x00415120 | 345 | StringPairMessage::Read | slot 5 of 0x73BCA0 (cursor*): hostAddress +0xC = first C string, zoneName +0x18 = second (each overwrite / append 0x40C2A0 / erase tail); cursor advanced past both NULs |
| 0x00414500 | 330 | TwoStringMessage::Clone | slot 3 of 0x73BC70: new 0x3C, refCount 0, version, name and unitName strings copied, state, isHost, heroId, playerId, slotIndex, chatWatermark (+0x24..+0x38) copied, vt 0x73BC70 |
| 0x004195D0 | 324 | LobbyServer::GetOrAddVariable | (out node, name string*): finds the StringDwordsMessage variable of the list +0x25b0 with that name (0x427FA0 compare) or appends a new one with the name |

## 0x41A000 - 0x42D000: net lobby: LobbyServer / LobbyClient (players, chat, maps, variables, file transfers, pairing/party records), ChunkFile fields, STLport/MFC helpers

612 functions, group `net`.

| addr | size | name | purpose |
|---|---|---|---|
| 0x0041D600 | 813 | LobbyClient::OnServerInfoPacket | (GameBuffer from the master TCP stream, CConnectProgressWin::Update): u32 raw length must equal the buffer length, LzDecompress (fresh LzCompressor) then records until exhausted: sockaddr[16] + NetPlayerInfo::Read (0x401170: u32 +... |
| 0x0041ADB0 | 777 | LobbyServer::OnJoin | created-handler for JoinMessage (kind 5, this = LobbyServer, edx = message): new TwoStringMessage player record appended to +0x2590 {name +0xc, unit +0x18 from the join, +0x24 = join +0x28, +0x2c = join +0x2c, +0x30 = g_0x79B60C++... |
| 0x0041DA50 | 757 | LobbyClient::FillParty | rebuilds the local exchange record +0x260c (CExchangeScreen::Update / OnLButtonDown when the party changed): +0x18 = party version, one BlobMessage (kind 16) per hero of +0x2660 (count +0x2664): {u32 model +0xc, u32 +0x10, u32 +0x... |
| 0x0041DDC0 | 723 | LobbyClient::LoadPartnerParty | rebuilds the partner's offer party +0x2690 from its record's BlobMessages (+0xc..): each blob = one serialized offered item (kind 16); the exchange screen shows it as the other counter. Was LoadPartyFromMirror |
| 0x0041CD80 | 695 | LobbyClient::ApplyPlayerEvents | applies the RefDwordsStringMessages of +0x2608 (kind 14 {player ref, u32 kind +0x10, u32 value +0x14, string +0x18}) to the local CPlayer (CWorldServer::FindPlayerById(0)): 0 experience (GiveExperience(0, string)), 1 money += valu... |
| 0x0041CA10 | 690 | LobbyClient::dtor | destructor body of the LobbyClient (vtable 0x73BDEC, 0x26f0+ bytes): party blocks +0x2690 / +0x2620, ref +0x2618, party list +0x2614, lists +0x2610..+0x25d8, local record +0x260c, join message +0x2600, then NetClient members (+0x2... |
| 0x0041B2A0 | 645 | LobbyServer::OnNamedEvent | created-handler for NamedEventMessage (kind 7, {u32 kind +0xc, name +0x10}): all require the sender to be the host player (+0x28) except 3..5 which need IsPeerLoaded; 0 -> Session_SetMapName(g_0x7AFE20, name); 1 -> the name must b... |
| 0x0041BC40 | 611 | LobbyClient::ctor | (this 0x2700): NetConnection::ctor, NetClient part (+0x2568 server addr, login GameBuffer +0x2578, server info list +0x25a8, NetServerRecord vector +0x25ac, pending list +0x25bc, state +0x2598 = 0), NetSocket::Open(-1 = any port),... |
| 0x0041ECC0 | 590 | LobbyClient::OnChunkReceived | created-handler for RefBlobMessage (kind 12 {ref +0xc transfer, u16 index +0x10, 64 bytes +0x12}): appends to +0x25e4 (ignored when the transfer name is empty); when all +0xc chunks of the transfer are present: sorts by index, con... |
| 0x00426CB0 | 587 | ObjRefVector_InsertN | (this = vector<ObjRef>, pos, n, value*): _M_fill_insert: capacity short -> ObjRefVector_InsertNRealloc; else takes a temp ObjRef copy of *value (shared objects are cloned), moves the tail up (ObjRefs_UninitCopy2 + ObjRef_Set backw... |
| 0x0041A7E0 | 559 | LobbyServer::FillServerInfo | slot 15 of 0x73BB70 (out ref): new ServerInfo {name +0x25c8, +0x34 = +0x25e4, map = Game_GetQuestName of the current map +0x25a8 (empty when none), +0x44 = peer count, +0x45 = +0x25e0, +0x46 = password set}; serialized by NetServe... |
| 0x0041AA10 | 533 | LobbyServer::SendMasterHeartbeat | builds a NetPlayerInfo-style record {name +0x25c8, map (current quest or empty), u8 players, u8 max +0x25e0, u8 kind +0x25e4, u8 hasPassword, u32 request id +0x25fc, u32 public address +0x2578, u32 magic 0xDEC0AD07} with NetPlayer... |
| 0x00421F60 | 515 | ObjRefVector_CtorN | (this, n, alloc&): STLport _Vector_base(n): n dwords (0 -> null; <= 0x80 bytes from the free list g_0x79B224 or inlined refill from the chunk g_0x79B268; else operator new); start = finish = buffer, end_of_storage = buffer + n; ca... |
| 0x0042AC70 | 515 | PtrVector_CtorN2 | (this, n, alloc&): STLport _Vector_base(n) of dwords: same allocation code as ObjRefVector_CtorN 0x421F60 (free list g_0x79B224 / inlined refill / operator new), start = finish = buffer, end_of_storage = buffer + n; caller IdHash_... |

## 0x42D000 - 0x42E000: CClientSpecificManager; ChunkFile (tag+length chunk reader-writer over CFile), vector<byte>, polygon clipping (ClipTriangle / ClipPoly_*)

33 functions, group `core`.

| addr | size | name | purpose |
|---|---|---|---|
| 0x0042DE90 | 397 | ClipPoly_YMax | (unused, verts (0x20 each), &nextVertex, idx[] ushort in/out, &count, yMax): Sutherland-Hodgman pass against y <= yMax: inside vertices kept, crossing edges get a Vertex32_Lerp'd vertex at verts[*nextVertex++]; scratch list g_0x79... |
| 0x0042DB70 | 389 | ClipPoly_XMax | fastcall (outcode ignored, verts, &vertCount, idx[], &n, xMax): Sutherland-Hodgman pass for x <= xMax: keeps inside indices, for an edge crossing t = (xMax - x0) / (x1 - x0): t outside [0, 1] -> keeps the next index, else Vertex32... |
| 0x0042DD00 | 389 | ClipPoly_XMin | as ClipPoly_YMax but keeps x > plane (x > 0 in ClipTriangle; vertex x at +0): kept vertices and Vertex32_Lerp'd crossings at verts[*nextVertex++] through the scratch list g_0x79B66C, idx[] and *count rewritten; ClipTriangle / Clip... |
| 0x0042D700 | 386 | CClientSpecificManager::CreateByKind | slot 14 factory (kind): 0x45 CFigureClientSpecific (0x3c bytes, ctor 0x5A7010), 0x46 CObject3DClientSpecific (0xfc, 0x51AB00), 0x47 CMapObjectClientSpecific (0x58, 0x510450), 0x48 CUnitClientSpecific (0xdc, 0x533AD0), 0x49 CPlayer... |
| 0x0042D910 | 363 | ClipTriangle | fastcall (verts 0x20-byte TL vertices, outcodes[], idx u16[3] in/out, &vertCount, &idxCount, xMax, yMax): scratch CArray g_0x79B668 SetSize(0x20); all three outcodes share a bit (& 0x3F) -> idxCount 0, returns -1; none set -> idxC... |
| 0x0042D370 | 357 | Bytes_InsertN | STL: vector<byte>::insert(pos, n, value) in-place path (memmove of the tail, fill), grow path Bytes_InsertNRealloc; ChunkFile::ReadBytesToString (chunk payload into a byte vector) and CString_Scramble |
| 0x0042D4E0 | 287 | Bytes_InsertNRealloc | STL: vector<byte>::insert(pos, n, value) reallocating path (buffer max(2 size, size + n) from the pool / new, both halves copied, fill, old buffer freed); Bytes_InsertN and ByteVector_Resize (CString_Scramble / Unscramble, LoadWor... |
| 0x0042DA80 | 229 | ClipPolygon | fastcall (verts, idx[3], &vertCount, &idxCount, xMax, yMax): same as ClipTriangle without the outcode tests (always runs the six clip passes, then the fan); no caller, no pointer, no raw E8/E9 hit [dead: no caller or pointer] |
| 0x0042D1C0 | 146 | ChunkFile::ctor | (name CString): vtable 0x73BE94 -> 0x73BE9C, tag / consumed / length / start = 0, parent +0x14 = 0, CFile at +0x18, opens the file; NO CALLERS (the chunk reader is only used through sub-chunk objects created by the owning serializ... |
| 0x0042D060 | 120 | ChunkFile::ReadVec4B | (out): reads 16 bytes (default {1, 0, 0, 0}) through CFile::Read and copies the four dwords read to the output (machine code 0x42D0BB..0x42D0CF; the decompile's {0x10, .., x, y} was a stack misread); second function named ReadVec4... |
| 0x0042D0E0 | 115 | ChunkFile::WriteDwordPair | (value): CFile::Write (vt[14]) of the dword twice plus 16 more bytes; consumed bytes accounted on the parent |
| 0x0042D160 | 96 | ChunkFile::Open | (name, mode): CFile::Open (CFile +0x18 vt[8]) with mode & 0xffff |
| 0x0042D270 | 73 | ChunkFile::dtor | destroys the CFile and restores the base vtable 0x73BE94 |
| 0x0042D020 | 52 | ChunkFile::Read | (buf, n): CFile::Read (vt[1] GetFile -> CFile vt[13]) then Consume(n) on the parent and +8 += n |

## 0x42E000 - 0x433000: GameBuffer; Vertex32_Lerp, OBB tests, Mat3_Solve, LZ compressor (LzCompress / LzDecompress), BigNum_ModExp + RIPEMD-128 key hash, NetPeer::ctor

41 functions, group `leaf`.

| addr | size | name | purpose |
|---|---|---|---|
| 0x004319E0 | 4787 | Ripemd128_Compress | (state[4], block[64]): RIPEMD-128 compression function (two parallel lines, constants 0x5a827999 / 0x6ed9eba1 / 0x8f1bbcdc and 0x50a28be6 / 0x5c4dd124 / 0x6d703ef3) |
| 0x0042E700 | 3436 | OBB_Intersects | fastcall (boxA, boxB, posA xyz, posB xyz, quatA*, quatB*): builds the 3 axes of each box from its quaternion (w,x,y,z), half extents at box +0x20/+0x24/+0x28, expresses the centre offset and B's axes in A's frame with Mat3_Solve, ... |
| 0x00430960 | 1523 | LzDecompress | (this = LzCompressor, in GameBuffer view, out GameBuffer): u32 raw length then tokens until it is produced: bit 0 = literal byte (next stream byte); bit 1 = match: length prefix tree (0 0 -> 2, 0 1 -> 4, 1 0 0 0 -> 3, 1 0 0 1 + 3 ... |
| 0x00430450 | 1243 | LzCompress | (this = LzCompressor, input view (LzInputAdapter over the source bytes), output GameBuffer): u32 raw length then LZ77 tokens over the persistent 1 KB ring window (+0 window, +0x400 copy, +0x800 chain distances, +0x1800 1024 hash h... |
| 0x0042FD10 | 1222 | LzCompressor::EmitMatch | (this, matchPos): writes the match length +0x2820 (< 35 table code; 35..98 prefix 0x17 in 5 bits + 6 bits of len-35; 99..226 prefix 0x3f in 6 bits + 7 bits of len-99) then the distance d = (pos - len - matchPos - 1) & 0x3ff (d < 3... |
| 0x004315B0 | 1068 | BigNum_DivMod | (a (2n ushorts), b (n), n bytes, q out): multi-precision division of a by b (Knuth D on 16-bit limbs): remainder left in a, quotient in q |
| 0x0042F4C0 | 976 | OBB_IntersectsSegment | fastcall (box, quat*, boxPos xyz, segCentre xyz, segVec xyz): box axes from the quaternion, segment direction = segVec / /segVec/, half length /segVec/ * 0.5, centre offset and direction expressed in the box frame (Mat3_Solve), 6 ... |
| 0x00432DA0 | 666 | NetPeer::ctor | (this 0x1168, linkId): refcounted base -> vtable 0x73BF38; +8 next send seq = GetTickCount() & 0xffff, +0xc last received seq = 0, +0x10 wrap base = 0x10000, receive window vector +0x14, +0x20 linkId, +0x24 rtt mean = NetConnectio... |
| 0x00431370 | 562 | BigNum_ModExp | (base, exponent bits, modulus, nBytes, out): square-and-multiply modular exponentiation on 16-bit limbs (BigNum_DivMod); NO CALLERS: a leftover key-exchange routine, never used by the game |
| 0x0042E4D0 | 465 | Vertex32_Lerp | fastcall (a, b, t, out): D3D TL vertex lerp: sx, sy, sz, rhw (+0..+0xC) and tu, tv (+0x18 / +0x1C) as floats; the colour +0x10 and specular +0x14 per byte channel, each rounded (fistp) and repacked; callers the six ClipPoly_* pass... |
| 0x0042FAE0 | 444 | LzCodec_InitTables | static initializer of the LZ match-length code tables g_0x79B848 (code) / g_0x79B7C0 (bits): lengths 1..6 fixed codes (1,3,5,3,5,5 bits), 7..10 = 6-bit code, 11..18 = 8-bit, 19..34 = 10-bit; offset tables g_0x79B738 / g_0x79B6B0 f... |
| 0x0042E020 | 397 | ClipPoly_YMin | as ClipPoly_YMax but keeps y > plane (y > 0 in ClipTriangle): inside vertices kept, crossing edges interpolated (Vertex32_Lerp) into new vertices at verts[*nextVertex++], results written back into idx[] and *count through the scra... |
| 0x0042E1B0 | 397 | ClipPoly_ZMax | as ClipPoly_YMax but against z <= plane (z <= 1 in ClipTriangle; vertex z at +8): kept / interpolated vertices through the scratch list g_0x79B66C, idx[] and *count rewritten; ClipTriangle / ClipPolygon |
| 0x0042E340 | 397 | ClipPoly_ZMin | as ClipPoly_YMax but keeps z > plane (z > 0 near plane in ClipTriangle; vertex z at +8): new vertices from Vertex32_Lerp at verts[*nextVertex++], idx[] and *count rewritten; ClipTriangle / ClipPolygon |

## 0x433000 - 0x444000: net transport: NetConnection / NetServer / NetClient (A)

472 functions, group `net`.

| addr | size | name | purpose |
|---|---|---|---|
| 0x00435A00 | 1295 | NetPeer::SendUpdate | (this = NetPeer, budget = NetPeer +0x50 = 220 bytes; from Tick): SyncObjectSet; builds in g_0x79B900 (reserve budget + 0x400): u8 0, u16 seq (+8 ++), WriteAcks, TakeStreamChunk(budget - 16 = 204 max) + the chunk {u16 offset, u8 le... |
| 0x00435F30 | 1268 | NetPeer::ReadMessage | (this = NetPeer, cursor*, packet seq): flag bit (LSB-first from the byte read before each group of 8), u8 kind, u16 id, key = kind << 16 / id in the received-object tree +0x1100 (node +0x14 ObjRef, +0x18 version = seq of the last ... |
| 0x00443080 | 1131 | NetServer::OnLogin | slot 5 (addr*, cursor*): u32 magic (must equal +8 else ignored), u32 linkId, u32 rate (clamped to GetRateFor(addr)); a known peer at that address -> "ERROR: Ignoring login request for active session"; a pending accept record (+0x2... |
| 0x0043D830 | 933 | RbTree_RebalanceForErase | fastcall (z, &root, &leftmost, &rightmost): STLport _Rb_tree_rebalance_for_erase: unlinks z (swapping in its successor when it has two children), fixes leftmost / rightmost, recolours / rotates (colour byte 0 red, 1 black; left +8... |
| 0x00441D40 | 918 | NetClient::OnInfo | slot 7 (addr*, cursor*): f32 echo, u32 magic (must match +8 ignoring the low byte = protocol version); vt[16] NewServerInfo makes a ServerInfo, vt[5] Read fills it, addr -> +8, +0x20 = version mismatch flag (low byte differs), +0x... |
| 0x00433BD0 | 826 | NetPeer::Tick | (this = NetPeer link, dt double): +0x44 += dt (silence timer), +0x48 += dt (clock); resend timeout = clamp(rttMean + 3 * sqrt(/mean^2 - var/), 0.1, NetConnection +0x10); sent packets +0x10f8 older than it -> PacketLost and moved t... |
| 0x00437370 | 823 | NetConnection::Receive | (dt double): g_0x79B920 = this, clock +0x20 += dt; for every datagram (NetSocket::ReceiveFrom into g_0x79B8E0, 2048 max; "ZERO length packet received from" when empty): u8 type -> slot 3 OnUpdate (0), 6 OnDisconnect (1), 4 OnGetIn... |
| 0x00433060 | 698 | NetPeer::Clone | slot 3: new 0x1168-byte NetPeer copy-constructed from this (all trees / lists / buffers) |
| 0x00434110 | 685 | NetPeer::RegisterObject | (this = NetPeer, {object, group}): kind = registry +0x2c lookup of the object's vtable; new 0x14-byte NetPendingEntry (vtable 0x73BF4C: +8 version sent, +0xc u16 id, +0xe u8 kind, +0xf bit 0 ack-mode (from the connection byte tabl... |
| 0x004346D0 | 673 | NetPeer::SyncObjectSet | (this = NetPeer): calls NetConnection vt[13] GatherReplicated of g_0x79B920 into the global vector g_0x79B8D0 of {object, group}, sorts it by object pointer, merges with the object tree +0x1068: new objects -> RegisterObject, gone... |
| 0x0043BBC0 | 666 | ObjRefTree_InsertWithHint | (out it, hint, value*): STLport insert_unique(hint, value) for the signed-int-keyed {key, ObjRef} tree (entries ObjRefNode18_New2): O(1) paths when hint is begin / end or value lies between hint-1 and hint (ObjRefTree_InsertAt or ... |
| 0x0043C100 | 666 | ObjRefTree_InsertWithHint2 | (out it, hint, value*): the same STLport insert_unique(hint) for an unsigned-keyed {key, ObjRef} tree (entries ObjRefNode18_New3, fallback ObjRefTree_InsertUnique2 / InsertAt2); one caller ObjRefTree_InsertWithHintIter2 |
| 0x0043C650 | 666 | ObjRefTree_InsertWithHint3 | (out it, hint, value*): the same STLport insert_unique(hint) for the third unsigned-keyed {key, ObjRef} tree (entries ObjRefNode18_New4, fallback InsertUnique3 / InsertAt3); one caller ObjRefTree_InsertWithHintIter3 |
| 0x0043CBC0 | 666 | ScriptRefTree_InsertWithHint | (out it, hint, value*): the same STLport insert_unique(hint) for the signed-int-keyed {key, ScriptObjRef, version} tree (entries ScriptRefNode20_New, fallback ScriptRefTree_InsertUnique / InsertAt); one caller ScriptRefTree_Insert... |

## 0x444000 - 0x446000: debug text (Courier New) + misc

62 functions, group `client`.

| addr | size | name | purpose |
|---|---|---|---|
| 0x00445360 | 703 | StringMap_InsertValue | (out it, hint, pair*): STLport insert_unique(hint, value) for map<string, string> (keys compared with StringRange_Less): O(1) paths at begin / end / between hint-1 and hint (StringMapNode_Create + RbTree_Rebalance, or StringMap_In... |
| 0x00444D00 | 584 | DebugText::Render | fastcall: when the window +0x14 exists: GetDC (0x5FDC00), clears it to colour 0 (0x70BB04), transparent background (0x7090C2), selects the font; walks the map +4 from begin: per entry draws key (left half) and value (right half) f... |
| 0x00444090 | 475 | NetServerRecordVector_Resize | STL: vector<NetServerRecord 0x2c>::resize(n): a default record (zero NetAddress + GameBuffer::ctor) inserted n - size times (NetServerRecords_InsertN) or the tail destroyed (GameBuffer::dtor); AddrBook_Serialize when loading the l... |
| 0x004447E0 | 441 | StringMap_SetValue | vt 0x73BFA4 slot 0 (key, text): builds a std::string key, StringMap_FindInsertPos (lower_bound); when it is end or key < its key (StringRange_Less) inserts {key, empty string} (StringMapValue_CopyCtor + StringMap_InsertAt); then o... |
| 0x00445A00 | 420 | StringMap_InsertUnique | (out {it, bool}, pair*): descends with StringRange_Compare; duplicate key -> {existing, false}; else links a new entry (StringMapNode_Create) and rebalances, {entry, true}; one caller StringMap_InsertValue |
| 0x00445870 | 389 | StringMap_InsertNode | (out it, x, parent, pair*): STLport _M_insert for map<string, string>: new StringMapNode_Create entry as the left child of parent (parent = header, x != 0, or key < parent key) updating root / leftmost, else the right child (right... |
| 0x00445E50 | 311 | CString_ReplaceAll | (this CString, find, repl): loop: pos = Find(find, pos), -1 -> return; string = Left(pos) + repl + Mid(pos + strlen(find)) (StringRangeRef_ctor helper, operator+); pos += strlen(repl); no caller or pointer [dead: no caller or poin... |
| 0x00445BD0 | 273 | StringMapNode_Create | (pair*): 0x28 entry from g_0x79B234 (refill 20 via stlport_node_alloc_chunk_alloc 0x28); copy-constructs the key string at +0x10 and the value string at +0x1c (String_CtorRange); returns the entry; 10 sites |
| 0x00445D30 | 270 | CStringFinder_Next | fastcall (finder): pos == src length -> pos = -1; pos -1 -> returns 0; CString::Find(sep, pos): not found -> piece = Mid(pos) (the tail), pos = length; found -> piece = Mid(pos, found - pos), pos = found + strlen(sep); returns the... |
| 0x00445170 | 259 | StringMapValue_CopyCtor | (this pair, key string*, value string*): pair<string, string> ctor: allocates len+1 for the key (PoolAlloc; String_ThrowLengthError on overflow), copies it with a terminating 0 (+0 begin, +4 end, +8 cap), then the same for the val... |
| 0x004449A0 | 208 | StringMap_RemoveByName | vt 0x73BFA4 slot 1 (key): builds a std::string key, StringMap_LowerBound (really map::find), frees the temp; when found erases the entry (Thunk_StringMap_Erase_445010) |
| 0x00444C10 | 191 | DebugText::SetRect | (rect*): stores the rect at +0x2c..+0x38, UiScale::ScaleRect with the owner +0x10, clamps width to [100, screen width] and height to [15, screen height], copies to +0x1c..+0x28 and UiScale::UnscaleRect it; when shown (+0x3c) re-cr... |
| 0x00445620 | 191 | StringMap_Find | (tree, out it, key string*): lower bound with StringRange_Compare, then end unless the found key equals the key (memcmp of the shorter length, then the lengths); callers StringMap_RemoveByName; CORRECTION: was 'StringMap_LowerBoun... |
| 0x00444F50 | 185 | DebugText::Show | (show): +0x3c = show; show: creates the 0x38-byte CUIBlitSurface window (owner +0x10, rect +0x1c, 5) when missing, +0x40 = timeGetTime, Render; hide: deletes the window, +0x14 = 0; callers DebugText::SetRect (2) |

## 0x446000 - 0x447000: CFakeUnit

35 functions, group `units`.

| addr | size | name | purpose |
|---|---|---|---|
| 0x00446C10 | 4909 | CFakeUnit::LoadModelFromScene | loads a "scenes\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\... |
| 0x00446910 | 466 | CFakeUnit::~CFakeUnit_body | vt = vt_CFakeUnit; animSystemOld +0x4e0 Detach + SetOwner 0; CUnit::Done; CFigures_CArraySetSize(+0x530, 0); refcount +4 of each figure -= 1; destroys the arrays materials +0x570 (0x9c each, vt[0](0)), vectors +0x55c (0x14, vt[1](... |
| 0x00446520 | 420 | dead_446520 | unregistered code with no call, jump or pointer anywhere in the image (gaps.py 2026-11-06) |
| 0x00446330 | 321 | dead_446330 | unregistered code with no call, jump or pointer anywhere in the image (gaps.py 2026-11-06) |
| 0x004466D0 | 213 | CFakeUnit::CFakeUnit | ctor (0x580 bytes, kind 0x39 in CMapObjectManager::CreateByKind): CUnit::CUnit, CAnimationSystemOld::ctor at +0x4e0, four empty MFC arrays at +0x530 (vt 0x73C108) / +0x544 (0x73C0FC) / +0x558 (0x73C0F0) / +0x56c (0x73C0E4), then v... |
| 0x004460D0 | 159 | dead_4460D0 | unregistered code with no call, jump or pointer anywhere in the image (gaps.py 2026-11-06) |
| 0x00446480 | 150 | dead_446480 | unregistered code with no call, jump or pointer anywhere in the image (gaps.py 2026-11-06) |
| 0x00446B20 | 116 | CFakeUnit::Done | slot 3: animSystemOld +0x4e0 Detach + SetOwner 0, CUnit::Done, destroys the figures +0x534 (count +0x538, 0x220 each, vt[0](0)), frees the array, figures = 0, count +0x538 = cap +0x53c = 0 |
| 0x00446050 | 114 | CString_FromInt | (out CString, int): CString::Format("%i" at 0x7842D8, value), copy into out; returns out; no caller or pointer; CORRECTION: was 'CString_InitDefaultConst' [dead: no caller or pointer] |
| 0x00446BA0 | 112 | CFakeUnit::Load | CFakeUnit slot 62 (name, arg): 0x446C10(name, arg, this); for each of the +0x534[] entries (0x220 bytes, count +0x538): 0x59F390(entry, vt[18]()->vt[23](0x45 kind, -1)) (RegisterInstant of a CFigureClientSpecific per entry); retur... |
| 0x004462C0 | 98 | dead_4462C0 | unregistered code with no call, jump or pointer anywhere in the image (gaps.py 2026-11-06) |
| 0x00446260 | 94 | dead_446260 | unregistered code with no call, jump or pointer anywhere in the image (gaps.py 2026-11-06) |
| 0x00446170 | 64 | dead_446170 | unregistered code with no call, jump or pointer anywhere in the image (gaps.py 2026-11-06) |
| 0x004461B0 | 45 | CString_WriteToStream | (this CString, stream): stream vt[14] (+0x38)(GetBuffer(0), length -8), then ReleaseBuffer(-1); no caller or pointer [dead: no caller or pointer] |

## 0x447000 - 0x453000: 3ds model import / figure export (editor leftover): Scene3ds_ResolveNodesAMF, WeldVertices, ComputeNormals, BuildIndexedComponents, CorrectMappings (.reg atlas), Material_ParseName, AnimNode_Write .ani / .ast / .bon, old CAnimationNode tree building; container serializers

124 functions, group `figures`.

| addr | size | name | purpose |
|---|---|---|---|
| 0x004492F0 | 3893 | Scene3ds_FillAnimNode | fastcall (CAnimationSystemOld, AnimNodeOld 0x128, 3ds node record 0x120, short frameCount): the node is appended to the system's flat node list +4 (count +8) (AlignedDwords_SetAtGrow5), name +4 = record +4, frame count +0x40 = fra... |
| 0x0044CFF0 | 3327 | Scene3ds_BuildIndexedComponents | fastcall (meshes CArray of 0x18c {data +4, count +8}): per mesh: every face corner (faces +0x138, 0x34, count +0x13c) is keyed by its vertex (+0..+8), normal (+0x18..+0x20) and uv (+0xc..+0x14) index into a local CMap (vt 0x73C190... |
| 0x0044B980 | 2787 | Scene3ds_CorrectMappings | fastcall (scene, regPath CString*, out {textureKind, materialKind} CArray, out {textureKind, CString texture} CArray) -> 1 / 0: CFileRegistry::Open(regPath) (failure: FatalError "Can't open .reg file \"%s\"."), GetSectionNames; pe... |
| 0x0044A7B0 | 2214 | AnimNode_WriteAniAst | fastcall (AnimNodeOld, ani CFile*, ast CFile*, splitFrame or -1), writes through the files' vt[14] Write: -1 = the whole clip: .ani gets rest rotation +0xe4 (16 B), pivot +0xf4 (12), vertex count + working morph vertices +0xd4, fr... |
| 0x0044C4A0 | 1892 | Scene3ds_WeldVertices | fastcall (scene, 3ds nodes CArray of 0x120): per mesh i (+0x548, 0x18c, count +0x54c; node i of the CArray is assumed to be the mesh's node, both walked in parallel): vertices +0xd8 (count +0xdc) that lie within 1e-5 of an earlier... |
| 0x0044DCF0 | 1728 | Scene3ds_ComputeNormals | fastcall (meshes CArray of 0x18c): per mesh: the normal vector +0xf8 cleared; every face (+0x138, 0x34) gets its plane (Plane_FromTriangleIndices with the vertices +0xd8, normal at face +0x24); per vertex v: the sum (from g_0x79B9... |
| 0x0044EB60 | 1603 | AnimNode_MakeRelativeToParent | fastcall (parent AnimNodeOld or 0, node, frame): the children +0x20 first (so they still see this node's absolute values); root: rotation key [frame] (+0x44, 0x28) quaternion +8 = Quat_FromAxisAngle(axis +0x18, -angle +0x24); with... |
| 0x00448A40 | 1507 | Scene3ds_ResolveNodesAMF | fastcall (CFakeFigure CArray CFakeUnit +0x544 (0x18c each), 3ds nodes CArray of 0x120, short frameCount = scene frames - 1, CAnimationSystemOld CFakeUnit +0x4e0) -> 1 / FatalError result: per node the first position key (+0xac, it... |
| 0x0044A2A0 | 1285 | Material_ParseName | fastcall (name, out textureKind*, out materialKind*) -> 1: the name is cut at every '.', only the parts BEFORE a dot are kept (CStringArray); exactly 3 parts: part 2 lower-cased (CString::MakeUpper 0x700EFE is really MakeLower) an... |
| 0x0044E3B0 | 1250 | Scene3ds_BuildSoAVertices | fastcall (scene): per mesh (+0x548, 0x18c, count +0x54c): the SoA vector +0xa8 (count +0xac) is sized to ceil(vertices +0xdc / 4) 0x40-byte blocks (AlignedAlloc32_452E70, growth grow or count/8 clamped 4..0x400), every lane first ... |
| 0x0044B370 | 1008 | dead_44B370 | unregistered code with no call, jump or pointer anywhere in the image (gaps.py 2026-11-06) |
| 0x00448000 | 931 | sub_448000 | unregistered function, reached by call / pointer from 0x33FD89 (gaps.py 2026-11-06). Dead: its only 4-byte match (file +0x33FD89) is unaligned float data, no call or jump; start of the 3ds transform chain (Nodes3ds_AccumulateTrans... |
| 0x004483B0 | 880 | Nodes3ds_AccumulateTransforms | (parent 3ds node record 0x120, nodes CArray, index*): from *index on, while the record's depth short +0xc is greater than the parent's: the record pointer is appended to the parent's child CArray +0x10c (data +0x110, count +0x114)... |
| 0x0044B060 | 779 | AnimNode_WriteAniAstFiles | fastcall (AnimNodeOld, dir, suffix, splitFrame): builds "<dir><node name +4><suffix>.ani" and the same with ".ast" (both lower-cased; CORRECTION: was "<pathAni>..ani / <pathAst>..ast": the third argument is a name suffix used for ... |

## 0x453000 - 0x455000: CFigure base in-place constructors (CFigures_ConstructN 0x220, CFakeFigures_ConstructN 0x18c = the 3ds mesh record), CFakeFigure dtor, hash / CList / aligned vector helpers

70 functions, group `figures`.

| addr | size | name | purpose |
|---|---|---|---|
| 0x00454A70 | 472 | StrRecs18Array::Serialize | slot 2 of vt 0x73C2BC (this = CArray {vt, data +4, count +8, cap +0xc, grow +0x10} of 0x18-byte {CString, CArray} records, CArchive*): loading (archive flags +0x14 bit 0): ReadCount then SetSize inlined (StrRecs18_ConstructN for n... |
| 0x004545C0 | 470 | CFakeFigures_ConstructN | (objs, n): zero-fills n*0x18c bytes, then per object: CObject base (vt 0x73B8E0), identity quaternions +0x34 / +0x44, vt 0x73BA38 (CObject3D), name +0x94 = empty CString, the eleven 4-dword vectors +0x98..+0x147 = 0, the StrRecs18... |
| 0x00454F80 | 458 | Nodes3ds_ConstructN | (records, n): zero-fills n*0x120 bytes, then per 3ds node record: CStrings +4 / +8 = empty, identity quaternions (1.0 first) at +0x30, +0x40, +0x7c, +0x98, MFC CArrays +0xa8 (vt 0x73C1AC), +0xbc (0x73C1B8), +0xd0 (0x73C114), +0xe4... |
| 0x00453550 | 421 | Materials9C_CArraySetSize | (this = CArray {vt, data +4, count +8, cap +0xc, grow +0x10} of 0x9c-byte CMaterial records, n, grow): MFC SetSize: n 0 -> vt[0](0) on every element then free; empty -> new(n*0x9c) + Materials9C_ConstructN; n <= cap -> construct t... |
| 0x00453240 | 412 | CFigures_ConstructN | (objs, n): zero-fills n*0x220 bytes, then for each 0x220-byte object: CObject3DBase_InitFields, +0x11c owner 0, names +0x120 / +0x124 = empty CString, +0x128..+0x134 = 0, AlignedArray_Init_A on the diffuse / specular colour arrays... |
| 0x00454830 | 409 | CFakeFigure::dtor | (this): StrRecs18 CArray +0x148 (vt 0x73C2BC): StrRec18_DeletingDtor(0) on each 0x18-byte element, data freed; the eleven 32-byte aligned vectors +0x138, +0x128, ... +0x98 freed (raw block at ptr[-4]); CString +0x94 destroyed; vt ... |
| 0x00453E00 | 381 | Recs10_CArraySetAtGrow | (this = CArray of 0x10-byte records, index, record*): MFC SetAtGrow: index >= count -> SetSize(index+1) inlined (new elements zero-filled, growth count/8 clamped 4..0x400), then copies the 4 dwords of the record into data[index] |
| 0x00453A30 | 341 | Pairs8_CArraySetSize | (this = CArray {vt, data +4, count +8, cap +0xc, grow +0x10} of 8-byte records, n, grow): MFC CArray SetSize; new elements zero-filled; growth = grow or count/8 clamped 4..0x400 |
| 0x00453CA0 | 341 | Recs10_CArraySetSize | (this = CArray {vt, data +4, count +8, cap +0xc, grow +0x10} of 0x10-byte records, n, grow): MFC CArray SetSize; new elements zero-filled; growth = grow or count/8 clamped 4..0x400 |
| 0x004538D0 | 311 | AlignedDwords_SetAtGrow6 | (this = 32-byte aligned dword vector {data +0, count +4, cap +8, grow +0xc}, index, value): if index >= count grows to index+1 (same SetSize logic as Aligned8_SetSize2, new slots uninitialised), then data[index] = value |
| 0x00453790 | 307 | Dwords_CArraySetSize | (this = CArray {vt, data +4, count +8, cap +0xc, grow +0x10} of dwords, n, grow): MFC CArray SetSize; new elements zero-filled; growth = grow or count/8 clamped 4..0x400; plain operator_new / delete (not aligned) |
| 0x004543E0 | 293 | Node3ds_Destroy | (3ds node record 0x120): destroys the six MFC CArrays in reverse order: +0x10c (vt 0x73C138), +0xf8 (0x73C12C), +0xe4 (0x73C120), +0xd0 (0x73C114: AlignedVector_Free on +4 of each 0x14-byte element first), +0xbc (0x73C1B8), +0xa8 ... |
| 0x00453100 | 263 | Aligned8_SetSize2 | (this = {data +0, count +4, cap +8, grow +0xc}, n, grow): SetSize of a 32-byte aligned vector of 8-byte elements: grow != -1 stored at +0xc; n 0 frees the block (raw pointer at data[-4]) and zeroes count / cap; empty -> new(n*8+0x... |
| 0x00454CE0 | 163 | StrRecs18Array::dtor | (this = CArray vt 0x73C2BC): vt reset, StrRec18_Destroy inlined on every 0x18-byte record (SetSize 0, Empty, CArray dtor, CString dtor), data freed |

## 0x455000 - 0x456000: CObject helpers (GetWorld, SetManager) + disk / capture

60 functions, group `core`.

| addr | size | name | purpose |
|---|---|---|---|
| 0x00455680 | 317 | PlayerRelations::DeclareHostile | (this = CWorldServer +0x12c64 PlayerRelations, attacker CUnit, victim CUnit): unless attacker.side is a FRIEND of victim.side (row bit): victim.SetAlarmMask(+0x244 / bit attacker.side) (sets flag 0x80000 = alarm to propagate) and ... |
| 0x00455170 | 311 | Records14_SetSize | (this = MFC CArray {vt, data +4, count +8, cap +0xc, grow +0x10} of 0x14-byte records, n, grow): MFC SetSize; new elements zero-filled by ZeroFill_Elem20_A, removed ones not destructed; growth = grow or count/8 clamped 4..0x400 |
| 0x004552B0 | 311 | Records18_SetSize | (this = MFC CArray of 0x18-byte records, n, grow): MFC SetSize; new elements zero-filled by ZeroFill_Elem24_A; growth = grow or count/8 clamped 4..0x400 |
| 0x004554E0 | 292 | PlayerRelations::Set | (side a, side b, mode, applyToUnits): mode 0 FRIEND (row[a] /= bit b, forced bit cleared), 1 NEUTRAL (both cleared), 2 ENEMY (forced bit set, friend bit cleared); applyToUnits: every unit of side a gets bit b set (enemy) / cleared... |
| 0x00455BA0 | 220 | Protection_Penalty | resets the application settings to defaults (AppSettings::SetDefaults on g_0x79BC70) and randomises the flags of every object of the world manager (CMapObject::SetSpecificRaw / CObject3D::SetSpecific46Raw with random 12-bit values... |
| 0x00455E50 | 176 | GlobalList79BC2C_Init | static initializer: allocates the 0x10-byte sentinel node of the STLport list g_0x79BC2C from the 16-byte node free list g_0x79B228 (refilled by stlport_node_alloc_chunk_alloc(0x10) and chained when empty) and links it to itself (... |
| 0x00455A90 | 166 | Disk_GetFreeBytes | (root path): GetDiskFreeSpaceExA (via GetProcAddress on kernel32) or the sector-based GetDiskFreeSpaceA fallback; 0 on failure |
| 0x00455FF0 | 110 | dead_455FF0 | unregistered code with no call, jump or pointer anywhere in the image (gaps.py 2026-11-06) |
| 0x00455F80 | 108 | dead_455F80 | unregistered code with no call, jump or pointer anywhere in the image (gaps.py 2026-11-06) |
| 0x00455A30 | 93 | sub_455A30 | unregistered function, reached by call / pointer from 0x56937 (gaps.py 2026-11-06) |
| 0x00455830 | 83 | HashSet_Begin4 | (this = STLport hashtable {?, buckets begin +4, end +8}, out iterator {node, table}): begin(): first non-null bucket head into out[0] (0 when all empty), out[1] = this |
| 0x00455B50 | 79 | Protection_RandomCheck | rolls rand (~1 in 100 per call); when it hits finds an entry flagged in the global list g_0x79BC2C (byte +8 == 0) and calls Protection_Penalty |
| 0x004553F0 | 74 | Pair8_ConstructN | (records, n): zero-fills n 8-byte records, then each record's CString +4 = empty (g_0x796E48) |
| 0x00455F40 | 59 | dead_455F40 | unregistered code with no call, jump or pointer anywhere in the image (gaps.py 2026-11-06) |

## 0x456000 - 0x461000: application: main window, settings sections, DX7 check, sysmsg.txt

108 functions, group `app`.

| addr | size | name | purpose |
|---|---|---|---|
| 0x0045B210 | 4934 | AppSettings::ReadFromRegistry | (subkey 'Software\Nival Interactive\EvilIslands'): HKEY_CURRENT_USER; every value with the current field as default: strings Work path +0x20, Resources path +0x24, CD-ROM Path +0x28, VideoResolution 'WxH' -> +0x30/+0x34, DWORDs Dr... |
| 0x00456440 | 4832 | main | WinMain: res\sysmsg.txt (3 lines), App_ProbeDirectX < 0x700 -> CRITICAL ERROR + return 0xDEAD; save factories; command line (-moviepath_, -nocam, *.cam/*.scr/*.mob/*.mpr substrings -> g_0x79BA1C/18/14/10, -benchmark_<n> -> NumberO... |
| 0x00459100 | 4734 | AppSettings::ReadFromIni | (ini path): reads every setting from an ini file (CIniFile) with the section names +0..+0x1C ('Path Settings' Work path / Resources path / CD-ROM Path, 'General Settings' video / audio / graphics / shadows, 'Terrain Settings' fog,... |
| 0x0045EA20 | 4124 | MapInfo_Load | (this MapInfo, file) parse <resources>\map.txt / map_LMP.txt (appends): per '#' line (CTextManager::SeekSection + ReadLines): #allod <name> + 5 lines -> 0x2C allod record (+0x18 CDWordArray of zone indices); #zone <name> <allod> <... |
| 0x0045A380 | 3722 | AppSettings::WriteToIni | (ini path): writes every setting back to an ini file (counterpart of ReadFromIni; no callers) |
| 0x0045C560 | 3650 | AppSettings::WriteToRegistry | (subkey): writes the same value set back to HKCU\Software\Nival Interactive\EvilIslands (called at exit from main) |
| 0x00458870 | 1318 | AppSettings::AppSettings | constructor of the settings object (global g_0x79BC70, 0x26C bytes): ini section names +0..+0x1C ('Statistics Output Settings', 'Game Specific Settings', 'Terrain Settings', 'General Settings', 'Camera Settings', 'Shadow Settings'... |
| 0x00457970 | 917 | App_HandleInputMessage | (msg, wParam, lParam): CInterface::FlushCloseRequest before and after; 0x200 WM_MOUSEMOVE -> CInterface::OnMouseMove(wParam, x, y), 0x201/0x202 L down/up, 0x203 L dblclk, 0x204/0x205 R down/up, 0x206 R dblclk, 0x20A wheel (delta =... |
| 0x00458DA0 | 857 | AppSettings::SetDefaults | re-applies a second (older) default set: 640x480x16, audio off, antialiasing 1, texture quality 3, shadows off, LightingFreq 1, fog 33/66/2, camera height -100..1000, distance -100..100 limits -50..80, pitch 0..90, border scroll 2... |
| 0x0045E310 | 853 | App_ProbeDirectX | returns the DirectX level found: LoadLibrary DDRAW.DLL + DirectDrawCreate (0 on failure), QI IDirectDraw2 (0x100 fail), DINPUT.DLL DirectInputCreateA (0x200), SetCooperativeLevel / CreateSurface (0), QI IDirectDraw4 (0x300), IDire... |
| 0x00457D40 | 705 | App_WndProc | WM_PAINT (0xF): BeginPaint/EndPaint only when GetUpdateRect; WM_DESTROY (2): PostQuitMessage when g_0x784751 (quit allowed); WM_ACTIVATE (6): minimized (HIWORD) -> g_0x79BC5D = 1 / g_0x79BC5C = 0 (inactive); WA_INACTIVE -> inactiv... |
| 0x00458270 | 613 | Movies_PlayIniList | (section, restoreBefore, restoreAfter, flag, arg): reads config\movie.ini key 'movies' of the given section (comma separated names) and plays Movies\<name> with Movie_PlayFile; before / after each file optionally clears the back b... |
| 0x00456140 | 537 | App_LoadMapAndScript | startup map: zone name = -mpr argument g_0x79BA10 or AppSettings DefaultMapName (g_0x79BEC8); when the .mob argument g_0x79BA14 is empty -> no map (LobbySession_ReleaseActive2 + Game_SessionTick: the game starts in the menu flow);... |
| 0x00458010 | 527 | App_RenderFrame | (present, drawUI): only when the window is active (g_0x79BC5C) and a screen is open (g_0x784750, else Sleep 50); I3DFigureImpl::BeginFrame -> CInterface::Render3D (top screen slot 20 inside the scene + ScreenFade) -> EndFrame; dra... |

## 0x461000 - 0x463000: CHoover + CInventory + keyboard.ini

68 functions, group `client`.

| addr | size | name | purpose |
|---|---|---|---|
| 0x00461CB0 | 1256 | CHoover::CollectUnitLoot | (world, unit, out items CList, out proto-drop CList, unused, out money): 0 when flag 0x100000 (looted); creates the unit proto's drop names (+0xe8 vector, CItemManager::CreateItemByName) into both lists; money = rand(proto +0xd0..... |
| 0x004626E0 | 560 | HashSetBuckets_Reserve | (this = STLport hashtable {buckets begin +4, end +8, eos +0xc, count +0x10}, hint): _M_initialize_buckets: n = first prime >= hint from the table 0x73B914 (binary search over 28 entries; 0xFFFFFFFB past the end), buckets.reserve(n... |
| 0x00461980 | 515 | StringIntMapBuckets_ctorN | STL: vector<node*>(n) bucket vector ctor (pool refill inlined); StringIntMap_Rehash = the NamedFloatMap / string -> int maps (NamedFloatMap_Set / Read, StringIntMap_GetOrInsert) |
| 0x00462C10 | 465 | KeyBindings_Load | fills the 88 key records g_0x7857B0 (0x48 each: +0 scancode, +4 static name '1'..'KP_STAR'..'DEL', +8 GetKeyNameText text for the options screen), zeroes the scancode -> action table g_0x79BF70[0x200] and reads <saves root>\config... |
| 0x00461750 | 382 | StringIntMap_Rehash | (this = STLport hashtable keyed by C strings, hint): STLport resize: when hint > bucket count, new count = first prime >= hint from the table 0x73B914..0x73B984 (0xFFFFFFFB past the end); if larger, allocates a zeroed bucket vecto... |
| 0x00462DF0 | 373 | KeyBindings_Save | writes <saves root>\config\keyboard.ini: one '%s %s\n' line (key name, action name) for every scancode of g_0x79BF70 with a non-zero action that has a key record |
| 0x00462960 | 334 | DwordVector_InsertRealloc7 | STL: vector<node*>::insert(pos, n, value) reallocating path of HashSetBuckets_Reserve = the bucket vector of the CInventory object hash set +0x50 (CInventory::ctor, 100 buckets) |
| 0x00461260 | 255 | NamedIntMap_Write | (map, stream): writes count + {name, int} pairs (CParty::Write). Was StringIntMap_Write |
| 0x00462420 | 241 | CInventory::dtorBody | (this): vt = vt_CInventory; STLport hashtable at +0x4c (buckets +0x50..+0x58, count +0x5c): every 0xc-byte node PoolFree(node, 0xc), buckets nulled, count = 0, bucket vector returned to the size-class pool g_0x79B224 (or Free_Wrap... |
| 0x004613C0 | 233 | StringIntMap_GetOrInsert | (this = STLport hash map CString -> dword, CString key*) -> &value: copies the key, StringIntMap_Rehash(count +0x10 + 1), hash = String_Hash5 % bucket count, strcmp along the chain; when absent a node from StringIntMapNode_Create(... |
| 0x004615B0 | 202 | StringIntMapNode_Create | (pair {CString key, dword value}*) -> node: takes a 0x10-byte node from the STLport free list g_0x79B228 (refilled by stlport_node_alloc_chunk_alloc(0x10) when empty), next = 0, CString copy of the key at +4, value at +8 |
| 0x00462AF0 | 201 | DwordBuffer_AllocCopy14 | STL: allocate n dwords (size pool / new) and copy a range in; HashSetBuckets_Reserve = the bucket vector of the CInventory object hash set +0x50 (CInventory::ctor) |
| 0x00461190 | 193 | Stream_ReadCString | reads a NUL-terminated string from a stream (stream->vt[1](ptr,1) per char) into a new std::string (STLport); returns it |
| 0x00462540 | 186 | CInventory::Done | slot 3: releases (vt[4]) every object of the hash set +0x4c.. then CItem::Done |

## 0x463000 - 0x466000: database.res loader (databases)

56 functions, group `data`.

| addr | size | name | purpose |
|---|---|---|---|
| 0x00463390 | 841 | LeversDb::Load | (this = g_0x79C1A0, res dir): opens <dir>database.res (CMappedResource; "databases.res" only in the error text), reads "Levers.ldb" into a CMemStream, tagged archive (Archive_Open version 1): tag 1 = the LeverPrototypes block (Lev... |
| 0x00465520 | 690 | LeverRecordVector_InsertN | STL: vector<LeverRecord 0x2c>::insert(pos, n, value) in-place path (CString copies via operator=, UninitCopy / UninitFill), grow path 0x465800; LeverRecordVector_Resize (Levers.ldb load) |
| 0x00463E80 | 580 | DwordVector_Reserve | STL: vector<node*>::reserve(n) of the bucket vector +4 of the string-keyed hash maps built by StringHashMap_ctor100 (LeversDb, PerksDb, SpellsDb, ItemsDb tables, QuestsDb, ObjPartyRecord, AddrBook); pool g_0x79B224 up to 0x80 byte... |
| 0x00464210 | 580 | DwordVector_Reserve2 | STL: vector<node*>::reserve(n) of the bucket vector of LeversDb +0xc leverMap (LeversDb::ctor, 101 buckets); DwordBuffer_AllocCopy9 |
| 0x00465310 | 515 | DwordVector_ctorN2 | STL: vector<node*>(n) bucket vector ctor (pool refill inlined); HashMapStrDword_Rehash3 = LeversDb leverMap +0xc (NameMap_FindOrInsert) and StringHash_FindOrInsert3 |
| 0x00465800 | 480 | LeverRecordVector_InsertRealloc | vector<LeverRecord>::insert with reallocation (grow by max(n, size)) |
| 0x004637C0 | 432 | StlList18_ctor | (this, allocator byte*): STLport list ctor for 0x18-byte nodes: takes the sentinel from the free list g_0x79B22C; when empty the node-allocator chunk refill is inlined (20 nodes from the start / end free pointers g_0x79B268 / g_0x... |
| 0x00463AA0 | 426 | StlList10_ctor | (this, allocator byte*): STLport list ctor for 0x10-byte nodes: sentinel from the free list g_0x79B228 (inlined chunk refill of 20 nodes, 0x140 bytes, as in StlList18_ctor), linked to itself; returns this |
| 0x00465EE0 | 385 | LightningNoise_AddBand | (this = table of 400 floats, i0, i1, amp0, amp1) ret 0x10: n = i1 - i0 + 1; for each harmonic i in [i0, i1) (i1 excluded): a = amp0/n + (i - i0)/n * (amp1/n - amp0/n), two ISAAC words from g_0x7A9D40 (Isaac_Generate refill inline)... |
| 0x00463120 | 376 | LeversDb::dtor | frees the name map nodes (0x14: next, std::string key, LeverRecord*) + buckets and the vector<LeverRecord 0x2c> |
| 0x00464F30 | 348 | HashMapStrDword_Rehash3 | (this = STLport hashtable {buckets +4..+0xc}, hint): resize: new count = first prime >= hint (table 0x73B914, 0xFFFFFFFB past the end); when larger, a zeroed bucket vector, every node re-linked by String_Hash(key +4) % new count, ... |
| 0x00464910 | 334 | DwordVector_InsertRealloc | STL: vector<node*>::insert(pos, n, value) reallocating path (buffer max(2 size, size + n), DwordRange_Copy both halves); DwordVector_InsertN (StringHashMap_ctor100 bucket fill) and ArchiveTagHash_Rehash (archive tag-name hash +0x5... |
| 0x00464B70 | 334 | DwordVector_InsertRealloc2 | STL: vector<node*>::insert(pos, n, value) reallocating path behind DwordVector_InsertN2 = the 101-bucket fill of LeversDb leverMap +0xc in LeversDb::ctor |
| 0x004645A0 | 331 | Archive_dtor | tagged archive dtor (45 callers): clears list +0x70 (0xc nodes), hash +0x60, list +0x50 (0x18 nodes), frees the two CMemStream buffers (+0x40, +0x18; vt 0x73BD90 -> base 0x73BDB0) |

## 0x466000 - 0x469000: CLight + CLightning (lightning.dat)

47 functions, group `effects`.

| addr | size | name | purpose |
|---|---|---|---|
| 0x004680D0 | 1454 | CLightning::Init | (from, to, type, demon): +0x50C type; +0x510 amplitude 0.3 (type <= 0) / 0.05 (> 0, or //dz/ - 30/ < 0.01); main segment +0x5C (LightningSegment_Init, seeds scale 1.5 / 2 / 1 for type > 0 / 0 / < 0, seed time (g_0x7AF61C + g_0x7AF... |
| 0x004662C0 | 1314 | LightningSegment::EvalNoise | (this = LightningSegment (main +0x5C or a branch), time, out dx[count], out dy[count], scale): per axis a ring of 3 seeds (+0x30.. / +0x60..: 2 table indices ISAAC & 15 + 2 weights U[-1,1) normalised x scale), shifted once per who... |
| 0x00467590 | 1045 | CLightning::RenderGlow | (ctx, pos, unused, size): 8 particle quads stacked from pos towards the camera g_0x79B2C4 (quad i at pos + dir * size * i/8, size * sqrt(1 - (i/8)^2)), colour 0xA0FFFFFF, frame (world +0x64 + round(15 pos.x) + 2i) % 15 of a 4x4 at... |
| 0x00467AD0 | 821 | CLightning::Render | (this, renderer): binds texture +0x1c (vt slot 0x2c), GetWorld, Update when type >= 0; type >= 0: DrawTexturedPolyline (vt slot 0x40) of the main points +0xf0 (width = /dir/ * count * 0.6, random frame 0..3 * 0.25) and the 6 branc... |
| 0x00466820 | 713 | LightningSegment_Init | (this = segment, from xyz, to xyz): origin +0, dir +0x24 = to - from, side +0xc / up +0x18 = orthonormal vectors scaled to min(len, 15), segment count 4..400 (len / 4), dir /= count, point CArray +0x94 SetSize(count) |
| 0x00467E10 | 689 | CLightning::InitRandom | (this, from, to): main segment +0x5c; type 0: 6 branches between random jittered copies of from/to (±5% of the length); else 6 branches from lerp(from, to, +0x4f4[i]) to `to`; type < 0: Update; ComputeBounds |
| 0x004672E0 | 676 | CLightning::Update | (this): UpdatePoints of the main segment +0x5c (scale 1.5 / 1 / 2 by type +0x50c), for type != 0 moves the 6 branch segments (+0x104, 0xa8 each) to a sine-animated offset along the main bolt at fraction +0x4f4[i] (frequencies g_0x... |
| 0x00468A00 | 530 | Lightning_LoadNoiseTables | reads 'gfx.res\lightning.dat' (CMappedResource) into g_0x79CE78: 32 tables x 400 floats (0x640 bytes each, 0xC800 bytes up to 0x7A9678) = the precomputed harmonic noise bands CLightning::EvalNoise blends; FatalError 'Unable to ope... |
| 0x00466AF0 | 483 | dead_466AF0 | unregistered code with no call, jump or pointer anywhere in the image (gaps.py 2026-11-06) |
| 0x00468DA0 | 423 | CArray8::Serialize | slot 2 of vt 0x73C584 (this = MFC CArray of 8-byte elements, CArchive*): loading: ReadCount and SetSize inlined (new elements zero-filled, growth count/8 clamped 4..0x400); storing: WriteCount; then count*8 raw bytes through CArch... |
| 0x00467150 | 391 | CLightning::ComputeBounds | (this): min/max +0x38/+0x44 over the main segment +0x5c and 6 sub segments at +0x104 (0xa8 each); centre +0x28, half extents, radius +0x34, +0x24 = 3 |
| 0x00466FD0 | 381 | CLightning::ExtendBounds | (this, segment): for each 16-byte point of the segment (+0x94, count +0x98) widens the box min +0x38..+0x40 / max +0x44..+0x4c (branchless min / max); CLightning::ComputeBounds |
| 0x00466CE0 | 377 | CLightning::UpdatePoints | (this, time, scale): EvalNoise into g_0x79C1F8 / g_0x79C838, then point i (+0x94, 16 bytes) = origin +0 + i * dir +0x24 + dx * side +0xc + dy * up +0x18 (drift-corrected) |
| 0x00468680 | 266 | CLightningManager::CreateByKind | slot 14 factory: operator new(0x514) + inline CLightning ctor: CObject base (vt 0x73B8E0, id -1), bounds / segments zeroed, the 6 LightningSegment branches +0x104 constructed (__ehvec), vt_CLightning, type +0x50c = 1.0, amplitude ... |

## 0x469000 - 0x470000: .mob object descriptors (CObjectDesc / CUnitDesc / CLightDesc / CSoundDesc / CParticleDesc / torch / lever / trap) + 3DS figure builder

154 functions, group `app`.

| addr | size | name | purpose |
|---|---|---|---|
| 0x0046F1F0 | 10397 | Scene3ds_BuildFigures | (this, 3DS scene*, out): editor leftover: converts a parsed .3ds scene into CFigure arrays (0x1c0 each); not reached by the game DEAD CODE: no call and no pointer anywhere in the file (byte search 2026-11-03). |
| 0x00469F70 | 1303 | dead_469F70 | unregistered code with no call, jump or pointer anywhere in the image (gaps.py 2026-11-06) |
| 0x00469090 | 831 | dead_469090 | unregistered code with no call, jump or pointer anywhere in the image (gaps.py 2026-11-06) |
| 0x0046AB20 | 824 | CObjectDesc::OnChunk | slot 0 (ChunkFile reader, OBJECT 0xB001 children): 0xB002 NID -> +0x18, B003 OBJTYPE -> +0x50, B004 OBJNAME -> +0x3C, B006 OBJMODEL -> +0x54, B007 -> +0x70, B008 -> +0x74, B009 OBJPOSITION -> +0x20, B00A OBJROTATION quaternion -> ... |
| 0x0046CBC0 | 792 | MobLogicSlot_OnChunk | (children of UNIT_LOGIC 0xBBBC0000 into the 0x4c MobLogicSlot): 0xBBBD0000 appends a 0x20 point {pos, CArray of 0x14 look-at records} and reads it (MobLogicPoint_OnChunk); 0001 byte (legacy aggressive: AggrMode -> 0), 0002 byte +0... |
| 0x0046B7D0 | 774 | CMagicTrapDesc::OnChunk | slot 0 (MAGIC_TRAP 0xBBAB0000 children, as the explicit compares in MobFile_ParseChunks 0x481F00 and this switch read them): 0xBBAB0001 diplomacy dword -> +0x8C, 0002 spell string -> +0x90, 0003 count + N x 12 bytes area circles {... |
| 0x0046C900 | 677 | MobLogicSlot_Write | (ChunkFile, slot +0x18): UNIT_LOGIC chunk: 0xBBBC000B alarm condition, 000C help radius, 0002 cyclic, 000E aggression mode, 000D always active, 0003 model, 0004 guard radius, 000A wait, 0005 guard point, 0007 use, 0006 num alarm; ... |
| 0x0046E680 | 623 | MobLogicPoints_Copy | (this = CArray of 0x20 point records, src): destroys the current points, SetSize to the source count, copy-constructs each (pos + look-at CArray). Was UnitLogicRecords_Copy |
| 0x0046BAD8 | 615 | sub_46BAD8 | unregistered function, reached by call / pointer from 0x6B813 (gaps.py 2026-11-06) |
| 0x0046A710 | 519 | CObjectDesc::CopyFrom | (src DescRecordB-like record): copies model / prototype / complection / name strings (+4..+0x48 of the source, CStrings +0x3c / +0x40) into the matching descriptor fields (editor path) |
| 0x0046D080 | 513 | CUnitDesc::OnChunk | slot 0 (UNIT 0xBBBB0000 children): 0xBBBC0000 UNIT_LOGIC -> the next 0x4c slot (+0x90 + (+0x8c)++ * 0x4c; +0x28 = 0, +0x48 = 4 before MobLogicSlot_OnChunk, 4 -> 0 after); 0xBBBB0002 UNIT_PROTONAME -> +0x20c; 0004 UNIT_STATS -> 0xa... |
| 0x0046A940 | 465 | CObjectDesc::WriteFields | slot 11 (shared by object / torch / lever / trap / unit): writes 0xB00D parts +0x78, B011 byte +0x6c, B002 id, B003 type +0x50, B004 name +0x3c, B006 model +0x54, B00E prototype +0x58, B007 +0x70, B008 +0x74, B00F comment +0x40, B... |
| 0x004696D0 | 454 | DescRecordA::operator_assign | (this, src 0x74 record): copies the CStrings +0..+0xc and the dwords +0x10..+0x20, then CStringArray::SetSize of +0x24 to src count +0x2c, of +0x38 to src count +0x40 and of +0x60 ALSO to src count +0x40, and copies the elements o... |
| 0x0046E190 | 441 | CSoundDesc::OnChunk | slot 0 (children of SOUND 0xCC01): 0xCC02 id -> +0x18, CC03 pos -> +0x20, CC04 range -> +0x68, CC05 name -> +0x3c, CC06 min -> +0x74, CC07 max -> +0x78, CC08 comment -> +0x40, CC0A wave file list -> CStringArray +0x54 (cleared fir... |

## 0x470000 - 0x474000: AI state names + DirectX error text

5 functions, group `misc`.

| addr | size | name | purpose |
|---|---|---|---|
| 0x00471B10 | 12595 | Scene3ds_ReadBinaryScene | (this = scene header out, stream, model out): reads a pre-converted binary scene with the stream slot 13 Read: object count + material count, per object a 0x2c-byte header, two name strings, vertex / normal arrays (12-byte), 0x1c ... |
| 0x00471A90 | 25 | Vec3_Ctor3f | (this, x, y, z): writes the three floats (3ds material colour triples in Scene3ds_BuildFigures / ConvertToModel) |
| 0x00471AB0 | 24 | Material3ds::SetDiffuse | stores a float triple at +0x20 of the 0x9c 3ds material record: the diffuse colour (chunk 0xA020, the second triple read), the D3DMATERIAL7 layout of CMaterial (+0x20 diffuse, +0x30 ambient, +0x40 specular); dead code (the 3ds imp... |
| 0x00471AD0 | 24 | Material3ds::SetAmbient | stores a float triple at +0x30 of the 0x9c 3ds material record: the ambient colour (chunk 0xA010, the first triple read; CMaterial layout); dead code |
| 0x00471AF0 | 24 | Material3ds::SetSpecular | stores a float triple at +0x40 of the 0x9c 3ds material record: the specular colour (chunk 0xA030, the third triple read; CMaterial layout); dead code |

## 0x474000 - 0x47E000: MFC CArray/CMap instantiations, IStream/CMemStream/CFileStream, MsgStream family (file / peer / TCP), CTcpNetDriver, Huffman coder, unit prototype apply

289 functions, group `net`.

| addr | size | name | purpose |
|---|---|---|---|
| 0x0047B1F0 | 1064 | PtrArray_Diff | (ecx old, edx new, outAdded, outRemoved, outKept): PtrArray_SortUnique(new); sizes the three outputs to max(count old, count new) (aligned SetSize inlined); merges the two sorted arrays: equal -> outKept, only in new -> outAdded w... |
| 0x00477710 | 994 | dead_477710 | unregistered code with no call, jump or pointer anywhere in the image (gaps.py 2026-11-06) |
| 0x00477B00 | 829 | MobUnitStats_ApplyTo | (this = unit prototype record, unit): empty prototype -> only adds the named spells (+0xc4 list, 0x531180) and perks (+0xb0, 0x531220); otherwise resets the unit stats (0x5315B0) and copies hp / mana / stats (+0..+0x50) into the u... |
| 0x0047ADF0 | 809 | PtrArray_SortUnique | (ecx = 32-byte aligned pointer array {data, count, cap, grow}): when count > 1: sizes the static scratch array g_0x7A9738 (lazy init + atexit 0x47B1D0, aligned grow), LSD radix sort of the dword keys by bytes 0, 1, 2 (counting sor... |
| 0x0047DCA0 | 785 | DeltaWriter::WriteBytes | (this = DeltaCodec, data, n): writes n bytes against both base snapshots: base A (+0 stream, bits to +0x50) and base B (+0x28, bits to +0x78): no snapshot at the base position -> a 0 bit then per dword 0 bit + 4 raw bytes, rest ra... |
| 0x0047BC10 | 620 | dead_47BC10 | unregistered code with no call, jump or pointer anywhere in the image (gaps.py 2026-11-06) |
| 0x0047C680 | 607 | TcpConnectThread_dead | thread body of ConnectAsync (socket, split host:port, connect); NO CALLERS (dead) |
| 0x00476430 | 513 | CMapArray_Serialize | slot 2 of vt 0x73C80C (this = MFC CArray of 0x1c-byte CMaps, CArchive*): loading: ReadCount and SetSize inlined (CMapArray_ConstructN for new elements, vt[1](0) for removed ones); storing: WriteCount; then count*0x1c raw bytes (th... |
| 0x004761C0 | 501 | CMap_Serialize | slot 2 of vt 0x73C818 (this = MFC CMap<DWORD, DWORD>, CArchive*): loading: ReadCount, then per entry key and value (4 bytes each through CArchive Read 0x708832) inserted with the operator[] code inlined; storing: WriteCount(count ... |
| 0x00479E30 | 499 | CTcpNetDriver::SyncPeers | (this = RefListPair CWorld +0x60, LobbyServer g_0x79B5DC): for every NetPeer of the server's peer list +0x28 without a PeerMsgStream in +8: when the peer's IN stream +0x1148 holds bytes (GameBuffer::CommitCursor first) AND LobbySe... |
| 0x00475310 | 488 | CArray1C_Serialize | slot 2 of vt 0x73C848 (this = MFC CArray of 0x1c-byte POD elements, CArchive*): ReadCount + SetSize inlined (zero-filled new elements) / WriteCount, then count*0x1c raw bytes |
| 0x0047CFA0 | 453 | CTcpNetDriver::AcceptNewClient_dead | accept() on a listening socket + TcpMsgStream; NO CALLERS (no listening socket is ever created): dead TCP server path |
| 0x0047D190 | 445 | dead_47D190 | unregistered code with no call, jump or pointer anywhere in the image (gaps.py 2026-11-06) |
| 0x00475130 | 435 | CArray0C_Serialize2 | slot 2 of vt 0x73C854: same code as CArray0C_Serialize (0xc-byte POD elements zero-filled on grow, raw count*0xc bytes) |

## 0x47E000 - 0x481000: net delta codec: DeltaWriter/DeltaReader (16-bit quantized fields vs two base snapshots), DeltaCodecSet, GameException, FatalError, DDraw error text, AnimKeyToString

45 functions, group `net`.

| addr | size | name | purpose |
|---|---|---|---|
| 0x004803E0 | 1939 | AnimKeyToString | (ecx key) -> char*: debug text of an animation key built in an STLport string then copied (no length check) into the static buffer g_0x7A979C: "(0x%8X) " prefix, state bits 15-17 (0x38000: Neutral / Attack / Warry / Lie), action b... |
| 0x00480B80 | 1390 | DDrawErrorString | (ecx hr) -> const char*: binary-search switch over 149 DirectDraw / Direct3D HRESULTs (DDERR_*, D3DERR_*, E_* values) returning the English description; 0 -> "No error.", anything else -> "Unrecognized error value."; FatalErrorHr,... |
| 0x0047F4D0 | 785 | DeltaWriter_WriteVec3F16 | (this = DeltaCodec, float[3]): packs three floats into 16-bit halves and delta-writes them (10 callers) |
| 0x0047F7F0 | 586 | DeltaReader_ReadVec3F16 | (out float[3]): reads three delta-coded 16-bit halves (DeltaReader_ReadBytes for the 2nd and 3rd) and expands them to floats |
| 0x0047E3A0 | 571 | DeltaWriter_WritePos3D | (this = DeltaCodec, float[3]): quantizes x, y as (v + 16) * 120.46875 and z as (v + 32) * 341.32812 to 16 bits and delta-writes them against both base streams (13 callers) |
| 0x0047EBF0 | 518 | DeltaWriter_WriteVec2Q64 | (this = DeltaCodec, float[2]): quantizes both as (v + 32) * 60.234375 to 16 bits and delta-writes them |
| 0x0047E740 | 488 | DeltaWriter_WriteBytes | (base CMemStream, data, n, updateBase, bitstream): when the base stream has no snapshot at its position: raw-writes and appends to the base; else delta-writes against the snapshot bytes, overwrites them with data when updateBase, ... |
| 0x0047F1E0 | 488 | DeltaWriter_WriteFloat16 | (this = DeltaCodec, float): 16-bit half {sign, 7-bit exponent (biased 0xc0ff trick), 8-bit mantissa rounded}; written against base A (+0 / +0x50) and base B (+0x28 / +0x78): no snapshot -> 0 bit + 2 raw bytes and appended to the b... |
| 0x0047E930 | 429 | DeltaReader_ReadPos3D | (out float[3]): reads x, y as delta-coded 16-bit values * 0.008300908 - 16 (range -16..16) and z via DeltaReader_ReadBytes as v * 0.0029297322 - 32 (range -32..32) |
| 0x0047EF80 | 391 | DeltaWriter_WriteFloatRange | (this = DeltaCodec, value, min, max): quantizes (value - min) * 65535 / (max - min) to 16 bits and delta-writes it against both base streams (+0 with bit stream +0x50, +0x28 with +0x78): unchanged -> bit 1, changed -> bit 0 + 2 by... |
| 0x0047FE90 | 388 | DeltaCodecSet::Pack | (this): state 1: mask +0x14 /= changed +0x18, +0x16 = changed; packed stream A +0x24 = u16 mask +0x14 + the +0x50 delta of every slot in it; packed stream B +0x4c = u16 +0x16 + the +0x78 delta of every slot in it; state 0 clears e... |
| 0x0047EE00 | 376 | DeltaReader_ReadVec2Q64 | (out float[2]): reads two delta-coded 16-bit values from the active base stream g_0x7A9794 / bit stream g_0x7A9798 (unchanged bit -> value from the base stream +0x18 buffer, else 2 bytes from the bit stream, stored back into this ... |
| 0x0047FBF0 | 323 | DeltaCodecSet::BeginSlot | (this = DeltaCodecSet, slot): returns 0 when bit slot of the mask +0x18 is clear (field not sent); codec = +4 + slot * 0xa0; mode +0x20: 0 (writer, full) -> rewind bases, empty deltas, g_0x7A9784 = g_0x7A9780 = 1; 1 (writer, delta... |
| 0x0047E000 | 308 | DeltaReader::Read | (dst, n): reads n bytes against the active base g_0x7A9794 with bits from g_0x7A9798 (CMemStream bit reader +0x10 mask / +0xc byte): 1 bit = copy n bytes from the base; 0 bit = per dword 1 bit copy / 0 bit 4 raw bytes, rest raw, a... |

## 0x481000 - 0x484000: mission/zone file loader (MobFile) + std::map<string,list> instantiations

71 functions, group `renderer`.

| addr | size | name | purpose |
|---|---|---|---|
| 0x00481660 | 2029 | MobFile_Load | (ChunkFile, noScript) from LoadMapObjects: MobFile_ParseChunks; parse result 1 -> PlayerRelations::Set(g_0x7C2BC4, i, j, m[i][j], 1) 32x32; AppSettings AutomaticQuantization (g_0x79BCF4) -> morphs of kind-1 objects (types not 0x32... |
| 0x00481F00 | 1327 | MobFile_ParseChunks | (ChunkFile, out descriptor hash keyed by NID, out 32x32 matrix + names, out script CString) -> 1 unless a 0xD000 marker came last: 0xACCEECCA plain script / 0xACCEECCB scrambled (LCG, 0x42C650); 0xABD0 wind {vec3 dir, strength -> ... |
| 0x004810F0 | 1064 | sub_4810F0 | unregistered function, reached by call / pointer from 0x80C7A (gaps.py 2026-11-06) |
| 0x00482EF0 | 703 | RbMap_Insert_482EF0 | STL: std::map<string-range,list> insert with hint: handles begin/end cases, InsertUnique fallback, rebalance |
| 0x00483950 | 580 | DwordVector_Reserve_483950 | STL: vector<uint>::reserve(n) |
| 0x004827D0 | 432 | List24_Ctor_4827D0 | STL: construct std::list<0x18-byte>: allocate sentinel node from pool g_0x79B22C |
| 0x00483760 | 420 | RbMap_InsertUnique_483760 | STL: std::map insert_unique: descend comparing string ranges (StringRange_Compare), then InsertAt unless the key exists |
| 0x004835D0 | 389 | RbMap_InsertAt_4835D0 | STL: std::map<string-range,...> _M_insert at a given parent: creates node (RbNode_New), links left/right, rebalances (rotate left/right) |
| 0x004831B0 | 387 | Vec3Vector_InsertRealloc_4831B0 | STL: vector<12-byte> insert grow path using Vec3Range_Copy / Vec3Range_FillN |
| 0x00483BA0 | 314 | DwordVector_InsertN_483BA0 | STL: vector<uint>::insert(pos,n,value), grow path 0x484180 |
| 0x00482AA0 | 286 | String_CtorFromRange | (this = pair {STLport string +0..+8, list +0xc}, src string*, src list*): pair copy ctor of MobFile_Load's map value: string = copy of [src.start, src.finish) (PoolAlloc(len+1), Bytes_Copy, NUL; length overflow -> String_ThrowLeng... |
| 0x00483DF0 | 235 | RbNode_New_483DF0 | STL: allocate a 0x20-byte tree node (pool g_0x79B230) and copy-construct key (String_CtorRange) and value list (0x483F00) |
| 0x00482640 | 212 | NodeAlloc32_482640 | STL: allocate 0x20-byte node from pool g_0x79B230 (refill 20 via chunk allocator) |
| 0x004833B0 | 202 | HashMap_Ctor_4833B0 | STL: construct hash map object: sentinel list, bucket vector of NextPrime(100) |

## 0x484000 - 0x499000: particle effects (CParticle, 'Particle effect ...')

200 functions, group `effects`.

| addr | size | name | purpose |
|---|---|---|---|
| 0x00485540 | 12920 | CParticle::Init | slot 2 (type +0x10 = 0x2000 + k): Reset, bounds = empty, columns +0xe0 = 4, probability +0xcc = 100, max live +0xd8 = 500, frames +0xdc = 16, flag 1 set; then the per-type constant table (texture name, spawn box +0xf0..+0x104, siz... |
| 0x00490130 | 2051 | ParticleEmit_Modifier | (this, particle, arg): colour -1, +0x50 = +0xec, +0x4c = +0xe4; "Particle effect Modifier without carrier" (FatalError without +0x94); record array +0x154 sized by the carrier extent; particles orbiting the carrier (0x40F560 rotat... |
| 0x00496EB0 | 2045 | CParticle::Update | slot 6 -> bool (alive): copies g_0x7AF5DC..+0xc (world wind / time block) into +0x140, resets the bounds, Attach(0) when the carrier has flag 1 (dying); +0xa4 = world pos delta since the last tick, +0x98 = world pos, +0xc8 = /delt... |
| 0x004923B0 | 1986 | ParticleEmit_Weather | (this = CParticle, record) invoke-lightning impact 0x2030: 10 records = impact points on a disk of radius 0.3 x size around +0x98 (first at the centre) at client terrain height + 0.3 with an outward-up direction; first 20 emission... |
| 0x0048E070 | 1871 | ParticleEmit_Shield | (this = CParticle, record, index) emitter of the protection / antimagic auras 0x2015..0x2017 and 0x201A (not the link 0x2014): needs a carrier ('Particle effect Link without carrier' trace, copy-pasted string); index 0: shell radi... |
| 0x004918E0 | 1864 | ParticleEmit_2027Casting | (this = CParticle, ParticleRecord) -> 1 / 0, types 0x2027..0x202A (converging casting glow). First call (no record): needs a carrier (else Trace_Stub_A, return 0); R = unit body radius (vt[60] for type id 0x50) or 0.8 x GetCarrier... |
| 0x00489320 | 1827 | ParticleEmit_200B | emit callback of type 0x200B (FireSmoke texture; ecx system, record): first call: no carrier -> Trace_Stub_A, returns 0; else 3 emitter records, GetCarrierExtent (centre, r): each record = ring point k (angle k * 120 deg in +0x10,... |
| 0x00484C00 | 1600 | CParticle::GetCarrierOffset | (out xyz, carrier): without flag 0x10: the carrier position; units (type 0x50) by attach mode +0x13C: 0 CUnit::GetAttachPoint2 (the default attach point); 1..7 = FindPartByNameStart with one of six short part-name prefixes (1..6: ... |
| 0x004976B0 | 1575 | CParticle::Render | (this, I3DFigureImpl): nothing when the live count +0x1c0 is 0 or the bounds box +0x17c is frustum-culled (CViewFrustum::CullBox == 2); static buffers g_0x7A9BD0 (aligned {x,y,z,size} x 0x1000) / g_0x7A9BE0 ({colour, uv rect*} pai... |
| 0x00490C10 | 1540 | ParticleEmit_202BCasting | (this = CParticle, ParticleRecord) -> 1 / 0, types 0x202B..0x202E (textures Casting2 / Casting3 / Casting6 / Casting1; +0xec = ring point count 2 / 1 / 2 / 1). While fewer records than +0xec: needs a carrier (else Trace_Stub_A, re... |
| 0x0048B630 | 1456 | ParticleEmit_Healing | (this, particle): "Particle effect Healing without carrier" (FatalError 0x44C470 without +0x94); record array +0x154 sized round(+0x10c * 6 + 2) (0x497EB0), ring of particles around the carrier (GetCarrierExtent) rising upward |
| 0x0048D8D0 | 1368 | ParticleEmit_2014Link | emit callback of type 0x2014 link (ecx system, record): first call: 4 emitter records from the link vector D = +0x130: rec0 +0x18 = /D/, +0x1c = 1 / /D/, rec1 +0x18 = D / /D/, rec2 +0x18 = u = normalised (Dy, -Dx, 0) (fallback (Dy... |
| 0x0048C090 | 1278 | ParticleEmit_2002Blast | emit callback of type 0x2002 blast (ecx system, record): white; variant v = rand % 29 - 14; no live particles and record 0 +0x30 == 1 -> the centre flash (variant -1, at the world pos, frame 15, size 0); v >= 0 -> a streak particl... |
| 0x0048B080 | 1262 | ParticleEmit_GridFill | (this = CParticle) acid fog / poison cloud 0x2007 / 0x2008: only while the live list +0x1b8 is empty: one particle per cell of the grid -n..n (n = trunc(size +0x10c), spacing 1, jitter +-0.25) inside radius +0x10c: size 0.75..1.25... |

## 0x499000 - 0x4B7000: script API (GS* functions: GetObject*, KillScript) + colour/percentage parsers

593 functions, group `script`.

| addr | size | name | purpose |
|---|---|---|---|
| 0x004B6760 | 13477 | SSCScript::Run | (this = SSCScript {+0 program, +4 pc, +8 run counter, +0xc sleep start, +0x10 kill flag*, +0x14 SSCVars}, vm GSWorld*) -> 0 only when pc was already past the last command at entry, else 1 (also after running the last command, 0x4B... |
| 0x004B41E0 | 4822 | SSCCmd::EvalFloat | (this, vm GSWorld*, vars SSCVars* (local), out float*): SetParameters (local +0x54, global +0x68 from vm +0xa0), CalcParameters for every nested binding, then by id (0x45..0xe6, else FatalError "Function does not return float at S... |
| 0x004A7630 | 2147 | GSPlayers_CreateDefaults | (this = vector of CGSPlayer refs, fourPlayers): the vector is emptied; CGSPlayer objects (0x28c, CGSPlayer::ctor) are created with the index +8 = 1..5 (indices 1, 3, 4, 5 when fourPlayers), flags +0xc / +0xd, the float table +0x10... |
| 0x004A3CA0 | 1715 | Deque16_InsertMiddle | (this = deque, position, range first / last by value, n): STLport _M_insert_range_aux: elems_before = position - start; when it is below half the size the front grows (Deque16_ReserveFront, new start = start - n) and either the fi... |
| 0x004A0BA0 | 1589 | Chunk3ds_ReadMaterial | (this = {IStream*}, chunk, out materials CArray): a temporary CMaterial (0x9c); sub-chunks: 0xA000 MAT_NAME, 0xA010 / 0xA020 / 0xA030 ambient / diffuse / specular (colour sub-chunk 0x10 floats or 0x11 bytes, else printf "WARNING :... |
| 0x0049BEA0 | 1563 | PointSet_ClusterCentroids | (points {Vec3 begin, end}, out Vec3 vector, levels) fastcall: levels 0: one cluster with every point (weight 1); else each point is quantised (_ftol of the components x 127) into a 10 / 10 / 10-bit packed key counted in a hash set... |
| 0x0049F890 | 1396 | Node3ds_StepFrame | (this = 3DS keyframe node {+0x10 pos key index, +0x14 rot key index, +0x18 .. +0x2c velocity, +0x30 local quat, +0x40 world quat, +0x50 angle per frame, +0xac pos keys (0x14), +0xc0 rot keys (0x18), +0xfc per-frame records 0x28}, ... |
| 0x004A7080 | 1009 | CGSPlayer::InitAllowedLists | (this): shop stock +0x11c: every weapon / armor / quick proto whose shop mask +0x64 (items.idb "Shop 1..5" bits) has bit (player index +8 - 1), materials (mask +0x4c) with range 20..50; spells (mask +0x80) and perks (mask +0x20) i... |
| 0x004A1220 | 996 | Chunk3ds_ReadKeyframer | (this = {IStream*, +4 short}, chunk, out nodes CArray): +4 = 0; sub-chunks until the chunk end: 0xB002 OBJECT_NODE_TAG: a temporary 3DS node filled from 0xB010 NODE_HDR (Chunk3ds_ReadNodeHdr), 0xB013 PIVOT (Stream_ReadThreeDwords_... |
| 0x0049F150 | 863 | Isaac_Init | (ctx {+0 initialised, +4 counter, +8 results[256], +0x408 mm[256], +0x808 aa, +0x80c bb, +0x810 cc}) fastcall: only when +0 is 0: Isaac_SeedFromFiles fills results[] with 1 KB of a random file on C: (see 0x49F7B1); aa = bb = cc = ... |
| 0x004B5B00 | 856 | SSCCmd::EvalGroup | (this, vm, vars, out ObjRefCArray**): parameters bound and nested bindings evaluated; the result = a new group node appended to the temp pool +0x1b0 (GroupCList_AddTail); 0x59 GroupAdd = ObjRefCArray_Union, 0x5a GroupSub = Differe... |
| 0x0049A220 | 842 | OctaveNoise_FillBand | slot 1 of 0x7420D4 (this, out 256 x 256 bytes, step): steps 1..4 regenerate octave table 0..3 (+0x18 + 0 / 0x400 / 0x800 / 0xC00, vt[0]); steps 7..14 fill rows (step - 7) x 32 .. +32: each pixel = (64 o1 + 32 o2 + 16 o3 + 8 o4 - 7... |
| 0x0049A7A0 | 838 | dead_49A7A0 | unregistered code with no call, jump or pointer anywhere in the image (gaps.py 2026-11-06) |
| 0x004B5FE0 | 837 | SSCCmd::Read | (this, stream): function id +0, float slots (count then values), string slots (Stream_ReadCStringTo), object slot count (slots zeroed: refs are re-bound at run time), group slot count (zeroed), local bindings +0x58 and global bind... |

## 0x4B7000 - 0x4BB000: script API (cont.)

89 functions, group `script`.

| addr | size | name | purpose |
|---|---|---|---|
| 0x004BAAB0 | 825 | QuestObjectives_GenerateScript | (vm GSWorld*): script QFinish(): builds the source "DeclareScript Su%i()\n" per objective, then per objective i "Script Su%i(if(\n<cond>\n)then(\nKillScript()\nSu%i+1()\nGSSetVar(0,\"q.%s.%s.%i\",2)\nGSSetVar(0,\"q.%s.%s.%i+1\",1)... |
| 0x004B9C08 | 337 | sub_4B9C08 | unregistered function, reached by call / pointer from 0xB6870 (gaps.py 2026-11-06) |
| 0x004BAEC0 | 307 | ScriptParty_Resolve | fastcall (player, name CString*) -> CParty*: 0 for no player; a name "Store::Rest" is split at "::" (g_0x78BB18): name = Rest, store = Store (else store = ""); store equal to the PartyList current name g_0x7AA5D4 (+0x10) -> the pl... |
| 0x004BAE10 | 159 | QuestObjectives_Add | (kind = the QObj* command id 0xd7..0xdd, text CString*, value): appends {kind, text, value} to g_0x7AA608 (Run cases 0xd7 QObjArea(f), 0xd8 QObjSeeObject(s), 0xd9 QObjSeeUnit(s), 0xda QObjKillUnit(s), 0xdb QObjKillGroup(s), 0xdc Q... |
| 0x004BA730 | 130 | AiLogicAggression::Clone | slot 3 of vt 0x73D260 (AiLogicAggression, 0x18): new 0x18-byte object (refcount 0, vt 0x73D22C then 0x73D260): target ObjRef +8 shared (refcount ++), revenge byte +0xc, spell +0x10, chosen +0x14 copied. Was ObjHolder73D260::Clone |
| 0x004BA4A0 | 112 | AiLogicFollow::Clone | slot 3 of vt 0x73D1C4 (AiLogicFollow, 0xc): new 0xc-byte object (refcount 0, vt 0x73D15C then 0x73D1C4) holding the same target ObjRef +8 (32-bit refcount +4 ++). Was ObjHolder73D1C4::Clone |
| 0x004BA7C0 | 106 | AiLogicAggression::Reset | slot 2 of vt 0x73D260, fastcall: target +8 released and nulled, vt = 0x73BB68, then re-constructed: +8 = 0, +0xc = 0, +0x10 = -1, +0x14 = 0, vt = 0x73D260. CORRECTION: was ObjHolder73D260::dtor |
| 0x004BA510 | 101 | AiLogicFollow::Reset | slot 2 of vt 0x73D1C4, fastcall: the target +8 released (vt[4]) and nulled, vt = 0x73BB68, then re-constructed: +8 = 0, vt = 0x73D1C4. CORRECTION: was ObjHolder73D1C4::dtor |
| 0x004BAA40 | 99 | QuestObjectives_Reset | (quest name): script QStart(name): the global objective table g_0x7AA608 (CArray vt 0x73D46C of 0xc {kind, CString text, float value}, data g_0x7AA60C, count g_0x7AA610) emptied and the name stored for QFinish. Was TriggerTable_Re... |
| 0x004BA120 | 96 | ColorObj_SetRGB | (this, r, g, b): +0x14 = 0xff000000 / (r, g, b rounded * 255); vt[0](colour) |
| 0x004BA1C0 | 85 | AiLogicGuard::Ctor | (this, pos[3], radius, stayTime): vt 0x73D128 then GuardPoint_Init; kind 0. Script UMGuard / UMGuardEx. Was Class73D128::ctor |
| 0x004BA250 | 84 | AiLogicGuard::Clone | slot 3 of vt 0x73D128 (AiLogicGuard, 0x20): new 0x20-byte object (refcount +4 = 0, vt 0x73D15C then 0x73D128) with +8..+0x1c copied (point, radius, stayTime, failedSpots). Called through the vtable (Brain logic copies). Was Class7... |
| 0x004BA5B0 | 84 | AiLogicFollow::dtor | fastcall: vt = 0x73D1C4, the target +8 released (vt[4]) and nulled, vt = 0x73BB68 (base). Caller AiLogicFollow::DeletingDtor. Was ObjHolder73D1C4::dtor_base |
| 0x004BA8C0 | 84 | AiLogicAggression::dtor_base | vt 0x73D260 dtor body: releases the kept target ref +8 (vt[4] Release) and clears it, vt -> 0x73BB68; AiLogicAggression::DeletingDtor. Was ObjHolder73D260::dtor_base |

## 0x4BB000 - 0x4D6000: WorldScript compiler (lexer, #define, Duplicate script) + spell name table

455 functions, group `script`.

| addr | size | name | purpose |
|---|---|---|---|
| 0x004D23A0 | 3951 | Compiler_CompileText | (this = GSWorld, text CString* (rewritten: "#define name value" lines (strncmp at line start, split at '\n') removed, later lines get their plain-text replacement), startWorldScript) -> 1 / 0 (error text +0x1ec, code +0x1f0, line ... |
| 0x004CFBC0 | 3575 | Compiler_ParseCommand | (this = GSWorld in ECX, out SSCCmd, cursor (token node**), tokens, function id -> out +0, local SSCVars* (never 0 from the parsers), param letters "f s o g" (capital 'O' = output object: GlobalVars object only), singleExpr, script... |
| 0x004D41C0 | 3156 | Decompiler_WriteCommand | (this = GSWorld, out CString, cmd (by value), decl, ...): "<name>(" from GSFunctions_NameById, then the arguments in signature order: float slots as numbers, string slots quoted, object / group slots as the variable name found thr... |
| 0x004D1330 | 2553 | Compiler_ParseCommands | (this = GSWorld, program SSCProgram*, cursor, tokens, local SSCVars* or 0 = an empty local table) -> 1 / 0: statements until ')' with no separator (a ',' statement is error 3): a table COMMAND (record +0xc params != 0) via Compile... |
| 0x004D39D0 | 1986 | Decompiler_WriteVars | (mode, out CString, SSCVars, signature or 0, header flag): "name : float / string / object / group" declarations comma separated (in signature letter order when a signature is given, so parameters keep their declared order); names... |
| 0x004BF000 | 1340 | GSWorld::Tick | (this = GSWorld) when +0x1a8: 1. every running SSCScript of +0x15c from the head (newest first): SSCScript::Run, removed when it returns 0 (finished one tick earlier); 2. every active instance of +0x140 from the head: SSCVars::Pur... |
| 0x004D0BF0 | 1080 | Compiler_ParseConditions | (this = GSWorld, program, cursor, tokens, local vars) -> 1 / 0: calls written one after another until ')' with NO separator (a ',' token has no signature: error 9 like any non-F name): each token id must have an 'F' signature (GSF... |
| 0x004D53C0 | 1015 | GSWorld::Decompile | (this = GSWorld, out CString): rebuilds source text: "GlobalVars" + Decompiler_WriteVars on +0xa0, "DeclareScript <name>(<params>)" per declaration of +0x90 (foreign-module names skipped), "Script <name>(" with "  if" / "  then" b... |
| 0x004CF7A0 | 968 | Compiler_Tokenize | (this = GSWorld, text CString*, out token CList (vt 0x73D424, 0x10 nodes {next, prev, CString text, int line})) -> 1 / 0 (error 1 "End of file before '*/'"): whitespace (CRT isspace, miscalled _isdigit_like) skipped counting '\n';... |
| 0x004D1FE0 | 956 | Compiler_ParseVarDecls | (this = GSWorld, decl SSCScriptDecl*, cursor, tokens, qualify) -> 1 / 0: "name : type" pairs separated by "," until ")" (error 4 "Expected ',' or ')' after variable"); ':' = token 0x73 (error 2 "Wrong lexeme %s"); type word (case-... |
| 0x004C01A0 | 891 | Compiler_DeclareScript | (this = compiler / GSWorld, text "name(params)") -> 1 / 0 / error: the module-qualified name must not exist yet (GSWorld::FindDeclIndexByName; error 0xf "Duplicate script %s"); the text is tokenized (Compiler_Tokenize 0x4CF7A0 int... |
| 0x004BFCC0 | 860 | Compiler_CompileGlobalVars | (this = compiler / GSWorld, text (stack CString)) -> 1 / 0 / error: the text tokenized (Compiler_Tokenize), the declarations parsed (Compiler_ParseVarDecls into a temporary SSCScriptDecl) and appended to the global SSCVars +0xa0 (... |
| 0x004C3DC0 | 782 | SSCGroupBlock_Append | (this = group block {CArray of groups +0, names +0x14}, src block): groups SetSize(old + src count) inlined (Group_ConstructN / Group_DestructN), names SetSize(old + src names count); for i < src names count: the name copied, a te... |
| 0x004C09F0 | 776 | Compiler_CompileCommands | (this = compiler / GSWorld, script name or 0, program) -> 1 / 0 / error: the named declaration (module-qualified) looked up in +0x90 (0 = global); the text (stack CString) tokenized (Compiler_Tokenize into a CString CList) and the... |

## 0x4D6000 - 0x4D7000: CServerSpecificManager + CTorchObjectSpecific

36 functions, group `core`.

| addr | size | name | purpose |
|---|---|---|---|
| 0x004D6E90 | 652 | Triangle_ClipByPlane5 | (tri float[15], extraTri, d0, d1, d2) -> 0 / 1 / 2 triangles: Sutherland-Hodgman of one triangle of 5-float vertices against a plane given by the three signed distances (inside = sign bit clear); the second result goes to extraTri... |
| 0x004D65E0 | 562 | SSCCmdCArray_SetAtGrow | (this = CArray of SSCCmd, index, cmd*): grown to index + 1 (SSCCmd_ConstructN) and the command assigned (Compiler_ParseConditions). Was SSCCmdArray_SetAtGrow |
| 0x004D6100 | 521 | SSCScriptDeclCArray_Add | (this = declarations CArray +0x8c, decl*) -> index: grown by one (SSCScriptDecl_ConstructN) and the declaration copied (Compiler_CompileText DeclareScript / undeclared Script). Was SSCScriptDeclArray_Add |
| 0x004D63E0 | 406 | DefineList::Serialize | slot 2 of 0x73D4C0 (CArchive): {CString name, CString value} nodes (unused). Was StringPairList::Serialize |
| 0x004D6970 | 406 | CServerSpecificManager::CreateByKind | slot 14 factory (kind): 0x46 CObject3DServer (0x20 bytes, ctor 0x51F240), 0x47 CMapObjectServer (0x94, 0x515610), 0x48 CUnitServer (0xb20, 0x545580), 0x49 CPlayerServer (0x5128, 0x66C540), 0x4b CTorchObjectSpecific (0x58, inline c... |
| 0x004D6D60 | 298 | ClipBuf::AddTriangleClipFar | clips a triangle against the plane 1 - v >= 0 (Triangle_ClipByPlane5) and adds the 0 / 1 / 2 results; no callers |
| 0x004D6340 | 153 | DefineList_Clear | (this = define CList vt 0x73D4C0): every node's two CStrings destroyed, the blocks freed. Was StringPairList_Clear |
| 0x004D6C50 | 148 | ClipBuf::AddVertex | (float[5] x y z u v) -> index: x / y / z into the SoA xyz array (descriptor +0: blocks of 16 floats = x[4] y[4] z[4] pad[4]; vertex n at block n >> 2 lane n & 3), (u, v) into the uv array (+4, 8 bytes each); count +0x74++; no vert... |
| 0x004D6CF0 | 105 | ClipBuf::AddTriangle | (v0, v1, v2 float[5]): 3 x AddVertex + 3 ushort indices into the index array +8 (count +0x78) |
| 0x004D6BA0 | 92 | CTorchObjectSpecific::~CTorchObjectSpecific | fastcall (this): vt = vt_CTorchObjectSpecific, CTorchObjectSpecific::Done, the CString at +0x24 destroyed, base dtor InitVtable_73B8E0_455930. Caller the deleting dtor |
| 0x004D6840 | 79 | DefinePair_ConstructN | (pairs of 8 {CString name, CString value}, n): both = the empty string g_0x796E48. Was StringPairArray_Construct |
| 0x004D68B0 | 41 | Quat_StaticInit_Identity3 | static initializer (pointer in the init table at 0x7832CC): g_0x7AA670..0x7AA67C = (1.0f, 0, 0, 0), a file-static identity quaternion |
| 0x004D65B0 | 40 | SSCCmdBinding_GetAt | (this = binding CArray, index, out {type, slot, varIndex}): the record copied out. Was Record0C_Get |
| 0x004D6310 | 35 | DefineList_ctor | (this, grow) -> this: empty CList vt 0x73D4C0 of 0x10 nodes {next, prev, CString name, CString value} (the "#define" table of one compile). Was CArray_Ctor_73D4C0 |

## 0x4D7000 - 0x4DA000: CSkyObject

56 functions, group `client`.

| addr | size | name | purpose |
|---|---|---|---|
| 0x004D8000 | 1130 | ShadowProjector::Build | (A, B, C array descriptors, corners SoA float[16] (only 0, 2, 3 read), L light dir, pos caster centre, a radius): L normalised (+0x48, +0x24); AU = Eu' / /Eu'/^2 with Eu' = (corner2 - corner3) minus its L component (+0xC), AV like... |
| 0x004D74B0 | 911 | ShadowProjector::ClipStage2 | planes v >= 0, 1 - u >= 0, 1 - v >= 0 then emit (the second half of a u-split triangle) |
| 0x004D7860 | 856 | dead_4D7860 | unregistered code with no call, jump or pointer anywhere in the image (gaps.py 2026-11-06) |
| 0x004D9360 | 791 | CSkyObject::Recolour | (this, r1 g1 b1, a2 r2 g2 b2 (ints after ftol of the colour floats)): rebuilds the horizon table +0x220 (one float per vertex: sqrt(-normal z) for vertices with z lane >= 1.0 in the SoA positions +0x1C0, else 0) when the vertex co... |
| 0x004D71D0 | 708 | ShadowProjector::ClipStage3 | planes 1 - u >= 0 then 1 - v >= 0; each result emitted with 3 x ClipBuf::AddVertex + indices |
| 0x004D7BD0 | 702 | ShadowProjector::ClipStage1 | (tri float[15]): planes cap (L . P + +0x68 >= 0), u >= 0, v >= 0 then Stage3 / Stage2: the prism clip of one triangle |
| 0x004D9B80 | 594 | MatrixStack_Push | (CD3DLibrary, matrix, index): pushes matrix * top on matrix stack `index` (stacks at +0x14 + 8 * index: {buffer, depth}) |
| 0x004D89A0 | 557 | ClipBuf::Finish | xyz count = ceil(v / 4) blocks, index count = the real count, unused lanes filled with vertex 0, the three array pointers detached (the arrays belong to the caster / decal) |
| 0x004D8780 | 531 | ClipBuf::ReserveForTriangles | (k): n = 3k: uv array count = v + n (8 B each), xyz blocks = v + n/4 + 1 (64 B), indices = i + n (2 B): the count fields are SET to the reserved sizes (Finish trims them) |
| 0x004D9880 | 530 | Matrix4_Mul | (out, a, b): row-major 4x4 product out = a * b (matrix stacks of the render context: world +0x24, view +0x1C) |
| 0x004D8EB0 | 411 | ByteVector_Resize | (this = vector<byte> {begin, end, eos}, n, fill byte*): n < size -> end = begin + n; else inserts n - size fill bytes at the end (in place when capacity allows: move + memset pattern, else Bytes_InsertNRealloc). Callers CString_Sc... |
| 0x004D96E0 | 403 | CSky::Render | (this = sky holder, ctx, cameraPos): the sky figure (+0) is placed at (camera x, y, z - 16), spun about Z by Game_GetTimeSum() / 3000 rad when +0xC, its matrix +0x54 rebuilt (Quat_ToMatrix4) and pushed on the context's world stack... |
| 0x004D85F0 | 381 | ShadowProjector::ClipTriangleB | same for object triangles (scratch g_0x7AA690; the dead object-receiver branch) |
| 0x004D9110 | 344 | CSkyObject::ctor | CFigure-style construction (CObject3D base, vt_CFigure then vt_CSkyObject): the figure's aligned SoA arrays (+0x128.., positions +0x1C0, normals +0x1D0, +0x1E0, index arrays +0x1F0 / +0x200 / +0x210, horizon table +0x220) zeroed, ... |

## 0x4DA000 - 0x4DB000: GameBuffer (stream)

38 functions, group `leaf`.

| addr | size | name | purpose |
|---|---|---|---|
| 0x004DAE30 | 389 | BufferedFile_ReserveWindow | slot 0 of vt 0x73F8A4 (FILE stream) and 0x73F8C8 (abstract base) (this, n): window smaller than n -> a new window of n + 0x400 bytes (old contents copied, cursor / valid end re-based), dirty data flushed (slot 8) and the window re... |
| 0x004DA200 | 381 | GameBuffer::ReadLength | (this, out string range, max): reads the 1/4-byte variable-length prefix and then the string bytes (assigns a std::string) |
| 0x004DA380 | 196 | GameBuffer::WriteLength | (this, range{begin,end}*): writes the range length as a 1-byte (n*2) or 4-byte (n*2+1) variable-length prefix, then flags dirty bit 1 |
| 0x004DABC0 | 188 | GameBuffer::InitWithSize | (this, valid size, capacity): dirty bit cleared, a buffer of capacity bytes, cursor = start, end = start + capacity, valid end = start + valid size, file offset 0; (a flush branch that cannot run since the dirty bit was just clear... |
| 0x004DA040 | 182 | dead_4DA040 | unregistered code with no call, jump or pointer anywhere in the image (gaps.py 2026-11-06) |
| 0x004DAD80 | 162 | dead_4DAD80 | unregistered code with no call, jump or pointer anywhere in the image (gaps.py 2026-11-06) |
| 0x004DA740 | 161 | GameBuffer::WriteGrow | slot 4 (data, n): reallocates to (length + n) * 2 + 0x40, rebases +8 / +0x10 / +0xc, copies n bytes at the cursor +8 and advances it (the overflow path of the inline write). Was GameBuffer::Write |
| 0x004DA9E0 | 156 | GameBuffer::CopyCtor | (this, src) -> this: +0x14 = 0, vt 0x73D624, a new buffer of src capacity (end +0xc - buffer +4) with the contents copied, valid end +0x10 and cursor +8 at the same offsets as in src, flags +0x18 copied. Callers NetPeer::Clone, Ne... |
| 0x004DACE0 | 149 | BufferedFile_MoveWindow | (this = buffered file {vt, window +4, cursor +8, window end +0xc, valid end +0x10, window file offset +0x14, flags +0x18 (1 dirty, 8 error), FILE* +0x1c}, file offset): flushes the dirty window (vt slot 8), reads min(valid end - c... |
| 0x004DA860 | 139 | GameBuffer::Seek | slot 6 (pos): valid end +0x10 = max(valid end, cursor); cursor +8 = data + pos, growing the buffer (x2 + 0x40) when pos is past the capacity |
| 0x004DA170 | 138 | GameBuffer::ReadOverrun | (this = GameBuffer {vt, buffer +4, cursor +8, buffer end +0xc, valid end +0x10, file offset +0x14, flags +0x18}, dest, n) -> bytes read: the out-of-line path the inline readers take when cursor + n passes the valid end: flags /= 8... |
| 0x004DAB40 | 125 | dead_4DAB40 | unregistered code with no call, jump or pointer anywhere in the image (gaps.py 2026-11-06) |
| 0x004DAFC0 | 125 | BufferedFile_GrowWindow | slot 1 of vt 0x73F8A4 / 0x73F8C8 (this, n): window size <= n + 0x400 -> a new window of n + 0x400 bytes, old contents copied, cursor +8 and valid end +0x10 re-based (no file I/O) |
| 0x004DA630 | 124 | GameBuffer::GrowForRead | vt 0x73D624 slot 0 (this, n): new buffer of ((file offset +0x14 - buffer) + cursor + n) * 2 + 0x40 bytes (pointer arithmetic as written), old contents up to the buffer end copied, old freed, cursor / valid end re-based, buffer end... |

## 0x4DB000 - 0x4E1000: CSysInfo + CTextManager

110 functions, group `app`.

| addr | size | name | purpose |
|---|---|---|---|
| 0x004E0150 | 4699 | CTextManager::GetItemTexts | (this, item, out name, out description, out category) -> 1: by item kind +0x10 builds a texts.res entry name (proto / material names with spaces -> '_' and lower-cased by 0x700EFE; spell / modifier names untouched), opens it in th... |
| 0x004DDF28 | 1887 | CSysInfo::CollectInfo | fastcall (this) -> 1: GetOsInfo (OSVERSIONINFOA at +0x74: build +0x80, platform +0x84, CSD text +0x88), GetSystemInfo (+0x50), GetMemoryStatus (+0x30), DetectCpuFeatures; flags +8 /= 8; by platform: 0 "Windows 3.11/Win32s" + "%d.%... |
| 0x004DF030 | 1186 | CTextManager::SeekSection | (this, name or 0) -> 1 found (cursor at the start of the matching line) / 0 end of text: name 0 = the next line whose first word starts with "#" (0x78DCFC) but not "##" (0x78DCF8); one-word name = the next line whose first word eq... |
| 0x004DFCB0 | 848 | CTextManager::ReadLines | (this, out CStringArray*, advance flag) -> 0 at end of text, else 1: reads ONE line from the cursor split into words: spaces and TABs separate (empty words dropped), LF or CR ends the line (the last word pushed), DBCS pairs kept; ... |
| 0x004DF8D0 | 674 | CTextManager::ReadLineValue | (this, out CString*, advance flag) -> 0 at end of text, else 1: out = the characters from the cursor up to the first LF or CR (no trimming, DBCS-aware); when advance != 0 the rest of the line and the following "##" comment lines a... |
| 0x004DD0D0 | 589 | StringVector_InsertN | STL: vector<std::string>::insert(pos, n, value) in-place path (String_Append / AssignRange shuffles, StringRange_UninitCopy), grow path 0x4DCE90; StringVector_Resize (SpellSetRecord rune lists) |
| 0x004DCA90 | 580 | CharSetBuckets_Reserve | STL: vector<node*>::reserve(n) of the bucket vector +4..+0xc of the global char hash set g_0x7AB8D8 (CharSet_ctor reserves 101); pool chunk refill inlined |
| 0x004DB200 | 538 | BufferedFile_Read | slot 3 of vt 0x73F8A4 / 0x73F8C8 (this, dest, n) -> bytes read: n larger than the window: dirty data flushed, n bytes read straight from the file at the cursor's position (slot 7, flags /= 8 when short), cursor += n, window slid t... |
| 0x004DD8A0 | 515 | CharSetBuckets_ctorN | STL: vector<node*>(n) bucket vector ctor (pool refill inlined); CharSetBuckets_Rehash of the global char set g_0x7AB8D8 |
| 0x004DCE90 | 472 | StringVector_InsertRealloc | STL: vector<std::string>::insert(pos, n, value) reallocating path (buffer max(2 size, size + n) from the pool, StringRange_UninitCopy both halves, old strings freed); the push_back grow path of 27 name-list builders (CollectNames,... |
| 0x004DF510 | 450 | CTextManager::SkipLineAndComments | fastcall (this) -> 1 more text / 0 end: skips the rest of the current line (past its LF), then while the next line's first word (PeekWord) starts with "##" skips that line too; the cursor ends at the start of the first non-comment... |
| 0x004DC670 | 445 | String_SplitBracketAware | fastcall (std::string*, out vector<string>*, separator char (stack)): the separator is stored into byte 5 of the static delimiter set g_0x7AB8D0 (built by dead_4DC4F0); loop: find_first_of the set from pos (0x4DD3B0); not found or... |
| 0x004DB420 | 434 | BufferedFile_Write | slot 4 of vt 0x73F8A4 / 0x73F8C8 (this, src, n) -> bytes written: n larger than the window: dirty data flushed, n bytes written straight to the file at the cursor's position (slot 8, flags /= 8 when short), cursor += n, valid end ... |
| 0x004DC060 | 402 | String_SplitOutsideBrackets | fastcall (text, out CStringArray*, separator, open char, close char) -> 1: a CString copy with the separator appended, out emptied; walks DBCS-aware (lead byte test 0x6E99F2 = _ismbblead, named _isdigit_mb; _mbsinc): the open char... |

## 0x4E1000 - 0x4E3000: CVisibilityFog / VisionFog

48 functions, group `client`.

| addr | size | name | purpose |
|---|---|---|---|
| 0x004E1EB0 | 937 | CVisibilityFog::Render | (ctx) -> bool: no cells -> 1; ComputeBounds when > 12000 cells and the bounds flag +0x68 is clear; CViewFrustum::CullBox(ctx +8, bounds +0x1C) == 2 -> 1; static arrays (g_0x7ABA30 16-byte {x, y, z, size 0.4}, g_0x7ABA44 8-byte {co... |
| 0x004E1770 | 725 | CVisibilityFog::ComputeBounds | min +0x30.. / max +0x3C.. over the cells of the hash map +0x54 (cell position +0xC.., half size), then the centre +0x1C and radius; flag +0x68 = 1 (bounds valid) |
| 0x004E1C60 | 584 | CVisibilityFog::RebuildCells | (grid {x0, y0, rows ptr, w, h}): marks every cell stale (+0x1C = 0); for every non-zero byte of the grid: key = (y0 + row) << 16 / (x0 + col): an existing cell is marked alive, a new one gets {+8 short x, +0xA short y, +0xC world ... |
| 0x004E2BC0 | 560 | HashBuckets_Reserve | (this = STLport hashtable {.., buckets vector +4 / +8 / +0xc, count +0x10}, n): the next prime >= n (binary search in the 28-entry table 0x73B914, 0xFFFFFFFB past it); the bucket vector reserved to it (pool alloc <= 0x80 bytes via... |
| 0x004E1AB0 | 405 | CVisibilityFog::Tick | slot 6, per tick: every cell: alpha (+0x18) *= 0.5 when stale (+0x1C == 0) else alpha = 1 - (1 - alpha) * 0.5 (eases towards 1); cells below 0.01 are removed (keys collected in g_0x7ABB58); sprite +0x1D = (sprite + 1) % 15; return... |
| 0x004E2FA0 | 334 | DwordVector_InsertRealloc9 | STL: vector<dword>::insert(pos, n, value) reallocating path (DwordRange_Copy_4E35C0), the push_back grow path of CVisibilityFog::Tick (cell list), DialogScreen::Build / Update and CBattleHud::Update (id lists) |
| 0x004E13E0 | 328 | CTextManager::GetItemDescription | (this, item, out CString*) -> 1, 0 when GetItemTexts fails: name / description / category fetched, then the description local is reused for the format: count +0x48 < 2 -> GetString("format_item1") and Format(out, fmt, category, na... |
| 0x004E2290 | 314 | CVisFogManager::CreateByKind | slot 14 factory: kind -> new CVisibilityFog (vt_CVisibilityFog, 0x69 bytes: CObject + bounds +0x1C.., min +0x30, max +0x3C, cell hash +0x54 (buckets +0x58 / +0x5C, count +0x64), bounds flag +0x68) |
| 0x004E24E0 | 310 | CVisibilityFog::dtorBody | vt_CVisibilityFog: frees the cell nodes (0x20 bytes, pool g_0x79B230) and buckets +0x58, then the CObject dtor 0x455930 |
| 0x004E15C0 | 244 | TextArchiveEntry_Create | cdecl (value*) -> 0x18 entry from the STLport free list g_0x79B22C (stlport_node_alloc_chunk_alloc(0x18, 20) refill chained when empty): name string copy-constructed at +8 (String_CtorRange2), resource +0x14 = value +0xc. Caller T... |
| 0x004E2760 | 229 | CVisFogManager::Tick | slot 6: calls vt[6] Tick on every fog of the list +0xB8; a fog returning 0 is unlinked and vt[4] released |
| 0x004E2620 | 210 | HashTable20_Destroy | hash table of 0x20-byte nodes (the fog cells): frees every chain and the bucket vector |
| 0x004E28D0 | 190 | CVisFogManager::CreateAndRegister | creates a CVisibilityFog (vt[23](0, -1)), refcount++, appends it to the CList +0xB8 (0xC-byte nodes); returns it |
| 0x004E2F00 | 146 | HashMapUint_EraseKey | (this = hash_map<uint, ..>, key*) -> removed count: every node of the key's bucket with node +4 == key unlinked and pushed on the 0x20 free list g_0x79B230 (the head checked last), count +0x10 decremented per node. Caller CVisibil... |

## 0x4E3000 - 0x501000: renderer: D3D7 device, state blocks, vertex buffers, MMX

513 functions, group `renderer`.

| addr | size | name | purpose |
|---|---|---|---|
| 0x004F16A0 | 4016 | CD3DLibrary::CullBox | (this = the library g_0x7AF068; bounds record {+0 byte flags: 1 sphere valid, 2 box valid; +4 centre xyz, +0x10 radius, +0x14 min xyz, +0x20 max xyz (extents relative to the centre)}) -> 0 fully inside, 1 partly, 2 outside: the 8 ... |
| 0x004EA4A0 | 2855 | CRenderDevice::DrawTrianglesTransformed | (positions VertexSoABlock[] (4 vertices per block), uvs 8 B per vertex, blockCount, indices, indexCount, diffuse, specular, bounds or 0, zScale): CD3DLibrary::TransformForDraw 0x4F2650 (CullBox; no bounds = partly -> vt[9] stack 2... |
| 0x004F93E0 | 2660 | LightParams_ReadMorphed8 | stdcall (out light, cursor**, a, b, c): the 8-corner version of LightParams_ReadMorphed6 for data with the 'FIG' magic (dead_4F8CB0 skips a 0x28-byte header): records of 8 vec3 (0x60 bytes, cursor += 0x60) and 8 scalars (ends at +... |
| 0x004E9A50 | 2632 | CRenderDevice::DrawMeshBatchBlended | (texA, texB, then the DrawMeshBatch arguments): DrawMeshBatch whose final draw is DrawIndexedTwoPassBlend(texA, texB, ...): the mesh twice, the second pass with texture B alpha-blended over A (the wounds texture of CObject3DClient... |
| 0x004E4240 | 2623 | CWeather::DrawRain | (this = CWeather at CWorldClient +0x6F4, render context): mode 1 of CEffectSoundObj::Update 0x4E5680: 81 x 81 half-unit cells around the camera (+0x20 world -> +0x210 camera, offsets +0x94), phase = hash table g_0x78DEA4 + ix*172 ... |
| 0x004E9010 | 2616 | CRenderDevice::DrawMeshBatch | (positions SoA (blocks of 4: 4x,4y,4z,4w), vertBlocks, normals SoA, normalBlocks, diffuse[], specular[], uv[] (8 B), uvCount, components[] {ushort posIdx, colourIdx (= normal index), uvIdx}, compCount, -, -, indices[], indexCount,... |
| 0x00500B70 | 2566 | I3DFigureImpl::InitRenderer | slot 2 (hwnd, AppSettings 0x79BC70): LoadLibraries; FindHw3dDriver (fatal "Can't find any driver with HW 3D.") or DrawingTarget (+0x2C); new CRenderDevice(hwnd, driver, FullScreen +0x44, w +0x30, h +0x34, bpp +0x38, 0, 1, zbpp +0x... |
| 0x004E5E50 | 2530 | CRenderDevice::CRenderDevice | (hwnd, driver node, fullScreen, w, h, bpp, 0, 1, zDepth, 0): CDXDirectDraw base at +0; QueryInterface IID_IDirect3D7 (0x745A80) -> +0x50; EnumDevices into +0xB0 and picks ONLY the node whose GUID == IID_IDirect3DHALDevice (0x745A5... |
| 0x004E4C80 | 2525 | CWeather::DrawSnow | (this = CWeather, render context): mode 2 of CEffectSoundObj::Update: same 81 x 81 grid as DrawRain, hash g_0x78DF24 (-0x7570*ix + 0x45B3*iy), period 1000, fall +0xB0 - 0.1, flakes outside [max(camZ-30,0), camZ+30] skipped, sway f... |
| 0x004F7F00 | 2245 | CRenderManager::BuildFigureMesh | (scene, part name, data** (FIG8 file), s, d, h): the entry of the scene's +0x14 map (FindOrCreateEntry) gets +0 = textureNumber (file +0x24), +4 = materialKind (file +0x20, the 17..25 word); the 10-dword header is read, the bounds... |
| 0x004E8900 | 1795 | Mesh_ComputeBounds2D_C | (this = CRenderDevice, {positions, count}, skipBytes or 0): screen RECT (rounded to nearest) of the 16-byte transformed positions {x, y, z, rhw} (the scratch g_0x7AEE10, 4 per iteration): min / max x, y over all, or only those who... |
| 0x004E7FB0 | 1783 | Mesh_ComputeBounds2D_A | compute 2D min/max of projected vertices for a draw call set (mode 0 / 1), 1.8 KB DEAD CODE: no call and no pointer anywhere in the file (byte search 2026-11-03). |
| 0x004EEE30 | 1756 | CDXDirectDraw::CreateSurfaces | (backBuffers (> 10 = "Probably, this is a PROGRAMMERS ERROR"), with3D): windowed (+0x20 bit 0 clear): primary (DDSCAPS_PRIMARYSURFACE 0x200) -> +0x14, then backBuffers == 0 -> the render surface +0x18 wraps the primary itself, els... |
| 0x004F9FD0 | 1690 | CRenderManager::ReblendFigureVertices | (scene, part name, data**, s, d, h): FIG8 re-morph of an existing part: header + bounds (0x168 B) skipped without a signature test, vertex blocks blended trilinearly (3.3), pipeline vt[7] ComputeBounds into the entry +8 (FindOrCre... |

## 0x501000 - 0x50A000: renderer: I3DFigure / C3DFigureImpl (scene, figure drawing)

140 functions, group `renderer`.

| addr | size | name | purpose |
|---|---|---|---|
| 0x00501D90 | 3211 | I3DFigureImpl::DrawTexturedPolyline | slot 16 (points {Vec3*, count}, width, mode, uLength, v0, v1) -> 1: static buffers g_0x7AF020 transformed points, g_0x7AF000 / g_0x7AF010 vertices, g_0x7AF030 indices; each segment becomes a screen-space quad of the given width (L... |
| 0x00502CA0 | 2524 | I3DFigureImpl::DrawParticleQuads | slot 17 (alphaBlend, {positions (0x10: xyz + size), count}, {.., records (colour, uv rect*) +4}) -> bool: static buffers g_0x7AEFA0 / g_0x7AEF90 vertices / g_0x7AEF78 / g_0x7AEF60 indices; the points transformed by the pipeline (s... |
| 0x00503A40 | 1238 | I3DFigureImpl::DrawArrow3D | slot 18 (pos, size, dir, right) -> 1: the SelectionType-1 selection marker (CWorldClient::RenderObjects, size 0.5 at the "hp" part): a flat arrow of 6 vertices at pos.z + 0.3 (s = size / 3: +-3 s along dir, 2 s / 5 s / 1.4 s / 4 s... |
| 0x00507530 | 1109 | AnimNodeOld_AdvanceTime | (node, double* dt): the OLD (.ani, double-precision) animation node of the 3ds tool chain: frame fraction +0x38 += dt, frame +0x34 ++ at 1, key indices step past their end frames; CFigureManager::MakeAnimationNode / LoadAnimationF... |
| 0x00505CB0 | 1078 | AnimNodeOld_ReadFromData | thiscall (AnimNodeOld, cursor*): one node of the old .ani data: rest rotation (16 B raw) +0xe4, pivot +0xf4, u32 n + n Vec3 into the base vertices +0x100 and the working copy +0xd4, frame count +0x40, u32 + rotation keys +0x44 (Al... |
| 0x005015A0 | 986 | I3DFigureImpl::SetupCamera | slot 25 (RECT, fov or 0): the 16 stack depths of the library +8 reset; P = Mat4_Perspective(fov (camera g_0x79B2A8 default), rect aspect, near, far) on stack 6, V = Mat4_Viewport(rect) on stack 8, identity on 7, stacks 1 and 0 = V... |
| 0x005041F0 | 982 | I3DFigureImpl::DrawRect2D | slot 19 (x0, y0, x1, y1, alpha, outline) -> 1: texture stage 0 cleared; a transformed quad (FVF 0x1C4, colour alpha << 24) drawn with DrawIndexedTriangles; UI panels / fades |
| 0x00505901 | 935 | dead_505901 | unregistered code with no call, jump or pointer anywhere in the image (gaps.py 2026-11-06) |
| 0x00504A60 | 923 | I3DFigureImpl::SetGamma | slot 22 (brightness, contrast, c) -> bool: needs DDCAPS2_PRIMARYGAMMA (0x20000 of caps +8); IDirectDrawGammaControl on the primary surface (+0x14 -> +0x10): a 256-entry ramp per channel from brightness / contrast ("Brightness" set... |
| 0x00507E80 | 826 | AnimNodeOld_ResetToFirstKey | fastcall (AnimNodeOld): pose from rotation key 0 (+0x44 data, read without a count check): root: world rotation +0xb8 = key 0 quaternion, world position +0xc8 = the pivot +0x110 rotated by it (no translation); child: world rotatio... |
| 0x005061E0 | 727 | dead_5061E0 | unregistered code with no call, jump or pointer anywhere in the image (gaps.py 2026-11-06) |
| 0x00506D00 | 709 | dead_506D00 | unregistered code with no call, jump or pointer anywhere in the image (gaps.py 2026-11-06) |
| 0x00507990 | 699 | AnimNodeOld_StepTimeRecursive | thiscall (AnimNodeOld, double dt) -> 1: the morph frame [+0x98] scaled by dt is added to the working vertices +0xd4; AnimNodeOld_AdvanceTime(node, &dt); world position / rotation composed as in AnimNodeOld_RefreshRecursive; recurs... |
| 0x00509000 | 628 | dead_509000 | unregistered code with no call, jump or pointer anywhere in the image (gaps.py 2026-11-06) |

## 0x50A000 - 0x50E000: CAnimationTrack (.anm keys: positions, rotations, morph frames; Evaluate) / CAnimationNode (hierarchy, bound figure part, morph interpolation, UpdateTransform) / CAnimationSystem2 (nodes, animation queue, SetTime / SetAnimation / SetMorphParams / BindPart)

97 functions, group `figures`.

| addr | size | name | purpose |
|---|---|---|---|
| 0x0050B8C0 | 827 | CAnimationNode::Interpolate8 | (cursor -> 8 Vec3 of a .bon entry): +0x5C local position = trilinear blend of the 8 corner offsets with the node's morph params +0x68 (s), +0x6C (d), +0x70 (h) (same corner order as the .fig); cursor += 0x60 |
| 0x0050C230 | 763 | CAnimationNode::UpdateTransform | (modelOffset xyz): root (no parent +0x28): world position +0x74 = the current track's position +0x4C scaled by CObject3D::GetModelLerp of the owner's height param (the .adb header floats: (min + (max - min) * h) / ref) minus model... |
| 0x0050ACD0 | 582 | CAnimationTrack::Evaluate | (time in frames, float): frame = floor(time), t = frac: +0x4C position = lerp(key[frame], key[frame+1], t) (the last key / zero past the end), +0x58 rotation = Quat_Slerp (identity when no keys), +0x68 morph vertices = lerp of the... |
| 0x0050AF40 | 575 | CAnimationNode::CAnimationNode | (0xA0, kind 0x300C): CObject base; +0x1C name, +0x20 parent name, +0x24 model name (CStrings), +0x28 parent node, +0x2C child CList (head +0x30), +0x48 bound part (CFigure*), +0x4C current track, +0x50 main track, +0x54 / +0x58 al... |
| 0x0050BC00 | 546 | CAnimationNode::Interpolate6 | (cursor -> 6 Vec3 of an old 72-byte .bon entry): +0x5C local position = lerp(lerp(lerp(p0,p1,s), p2, d), lerp(lerp(p3,p4,s), p5, d), h) with the morph params +0x68..+0x70; cursor += 0x48 |
| 0x0050AAF0 | 473 | CAnimationTrack::LoadFromData | (cursor*): one .anm part track: u32 rotation key count -> +0x2C, quaternions (w,x,y,z 16 B) pointed at +0x30 (not copied: the mapped file), u32 position key count -> +0x24, Vec3s at +0x28, u32 V (morph vertices per frame) -> +0x34... |
| 0x0050A210 | 472 | Records18Array::Serialize | (CArchive): loading (flag +0x14 bit 0): ReadCount n; n == 0 destroys every record (Record18_Destroy) and frees; else grows like CArray::SetSize (new n * 0x18 block, old records moved by raw copy, Records18_ConstructN on the new ta... |
| 0x0050B510 | 456 | CAnimationNode::Done | slot 3: releases (vt[4]) the bound part +0x48 and the tracks +0x50 / +0x54 / +0x58, then the Init reset |
| 0x0050D600 | 444 | CAnimationSystem2::SetTime | (t, frames since the clip start, = world ticks: 1 frame per tick): +0x60 = t (dirty when changed); no current clip (+0x40) but a queue (+0x44, count +0x50): the first queued .adb entry becomes current at time 0; while the current ... |
| 0x0050B1D0 | 416 | CAnimationNode::dtor | vt = vt_CAnimationNode; the part, main track, track2, track3 released through their vt slot 0x10 and cleared; name / parentName / modelName set to ""; parent = 0; children list reset (CList_Reset6) then its MFC CList shell (vt 0x7... |
| 0x0050B370 | 406 | CAnimationNode::Init | slot 2 -> 1: names +0x1C / +0x20 / +0x24 = "", parent +0x28 = 0, child list +0x2C reset, bound part +0x48 and tracks +0x4C..+0x58 = 0, world position +0x74 = 0, rotation +0x80 = identity, bone matrix vector +0x90 emptied, morph pa... |
| 0x0050B6E0 | 391 | dead_50B6E0 | unregistered code with no call, jump or pointer anywhere in the image (gaps.py 2026-11-06) |
| 0x0050D7C0 | 351 | CAnimationSystem2::SetMorphParams | (s, d, h): the model record (CFigureManager::GetModel of the owner's model name); every node gets +0x68..+0x70 = (s, d, h) and its rest offset rebuilt (SetNodeRestOffset); +0x68.. of the system = the params; dirty |
| 0x0050D180 | 342 | dead_50D180 | unregistered code with no call, jump or pointer anywhere in the image (gaps.py 2026-11-06) |

## 0x50E000 - 0x51A000: CMapObject (net state pack / unpack, buffs, specifics) + CMapObjectClientSpecific (buff visual effects + sounds, light) + CMapObjectServer + CObject3D (parts, figure / .adb animation database, LoadModel / LoadFigures / CreatePart, scale, bounds, effects list) + units prototype lookup (Units_FindProtoByName, Unit_InitStatsFromProto, Unit_ApplyPrototype, Unit_GiveProtoEquipment)

288 functions, group `objects`.

| addr | size | name | purpose |
|---|---|---|---|
| 0x005179E0 | 1656 | CObject3D::BuildFromAnimation | (modelName): with the animation system: SetMorphParams, BindSkeleton, SetAnimation(0), Update; the part records +0x160 (0x38 each, count +0x164) and names +0x114 are created from the node list (+0x28, count +0x30): LoadPartBounds(... |
| 0x00510B80 | 1605 | CMapObjectClientSpecific::UpdateBuffEffects | (silent): the owner's buffs (+0x198) are reduced to kinds with their highest level; the effect records +0x28 (0x10: kind, level, effect, active) are synced: existing ones are rescaled (0x485480, level / 16 / effect +0x128); new on... |
| 0x00518310 | 1175 | CObject3D::ComputeBoundsFromParts | min / max over every part of +0xD0: local position +0x1C + bounds centre +0x18C +- extents +0x19C / +0x1A8 (parts farther than 1e9 skipped; the +0xE0 pending parts get their position zeroed); writes +0x128 centre, +0x134 radius, +... |
| 0x005136D0 | 1157 | Unit_InitStatsFromProto | (placement, stats, protoName): placement HP = max = proto +0x48, MP = max = proto +0x4C, regen udb +8 / +0xC, armor pairs = udb +0x6C[i] * proto +0x58; stats: attributes (hero record +0xC..+0x14 or HP/3, MP/3, 25), speed proto +0x... |
| 0x00513DA0 | 1096 | Hero_InitStatsFromProto | (unitStats, protoName) -> bool: like Unit_InitStatsFromProto for a new hero (both current / max HP, MP, stamina set; warning when no hero record), then 0x524CD0(stats, 1.0), 0x525AF0, 0x524CD0 (recompute); FatalError "Can't find p... |
| 0x0050EC30 | 1001 | CMapObject::ReadNetState | slot 49 (mode: 0 full / 1 delta) -> 1 when every section ended exactly: section 0 -> the placement (DeltaReader::ReadUnitSmall into +0x17C; a changed HP short +0x17E or a full read calls vt[58] (HP changed)); 1 -> 12 bytes into Se... |
| 0x00516DA0 | 866 | CObject3D::LoadModel | (modelName, part names CArray) -> 1: +0xA4 = name; with an animation system +0x98: SetMorphParams(+0xC4..) and BindSkeleton (LoadSkeleton: the link table -> nodes); no part names -> the nodes' names (CollectNodeNames) and the figu... |
| 0x00510890 | 738 | CMapObjectClientSpecific::Update | slot 6 (every frame from CMapObject::Update): a hero's buff (owner +0x198[], 0x14) whose duration +4 drops below 30 ticks plays "magic\\end1.wav" once (+0x54 sound id); the buff sounds +0x40 (count +0x44; 0x14 records {kind, handl... |
| 0x00512730 | 732 | CMapObjectManager::CreateByKind | slot 14 factory (kind): 0x32/0x33/0x34 CUnit (0x4dc bytes, 0x52E110), 0x35 CTree (0x238, 0x5296E0), 0x36 -> 0x5297C0 (0x238), 0x37 -> 0x5298A0 (0x238), 0x38 -> 0x529980 (0x238), 0x39 CFakeUnit (0x580, 0x4466D0), 0x3a -> 0x52BEE0 (... |
| 0x0050F2D0 | 724 | CMapObject::ApplyBuffKinds | slot 27: the morph params +0xC4 / +0xC8 / +0xCC += 0.4 * (has strengthen 0x1D - has weaken 0x1E + has enlarge 0x25 - has shrink 0x26) (presence, not count; clamped >= 0; +0xC8 (the dexterity axis) is skipped for the strengthen / w... |
| 0x00517240 | 658 | CObject3D::CreatePart | (partName, FigureModel, arg) -> CFigure or 0: CFigureManager::CreatePart(model name +0xA4, part, kind A unless flag 0x1000, s, d, h, arg); with a scene +0xF0 the part must exist in it (scene vt[4](name)) and copies the scene entry... |
| 0x005114D0 | 632 | BuffSound_Start | (start, rec {kind, handle +8}, pos, kind, suffix): the sound folder by buff kind (9 "magic\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\... |
| 0x005190C0 | 596 | CObject3D::PickAnimDbEntryByMask | (key) -> .adb entry or 0: the entries whose key +0x14 matches (bits 0..14 subset, 15..31 equal) and pass vt[18](entry); one -> it; several -> those whose chance +0x1C >= a random percent, one at random (adb-format.md) |
| 0x00513B60 | 573 | Unit_ApplyPrototype | (obj, protoName) -> 1: unknown prototype: default stats (Object_InitDefaultStats -> vt[35] / vt[36]) and the warning "Can't find prototype for object %d"; a CUnit (typeId 0x50): Unit_InitStatsFromProto -> vt[36](placement) (flag 1... |

## 0x51A000 - 0x520000: CObject3DClientSpecific (texture sets +0x20/+0x24, materials +0x38, skins SetTextureNames, wounds texture + TextureBlend, shadows, invisibility / stun material tweaks, material colour push/pop) + CObject3DServer + perks.pdb database (PerksDb_FindByName / Save) + aligned dword vectors / CString vector helpers

153 functions, group `objects`.

| addr | size | name | purpose |
|---|---|---|---|
| 0x0051CE60 | 4952 | CObject3DClientSpecific::RenderShadow | slot 20 (render context): the vanilla shadow (docs/shadows.md): ShadowQuality g_0x79BD0C > 0 and the kind switch (UnitShadow 0x79BD14 for typeId 0x50, FloraShadow 0x79BD1C for 0x52, BuildingShadow 0x79BD18 others); 64x64 texture +... |
| 0x0051B7E0 | 2337 | CObject3DClientSpecific::Draw | slot 19 (renderer I3DFigureImpl, out RECT or 0) -> 1: unless the object (+0x1C) is hidden (flag 0x40 of +8): owner vt[26] Activate; the lighting cache flag 0x400 cleared unless 0x200 (static lighting); CAnimationSystem2::Update; t... |
| 0x0051C3D0 | 824 | CObject3DClientSpecific::CreateWoundsTexture | (name) -> bool: needs a primary set +0x20; the wounds texture +0xD8 = a kind-6 texture named name (texture manager g_0x7AF744: find 0x692D30 or create 0x691D00; FatalError "Can't create wounds texture"); the texture blend +0xDC (T... |
| 0x0051FF40 | 721 | PerksDb_Save | (this = perks database, CString dir; ret 4) -> 1: printf "Saving database."; opens dir + "perks.pdb" with the CFile vt+0x20 (mode 0x1001 create/write); Archive_Open(archive, file, store, version 1), Db_SaveRecords18(tag 1, groups ... |
| 0x0051E5D0 | 476 | TexNameRefs_InsertRealloc | (this = vector of 0x14-byte {key, TexNameRef}, pos, value*, n): STLport _M_insert_overflow: new capacity = size + max(size, n) from the size pool (PoolAlloc_Refill when empty) or operator new; TexNameRefs_UninitCopy [begin, pos), ... |
| 0x0051F5B0 | 471 | CStrings_Resize | (this = vector<CString>, n): shrinks (destroying) or grows with empty CStrings (CStrings_InsertRealloc when the capacity is exceeded) |
| 0x0051A440 | 389 | dead_51A440 | unregistered code with no call, jump or pointer anywhere in the image (gaps.py 2026-11-06) |
| 0x0051C250 | 383 | CObject3DClientSpecific::SetTextureNames | (primary name, secondary name) -> 1: +0xE0 / +0xE4 = the names; for each non-empty name an existing texture of the set's kind with that name (texture manager world vt[34]() 0x692D30) replaces the set's texture (the skin "<prefix>s... |
| 0x0051F790 | 382 | CStrings_InsertRealloc | (this = vector<CString>, pos, value*, n): STLport _M_insert_overflow: new capacity size + max(size, n) (size pool g_0x79B224 / PoolAlloc_Refill for <= 0x80 bytes, else operator new); CStrings_UninitCopy [begin, pos), n copies (n =... |
| 0x0051FB50 | 382 | PerksDb_DestroyTables | fastcall (db): every 0x44-byte perk record of +0xC..+0x10: ~CString +0x14, +0xC, +4, +0; the buffer PoolFree'd (capacity +0x14); every 0x18-byte group record of +0..+4: Strings_DestroyRange + vector4_Deallocate of its CString vect... |
| 0x0051B2F0 | 358 | CObject3DClientSpecific::GetOrCreateTextureSet | (textureNumber) -> set (0x14: +0 CTexture* (CWorldClient::FindTexture(kind)), +4 material slots vector, +8 count, +0xC cap, +0x10 grow): an existing texture of that kind in +0x28 (count +0x2C) returns its set; kinds 1, 3, 4, 6..9,... |
| 0x0051A5D0 | 352 | dead_51A5D0 | unregistered code with no call, jump or pointer anywhere in the image (gaps.py 2026-11-06) |
| 0x0051A010 | 311 | AlignedDwords_SetAtGrow3 | the same code as AlignedDwords_SetAtGrow (another instance): index >= count -> grow to index + 1 (32-byte aligned block, raw pointer at [-4], grow +0xC or count/8 clamped 4..0x400), data[index] = value; caller CObject3D::PickAnimD... |
| 0x0051A900 | 311 | dead_51A900 | unregistered code with no call, jump or pointer anywhere in the image (gaps.py 2026-11-06) |

## 0x520000 - 0x529000: unit stats & damage model (perks.pdb, damage types, hit locations)

172 functions, group `units`.

| addr | size | name | purpose |
|---|---|---|---|
| 0x00524560 | 1902 | UnitStats_Recompute2 | (stats, placement, unit): the full recompute: attributes +0x10/+0x18/+0x20 = base + perk groups 23/24/25 bonus; sight pairs current = base (+0xb4.. = +0xb0.., +0xe4.. = +0xe0..), night sight +0xbc *= 1 + perk 15 %; buffs (max per ... |
| 0x00520220 | 961 | PerksDb_Load | (this = perks database object {+0 vector<Record18> skills?, +0xc vector<Record44> perks}, path): opens the resource file (0x671280 / 0x671730 / 0x671B40 "perks.pdb"), parses it with the database reader (0x47A200 / 0x409AE0 / 0x409... |
| 0x00527850 | 909 | UnitRecord_SkillValue | thiscall (record 0x108 bytes, weapon kind, weapon entry or 0) -> float: kind 0..4: perk bonus = perk table g_0x7AF274 entry [3*kind - 1 + byte +0xE3[kind]] +0x34 (0 when the byte is 0 / out of range), weapon byte +0x7B, skill byte... |
| 0x00526C70 | 895 | UnitStats_WeaponSkill | (stats, damageKind, unit): the attack skill: melee kinds 0..4: perk group = kind bonus + melee byte +0x6cb (+ heroes: weapon proto +0x7b + governing attribute - 25 from the skills table letter d/i/s); 5/6: archery byte +0x6cc + pr... |
| 0x00525070 | 848 | UnitStats_ApplyDamage | (stats, unit, placement, hit, weapon): part = +0x110 + hit[11]*0xf4, state < 2 -> 0; per type i with dmg = hit[i]*hit[9] > 0: armor tiers (heroes 2..0, others 0) with state (+0x44+0x48t) > 1 absorb hit[10] * tierValue_i (wearing t... |
| 0x00522CE0 | 822 | UnitStats_PackToRecord | (stats, unit, record 0x108 = CUnit +0x330): +4 exp, +8 exp + accum - spent, +0xc/+0xd/+0xe str/dex/int, +0x10..+0x13 stance speed mults * 127.5, +0x14 base speed * 25.5, +0x15 action speed, +0x16 carry max short, +0xf move type, +... |
| 0x00527520 | 806 | dead_527520 | unregistered code with no call, jump or pointer anywhere in the image (gaps.py 2026-11-06) |
| 0x00527220 | 763 | dead_527220 | unregistered code with no call, jump or pointer anywhere in the image (gaps.py 2026-11-06) |
| 0x00528AB0 | 738 | UnitStats_RaiseSkill | (stats, skill, delta, unit): server units: byte +0x6cb[skill] += delta; the cost is added to the experience accumulator and flushed into total +4 and spent +8; max HP/mana and parts refreshed; Recompute2 |
| 0x00528DA0 | 738 | UnitStats_RaiseAttrSkill | (stats, kind, delta, unit): server units only (unit specific +0x170 vt[12] type 0x39): attributeSkills byte +0x6c8[kind] += delta, the experience cost (pow of the old / new level) goes into expAccumulator +0x700 and a positive acc... |
| 0x00524CD0 | 712 | UnitStats_Recompute | (stats, carriedWeight): older/simpler recompute (camp / client side): attributes + perk bytes +0x6ea..+0x6ec, +0x44 = -2, part max HP rescale with the health perk, +0x50 = arg, +0x54 carry max, +0x58/+0x5c limb factors, +0x28, +0x... |
| 0x00520FA0 | 631 | vector18_InsertN | (this = vector of 0x18-byte records, pos, n, value*): n == 0 -> nothing; room for n: a local copy of value (two CStrings, int, CString vector copy); n < elements after pos -> Record18_ConstructRange of the last n to the new end, t... |
| 0x005254B0 | 618 | UnitStats_ApplyHit | (stats, unit, placement, hit[15], weapon): location > 6 -> 0; sets statFlags +0x6F8 bit 1 (head hit, location 0) / bit 2 (hit[5] > 0: damage type 5 present; the damage-type order is the items.idb one); per damage type i: eff_i = m... |
| 0x005239B0 | 614 | UnitStats_GainExperience | (stats, unit, xp): server units only (specific GetTypeId 0x39): accumulator +0x700 += xp * pow(1.04 (0x73E128), int +0x20 - 25.0 (0x73E124)); when > 0 it is added to experience +4 and reset (the accumulator only keeps negative bal... |

## 0x529000 - 0x52E000: CTree / CBuilding / CStone / CStuffObject / CTrapObject / CMagicTrapObject (areas, cast points, targets, CastAtTargets) / CLeverObject (SwitchState: AI map blocking, animation phase) / CTorchObject + specifics (light, effect, looping sound) + units.udb / prototype / hero record field serializers + UnitStats_BoostMax*

154 functions, group `objects`.

| addr | size | name | purpose |
|---|---|---|---|
| 0x0052B7A0 | 805 | CLeverObject::SwitchState | (stateIndex or -1 = next, duration or < 0 = the levers database value (+0x1c of the record by model name; 10 when none)): +0x238 = the new state (cyclic +0x240 wraps, else clamped to the count - 1); on the server: the units within... |
| 0x0052D740 | 798 | UnitProto::SerializeFields | (this = Monsters record 0x120, archive): tag 0 name +0, 1 base race +4 (-> +8 race index at load), 2 int +8, 3 skin index +0xc, 4 hair +0x10, 5/6/7 complection x y z +0x14/+0x18/+0x1c, 8 string +0x34 (Unk8), 9 health +0x48, 0xa ma... |
| 0x0052A680 | 618 | CMagicTrapObject::UpdateTargets | fastcall (interval < 1 mode): drops every listed target (CList +0x268, head +0x26C) whose position +0x1C / +0x20 is in none of the area circles (dx^2 + dy^2 < r^2) or that is dead (vt+0xDC != 0); then, when armed (flags +8 & 0x800... |
| 0x0052DC70 | 616 | UdbRaceModel::SerializeFields | (this = RaceModels record 0x168, archive): tag 0 name +0, 1 type id +4, 2 regen health +8, 3 regen mana +0xc, 4 secondary language +0x10, 5 locomotion +0x14, 6 vision arc +0x18, 7 speeds run / walk / sneak / crawl +0x1c, 8 attack ... |
| 0x00529400 | 578 | UnitStats_AddIntelligence | thiscall (stats, n, unit; ret 8): server units only; base +0x1C += n; the same max HP +0x2C and +0x38 recompute as AddDexterity with the perk table entries read inline (47 + byte +0x6E3 = category 16, 50 + byte +0x6E4 = category 1... |
| 0x0052A920 | 519 | MagicTrap_CollectUnitsInAreas | fastcall (world, areas CArray {x, y, r}, faction mask, out CList): the out list reset; per area CollisionMap::FindUnitsNear(world +0x294, {x, y}, temp list, r); every found unit that is alive (vt+0xDC == 0) and whose faction +0x25... |
| 0x0052DA60 | 519 | UdbNpcRecord::SerializeFields | (this = NPC record 0x60, archive): tag 0 name +0, 1 int +4 (Unk1), 2 exp +8, 3 strength +0xc, 4 dexterity +0x10, 5 intelligence +0x14, 6 three bytes +0x18 science / stealing / tame, 7 eight bytes +0x1b melee / archery / backstab /... |
| 0x0052BBE0 | 490 | CLeverObject::AttachPoints | slot 44 (out array, f): from the lever figure data 0x52CB40(name +0xa4): points = pos + forward (0x40D940 of quat) * .. + right (0x40F560) * .. scaled by (1 - +0xcc) + +0xcc * data[6] (70 lines) |
| 0x00529220 | 472 | UnitStats_AddDexterity | thiscall (stats, n, unit; ret 8): server units only; base +0x14 += n; max HP +0x2C the same formula as AddStrength (Unit_BuffHPRatio for the buffs); +0x38 = 1.1^(log_1.61(+4 / 50)) * (+0x18 / 25) * 30 * (1 + perk 17 * 0.01) (trunc... |
| 0x0052A4B0 | 452 | CMagicTrapObject::CastAtTargets | (target list): needs a spell +0x23c; world vt[28](x, y); an area spell (flags +0x3c & 0x10000000 clear) with cast points (+0x254 count +0x25c) casts at every cast point (CWorldServer::CastSpellMulti(g_0x7AFF60, spell, 0, trap pos,... |
| 0x00529B80 | 407 | CTrapObject::Update | slot 6: CMapObject::Update; if armed (flags bit 19 0x80000) and world->IsServer(): 0x5C0BD0(world+0x294 unit grid, pos +0x1c, list (vtable 0x73D454), 1.0) collects the units within 1.0 and triggers on them (76 lines) |
| 0x0052C740 | 407 | ObjRefCList::Serialize | slot 2 of 0x73E730 (CList<ObjRef>::Serialize): load: ReadCount, per item 4 raw bytes (a pointer value), AddTail with the ref set to that pointer (refcount ++) and the temporary released (vt+0x10), i.e. it dereferences the loaded v... |
| 0x0052BFC0 | 400 | CTorchObject::Init | slot 2 Init() -> 1: CMapObject::Init, the specific ref +0x238 released (vt[4]), then the DeltaCodecSet +0x1C4 is sized to exactly 5 NetSections (0xA0 each: array +0x1C8, count +0x1CC, capacity +0x1D0, grow +0x1D4; allocated / grow... |
| 0x00529090 | 390 | UnitStats_AddStrength | thiscall (stats, n, unit; ret 8) -> al: unit specific +0x170 vt+0x30 != 0x39 (server) -> 0; base +0xC += n; spec max HP +0x2C = max(1, 1.1^(log_1.61(stats +4 / 50)) * (+0x10 / 25) * 30 * (1 + perk 16 +0x34 * 0.01) * (1 + buff 0x1D... |

## 0x52E000 - 0x533000: CUnit (+ CFakeUnit overrides)

120 functions, group `units`.

| addr | size | name | purpose |
|---|---|---|---|
| 0x0052EF90 | 1962 | CUnit::ReadNetState | slot 49 (mode 0 full / 1 delta) -> 1 only when every net section ended where the delta base says (g_0x7A9794 vt[5] == vt[4]). CMapObject::ReadNetState (sections 0-3) then the unit sections of the DeltaCodecSet +0x1C4: 4 = the 0x10... |
| 0x00531A80 | 1848 | CUnit::Read | slot 9 (stream): CMapObject::Read, specific (+0x240) vt[9] Read, flags +0x10.. &= ~0x800000; player name CString +0x238 (Stream_ReadCString), a player id that is resolved only on a world of kind 8 (CWorldServer::FindPlayerById); s... |
| 0x005321C0 | 1160 | CUnit::Write | slot 10 (stream): mirror of CUnit::Read: CMapObject::Write, specific vt[10], player name (length-prefixed), owner player id (or -1), side, clips, start, loop, paused, stance pair, weapon pair, dead, walking, MoveCurve::Write, flyH... |
| 0x0052EC70 | 798 | CUnit::WriteNetState | CUnit slot 50: CMapObject::WriteNetState, then sections 4 = +0x330 (0x523720), 5 = 0x52EC10 stats (4 dwords, 1 dword (-1 if flags&0x40000000), 6 dwords), then if server (specific typeId 0x3e) specific->vt[29](); 6 = +0x264,+0x268,... |
| 0x0052FDF0 | 723 | CUnit::GetAttachPoint | slot 53 (out; vtables 0x73C0BC / 0x73EB60): alive (slot 55 = dead +0x274 is 0) -> out = position +0x1C; dead: entry = CObject3D::GetAnimState(current clip byte +0x254) (missing -> trace 0x78F918 with the model name +0xA4, offset 0... |
| 0x00530CD0 | 706 | CUnit::HitChanceVs | client estimate for the HUD (target unit, hitLocation): backstab path for player units behind the target (facing dot < -0.5, melee only); chance = own AttackParams[6], minus 2*dist/10 for a ranged weapon, plus own[2]/(target[2]*20... |
| 0x00532AC0 | 639 | CUnit::DropBrokenItems | armor +0x480[] and weapons +0x440[] with durability (+0x20) < 1 are unequipped (specific vt[27] / weapon slot shift, +0x44c = 0, net 5 / 6 / 4, specific->vt[25](-1)) and handed to the player's inventory (0x665470(player, list, 1))... |
| 0x00530570 | 442 | CUnit::UpdateMoveParams | specific vt[45] (UpdateSpeedFactors); server: +0x324 move class = server +0x24c (crawl) / +0x24e (sneak) / +0x248 (walk/run) by stance, +0x32c radius = max(+0x144,+0x148) * 0.9 (+0.2 player units, max 2.0; 0.01 for big protos with... |
| 0x00532860 | 435 | CUnit::GetAttachPoint2 | (out pos[3]): on a client world (world vt[26] IsServer == 0): kind 0x32 units take the part whose name starts with "rh3" (CObject3D::FindPartByNameStart) and return CObject3D::TransformSpecificPoint of it; other units ask the spec... |
| 0x00532E70 | 434 | Segments1C_Serialize | slot 2 of vtables 0x73EB9C and 0x73EBA8 (CArchive): MFC CArray<0x1C-byte record>::Serialize: loading (archive +0x14 bit 0): ReadCount n, SetSize-style resize of {data +4, size +8, capacity +0xC, grow +0x10} (grow 0 = clamp(size / ... |
| 0x0052E400 | 396 | CUnit::Init | slot 2 Init() -> 1 (CFakeUnit::Init too): CMapObject::Init, CUnit::ResetFields, then the DeltaCodecSet +0x1C4 is sized to exactly 14 NetSections (0xA0 each: array +0x1C8, count +0x1CC, capacity +0x1D0, grow +0x1D4; NetSections_Con... |
| 0x0052E5B0 | 392 | CUnit::ResetFields | fastcall: player name emptied, proto index -1, fly height 0, anim speed class 3, move radius 0.75, radius 0.5, alarm mask 0, attackable mask -1, side 0; the unit specific +0x240 unlinked (+0x1C = 0) and released, then 0; current /... |
| 0x00530A60 | 391 | CUnit::IsBehindTarget | (target): player units only, melee weapon: 1 when the target's facing (quat +0x34) . direction to this unit < -0.5 (attacking from behind = backstab) |
| 0x00531220 | 387 | CUnit::AddObject4A0 | (obj): refcount++, appended to the CArray +0x4a0 (count +0x4a4, cap +0x4a8, step +0x4ac); MarkNetDirty |

## 0x533000 - 0x53A000: CUnitClientSpecific

155 functions, group `units`.

| addr | size | name | purpose |
|---|---|---|---|
| 0x00536670 | 1970 | CUnitClientSpecific::BuildSkinTextures | (mode, out): composes the body texture: mode 0 "mo%ds"/"prot%ds" (size 0x80) from the armor +0xa0 (0x6A3480 item texture name) and the weapon (0x6A0AA0); mode 1 "mo%dp"/"prot%dp" (0x100): "%sskin_%.2d.mmp" base skin + the items of... |
| 0x00537500 | 1661 | CUnitClientSpecific::v46_PlayHitEffect | slot 46 (body part 0..5, intensity = the part's HP-ratio drop, flags = packed stats byte +0x107) from CUnit::ReadNetState section 4 when a part ratio changes: flag 8 or intensity <= 0.0001 -> nothing; hit sound "weapons\<slash/pie... |
| 0x00535CB0 | 1095 | UnitProto::BuildModelPartNames | (this = 0x120 UnitProto row of g_0x7AF358, armour ids vector, weapon ids vector, out armour/weapon part names, out base part names); only caller PrebuildUnitSkinsAndModels when the look record has no explicit part list. Armour id ... |
| 0x005381B0 | 1006 | CUnitClientSpecific::Equip | slot 26 (item id; hi word = index into the item database g_0x7C8718 (200-byte entries; +0xc = equipment slot 0..6)): +0xa0[slot] = id and attaches the item's figure / texture to the owner's figure (figure manager g_0x7AF5F4, name ... |
| 0x00534760 | 953 | CUnitClientSpecific::GetAnimSoundName | (anim record, t0, t1, out, miss): the sound of an animation event (weapon swing / hit / miss sounds "weapons\\..") |
| 0x00534E49 | 941 | CUnitClientSpecific::UpdateAnimSounds_EH | exception-handling funclet inside CUnitClientSpecific::UpdateAnimSounds 0x534B60; not a real function |
| 0x005341A0 | 756 | CUnitClientSpecific::Update | per client frame: paused (+0x260) shifts the move request ids, looping (+0x25c) advances +0x258; weapon parts hidden while the current anim has no weapon bits; hidden flag (owner flag 8) by anim class / death; fade +0x78 += 1/15 p... |
| 0x00534B60 | 745 | CUnitClientSpecific::UpdateAnimSounds | per frame: plays the animation's sound events (3D at the unit) when the animation time crosses them; the miss byte of the anim entry selects the miss sound |
| 0x00535850 | 706 | CUnitClientSpecific::ReplayMove | (this, time, dt): advances the client-side move interpolation (slots 40 / 41) |
| 0x005385A0 | 695 | CUnitClientSpecific::Unequip | slot 27 (slot 0..6): +0xa0[slot] = -1 and restores the base figure part (figure manager g_0x7AF5F4, name +0xa4, table 0x73EC24 + 0x1c * kind) |
| 0x00536300 | 682 | CUnitClientSpecific::BuildEquipmentList | (mode, out CArray<12-byte {type, id, model}>): mode 0: SetSize 0 then 2: entry 0 = {0x3005, armorIds[0] +0xA0, armor db record (0x7C8718, 200 bytes, index = id >> 16) +0xC} when valid; entry 1 = {0x3004, weaponIds[owner weaponSlot... |
| 0x005370F0 | 622 | CUnitClientSpecific::BuildDamageTextures | -> 0 / ComposeAndSaveLayers result: fills a local recipe {key = byte[6] damage levels (AlignedAlloc32_Bytes(6)), layer CStringArray +0x10, size +0x24 = 0x40, 0x80 for kind 0x32}; GetBodyPartDamageLevels(levels) false -> returns 0;... |
| 0x00536EA0 | 587 | CUnitClientSpecific::GetBodyPartDamageLevels | (out byte[6]): from the record parts (+0x374 + 0x1c i: state, +4 HP short, +6 max): 0 >= 75 %, 1 >= 33 %, 2 < 33 %, 3 <= 1/max; heroes: armor items (db +0xb4 flag) hide the covered parts; others mirror the limb pairs; returns 1 wh... |
| 0x00538960 | 579 | CUnitClientSpecific::GetAttackParams | slot 50, client mirror from the packed record: [6] = +0x349 (attack), [7] = +0x34b (defence), [2] = +0x34c (weight), [3] = +0x34d, [4] = +0x34d + +0x34e (damage), [5] = +0x34a (spread); armed heroes: from the item id +0xbc[+0x26c]... |

## 0x53A000 - 0x545000: units database (units.udb prototypes)

161 functions, group `data`.

| addr | size | name | purpose |
|---|---|---|---|
| 0x0053A200 | 1448 | UnitsDb::Load | (this = UnitsDb 0x80 at g_0x7AF358, res dir, res name "database.res" / "databaseLMP.res"): opens <dir><name> (CMappedResource), reads "units.udb" into a CMemStream, four tagged archives (Archive_Open version 1): tag 1 HitLocations... |
| 0x00542FC0 | 1351 | UdbRaceModel::operator_assign | copy-assign UdbRaceModel (0x168): name +0, ints +4..+0x18, speeds +0x1c, anim speeds +0x2c, +0x44..+0x4c, attack +0x50, defence +0x6c, 6 HL slots +0x88 {CString, 3 ints}, +0xe8 mask, textures +0xec / +0xf8, +0x104..+0x124, +0x128.... |
| 0x00542830 | 1313 | UnitProto::operator_assign | copy-assign UnitProto (0x120): CStrings +0/+4/+0x34/+0x118/+0x11c, ints +8..+0x30, +0x38..+0x94, 6 floats +0x98 senses, 6 floats +0xb0 detections, loot +0xc8..+0xe4, string vectors +0xe8/+0x100/+0x10c, 3 floats +0xf4 |
| 0x0053A8E0 | 1104 | UnitsDb::CountSoundVariants | (this, CSoundManager): ListSoundFiles("*.wav") of every sound archive, lower-cased; for every RaceModel the step-sound names +0x138 (vector<CString>) are lower-cased and +0x144[i] = the number of wav files whose name starts with n... |
| 0x00541960 | 1098 | UdbRaceModel::CopyCtor | copy-construct UdbRaceModel (0x168). Was UnitDbEntry::CopyCtor |
| 0x0053F530 | 1050 | UnitProtoVector_InsertN | vector<UnitProto>::insert(pos, n, value) (copies value to a 0x120 local, shifts, assigns) |
| 0x0053FCC0 | 1038 | UdbRaceModelVector_InsertN | vector<UdbRaceModel>::insert(pos, n, value). Was UnitDbEntryVector_InsertN |
| 0x00540B80 | 960 | UnitProto::CopyCtor | (src): copy-constructs the 0x120-byte UnitProto field by field: name +0, raceName +4 (CString copies), raceIndex +8 .. detonation +0x30 (dwords), field_34 (CString), +0x38..+0x44, hpMax +0x48 .. naturalAttack +0x64[5], naturalAtta... |
| 0x0053F950 | 873 | UdbNpcRecordVector_InsertN | vector<UdbNpcRecord>::insert(pos, n, value). Was HeroRecordVector_InsertN |
| 0x00543760 | 653 | UdbRaceModel_CopyRangeBackward | copy-assign a range of UdbRaceModel (0x168) records backward. Was UnitDbEntry_CopyRangeBackward |
| 0x00540510 | 651 | UdbRaceModel_CopyRange | copy-assign a range of UdbRaceModel (0x168) records forward. Was UnitDbEntry_CopyRange |
| 0x00541680 | 640 | UdbRaceModelVector_InsertRealloc | vector<UdbRaceModel>::insert with reallocation. Was UnitDbEntryVector_InsertRealloc |
| 0x0053E2A0 | 634 | UnitProtoVector_Resize | vector<UnitProto>::resize(n): erase tail or insert default records |
| 0x00542530 | 631 | UdbRaceModel_CopyRange2 | copy-assign a range of UdbRaceModel (0x168) records forward (variant). Was UnitDbEntry_CopyRange2 |

## 0x545000 - 0x557000: CUnitServer (update, combat, skills, orders)

277 functions, group `units`.

| addr | size | name | purpose |
|---|---|---|---|
| 0x00547890 | 2850 | CUnitServer::TickMovement | per tick for states 0 (walking a path) and 1 (turning): Predict the curve at the world tick, test the entered cell (CanUnitMoveTo, FindOverlappingUnit: standing blocker = re-path, walking blocker = wait unless faster, follow-chain... |
| 0x0054A900 | 2285 | CUnitServer::DispatchQueuedOrder | the order dispatcher (queued +0x1b4 -> current +0x190): nothing queued (kind 9) -> 0; sitting (stance 4) and not kind 11 -> stand up first (wanted stance 2, ApplyPendingStance); run byte +0x1d4 with stance != 2 and no reachable co... |
| 0x0054D140 | 2079 | CUnitServer::ApproachTarget | chase step of combat orders (every tick while out of reach): uniform cost table (+0x43c = 1), predicts the target over the attack window, waits (state 2) when it will come into reach, else re-paths every 16 ticks to the intercepti... |
| 0x0054CA40 | 1788 | CUnitServer::PlanActionApproach | (order, out action record +0xc4): builds the action animation (ChooseAttackAnim kind 3 / BuildSpellAnimQueue 4,5 with cast time spell+0x4c*15/actionSpeed / BuildActionAnimQueue 6,10) to know its duration D; reach = OrderReachDista... |
| 0x0054F970 | 1728 | CUnitServer::ChooseAttackAnim | (hit, queue, out duration, target): hit location: order param 6 = random from a 20-entry table (0 x2, 1 x6, 2 x3, 3 x3, 4 x3, 5 x3) else the param, resolved by UnitStats_PickHitLocation on the target; the strike height band from b... |
| 0x005561C0 | 1427 | CUnitServer::Write | slot 10 Write(stream) (root layout: save stream, vt[2] write) |
| 0x00550100 | 1298 | CUnitServer::BuildActionAnimQueue | (queue, out duration, kind, flags): attack: key = attack kind mask / flags / 0x90000; kind 6 interactions by param (0x5440000 / 0x5040000 / 0x4c40000, talk 0xc000000..); fallback 0x2890000; stance transitions around it; *duration ... |
| 0x0054E600 | 1203 | CUnitServer::ValidateQueuedOrder | (order, msg): non-player units: always 1 (kind 6 excepted); player units: FindInteractPoint (fails -> SendMsg0E_MoveState kind 3 flag 1, 0); CanReachTarget and IsOrderTargetReachable -> SendMsg0E_MoveState(kind 3), player message ... |
| 0x00555D10 | 1198 | CUnitServer::Read | slot 9 Read(save stream; +8 = save version): the field order of CUnitServer::Write 0x5561C0: paused +0x20 (1), frozenAnim +0x21 (1), currentAnim +0x24 (AnimQueue_ReadSave) + its start tick +0x38 (4), CMover::ReadState +0x3c, actio... |
| 0x0054A150 | 1104 | CUnitServer::ApplyPendingStance | when +0x170/+0x174 (wanted stance / combat mode) differ from +0x168/+0x16c: tests the new stance on the passability map (refused -> restore, message 0xd, 0); accepted: walking units are stopped (state 6), the transition animation ... |
| 0x00553F40 | 1097 | CUnitServer::TakeHit | slot 20 (hit[15], attacker CUnit or 0, attacker object): unresolved hits (/hit[8]/ finite, spells) are rolled here (ResolveHitChance); alive: ActivateObject + combat log; damage is skipped for player units when their player +0x108... |
| 0x0054F000 | 1092 | CUnitServer::ExecuteInteraction | (target) order kind 6 at the action end tick (CompleteOrder): param 0x18A95 loot a unit body (CWorldServer::LootUnit for player looters, RemoveObject(body), HearSound noise x 1.5 delay 26); params 0 / 1: own vs target UnitStats_At... |
| 0x00550620 | 1075 | CUnitServer::BuildSpellAnimQueue | (queue, out duration, order kind, castTime): cast clip key (humanoid: 1<<weapon kind / 0xC0000, sneak / 0x28E0000, combat mode / 0x10000, random / 0x2800000 / 0x1800000; others 0x2890000); clip duration A >= castTime C -> the clip... |
| 0x0054D960 | 1033 | CUnitServer::ExecuteCombatOrder | runs a combat/interaction order (kinds 3..6) once the target is in reach: weapon switch; PlanActionApproach(order, +0xc4) fails -> ApproachTarget (chase); talk on an aggro target -> drop; spells of kind-0x32 units in crawl -> want... |

## 0x557000 - 0x55E000: CUnitServer helpers (no strings)

148 functions, group `units`.

| addr | size | name | purpose |
|---|---|---|---|
| 0x005593D0 | 1883 | CUnit::BuildEquipmentTables | (unit, tables): fills a local look record: prototype = low 16 bits of +0x18, complexion +0xC4..+0xCC each * 127 packed in 3 x 10 bits; kind 0x32: 7 equipped item ids (CUnit::GetEquippedItem, item +0x18 = proto << 16 / material, -1... |
| 0x00559E60 | 801 | UnitHashes_WriteSave | thiscall (look tables of CUnit::BuildEquipmentTables, stream): counts the looks of every per-prototype inner table and writes the count (stream vt[2]); for every look: PathBuffer_WriteSave(look), the dwords at cell +0x24 / +0x28 /... |
| 0x0055BD60 | 681 | vector3C_InsertN | thiscall (STLport vector<0x3C look record>, pos, n, &value): vector::insert(pos, n, value): n <= spare capacity: a local copy of value (Record24_CopyCtor + its dwords + vectorString_CopyCtor), the tail moved up (Records3C_Construc... |
| 0x0055D5A0 | 592 | Records3C_CopyRange | (first, last, dst) -> dst end: assignment copy of (last - first) / 0x3C look records: short, the two dword vectors (vectorInt_Assign), byte, dwords, the CString vector (Strings_AssignRange / Strings_ConstructRange for growth); cal... |
| 0x0055A190 | 591 | UnitHashes_ReadSave | (stream, this): reads the two tables written by UnitHashes_WriteSave: 0x3c-byte records (2 vectors via 0x4FE0C0, 4 dwords, a vector of strings 0x51F5B0 / 0x4E5840) and 0x24-byte records (2 vectors, byte, dword) |
| 0x0055C010 | 586 | vector24_InsertN | thiscall (STLport vector<0x24-byte record>, pos, n, &value): vector::insert(pos, n, value): spare capacity >= n: local copies of value's two dword vectors, the tail moved up (PathRecord_Assign backwards) and the n slots assigned (... |
| 0x0055B170 | 580 | vector4_Reserve | thiscall (vector<dword>, n): when n > capacity: a new block of n dwords (<= 0x80 bytes from the node free list g_0x79B224, refilled from the chunk pool g_0x79B268..0x79B26C / operator new, as STLport's _S_chunk_alloc inlined; bigg... |
| 0x0055D050 | 521 | vector3C_InsertGrow | thiscall (vector<0x3C look record>, pos, &value, n): reallocating insert: new block of size + max(size, n) records (node pool / operator new), [begin, pos) moved (Records3C_ConstructRange), n copies of value (Record3C_CopyCtor), [... |
| 0x0055C7D0 | 515 | vector4_CtorN | thiscall (this, n, alloc) -> this: STLport vector<dword>(n) storage: begin / end / cap = 0, then a block of n dwords (node pool <= 0x80 bytes with the chunk refill inlined, else operator new), begin = end = block, cap = block + n ... |
| 0x0055C9F0 | 515 | vector4_CtorN2 | STL: STLport vector<dword>::vector(n, value) (size pool); the bucket vector of HashTable_Resize2 |
| 0x0055DDD0 | 515 | vector4_CtorN3 | STL: STLport vector<dword>::vector(n, value) (size pool); the bucket vector of HashTable_Resize3 |
| 0x0055D300 | 503 | vector24_InsertGrow | thiscall (vector<0x24-byte record>, pos, &value, n): reallocating insert: new block of size + max(size, n) records (node pool / operator new), [begin, pos) moved (MapRecord24_CopyRange), n copies of value (Record24_CopyCtor), [pos... |
| 0x00557960 | 498 | Records4C_Serialize | slot 2 of vtables 0x73F034 and 0x73F04C (CArchive): MFC CArray<0x4C-byte MobLogicSlot>::Serialize: ReadCount / SetSize-style resize (new records GridRows_Init, removed ones MobLogicSlot_dtor) / WriteCount, then size * 0x4C raw byt... |
| 0x0055D9F0 | 491 | MapExitRecord_Assign | STL: operator= of the 0x3C-byte zone exit record (MapExitRecord_ConstructN 0x4606A0): short +0, vector<dword> +4 (DwordBuffer_AllocCopy5 / Dwords_CopyRangeReturnEnd_B), vector<dword> +0x10 (vectorInt_Assign), byte +0x1c, dwords +0... |

## 0x55E000 - 0x565000: CWorld base (managers, time of day, RefListPair +0x60) + CWorldClient (object lists / grids, CBridgeManager walkable quads, sky, day-light tables, CWeather at +0x6f4), CTerrain / CTextManager / CParticleManager / CVisFogManager dtors

212 functions, group `world`.

| addr | size | name | purpose |
|---|---|---|---|
| 0x00561E50 | 2133 | CWorldClient::Init | slot 2: CWorld::CreateManagers, LoadDayLightTable, +0x1cc CSoundManager, +0x1d0 CTextManager (texts.res / textsLmp.res), +0x180 CTerrain (0xafc), +0x184 = "gipath", +0x44 CClientSpecificManager, +0x18c CTextureManager, +0x188 CMat... |
| 0x00563930 | 1587 | CWorldClient::LoadDayLightTable | (ini name) from Init ("LightsGipat.ini") and CWorldClient::LoadZone ("Lights[Cave]<WORLD>.ini"): config\<name>; sections "sunlight" -> +0x22c, "ambient" -> +0x3ac, "sky" -> +0x52c (24 x rgba); keys "hourNN" (chars 4..5 = hour) = r... |
| 0x00560820 | 1519 | CBridgeManager::AddBoxFaces | (owner id, part bounds, pos, quat): the 8 corners and 6 normals of the part's box rotated and translated, AddQuad per face (bridges, stairs, platforms the units walk on) |
| 0x005641F0 | 1478 | CWorldClient::Update | slot 6, once per frame: +0x6ec frame counter (10 -> "autosave" + volumes), terrain / music / sound / vis fog updates, net pumps (+0x5c, +0x1c8, CTcpNetDriver::SyncPeers +0x60), the local player: PurgeSyncedItems, connection vt[23]... |
| 0x00563290 | 1029 | CWorldClient::Done | slot 3: camera carrier cleared, floating numbers +0x1b4, sky +0x1ac, moshka effect +0x1b0 destroyed, UnpinItemsAndSpells, zone exit effects, print lists, CWeather / CBridgeManager Done, grid A cells, hidden ids, hero list, object ... |
| 0x005612F0 | 962 | CWorldClient::ctor | CWorld::ctor then the client members: +0x8c zone exit CArray (0x3c records: data +0x90, count +0x94), +0xa0 zone exit effect array (+0xa4 / +0xa8), +0xb4 hero unit list, +0xb8 object list, +0xbc visible CList (+0xc0 head), +0xd8 v... |
| 0x00561B00 | 844 | CWorldClient::dtor | Done, then the members in reverse (strings, hidden list, CWeather, CBridgeManager, print lists, grids, hashes, lists, arrays) and CWorld::dtor |
| 0x0055EE40 | 842 | CWorld::CreateManagers | (from CWorldClient::Init and CWorldServer::Init): paused +0x6c = 0; the path strings g_0x7AF460 (maps root), g_0x7AF45C (database), g_0x7AF458 (game root) copied from the settings g_0x79BC90..98; tick +0x64 = 0, frame +0x68 = 0, w... |
| 0x005647E0 | 774 | CWorldClient::AddObject | (obj): present in +0xb8: refcount ++ / release; else AddVisibleObject when obj vt[29]; types 0x54 / 0x5d: walkable part faces (flag 0x400) into the bridge manager; party units get stats vt[43]; inserted into +0xb8, the 16-unit gri... |
| 0x00564BE0 | 606 | CWorldClient::RemoveObject | (obj): removed from the quad list / id hash / visible CList / object list / hero list and its grid A cell (CUnitClientSpecific::ClearTargetCC in game mode 3) |
| 0x00564E70 | 542 | CWorldClient::ClearAll | both grids, the visible CList + hash, the hidden ids, the hero and object lists, CPlayer::ClearAll(+0x20c), the render manager +0x34 vt[5], and the figure / map object / specific managers cleared. Was CPlayer::ClearAll (misnamed) |
| 0x0055F8E0 | 484 | CWorld::Done | slot 3: every object of the specific manager (vt[21]) vt[5]; releases +0x38 player manager, the refs +0x5c / +0x60 (detach + release), +0x40, +0x3c, +0x34, +0x44, +0x58, +0x54, +0x50, +0x4c, +0x48 (reverse creation order) |
| 0x00562B80 | 484 | CTerrain::dtor | vt = CTerrain; CTerrain::Done; the sector height-delta CArray (vt 0x73F730 at +0xAE8, 0x14-byte records each Record14_73D3E8::DeletingDtor(0), data deleted); the ambient-world CString; the 32-byte-aligned buffers (visibleColMax / ... |
| 0x00563710 | 465 | CWorldClient::InitClientResources | (device): CTextureManager::SetDevice, CreateSky, +0x1b4 = the floating-number list with the "numbers" texture (flag 0x40000), +0x34 = new CRenderManager(g_0x7AF080, g_0x79BC94), UnitsDb::CountSoundVariants |

## 0x565000 - 0x579000: CWorldClient: LoadZone (terrain / sky / day-light tables / skins), Render / RenderObjects / ClassifyCellObjects / shadows, object picking (screen rects), terrain prints (prints.db), zone exits, hero unit creation, net attach (file / TCP streams), session helpers; STL sort / container instantiations

504 functions, group `world`.

| addr | size | name | purpose |
|---|---|---|---|
| 0x00568800 | 4459 | CWorldClient::PrebuildUnitSkinsAndModels | (model records 0x24, unit records 0x3c from the zone state): per unit record the skin textures "<prefix>skin_NN.mmp" + armor / spell overlays composed into "prot.dp / mo.dp" (256) and "prot.ds / mo.ds" (128) in the texture manager... |
| 0x0056AFE0 | 3909 | CWorldClient::RenderObjects | frame object pass: lists reset, moshka flight; grid A cells of camera +-100 classified, then deferred units and the hero list; relight budget: the 0x7AF508 objects sorted newest-lit first, max(10, stale/8) lose flag 0x400 (relit a... |
| 0x00567AB0 | 2277 | CWorldClient::LoadZone | (zone state stream = message 0x02): u32 tick -> +0x64, zone name, map name, u32 exit count + CEffectKey records (0x3c, +0x8c array), u32 indoor -> +0x6e0, the unit save records (UnitHashes_ReadSave: 0x3c unit records + 0x24 model ... |
| 0x00566360 | 2200 | CWorldClient::UpdateDayLight | every frame (Render): UpdateZoneExitEffectStates; indoor (+0x6e0 == 1): hour-0 colours, light direction (0.5, 0.5, -0.707), fog density g_0x79BED4; outdoor: tables interpolated by the hour (CWorld::GetHourAndRate), sun elevation 3... |
| 0x0056C750 | 1721 | CWorldClient::BuildShadowReceiverMesh | (light dir, pos, radius, out buffers, ..., onBridge, withObjects): ShadowProjector::Build (the prism), the terrain receiver mesh (CTerrain::BuildShadowReceiverMesh) unless the caster stands on a bridge quad, and when withObjects t... |
| 0x0056A990 | 1535 | CWorldClient::ClassifyCellObjects | (cell list, view matrix, _, timeGetTime ms, deferPassDone, includeHeroes) -> count of stale-lit objects: per object with a render specific (+0x94): units (type 0x50) skip heroes unless includeHeroes; first pass (deferPassDone 0) p... |
| 0x00569E40 | 1327 | CWorldClient::Render | the frame: UpdateDayLight; view matrix (CCamera::BuildViewMatrix) pushed on the view / world stacks of g_0x7AF068; flag 0x1000: sky (fog / z off); Net_TickAll; flag 0x100: CTerrain::SetVisibleArea(camera xy, 100) + RenderOpaque; R... |
| 0x00566F70 | 1167 | CWorldClient::CreateHeroUnits | (hero record array, allArmor byte) from Game_CreateHeroUnits (CCampScreen::Build, CCampScreen2::InitHero): per record a local CUnit kind 0x32 with the record id +0x1C, parts hd bd hp hr(.NN from the unit-db row +0x10, dropped when... |
| 0x0056BF30 | 1053 | CWorldClient::PickTerrainPointRaw | (out pos, screen x, y): PickTerrainPoint sampling the terrain directly (0x6C8680) instead of vt[29]. Was PickTerrainPoint2 |
| 0x0056C350 | 1023 | CWorldClient::PickTerrainPoint | (out pos, screen x, y): the screen ray from the inverted view-projection, z = terrain height (vt[29], bridges included) found by iteration from half the terrain extent and bisection (100 steps, 0.01) |
| 0x0056DAF0 | 911 | CWorldClient::CollectObjectsNear | (pos, radius, out list): the grid A cells of the square pos +- radius; every object whose bounding sphere (+0x1c + +0x128, radius hypot(+0x144, +0x148)) is within radius |
| 0x005769C0 | 892 | UnitRecord_PartialSort | fastcall (first, middle, last, _, plane, manager): STL partial_sort of 0x18-byte pick records: make_heap of [first, middle) (UnitRecord_AdjustHeap2 from the last parent down), every later record less than the top is swapped in (po... |
| 0x00573740 | 787 | UnitRecord_IntroSortLoop | fastcall (first, last, _, depth limit, plane, manager): STLport __introsort_loop over 0x18-byte records: stops at <= 16 records (the final insertion sort is the caller's); depth 0: UnitRecord_MakeHeap + AdjustHeap pop loop (heap s... |
| 0x00577920 | 750 | UnitRecord_MakeHeap | fastcall (first, last, plane, manager): STL make_heap of 0x18-byte pick records: from the last parent down, __adjust_heap (hole to the larger child) + __push_heap with the inlined UnitRecord_LessByDepth; caller UnitRecord_IntroSor... |

## 0x579000 - 0x57A000: game session helpers: stat scramble (anti-tamper), save writer/reader, MainMenu entry, CUIWindow 73F7F8

39 functions, group `ui`.

| addr | size | name | purpose |
|---|---|---|---|
| 0x00579830 | 841 | MpFile::ReadContainer | (ecx out CMemStream, edx expected head word 0x102, u32 newest version g_0x790260 0x74, u32 oldest g_0x790268 0x42, source stream) ret 0xc; compressed above g_0x790264 (0x64): checksum test, then the flag byte: non-zero -> LzDecomp... |
| 0x00579B80 | 734 | MpFile::WriteContainer | (ecx out IStream, edx head word 0x102, u32 version g_0x790260 = 0x74, CMemStream* body) ret 8: Stream_WriteHeader4, version > 0x6b checksum; LzCompress the body, LzDecompress the result again and compare it with the body: equal ->... |
| 0x005791C0 | 400 | World_XorStats | (player manager, withUnits, key): for every player: +0xc0 (money) ^= key, every hero record (+0xa0, count +0xa4, 0x784 each) dwords +0x20..+0x724 ^= key, and when withUnits every tracked unit (hash +0x40) UnitStats +0x1e0..+0x8e4 ... |
| 0x00579E60 | 323 | Session_ReadMissionSave | (CFileStream): CSoundManager::StopAll; MpFile::ReadContainer (max 2e9 bytes, flags 0x80); version < 0x52: CCamera::Read; < 0x47: CInterface::ReadFlags + the embedded UI stream g_0x7AFDF8 (version g_0x7AFE00); then CWorldServer::Lo... |
| 0x005790E0 | 210 | World_SetStatScramble | (active): main calls it with 1 while the window is active and 0 when not; acts only when the flag g_0x79026C changes: on 1 -> 0 (deactivation) a fresh key is drawn from the ISAAC pool g_0x7A9D40[g_0x7A9D44] into g_0x7AFECC and Wor... |
| 0x00579FB0 | 168 | Session_WriteMissionSave | (CFileStream, a): CWorldServer::WriteSave into a CMemStream then MpFile::WriteContainer (version g_0x790260, flags 0x80). Writes mission.sav. Was Game_WriteSaveFile |
| 0x005794B0 | 155 | CDemoScreen::Create | (CInterface): new 0x184-byte CDemoScreen (vt 0x73F7F8: the attract-mode demo / intro screen, not a base class; CString +0x164, demo names +0x170) handed to CInterface::SetPendingScreen. Was CUIWindow_73F7F8::Create |
| 0x00579730 | 155 | CDemoScreen::Create2 | identical second copy of CDemoScreen::Create (CUIWindow 0x184 bytes, vt 0x73F7F8, +0x94 = the 0x73F7C4 interface, the CStringArray at +0x188, the byte +0x184 = 1): CInterface::SetPendingScreen(g_0x7C7D50, window). No callers (link... |
| 0x00579420 | 126 | Session_LeaveGameToMenu | (this, a, CParty): same as request kind 5: CPlayer::SetPartyBlock(client connection owner, party), UnloadZone, CWorldClient::ConnectToLobbyHost; without a lobby server Game_EnterZone("MainMenu", 2) else Lobby_ResetMpGameState; con... |
| 0x005796B0 | 95 | CDemoScreen::~body | destroys the CString +0x164 and the demo list, then CUIWindow::~body. Was CUIWindow_73F7F8::~body |
| 0x005797D0 | 63 | dead_5797D0 | unregistered code with no call, jump or pointer anywhere in the image (gaps.py 2026-11-06) |
| 0x005793A0 | 62 | dead_5793A0 | unregistered code with no call, jump or pointer anywhere in the image (gaps.py 2026-11-06) |
| 0x005793E0 | 62 | Session_PlayRecordedStream | (file): UnloadZone, CWorldClient::AttachDualFileStream (recorded packet playback), connection +0x84 = 1, +0x85 = 0. Caller CUIScreen73F7F8::PlayIntroMovie. Was Game_AttachClientStream |
| 0x00579370 | 34 | dead_579370 | unregistered code with no call, jump or pointer anywhere in the image (gaps.py 2026-11-06) |

## 0x57A000 - 0x58A000: game session: saves, MainMenu, autoexec, scenario/mission.sav, MpState

320 functions, group `app`.

| addr | size | name | purpose |
|---|---|---|---|
| 0x0057B3B0 | 2175 | Session_SaveToSlot | (slot name): server mode 1 only; zone kind 4 (camp): local player's CParty stored into the party store first; per zone record "zs.<zone>" set and (bz7g or same allod) -> saves\current\<zone>.sav copied to saves\<name>\ else "zs.<z... |
| 0x0057ABA0 | 2058 | Session_LoadSaveSlot | (slot name) (request kind 2): not in server modes 2/3; console exec, busy cursor; clears saves\current; scenario.sav (head 0xDEAD): ObjPartyRecord_Read -> party store g_0x7AA5A0, version > 0x46 CInterface::ReadSaveState + embedded... |
| 0x005800C0 | 1894 | Campaign_AdvanceMission | MP client, on a lobby variable message: rebuilds CPlayer +0xC4 named floats from the lobby campaign state (keeps constr.current; start-allod building + intro_N; briefings +0x25F8; variables +0x25FC with q.<x>.<y> quest scans; in a... |
| 0x0057A060 | 1816 | Session_EnterZoneFromParty | (CParty) from Game_EnterZone: zone record of party +0x68; variable "movie" == 1: Movies\dcity.bik on entering bz3g (then removed; kept while elsewhere); "zs.<zone>" == 0 (first visit): LoadZone(+0xc ".mpr" (gz6g: zone6_2.mpr when ... |
| 0x0057BDF0 | 1641 | Session_HostMpZone | (zone name) (request kind 6, Session_SetMapName): g_0x7C7D94 = 1; zone record kind +0x28 must be 0; UnloadZone; saves\current\<zone>.sav -> MpFile::ReadContainer + LoadSave(stream, 0) (failure -> return 0), else LoadZone(+0xc ".mp... |
| 0x0057C700 | 1068 | Session_ProcessRequest | (SessionRequest g_0x7AFE20) called once per frame from main (0x456440) after the tick loop and from App_LoadMapAndScript: returns at once when the request kind +0 is 0 (NOT a per-frame tick). Else: when the server mode g_0x7B0098 ... |
| 0x005809D0 | 1015 | Lobby_QueueMapFileTransfer | (quest name): looks for maps\<name>.mq (CFileFind, suffix string 0x790774); when found and a LobbyServer is alive reads it into a GameBuffer and LobbyServer::AddFileTransfer("<name>.mq", buffer); on both sides registers the name i... |
| 0x0057F850 | 769 | Campaign_ShuffleNames | build a string vector from the lobby list and shuffle it with Isaac_NextWord; 769 bytes |
| 0x0057A780 | 760 | Session_SaveZoneToCurrent | (CParty): CCamera::SaveView(7 for zone kind g_0x7C2CC4 1, 6 for kind 3); writes saves\current\<zone +0x12d6c>.sav: CWorldServer::ResetUnitEffects, WriteSave into a CMemStream, MpFile::WriteContainer (head 0x102); party variables "... |
| 0x0057EE10 | 721 | SessionRequest::RequestMpMapChange | (this = SessionRequest g_0x7AFE20): pending (+0x80 set): lobby client state record (+0x2600) +0x28 = 1 (++ version), sends CPlayerClient::SendMsg06_String(skirmish map name) and on the host LobbyServer::SendToUnreadyPlayers(9, map... |
| 0x0057F560 | 698 | Campaign_ScanMissionFiles | enumerate "*.mq" mission files in the world map directory into the campaign list g_0x7AFDE0; 698 bytes |
| 0x0057DEE0 | 696 | Databases_Reload | (lmp flag): reloads the databases from "database.res" or "databaseLMP.res" (PerksDb, unit sounds, unit tables) and swaps g_0x79BEC0 / g_0x79BEC4 (the difficulty / balance index used by the stat tables g_0x7C54A0) |
| 0x0057C460 | 668 | Game_EnterZone | (zone name, kind): cursor frame 3; "endofgame" -> kind 8; else the zone record; UnloadZone; kind 0 is derived from the record +0x28 (0 -> 1 playable zone, 1 -> 4 camp / map screen, 2 -> 3 scripted zone); unknown zone -> FatalError... |
| 0x00589ED0 | 619 | CWorldServer::ctor | the global CWorldServer g_0x7AFF60: CWorld::ctor; zone exit CArray +0xec (vt 0x73C400); active object hash +0x104; +0x118 = 0; progress zone name +0x120; loaded mob names CStringArray +0x124; SMessBase registry +0x144; CAIMap::Cto... |

## 0x58A000 - 0x595000: CWorldServer: object creation from descriptions, object hashes (+0x12d70 all, +0x104 active, +0x12de8 effects, +0x12dd0 named), per-player replication (ReplicateToPlayer), weather / time / day-night, spell casting, hero spawning from CParty records, mercenaries, loot, experience, script VM dtor, CAIMap dtor

118 functions, group `world`.

| addr | size | name | purpose |
|---|---|---|---|
| 0x005927C0 | 3535 | CWorldServer::CreateObjectFromDesc | (CObjectDesc*) -> object id or -1: by kind +0x48: 1 object / 5 unit / 8 lever / 9 (active object): map object manager vt[23](type +0x50, id +0x18) (needs +0x11d for units, +0x11e for objects), flags 0x20000 (byte +0x6c or +0x4c no... |
| 0x0058B750 | 3349 | CWorldServer::ReplicateToPlayer | (CPlayer) per server tick for every player (returns 0 = disconnect): needs CPlayerServer +0x20 == 1 and +0x15c map loaded; flags +0x130: 1 -> SendZoneState then ResendTrackedObjects + FlushSendStream (returns); 2 -> SendMsg70(+0xe... |
| 0x0058EF20 | 3136 | CWorldServer::SpawnPartyHeroes | (CPlayer or 0, HeroRecord* array, pos, quaternion, previous Array28 states) = PlayerIn step 2: a "WorldScript( ... AddObject(Heroes, GetObjectByID(..)) ... PlayAnimation(..) )" text; per record: CUnitDesc (0x46C010: name, model, p... |
| 0x00591800 | 1489 | CWorldServer::UpdateMercenaryFlags | (script RecalcMercBriefings) for every player and merc i = 1..10: from the player named floats +0xc4 (b.merc<i>.n8 / n10, amerc_<i>), membership (a HeroRecord id == String_HashCI("merc<i>")), live heroes - 1 and the current zone +... |
| 0x0058CCD0 | 1450 | CWorldServer::UpdateEnvironment | every 64 game ticks (Update) and from SetTime: +0x84 = CWorld::NightFactor; in a playable zone (+0x12d64 == 1): weather mode +0xdc 3 = random lightning strikes (CEffectMoshka kind 0x65 at a random point 30..70 m from a random pass... |
| 0x005942D0 | 1292 | CWorldServer::LootUnit | (unit, player): dead or non-player units not yet looted (flag 0x20000000): CHoover::CollectUnitLoot; lobby game: the host (+0x24 == 0) takes quest items (type 0x14) first; player money +0xc0 += loot money, CPlayer::AddItems; an ex... |
| 0x00592090 | 1263 | CWorldServer::OnNamedEvent | (player index, "a.b.c" briefing event) from DialogScreen::StartDialogLine / CPlayerServer::OnMsg_NamedEvent: lobby alive -> Lobby_SendNamedEvent(b); else Briefing record of b (g_0x7C87D4), OnMercenaryEvent(player, name), GiveExper... |
| 0x0058DFF0 | 1217 | CWorldServer::Tick | slot 6, main once per 55 ms server slice: 1) per player: net stream +0x1c null / stream vt[4]() false / CPlayerServer vt[23]() 0 -> ClearNetStream + release ("Disconnecting player N"), else ActivateObjectsAround(party unit +0x1c, ... |
| 0x0058E850 | 1160 | CWorldServer::CastSpellMulti | (CSpell*, caster, pos, target, point, quiet) (12 callers: CastOrderSpell, items, traps, scripts): returns without casting when spell null, when a caster is given outside zone kind 1 (+0x12d64), or when the kind +0x18>>16 is 0x15 /... |
| 0x005911C0 | 1054 | CWorldServer::GiveExperience | (CPlayer* or 0, xp): xp 0 -> nothing. Player: live kind-0x32 units of CPlayer +0x40 share xp / n each (CUnitServer::UpdateStatsFromPlacement on the unit, UnitStats_AddExperienceScaled on the HeroRecord with the same id), SendMsg15... |
| 0x005935C0 | 1052 | CWorldServer::RemoveObject | (obj) (12 callers): must be in +0x12d70; non-0x61 objects leave the CollisionMap (+0x294); erased from the active hash +0x104, +0x12d70, the named hash +0x12dd0, +0x12d88 (static) or +0x12da0 + +0x12db8 (dynamic); vt[5] Done; unit... |
| 0x00590C70 | 1021 | CWorldServer::StorePartyFromUnits | (CPlayer): loose items +0xe4 -> CParty::AddItem(party +0x70); every hero record whose unit is alive: its four item arrays rebuilt from the unit's items (clones via party vt[24]; each unit item linked by a PartyItemLink +0x1c; weap... |
| 0x0058FD90 | 971 | CWorldServer::DetachHeroes | (HeroRecord* array, CParty, out Array28 state list) = PlayerOut: zone kind 1: follow target +0x77c recorded; per record {id, +0x9a, +0x9b, pos, quat} appended to the state list; live units (or multiplayer): the record's four item ... |
| 0x0058B250 | 941 | CWorldServer::ClearObjects | zone reset without a map change (NewZone same mob, Reset): loot records +0x12e64, mob names +0x124, water levels +0x70, zone exits +0xec, both GSWorld::Reset, cast list +0x12e2c, item update list +0x12e48, CPlayer::ClearAll per pl... |

## 0x595000 - 0x59F000: CWorldServer::LoadZone / NewZone / LoadMapObjects / LoadSave / WriteSave / UnloadZone, world script loading, progress videos; the server object hash / list / array container instantiations (IdHash_*, RefList / RefArray / LootRecordList serializers)

225 functions, group `world`.

| addr | size | name | purpose |
|---|---|---|---|
| 0x00596E70 | 3415 | CWorldServer::LoadSave | (reader, mpr override) the mission.sav / <zone>.sav body: mode +0x138 / +0x160 = 0 (1 when players exist: CPlayer::ClearAll each); Flush, +0x100 / +0x172 = 0; strings mpr, mob, progress zone (-> +0x120, loading video); every playe... |
| 0x00597BD0 | 2083 | CWorldServer::WriteSave | (stream) the mission.sav / <zone>.sav body (Session_WriteMissionSave, Session_SaveZoneToCurrent): BroadcastGameTime in zone kinds 1 / 3; ReleaseAllObjects x4; strings +0x12d68 mob, +0x12d6c mpr, +0x120 zone; wind +0x24 (12), +0x30... |
| 0x00595C70 | 959 | CWorldServer::NewZone | (mpr, mob, AIGRAPH stream, fromSave): mpr differs from +0x12d6c: CAIMap::Dtor + Flush + CAIMap::Load(World_MapFilePath(mpr)) (0 on failure), +0x12d6c = mpr, +0x12d68 = ""; the stream (seek 0) must be empty or NavGrid_Read(+0x2468)... |
| 0x00596900 | 856 | CWorldServer::ReadSaveObjects | (reader): object ids until -1: CBaseManager::FindById(map object manager +0x40) (created by LoadSave's id table); version > 0x58: name (Stream_ReadCString) -> vt[34]; PackFullNetState, vt[9] Read, u32 dirty mask; MarkNetDirty(-1);... |
| 0x005951A0 | 724 | CWorldServer::SeparateStaticObjects | after LoadMapObjects of a base map: the CMemStream +0x12e00 cleared; every object of +0x12d70 not in +0x12da0 whose vt[29]() (IsDynamic) is false moves to the static hash +0x12d88 (max +0x12d9c) and out of +0x12da0 / +0x12db8; a t... |
| 0x00596350 | 671 | CWorldServer::LoadWorldScript | (mob file name): World_MapFilePath; chunk 0xA000 then 0xACCEECCB (encrypted script: CString_Unscramble) or 0xACCEECCA (plain text); GSWorld::Reset(+0x12874) and Compiler_CompileText(+0x12874, text, 1). Used for zone records of kin... |
| 0x005957E0 | 645 | CWorldServer::UnloadZone | (7 callers: every session transition): every player ClearOwnerObject + removed from the manager; +0x15c = 0; loot records +0x12e64 cleared; mob names +0x124 cleared; navGridOn +0x172 = 0; both GSWorld::Reset; PlayerRelations::Rese... |
| 0x00595480 | 606 | CWorldServer::LoadZone | (mpr name, mob name, progress zone name): +0xe0 = 0; World_SelectProgressVideo + Movie_PlayIntro (the loading screen video); both files must exist (as given or under World_MapPath) else 0; World_LoadAIGraphStream -> FatalError "CW... |
| 0x0059A710 | 580 | Dwords_Reserve3 | STL: vector<dword>::reserve(n) with the inlined STLport allocator (Dwords_AllocCopy5 copies, old block freed); the bucket vector of IdHash_CtorDefault2 |
| 0x0059AB10 | 580 | Dwords_Reserve4 | STL: vector<dword>::reserve(n) with the inlined STLport allocator (Dwords_AllocCopy6 copies, old block freed); the bucket vector of IdHash_CtorDefault3 |
| 0x0059D800 | 580 | Dwords_Reserve5 | STL: vector<dword>::reserve(n) with the inlined STLport allocator (Dwords_AllocCopy7 copies, old block freed); the bucket vector of IdHash_CtorDefault4 |
| 0x005993A0 | 532 | NavSectorArray::Serialize | slot 2 of 0x73F9EC: CArchive of the 0x54-byte NavSector array (NavSector_ConstructN for new ones) (CollisionMap sectors). Was Array54::Serialize |
| 0x00596140 | 521 | World_LoadAIGraphStream | (mpr name, mob name, out CMemStream): with a mob name: its file is opened and the AIGRAPH chunk (magic 0x31415926, Stream_CompareWith) copied into the stream; else "<mpr base>.grp" copied whole (vt[7] CopyFrom); false when neither... |
| 0x00596C60 | 519 | CWorldServer::RemoveAllObjects | RemoveObject on every object of +0x12da0 (collected first); cast list +0x12e2c released; item update list +0x12e48 cleared; WaterLevels cleared (+0x70) |

## 0x59F000 - 0x5AD000: CFigure (.fig mesh: raw SoA buffers +0x1c0.., morph deltas +0x128, bounds FIG8 / FIG6 interpolation, SetMorphDeltas) + CFigureClientSpecific (Render / RenderLit, texture frame animation, screen rect) + CFigureManager (figures.res models: FigureModel records, LoadSkeleton (.lnk), BuildAnimationNode (.bon), LoadNodeTrack, SetupAnimation, LoadPartBounds, CreatePart; old .ani / .bon loaders)

109 functions, group `figures`.

| addr | size | name | purpose |
|---|---|---|---|
| 0x005A3800 | 4903 | CI3DFigure::LoadFigOld | slot 21 of vt 0x73C224 (data**, s, d, h): the UI-figure loader of the pre-FIG8 6-corner format (+0x158 = file +0, +0x15C = +4; bounds centre / min / max as 6 corner vec3 each + 6 radius floats, blended lerp(lerp(lerp(c0,c1,s),c2,d... |
| 0x005A2500 | 4793 | CI3DFigure::LoadFig8 | slot 20 of vt 0x73C224 (data**, s, d, h): the UI-figure FIG8 loader: unless flag 0x20: +0x158 / +0x15C from file +0x20 / +0x24, ReadBoundsHeader, vertex blocks blended trilinearly into own +0x1C0 (vt[7] bounds), and normals +0x1D0... |
| 0x005A7A70 | 4204 | CFigureClientSpecific::RenderLit | (device, lights {CLight**, count}, light params, ambient, out RECT, blend texture A, B) -> 1: the lit draw of every world object part: hidden (0x80) / culled -> 1; texture frame animation; unless the colours are cached (figure fla... |
| 0x005A5C70 | 4094 | CFigure::LoadFig8 | slot 20 (data**, s, d, h): unless flag 0x20: header (+0x158 materialKind = file +0x20, +0x15C textureNumber = +0x24), ReadBoundsHeader, vertex blocks blended trilinearly (FIG8) or copied (other signature) into own +0x1C0, vt[7] Co... |
| 0x005A4D60 | 3845 | CFigure::LoadFigOld | slot 21 (data**, s, d, h): the pre-FIG8 6-corner figure (288-byte vertex blocks, 244-byte bounds) into the same own arrays. Was LoadFig6 |
| 0x0059FDF0 | 3351 | CFigure::AccumulateWorldBounds | (max x*, max y*, max z*, min x*, min y*, min z*): skipped when the part's local position +0x1C has a squared length > 1e8 (parked); else the 8 corners centre + min / max of the BoundsRecord +0x188 are transformed by CObject3D::Bui... |
| 0x005A18E0 | 2967 | CFigure::ReadBoundsHeader | (cursor, a, b, c, magic): "FIG8" (0x38474946): the 8-variant trilinear interpolation (morph params a, b, c) of the centre +0x160, min +0x16c, max +0x178 (three 0x60-byte groups) and radius +0x184 (0x20 bytes); other magics read th... |
| 0x005AAE60 | 2867 | PartRecord_ReadBoundsFIG8 | (record (0x38: flags, centre +4, radius +0x10, min / max +0x14..), data, a, b, c, magic): "FIG8": the 8-variant trilinear interpolation of centre, min, max (three 0x60 groups) and radius (0x20); else raw; flag 1 and 0x4F93A0 |
| 0x005A1210 | 1736 | CFigure::ReadBoundsFIG6 | (cursor, a, b, c): old .fig bounds: three 0x48-byte groups (6 points) + a 0x60 group interpolated with the morph params into the offset +0x18c, +0x198, min +0x19c, max +0x1a8; flags +0x188 /= 3; the cursor advances DEAD CODE: no c... |
| 0x005AB9A0 | 1646 | PartRecord_ReadBoundsFIG6 | (record, data, a, b, c): the 6-variant interpolation of the old .fig bounds (0x48-byte groups) into the record; flag 1 and 0x4F93A0 |
| 0x005ACE80 | 1612 | Menus_LoadRects | thiscall (resource loader = the figure manager, RECT out[11]), called once from CFigureManager::Init with out = g_0x7C8048: vt[19]("menus.reg", 0), CFileRegistry::Attach; then 11 times CFileRegistry::GetFloatArray(section, key) in... |
| 0x0059F8F0 | 1273 | CFigure::ReadFromStream | slot 17 (stream): through stream vt[13] Read: name (u32 length + chars), u32 vertBlocks + VertexSoABlocks into own +0x1C0, u32 normalBlocks + blocks into +0x1D0, u32 uvCount + float[2] into +0x210, a padding u32, u32 componentCoun... |
| 0x005A7640 | 1034 | CFigureClientSpecific::Render | (device, quat*, lights, params, alpha, out RECT or 0) -> 1: the unlit draw: skipped when the figure (+0x1C) is hidden (flag 0x80); flag 0x40 (drawn) cleared; CullBox(device +0xAC, figure +0x188): 2 -> return; animated texture kind... |
| 0x005A8FB0 | 855 | CFigureClientSpecific::ComputeScreenRect3 | (projected points {data, count}, 4-wide SoA): SetRect(+0x20) from the min / max of the points |

## 0x5AD000 - 0x5C6000: CAIMap: terrain / passability grids, collision map, path finders, nav grid, stamps; Alertness

443 functions, group `ai`.

| addr | size | name | purpose |
|---|---|---|---|
| 0x005B43C0 | 6705 | CAIMap::Load | (map name): reads the zone's .mp header (magic 0xCE4AF672) and every <stem>XXXYYY.sec (magic 0xCF4BF774) again for the AI: tile height = 0.5 nearest vertex + 0.25 each adjacent vertex (round(h * 511/maxZ)), water depth (bits 0-5, ... |
| 0x005B6A80 | 3438 | CAIMap::StampObject | (obj, remove): units (type 0x50): FootprintStamp (radius) into the StampMap +0x12A4 (obstacle grid) when CUnit +0x278 (walking) is set, else +0x27C (terrain byte +8), not at all when +0x274; other objects: body-part boxes sampled ... |
| 0x005B9C90 | 1881 | CAIMap::FindLongPath | (unit, x0, y0, x1, y1, mover, allowRetry, maxLenTiles): NavGrid::FindPath (fails when navCells * 8 > maxLen), representative tile per nav cell + the goal as waypoints, one PathFinder chained with ShiftWindow (reopen 1 after a subs... |
| 0x005BB350 | 1454 | CAIMap::FindPath | (unit, goal[3] (z = ignored, kept at mover +0x68), mover, maxLength, minDist): the path request: reject far goals, remove the unit from the collision map, FindFreeCellNear, same tile = 1-cell path, octile distance < 25 tiles = two... |
| 0x005BAC10 | 1397 | CAIMap::FindFreeCellNear | (unit, in/out goal (metres in, tile units out), checkRegion, minDist): checkRegion 0 keeps a standable goal; else candidates goalTile - round(d * w) for w in 8 directions ordered 0, +45, -45, -90, +90, +135, -135, 180 deg from the... |
| 0x005B8FA0 | 1256 | CAIMap::UpdatePassability | (x0, x1, y0, y1): recomputes the 8 class nibbles of the rectangle (+3 margin): GetCellStepCost per tile into the scratch grid, nibble copy, erosion by the class footprint list (+0x204 5 tiles, +0x218 13, +0x22C 29), MarkSteepCells... |
| 0x005C46C0 | 1247 | CAIMap::SmoothPathStart | (unit, nav world, path, lastDir, straightness): for paths >= 3 cells re-plans the WHOLE path inside the 3-cell corridor around it (9 window slots per path cell, portal table g_0x7C3058[inDir][outDir] via NavGraph::RelaxPortal), tu... |
| 0x005C25D0 | 1105 | PathFinder::ShiftWindow | (x0, y0, x1, y1, reopen): SetWindow for the new pair, fails when the origin moves >= 31 tiles; overlap nodes keep cost / reached / backDir, new nodes INF; reopen 0: reached -> closed, unreached finite -> open lists; reopen 1: node... |
| 0x005B8BA0 | 1011 | CAIMap::GetCellStepCost | (out info {zmin, zmax, cost, slopeClass, free, flag}, unitClass, height, x, y): cost of entering a cell: special cells (flag 0x100 with no water bits) use the material's packed nibble; water depth (cell +6 & 0x3f) raises the cost ... |
| 0x005BC3E0 | 873 | CAIMap::TraceVisibility | (from, to): walks the 3D segment in 0.5 m steps (16.16 fixed point); returns 0 when the terrain is above the ray, multiplies the visibility by the object transparency table 0x791744 for every inline / sector object whose z range c... |
| 0x005B7BB0 | 822 | CAIMap::FindOverlappingUnit | (unit, pos): among the units within 10 m (CollisionMap::FindUnitsNear) that are alive and closer than the sum of the radii (+0x32c): the first whose footprint stamp (ScriptRefCache::Get on g_0x7C2F70 by radius, 16 x 16 bytes) at t... |
| 0x005BF8F0 | 794 | CellOffsets_Build | static init of the radius-sorted neighbour cell offset tables: the 21 x 21 offsets (-10..10) sorted by distance (0x6E88FC qsort) into g_0x7C2FFC (short pairs, count g_0x7C3000), the distinct distances into a temporary array and th... |
| 0x005B07C0 | 790 | NavGrid::IsCellInRegion | (class, x, y, region, byte grid): true when the tile can reach a nav cell of the region (local PathFinder, stamps ignored); marks reached tiles 1 / 2 in the scratch byte grid |
| 0x005C02E0 | 788 | CollisionMap::Ctor | (w, h, world): vtables 0x73F9DC / 0x73F9EC (+0x10); sectors of 32 x 32 cells (w >> 5 x h >> 5, 0x54 bytes each, NavSector::Ctor), +0x24 = world |

## 0x5C6000 - 0x5C8000: Mover (unit movement state)

33 functions, group `units`.

| addr | size | name | purpose |
|---|---|---|---|
| 0x005C67D0 | 1545 | MoveCurve::Build | (request): rebuilds the whole timed curve: kind 1 / 4 = turn in place, kind 2 = runs of equal direction (corner point when the direction changes >= 90 deg) -> segments -> speed limit -> optional initial turn (skipped when within c... |
| 0x005C7430 | 1046 | MoveCurve::Predict | (world, request, tick, ...): position / direction / speed / cell of a unit at a tick, by request kind (0 stand, 1 and 4 turn, 2 EvalPath, 3 face the tracked unit); the server's TickMovement, the client's ReplayMove, GetDisplayPos ... |
| 0x005C6FC0 | 1005 | MoveCurve::EvalPath | (t, &turning, pos[2], dir[2], &speed, &cellx, &celly): position, direction, speed and the cell being entered at ticks t after the request's start; per segment a Hermite cubic in distance (HermiteInit); past the end: end point, spe... |
| 0x005C6370 | 844 | MoveCurve::AddSegmentsBetween | (p0, p1, n, off0, off1, &speedIdx): appends n 0x1C-byte segments {x, y, dirx, diry, speed = cellSpeed[i] * baseSpeed / 512, cellx, celly} spread along p0->p1 at (i + 0.5 + off0) / (n + off0 - off1); n == 1 with both offsets = just... |
| 0x005C7FC0 | 589 | MoveRequest::ReadNet | counterpart of WriteNet: rebuilds the request and its cell list from net section 10 (CUnit::ReadNetState, CPlayerClient::OnMsg_UnitMoveState) |
| 0x005C7DB0 | 519 | MoveRequest::WriteNet | writes the request into net section 10 (kind, cell count, pos Q64, heading 16 bit, +0x10..+0x1C raw, per cell one u16 {x code, y code, flags, speed factor <= 0x3FF} + explicit shorts); CUnit::WriteNetState and CPlayerServer::SendM... |
| 0x005C79B0 | 330 | MoveCurve::Write | (stream): writes the segments (0x1C each), points (0xC), cell speeds (4) and +0x3C..+0x4C (save game) |
| 0x005C7860 | 327 | MoveCurve::Read | (stream): reads the segments / points / cell speeds and +0x3C..+0x4C (save game) |
| 0x005C7B00 | 244 | MoveRequest::SetStand | (pos[2], facing dir[2], tick): request kind 0 = stand at pos*2 with heading atan2(dir), start tick, clears the cell list; returns true when something changed (16 callers) |
| 0x005C6290 | 219 | MoveCurve::LimitSegmentSpeeds | (maxTurnRate): caps each segment's speed so traversing it takes at least (heading change to the next segment) / maxTurnRate |
| 0x005C6110 | 214 | dead_5C6110 | unregistered code with no call, jump or pointer anywhere in the image (gaps.py 2026-11-06) |
| 0x005C6E50 | 202 | dead_5C6E50 | unregistered code with no call, jump or pointer anywhere in the image (gaps.py 2026-11-06) |
| 0x005C6000 | 191 | PathCells_RemoveDuplicates | (list {vt, head, tail, count, free, pool}): unlinks consecutive nodes with equal (x, y) shorts into the free list; frees the pool (0x6FE90F) when the list becomes empty |
| 0x005C6F20 | 150 | MoveCurve::EvalTurn | (t, out dir[2]): heading at time t of a turn in place (+0x48 start + clamp(+0x4C delta, +0x44 rate * t)); returns 1 while still turning |

## 0x5C8000 - 0x5D3000: unit AI: perception (aiinfo.res, hearing, vision, smell)

181 functions, group `ai`.

| addr | size | name | purpose |
|---|---|---|---|
| 0x005D0030 | 1965 | AiSpellInfos::ChooseTarget | (this = brain +0xA4 the spell tables; brain, out {spell, target}, candidates list (+4 head, +0xC count), pos, filter, direct): baseline = the current order pair if IsTargetClear (returned at once when 1 candidate and no support en... |
| 0x005D2960 | 1846 | AiLogicFear::Evaluate | slot 4 (brain): jitter +0xc = (2 j + 3) / 3; with a brain attacker +0x30: SetNoticed(its pos, 1000, -2) and radius +8 = max(radius, 1.5 x distance); KITING for ranged units (CUnitServer::IsRangedAttacker, animSpeedClass < 6, no sp... |
| 0x005CE0F0 | 1577 | AiInfo_Load | (this = g_0x7C53F8 AiInfo, file; from main 0x456440, once: +0 loaded byte): CFileRegistry over aiinfo.res: [Logic] GuardStayTime +4 .. SwarmRadius +0x30; [RPG] HumanCrossAnimationSpeed +0x34, BackstabAdd +0x38, BackstabMult +0x3c,... |
| 0x005CF9A0 | 1111 | AiSpellInfo_Score | (casterPos, caster CUnit, candidates list, self CUnit, target CUnit, minScore, AiSpellInfo): base 10 when target == self; kind 0 weapon: + sum of the positive (damage - target absorption +0x8e4) shorts; 1 attack spell: + sum * 1.3... |
| 0x005CA5A0 | 1105 | StimulusMap::SeeUnits | (observer CUnit, AiMemory): VISION TEST for every neighbour (AiMemory +0x28 list): non-player observers (CUnit +0x24c == 0, flag 0x2000000 'witness' off) only look at enemies (IsPlayerInMask) or units with a player (+0x24c); alrea... |
| 0x005CAA00 | 1059 | StimulusMap::UpdateVision | (CUnitServer, AiMemory): per tick from AiMemory::Tick: eye = unit pos / cell; RefreshNearbyUnits; for every seen unit (+0x44 list): dead -> moved to the corpse lists ('sees corpse of unit'; a friendly corpse (g_0x7C2BC4) also sets... |
| 0x005C9CD0 | 882 | StimulusMap::PerceiveFootsteps | DEAD CODE (only caller PerceiveStimuli 0x5CA060 has none): would let the observer 'see' (age <= +0x904 * +0x2bc Senses Tracking * stimulus +0x14, within 1.5 m, in the view cone +0x224, TraceVisibility) or 'smell' (age <= +0x2b4 Se... |
| 0x005CF3C0 | 862 | Brain::TryAttackTarget | (brain, {spell, target}, direct): false when the spell item has flag 0x20000000 and the target is not fighting (+0x324 == 0 and proto +0x24 >= 0.5); attack position = the unit itself when direct or self-target, else the end of a p... |
| 0x005CB850 | 834 | AiMemory::ComputeDetectionFactors | (CUnitServer, out float[5] = CUnit +0x4c8, replicated 0xc for the party panel eye / ear icons; player units only, from CUnitServer::Update): for every NEIGHBOUR (+0x28 list, not only seen) that is alive and an enemy: d = 3D distan... |
| 0x005D0BE0 | 707 | AiLogicGuard::Update | slot 5 (brain): 1/1001 chance of AiAcknowledge 1; Brain::CastSupportSpell; when the brain order kind +4 == 0: waypoints (+0x50 prev = old current, +0x5c current = guard point), brain +0x44 = radius + 35, +0x48 = radius + 80 (leash... |
| 0x005C98C0 | 631 | StimulusMap::HearSound | (source CUnitServer, loudness, delayTicks, pos or 0 = source pos): for every unit within 50 m (CollisionMap::FindUnitsNear on world+0x124) for which source.side is an enemy (CUnit::IsPlayerInMask(listener, source)), whose AiMemory... |
| 0x005CEED0 | 607 | AiSpellInfos_Build | (this = brain +0xa4, CUnit): rebuilds the two tables: non-heroes with a natural weapon (+0x28c != 0x10) get {kind 0 weapon, slot -1, range GetAttackRange, damage GetDamage7} in the attack table; then every spell slot 0..7 (CUnit::... |
| 0x005C8D40 | 584 | ByteGrid_EnsureCovers | thiscall (map, cx, cy, r): the square [cx - r, cx + r] x [cy - r, cy + r] must lie inside the map: first use (originX == 0x7FFFFFFF): (re)allocates a (2r + 1)^2 map, zeroed, origin = (cx - r, cy - r); else, when the square leaves ... |
| 0x005D1450 | 568 | dead_5D1450 | unregistered code with no call, jump or pointer anywhere in the image (gaps.py 2026-11-06) |

## 0x5D3000 - 0x5E2000: unit AI: behaviours (Guard / Patrol / Sentry / CorpseWatcher / Tamed) + #say

265 functions, group `ai`.

| addr | size | name | purpose |
|---|---|---|---|
| 0x005DC620 | 4571 | Console_ExecuteLine | (line): registers the console commands on first use (string maps: g_0x7C7B28 = always: console, lastfps, exec, execute, kick, ban, rate, localrate, net; g_0x7C7B38 = cheats only (g_0x7C7B64, never in a lobby): help, debuginfo, his... |
| 0x005E18E0 | 2959 | CFlyingHitPoints::Draw | (renderer): draws the floating damage numbers (list +4 of entries {.., pos, time +0x14, text}; entries older than 3000 ms removed with 0x5E25B0); sets the digits texture (+0 via renderer vt[11], "CFlyingHitPoints::Draw. SetTexture... |
| 0x005D6630 | 1589 | Brain::BuildFromLogicSlot | (MobLogicSlot (0x4c), index): LogicList_Clear, +0x44 / +0x48 = 100000, +0x4c = 1; +0x38 callForHelpRadius = slot +0x3c (CallForHelpRadiusDefault 7 -> CallForHelpRadius); +0x3c alarmCondition = slot +0x40; Model +0: 1 = AiLogicGuar... |
| 0x005D30A0 | 1450 | AiLogicFear::Update | slot 5 (brain): flee origin = the nearest seen enemy when +0x18c (and Brain::CallForHelp(its pos)), else the unit position when inside a spell area, else the remembered spot +0x10 when that is in an area (otherwise the noticed pos... |
| 0x005DB3F0 | 1435 | Movie_PlayFile | (name, flags, p3, p4, p5, withSound): plays a Bink file to the end (handles input to skip, volume g_0x79BCC4, sound system Miles, renderer reset 0x5E5270); returns true when the file could not be opened or playback ended |
| 0x005DE460 | 1429 | Console_Show | console `show world_data/render_info/managers` (cheat table): CWorldClient debug overlay flags +0x88 (ClearStateFlag1 / HideManagerStats ...); "Invalid number of parameters" |
| 0x005D6F90 | 1410 | Brain::TickFollow | (brain; order kind 6): target +0x2c dead or gone -> kind 0; every 8 ticks (+0x6c): moving target (its order 1): predicted position 5 ticks ahead (vt[52] GetPosAt); farther than FollowMinDist 2 from it or its path (own move target ... |
| 0x005D8310 | 1296 | Brain::Read | (stream, save): ClearLogics; +4 kind, +8 pos, actions +0x14, +0x28, targets +0x2c / +0x30 (object ids), +0x3c alarm condition, +0x44, +0x48, +0x4c, waypoints +0x50 / +0x5c (0xc each), +0x3c, +0x7c count, current index; every logic... |
| 0x005D53B0 | 1235 | Brain::Tick | (brain; every tick from CUnitServer::Update 0x552300 for every living unit after AiAcknowledge::Tick): ALARM: when +0x34 alarm flag and +0x40 wait > 0: wait--, at 0 with +0x35 alarm slot != 0 -> CWorldServer::ActivateLogicSlot(slo... |
| 0x005DA880 | 1221 | Movie_PlayProgress | AfxBeginThread body (only from dead_5DAD50): CString path g_0x7AF460 + "Movies\progres.bik"; _BinkSetFrameRate(15, 1), _BinkOpen(path, 0x3000) (fails -> trace, return 0); a 0x20-byte CDXDirectDrawSurface of the movie size (rendere... |
| 0x005D4170 | 1167 | AiLogicPlayer::Regroup | (brain; AiLogicPlayer::Update when idle): only for units with CUnit +0x438 battle button == 1: keeps attacking the kept target +8 while alive and Brain::TryAttackTarget(direct) (brain kind 3, QueueOrderAttack(target, 6, 0, run 0x1... |
| 0x005E00D0 | 1143 | Console_Days | console `days ddmmyyyy [DDMMYYYY]` (an easter egg): days between a date of birth and today / a date (CTime), example 25011978 01072000 |
| 0x005DDFE0 | 1137 | Console_LogMask | console `filter` handler: log channel mask g_0x7C7B68 (1 AI, 2 EVENT, 4 GRAPHICS, 8 RPG, 0x10 UNITACKS, 0x20 NET); no argument prints each channel on / off, "none" clears all, <channel> [on/off]; "Invalid parameters" |
| 0x005DAEA0 | 1058 | Cursors_Register | (small, p2, smallSet): 0x5E60B0(g_0x7C7BD8 cursor manager, g_0x7AF080, 1 / 2, p2, smallSet); registers the cursor names "cursor_default" .. "cursor_steal" (or the "sm_cursor_*" set) with hot-spot flags (0x10 / 0x20 for the scroll ... |

## 0x5E2000 - 0x5E3000: IUIEvents defaults + CUIMessageLog (timed chat/message lines)

27 functions, group `ui`.

| addr | size | name | purpose |
|---|---|---|---|
| 0x005E2A30 | 2045 | CUIMessageLog::AddMessage | (text, colour): word-wraps the text at 0x30C px (CInterface::MeasureText font 1; DBCS aware; a '^' followed by a digit 0..7 is a colour escape: bit0 blue, bit1 green, bit2 red -> ^7 white, ^4 red, ^2 green, ^1 blue, ^6 yellow) int... |
| 0x005E26F0 | 265 | CUIMessageLog::CUIMessageLog | (CInterface): CUIWidget base with vt 0x7400DC (sub-object 0x7400A8 at +0x94): line list +0x148 (0x18 nodes: +8 text, +0xC colour codes, +0x10 time, +0x14 CUIBlitSurface), chat input text +0x150, caret +0x154, input mode +0x14C = 0... |
| 0x005E28E0 | 172 | CUIMessageLog::~body | releases CString +0x150, destroys every line, frees the list, base widget dtor 0x5FE930 |
| 0x005E2520 | 139 | NodeAlloc24_5E2520 | STL: allocate 0x18-byte node from pool; callers List24_InsertBefore_5E24E0 |
| 0x005E2670 | 113 | UIMessageLine::Destroy | fastcall (line payload {CString +0, CString +4, time +8, CUIBlitSurface* +0xc}; the UIMessageLine struct rows count from the list entry, 8 more): the surface +0xc destroyed (CUIBlitSurface::~body) and deleted, +0xc = 0; the CStrin... |
| 0x005E2990 | 79 | CUIMessageLog::Tick | slot 24 (called every frame by CInterface::ProcessPendingScreens on +0x1C): erases the lines older than 20000 ms (node +0x10 vs timeGetTime) |
| 0x005E29E0 | 69 | CUIMessageLog::BlitSurfaces | slot 22: places each line surface at (10, 10 + 16 * i), blits it (surface vt[0]) and then the base CUIWidget::BlitSurfaces (the input line). Was Layout |
| 0x005E2480 | 67 | List24_Clear_5E2480 | STL: std::list clear into 0x18-byte pool; 1 unwind funclet(s) (0x728da6) |
| 0x005E24D0 | 65 | dead_5E24D0 | unregistered code with no call, jump or pointer anywhere in the image (gaps.py 2026-11-06) |
| 0x005E24E0 | 41 | List24_InsertBefore_5E24E0 | STL: allocate and link a 0x18-byte node; callers Timer_Schedule |
| 0x005E2620 | 41 | Quat_StaticInit_Identity4 | static initializer (pointer 0x783740): g_0x7C7B98 = {1.0, 0, 0, 0}; the only reference to it in the exe (xq refs) |
| 0x005E25B0 | 40 | List24_EraseNode_5E25B0 | STL: unlink node to 0x18-byte pool; callers CFlyingHitPoints::Draw |
| 0x005E25E0 | 32 | StaticInit_NoopGuarded_f5E25E0 | compiler: per-TU initialiser of a header static object with an empty ctor and an empty dtor (0x4013D0 is a bare ret): sets bit 0 of the shared guard g_0x7CAEE4 and registers the dtor with atexit; no effect (163 copies in the exe) |
| 0x005E2600 | 31 | StaticInit_Vec3Zero_g7C7BA8 | per-TU static const zero vector g_0x7C7BA8 = (0, 0, 0) (3 floats, a math header constant copied into every TU next to its 1.0f / identity-quaternion copies); unreferenced (every use was folded by the compiler) |

## 0x5E3000 - 0x5E7000: chat/console panels (CUIMessageLog edit, CUIConsole), software cursor (CSoftCursor), ScreenFade, CI3DFigure

83 functions, group `renderer`.

| addr | size | name | purpose |
|---|---|---|---|
| 0x005E5690 | 1453 | CSoftCursor::ThreadProc | thread body (AfxBeginThread from Start when kind != 1): saves the background once, then every 41 ms (Sleep to 0x29) while the window is active and until the stop flag +0x9C: UpdateState; when drawn (+0xC) and changed: if the old (... |
| 0x005E5C40 | 1125 | CSoftCursor::ComposeFrame | (frame, dstRect, srcRect): locks the frame surface and the 32x32 scratch (+0x60), for each row: scratch = (scratch & AND mask row from frame +0x1C) / frame pixels (16 or 32 bpp), unlocks ('Can't lock surface.' / 'Can't unlock surf... |
| 0x005E4A10 | 789 | CUIConsole::RedrawInput | draws the input band (0x282828, 31 px at the bottom): '> ' + text in white, selection in 0x505050, caret as a 1 px line |
| 0x005E44A0 | 777 | CUIConsole::OnChar | (char): backspace deletes the char / selection, Esc clears the line, Enter: pushes the line into the history (30 max), echoes it ('> ' prefix g_0x792DE0) via AddText and runs Console_ExecuteLine; printable chars are inserted at th... |
| 0x005E3230 | 709 | CUIMessageLog::OnKeyDown | sub-object slot 9 (chat edit, only in input mode +0x14C): backspace, Delete, Home/End, Left/Right move the caret +0x154, Enter sends the text +0x150 as a lobby chat message (LobbyClient::AddKeyedRef2604 with a 0x18-byte string mes... |
| 0x005E4180 | 709 | CUIConsole::OnKeyDown | (vk): 0x10 shift / 0x11 ctrl flags; PgUp/PgDn scroll by 1 (Ctrl: a page +0x70); End/Home: caret to end/start (Ctrl: scroll to newest / oldest); Left/Right move the caret (+0x2C follows unless shift = selection); Up/Down walk the c... |
| 0x005E4EB0 | 677 | CSoftCursorFrame::CreateFromMmp | (this 0x20-byte frame, '<name>.mmp' CString buffer, hotX, hotY, image data): creates a DirectDraw surface from the decoded .mmp picture (CDXDirectDrawSurface::CreateFromPicture; texture manager vt[19]/[20] decode), +8 = 0x7C, +0xC... |
| 0x005E4800 | 524 | CUIConsole::RenderHistory | clamps the scroll offset, fills the history area with 0x282828, draws the visible output lines bottom-up (grey 0xB4B4B4, 15 px apart) and scroll markers; the surface is only drawn when visible |
| 0x005E6D90 | 517 | CI3DFigure::Draw | (renderer I3DFigureImpl) -> 1: when visible (+0x220): renderer vt[11] SetTexture(0, texture +0x280) (FatalError "CI3DFigure::Draw: SetTexture failed"); the model matrix (Quat_ToMatrix4 of +0x34 into +0x54, position +0x1C in column... |
| 0x005E63F0 | 506 | CSoftCursor::AddFrame | (name, hotX, hotY, data): loads '<name>.mmp' through the texture manager into a new 0x20-byte CSoftCursorFrame (CreateFromMmp) and appends it to +0x68 (count +0x6C); Cursors_Register adds the 25 'cursor_*' (or 'sm_cursor_*' for th... |
| 0x005E3700 | 481 | CUIMessageLog::RedrawInput | shows / hides the input blit surface (+0x118[0]) with the input mode and draws the text +0x150 (white, DT 0x800) and a caret line at the measured width |
| 0x005E69C0 | 444 | ScreenFade::Draw | (renderer): with the letterbox flag +0x10 draws two black bars (renderer vt[19] fill rect with render state 7 off) at the top and bottom; when active (+8): value += (now - +0xC) * rate, clamped to 255 (stops below 0) and fills the... |
| 0x005E52E0 | 438 | CSoftCursor::UpdateState | advances the animation frame every 125 ms (frame +0x10 index += elapsed / 125 mod count), accumulates the DirectInput delta into +0x88/+0x8C, cursor position from GetCursorPos (or the stored software position +0x80/+0x84 when +0x9... |
| 0x005E60B0 | 426 | CSoftCursor::Init | (device CDXDirectDraw, kind 1 = compose in App_RenderFrame only / 2 = also the 41 ms thread on the front buffer, DInputMouse, small): cursor size g_0x7C7C90/94 = 32 (or 16 when small = software device), event +0x1C, device +4 (ref... |

## 0x5E7000 - 0x5EE000: UI: camera input handlers, UIQuadMesh, unit preview/portraits, camp screen widgets

90 functions, group `ui`.

| addr | size | name | purpose |
|---|---|---|---|
| 0x005ECA10 | 3316 | UIUnitPreview::DrawItemFigure | (renderGlobals, slot {item, x, y, z, size, kind}) 3D item icon of an inventory / camp slot: model (item vt[20]) and texture (vt[19]) through the name caches (GetDefaultQuad / LoadPictureTo256Target, TrimPortraitCache / TrimFaceCac... |
| 0x005E8450 | 2050 | UIQuadMesh::InitBorderRing | (z, scale, l, t, r, b, border, u0, v0, u1, v1): 8 vertices (4 outer corners +-w/2 and 4 inner corners inset by border), 24 indices = 8 triangles forming the ring; the inner uv is the uv rect inset by border * (u1 - u0) / (r - l) (... |
| 0x005E91F0 | 2009 | UIQuadMesh::InitRing | (z, scale, cx, cy, radius, n, thickness, u0, v0, u1, v1): n quads (4n vertex->uv entries, 6n indices) around a circle between radius and radius - thickness (vertex pairs i / i+1 outer and inner), every quad with the whole uv rect;... |
| 0x005E7D60 | 1772 | UIQuadMesh::InitBorderStrips | (z, scale, l, t, r, b, border, u0, v0, u1, v1) 8 vertices (outer / inner corners) and 16 triplets = 4 mitred trapezoid strips top / right / bottom / left, each with the whole uv rect (v from the outer to the inner edge); centre po... |
| 0x005EC300 | 1768 | UIUnitPreview::Render | (renderGlobals, unit, yaw, x, sixRects) camp hero preview drawn straight into the frame: Clear depth (+ stencil) over the target and the preview rect(s) (UI 200..600 / 410..700 x 100..500, or the six rects of tables 0x7402F8 / 0x7... |
| 0x005E7680 | 1758 | UIQuadMesh::InitGrid | (z, scale, x, y, x2, y2, u0, v0, u1, v1, cols, rows): (cols+1)*(rows+1) own vertices (SoA blocks of 4 at +0x1C0) spaced (w/scale)/cols, (h/scale)/rows from the TOP-LEFT corner (position = SetScreenPos2D of (x, y), not of the centr... |
| 0x005ED7B0 | 1475 | UIUnitPreview::DrawFaceFigure | (renderGlobals, {prototype name, x, y, z, size}) hero face figure: UnitsDb entry model +0xE8 equal to unhuma / unhufe picks the sex part, figure infa%sme%dface + texture face%s%02d through the item-icon caches, tilt quat(-x, (y - ... |
| 0x005E70E0 | 1431 | UIQuadMesh::InitRect | (z, scale, x, y, x2, y2, u, v, texW, texH): builds a CI3DFigure quad: 4 vertices (structure of arrays at +0x1C0: x[4] then y[4]) at +-(w/2)/scale, uv rect (+0x210, inset by 1/512), 2 triangles (+0x1E0), one 0x40 draw record (+0x1D... |
| 0x005E8C60 | 1412 | UIQuadMesh::InitDisc | (z, scale, cx, cy, radius, n, uc, vc, ur) n vertices radius / scale * (cos, sin)(2 pi i / n) around (cx, cy), uv circle (uc + ur cos, vc + ur sin) texels, fan (0, i+1, i), no centre vertex; the round gauges of the battle HUD (CUIS... |
| 0x005E99D0 | 1368 | UIQuadMesh::InitTriangle | (z, scale, x0, y0, x1, y1, x2, y2, u0, v0, u1, v1, u2, v2) one textured triangle (indices 0 1 2): vertex 0 at the origin (SetScreenPos2D of x0, y0), vertices 1 / 2 = (xi - x0, yi - y0) / scale, explicit uv per corner (u/256, 1 - v... |
| 0x005EB210 | 812 | CamCtlFree::OnMouseMove | slot 4: not dragging: edge scrolling when CameraBorderScrollArea (g_0x79BDAC) > 0: x < margin / >= width - margin and y likewise give a direction mask (1 left, 0x10 right, 0x100 up, 0x1000 down) -> scroll velocity (-1/+1 per axis,... |
| 0x005EC020 | 717 | UIUnitPreview::LoadPictureTo256Target | (name): CTextureManager::CreateRenderTarget 256x256x32 (refcount++), decodes '<name>.mmp' through the texture manager vt[19] (width +4, height +8, pixels +0x4C, 'PNT3' = DXT flag) into ARGB and writes it row by row into the target... |
| 0x005EDD80 | 652 | CampScreen::CreateFrameWidget | (screen): creates the 0x148-byte UIFrameWidget (vt 0x740444, slot 18 BuildFrameMesh) and registers the layout groups 'modifiers2' and 'campslots' (tutorial-registered widget names); used by CCampScreen2::Build, CUIScreen740D18::Bu... |
| 0x005EADD0 | 650 | CamCtlFree::OnKeyDown | slot 9: keyboard.ini action of the scancode (g_0x79BF70[flags & 0x1FF]): 5/6 camera_up/down -> forward +0x20 = +1/-1, 7/8 camera_left/right -> side +0x24, 9/10 zoom_in/out -> zoom +0x28, 0xD..0x10 camera1..4 -> CCamera::LoadView(0... |

## 0x5EE000 - 0x5FD000: UI: CInterface screen manager, item/spell/perk info panels, CUIBlitSurface, string-pair arrays, STL leaves

344 functions, group `ui`.

| addr | size | name | purpose |
|---|---|---|---|
| 0x005F1590 | 4904 | UIItemInfoWin::DrawPerkDetail | (perk record*: +0 kind, +0xC item, +0x14 name): the skills window detail panel (4.9 KB): kinds 0..3 read the 'perk <name>' text section (ReadLineValue x2), kinds 4..7 the '<name>_desc' text; PerksDb_FindIndexByName(g_0x7AF268) the... |
| 0x005EE760 | 2968 | UIItemInfoWin::Refresh | redraws the item info surface (+0x194 selected item): kind 0x3007 loot: category 1 -> DrawMaterialInfo, category 2 subtype 0..2 -> DrawWeaponArmorInfo, 3 -> DrawSpellInfo, 4 -> DrawPerkInfo; 0x3006 quick item subtype 8 -> DrawItem... |
| 0x005F0370 | 2070 | UIItemInfoWin::DrawWeaponArmorInfo | loot category 2 subtype 0 weapon (WeaponProto g_0x7C86F8, 0xB4 each) / 1 armor (ArmorProto g_0x7C8718, 200 each) / 2 (g_0x7C8738, 0x8C each) by +0x50 and +0x54: name, '%s %s' 'mattype_' text of +0x10, '%s %s x %d', '%s %d' price /... |
| 0x005EF5D0 | 1979 | UIItemInfoWin::DrawSpellItemInfo | spell container / spell item panel (kind 0x3008, or Ctrl held on a spell): texts 'spell <code>' section (CTextManager::OpenText / ReadLineValue / SkipToSectionEnd) and 'item_spell', 'school_%d', 'modifier' rune lines from the perk... |
| 0x005F4990 | 1521 | CInterface::Init | (device CDXDirectDraw g_0x7AF080): +0x8 = device, screen size +0/+4 from the primary surface description (used by UiScale); fonts +0x70..+0x78: 'Times New Roman' 75 pt measured to get the pixel scale then 3 fonts of size * 0x68 / ... |
| 0x005EFD90 | 1494 | UIItemInfoWin::DrawMaterialInfo | loot category 1 (materials): record = g_0x7C86D8[+0x54] (0x50 each): name, '%s %s' with the 'mattype_<NAME>' text (+4 lower-cased), labels +0x1CC[17] ('%s %d' price / weight / ...), '%s %.1f', then FormatResistLists over the 7 flo... |
| 0x005F0B90 | 1431 | UIItemInfoWin::DrawSpellInfo | loot category 2 subtype 3 (spell scrolls): SpellProto = SpellProtoTable_GetAt(g_0x7C8510, +0x54): name white, 'school_%d' line (labels [18]), '%s %d' / '%s %s' for mana, range (+0x20 speed >= 1), area (+0x28 >= 1), duration (+0x38... |
| 0x005F5320 | 991 | CInterface::Draw2D | (after EndFrame): debug lines 'surface stored' / 'surface alloc' (g_0x7C7E74 / g_0x7C7E78 bytes); top screen vt[22] BlitSurfaces and vt[23] UpdateHoverAndButtons; message bar +0x64; transition +0x50: 1 -> grabs the back buffer int... |
| 0x005F2A90 | 876 | UIFrameWidget::BuildFrameMesh | vt 0x740444 slot 18: the camp screens' full-screen frame art: one InitGrid 8x4 tile (z 8, uv 14..114) over (0, 100)-(800, 500), then per 200 px column two InitGrid 1x4 side strips (z 7.9, uv 254,76-233,176 mirrored) and 4 InitRect... |
| 0x005F5BA0 | 860 | CInterface::CreateScreenById | (id from CPlayerClient::OnMsg07_GameMode): 1 battle HUD CUIScreen740764 (0x230, vt 0x740764 / sub 0x740730), 2 CMainMenu (0x17C, 0x740688 / 0x740654), 3 DialogScreen (0x2A4, 0x7405AC / 0x740578), 4 CUIScreen740B3C (0x294, main men... |
| 0x005FAB80 | 771 | RbMap_InsertHint_005FAB80 | thiscall (tutorial map<CString, int>, out, hint, value*): byte-identical to StrTree_InsertWithHint 0x5E0E90 except the call targets: STLport insert_unique(hint, v) with strcmp on the CString key +0x10, entries from NodeAlloc18_Cop... |
| 0x005F57C0 | 748 | CInterface::ProcessPendingScreens | once per frame from main: while the pop request +0x4C is set: cursor wait (frame 3), modifiers cleared, top screen Deactivate (sub vt[1]) + OnClose (vt[26]) + delete, App_PumpMessages, transition 4 -> restore the parked screenshot... |
| 0x005EF300 | 711 | UIItemInfoWin::DrawItemOrSpellInfo | quick item (kind 0x3006 subtype 8) panel: CTextManager::GetItemTextKeys (name, description, shape keys) of the selected item (+0x190 list head, +0x194 count); on the 200x300 surface (+0x118[0]) rect (15, 10)-(185, 25): name white ... |
| 0x005F3470 | 703 | RbMap_InsertHint_005F3470 | thiscall (portrait map<std::string, ..>, out iterator, hint, value*): STLport _Rb_tree::insert_unique(hint, v) with StringRange_Less on the key at +0x10 and 0x28-byte entries (NodeAlloc28_CopyCtor_5F4390): hint == begin: v < begin... |

## 0x5FD000 - 0x602000: UI: CUIWindow (element/quad/button/blit-surface arrays, event dispatch), CUIBlitSurface (DirectDraw backing surface)

106 functions, group `ui`.

| addr | size | name | purpose |
|---|---|---|---|
| 0x005FDFA0 | 838 | CUIBlitSurface::DrawWrappedText | (text, rect*, maxLines, format, colour, lineHeight, shadow): word-wraps with CInterface::MeasureText (spaces / newlines) into at most maxLines lines drawn with DrawText lineHeight apart; returns the height used; 34 callers |
| 0x005FD440 | 792 | CUIBlitSurface::StretchTo | (w, h): new CUIBlitSurface of that size with this surface stretch-blitted into it (and the backup buffer copied); the 256x192 transition thumbnail |
| 0x005FD7A0 | 734 | CUIBlitSurface::SuspendOrRestore | (0: lock the surface, copy its pixels into a heap backup +0x1C (size +0x20, counted in g_0x7C7E74) and release the DirectDraw surface; 1: recreate the surface and copy the backup back) - screens below the top one park their surfac... |
| 0x005FE440 | 699 | CUIBlitSurface::ToGrayscale | converts every pixel to luminance (16-bit 565 / 555 and 32-bit) - the frozen background behind message boxes |
| 0x005FFFB0 | 697 | CUIWidget::BlitPictureIntoTexture | (texture, name, x, y): decodes '<name>.mmp' the same way (device decoder via CInterface +8 +0x4C) and writes it as A1R5G5B5 into the texture's own surface (+0x5C) at pixel offset x + y * 256 (a 256-wide texture page); returns true... |
| 0x005FF460 | 660 | CUIWidget::CloneQuadMesh | (figure): replaces a quad figure of +0x108 by a fresh copy (position, scale, texture copied). Was CUIWindow::CloneQuadMesh |
| 0x005FE930 | 599 | CUIWidget::~body | releases the materials +0xEC/+0xF0/+0xF4, the quad figures (vt[4]), the blit surfaces, the buttons (UIButton::Destroy), the textures (vt[4]), the figure holders, frees the arrays and runs the CObject3D base dtor; 48 callers. Was C... |
| 0x005FE700 | 498 | CUIWidget::CUIWidget | (CInterface): base of every UI window (0x148 bytes, vt 0x740F60 25 slots, IUIEvents sub-object vt 0x740F2C at +0x94): CObject3D transform (+0x1C pos, +0x34 / +0x44 quaternions), +0x98 CInterface, +0x9C handled, +0xA0..+0xD4 zero, ... |
| 0x005FFD80 | 438 | CUIWidget::AddFigureHolder | new CI3DFigureHolder appended to +0x138 (count +0x13C); returns it. Was CUIWindow::AddFigureHolder |
| 0x005FEF20 | 392 | CUIWidget::AddTextureByName | (name, kind, flags): CTextureManager::FindOrCreateNamed + LoadTexture, appends to +0xF8; the first texture is bound to every quad (AddQuadMesh); 50 callers. Was AddSurfaceFigureByName |
| 0x005FF830 | 391 | CUIWidget::AddButton | allocates a 0x34-byte UIButton (+0 CInterface, polygon +4.., rect +0x14, flags +0x24/+0x25, origin +0x28/+0x2C, tooltip +0x30) and appends it to +0x128 (count +0x12C); 158 callers. Was CUIWindow::AddButton |
| 0x005FDA80 | 384 | CUIBlitSurface::Blt | vt slot 0: when visible (flag 4) blits the surface to the back buffer at its rect clipped to the screen (DDBLT_WAIT, or DDBLT_KEYSRCOVERRIDE 0x1010200 with the colour key +0x34 when flag 1); called from CUIWidget::BlitSurfaces. Wa... |
| 0x00601ED0 | 384 | CDemoScreen::OnOpen | vt 0x73F7F8 slot 25 of the attract-mode demo screen (0x184 bytes, CUIWindow_73F7F8::Create): once (+0x16C): enumerates 'demo%d.apl' recorded sessions into the CStringArray +0x170; adds a CamCtlFree handler, clears the camera free ... |
| 0x005FF240 | 379 | dead_5FF240 | unregistered code with no call, jump or pointer anywhere in the image (gaps.py 2026-11-06) |

## 0x602000 - 0x60E000: UI: dialog screen (speakers, topics, briefing), status-lamp screen, STL leaves

148 functions, group `ui`.

| addr | size | name | purpose |
|---|---|---|---|
| 0x0060C870 | 6549 | CCampScreen::Build | vt 0x74091C slot 25 (screen id 5, camp / shop): Game_SetServerPaused(1); merchant +0x198 = g_0x7AA5D8 by id trunc("constr.current") (id > 0 not found -> Game_SendEnterWorld; id <= 0 -> no merchant); +0x195 spell shop = CGSPlayer +... |
| 0x00604C50 | 4482 | DialogScreen::ParseDialogScript | (skipToEnd): continues "briefing <topic>" to the next #phrase: #nolips, #show (AddSpeaker), #hide (RemoveSpeakerByName), #animation n, #camera shot [yaw dist h], #phrase name [voice]: "\aC<FFB331>label:\n\aC<E4D7A7> text\n" (grey ... |
| 0x00604130 | 2827 | DialogScreen::Update | vt 0x7405AC slot 24: walk nodes +0x240 (state 1 -> 2 within 0.1 m + OrderTurnTo, 2 -> 0 when facing, 30 s -> teleport); mode 1 topic scroll; modes 0/1: SendMsg19(1, id) for the +0x284 heroes having a 3-part "b.<hero>.<topic>" == 1... |
| 0x006064E0 | 2645 | DialogScreen::SelectAnswer | (isBriefing, flag): +0x27C = isBriefing; "goodbye" -> mode 0; "constr*" topic -> World_SetConstrCurrent(id from chars 0..6) + Game_SendLeave(5) (enter the camp / constructor); opens the text "briefing <topic>" (+0x184): its "#show... |
| 0x006024B0 | 1742 | DialogScreen::Build | vt 0x7405AC slot 25 (screen id 3, zone talk mode): +0x27E = 1 only when the UI stream g_0x7AFE1C is empty (fresh build) and the client mode (connection +0x1C +0x104) != 5, else the stream is emptied; LoadPersonalities; figure "sav... |
| 0x006034A0 | 1610 | DialogScreen::OnKeyDown | vt 0x740578 slot 9: DispatchSlot9; Backspace in a lobby = ClearChatLines; Enter: lobby = EnableChatInput, mode 3 = ParseDialogScript(1) until mode != 3 (skip the line), mode 4 = StartDialogLine; Esc: mode 0 pushes CEscMenu (0x168,... |
| 0x00608450 | 1405 | DialogScreen::EndBriefing | (was StartDialogLine; ends the briefing, callers: OnLButtonDown mode 4, OnKeyDown Space / Enter / Esc mode 4): stop voice +0x250, Music_FadeIn, remove NPC portraits +0x278; per speaker +0x294 SendMsg19(2, id), mouth closed, SetVis... |
| 0x00605DE0 | 1231 | DialogScreen::AddSpeaker | (name, portraitIndex, unused): "#show" of ParseDialogScript: a live unit (Game_FindUnitById(String_HashCI)) is re-appended to +0x294 (Record12 {unit, name, byte}), SendMsg19(1, id), SetVisibleToAll(0) for the player unit +0x1A4, b... |
| 0x00603D00 | 1058 | DialogScreen::OnMouseMove | vt 0x740578 slot 4: lobby modal +0x2A0 -> cursor 0xD, forwarded; mode 1 hovered topic row -> +0x188 + DrawTopicList; mode 3 cursor 0xE, 4 cursor 0; mode 0 (no button held): an idle living unit (brain +0x190 0 / 0xB, +0x1B4 == 9; n... |
| 0x006091C0 | 977 | CLobbyStatusBar::RebuildPlayerButtons | clears quads/buttons/elements; +0x150 = the lobby players (g_LobbyClient +0x25D8 list, +0x25F0 = the local player first), +0x148 = count; for each: unit prototype of its hero (UnitsDb), face "infa%s%s%dface" (race "unhuma"/"unhufe... |
| 0x00607360 | 952 | DialogScreen::LoadSpeakersFor | (unit id hash) [static, writes the dialog screen's topic array +0x170/+0x178]: every "b.<unit>.<topic>" variable == 1 (World_ApplyVariableTable "b.") whose unit hash matches: "constr*" topics kept last, the others sorted (IntroSor... |
| 0x00602C90 | 896 | DialogScreen::OnPartyChanged | vt 0x7405AC slot 29 (unit record): inserts the unit into the portrait array +0x190/+0x194/+0x198 at its party index (party records 0x784 bytes, +0x1C id); when it is the host player's own unit (party +0x2C [1] +0x1C): +0x1A4 = uni... |
| 0x00607EC0 | 798 | DialogCamera::SetShot | (shot 1..12): 1 both from 3 x the speaker distance at height 1.5; 2 player close-up (-30 deg, 4 m, 1.5; the other sunk to -100 by World_SetUnitHeight); 3 second speaker (+30 deg); 4 third (0 deg); 5 both 2.7 x d, 15 deg, height 3;... |
| 0x00607BB0 | 782 | DialogCamera::FocusOn | (unit or 0, yaw rad, distance, height): target = unit pos (the stored pose +0x1B0/+0x1BC/+0x1C8 for the three speakers, forward dir +0x1D4/+0x1E0/+0x1EC) + dir * proto radius (+0x28), height + proto +0x2C; eye = target + rotate(di... |

## 0x60E000 - 0x645000: UI: camp/inventory/trade/spell+item constructor, hero stats, main menu, save/load, lobby, battle HUD, credits

684 functions, group `ui`.

| addr | size | name | purpose |
|---|---|---|---|
| 0x0062D350 | 6721 | CWorldMapScreen::Build | vt 0x740B3C slot 25 (screen id 4): current zone -> allod record; textures Map<allod> / Sys<allod> / MapG<allod> / saveload + 256x256 target; zone entries +0x190 (every kind-0 zone of the allod with a non-empty record +0x10: +0 tru... |
| 0x00613EA0 | 6425 | CCampScreen2::Build | vt 0x74214C slot 25 (Build): the multiplayer party / hero-creation screen (0x220 bytes, pushed by CMainMenu button 5): Databases_Reload; figures "campinfo", "MainMenu00"/"MainMenu00labels" (holder +0x138[0] at (400,350) depth 8, p... |
| 0x00643DC0 | 4142 | CHudMinimap::DrawMinimapUnits | 4 KB: unit / zone-exit markers (CWorldClient::GetZoneExitCenter, "gz20g" exits, "none"), colours by relation, written into the marker arrays. Was CUIScreen742A18::DrawMinimapUnits |
| 0x0061B2C0 | 4110 | CLobbyScreen::Build | vt 0x741CD0 slot 25: the multiplayer lobby (0x260 bytes, pushed by CCampScreen2::OnStart): figures "saveload", "scrollbar", "MainMenu00" (+ "unmoco2", parts), buttons 0 join (0xA3,0x20E)-(0xED,0x23E) tip 0x9E98, 1 back (0x233..0x2... |
| 0x0063A560 | 3961 | CBattleHud::OnKeyDown | vt 0x740730 slot 9 (key, char, scancode): lobby: Enter = EnableChatInput, Backspace = ClearChatLines; Esc = cancel the command mode (+0x17C/+0x180/+0x184, highlights) or push CEscMenu; then by keyboard.ini action g_0x79BF70[scanco... |
| 0x00642E80 | 3897 | CHudMinimap::Init | minimap geometry: centre +0x14C / +0x150 = camera point g_0x79B388 / 38C, direction +0x154 / +0x158 from the camera (g_0x7AF7C8 +0x34) up axis -> rotation of quad 10; the 16 disc points +0x178 -> UVs +0x188 = centre + p / zoom +0x... |
| 0x00638EF0 | 3651 | CBattleHud::IssueCommandAt | (this = the window; target object or 0, x, y): the left click -> order: no selected unit (+0x16C flag 0x10) = nothing; alt held (CInterface +0x7E) = SendMoveOrder(point or target pos, formation 0, shift) (attack-move); a called-sh... |
| 0x00641080 | 3162 | CHudMainPanel::Update | vt 0x742A18 slot 24 (portraits): HP bar +0x17E / +0x180 and mana bar +0x182 / +0x184 as UV fills; HP below record +0 -> red flash (+4 = 1 s) and face variant 2 for 2 s (variant 1: 4 s); variant 3 while +0x184 == 0 or +0x182 / +0x1... |
| 0x0061D780 | 2911 | CLobbyScreen::LayoutLobby | 2.9 KB: draws the server table on the surfaces (columns "%s %s" name / "%d/%d" players / "%s %d" ping, a question mark when unknown, "%s %d / %d"), column separators (CDC::LineTo), tooltips, the detail text (DrawWrappedText) of th... |
| 0x00623880 | 2645 | CUnitInfoWin::Redraw | draws the hero pane (name at (0,0x1B)); mode +0x158 10 (new multiplayer hero): Strength / Dexterity / Intelligence (base stats +0xC / +0x14 / +0x1C), lmp_points_left = trunc(75 - sum), lmp_pers_tall +0x170, "-" grey when stat <= 1... |
| 0x00635250 | 2596 | CBattleHud::Build | vt 0x740764 slot 25 (screen id 1, 0x230 bytes): unit id list +0x1DC = the hero only (first client party unit +0x1C, +0x200 = 1) or, on a load, the UI stream g_0x7AFDF8 (count + ids, 4 bytes, pause byte -> Game_SetServerPaused, v >... |
| 0x00641F20 | 2561 | CHudMinimap::Build | (maxZoom 1.0, shown flag CInterface +0x8B): called by CBattleHud::Build on the +0x1AC object (vt 0x742660), not on CHudMainPanel: rect (0x267,0)-(800,0xA5); zone map size = record +0x20 / +0x24 sectors * 32 (+0x15C / +0x160, 32 x ... |
| 0x00611930 | 2361 | CCampScreen::EnterMode | recomputes the confirm state for tab +0x190: +0x20C = price total, +0x210 = confirm allowed, +0x211 = cancel allowed, +0x212 = deconstruct flag, title key: 0 "camp_repair_all" (sum of GetItemPrice(.., 8) over the hero's items, ite... |
| 0x00628F80 | 2349 | CCampItemConstrWin::Draw | vt 0x7415D4 slot 20: components of +0x150: usable item at (250,300), blueprint proto figure at (350,250), spell container at (350,350), materials 2 x 4 from (450,150); pulsing marker on the material cells i with count < i <= proto... |

## 0x645000 - 0x65F000: UI: battle HUD strips, scroll bars, edit/list controls, save/load, options, tutorial, key config; DirectInput mouse; RichEdit

389 functions, group `ui`.

| addr | size | name | purpose |
|---|---|---|---|
| 0x00659420 | 4310 | CTutorialScreen::BuildPage | rebuilds page +0x184 of "tutorial <key>" (the (page+1)-th "#screen"; missing -> close): "#position x y right [lines]" (right >= x + 300; bottom = y + lines * 16 + 110, <= y + 600; without lines the body is measured first), default... |
| 0x0064BE80 | 3459 | CQuestLogWin::Build | vt 0x7411BC slot 25: render state 0x18 saved in +0x1EC and set to 1; "obj_0..2" into +0x1D8; CTextListWin (0x3C,0xBE)-(0x180,0x1FE), CScrollBar +0x1BC (0x3C,0x1E)-(0x180,0x96) step 3; art "saveload" + "cross"; surfaces [0] (0x32,0... |
| 0x0064CC10 | 3412 | CQuestLogWin::ShowMap | (map name): +0x184 = name; extra textures / quads / buttons beyond 2 / 0x15 / 9 removed; zone record +0x14 figure on quad 3 centred (580,190), larger side 300 px; per exit of record +0x4C (0x3C, "none" skipped, index counts all): ... |
| 0x00655D40 | 2301 | CSaveLoadWin::ScanSaves | rebuilds the slot vector +0x164 (0x44-byte SaveSlotRecord): every directory "<root>saves\save*": folder name -> +4; its "info.sav" (CFileFind; missing -> error flag +0x40 = 1, label "Error Save") read with Stream_CheckHeader4(0x11... |
| 0x006462A0 | 2203 | CHudUnitInfoPanel::Draw | vt 0x7426F8 slot 20: 2.2 KB: the selected unit's figure (CI3DFigure::Draw with the damaged parts marked via SetPartsValueD4/D8 / flags), progress bars (DrawBar), body parts (DrawParts), stats (DrawStats), spell info (DrawSpellInfo... |
| 0x006456E0 | 2201 | CHudUnitInfoPanel::Build | 2.2 KB: the sliding unit info panel: tab buttons (w_info1-4 tooltips), "in25arrow" art, body-part colour markers ("#iFF" codes), "infounit_%d" texts, "hl_skull" / "hl_torso" / "hl_leg" / "hl_arm" labels into +0x1B8.., InitGrid / I... |
| 0x00647040 | 1949 | CHudUnitInfoPanel::DrawStats | 1.9 KB: "%s %d/%d" HP / mana, "move_%d" stance speeds (CUnit::GetStanceSpeed, GetAttackModeSpeed), "dmg_%d", damage list (UIItemInfoWin::FormatDamageList), skills (UnitStats_WeightedSkills7), level, "consider_more%d" / "consider_l... |
| 0x006548A0 | 1835 | CSaveLoadWin::OnKeyDown | vt 0x741354 slot 9: ignored while sliding; DispatchSlot9; Enter = OnConfirm, Esc = cancel (close 0x100001), PgUp / PgDn +-16 slots, Up / Down +-1 (select.wav, ClampSelection); unhandled keys in save mode (+0x178): editing of the s... |
| 0x0064DC60 | 1742 | CQuestLogWin::FillObjectives | when +0x1A0 != +0x1A4: g_0x7C8178 = id; "quest <id>" description (beige) + rewards of g_0x7C87B4[id] (white "obj_cost_exp %d" from +0xC, "obj_cost_money %d" from +0x14) + every "#subobj <n>" before the next "#quest" whose "q.<map>... |
| 0x006567A0 | 1634 | CSaveLoadWin::RefreshSaveList | redraws surface [0]: 16 rows from the scroll +0x174: label (edited text +0x188 for the selected slot in save mode, with a caret line at the caret position +0x190 unless a special slot) at (0xC,0xB)-(0xBE,0x20), the CTime "%d/%m, %... |
| 0x00653DD0 | 1629 | CSaveLoadWin::Build | vt 0x741388 slot 25: rect (0,0)-(800,600) (+0xE4 written as the bytes 0x58 0x02 0x00 0x00); art "saveload" + 256x256 scratch figure; quads 0 / 1 / 2 = load-or-save / cancel / delete button art (163..237 / 563..637 / 363..437 x 526... |
| 0x006503C0 | 1291 | COptionsScreen::Build | vt 0x741FAC slot 25: full-screen (0,0)-(800,600); art "saveload" + a 256x256 scratch surface figure; quads 0 / 1 = OK / cancel button art (213..287 / 513..587 x 526..574), buttons 0 OK (0xD5,0x20E)-(0x11F,0x23E) tip 0x111D4, 1 can... |
| 0x006570E0 | 1226 | CSaveLoadWin::QuickSave | static (ecx = slot folder name "saveq" / "savea" / "saveb", renderFrame): not in a lobby; free disk space check (silent); App_RenderFrame when asked; grabs the back buffer (CUIBlitSurface 800x600 BlitToBack), busy cursor, ShowMess... |
| 0x00650E40 | 1221 | COptionsScreen::OnKeyDown | vt 0x741F78 slot 9 (vk, char, scancode): ignored while sliding; DispatchSlot9; waiting for a key (+0x3F4): Esc cancels; the scancode & 0x1FF looked up in the key-name table g_0x7857B0 (88 x 0x48: +0 scancode, +4 name, +8 CString) ... |

## 0x65F000 - 0x664000: CParty (hero records 0x784 bytes with four item arrays: equipped / B / inventory / spells, party items, named variables, map name), HeroRecord save reader, PartyStore (*.mp folder), PartySlot::SaveToFile, RichEdit credits scroller

95 functions, group `world`.

| addr | size | name | purpose |
|---|---|---|---|
| 0x0065F9B0 | 1061 | HeroRecord::Read | (stream): ReleaseItems; strings +0, +8, +0xc, +0x10, +0x14 (+0x18 = +0x14 copy), id +0x1c, UnitStats_ReadSave(+0x20), 12 bytes +0x724, then the four arrays +0x730 / +0x740 / +0x750 / +0x760 as count + object ids resolved with CBas... |
| 0x00660100 | 1017 | HeroRecord::CopyFrom | (src, party): strings +0 +8 +0xc +0x10 +0x14, id +0x1c, stats 0x704, complection +0x724..+0x72c; the four arrays cloned with party->vt[24] CloneItem (count +0x48 kept) in the order weapons +0x730, quick +0x740, armor +0x750, spell... |
| 0x006621F0 | 954 | CParty::InitHeroFromProto | (hero record): proto = Units_FindProtoByName(+0x14); +8 = udb entry +0xe8 (class name); Hero_InitStatsFromProto(+0x20); complection +0x724..+0x72c = proto +0x14..+0x1c; Units_GetProtoEquipment: every armor name -> CreateItemByName... |
| 0x006618C0 | 842 | CParty::Read | (stream, readVars): Clear; zone name +0x68 (length-prefixed), u32 +0x6c; item manager vt[9] + spell manager vt[9] (object headers {id, kind, u16 a, u16 b}), CBaseManager::ReadObjects for both (the detail blocks, each ended by 0xFF... |
| 0x00660500 | 819 | HeroRecord::CloneItems | (src record, party): ReleaseItems then every item of src's +0x730 / +0x740 / +0x750 / +0x760 cloned with party->vt[24] (count +0x48 copied, refcount ++) |
| 0x006625B0 | 800 | CParty::ApplyHeroKit | (hero record): drops the record's spells +0x760 and weapons +0x730; kit = units.udb NPC record (g_0x7AF384 map, 0x60 bytes) of the proto name +0x14: +0x48 spell names -> +0x760 (CreateSpellContainer), +0x3c item names -> quick ite... |
| 0x0065FDE0 | 796 | HeroRecord::Write | (stream): the inverse of HeroRecord::Read: 5 length-prefixed strings +0 +8 +0xc +0x10 +0x14, id +0x1c, UnitStats_WriteSave (0x704 raw), 12 bytes +0x724, 4 x (count + object ids (+0xc) or -1) for +0x730 / +0x740 / +0x750 / +0x760, ... |
| 0x00662AC0 | 776 | PartySlot::SaveToFile | (this = PartySlot 0x74 {CParty +0, file name +0x70}, dir): the name must be alnum + '.' + ext else dir\name is deleted; MoveFileA(dir\name -> dir\temp.mp), CParty::Write into a CMemStream, MpFile::Writer (0x579B80 compressor, head... |
| 0x00662FB0 | 767 | PartyStore::LoadAll | clears the slots; FindFirstFileA(dir + "\*.mp"); every file CParty::LoadMp into a temporary CParty; parties with exactly one hero are copied (CParty::CopyCtor) into a new 0x74 slot with the file name at +0x70 |
| 0x006636D0 | 560 | StringFloatHash_InitBuckets | thiscall (hash, n): STLport hashtable::_M_initialize_buckets: n buckets = lower_bound of n in the 28-entry prime table 0x73B914 (0xFFFFFFFB past the end); the bucket vector +4..+0xC reserved to that size (DwordBuffer_AllocCopy5, o... |
| 0x0065F2D0 | 473 | PartyStore::dtor | destroys the CString +0x80, the CParty +0x10 (Clear, Done, hash, items, hero records), every 0x74 slot of the vector +4 and the dir CString +0 |
| 0x00663FA0 | 438 | HeroRecordArray::SetSize | thiscall (array, n, grow): MFC CArray<HeroRecord 0x784>::SetSize: grow != -1 -> +0x10; n 0 -> HeroRecord::dtor on every record, delete, data / size / max = 0; first allocation operator_new(n * 0x784) + HeroRecord_ConstructN; withi... |
| 0x00662DD0 | 431 | CParty::LoadMp | (file name; dir in this[1]+0x1c): opens dir\name (CFile), MpFile::Reader (header check, decompress) into a CMemStream and CParty::Read(this, stream, 1); false when the reader fails. Was MpFile::Load |
| 0x006608D0 | 380 | HeroRecord::AddWeapon | (item): CItem::Clone (0x69FF70, through the item's manager) and append to the weapon array +0x730 (count +0x734, max +0x738, grow +0x73c; 32-byte aligned data); returns the clone. Was AddInventoryItem: +0x730 feeds CUnit +0x440 (C... |

## 0x664000 - 0x674000: CPlayer / CPlayerClient / CPlayerServer: in-game protocol opcodes (client SendMsgXX / OnMsg_*, server SendMsgXX / OnMsg_*), party sync (MpFile), tracked units and objects, CPlayerManager, CMappedResource (resource file cache), hero record array

382 functions, group `net`.

| addr | size | name | purpose |
|---|---|---|---|
| 0x00667EC0 | 2274 | CPlayerClient::OnMsg40_UnitCreate | server 0x40: reads the unit record (u32 id, kind, prototype, flags, f32[3] morph, CStrings name / model / specific +0x78 / +0x7c, u16 part count + names, display name, u32 +0x23c) then the length-prefixed full net state (CMapObjec... |
| 0x00668C50 | 951 | CPlayerClient::OnMsg60_PartyStatus | server 0x60 (u8 paused, u32 sync marker, u32 player index, u32 n, n x u32 VISIBLE unit ids = CPlayerServer +0x510c (party units + what they see), u32 party mode, u32 m, m x u32 loot-owner unit ids): clears flag 0x400000 on the pre... |
| 0x0066F4C0 | 893 | CPlayerServer::Respawn | slot 20: outside a session: server 0x09 then 0x0B {hero id, 0xFF}; in a session: money -= money * g_0x7C5490, 0x0B {hero id, 0xFF}, a free id >= hero id + 250000 (or the id itself when already >= 0x3baa0c40), World::CopyHero, Clea... |
| 0x0066E430 | 867 | CPlayerServer::SendMsg60_PartyStatus | party mode +0x5120 = 1 when a party unit has a body part stat +0x4c8..+0x4d8 <= 2.0 (hurt), 2 when a visible unit (+0x5110) casts an offensive spell or is the current target of a brain hostile to the player's side; server 0x60 {u8... |
| 0x00670310 | 851 | CPlayerServer::OnMsg36_Interact | client 0x36 {u32 n, n x u32 unit ids, u32 a, u32 interaction id, u32 b, u32 target object id}: with several units the nearest one that can reach the target (CUnitServer::CanReachOrderTarget / CAIMap::FindPath) is picked; AiLogicPl... |
| 0x00671730 | 778 | CMappedResource::Open | slot 1 (path, writable): closes (vt[3]), opens the file (0x6EB35C, flags 0x8010 / 2 when writable), reads the 16-byte header {magic 0x019ce23c, entry count +8, data offset +0xc, data size +0x10}, allocates and reads the entry tabl... |
| 0x0066E130 | 766 | CPlayerServer::UpdateVisibleSet | sync marker +0x24 == -1: known objects +0x50f8 = every party unit (+0x40) plus its AiMemory seen lists (+0xa90 / +0xaac), visible units +0x510c = the union; else +0x510c copied from the previous known array and +0x24 = -1. Was Upd... |
| 0x00669FA0 | 736 | CPlayerClient::OnMsg0E_MovePath | server 0x0E (u32 request id, u32 unit id, u32 x2, u32 unit id, u32 kind, [embedded delta stream: the mover state (MoveState_WriteNet) unless kind == 3]): a temporary kind-0x32 unit decodes it (DeltaWriter / ReadUnitOrder); with th... |
| 0x0066D7F0 | 722 | CPlayerServer::SendMsg40_UnitCreate | (array of units): per unit server 0x40 {id, kind +0x10, +0x18 prototype, flags +8, f32[3] morph +0xc4, name +0x1a8, model +0xa4, specific +0x78 / +0x7c strings, u16 part count + part names, display name (vt[7] or "") and +0x23c (0... |
| 0x00671B40 | 698 | CMappedResource::OpenFile | slot 4 (name, out size): the cached entry of the name hash +0x44 → refcount ++ and its buffer; else the entry index (0x674850), when memory-mapped the base +0x28 + entry offset, else allocates an entry {name, buffer, size (table +... |
| 0x0066FD70 | 631 | Formation_ComputeOffsets | (unit list, out Vec3 array): one unit = zero offset; else per unit the unit vector (1 m) from the group's centroid to the unit; added to the move target when the offset point is reachable |
| 0x00669A30 | 599 | CPlayerClient::OnMsg80_Party | server 0x80 (embedded stream: MpFile::Read(party +0x70, full = 1)): the party replaces the local one, the synced item id list +0x114 is rebuilt from the party item manager (items with +0x28 < +0x2c = damaged), in a session the nam... |
| 0x0066B570 | 580 | Dwords_Reserve | thiscall (vector, n): vector<dword>::reserve: n <= capacity -> return; new buffer of n dwords from the size pool g_0x79B224 (with the _S_chunk_alloc refill inlined: 20 blocks from the chunk g_0x79B268..0x79B26C, else operator_new(... |
| 0x00672FA0 | 580 | Dwords_Reserve2 | STL: vector<dword>::reserve(n) (size pool buffer, copy via Dwords_AllocCopy4); the id vector of CMappedResource::ctor |

## 0x674000 - 0x676000: resources: CMappedResource / CResourceBase / CMappedFile + CObjectManager

60 functions, group `data`.

| addr | size | name | purpose |
|---|---|---|---|
| 0x00675770 | 721 | CMappedFile::Open | (path, writable): GetDiskFreeSpace / GetSystemInfo choose FILE_FLAG_NO_BUFFERING (0x20000080) when the sector size divides the page size else 0x10000080; CreateFileA (+0; read or read-write, share read), GetFileSize (+0xc; "Too la... |
| 0x00674AC0 | 540 | CResourceBase::ListFiles | (out CStringArray, pattern or 0): every entry name, or those whose name / extension match the wildcard (_splitpath + WildcardMatch, case-insensitive) |
| 0x00674070 | 515 | DwordVector_ctorN3 | STL: vector<node*>(n) zeroed bucket table with the STLport node allocator inlined; the rehash of the CMappedResource file name hash (StringHash_Rehash2 <- StringHash_GetOrInsert2 <- OpenFile) |
| 0x00675050 | 429 | CObjectManager::OpenProtoResource | slot 16 (name) -> bool: when a record of +0xb0 has that name: 1; else a new CMappedResource (0x58, 0x671280) is appended as {name, resource} and opened with its vt[1] Open(name, 0): the manager's prototype / database archives (CIt... |
| 0x00675DD0 | 408 | CEffectSound::ctor | CObject (vt 0x73B8E0) -> CEffect (vt 0x73BA38: delta codec +0xa8 with 4 CMemStreams, the +0xf8 one vt 0x73C960) -> vt_CEffectSound: sound handle +0x25c = -1 (stops it via 0x6B94A0 when set), +0x254 = 1, +0x14c byte, +0x24c/+0x250 ... |
| 0x00674910 | 378 | CResourceBase::CreateAccelTable | (pairs {name, index}*, count): +0x1c = max index + 1 slots (-1 filled) with the entry index of every name (fixed numbering of often-used entries); "CResourceBase::CreateAccelTable(). Index out of range / No entry" |
| 0x00674DF0 | 332 | CObjectManager::dtor | vt 0x743338: every prototype record of +0xb0: object vt[3] Done then vt[0](1) delete; the strings destroyed; CBaseManager::Done; the vector buffer returned to the size pool; CBaseManager::DestroyBody |
| 0x00675200 | 313 | CObjectManager::CloseProtoResource | slot 17 (name): the record with that name: resource vt[3] Release + delete, the record erased (vector shift); 1 when found. Was FindProtoByName2 |
| 0x00674310 | 288 | dead_674310 | unregistered code with no call, jump or pointer anywhere in the image (gaps.py 2026-11-06) |
| 0x00674F40 | 245 | CFigureManager::Done | slot 3: deletes every cached figure entry (stride 0x10) then CBaseManager::Done |
| 0x006744C0 | 244 | CResourceBase::OpenFile | (name, out size): CloseFile; a loose file of that name on disk (_access) is mmioOpen'd into +0x30 (size from _findfirst) -> loose files override archive entries; else FindEntry and mmioSeek(+0x24, entry offset) with +0x28 = offset... |
| 0x00675340 | 243 | CObjectManager::ClearProtos | slot 18: every record: resource vt[3] + delete; the vector emptied |
| 0x00675BE0 | 185 | CEffect::ResetState | zeroes position +0x1c, velocity +0x94..+0xa4, SetSize(0) of the four delta CMemStreams +0xa8/+0xd0/+0xf8/+0x120, last tick +0x148 = -1 |
| 0x00675560 | 183 | Record10Vector_Destroy | fastcall (vector): for each 0x10-byte record {std::string +0, dword +0xC} of [begin, end +4): String_Free of the string (+ an empty member dtor Nop_424C00); then the buffer freed (pool <= 0x80 bytes or operator_delete); SEH frame;... |

## 0x676000 - 0x683000: CEffect* (sounds, lights, particles, arrows, tornado)

245 functions, group `effects`.

| addr | size | name | purpose |
|---|---|---|---|
| 0x0067F870 | 8842 | CSpellEffect::Start | phase 0 by spell kind (proto index): 0/0x28 fire arrow & 2 acid ray -> CreateArrow toward the target unit (phase 1, countdown 150); 1/0x29 lightning -> CreateLightningToUnit + light, hit applied at once (ApplyHitTo), phase 2 count... |
| 0x00682650 | 3810 | CSpellEffect::TickActive | phase 2 by kind: 0/2/3/0x18/0x28: countdown then phase 3; 1/0x29: at 0 the lightning is removed, the light drifts up; 4: at countdown 5 a second lightning, then phase 3; 5 acid column: at countdown 15 hits every live unit within +... |
| 0x00681BB0 | 2649 | CSpellEffect::TickFlight | phase 1 (projectiles) by kind: 0/0x28/2 arrows: when the arrow +0x90 reports arrival (+0x14C): BuildHitRecord + ApplyHitTo(target), hit particle 0x2041 / 0x2042 and 'end.wav' at the target, light * 1.5, countdown 0x23, phase 2; co... |
| 0x0067CEF0 | 1324 | CEffectMoshka::UpdateFlight | (time): drives the bird object +0x14C around the target unit +0x154: state +0x168 0 approach (within 5 -> 1 circling, timer 100, radius 30), 1 circle at angle +0x160 + time * 0.2 (timeout -> a visible point 50 away, state 2, timer... |
| 0x00678EC0 | 656 | CEffectParticle::CreateParticleSystem | client: CParticleManager::CreateParticleSystem(g_0x7AF754, type +0x158) into +0x178 at pos, ScaleParams(scale +0x15C / system +0x128), +0x118/+0x11C = intensity / radius, AttachToObject; pre-run (world tick - start tick, max 0x1E,... |
| 0x0067E170 | 612 | CEffectManager::CreateByKind | slot 14 factory (kind, id): 0x48 CEffectSound (0x264), 0x49 CEffectAmbientSound (0x174), 0x4A CEffectParticle (0x180), 0x4B CEffectPointLight (0x164), 0x4D CEffectLightning (0x178), 0x4E CEffectArrow (0x6C0), 0x5A CEffectWall (0x1... |
| 0x006782D0 | 591 | CEffectPointLight::Activate | slot 23 (update): start: CLight (kind 0x31, light manager g_0x7AF748) InitPositional(colour +0x14C, pos), radius /+0x158/ (+0xE0, r^2, 1/r^2), flags /= +0x160, CTerrain::AddLight, moving -> flag 0x100; update: colour, radius chang... |
| 0x0067AE30 | 533 | CEffectArrow::ctor | projectile base vt 0x743754 + DeltaCodec, embedded CEffectParticle +0x158 (system ref +0x2D0), CEffectPointLight +0x2D8 (light +0x434), CEffectSound +0x43C (handle +0x698), CString +0x6A0 sound name, +0x6A4 light colour, +0x6B0 li... |
| 0x0067D420 | 533 | CEffectMoshka::CreateBird | (target unit id): +0x154 = id; client object (kind 0x38, g_0x7AF5F8) with model 'unmobi' / animation 'bird00' at a visible point around the target, added to the client world, particle 0x203C (scale 0.5) attached at +0x150, state 0... |
| 0x0067DBD0 | 503 | CEffectTornado::Update | slot 6: moving: pos += velocity, particle and sound follow; age +0x154 < 0x6F keeps the particle attached; on the server after 20 ticks every 8 ticks: every unit within 5 m gets a hit record (damage 100.0, location 6, factors 1) t... |
| 0x0067B1C0 | 463 | CEffectArrow::dtor | destroys the sound name, the three embedded sub-effects and the delta streams |
| 0x006795C0 | 456 | CEffectParticle::Update | slot 6: +0x17C = -1; moving (+0xA4): advances toward the target point +0x16C by /velocity/ (not past it), z follows the terrain delta; the one-shot types (0x2002, 0x2006, 0x200B..0x200F, 0x2018, 0x201B..0x2026, 0x2030..0x2033, 0x2... |
| 0x0067AAB0 | 439 | CEffectArrow::ComputeFlightVector | -> 0 when arrived: velocity +0x98 = target unit (+0x150, world vt[40] FindObjectById) centre (pos + offset +0x130 + half height +0x14C) - pos, normalised to 0.667 m/tick (1.333 for the CEffectArrowFigure kind 0x5B); shorter than o... |
| 0x006797E0 | 435 | CEffectWall::ctor | CEffectParticle fields + vt_CEffectWall (size 0x18C, kind 0x5A): scale / intensity / radius = 1, +0x16C / +0x170 / +0x158 = 0, attach unit id -1, source unit id -1, +0x174 = 0, wall direction vec3 +0x180 = 0 (half extent); used fo... |

## 0x683000 - 0x68C000: CSpell / CSpellContainer / CSpellConstructor / CSpellManager

193 functions, group `spells`.

| addr | size | name | purpose |
|---|---|---|---|
| 0x0068BF60 | 1333 | CSpellContainer::Finalize | rebuilds the embedded CSpell +0x30 from the proto +0x1C (InitFromProto values; id = proto index << 16) then applies every rune of +0x20 by its code: 'a' area += v * proto Area (then radius = sqrt(area / pi)); 'd' duration += ftol(... |
| 0x00684550 | 1174 | SpellsDb::Load | (res path): 'spells.sdb' from the database .res (CMappedResource) into a CMemStream; archive tag 1 -> vector<SpellProto 0x84> +0 (begin g_0x7C8510 / end g_0x7C8514) and the code -> proto map +0xC (g_0x7C851C, keyed by the CODE +4,... |
| 0x00685BE0 | 836 | SpellProtoVector_InsertN | STL: vector<SpellProto 0x84>::insert(pos, n, value) (in-place shift with SpellProto::operator_assign / placement copies, else InsertRealloc); used by SpellProtoVector_Resize |
| 0x006850D0 | 756 | SpellSetRecordVector_InsertN | STL: vector<SpellSetRecord 0x34>::insert(pos, n, value) (in-place shift with operator_assign / placement copies, else InsertRealloc); used by SpellSetRecordVector_Resize |
| 0x00689030 | 728 | CSpellManager::CreateSpellByName | ('code{rune;rune;...}', e.g. 'fireball{e1;r1}'): the spell CODE (+4) looked up in g_0x7C851C, the rune codes in g_0x7C853C; world vt[17] = the CSpellConstructor: SetManager, BeginConstructSpell(proto), AddRune per rune, EndConstru... |
| 0x006853D0 | 614 | SpellSetRecord_CopyRange2 | STL: copy [first, last) of SpellSetRecord 0x34 forward onto dest (operator_assign per record: the three std::strings and two string vectors); the erase shift of SpellSetRecordVector_Resize |
| 0x006863C0 | 599 | SpellSetRecord_CopyRange | STL: copy [first, last) of SpellSetRecord 0x34 forward onto dest (operator_assign per record); the loading reset of SpellSetRecordVector_Serialize |
| 0x0068A0F0 | 580 | DwordVector_Reserve3 | STL: vector<dword>::reserve(n) (size pool buffer, elements copied); the bucket / id vector of CSpellManager::CSpellManager |
| 0x0068B4C0 | 576 | CSpell::IsValidTarget | (caster, target) -> bool: flags +0x3C low 16 bits: 1 'ff' only friends (the player masks +0x244/+0x248/+0x250 say not enemy), 2 'fe' only enemies, 4 'fh' model unhuma/unhufe, 8 'fg' unmogo, 0x10 'fo' unorma/unorfe, 0x20 'fl' unmol... |
| 0x006875D0 | 571 | SpellProto::operator_assign | copy-assigns a SpellProto 0x84 (the std::string code / name fields and the numeric spell fields); 3 calls in SpellProtoVector_InsertN |
| 0x00689B70 | 560 | HashMapUint_InitBuckets | thiscall (hash, n): STLport hashtable::_M_initialize_buckets: lower_bound of n in the prime table 0x73B914 (0xFFFFFFFB past the end), bucket vector +4..+0xC reserved to that size and null-filled, count +0x10 = 0; caller SpellRegis... |
| 0x00686910 | 543 | SpellProtoVector_InsertRealloc | STL: vector<SpellProto 0x84>::_M_insert_overflow (grow path of insert); used by SpellProtoVector_InsertN |
| 0x00685F30 | 528 | SpellSetRecordVector_InsertRealloc | STL: vector<SpellSetRecord 0x34>::_M_insert_overflow (grow path of insert: new size-pool buffer, placement copies, old records destroyed); used by SpellSetRecordVector_InsertN |
| 0x0068A500 | 515 | DwordVector_ctorN5 | STL: vector<node*>(n) zeroed bucket table with the STLport node allocator inlined; the rehash of the spell id -> CSpell map (HashMapUint_Rehash <- CSpellManager::CreateSpellById, SpellRegistry_ctor) |

## 0x68C000 - 0x68F000: CLight / CLightManager / CMaterial / CMaterialManager

88 functions, group `renderer`.

| addr | size | name | purpose |
|---|---|---|---|
| 0x0068E640 | 681 | CMaterialManager::Init | slot 2: CBaseManager::Init; opens '<data>materials.res' (vt[16]); for every (kind, file name) pair of the table 0x743BF8..0x743C37: the material file (vt[19]: u16 name length + name + 17 dwords: diffuse rgb, alpha, ambient rgb, sp... |
| 0x0068EC20 | 580 | DwordVector_Reserve_68EC20 | STL: vector<uint>::reserve(n) |
| 0x0068D5A0 | 394 | CLight::ApplyPointWrap | (atten, in rgb, normal, L unit, d, out): k = 1 - d^2 * (+0x104); k < 0 -> out = in; w = N.L + 1 when N.L <= 0 else 1 ('wrap'); out_c = min(1, in_c + w * k * atten * colour_c); the terrain point-light term (TerrainSector_LightVerte... |
| 0x0068C680 | 362 | StlString_Reserve_68C680 | STL: basic_string grow/reserve with length_error check ("basic_string") |
| 0x0068DE30 | 347 | CMaterial::~CMaterial_body | vt_CMaterial: pops all deque elements, frees nodes and map, CStrings released, CObject base dtor 0x455930 |
| 0x0068CB90 | 327 | PartialSort_Bytes | STL: partial_sort over a char range (heap select: make_heap + adjust_heap + sort_heap); used by CSpellContainer::Finalize |
| 0x0068D450 | 323 | CLight::ApplyPoint | plain N.L point light without the wrap term; no callers (dead) |
| 0x0068EE70 | 314 | DwordVector_InsertN_68EE70 | STL: vector<uint>::insert(pos,n,value), grow path 0x68F040 |
| 0x0068D310 | 310 | CLight::ApplyDirectional | (intensity, in rgb, normal, out rgb): t = -(dir . N); t <= 0 -> out = in; else out_c = min(1, in_c + t * intensity * colour_c); the terrain sun term (TerrainSector_LightVertexSun) |
| 0x0068DBC0 | 294 | CMaterial::CMaterial | vt_CMaterial (0x9C bytes): CObject header, name +0x1C = '', alpha +0x2C = 1.0, texture-frame deque +0x64 (0x200-byte nodes), texture names +0x8C / +0x94 = '', +0x90 / +0x98 = 0; the D3D material block is +0x20 diffuse rgb, +0x2C a... |
| 0x0068E1C0 | 272 | Deque_InitMap_68E1C0 | STL: compute node map size ((n>>5)+3, min 8), allocate map and nodes, set start/finish |
| 0x0068C820 | 249 | SpellProto_FindByCode | (code C string) -> SpellProto* or 0: std::string lookup in the spell code map g_0x7C851C (CSpellContainer::Finalize derives the proto index) |
| 0x0068E3F0 | 246 | CMaterialManager::CMaterialManager | CObjectManager ctor (0x674D90); the kind -> material template hash at +0xBC (NextPrime(100) buckets +0xC0 / +0xC4, count +0xCC), nodes 0x50 bytes {next, kind, 0x44-byte template} |
| 0x0068D850 | 245 | CLightManager::CLightManager | CBaseManager ctor; +0xC0 / +0xCC fields, +0xD4 light-id std::list sentinel (pool g_0x79B228) |

## 0x68F000 - 0x69F000: CTexture / CTextureManager (.mmp), CFileRegistry (binary key/value registry), CIniFile, CWinRegistry, CItemConstructor, STL leaves

351 functions, group `renderer`.

| addr | size | name | purpose |
|---|---|---|---|
| 0x0069AE20 | 1828 | ShopStock::GenerateItems | (proto vector, min, max, out list, kind 0 weapons / 1 armours / 2 quick; from GenerateWeapons / GenerateArmors / GenerateQuickItems): SelectSpellFilters S_Weapons (0/3) or S_Armors (1); n = rand(min..max) (max when min >= max); pr... |
| 0x00693180 | 1684 | CTextureManager::ComposeAndSaveLayers | (out name CString, recipe {+0 key bytes, +4 key length, +0x10 layer CStringArray (+0x18 count), +0x24 size}, name, altName, mips, fill ARGB, format): reuses <root g_0x7AF460>textures\<name>.mmp (or altName's) when its MMP extra bl... |
| 0x0068FBF0 | 1646 | CTexture::LoadFromBmp | (device, bmpPath, alphaBmpPath) -> bool: loads the 24-bit .bmp (CBitmapDib::LoadStream) and the optional 8-bit alpha .bmp ('Alpha-file'), packs B G R A per pixel with PixelFormat_PackRGBA into a 16-bit system-memory surface (caps ... |
| 0x0069B5B0 | 1294 | ShopStock::Read | thiscall (stream) from CGSPlayer::Serialize 0x4A6C40 tag 5: shopParty +0 vt[22] GetSpellManager and vt[18] GetItemManager (results unused); then six blocks of a dword count (stream vt+4 read 4) + names (Stream_ReadCString 0x461190... |
| 0x00690430 | 1189 | CTexture::LoadMipChain | (device, mmpHeader, firstLevel) -> bool: already loaded (flag 8) -> 1; w / h = header >> firstLevel (more levels skipped while w or h > 511 when the manager's small-texture flag +0xEC); levels = mip count (hdr +0xC) - first (1 whe... |
| 0x0069A780 | 1169 | ShopStock::GenerateSpells | (out list of CSpell, float[8] limits, skipPercent): for every filtered spell proto (hash +0x50) with ISAAC%101 >= skipPercent: perk list = the proto's mandatory rune names (+0xc vector) plus random optional ones (+0x18 vector, 50 ... |
| 0x006922A0 | 1060 | CTextureManager::LoadTexture | (texture) -> bool: loaded (flag 8) -> 1; '<root g_0x79BC90>textures\<name>.bmp' exists (CFileFind) -> the BMP path: device format by kind (1 / 2 / 3 / 4 / 8 -> ARGB1555 id 0x5551, 6 / 0xD -> 4444, else RGB565 0x5650; IdMap_Lookup_... |
| 0x0069A110 | 803 | ShopStock::AddMaterials | (min, max, out list): n = rand(min..max) random material entries of +0x2c: a "material" loot item (category 1) with that material, count +0x48 = rand(entry min..max) |
| 0x006928C0 | 802 | CTextureManager::LoadMmpExtraBlock | (name, out CArray<byte>): from the loose file '%stextures\%s.mmp' (header 0x4C read first) or the archive copy (vt[19] / vt[20]): copies the header's extra block (hdr +0x48 bytes starting at +0x4C) into the array; 0 when the textu... |
| 0x00693970 | 747 | CTextureManager::SaveComposedTexture | static (layer set {+0 key bytes, +4 key length, +0x10 layers, +0x24 size}, name, fill ARGB): path = g_0x7AF460 + "textures\" + name + ".mmp"; when that file exists and its 0x4C-byte MMP header's extra block (+0x48 length) equals t... |
| 0x0069A480 | 733 | ShopStock::SelectSpellFilters | (table = SpellsDb S_* vector of 0x34 records): clears the hashes +0x50 / +0x64; +0x50 = the allowed spell protos (+0x38 list) that the table lists with the player's bit in +0x30; +0x64 = the allowed perk protos (+0x44) keyed by th... |
| 0x00691D80 | 649 | CTextureManager::CreateRenderTarget | thiscall (width, height, kind), ret 0xC: tex = this vt+0x5C (kind, -1); tex width +0x20 / height +0x24; kind 0x1E -> device format 0x5650 (R5G6B5 masks F800 / 7E0 / 1F), 0x20 -> 0x5551 (A1R5G5B5 8000 / 7C00 / 3E0 / 1F), 0x21 -> 0x... |
| 0x0069BAC0 | 621 | ShopStock::Write | (this = ShopStock, stream) the writer twin of ShopStock::Read 0x69B5B0 (CGSPlayer::Serialize tag 5): count + names of +8 armour protos, +0x14 weapon protos, +0x20 quick protos, +0x2c materials (name + min, max, stock), +0x38 spell... |
| 0x00699A40 | 579 | ShopStock::AddQuickInstructions | (min, max, out list): "instruction quick item" loot items per allowed quick proto (+0x20) and "scroll" items, count rand(min..max) |

## 0x69F000 - 0x6AE000: CItem* + CItemManager + items.idb

303 functions, group `items`.

| addr | size | name | purpose |
|---|---|---|---|
| 0x006A1780 | 4007 | WeaponId_ListFigurePartNames | fastcall (packed item id, row = id >> 16, out std::vector<std::string>): the same names as WeaponProto_GetFigurePartNames 0x6A0FF0, each built as a std::string and appended with Record0CVector_PushBack 0x6A5910 (5 pushes per case ... |
| 0x006A0FF0 | 1863 | WeaponProto_GetFigurePartNames | fastcall (weapon proto index, out CStringArray): SetSize(0); for a valid index (vector g_0x7C86F8, 0xB4 records) NN = "%.2d" of proto +0x18 and by type +0xC (SetAtGrow each): 0..4 "rh3.sword" / "rh3.axe" / "rh3.dagger" / "rh3.pike... |
| 0x006AC970 | 1646 | CItemManager::CreateItemByName | (this, "name[.material][[x]]"): the '[..]' text split off and trimmed, the rest split on '.' and LOWER-cased (0x700EFE); "spell container" -> CreateSpellByName(x) (none -> 0) in CreateItem(0x3008, quick row of "spell container", 0... |
| 0x006A5D60 | 1564 | CItemConstructor::ApplyProto | (this): 0 for a weapon / armor (vt[12] 0x11 / 0x12) without a material. Builds a temporary proto T of the item's kind from P = +0x24 and material M = +0x20: T.Price +0x34 = P.Price + P.Components +0x4c * M.Price +0x10; T.Weight = ... |
| 0x006A67A0 | 1141 | ItemsDb::Load | (this = ItemsDb 0xc0 at g_0x7C86D8, res path): items.idb through a CMappedResource into a CMemStream; tagged archive: tag 1 Materials (vector +0 stride 0x50, name map +0xc), 2 Weapons (+0x20 stride 0xb4, map +0x2c), 3 Armors (+0x4... |
| 0x006A4B90 | 1071 | CItemLoot::GetIconName | slot 19 (out CString): category 1 "material%04d" (material index +0x54); category 2: table 0 / 1 -> "%s_%.2d.%d" with the referenced weapon / armor proto's type prefix, 2 "qitem%04d", 3 "prototype%04d", 4 "modifier.."; else "litem... |
| 0x006A5030 | 973 | CItemLoot::GetInitIconName | slot 20 (out CString): "initwe%s%dweapon" / "initar%s%darmor" / "initqi%ditem" / "initlim%ditem" by the referenced table |
| 0x006A9420 | 835 | QuickProtoVector_InsertN | STL: vector<QuickProto 0x8C>::insert(pos, n, value) (in-place shift with ItemProto8C::operator_assign / placement copies, else InsertRealloc); used by QuickProtoVector_Resize |
| 0x006A3CD0 | 797 | CItemQuick::GetIconName | slot 19 (out CString): "qitem%04d" (+ ".%s" of the proto's M.Type +0x10 when set) from the quick proto index; the weapon / armor / loot / quest tables are consulted by kind first |
| 0x006A8C50 | 715 | WeaponProtoVector_InsertN | STL: vector<WeaponProto 0xB4>::insert(pos, n, value) (in-place shift with operator_assign / placement copies, else InsertRealloc); used by WeaponProtoVector_Resize |
| 0x006A9830 | 713 | QuestProtoVector_InsertN | STL: vector<QuestProto 0x7C>::insert(pos, n, value) (in-place shift with operator_assign / placement copies, else InsertRealloc); used by QuestProtoVector_Resize |
| 0x006A90B0 | 693 | ArmorProtoVector_InsertN | STL: vector<ArmorProto 200>::insert(pos, n, value) (in-place shift with operator_assign / placement copies, else InsertRealloc); used by ArmorProtoVector_Resize |
| 0x006AD900 | 674 | Briefing::SerializeFields | (this = quests.qdb briefing record 0x6c, archive): tag 0 name +0, 1 exp +0xc, 2 money +0x10, 3 give items +0x14, 4 string +0x5c, 5 remove items +0x20, 6 give quests +0x2c, 7 complete quests +0x38, 8 available zones +0x44, 9 comple... |
| 0x006A9B00 | 663 | LootProtoVector_InsertN | STL: vector<LootProto 0x78>::insert(pos, n, value) (in-place shift with operator_assign / placement copies, else InsertRealloc); used by LootProtoVector_Resize |

## 0x6AE000 - 0x6B4000: quests database (quests.qdb)

88 functions, group `data`.

| addr | size | name | purpose |
|---|---|---|---|
| 0x006AE8E0 | 3042 | QuestsDb::ParseQuestReg | (this = QuestsDb (ECX), text) -> 1: parses a zone's quest.reg (CFileRegistry::Attach on the CTextManager buffer): [quest] name, exp, money, zone index, order, questplaces "{x;y}" pairs -> a Quest appended to +0 (name map rebuilt);... |
| 0x006B27E0 | 958 | BriefingVector_InsertN | STL: vector<Briefing 0x6C>::insert(pos, n, value) (in-place shift with Briefing::operator_assign / placement copies, else InsertRealloc); used by BriefingVector_Resize |
| 0x006AE3A0 | 923 | QuestsDb::Load | (this = g_0x7C87A8, res dir): opens <dir>database.res (always; "databases.res" only in the error text), reads "quests.qdb" into a CMemStream, tagged archive version 1: tag 1 = the quest block -> vector +0 (Quest::SerializeFields 0... |
| 0x006B39E0 | 727 | Briefing_CopyRange | STL: copy [first, last) of Briefing 0x6C forward onto dest (Briefing::operator_assign per record); the loading reset of BriefingVector_Serialize |
| 0x006B2450 | 679 | QuestVector_InsertN | STL: vector<Quest 0x38>::insert(pos, n, value) (in-place shift with Quest::operator_assign / placement copies, else InsertRealloc); used by QuestVector_Resize |
| 0x006B3E60 | 662 | Briefing::operator_assign | copy-assigns a Briefing 0x6C (its std::strings and vectors); 2 calls in BriefingVector_InsertN |
| 0x006B02B0 | 609 | CStringVector_Reserve | STL: vector<CString>::reserve(n) (size pool buffer, CString copies, old destroyed); 8 calls in QuestsDb::ParseQuestReg (the quest .reg string lists) |
| 0x006B2BA0 | 569 | Briefing_CopyRange2 | fastcall (first, last, dest): copy-assigns (last - first) / 0x6C Briefing records forward: name string +0 (String_AssignRange2), exp +0xC, money +0x10, giveItems vector<CString> +0x14 (inline assign: realloc via Strings_AllocCopy ... |
| 0x006AFCF0 | 560 | QuestMap_Rehash | (this = quest name map g_0x7C87B4, minBuckets): grow buckets to the next prime and re-link the nodes |
| 0x006AFFC0 | 560 | BriefingMap_Rehash | (this = briefing name map g_0x7C87D4, minBuckets): grow buckets to the next prime and re-link the nodes |
| 0x006B1CB0 | 553 | Briefing::CopyCtor | copy-constructs a Briefing 0x6C (its std::strings and vectors); used by BriefingVector_InsertRealloc |
| 0x006B1480 | 524 | BriefingVector_InsertRealloc | STL: vector<Briefing 0x6C>::_M_insert_overflow (grow path of insert); used by BriefingTable_AddUnique, BriefingVector_PushBack and BriefingVector_InsertN |
| 0x006B0EF0 | 523 | QuestVector_InsertRealloc | STL: vector<Quest 0x38>::_M_insert_overflow (grow path of insert); used by QuestVector_PushBack and QuestVector_InsertN |
| 0x006B2240 | 519 | BriefingVector_Resize | STL: vector<Briefing 0x6C>::resize(n) (QuestsDb briefings): InsertN of default records or erase of the tail; used by BriefingVector_Serialize |

## 0x6B4000 - 0x6C3000: CMusicSystem / CSoundManager / mp3 (Miles)

233 functions, group `sound`.

| addr | size | name | purpose |
|---|---|---|---|
| 0x006C1C00 | 5336 | TerrainSector_BuildMesh | (sector): the vertex records +0x194 (0x54 each, 0x441 per layer, 0x882 with water) from the packed vertices +0x18: normal +0x2B/+0x2F/+0x33 from the packed dword d (nx = ((d >> 11 & 0x7FF) - 1000) / 1000, ny = ((d & 0x7FF) - 1000)... |
| 0x006BFF40 | 5050 | TerrainSector_Draw | (sector, ctx, terrain, pass) -> bool: unless culled (+0x60 == 2): ApplyMaterialHeightDeltas when water; per quarter q with +0x144[q] != 2 and triplets for the pass ((+0x318 + q*0x14) vector[pass], 9 triplets per tile): the vertice... |
| 0x006BE470 | 3676 | dead_6BE470 | unregistered code with no call, jump or pointer anywhere in the image (gaps.py 2026-11-06) |
| 0x006C13A0 | 2129 | TerrainSector_Load | (sector, reader, sx, sy, textureCount, tilesPerTextureSq, mapStem, terrain) -> bool: reads '<stem>%3.3i%3.3i.sec' (reader vt[5]); header dword magic 0xCF4BF774 + byte flags (bit 0 water layer -> +0x10, bits 0-1 -> +0x14); payload ... |
| 0x006B89F0 | 1737 | CSoundManager::LoadSample | (channel, index, name, decompressMp3) -> bool: the sample cache: map +0x870 (name -> cache node) hit: the node moves to the list head +0x858 (LRU list +0x854, tail +0x85c), tick = GetTickCount, refcount ++, AIL_set_named_sample_fi... |
| 0x006B4710 | 1584 | CMusicSystem::Init | slot 2: reads "<g_0x7AF45C>music.reg" (CFileRegistry; FatalError "CMusicSystem::Create(): Unable to..."): [Common] Credits -> +0x30, MainMenu -> +0x34; every other section = a zone: two 0x38 MusicZoneRecords appended to the CArray... |
| 0x006B9B10 | 1501 | CSoundManager::Update | per frame (CWorldClient::Update): (1) channels flagged done by the EOS callback (+0x30) and loaded: cache refcount--, AIL_end_sample, id -1; (2) for every unloaded channel the pending record with the highest priority (+0x18 of the... |
| 0x006BB3A0 | 1266 | CSoundManager::MusicFillBuffer | (block +0x100): the stream feeder called by MusicThreadProc for every AIL buffer: state 2 (switching) at the switch position -> opens the pending track +0x164 through CResourceBase::OpenFile(+0x24, path +0x58 + name) (size +0x1a4)... |
| 0x006B8250 | 938 | CSoundManager::Play2D | (name, priority, volume percent, pan -100..100, loop, heightAttenuate, category (0 effects / 1 voice), variant) -> sound id or -1: FindChannelSlot(priority) (no channel and loop -> a pending record in +0x884 keyed by a new id +0x2... |
| 0x006B5350 | 875 | CMusicSystem::Update | slot 6: zone mode logic: current = MusicGetCurrentTrack; a forced track (+0x50) ends -> +0x50 = 0, +0x40 = 0, next calm time +0x54 = now + 120 s + rand 60 s; combat = (g_0x7AFC9C == 2): entering combat picks a random Combat track ... |
| 0x006B8600 | 849 | CSoundManager::Play3D | (name, priority, float[3] pos, min range, max range, loop, category) -> sound id or -1: FindChannelSlot(priority) (none and loop -> pending record); channel pos +0x3c.., ranges +0x48 / +0x4c, +0x34 is2D = 0, +0x38 = 0, UpdateChann... |
| 0x006B6D80 | 722 | CSoundManager::Done | slot 3: stops the music / sequence threads (+0x248 flag, WaitForSingleObject), releases the AIL samples, frees the sample cache, clears the pending map and the sound archive list, AIL_waveOutClose, +0x898 = 0 |
| 0x006BFC80 | 695 | TerrainSector_TransformVertices | (sector, ctx) -> +0x60: for every quarter with +0x144[q] != 2: its 17 x 17 vertices (step 1 / 2 / 4 at LOD 0 / 1 / 2 (+0x170); the water layer (+0x441) too when +0x10) are copied from +0x180 into g_0x7C88F8 (16 bytes each) and pro... |
| 0x006B7D10 | 682 | CSoundManager::FindChannelSlot | (priority) -> channel index or -1: picks the channel with the lowest priority +0x1c (free ones have priority 0 / none); -1 when the new priority <= that; a stolen looping channel (+0x24) is saved as a 0x48 pending record {name, +0... |

## 0x6C3000 - 0x6CC000: CTerrain: Load (.mp header, materials, tile types, textures + lightmap pages, UV tables), sector LRU cache (GetSector), per-vertex sun / dynamic lighting with shadow ray tracing, water waves + animated tiles, rendering passes, lights link lists, height / material queries, ray to ground, ambient sounds, modified-sector writer

114 functions, group `world`.

| addr | size | name | purpose |
|---|---|---|---|
| 0x006C68B0 | 3328 | CTerrain::Load | (mapPath, worldKey, isDungeon) -> bool: Unload; reader +0x15C vt[1] opens the archive (fail -> 0, silent); +0xFC = path without extension; +0xF8 = the first '*.mp' entry of the archive (ListFiles 0x674AC0) up to its first '.'; rea... |
| 0x006C98F0 | 2164 | CTerrain::SetVisibleQuad | (float[8] quad): visible rows / column ranges from the grid corners inside the 4 edge lines of a ground quadrilateral (each inside corner marks its 4 sectors); DEAD: only called by the unreferenced wrapper 0x6CA170 (not in the dat... |
| 0x006CB390 | 2159 | CTerrain::UpdateAmbientSounds | (flag 8, every tick from CTerrain::Tick): listener P / forward (sound mgr g_0x7AF784 +0x250 / +0x25C); 3x3 sectors around it: dominant tile type = argmax hist(+0x374)[t] * table 0x7969D0 weight (+8; redirect +0xC), proximity p = (... |
| 0x006C31C0 | 1261 | TerrainSector_LightVertexDynamic | (sector, pos, record, terrain, isWaterLayer): c = cached sun colour +0x37..; for every light of the record's link list +0x50: L = light pos - pos, atten = 1 (ground under water: max(0, 1 - material +0x14 * depth^2 * Lz^2 / 15)), o... |
| 0x006C3E80 | 1046 | TerrainSector_Animate | (sector, terrain, tick) from CTerrain::Tick, once per 55 ms tick for every visible lit sector: tick % 3 == 0 and flag 2: animated tiles +0x364 advance (mid-cycle or chance >= 1000 always, at frame 0 when ISAAC % 1000 + 1 <= chance... |
| 0x006C5AA0 | 1011 | CTerrain::LoadTextures | (count) -> bool: the texture array +0xD8 and material array +0xE8 get count entries '<stem>%3.3i' (texture manager g_0x7AF744 FindOrCreateNamed kind 0xB, refcount++) plus, for each of the two optional lightmap pages '<stem>L%2.2i'... |
| 0x006C8ED0 | 925 | CTerrain::SetSunLight | (sunLight, ambient float[4]): ambient -> +0x224..+0x230; direction towards the sun = -(light +0xD4..+0xDC), components clamped to /c/ >= 0.01, normalised; +0x234 = nz/nx, +0x23C = nz/ny, +0x238 = nx/nz, +0x240 = ny/nz, +0x244 / +0... |
| 0x006C8140 | 913 | CTerrain::GetSector | (sx, sy, wantedFlags, load) -> sector or 0: the grid +0x1F8[sx][sy] holds the node of the loaded-sector CList +0x1FC (MRU: a touched sector whose stamp +0x384 is older than the frame +0x218 moves to the head); with load: a missing... |
| 0x006C8990 | 905 | CTerrain::SnapQuadsToGround | (array {0x40-byte quads (4 x at +0, 4 y at +0x10, 4 z at +0x20), count}): every corner z becomes the average height of the 4 vertices of the tile under it (sectors loaded with GetSector flags 1); the corner -> sector / tile mappin... |
| 0x006C9560 | 897 | CTerrain::TraceRayToGround | (origin, rotation quat (w, x, y, z), maxDist, out hit) -> bool: the quaternion rotates the forward axis into a step direction (normalized by its larger x / y component); walks up to maxDist unit steps, loading the sector under eac... |
| 0x006CA260 | 867 | CTerrain::SetVisibleArea | (x, y, radius): the visible rows +0xAA4 / +0xAAC and per-row column ranges +0xAA8 / +0xAB8 = every sector overlapping the square [x - r, x + r] x [y - r, y + r] (floor((v - 0.5) / 32), clamped to the map); THE live terrain visibil... |
| 0x006CAC10 | 859 | TerrainSector_LinkLight | (sector, light, pos, rect, forceRemove): for every vertex in the rect's sub range (33-wide rows): unlink the light's node (or skip) when forceRemove or /vertex - pos/^2 >= radius^2 (+0x100), else push a node {next, prev, chunk, li... |
| 0x006C36B0 | 843 | TerrainSector_LightVertexSun | (sector, sun, pos, record, terrain, isWaterLayer): amb = terrain +0x224.., base = record +0x43.., I = 1; ground vertex under water (water record +0x25 != 0xFFFF, depth +0x27 >= 0): k = material +0x14 * depth^2 / 15, amb *= max(0, ... |
| 0x006C4310 | 769 | TerrainSector_PackVertex | (sector, vertexIndex, out 8 bytes): byte x / y offsets within the sector (126 / 0.5 units), ushort z (65534 / max height +0x114 of the terrain +0x1c), dword packed normal (11 bits each: nz * 1000, (nx + 1) * 1000, (ny + 1) * 1000)... |

## 0x6CC000 - 0x6E0000: terrain prints (foot/blood/fire decals), terrain tile helpers, CBitmapDib (BMP/mip loading), database.res loaders (CDatabase/CDatabase2/CDatabaseTables)

365 functions, group `renderer`.

| addr | size | name | purpose |
|---|---|---|---|
| 0x006D7620 | 3520 | CAcksDb::Load | (dataDir) -> bool: <dir>database.res / acks.db ('Can't open acks database'); tags 1 / 2 / 3 Answers (0x1C4) / Cryes (0x5C) / Others (0x38) into +0 / +0x20 / +0x40 with name maps; then per record an ack id -> list map at +0xC: Answ... |
| 0x006DE370 | 2156 | AcksAnswer::CopyCtor | copy-constructs an acks.db Answers record 0x1C4: the name std::string, the id -> list hash map +0xC and the 0x16 voice-line vectors +0x20 (0x28 lines each, PtrVector_CtorN_6DEF90 + copies); used by AcksAnswersVector_Resize / _Inse... |
| 0x006CCF20 | 1567 | CTerrain::BuildShadowReceiverMesh | (clipBuf, corners SoA float[16]): the terrain cell box under the shadow prism (the rectangle slid up the light ray to the highest vertex of the box, 1-cell margin, clamped to the map), ClipBuf::ReserveForTriangles(((nx+1)*(ny+1) +... |
| 0x006D61D4 | 1557 | sub_6D61D4 | unregistered function, reached by call / pointer from 0x2D617D (gaps.py 2026-11-06) |
| 0x006D5BF0 | 1475 | CBitmapDib::LoadMmpMipLevel | (stream, level) -> bool: MMP header 0x4C ('read error 1'; 'wrong level for mipmap' when width < 2^level, AfxMessageBox), seek to the level (hdr +0x48 extra + level sizes), DIB of w >> level x h >> level: DXT1..5 decoded to RGB565 ... |
| 0x006DC390 | 1423 | AcksAnswer::~AcksAnswer | destroys a 0x1C4-byte Answers record (hash tables, line vectors, strings) |
| 0x006CEDE0 | 1324 | CPrintDecal::Render | (this) -> bool: inactive (+5 == 0) -> 1; mesh not built (+4 == 0): z0 = CBridgeManager::GetHeightAt(world +0x1D4, x, y, world vt[27] terrain height), the 4 rectangle corners (half extents +0x10 / +0x14 rotated by +0x44) at z0 -> C... |
| 0x006D0630 | 1106 | CPrintsDb::Load | (dataDir) -> bool: opens '<dir>database.res' (CMappedResource; FatalError 'Can't open database file' naming databases.res) and its 'prints.db' ('Can't open terrain prints database'); the Archive tags 1 / 2 / 3 (database-format.md ... |
| 0x006DF3E0 | 972 | AcksLineRange_Assign2 | STL: vector<voice-line record 0x28>::assign(first, last) variant (String_Assign per line, Rec28Vector_Assign / Rec2CVector_Assign for the nested lists, realloc via Rec28_AllocCopy); the line list copy of AcksAnswersVector_Resize. ... |
| 0x006DC000 | 910 | AcksAnswer::AcksAnswer | constructor of a 0x1C4-byte Answers record: name string, the id -> list hash (+0xC), 0x19 vectors of 0x28-byte voice-line records {wav path, weight, subtitle, text id} (+0x20 + k * 0xC) and the remaining fields; was CDatabaseTable... |
| 0x006DD6C0 | 875 | AcksCryesVector_AssignN | STL: vector<Cryes record 0x5C>::_M_fill_insert / assign n copies of one record (each record = std::string name, a vector of 0x28 voice-line records and a hash set; in-place copies or AcksCryesVector_InsertRealloc); used by AcksCry... |
| 0x006D8430 | 796 | CUnit::PlayAck | (this = client CUnit, code): plays the unit's acknowledgement for a unit event code (0xFF: respawn vt+0xD0): heroes below a counter remap codes 0..10 -> 0x27 / 0xB..0x13,0x1D -> 0x28; 0x25/0x26 weapons\timecrash.wav only, 0x23/0x2... |
| 0x006DDA30 | 755 | AcksOthersVector_AssignN | STL: vector<Others record 0x38>::_M_fill_insert / assign n copies of one record (std::string name, a 0x28 voice-line vector and a hash set; in-place copies or AcksOthersVector_InsertRealloc); used by AcksOthersVector_Resize (CAcks... |
| 0x006CCC40 | 736 | dead_6CCC40 | unregistered code with no call, jump or pointer anywhere in the image (gaps.py 2026-11-06) |

## 0x6E0000 - 0x73B000: MSVC runtime (CRT)

1926 functions, group `crt`.

| addr | size | name | purpose |
|---|---|---|---|
| 0x006F6F2C | 2597 | _input | CRT (input.c _input(stream, format, args)): the scanf engine: white space (_whiteout), literal matching, %[*][width][h l L I64] c s [ ] d i o x X u n e f g p, DBCS lead bytes, hex via _hextodec, floats through _fassign; returns th... |
| 0x006F17D4 | 1825 | _output | CRT (output.c _output(stream, format, args)): the printf engine: state table 0x7476B0 / 0x7476D0, flags - + space # 0, width / precision (* from args), h l I64 L size, %c %C %s %S %Z %d %i %u %o %x %X %p %n %e %f %g %E %G (through... |
| 0x006FB9C0 | 1668 | dead_6FB9C0 | unregistered code with no call, jump or pointer anywhere in the image (gaps.py 2026-11-06) |
| 0x006E3780 | 1585 | AcksAnswer::Serialize | (this = AcksAnswer 0x1C4, archive): tag 1 name string +0; tags 10..0x26 the voice-line vectors through Archive_SerializeRec28Vector (+0x20, +0x2C, .. in a fixed tag -> offset order, e.g. 0x14 -> +0xA4, 0x21 -> +0xB0, 0x24 -> +0x98... |
| 0x0070592E | 1198 | CWnd::OnWndMsg | vt slot 39: the central message dispatcher (commands, notifications, sys colors, ctl colors) via message maps, 1.2 KB |
| 0x006F9989 | 1185 | _strgtold12 | CRT (strgtold.c __strgtold12(_LDBL12*, &end, str, mult12, scale, decpt, implicit_E)): state machine over sign, digits, decimal point (locale *_decimal_point), exponent (e / E / d / D), up to 21 significant digits into a buffer, th... |
| 0x006F2F30 | 1142 | sub_6F2F30 | unregistered function, reached by call / pointer from 0x398382 (gaps.py 2026-11-06) |
| 0x006EE4B3 | 1007 | _expandtime | MSVC6 CRT: static _expandtime(spec, tm, &out, &left, lc_time): one strftime letter: a / A / b / B names from lc_time (+0 / +0x1C / +0x38 / +0x68), c / x / X through _store_winword with the ww_ formats, d H I j m M S U W w y Y numb... |
| 0x006E0540 | 1006 | AcksAnswerRange_Copy_A | STLport: std::copy(first, last, dest) for AcksAnswer 0x1C4 records, FORWARD: per record String_Assign +0, HashSet_Assign_6E14E0 +0xC, inline vector<line 0x28>::operator= +0x20, Rec28Vector_Assign for the 32 line vectors +0x2C..+0x... |
| 0x006E1570 | 1002 | AcksAnswerRange_CopyBackward | STLport: std::copy_backward(first, last, dest_end) for AcksAnswer 0x1C4 records (the real backward one): from the end, per record String_Assign +0, HashSet_Assign_6E14E0 +0xC, inline vector<line 0x28>::operator= +0x20, Rec28Vector... |
| 0x006E0930 | 860 | AcksCryRange_Copy | STLport: std::copy forward for AcksCry 0x5C records: string assign +0 (String_AssignRange2), HashSet_Assign_6E14E0 +0xC, inline vector<line 0x28>::operator= for +0x20 and +0x2C, Rec28Vector_Assign +0x38/+0x44/+0x50; caller AcksCry... |
| 0x006F3E9F | 846 | _get_lc_time | CRT (inittime.c _get_lc_time(__lc_time_data*)): fills the LC_TIME table from the LC_TIME lcid (0x7CAC4E) / ANSI code page through _getlocaleinfo: abbreviated day names 0x31..0x37 (+0..+0x18), day names 0x2A..0x30, abbreviated mont... |
| 0x006FDC12 | 845 | CString::FormatV | MFC (void CString::FormatV(lpszFormat, va_list)): estimates the maximum length by parsing every % item (flags, width / precision incl. '*', size prefixes h l I64 F N, types c C s S d i u x X o e f g G p n; strings by their length,... |
| 0x006F62D5 | 815 | __sbh_heap_check | CRT (sbheap.c __sbh_heap_check): validates every region (IsBadWritePtr of the headers and group data, size chains, bucket links and counts against the bitmaps); returns 0 or a negative error code; lib, unused (no call, no pointer,... |

# Net / players / party class notes (game.exe, Russian build) — 2026-10-17

> **2026-10-27:** CPlayer / CPlayerClient / CPlayerServer layouts are corrected in [world-client-players.md](world-client-players.md) (CPlayerClient is 0x50F8 and has no DeltaCodec member; CPlayerServer +0x50F8 known objects, +0x510C visible units, +0x5120 party status mode; CPlayer +0x118 synced item ids).


Raw notes from reading modules 0x411000-0x433000, 0x433000, 0x474000, 0x47E000, 0x664000, 0x65F000. Function names are in
functions.tsv; this file keeps the layouts that the one-line purposes cannot hold. To be rewritten into docs/game-exe/net.md.

## Streams
- IStream (vt 0x73BDB0, 8 slots): 0 dtor, 1 Read(buf,n), 2 Write(buf,n), 3 Seek(pos), 4 GetSize, 5 GetPos, 6 Clear, 7 CopyFrom.
- CMemStream (vt 0x73C960, base 0x73BD90): {vt, +5 ok byte, +0xc bit byte, +0x10 bit mask (0x80), +0x14 bit pos (-1), +0x18 buf,
  +0x1c cap, +0x20 pos, +0x24 size}; ctor 0x41DD70 / 0x66C4F0, dtor 0x4173D0, Clear 0x417350, SetSize 0x4A54D0; bit I/O
  BitWriter_PutBit 0x47DFC0 / BitWriter_Flush 0x4A5610 / BitReader_GetBit 0x47EAE0. Length-prefixed chunks: 0x4A5250 write,
  MsgStream_ReadLenPrefixed 0x47DAE0 (1- or 4-byte prefix, bit 0 = long form, max 100000 bytes).
- CFileStream (vt 0x73C8D8) = IStream over a CFile at +0xc. CFile = {vt, hFile, bCloseOnDelete, CString name}, ctor 0x701036,
  dtor 0x70112B, Open = slot 8, Read +0x34, Write +0x38.
- MsgStream family (IMsgStream vt 0x73C8A4, 13 slots): FileMsgStream 0x73C8F8 (0x30 bytes: CFileStream +0xc, chunk list +0x28 of
  0x30-byte nodes holding CMemStreams, link +0x2c), DualFileMsgStream 0x73C92C (0x44), MsgStreamBase 0x73C9C8, PeerMsgStream
  0x73C990, TcpMsgStream 0x73CA10 (0x54: CFileStream +0xc, CMemStream +0x28, ref +0x50 = NetPeer or TcpSocketRef 0x73CA00
  {vt, count, socket}). Shared slots: 6 HasPacketHeader, 7 CountPackets, 8 SendBuffered (calls vt[13]), 9 PopPacket; PeerMsgStream
  10 PullFromPeer (NetPeer +0x1148), 11 FlushToPeer, 13 Write into NetPeer +0x112c; TcpMsgStream 13 = send().
- CTcpNetDriver: +8 list of PeerMsgStreams, +0xc list, connect task (TcpAsyncTask 0x73CA08 / 0x73CA0C); AcceptNewClient 0x47CFA0,
  SyncPeers 0x479E30, ConnectAsync 0x47C8E0 (thread 0x47C680, default port 8000), TakeConnectResult 0x47CAA0.
- LZ: LzCompress 0x430450 (1 KB ring window), LzDecompress 0x430960, LzCompressor state 0x2828 bytes (Reset 0x42FCA0,
  EndAndReset 0x6671A0). HuffmanCoder 0xc08 bytes (codes +0, lengths +0x400, freq +0x800, root +0xc04; BuildTree 0x47A8B0).
  BigNum_ModExp 0x431370 + Ripemd128_Compress 0x4319E0 + KeyHash_Derive 0x431300 (session key).
- ChunkFile (vt 0x73BE94 / 0x73BE9C): {vt, tag +4, consumed +8, length +0xc, start +0x10, parent chunk +0x14, CFile +0x18};
  slot 0 OnChunk, slot 1 GetFile; Consume 0x42BF00, BeginChunk 0x42BF50, EndChunk 0x42BFB0, NextChunk 0x42C0B0, ForEachChunk 0x42C140.

## Delta codec (unit state replication)
- DeltaCodec 0xa0 bytes: base snapshots +0 / +0x28, delta bit streams +0x50 / +0x78; ctor 0x66A300, dtor 0x66A280. Globals
  g_0x7A9794 active base, g_0x7A9798 active bit stream, g_0x7A9784 / g_0x7A9780 update flags.
- DeltaWriter_WriteFloat16 0x47F1E0, WritePos3D 0x47E3A0 (x,y: (v+16)*120.46875; z: (v+32)*341.32812), WriteVec3F16 0x47F4D0,
  WriteFloatRange 0x47EF80; readers 0x47F3D0 / 0x47E930 / 0x47F7F0; Begin/End 0x47FA40 / 0x47FAA0 / 0x47FAF0;
  DeltaCodecSet::Pack 0x47FE90, Reset 0x47FB70, SetMode 0x47FD50. MoveState_WriteNet 0x5C7DB0 writes section 10.

## NetPeer / NetConnection (UDP object replication, lobby level)
- NetPeer (vt 0x73BF38, 0x1168 bytes, ctor 0x432DA0, dtor 0x433320, Clone 0x433060): +8 short tick, +0x20 id/flag, +0x24 timeout
  (registry g_0x79B920 +0x10, default 5.0 s), +0x54 256 x {vector<short>, short}, +0x1054 list, +0x105c id->object tree, +0x1068
  object->NetPendingEntry (0x73BF4C) tree, +0x1074 11 per-kind trees, +0x10f8 / +0x10fc 0x120-node lists, +0x1100 ScriptRefTree
  (pending replicated), +0x110c PairTree object->{short id, byte kind}, +0x112c incoming GameBuffer, +0x1148 outgoing GameBuffer;
  ReadObjectRef 0x435820, RegisterObject 0x434110, SetObjectId 0x4352F0.
- NetConnection (vt 0x73BF5C, ctor 0x436770, dtor 0x436A50 sends {1, peer id} disconnect): +8 protocol magic 0xdec0ad07, +0x28 peer
  list (0xc nodes), +0x2c class hash (vtable -> class id), +0x40 id -> creator hash, +0x54 NetSocket (UDP; ctor 0x4407F0 WSAStartup /
  gethostbyname, Open 0x4409A0 SO_BROADCAST, dtor 0x440940), handler tables +0x164 (created) / +0xd64 (updated) / +0x1964
  (removed) indexed by class id * 0xc; AddHandler 0x4377D0(table, classId, cb). NetClassRegistry::Register 0x42A890.
  NetAddress::ToString 0x440690 / FromString 0x440440 (default master port 28004). NetServer_TickPeerTimeouts 0x47C010 (30 s).
- NetMessage base (vt 0x73BC10, 6 slots: 0 dtor, 1 ref, 2 Reset, 3 Clone, 4 Write(cursor*), 5 Read(cursor*)). Classes: JoinMessage
  0x73BBF8 (0x34), ChatMessage 0x73BC28, NamedEventMessage 0x73BC40, TwoRefMessage 0x73BC58 (0x14), TwoStringMessage 0x73BC70
  (0x3c = player record: name, +0x24, ready byte +0x28, id +0x2c), IdStringMessage 0x73BC88 (chat line: id +0xc, text +0x10, peer
  ref +0x1c), StringPairMessage 0x73BCA0, TwoRefMessage2 0x73BCB8, StringMessage 0x73BCD0 (map name), StringDwordsMessage
  0x73BCE8 (variable: name, value +0x18, serial +0x1c), StringMessage2 0x73BD00 (current map), ListStringMessage 0x73BD18 (file
  transfer: chunk count +0xc, size +0x10, name +0x14, chunk refs +0x20), RefBlobMessage 0x73BD30 (0x54: owner +0xc, index +0x10,
  64-byte payload +0x12), RefDwordsStringMessage 0x73BD48 (player event: player ref, kind, value, string), ObjPairMessage 0x73BD60
  (0x28: 4 refs, byte +0x1c, +0x20 / +0x24), BlobMessage 0x73BD78 (CMemStream +0xc), ObjListMessage 0x73BDD0 (0x34 pairing record:
  ref vector +0xc, +0x18.. +0x24, refs +0x28 / +0x2c, byte +0x30), ServerInfo 0x73BB28 (0x48: NetPeerInfo +8..+0x24, name +0x28,
  +0x34, map +0x38, bytes +0x44..+0x46), NetPeerInfo 0x73BB40.

## Lobby
- NetClient (vt 0x73BE30) -> LobbyClient (0x73BDEC); NetServer (0x73BBB4) -> LobbyServer (0x73BB70). (ctor sets base vtable first.)
- NetClient: +0x2568 address, +0x2578 login GameBuffer, +0x2594 session id, +0x2598 state, +0x25a8 server info list, +0x25ac
  NetServerRecord vector (0x2c: addr + GameBuffer), +0x25bc keep-alive pending list; TickDiscovery 0x442180, BeginConnect 0x442980,
  AddServerInfo 0x4428A0, SetServerList 0x442300, OnServerFound 0x442590, RemoveServer 0x4427F0, dtor 0x41C530.
- LobbyServer (ctor 0x412D80, dtor 0x417BC0): +0x2568 pending joins, +0x2570 heartbeat timer, +0x2574 public flag, +0x2578 public
  address, +0x257c master address, +0x258c chat counter, +0x2590 players (TwoStringMessage), +0x2594 chat lines (max 50), +0x2598
  file transfers, +0x259c chunk records, +0x25a4 peer records {+0xc player, +0x10 peer}, +0x25a8 current map (StringMessage2),
  +0x25ac map list, +0x25b0 variables, +0x25b4 timed player events (60 s), +0x25b8 pairing records, +0x25bc ObjPairMessages,
  +0x25c0 timed list, +0x25c4 ban list, +0x25c8 server name, +0x25d4 map name, +0x25e0 max players (8), +0x25e8 password[20],
  +0x25fc master request id. Slots: 10 OnMasterPacket, 11 OnPeerTimeout, 12 OnPeerDisconnect, 13 GatherReplicated, 14 Tick,
  15 FillServerInfo, 16 CanAccept (1 ok / 2 banned / 3 bad password). SetMap 0x4186F0, UnloadMap 0x418CC0, RefreshMapList 0x418530,
  AddFileTransfer 0x419A00, PostChat 0x41A310, PostSystemChat 0x41A490, SendMasterHeartbeat 0x41AA10, KickPeer 0x41A550, BanPeer
  0x41A620, OnPairRecordAdded 0x41B7A0, OnPairRecordRemoved 0x41BA30, ConfirmPlayerPair 0x41B970.
- LobbyClient (ctor 0x41BC40, dtor 0x41CA10): +0x25d8 players, +0x25dc chat, +0x25e0 transfers, +0x25e4 chunks, +0x25e8 requests,
  +0x25f0 request ref, +0x25f8 maps, +0x25fc variables, +0x2600 join message, +0x2604 keyed refs, +0x2608 player events, +0x260c
  local pairing record, +0x2610 pairing records, +0x2614 party list (0x160 nodes: three 0x70-byte CParty blocks), +0x2618 pair
  confirmation, +0x2620 / +0x2690 party blocks; slot 13 GatherReplicated 0x41D050, slot 16 NewServerInfo 0x41D420;
  OnChunkReceived 0x41ECC0 (sorts chunks, saves via 0x5813A0), OnFileTransferAdded 0x41EAC0, ApplyPlayerEvents 0x41CD80 (kinds
  0..4: ?, money, give item, take item, clear), LoadPartyFromMirror 0x41DDC0, ConfirmPair 0x41E0A0, CreateLocalRecord 0x41E270.

## In-game protocol (module 0x664000)
- CPlayer (vt 0x743104, ctor 0x664BF0, 0x134 bytes): +0xc id, +0x1c owner (short refcount), +0x20 connection (CPlayerClient /
  CPlayerServer, slots 13..22 callbacks), +0x24 player id, +0x28 unit hash (id -> CUnit, count +0x38, max +0x3c), +0x40 tracked
  unit hash (party units), +0x58 object hash (count +0x68, max +0x6c), +0x70 CParty block, +0xa0, +0xa4, +0xc0 money, +0xc4 named
  value table, +0xd8 map name, +0xdc entry, +0xe0 item CList (0xc assocs), +0x100 / +0x104 game state (new / previous), +0x108,
  +0x10c log flag, +0x110, +0x114 id CList (vt 0x743138), +0x130 flags.
- CPlayerClient (vt 0x743180, 0x5128 bytes): +0x20 state, +0x24 handler table / +0x28 count (DispatchPackets 0x667970), +0x34
  receive CMemStream, +0x5c send CMemStream, +0x84 / +0x85 lobby bytes, +0x88 and +0x28b0 LzCompressor, +0x2898 DeltaCodec,
  +0x50d8 party unit id array (aligned), +0x50e8 4 dwords. Sends on +0x5c (vt slot 2 = Write) then FlushSendStream 0x667810
  (compress + owner net object +0x1c vt[8]).
  Client -> server opcodes: 2 start file, 3, 4 leave, 5, 6 string, 0x10 flag, 0x30 / 0x31 / 0x36 (order) / 0x3a id lists,
  0x32 / 0x33 use weapon (decrement subtype-8 charges), 0x34 / 0x35, 0x39, 0x3b, 0x50 party (MpFile::Write).
- CPlayerServer (vt 0x743280): +0x20 state (1 in game, 3 leaving), +0x48 send CMemStream, +0x70 in-world flag, +0x88 / +0x28b0
  LzCompressor, +0x50d8 unit id array, +0x50f8 / +0x510c object arrays (known / visible, UpdateKnownObjects 0x66E130), +0x5110
  / +0x5114 party unit array, +0x5120 danger level (1 low stats, 2 combat), +0x5124 party resend countdown (900..1800).
  Handlers read the current packet stream g_0x7C83D0. Server -> client opcodes: 2 blob, 4 position, 7, 8 change map (name +
  entry), 0xb, 0xd string, 0xe move state (delta), 0xf move, 0x12, 0x15, 0x16, 0x18, 0x19, 0x40 units (SendMsg40_Units), 0x41
  figures, 0x42 untrack, 0x43, 0x50 objects, 0x51, 0x52 remove objects, 0x60 party status, 0x70..0x73 owner flag bits, 0x80 party
  (MpFile), 0x81 named value, 0x82 name, 0x83 party.
- CMappedResource (vt 0x7432E0, derived 0x743300): {vt, header +4 magic 0x019ce23c, +8 entry count, +0xc data offset, +0x10 data
  size, +0x14 entry table (0x16 bytes each: name hash?, +4 size, +8 offset), +0x18 data, +0x24 file handle, +0x28 mapped base, +0x2c
  writable, +0x30 buffer->entry hash, +0x44 name->entry hash}; entries 0x1c bytes {std::string name, buffer +0xc, size +0x10,
  refcount +0x14, offset +0x18}. Slots: 1 Open(path, writable), 3 Release, 4 OpenFile(name, &size), 5 CloseFile(buffer).

## CParty (module 0x65F000)
- CParty (vt 0x743000, base 0x74306C "object container" slots 13..26, 0x70 bytes, ctor 0x661050, dtor 0x6611D0): +0xc = -1,
  +0x1c / +0x20 / +0x24 / +0x28 managers (slot 18 / 16 / 22 / 17 getters; slot 24 = CreateObject clone), +0x2c HeroRecordArray
  (vt 0x742FF4: data +0x30, count +0x34), +0x40 party item array (aligned; item +0x48 = stack count), +0x50 money, +0x54 named
  value hash (party variables, "z.<zone>" visited flags), +0x68 map name CString, +0x6c. PartySlot = CParty + file name CString
  +0x70 (0x74 bytes); PartyStore {CString dir, vector<PartySlot>, CParty +0x10, CString +0x80} loads "*.mp" with MpFile::Load.
- HeroRecord (0x784 bytes, ctor 0x65F5F0, dtor 0x65F740): CStrings +0 display name, +4 model name (+0x1c = its hash id), +8 class
  name, +0xc, +0x10, +0x14 prototype name, +0x18 last applied prototype; +0x20 stats block (0x704 bytes, UnitStats_ReadSave); floats
  +0x724 / +0x728 / +0x72c (0.8 / 0.2 / 1.0 from the prototype); aligned item arrays +0x730 inventory, +0x740 weapon slots (item +0x48 = ammo count),
  +0x750 armor pieces, +0x760 quick slots (spells / scrolls / potions, created with the +0x24 manager); CPlayer +0x70 IS a CParty; +0x770 = 2, +0x774, byte +0x778, +0x77c = -1,
  +0x780 = 1 (save-version gated: > 0x50, > 0x5a, > 0x60, > 0x6a).
- MpFile::Read 0x6618C0 / Write 0x661C10 / Load 0x662DD0 serialize a CParty to a CMemStream (used for the net party sync 0x50 / 0x80 / 0x83).

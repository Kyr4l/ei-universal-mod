# The network layer of game.exe: UDP transport, lobby objects, streams and codecs (NetConnection / NetPeer, LobbyServer / LobbyClient, MsgStream, LZ, delta codec)

Written 2026-11-01 from the typed Ghidra decompiles of the Russian `EIStarter OBT-1/Engine/game.exe` (modules 0x411000-0x42E000 lobby and
message classes, 0x42D000-0x433000 ChunkFile / GameBuffer / LZ / dead crypto, 0x433000-0x444000 transport, 0x474000-0x481000 streams,
MsgStream family, Huffman, delta codec, GameException / FatalError). The in-game protocol that rides on top (opcodes 0x00-0x83 of
`CPlayerClient` / `CPlayerServer`) is `world-client-players.md`; the session glue (`Lobby_*`, `Session_*`) is `world-server.md` 2.6. Batch files:
`_cpr/claude-re/full/batch-net.tsv` (220 function rows) and `batch-net-structs.tsv` (309 field rows).

Tags: **[V]** read directly in the decompile (or the asm), **[I]** inferred from several functions, **[?]** guess / not resolved.
Addresses are VAs; offsets are bytes; "peer" = a `NetPeer` (one per remote endpoint); "link" = the byte stream of a peer.

## How to re-implement it (short version)

1. **One UDP socket per side** (`NetSocket::Open` 0x4409A0): the server binds 8888 (`g_0x783E30`), a client any port. Datagrams are at most
   2048 bytes received, about 220-230 sent. The first byte is the type: `0` update, `1` disconnect, `3` get-info, `4` login, `5` info,
   `6` accepted, `7` rejected, `0xFF` master-server reply (`NetConnection::Receive` 0x437370). There is no TCP in a game: the only TCP
   socket is the master-server list download of the lobby browser (`TcpStream` 0x440C60); the "CTcpNetDriver" is historical (section 4.9).
2. **Handshake** (3.3): `C->S {3, f32 clock}`, `S->C {5, f32 echo, u32 magic 0xDEC0AD07, ServerInfo}` (optional, the browser's ping),
   `C->S {4, u32 magic, u32 linkId, u32 rate, password[20] or nothing}`, `S->C {6, linkId}` or `{7, linkId, u8 reason}`. The server creates
   the peer only on the client's **first type-0 update**; a login sits in a pending list for 60 s. Both sides then exchange type-0 updates
   every `250 / rate` s (20 ms LAN, 100 ms internet) for as long as the link lives; 60 s of silence drops it.
3. **The update packet** (3.4): `u8 0, u16 seq, u16 ack, u32 ackBits, u16 chunkOffset, u8 chunkLen, chunk, u8 n, n messages`. Sequences are
   16-bit on the wire, unwrapped to 32-bit by the receiver; `ack` / `ackBits` acknowledge the 33 most recent packets of the peer. A packet is
   built with a **220-byte payload budget** (`NetPeer +0x50`): the stream chunk takes at most 204 bytes, messages are added until the
   budget is passed.
4. **Reliability is per object, not per packet** (3.6): the sender keeps one `NetPendingEntry` per replicated object per peer with the
   object version last written; a lost packet (RTT-based timeout, 0.1..5 s) resets the versions of the messages it carried so they go out
   again; an acked removal frees its id. Nothing is retransmitted byte-for-byte except stream chunks, which are re-queued at their old
   offset. A duplicate or too-old (> 32 behind) sequence is dropped on receipt.
5. **The byte stream** (3.7) is an ordered stream per direction cut into chunks `{u16 offset, u8 len, data}` carried one per update and
   reassembled in order; it carries the in-game link: frames `{u32 n, u32 rawLength, LZ tokens}` (5.5) with the opcodes of
   `world-client-players.md` inside. The lobby never uses the stream; the game never uses reliable messages.
6. **Reliable messages are replicated lobby objects** (3.8, 4.2): `[flag bit] u8 kind u16 id payload`, keyed `kind << 16 | id`, kinds 1..17
   registered by `RegisterMessageFactory` 0x4142F0; flag 1 = create / update (the packet seq is the object version), 0 = removal. Each side
   gathers "what this peer must see" every send (`vt[13] GatherReplicated`) and the diff against the last gather becomes creations /
   removals. The server replicates players, chat, quests, variables, file transfers, swap records; a client only its `JoinMessage`, timed
   named events / chat lines, chunk requests and its swap offer.
7. **Lobby to game**: the host's server opens the in-game link for a peer when its `JoinMessage` state is 3 and stream bytes have arrived
   (`CTcpNetDriver::SyncPeers` 0x479E30 -> `CWorldServer::AcceptPlayers` 0x58DAF0); a client wraps its single server peer in a
   `PeerMsgStream` and makes it `CPlayer +0x1c` (`CWorldClient::ConnectToLobbyHost` 0x566D80 -> `ConnectBlocking` 0x47A080), dropping the
   frames that queued during the lobby. The same NetPeer, socket and sequence space continue.
8. **Dead code to ignore**: `KeyHash_Derive` / `BigNum_ModExp` / `Ripemd128_Compress` (no callers: no key exchange, no CD check),
   the `HuffmanCoder` (no constructor, no caller), the TCP driver (`AcceptNewClient`, `ConnectAsync`, `TcpMsgStream`, no callers),
   `NetClient::SendAccept/RejectPacket_unused`, `TcpDriver_TickPeerTimeouts_dead`.

## 1. Conventions

| Thing | Value |
|---|---|
| integers / floats | little-endian; `f32` IEEE; times are seconds as `float` (timers) or `double` (clocks) [V] |
| strings in lobby payloads | `JoinMessage` / `TwoStringMessage` / `ServerInfo` write C strings **with** the NUL (`strlen + 1` bytes: 0x4136B0, 0x414770, 0x412990); the single-string kinds 2, 3, 6, 7, 8, 9, 10, 11, 14 write `std::string` contents **without** NUL through their own Write slots (0x413B50.., the reader takes the rest of the message) [V] |
| object ids | `u16` per kind, allocated from 1 by the sender (`NetPeer::AllocSeqId` 0x435F10), recycled 75 s after an acked removal; 0 = none [V] |
| object refs inside payloads | one flag bit + `u8 kind` + `u16 id` (`NetWriter::WriteRef` 0x4355B0 / `NetPeer::ReadObjectRef` 0x435820): bit 0 = an object the writer created, bit 1 = one the reader created (its own id space); `{0, 0}` = null [V] |
| flag bits | taken LSB-first from a flag byte that is reserved in the packet before the first message of every group of 8 bits (`NetCursor` +8 / +0xc, 0x4DA490); refs consume bits of the same byte, so the n-th message's bit is not simply bit n [V] |
| sequences | `u16` on the wire; the receiver keeps `wrapBase` (0x10000 at start) and adds / subtracts 0x10000 when the 16-bit distance to the last received crosses 0x8000 (`ParseUpdate` 0x436470) [V] |
| versions | an object's `+8` counter, bumped by its owner after every change; on the wire the version of a received object = the unwrapped seq of the packet that carried it [V] |
| rate | bytes per second as the login says it: 12500 (`g_0x783E38`, LAN) / 2500 (`g_0x783E34`); send interval `250 / clamp(rate, 500, 50000)` s [V] |
| magic | `0xDEC0AD07` (`NetConnection +8`); `OnInfo` compares the top 24 bits only, the low byte is the protocol version (`ServerInfo +0x20` mismatch flag) [V] |
| net log | `g_0x79B91C` ostream (console `net server 1` / `net client 1`); the quoted CAPITAL messages below are its lines [V] |
| globals | `g_0x79B920` = the NetConnection being serviced (Receive), `g_0x79B924` = the NetPeer whose update is being parsed (the lobby handlers read the sender from it), `g_0x79B900` send GameBuffer, `g_0x79B8E0` receive GameBuffer, `g_0x79B8D0` gather vector; `g_0x79B5DC` LobbyServer, `g_0x79B5D8` LobbyClient (alive when `+4` bit 31 is clear) [V] |

## 2. Call chain

`Net_TickAll` 0x4122D0 (main / `CWorldClient::Render` / `Session_ProcessRequest` / `Movie_PlayIntro`; dt = ms since the last call / 1024,
1 s when > 5 s) -> `LobbyServer::v14_Tick` 0x4182E0 then `LobbyClient::v14_PerFrame` 0x41CCD0 -> `NetServer::Receive` 0x443590 /
`NetClient::Receive` 0x442AD0 (handshake ageing, login retries) -> `NetConnection::Receive` 0x437370: `g_0x79B920 = this`, clock `+0x20 += dt`,
then **every waiting datagram** (`NetSocket::ReceiveFrom` 0x440BE0, "ZERO length packet received from" when empty) dispatched by its type
byte to the vtable: 0 `vt[3] OnUpdate`, 1 `vt[6] OnDisconnect`, 3 `vt[4] OnGetInfo`, 4 `vt[5] OnLogin`, 5 `vt[7] OnInfo`, 6 `vt[8] OnAccepted`,
7 `vt[9] OnRejected`, 0xFF `vt[10] OnMasterPacket` (the stubs 0x4371E0.. log "ERROR: Can`t handle ..."); then **`NetPeer::Tick(dt)`
0x433BD0 for every peer** (resend sweep, send cadence, id recycling, timeout -> "TIMEOUT client, drop", `vt[11] OnPeerTimeout`,
`vt[2] RemovePeer`); `+0x18 ++`, `g_0x79B920 = 0`. [V]

`NetPeer::Tick` -> `NetPeer::SendUpdate(220)` 0x435A00 every `+0x3c` s (and again while the out stream still has bytes) -> `SyncObjectSet`
0x4346D0 (`vt[13] GatherReplicated` + diff) -> header, `WriteAcks` 0x434C20, `TakeStreamChunk` 0x434990, the messages (`vt[4] Write` of each
object through a `NetCursor`), `NetSocket::SendTo` 0x440B90. Receive side: `vt[3] OnUpdate` -> `NetPeer::ParseUpdate` 0x436470 ->
`AcceptSeq` 0x434E50, `ReadAcks` 0x434D70 (`AckOne` 0x434CE0 -> `OnPacketAcked` 0x433FA0), the chunk into `+0x1120`, `DrainChunks`
0x434AE0, n x `ReadMessage` 0x435F30 (factory 0x438930, `vt[5] Read`, the created / updated / removed handler vectors). [V]

The vtables: `NetConnection` 0x73BF5C (15 slots: 2 RemovePeer, 3..10 the packet handlers, 11 OnPeerTimeout, 12 OnPeerDisconnect,
13 GatherReplicated, 14 Receive / Tick), `NetServer` 0x73BBB4 (+15 FillServerInfo, 16 CanAccept), `NetClient` 0x73BE30 (+15 OnConnected,
16 NewServerInfo), `LobbyServer` 0x73BB70, `LobbyClient` 0x73BDEC; `NetPeer` 0x73BF38 (refcounted, 3 Clone); `NetMessage` 0x73BC10
(2 Reset, 3 Clone, 4 Write(cursor*), 5 Read(cursor*)). [V]

## 3. Transport

### 3.1 Sockets and addresses [V]

`NetSocket` (0x10 bytes at `NetConnection +0x54`): `ctor` 0x4407F0 = `WSAStartup(1.1)`, `gethostname` / `gethostbyname` -> one 16-byte
`sockaddr_in` per local interface in the vector `+4` (netmask guessed from the address class: `NetAddressList::IsLocal` 0x440AF0 treats
127.0.0.1 and any address matching an interface prefix as LAN); `Open(port)` 0x4409A0 = `socket(AF_INET, SOCK_DGRAM)`, `bind(htons(port))`
when port > 0 (the server: 8888; a client passes -1 = ephemeral), `FIONBIO`, `SO_BROADCAST`; `SendTo(addr, GameBuffer)` 0x440B90 sends
`validEnd - data + bias` bytes; `ReceiveFrom` 0x440BE0 reads up to 2048. `NetAddress::FromString` 0x440440 parses `host[:port]` (inet_addr
or gethostbyname; default port = argument, 28004 for the master), `ToString` 0x440690 formats `%i.%i.%i.%i:%i`.

LAN discovery: `NetClient::TickDiscovery` 0x442180 (lobby screen) sends `{3, f32 clock}` to the broadcast address of every interface on
port 8888, then to one queued / address-book server per call (round robin `+0x25d0`). Every server answers get-info at any time, in the
lobby or in a quest (`NetServer::v4_OnGetInfo` 0x443060 -> `ReplyInfo` 0x442C60).

### 3.2 Packet types [V]

| type | direction | payload after the type byte | sender | handler |
|---|---|---|---|---|
| 0 | both | the update (3.4) | `NetPeer::SendUpdate` 0x435A00 | `NetConnection::v3_OnUpdate` 0x437080 (server 0x4434F0, client 0x4418F0 only in state 2) |
| 1 | both | `u32 linkId` | `NetConnection::dtor` 0x436A50 to every peer; `SendDisconnect` 0x436FA0 (kick / ban) | `v6_OnDisconnect` 0x437240: the peer at that address with that linkId -> `vt[12] OnPeerDisconnect`, `RemovePeer` |
| 3 | C->S | `f32 clientClock` (any 4 bytes: echoed) | `NetClient::SendGetInfo` 0x441660 | `NetServer::v4_OnGetInfo` 0x443060 -> `ReplyInfo(addr, those 4 bytes)` |
| 5 | S->C | `u32 echo, u32 magic, ServerInfo` = `name\0, u32 gameKind, map\0, u8 players, u8 maxPlayers, u8 hasPassword` (`ServerInfo::Write` 0x412990; map = the quest name in a quest, empty in the lobby) | `NetServer::ReplyInfo` 0x442C60 | `NetClient::v7_OnInfo` 0x441D40: magic top 24 bits must match; new / replaced `ServerInfo` in `+0x25a8` with ping = clock - echo, source 0 LAN / 1 address book, timeout 20 s |
| 4 | C->S | `u32 magic, u32 linkId, u32 rate, payload` (payload = the 20-byte password when the lobby has one, else nothing: 13 or 33 bytes) | `NetClient::SendLogin` 0x441740 | `NetServer::v5_OnLogin` 0x443080 (3.3) |
| 6 | S->C | `u32 linkId` | `v5_OnLogin` | `NetClient::v8_OnAccepted` 0x441940 |
| 7 | S->C | `u32 linkId, u8 reason` 1 full / 2 banned / 3 bad password | `v5_OnLogin` | `NetClient::v9_OnRejected` 0x441B40: reason -> `+0x259c`, state 0 |
| 0xFF | master->S | `u32 requestId, u32 publicAddress` | the master server | `LobbyServer::v10_OnMasterPacket` 0x41ACA0 (4.8) |

Any other type byte is ignored (no default case). The server also accepts type 0 from an address with a pending accept record (3.3).

### 3.3 Handshake, connection states, disconnects [V]

**Client** (`NetClient`, `+0x2598` state): `BeginConnect(addr, payload)` 0x442980 (from `Lobby_StartServerOrClient` 0x57E740 /
`Lobby_BeginConnectFromAddress` 0x57E970) sets `+0x2568` = addr, `linkId +0x2594 = rand() << 16 | GetTickCount() & 0xffff`, state 1, retry
interval 0.1 s. `NetClient::Receive` re-sends `SendLogin` whenever the retry timer passes the interval and doubles the interval (0.1, 0.2,
0.4, 0.8, 1.6, 3.2 s): once it exceeds 3 s the log says "Connection failed" and the state falls back to 0 (about 6 tries in 6.3 s).
`v8_OnAccepted`: from the server address with our linkId in state 1 -> state 2, `AddPeer(serverAddr, linkId, GetRateFor(addr))` 0x436DD0
("ADD client"; the client's single peer), "Start connection", `vt[15] OnConnected` = `LobbyClient::v15_OnConnected` 0x41D410 (only
`Lobby_ResetMpGameState`; nothing is sent: the JoinMessage goes out with the first update). `v3_OnUpdate` 0x4418F0 refuses updates outside
state 2 ("ERROR: Normal update when not in active state"). `v2_RemovePeer` 0x4420E0: when the peer list becomes empty, "CONNECTION LOST
server link is lost, entering idle", state 0.

**Server** (`NetServer`): `v5_OnLogin` 0x443080 reads magic (must equal `+8`, else silently ignored), linkId, rate (only capped by
`GetRateFor(addr)` here; the 500..50000 clamp is `AddPeer`'s). A known peer at that address -> "ERROR: Ignoring login request for active
session" and nothing is sent: the same `address:port` cannot log in again until its old peer times out (60 s). A pending accept
record (`+0x2568` list, 0x28-byte nodes `{addr +8, linkId +0x18, rate +0x1c, age +0x20}`) for that address -> "Reaccepting" (linkId and
rate updated, the age kept, so the record still dies 60 s after the first login; "WARNING: LinkID mismatch" when the linkId differs) and
`{6, linkId}` again. Otherwise `vt[16] CanAccept(addr, cursor, &reason)`
(`LobbyServer::v16_CanAccept` 0x41A050: 2 when the address is in the ban list `+0x25c4`; 3 when a password is set and the next 20 payload
bytes differ; 1 when `peers + pending >= maxPlayers +0x25e0` (8)) -> `{7, linkId, reason}`; accepted -> "Accepting client from", record
pushed, `{6, linkId}`. `NetServer::Receive` 0x443590 ages the records and drops them after 60 s ("Accept period elapsed for").
`NetServer::v3_OnUpdate` 0x4434F0: an update from an unknown address that has a pending record -> `AddPeer(addr, linkId, rate)`, record
erased, the update parsed: **the first type-0 packet completes the handshake**. Rate -> `NetPeer +0x3c = 250 / rate` s.

**Peer removal** (`NetConnection::v2_RemovePeer` 0x436CF0, "REMOVE client"): `NetPeer::ReleaseReceivedObjects` 0x436620 fires the removed
handlers for everything that peer had sent (the server thus runs `LobbyServer::OnLeave` for its JoinMessage), then the node goes. Causes:

| cause | where | side effects |
|---|---|---|
| 60 s without a parsed update (`NetPeer +0x44 > NetConnection +0xc`) or > 32000 bytes pending in the out stream | `NetPeer::Tick` false -> `Receive`: "TIMEOUT client, drop" | `vt[11] OnPeerTimeout` (`LobbyServer` 0x41A700: system chat 4 `lmp_lost_connection`) |
| type 1 with the peer's linkId | `v6_OnDisconnect` | `vt[12] OnPeerDisconnect` (0x41A770: chat 1 `lmp_player_disconnected`) |
| an update that fails to parse (unknown kind / factory) | `v3_OnUpdate`: "ERROR: client ... sent incorrect update" | `vt[11]` then `vt[2]` |
| kick / ban (`LobbyServer::KickPeer` 0x41A550 / `BanPeer` 0x41A620: named events 2..5 from the host) | chat 2 / 3, `SendDisconnect {1, linkId}`, ban list | `vt[2]` |
| the owner is destroyed | `NetConnection::dtor` sends `{1, linkId}` to every peer | |

### 3.4 The update packet [V]

```
u8 0 | u16 seq | u16 ack | u32 ackBits | u16 chunkOffset | u8 chunkLen | chunk[chunkLen] | u8 n | n x message
message = [flag bit] u8 kind u16 id [payload when the flag bit is 1]
```

`SendUpdate(budget = NetPeer +0x50 = 220)` (asm 0x433D58 `push [esi+0x50]`): the `NetCursor` is opened on `g_0x79B900` with budget +
0x400 bytes of room (the 0x400 is only slack); `seq = +8 ++`; `WriteAcks`; `TakeStreamChunk(budget - 16 = 204)`; then the group trees
`+0x10ec` down to `+0x1074` (group 10 first, 0 last) and, for each entry whose object is gone (removal, only while the entry version is 0)
or whose version differs from the entry's, one message. The budget test (`cursor - (start + 1) > budget`, i.e. counted from the seq field)
runs **after** a message is written: the message that crosses it stays in the buffer but is neither counted in `n` nor recorded, the group
loop ends, and each later group writes (and drops) one more message the same way; the receiver reads only `n` messages, so these are
harmless trailing bytes (their ids are already allocated, their entries are re-sent next time). A datagram is therefore ~220-230 bytes
plus at most one message per remaining group (um-bot measured 228 in the lobby). A `SendTo` failure runs `PacketLost` on the new record at
once (deep read 2026-11-06).
`NetSocket::SendTo` sends the buffer's content length (`validEnd`), which can include stale bytes of an earlier, longer packet. Every message
is recorded in the packet's `SentPacket` (`+0x10f8` list, 0x120-byte node: `+8` time, `+0x10` seq, `+0x14` list of `{channel, version,
entry}`, `+0x18..+0x11b` the copy of the whole 0x104-byte stream-chunk node tail `{u16 offset, u8 len, data[257]}` (`rep movsd` of 0x41 dwords at 0x435B2B), `+0x11c` lost flag; `PacketLost` gets the record at node + 8, so its offsets read 8 lower) [V].

An empty keep-alive is `00 seq ack bits 0000 00 00` (offset = the current stream position); it is enough to keep a peer alive.

### 3.5 Sequences and acks [V]

- `ParseUpdate` 0x436470: `+0x44 = 0` (silence timer); unwrap the seq; `AcceptSeq` 0x434E50: `seq + 32 < lastRecv` -> "SKIP too old
  update, dif=" (false, packet dropped); `lastRecv = max`; seq already in the window vector `+0x14` -> "DUPLICATE update received from"
  (dropped); else pushed. `WriteAcks` prunes window entries older than `last - 32`.
- `WriteAcks` 0x434C20: `ack = lastRecv (u16)`, `bit i-1 of ackBits` = `lastRecv - i` is in the window (i = 1..32).
- `ReadAcks` 0x434D70: shifts the seen-mask `+0x1124` by the signed 16-bit distance from `+0x1128` (an x86 `shr` by >= 32 leaves the
  mask unchanged: kept as a quirk), `fresh = bits & ~mask`, `AckOne(ack)` and `AckOne(ack - i)` for every fresh bit -> `OnPacketAcked`
  0x433FA0: rtt = clock - record time; `rttMean +0x24` and `rttMeanSq +0x28` updated with factor 0.93; channel 1 entries get
  `version = max(entry, sent)`; channel 2 / 3 (removals) are erased from their group tree and the key tree and their `{id, kind}` pushed to
  the recycle list `+0x1054`; the record leaves `+0x10f8` / `+0x10fc`.
- A client must ack from the server's own first sequence (the server starts anywhere: `GetTickCount & 0xffff`); acking from 0 makes
  everything look lost (um-bot bug of 2026-10-04).

### 3.6 Reliability [V]

- `SyncObjectSet` 0x4346D0: `vt[13] GatherReplicated` of `g_0x79B920` fills `g_0x79B8D0` with `{object, group}` for this peer, sorted by
  pointer and merged with the object tree `+0x1068`: new -> `RegisterObject` 0x434110 (a `NetPendingEntry` 0x14: `+8` sentVersion 0,
  `+0xc` id 0, `+0xe` kind from the vtable registry `+0x2c`, `+0xf` bit 0 = the kind's reliability byte `NetConnection +0x64 + kind`,
  bits 1.. = group; inserted into `+0x1068` and the group tree `+0x1074 + group * 0xc`); gone -> the entry's object is released and its
  version set to 0 so a removal goes out (an entry that never got an id is simply dropped).
- Reliability byte per kind (`+0x64`, 256 entries): `RegisterMessageFactory` clears the bytes of kinds 1..17 -> **mode 0 for every lobby
  kind**: the entry version is set to the object version when written and reset to 0 only by `PacketLost`, so an object is re-sent only
  after its packet is declared lost. Mode 1 (unused) would keep the entry version until the ack, re-sending every packet.
- `NetPeer::Tick`: `timeout = clamp(rttMean + 3 * sqrt(|rttMean^2 - rttMeanSq|), 0.1, NetConnection +0x10 = 5)` s; sent records older
  than it -> `PacketLost` 0x433F20 (once: `+0x114`; channel 0 entries whose version still equals the sent version get version 0, channel 2
  entries too; the chunk copy is re-queued at the front of `+0x111c`) and the record moves to `+0x10fc`, where it is discarded after 5 s.
  `rttMean` starts at 5.0 s, so the first loss is detected late; it converges with the 0.93 EWMA.
- Removals (flag 0): when `SendUpdate` finds the entry's object gone (null or refcount bit 31) it drops the object from the object tree
  `+0x1068` and sets the entry version to 0; a removal is written only while the entry version is 0 and the version then becomes 1
  (record channel 2, mode 0; channel 3 in mode 1), so it goes out **once** and again only after `PacketLost` resets channel-2 entries
  to 0; the entry goes on ack [V] (corrected 2026-11-06: it was noted as "written in every update"). The ids return to the per-kind free
  stack every 75 s (`NetConnection +0x14`, `NetPeer_DistributeIds` 0x432D20).
- Receiver (`ReadMessage` 0x435F30, key `kind << 16 | id` in `+0x1100`, node `{ObjRef +0x14, version +0x18}`): flag 1 + unknown key ->
  insert, `NetObjectFactory` 0x438930 (creator hash `+0x40`), `SetObjectId` 0x4352F0 (pair tree `+0x110c` for refs), `vt[5] Read`, the
  **created** handlers (`+0x164 + kind * 0xc`); known + `version < seq` -> an object that was released (a removed key / tombstone) is
  re-created through the factory with version 0 and gets `Read` + the **created** handlers; a live one gets `Read` + the **updated**
  handlers (`+0xd64`); otherwise "SKIP update, newer version of object is already here" and a throwaway object consumes the payload.
  Flag 0 + unknown -> tombstone `{null, seq}` (an older update cannot resurrect it); known + `version < seq` -> the **removed** handlers
  (`+0x1964`, only when the object is still there) and release, the key stays as a tombstone with the new version; else "SKIP remove".
  Only a failing factory returns false (-> peer drop).
- Handlers are `void(ecx = connection, edx = object)` registered by `NetConnection::AddHandler` 0x4377D0 (table 0 / 1 / 2) through the
  per-class helpers 0x4279B0-0x4288E0; `NetClassRegistry::Register` 0x42A890 maps vtable -> kind and kind -> creator.

### 3.7 The byte stream (chunks) [V]

Per direction: `outStream` GameBuffer `+0x112c` (bytes queued by `PeerMsgStream::v13_Write` 0x4796B0), `outStreamOffset +0x1118`,
`outChunks +0x111c` (0x10c-byte nodes `{+8 u16 offset, +0xa u8 len, +0xb data}`), `inChunks +0x1120`, `inStreamExpected +0x111a`,
`inStream +0x1148` (what `PeerMsgStream::v10_PullFromPeer` 0x4797A0 drains).

- `TakeStreamChunk(maxLen)` 0x434990: a re-queued chunk in `+0x111c` (oldest by offset) is sent first, unchanged; else `min(maxLen, 255,
  pending)` bytes are cut off the out stream into a new node, `offset = +0x1118`, `+0x1118 += len` (u16, wraps). Every update carries one
  chunk (possibly empty); its copy lives in the sent record until acked, and `PacketLost` re-queues it (never re-cut: offsets are stable).
- `ParseUpdate` queues a received chunk when `len > 0` and `(u16)(offset - expected) <= 0x8000`; `DrainChunks` 0x434AE0 appends every
  queued chunk whose offset equals `expected` to the in stream, bumps `expected`, repeats. Out-of-order chunks wait; duplicates of an
  already-drained offset are skipped by the window test.
- Limits: 204 bytes per update (3.4); `NetPeer::Tick` drops the peer when more than 32000 bytes wait in the out stream (a client that stops
  acking during a big snapshot is cut, not throttled); nothing bounds `inChunks`. Throughput = chunk size / send interval: ~10 KB/s LAN, ~2 KB/s
  at 2500 (the 5-6 KB zone snapshot takes 0.6 / 3 s).

### 3.8 Object replication details [V]

- `NetWriter::WriteRef` 0x4355B0 (writer = the NetPeer): null / dead -> `bit 0, kind 0, id 0`; an object this side replicates (`+0x1068`)
  -> allocates its id if still 0, `bit 0, kind, id`; an object received from this peer (`+0x110c`) -> `bit 1, kind, id`; anything else -> null.
- `ReadObjectRef` 0x435820: bit 1 -> the object WE sent with that key (`+0x105c`); bit 0 -> the received object of that key in `+0x1100`,
  created through the factory when unknown (an unknown ref makes a blank object: the "Player  is offering you a swap" symptom).
- The 11 groups: lobby objects use group 2 (server) / 1 (client, and the server's file chunks); the group only orders the walk in
  `SendUpdate` (higher group first) and lets `GatherReplicated` keep a chunk list separate. [V]
- `NetPeer::Clone` 0x433060 copy-constructs the whole 0x1168 (never called by the lobby [I]).

### 3.9 Timers, rates and limits (summary)

| what | value | where |
|---|---|---|
| server port / master port | 8888 / 28004 | `g_0x783E30`, `NetAddress::FromString` default |
| rate LAN / other; send interval | 12500 / 2500 B/s -> 20 / 100 ms; clamp 500..50000 | `GetRateFor` 0x436F80, `AddPeer` 0x436DD0 |
| login retries | 0.1 s doubling, give up past 3 s | `NetClient::Receive` 0x442AD0 |
| pending accept lifetime | 60 s | `NetServer::Receive` 0x443590 |
| peer silence timeout | 60 s (`NetConnection +0xc`) | `NetPeer::Tick` |
| resend timeout | `rttMean + 3 sigma`, clamped 0.1..5 s; lost records kept 5 s | `NetPeer::Tick`, `+0x10` |
| id recycle period | 75 s (`+0x14`) | `NetPeer::Tick` |
| receive window / ack bits | 32 behind the last; 32 ack bits | `AcceptSeq`, `WriteAcks` |
| packet budget / chunk / receive size | 220 / 204 (hard cap 255) / 2048 | `NetPeer +0x50`, `TakeStreamChunk`, `ReceiveFrom` |
| out stream cap | 32000 bytes -> drop | `NetPeer::Tick` |
| server-info ageing | 20 s LAN entries; internet entries never | `NetClient::Receive` |
| default NetPeer send interval | 0.04 s before the rate is known | `NetPeer::ctor` |

### 3.10 The NetPeer record

0x1168 bytes (`ctor` 0x432DA0), one per remote endpoint, owned by the `NetConnection` peer list `+0x28`; the full field list is in
`batch-net-structs.tsv` (NetPeer, NetPendingEntry, SentPacket, StreamChunk, IdAllocator). Everything per link lives here: sequences and the
window (`+8`..`+0x1c`), linkId `+0x20`, RTT `+0x24/+0x28`, address `+0x2c`, send interval / accumulator / silence / clock
(`+0x3c`..`+0x48`), the packet budget `+0x50`, 256 id allocators `+0x54`, the recycle list `+0x1054`, the entry trees `+0x105c` (key) /
`+0x1068` (object) / `+0x1074` (11 groups), sent / lost packet lists `+0x10f8` / `+0x10fc`, received objects `+0x1100`, their ids
`+0x110c`, the stream state `+0x1118`..`+0x1128` and the two stream buffers `+0x112c` / `+0x1148`. [V]

## 4. Lobby

### 4.1 Objects [V]

`LobbyServer` (0x2600, `ctor` 0x412D80, `g_0x79B5DC`, created by `Lobby_StartServerOrClient` 0x57E740 on the host with `SetPublic` /
`SetPassword`) and `LobbyClient` (0x2700, `ctor` 0x41BC40, `g_0x79B5D8`, created by `Lobby_Create` 0x57F390; the host runs one too,
connecting to 127.0.0.1 as "Server"). Both ctors run `RegisterMessageFactory` and `AddHandler` for their side (the rows of both ctors in
`batch-net.tsv` list every handler). Their lists (section 4.2's "server list" / "client list" columns) are in `batch-net-structs.tsv`.
`LobbyServer::v13_GatherReplicated` 0x417EB0 (deep read 2026-11-06) builds, for the peer `g_0x79B924`, in this order (group 2 unless
noted): every player record `+0x2590`; every variable `+0x25b0`; the peer's own peer record of `+0x25a4` (its player record decides the
rest); the chat lines `+0x2594` whose id `+0xc` is above that player's watermark `+0x38` and whose target `+0x1c` is 0 or this peer (only
while the player record is alive); every file transfer `+0x2598` and, in group 1, all chunks `+0x20..+0x24` of a transfer this peer
requested (`+0x259c` `{transfer +0xc, peer +0x10}`); the map list `+0x25ac` only when the player is the host (`+0x28`); the quest host
line `+0x25a0` and the current map `+0x25a8`; the player events `+0x25b4` addressed to the peer (`+0xc`); the exchange records `+0x25b8`
the player owns (`+0x2c`) **with their item blobs, but only when every blob is alive** (one dead blob holds the whole offer back); the
exchange pairs `+0x25bc` and the expired pairs `+0x25c0` that involve the player. [V]
`LobbyServer::v14_Tick` 0x4182E0 also ages the player-event list `+0x25b4` and the expired-pair list `+0x25c0` (60 s) and sends the master
heartbeat; `LobbyClient::v14_PerFrame` 0x41CCD0 ages the timed outgoing list `+0x2604` and applies queued player events once the local
player is back in the lobby.

### 4.2 Message catalogue (kinds 1..17, `RegisterMessageFactory` 0x4142F0) [V]

Payload = the `vt[4] Write` order (what follows `u8 kind u16 id` when the flag bit is 1). "S" = server, "C" = client. Handlers: created /
updated / removed on the receiving side.

| kind | class (size, vtable) | wire payload | sent by -> list | receiver handlers (created / updated / removed) |
|---|---|---|---|---|
| 1 | `TwoStringMessage` 0x3c 0x73BC70 player record | `name\0, unit\0, u32 state, u8 isHost, u32 heroId, u32 playerId` | S `+0x2590` -> every peer | C `OnPlayerRecordAdded` 0x41E490 / UI refresh 0x41E510 / `OnPlayerRecordRemoved` 0x41E520 |
| 2 | `IdStringMessage` 0x20 0x73BC88 chat line | `u32 lineId, text` | S `+0x2594` -> everyone or the addressed peer (`+0x1c`), only lines newer than the player's watermark | C `OnChatLine` 0x41E5D0 (`CInterface::AddChatLine`) / - / `OnChatLineRemoved` 0x41E690 |
| 3 | `StringPairMessage` 0x24 0x73BCA0 quest host line | `"ip:8000", zone` | S `+0x25a0` while a quest runs | C 0x41E730 (`+0x25ec`; `RequestMpMapChange` takes the zone from it) / - / 0x41E760 |
| 4 | `TwoRefMessage2` 0x14 0x73BCB8 peer record | `ref playerRecord` | S `+0x25a4`, each peer gets only its own | C `OnOwnPeerRecord` 0x41E430 (`+0x25f0`) / - / 0x41E460 |
| 5 | `JoinMessage` 0x34 0x73BBF8 | `name\0, unit\0, u32 state (4 created / 1 lobby / 3 loading / 0 in quest), u8 hostFlag, u32 heroId (-1 none)` | C `+0x2600`, always; version ++ on every field change | S `OnJoin` 0x41ADB0 / `OnJoinUpdate` 0x41AD80 / `OnLeave` 0x41B0C0 |
| 6 | `ChatMessage` 0x18 0x73BC28 | `text` | C via `QueueTimedObject` 0x41D1A0 (60 s) | S `OnChat` 0x41B1C0 -> `PostChat` "^%i%s^7: %s" |
| 7 | `NamedEventMessage` 0x1c 0x73BC40 | `u32 kind, name` (0 set map name, 1 quest event `<quest>_1/_2/_3`, 2 / 3 kick / ban by name, 4 / 5 by id) | C `SendNamedEvent` 0x41D260 (timed, 60 s) | S `OnNamedEvent` 0x41B2A0 (host only; 3..5 need `IsPeerHost`) |
| 8 | `StringMessage` 0x18 0x73BCD0 quest state | `"<quest>_1"` (available) / `_2` (zone open) / `_3` (completed) | S `+0x25ac` -> the host player only | C `OnQuestStateAdded` 0x41E790 (`Briefing_SetNamedFloat`) / - / `OnQuestStateRemoved` 0x41E850 |
| 9 | `StringMessage2` 0x18 0x73BD00 current map | `"<quest>"` | S `+0x25a8` -> everyone | C 0x41E900 (`+0x25f4`; without a local server `Lobby_QueueMapFileTransfer(name)` + `QuestFiles_Scan`) / - / 0x41E960 |
| 10 | `StringDwordsMessage` 0x20 0x73BCE8 variable | `name, f32 value, u32 serial` | S `+0x25b0` -> everyone (`SetVariable` 0x419730, `MaxVariable` 0x4197D0, value 0 removes) | C `OnVariableAdded` 0x41E990 (`Campaign_AdvanceMission`) / 0x41EA10 / `OnVariableRemoved` 0x41EA20 |
| 11 | `ListStringMessage` 0x2c 0x73BD18 file transfer | `u16 chunkCount, u32 size, name` | S `+0x2598` -> everyone (`AddFileTransfer` 0x419A00) | C `OnFileTransferAdded` 0x41EAC0 / - / `OnFileTransferRemoved` 0x41EBF0 |
| 12 | `RefBlobMessage` 0x54 0x73BD30 file chunk | `ref transfer, u16 index, byte[64]` | S (group 1) -> only the peers that requested that transfer | C `OnChunkReceived` 0x41ECC0 / - / `OnChunkRemoved` 0x41EF10 |
| 13 | `TwoRefMessage` 0x14 0x73BC58 chunk request | `ref transfer` | C `+0x25e8` | S `OnChunkRequest` 0x41B690 / - / `OnChunkRequestRemoved` 0x41B720 |
| 14 | `RefDwordsStringMessage` 0x24 0x73BD48 player event | `ref player, u32 kind (0 experience, 1 money, 2 give item, 3 take item, 4 clear kind-0x14 items), u32 value, text` | S `+0x25b4` (60 s) -> the addressed player | C `OnPlayerEventReceived` 0x41EF90 -> `ApplyPlayerEvents` 0x41CD80 |
| 15 | `ObjListMessage` 0x34 0x73BDD0 swap offer | `u32 n, n x ref item blob, u32 money, u8 confirmed, u32 partnerNonce, u32 nonce, ref owner, ref partner` | C `+0x260c` (`CreateExchangeRecord` 0x41E270, `FillParty` 0x41DA50) | S `OnPairRecordAdded` 0x41B7A0 / `ConfirmPlayerPair` 0x41B970 / `OnPairRecordRemoved` 0x41BA30; C (the partner's) `OnExchangeRecordAdded` 0x41EFF0 / 0x41F090 / `OnExchangeRecordRemoved` 0x41F0D0 |
| 16 | `BlobMessage` 0x34 0x73BD78 offered item | `u16 size, bytes` (the item's own serializer, 72 bytes for a .mp item) | C, one per item on the counter | S no-op 0x41BA20; C 0x41F150 / 0x41F180 / 0x41F1B0 (`LoadPartnerParty`) |
| 17 | `ObjPairMessage` 0x28 0x73BD60 swap pair | `ref recordA, ref recordB, u8 confirmed, u32 nonceA, u32 nonceB` | S `+0x25bc` -> the two players | C `OnExchangePairAdded` 0x41F1E0 / 0x41F270 (confirmed -> `CInterface::OnLobbyRef` opens the exchange screen) / 0x41F3A0 |

The `Reset` / `Clone` slots and the ctors of every class are at 0x413350-0x4177A0 (batch rows); `ServerInfo` 0x48 0x73BB28 (parent
`NetPeerInfo` 0x73BB40) is the type-5 payload, not a replicated object.

### 4.3 Join, leave, host, kick / ban [V]

- `OnJoin`: a new player record `{name, unit, state, heroId, playerId = g_0x79B60C++, slotIndex = first free from 1, chatWatermark =
  +0x258c}` in `+0x2590`, a peer record `{player, NetPeer g_0x79B924}` in `+0x25a4` and in the join's `+0x30`; `isHost = 1` when the join's
  hostFlag is set, or when no local LobbyClient exists and this is the only peer (a dedicated server makes its first player the host);
  system chat 0 (`lmp_player_connected`). `OnJoinUpdate` copies state and heroId into the record and bumps its version (everybody sees the
  state change). `OnLeave`: the record goes; if it was the host, the first remaining player becomes host.
- `OnNamedEvent` kinds 2..5 -> `FindPeerByPlayerName` 0x41A170 / `FindPeerByPlayerId` 0x41A270 -> `KickPeer` / `BanPeer` (never the host).
- System lines: `PostSystemChat(kind, player, flag)` 0x41A490 formats `Lobby_FormatSystemLine` 0x5F8C90 (0 connected, 1 disconnected, 2 kicked,
  3 banned, 4 lost connection, 5 quest taken, 6 canceled, 7 completed, 8 zone enabled, 9 zone disabled, 10 trade requested, 11 trade
  canceled) and posts it (`PostChat` 0x41A310, to everyone or to one peer); `PostSystemChatToNonHosts` 0x41A500 = every player but the host.

### 4.4 Chat [V]

A typed line is a `ChatMessage` queued for 60 s (`QueueTimedObject`; `CUIMessageLog::EditOnKeyDown`); the server answers with an
`IdStringMessage` `"^<slot>Name^7: text"` for everyone (`OnChat`). Lines are numbered by `+0x258c`; a player receives only lines with an id
above its join watermark (no history on join); the server keeps the last 50 (`PostChat`), older ones are withdrawn (removal messages ->
`OnChatLineRemoved`). um-engine's ghost / speed features ride on this channel (`mp_ghost.cpp`).

### 4.5 Quests, states, variables, player events [V]

- `RefreshMapList` 0x418530 fills `+0x25ac` with one `StringMessage` `"<quest>_1"` per `maps\*.mq` (`Campaign_ShuffleNames` 0x57F850 by
  game kind) and queues each `.mq` as a file transfer (4.6). The host's client sees the list (kind 8, host only) and picks a quest:
  `Lobby_SendNamedEvent` 0x57FB60 -> kind 7 `{1, "<quest>_1"}` -> `OnNamedEvent` -> `SetMap` 0x4186F0 (deep read 2026-11-06): the map list
  `+0x25ac` **and every variable `+0x25b0`** are cleared, the list := `"<quest>_2"`, current map `+0x25a8` := a new `StringMessage2 "<quest>"`
  (kind 9 to everyone; the name also in `+0x25d4`), `QuestFiles_Scan`, `Session_SetMapName` 0x57ED60 (the host loads the zone:
  `world-server.md` 2.6), chat 5 with the localized quest name (`Game_GetQuestName`); then, when the briefing record
  `g_0x7C87D4["<quest>_1"]` exists: one variable = 1.0 per name of its `+0x2c` vector (written directly: value, serial `g_0x79B608++`,
  version + 1), `SetVariable("q.<quest>.<quest>", 1)`, `SetVariable("q.<quest>.<quest>.1", 1)` and `PostMapEvents(record, 0)`; `Session_HostMpZone` then creates the kind-3 line `{host:8000, zone}` and posts chat 8 to the
  non-hosts. `"<quest>_2"` -> `UnloadMap` 0x418CC0 (chat 6, `PostMapEvents` with start, map released, lists cleared, `RefreshMapList`);
  `"<quest>_3"` -> `OnMapRecord` 0x418ED0 (chat 7, the record `g_0x7C87D4["<quest>_3"]`: experience (kind 0) and money (kind 1) player
  events to every peer, map events). `Lobby_QuestMapFinished` 0x57FB90 (script `QuestComplete`) -> `OnQuestMapFinished` 0x4193F0 appends
  `"<quest>_3"`.
- Variables (kind 10): `SetVariable` / `MaxVariable` / `RemoveVariable` 0x4198D0 from the quest scripts via `Lobby_*`; serial `g_0x79B608++`.
- Player events (kind 14, `PostPlayerEvent` 0x419D90, 60 s): applied on the client by `ApplyPlayerEvents` 0x41CD80 to the local
  `CPlayer` (0 `GiveExperience(0, text)`, 1 money += value, 2 / 3 give / take the item named, 4 clear party items of kind 0x14) only when
  the game state is 3 and the local player is in state 1 (back in the lobby), then the list is emptied.
- The client's kind-9 handler also loads the quest's text archive (`Lobby_QueueMapFileTransfer` -> `CTextManager::AddArchive`) when the
  `.mq` is present locally.

### 4.6 File transfer (how a client gets a quest file) [V]

Only `.mq` files travel. `AddFileTransfer(name, GameBuffer)` 0x419A00 makes a `ListStringMessage {chunkCount = (size + 63) / 64, size,
name}` plus one `RefBlobMessage {transfer, index, 64 bytes}` per chunk (the last padded) in its vector `+0x20`. Every client receives the
kind-11 object; `OnFileTransferAdded` creates a `TwoRefMessage {transfer}` (kind 13) in `+0x25e8` unless `Game_MapFileExists` 0x5812F0 says
the file is already there: it runs `CFileFind::FindFile(World_MapPath() + name)` with the transfer's file name (`+0x14`), so it
tests the specific `maps\<name>` file, not the folder [V]. The server's `OnChunkRequest` records `{transfer, peer}` in
`+0x259c`; `GatherReplicated` then includes that transfer's chunks (group 1) for that peer only, so all chunks go out through the normal
reliable path (one 64-byte chunk is 67 bytes + 3 of header: two or three per update, ~150 chunks/s on LAN). `OnChunkReceived` keeps the
chunks in `+0x25e4`; when `chunkCount` chunks of a transfer are present it sorts them by index, concatenates, truncates to `size`,
`Game_SaveReceivedMapFile(name, buffer)` 0x5813A0 writes `maps\<name>` and the request is released (its removal -> `OnChunkRequestRemoved`).
There is no hash, no resume and no per-file progress message.

### 4.7 The exchange (item swap between two players) [V]

Clicking a player's head: `CreateExchangeRecord(partner)` -> kind 15 `{items of the offer party +0x2620 as kind-16 blobs, money, confirmed
0, partnerNonce 0, nonce 1, owner = own record, partner}`; `FillParty` 0x41DA50 re-uploads it (nonce ++) whenever the offer changes
(`CExchangeScreen::Update` 0x62C930). Server: `OnPairRecordAdded` stores it; when the partner's mirrored record exists (owner / partner
swapped) it creates the `ObjPairMessage` (kind 17) for both players, else chat 10 (`lmp_trade_requested`) to the partner. Accepting
(`CExchangeScreen::OnLButtonDown` 0x62CF70 -> `ConfirmPair` 0x41E0A0): `confirmed = 1`, `partnerNonce = partner.nonce`, version ++;
`ConfirmPlayerPair` marks the pair confirmed once both records are confirmed and each `partnerNonce` equals the other's `nonce` ->
the clients open the exchange screen and move the items themselves (`Party_MoveAllItems`; the server never touches items). Retract =
`RetractExchange` 0x41E180, leave = `ReleaseExchangeRecord` 0x41E3E0 (chat 11 unless paired); a confirmed pair whose records vanished stays
in `+0x25c0` for 60 s. um-bot implements the whole dance (`AnswerSwap`).

### 4.8 Master server and the browser [V]

- Heartbeat: `LobbyServer::SetPublic(1)` 0x417E10 resolves `g_0x79BED8` (the master host name; **empty in this build**, so nothing is ever
  sent) with default port 28004; `v14_Tick` sends `SendMasterHeartbeat` 0x41AA10 every 20 s: a raw `NetPlayerInfo::Write` 0x401000 record
  `{name, map, u8 players, u8 max, u8 kind, u8 hasPassword, u32 requestId, u32 publicAddress, u32 magic}` with **no type byte** through the
  UDP socket. The master answers `{0xFF, u32 requestId, u32 publicAddress}` -> `v10_OnMasterPacket` stores the public address.
- List download: `CLobbyScreen::StartConnect` 0x61D0D0 -> `CConnectProgressWin` 0x61E2F0 opens a `TcpStream` to the master (`NetAddress::FromString(g_0x79BED8, 28004)`, not a typed address) whose whole request is one
  `u32`: `NetClient +0x25b8`, the dword stored with the address book (`SetServerList`; 0 without a saved book) [V];
  `Poll` 0x440DE0 sends the request buffer, reads until the remote closes; `LobbyClient::OnServerInfoPacket` 0x41D600 parses
  `{u32 rawLength, LZ(records)}` where each record is `sockaddr[16] + NetPlayerInfo::Read 0x401170` -> `ServerInfo` (source 2) ->
  `NetClient::AddServerInfo` 0x4428A0 and one get-info probe. The address book (`NetServerRecord` vector, `AddrBook_Serialize` 0x4414E0)
  seeds the list with source 1 entries (`SetServerList` 0x442300).

### 4.9 Handover from the lobby to the game [V]

- **Client**: `CWorldClient::ConnectToLobbyHost` 0x566D80 (from `Session_ProcessRequest` kinds 5 / 6, `Session_LeaveGameToMenu` 0x579420):
  with a live LobbyClient whose peer list is not empty, `CWorld::ResetNetDriver` 0x55F410 makes a fresh `RefListPair` at `+0x60`,
  `CTcpNetDriver::ConnectBlocking(+0x60, first peer)` 0x47A080 creates one `PeerMsgStream` 0x73C990 over the server NetPeer, pulls and
  **discards** every complete frame already queued, and the stream becomes `CPlayer +0x1c` (`CPlayer::SetNetStream`). From then on
  `CPlayerClient::SendClientState` / `FlushSendStream` write frames into the peer's out stream and `Update` pops them from the in stream
  (`world-client-players.md` 2). The lobby objects keep flowing in the same packets (the JoinMessage state changes 3 -> 0 are how the server
  learns the client is in the world).
- **Server**: `CWorldServer::Tick` 0x58DFF0 calls `CTcpNetDriver::SyncPeers(+0x60, g_0x79B5DC)` every tick: a NetPeer whose in stream holds
  bytes and whose player record is in state 3 (`LobbyServer::IsPeerInGame` 0x417E40) gets a `PeerMsgStream` in both lists of the pair;
  `AcceptPlayers` 0x58DAF0 takes the new ones (`vt[6] MoveRefsToList`) and makes `CPlayer` + `CPlayerServer` for each
  (`CPlayerManager::CreatePlayerWithConnection`, `+0x130 = -1`, start position, "Accepted player N"). A stream whose peer died is dropped by
  `SyncPeers`; `PeerMsgStream::dtor` pushes unread bytes back into the peer (`v11_FlushToPeer`).
- Leaving: the client's `0x04` opcode (stream) and `JoinMessage` state 1; the NetPeer survives, so a second quest reuses the link.
- The `CTcpNetDriver` name, `ConnectAsync` 0x47C8E0 (default port 8000), `AcceptNewClient` 0x47CFA0, `TcpMsgStream` 0x73CA10 and
  `TcpSocketRef` are an abandoned TCP design: none has a caller; the `":8000"` in the kind-3 line is its only trace. The object the two
  live functions take as `this` is the `RefListPair` 0x73F3A8 `{vt, refcount, +8 all streams, +0xc new streams}`.

## 5. Primitives

### 5.1 GameBuffer and NetCursor [V]

`GameBuffer` (0x1c, vtable 0x73D624: 1 GrowForWrite, 4 WriteGrow(data, n), 6 Seek(pos); `ctor` 0x4DA7F0 32 bytes): `+4 data, +8 cursor,
+0xc capacityEnd, +0x10 validEnd, +0x14 bias, +0x18 flags (bit 0 dirty, bit 3 read error)`; `Write` 0x436C60 inlines the fast path and
calls `vt[4]` (0x4DA740: realloc to `(len + n) * 2 + 0x40`) when full; `CommitCursor` 0x419CC0 (`validEnd = max(validEnd, cursor)`) after
raw writes; `Seek` 0x4DA860 grows on demand. `NetCursor` (0x18, `ctor` 0x4DA490: `{ptr, flagBytePtr, bitAccum, bitCount, buffer, start}`)
is the raw pointer cursor every message `Write` / `Read` slot takes ("cursor*"): `WriteBytes` 0x430930 / `ReadBytes` 0x430F60 have no bounds
check; a flag bit reserves a byte at the current position the first time it is needed and fills it LSB-first.

### 5.2 Length prefixes (three variants, all "even / odd") [V]

| writer / reader | encoding | used by |
|---|---|---|
| `GameBuffer::WriteLength` 0x4DA380 / `ReadLength` 0x4DA200 | `u8 n*2` when n < 128 else `u32 n*2+1` | `NetPlayerInfo::Write` (master records) |
| `MsgStream_WriteLenPrefixed` 0x47DA80 / `ReadLenPrefixed` 0x47DAE0 | same; reader refuses > 100000 | in-game opcodes with embedded streams (0x66E8D1, 0x66EC01, 0x66906F..) |
| `Stream_WriteEmbeddedStream` 0x4A5250 / `ReadEmbeddedStream` | `u16 n*2` when n < 0x7531 else `u32 n*2+1` | party blobs 0x80 / 0x83, recorded frames (`DualFileMsgStream`) |
| `DeltaWriter_WriteStreamLenPrefixed` 0x47E190 / `DeltaReader_ReadLenPrefixed` 0x47E240 | u8 / u32 as the first row, raw into both delta streams | `CEffectShowVisibility::WriteNet` / `ReadNet` |

### 5.3 ChunkFile (.mob and the save's object descriptors) [V]

Not a net format: a nested tagged-chunk reader / writer over an MFC `CFile` used by `MobFile_ParseChunks` 0x481F00, the `CDesc` classes
(`CObjectDesc::OnChunk` 0x46AB20, `CUnitDesc`, `MobLogicSlot_OnChunk`...) and `CWorldServer::LoadMapObjects` 0x596030. Object 0x1c:
`{vt 0x73BE94 (0 OnChunk(this), 1 GetFile), +4 tag, +8 consumed, +0xc length, +0x10 lengthFieldPos, +0x14 parent, +0x18 CFile (root)}`.
Wire: `u32 tag, u32 length` where **length includes the 8-byte header**, payload follows; children are nested chunks. `BeginChunk(tag)`
0x42BF50 writes the tag and a placeholder, `EndChunk` 0x42BFB0 seeks back and writes `consumed`; `Consume(n)` 0x42BF00 adds to this chunk and
every parent; `NextChunk` 0x42C0B0 skips the unread rest and reads the next header; `ForEachChunk` 0x42C140 iterates children calling
`vt[0]`; helpers `ReadDword` 0x42C280 / 0x42C340, `ReadByte` 0x42C4C0, `ReadString` 0x42C580 (rest of the chunk), `ReadStringArray`
0x42CA20 (`u32 count` then one string chunk each), `ReadVec3` 0x42CCC0, `ReadVec4` 0x42CEC0 / `ReadVec4B` 0x42D060, and the
`Write*Field(tag, ...)` family (0x42C2C0..0x42D0E0) writing a header plus the payload. No varint anywhere (the even / odd rule is 5.2).
`ChunkFile::ctor` 0x42D1C0 has no caller (children are built with `ctor_base` 0x42BEC0 and the root is opened by `Open` 0x42D160).

### 5.4 The LZ format (`LzCompress` 0x430450 / `LzDecompress` 0x430960) [V]

The same coder as the `.mp` files (`docs/file-formats/mp-format.md`), kept as one `LzCompressor` (0x2828 bytes) **per direction** whose
1 KB ring window persists across messages (`CPlayerClient +0x88` send / `+0x28b0` receive, `CPlayerServer` the same; `LzCompressor::Reset`
0x42FCA0 only at construction / `EndAndReset` 0x6671A0). Output = `u32 rawLength` then a bit-interleaved token stream: bits are taken
LSB-first from a byte pulled from the stream when the accumulator is empty; literals and prefix codes are inline.

- token: bit 0 -> one literal byte; bit 1 -> match `{length code, distance code}`, copy from `window[(pos - d - 1) & 0x3ff]`.
- length codes (decoder tree, 0x430960): `00` 2, `01` 4, `1000` 3, `1001`+3 bits 11..18, `101`+2 bits 7..10, `1100` 5, `1101`+6 bits
  35..98, `1110` 6, `11110`+4 bits 19..34, `11111`+7 bits 99..226.
- distance codes: `0000`+3 bits 10..17, `0001`+2 bits 2..5, `0010`+2 bits 6..9, `00110`+4 bits 18..33, `00111`+1 bit 0..1, `010`+6 bits
  34..97, `011`+8 bits 98..353, `11`+10 bits 354..1377, `10`+12 bits 1378..5473.
- the encoder (`EmitMatch` 0x42FD10, tables from `LzCodec_InitTables` 0x42FAE0) emits lengths 2..225 and distances < 798 only (hash
  `(lastByte << 2) ^ byte`, 1024 heads `+0x1800`, chain `+0x800`); a literal-only stream (flag bits 0) is valid, which is what um-bot sends.
- encoder search (`LzCompress(compressor, input view, output buffer)`; deep read 2026-11-06) [V]: every input byte is stored in the window
  (`+0` and the copy `+0x400`), the hash head updated and `chain[pos]` = distance to the previous position with the same hash (0 -> 0x400 =
  none). Without a current match, the chain is walked while the distance is < 798 (0x31E) for a position whose two bytes equal the last two
  input bytes: the match starts with length 2; else the previous byte goes out as a literal (literals are emitted one byte late, `+0x2824`).
  With a match, the next byte extends it while the length is < 225 (0xE1); on a mismatch the chain is walked again for a candidate that
  matches `length + 1` bytes; failing that, `EmitMatch`. The length tables are indexed by the length and include the match flag bit (entry 1
  = the literal flag); `d = (pos - len - matchPos - 1) & 0x3ff`, so the 12-bit distance form is never produced. The decoder trusts the raw
  length (the output is reserved at once) and has no input bound: a truncated or hostile frame reads past its buffer (a remote-crash
  surface of `MsgStream_Decompress`, `LobbyClient::OnServerInfoPacket` and `MpFile::ReadContainer`); distances >= 1024 alias modulo the window.

### 5.5 Frames and the MsgStream family [V]

A **frame** on the byte stream is `u32 n` + `n` bytes = `MsgStream_Compress` 0x47D7F0 output `{u32 rawLength, LZ tokens}`; `n` is written by
`MsgStreamBase::v8_SendBuffered` 0x47BAD0 (refuses > 0x7ffc bytes), read by `v9_PopPacket` 0x47BB70, tested by `v6_HasPacketHeader`
0x47B9F0 / `v7_CountPackets` 0x47BA40. The empty keep-alive is `04 00 00 00 00 00 00 00`. `MsgStream_Decompress` 0x47D970 inflates a popped
frame with the receive `LzCompressor` into the player's receive `CMemStream`.

`IMsgStream` 0x73C8A4 (14 slots: 2 dtor, 3 Clone, 4 IsAlive, 5 IsOpen, 6 HasPacketHeader, 7 CountPackets, 8 SendBuffered, 9 PopPacket,
10 Pull, 11 Flush / Close, 13 Write): `+8` open, `+9` fresh, `+0xc` a `CFileStream` 0x73C8D8 (`IStream` 0x73BDB0 slots 1 Read, 2 Write,
3 Seek, 4 GetSize, 5 GetPos, 6 Clear, 7 CopyFrom over an MFC `CFile`). Subclasses:

| class (vtable, size) | role | slot 4 / 9 / 10 / 13 |
|---|---|---|
| `PeerMsgStream` 0x73C990 0x54 | the live link: `+0x28` CMemStream of frames, `+0x50` NetPeer ref | alive = peer alive; PopPacket; `v10_PullFromPeer` 0x4797A0 (in stream `+0x1148` -> `+0x28`); `v13_Write` 0x4796B0 -> out stream `+0x112c` |
| `TcpMsgStream` 0x73CA10 0x54 | dead (no creator is called) | `v13_Send` 0x47C420 = `send()` |
| `FileMsgStream` 0x73C8F8 0x30 | the packet recorder / replay: `+0x28` list of recorded frames, `+0x2c` chained stream; `Create` 0x4780B0 (`CPlayerClient::SendMsg02_RequestParty` records), `CreateChain` 0x4782E0 (`CWorldClient::PlayRecordedStream` 0x566CE0) | `v9_PopPacket` 0x478270 = next recorded frame; default `v9_WriteChunk` 0x478110 appends a frame to the file |
| `DualFileMsgStream` 0x73C92C 0x44 | replay from a second `CFileStream +0x28` (`Open` 0x478960, `PlayRecordedStream2` 0x566D30) | `v9_PopPacket` 0x478C70 = `Stream_ReadEmbeddedStream` |

`CMemStream` (0x28, `vt` 0x73C960: `+5 ok, +0xc bitByte, +0x10 bitMask, +0x14 bitPos, +0x18 buffer, +0x1c capacity, +0x20 position,
+0x24 size`; `ctor` 0x41DD70, `SetSize` 0x4A54D0, bit I/O `BitWriter_PutBit` 0x47DFC0 / `BitStream_BeginByte` 0x4A5610 / `BitReader_GetBit`
0x47EAE0) is the in-memory `IStream` everything above serializes into.

### 5.6 The delta codec (unit / object state replication) [V]

`DeltaCodec` (0xa0, `ctor` 0x66A300) = four `CMemStreams`: **base A** `+0`, **base B** `+0x28`, **delta A** `+0x50`, **delta B** `+0x78`.
Writers (`DeltaWriter::WriteBytes` 0x47DCA0 and the typed helpers) encode a value against **both** bases at once and append a bit-coded
delta to each delta stream; readers (`DeltaReader::Read` 0x47E000, 73 callers = every `*::ReadNetState` field) decode against the one active
base `g_0x7A9794` with bits from `g_0x7A9798`. Coding of n bytes vs a base: base shorter than the position -> `0` bit then per dword `0` bit
+ 4 raw bytes, remainder raw (and the bytes appended to the base); equal -> one `1` bit; different -> `0` bit then per dword `1` (same) or
`0` + 4 raw bytes, remainder raw; the base is overwritten when the update flag of that base is set (`g_0x7A9784` A, `g_0x7A9780` B);
position += n. Bits come from the current bit byte of the stream (`BitReader_GetBit`), a new byte when 8 are used.

Typed fields: `WriteFloat16` 0x47F1E0 / `ReadFloat16` 0x47F3D0 (sign, 7-bit exponent, 8-bit mantissa), `WritePos3D` 0x47E3A0 /
`ReadPos3D` 0x47E930 (x, y `(v + 16) * 120.46875`, z `(v + 32) * 341.32812`, u16 each), `WriteVec2Q64` 0x47EBF0 / `ReadVec2Q64` 0x47EE00
(`(v + 32) * 60.234375`), `WriteFloatRange` 0x47EF80 / `ReadFloatRange` 0x47F110 (`(v - min) * 65535 / (max - min)`), `WriteVec3F16`
0x47F4D0 / `ReadVec3F16` 0x47F7F0, `WriteRaw` 0x47E140 (no compare), the length-prefixed stream (5.2). `DeltaWriter_Begin` 0x47FA40 /
`DeltaWriter_Begin2` 0x47DB70 (`which`: rewinds the bases, empties the deltas, A updated only when `which == 0`, B always),
`DeltaReader_Begin` 0x47FAA0 / 0x47DBD0 (selects base A or B and the bit stream), `End` 0x47FAF0 / 0x47DC20 (flush bits; a writer
truncates the bases to the position when the update flags are set).

`DeltaCodecSet` (0x74: `+4` array of `DeltaCodec` per field slot, `+8` count, `+0x14` maskA, `+0x16` maskB, `+0x18` changedMask, `+0x1c`
inStream, `+0x20` state 0 full-write / 1 delta-write / 2 read vs A / 3 read vs B / 4 packed, `+0x24` packedA, `+0x4c` packedB) is what the
objects own (`CMapObject +0x1C4`): `SetMode` 0x47FD50 (0 -> changedMask 0xffff), `BeginSlot(slot)` 0x47FBF0 (0 when the slot's bit is clear:
the field is skipped; else begins its codec in the current mode), `Pack` 0x47FE90 (`packedA = u16 maskA | changed` + delta A of every
accumulated slot, `packedB = u16 changed` + delta B of the slots changed now; state 4), `BeginRead(which, IStream)` 0x47FD80 (reads the u16
mask), `Reset` 0x47FB70. Semantics [I]: base A = the object as of its last **full** pack, base B = as of the last pack of any kind; so
`packedA` holds everything changed since the last full state and `packedB` only what changed since the last send; a full pack
(`CMapObject::PackFullNetState` 0x50EA40 -> the 0x50 / 0x40 payloads) is read against empty bases (um-bot decodes creations from zero), a
delta pack (`PackDeltaNetState` 0x50EA70 -> 0x51 / 0x41) against the receiver's previous state. Which form each receiver gets is decided
by `CPlayerServer` (`world-client-players.md`). Users: `CMapObject::WriteNetState` 0x50EAF0 / `ReadNetState` 0x50EC30 (sections 0-3),
`CUnit::WriteNetState` 0x52EC70 (4-13, `MoveRequest::WriteNet` 0x5C7DB0 = section 10), `CLeverObject` 0x52B560 (5), `CTorchObject` 0x52C180
(4), `CEffect::WriteNetStateA/B` 0x675B40 / 0x675B70 (own codec `+0xA8`), `CEffectShowVisibility`, and stack codecs in
`CPlayerClient::OnMsg0E_MovePath` / `CPlayerServer::SendMsg0E_MovePath`.

### 5.7 Dead code: Huffman, BigNum / RIPEMD [V]

- `HuffmanCoder` (0xc08: codes `+0`, lengths `+0x400`, frequencies `+0x800`, root `+0xc04`; `BuildTree` 0x47A8B0, `BuildCodes` 0x47AA10,
  `RebuildCodes` 0x47A800, `AssignCodes` 0x47AA80, node pool `g_0x7A972C`): no constructor exists and no function outside the coder calls
  it; the LZ stream uses fixed prefix tables. Linker leftover.
- `KeyHash_Derive` 0x431300 (xor-folds 64 bytes at `this +0x40` into 16, one `Ripemd128_Compress` 0x4319E0 round from the standard IV),
  `BigNum_ModExp` 0x431370 (square-and-multiply on 16-bit limbs over `BigNum_DivMod` 0x4315B0): no callers. There is no key exchange, no
  session key and no CD / integrity check in the protocol; the login magic is the only "secret".

### 5.8 GameException and FatalError [V]

`FatalError(fmt, ...)` 0x480080 (192 callers) logs at level 0x10, calls the fatal callback `g_0x7A9BA0`, `g_0x7A9B9C++`, `exit(-1)`;
`FatalErrorHr` 0x4800E0 appends `(0x%.8X) <DDrawErrorString>`. The net code never throws: parse errors end in `NetFatalError` 0x664B70
("Wrong packet ID", "Packet has no handler", "Error during parsing update prevID %x curID %x", "Pkt read error", `Serv_` variants) or
in a peer drop. `GameException` (0xc: `vt` 0x73CAB4, `+4` message, `+8` hr; ctors 0x480250 / 0x480300) is thrown only by the DirectDraw /
texture layer (0x4E6000-0x4F1000, 43 sites) and caught there.

## 6. What is not known

Settled in the T3 pass (2026-11-02; log in `_cpr/claude-re/full/t3-settled.tsv`):

- The master request is 4 bytes: `u32 NetClient +0x25b8` (the address-book dword, also passed to `Lobby_StartServerOrClient` as its last
  argument), written by the `CConnectProgressWin` ctor into the `TcpStream` send buffer; the TCP target is the master name
  `g_0x79BED8`:28004 (4.8) [V]. The master's reply beyond `{rawLength, LZ(records)}`: [untestable] (no master exists and the name is
  empty in this exe).
- `SentPacket`: the chunk copy is 260 bytes (0x41 dwords: the stream-chunk node from `+8`), at `+0x18..+0x11b`; the lost flag is `+0x11c`
  (3.4) [V].
- `Game_MapFileExists` 0x5812F0 tests the specific file `maps\<name>` (FindFile on `World_MapPath() + name`) [V]; the name is misleading
  (batch-t3b renames it `Game_MapFileExists`).
- The base A / B semantics of the delta codec stay [I] (5.6): **[live]** (`live-checklist.tsv`: log `DeltaCodecSet::BeginRead` /
  the base lengths on a client for a 0x40 and a following 0x41 of the same unit).
- `NetPeer +0x1108` / `+0x1114` are the empty `less<>` comparator bytes of the STLport maps `+0x1100` / `+0x110c` (the `_Rb_tree` ctor
  stores the comparator byte at map + 8 after the node count, 0x432EC9 / 0x432EF5): no flag, never read [V].
- `ServerInfo.gameKind` = `LobbyServer +0x25e4` = the ctor argument = the lobby screen's allod tab `CLobbyScreen +0x19c` (0..3,
  `lmp_allod_name_%d`; 0 after `Build`): `JoinSelected` 0x61CD8C -> `Lobby_StartServerOrClient` third argument -> `LobbyServer::ctor`.
  The zero bytes of the captures are the first allod tab [V]; the "4 by default" was wrong.

## 7. Corrections to earlier notes

| file | wrong | right |
|---|---|---|
| `docs/multiplayer-protocol.md` 1 | type 3 "get info: u32 token", type 4 "u32 12500, 20 zero bytes (33 bytes)" unknown meaning; "keep-alives every 0.3 s" | type 3 carries the client's f32 clock (echoed = ping); 12500 is the rate in bytes/s (LAN; 2500 otherwise) that sets the send interval `250/rate`; the 20 bytes are the lobby password (absent when none); a type-0 update is the keep-alive, the interval is 20 ms LAN / 100 ms internet, 60 s silence drops the link |
| `multiplayer-protocol.md` 1 | "0xFF master server: unknown, unused" | `{0xFF, u32 requestId, u32 publicAddress}` reply to the 20 s heartbeat (4.8); dead in this build (empty master name) |
| `multiplayer-protocol.md` 2 | "chunk up to 204 bytes", "the resend timing constants are unknown", "packets are 228 bytes: old buffer bytes" | the 220-byte budget `NetPeer +0x50` gives 204 (3.4); resend = `rtt + 3 sigma` clamped 0.1..5 s (3.6); the datagram length is the buffer's `validEnd` (stale bytes possible) |
| `multiplayer-protocol.md` 3 | kinds 4, 12, 13, 14, 17 unknown; kind 3 "8000 port unknown"; kind 11 "quest of the session: u16 index, u32, str file" | 4 = own peer record, 12 / 13 = file chunk / chunk request, 14 = player event (xp / money / items), 17 = swap pair (4.2); 8000 is the dead TCP driver's default port; 11 = file transfer `{u16 chunkCount, u32 size, name}` of the `.mq` |
| `multiplayer-protocol.md` 3 | "kind 16 hero blob ... vtable +0x28"; "Kinds 15 and 16 re-sent by the camp UI ... the heroes" | kind 16 = one offered ITEM (the swap counter), kind 15 `+0x18` = money offered, `+0x24` = the offer nonce (the page's own "swaps" subsection was already right; the table rows above it were not) |
| `multiplayer-protocol.md` 1 | "Server side ... a known cookie from a new address is Reaccepting" | re-accept matches the ADDRESS of a pending (not yet updated) login; a mismatching linkId is only a warning (3.3) |
| `docs/game-exe/net-notes.md`, `notes-net.md` | NetPeer "+8 short tick, +0x20 id/flag, +0x24 timeout (5.0 s)", "+0x112c incoming GameBuffer, +0x1148 outgoing" | +8 = next send seq, +0x20 = linkId, +0x24 = RTT mean (init 5.0); `+0x112c` = OUT stream, `+0x1148` = IN stream (3.7) |
| same | "BigNum_ModExp + Ripemd128 + KeyHash_Derive (session key)"; "HuffmanCoder" listed as live | all dead code (5.7) |
| `docs/game-exe/net-notes.md` | "LobbyClient +0x2604 keyed refs", "+0x2614 party list (three 0x70-byte CParty blocks)" | +0x2604 = timed outgoing objects (named events, chat lines; 60 s); +0x2614 = a list, the two CParty blocks are +0x2620 (own offer) / +0x2690 (partner's) |
| `docs/game-exe/world-client-players.md` 1 | "`MsgStream_Decompress2` 0x4A5790 writes {u32 compressed length, u32 raw length} + LZ bytes; `MsgStream_Decompress` 0x4A5790.." | `MsgStream_Compress` 0x47D7F0 writes `{u32 raw length, LZ}`; the frame length is added by `MsgStreamBase::v8_SendBuffered` 0x47BAD0; the inflater is `MsgStream_Decompress` 0x47D970; nothing is at 0x4A5790 |
| `world-client-players.md` 1 | "slots 4 ready" of the net stream (no name) | slot 4 = `v4_IsAlive` (peer alive); the earlier function comments `IsEof` had the sense inverted |
| `docs/game-exe/world-server.md` 2.6 | "`SendToUnreadyPlayers(8, zone)`" | `PostSystemChatToNonHosts` 0x41A500: the byte it tests is the host flag, the message is system chat 8 (`lmp_zone_enabled`) to every non-host |
| `world-server.md` 2.6 / `flows.md` | "`AcceptPlayers`: server vt[6](&list) takes the accepted joins" | `vt[6]` is `RefListPair::v6_MoveRefsToList` of the stream pair `CWorld +0x60`: the new `PeerMsgStreams` made by `SyncPeers` (4.9) |
| `_cpr/claude-re/net/README.md` | "keep-alive packet {3, time}"; info payload "00x6, u8 1, u8 5, u8 0 (players 1 / max 5 ?)"; "TRIED kind 13 with ref (3,1)" | type 3 = get-info; `u32 gameKind, map\0, players, max, hasPassword`; kind 13 is a file-chunk request (a ref to a kind-11 object) |
| `_cpr/claude-re/net/engine/README.md`, `net_types.hpp` | "NetConnection layout +0x08 nextSendSeq ... +0x1148"; "NetConnection::WriteAcks / AckOne / ReadAcks / AcceptSeq / ParseUpdate" | those fields and functions belong to `NetPeer` (0x1168, one per link); `NetConnection` (0x2568+) owns the socket, registries, handler tables and the peer list; the engine's struct works because it only ever points at a NetPeer |
| `functions.tsv` (merged by the caller) | `NetServer::GetLoginReply`, `SendKeepAlive`, `SendReject`, `LobbyServer::RaiseMaxCounter`, `IsPeerLoaded`, `FirstReadyPlayerId`, `Game_LoadWorldMapStreams`, `Lobby_SendMapFiles`, `MsgStream73C*::*`, `NetSocket::Close` | `GetRateFor`, `SendGetInfo`, `SendDisconnect`, `GameBuffer::CommitCursor`, `IsPeerHost`, `HostPlayerId`, `Game_SaveReceivedMapFile`, `Lobby_QueueMapFileTransfer`, `CFileStream / FileMsgStream / DualFileMsgStream / PeerMsgStream / TcpMsgStream::*`, `TcpStream::Close` (batch rows) |
| this page 3.6 (until 2026-11-06) | "Removals (flag 0) are written in every update while their entry lives [I]" | written once (entry version 0 -> 1 after writing), again only after `PacketLost` resets it; erased on ack (`SendUpdate` 0x435A00) |
| this page 3.4 | "the loop stops once `cursor - start > budget`" | the test follows each write and counts from the seq field; the crossing message stays in the datagram uncounted, and each later group adds one more |
| this page 3.3 | `v5_OnLogin` clamps the rate to `GetRateFor` "and 500..50000" | `OnLogin` only caps it by `GetRateFor`; 500..50000 is `AddPeer`'s clamp; a re-accept keeps the record's age |
| this page 3.6 | "created handlers when the old version was 0 = a tombstone" | a released object (removed key / tombstone) is re-created with version 0 and gets the created handlers; the tombstone keeps the removal's seq |
| this page 4.5 | `SetMap`: "list `+0x25ac` := `<quest>_2`" | it also clears every variable `+0x25b0`, then seeds the briefing-record variables and `q.<quest>.<quest>[.1]` = 1 |
| `functions.tsv` (merged by the caller) | `LzCompress` "rewrites the buffer in place"; `Scene3ds_ConvertToModel` | `(compressor, input view, output buffer)`; `Scene3ds_ReadBinaryScene` (a binary scene reader, dead in practice) (batch-t5-wcp-net rows) |
| `structs.tsv` | `LzCompressor +0 byte[10256] "1 KB ring window and the Huffman / bit writer"`; `DeltaCodec +0 byte[160] streams`; `DeltaCodecSet +0x22 inStream` | the LZ coder has no Huffman part (window, window copy, chain, hash heads, bit writer, pos, match); four named CMemStreams; `+0x1c` (batch CORRECTION rows) |

## 8. Function index

Transport: `Net_TickAll` 0x4122D0, `NetConnection::ctor` 0x436770 / `dtor` 0x436A50 / `Receive` 0x437370 / `v2_RemovePeer` 0x436CF0 /
`AddPeer` 0x436DD0 / `GetRateFor` 0x436F80 / `SendDisconnect` 0x436FA0 / `v3_OnUpdate` 0x437080 / `v6_OnDisconnect` 0x437240 / `AddHandler`
0x4377D0 / `FindPeerByAddress` 0x436CC0 / stubs 0x4371E0-0x437340; `NetServer::Receive` 0x443590 / `v3_OnUpdate` 0x4434F0 / `v4_OnGetInfo`
0x443060 / `v5_OnLogin` 0x443080 / `ReplyInfo` 0x442C60 / `dtor` 0x4132F0; `NetClient::BeginConnect` 0x442980 / `Receive` 0x442AD0 /
`SendGetInfo` 0x441660 / `SendLogin` 0x441740 / `v7_OnInfo` 0x441D40 / `v8_OnAccepted` 0x441940 / `v9_OnRejected` 0x441B40 / `v3_OnUpdate`
0x4418F0 / `v2_RemovePeer` 0x4420E0 / `TickDiscovery` 0x442180 / `AddServerAndPing` 0x442590 / `AddServerInfo` 0x4428A0 / `SetServerList`
0x442300 / `RemoveServer` 0x4427F0 / `GetServerInfoAt` 0x442A40 / `dtor` 0x41C530; `NetPeer::ctor` 0x432DA0 / `Tick` 0x433BD0 / `PacketLost`
0x433F20 / `OnPacketAcked` 0x433FA0 / `RegisterObject` 0x434110 / `SyncObjectSet` 0x4346D0 / `TakeStreamChunk` 0x434990 / `DrainChunks`
0x434AE0 / `WriteAcks` 0x434C20 / `AckOne` 0x434CE0 / `ReadAcks` 0x434D70 / `AcceptSeq` 0x434E50 / `SetObjectId` 0x4352F0 / `ReadObjectRef`
0x435820 / `SendUpdate` 0x435A00 / `AllocSeqId` 0x435F10 / `ReadMessage` 0x435F30 / `ParseUpdate` 0x436470 / `ReleaseReceivedObjects`
0x436620 / `Clone` 0x433060 / `dtor` 0x433320; `NetWriter::WriteRef` 0x4355B0; `NetPeer_DistributeIds` 0x432D20; `NetClassRegistry::Register`
0x42A890; `NetObjectFactory` 0x438930; `NetSocket::ctor` 0x4407F0 / `Open` 0x4409A0 / `SendTo` 0x440B90 / `ReceiveFrom` 0x440BE0 / `dtor`
0x440940; `NetAddress::FromString` 0x440440 / `ToString` 0x440690; `NetAddressList::Get` 0x440A90 / `IsLocal` 0x440AF0; `TcpStream::ctor`
0x440C60 / `Poll` 0x440DE0 / `Close` 0x440DA0 / `dtor` 0x440D40; `AddrBook_Serialize` 0x4414E0; `NetPlayerInfo::Write` 0x401000 / `Read`
0x401170; `RegisterMessageFactory` 0x4142F0.

Lobby server: `ctor` 0x412D80 / `dtor` 0x417BC0, `SetPublic` 0x417E10, `IsPeerInGame` 0x417E40, `v13_GatherReplicated` 0x417EB0, `v14_Tick`
0x4182E0, `AddMapListEntry` 0x418430, `RefreshMapList` 0x418530, `SetMap` 0x4186F0, `UnloadMap` 0x418CC0, `OnMapRecord` 0x418ED0,
`PostMapEvents` 0x4191C0, `OnQuestMapFinished` 0x4193F0, `FindVariableNode` 0x419570, `GetOrAddVariable` 0x4195D0, `SetVariable` 0x419730,
`MaxVariable` 0x4197D0, `RemoveVariable` 0x4198D0, `GetVariableValue` 0x419940, `AddFileTransfer` 0x419A00, `PostPlayerEvent` 0x419D90,
`IsPeerHost` 0x419F90, `HostPlayerId` 0x41A020, `v16_CanAccept` 0x41A050, `SetPassword` 0x41A140, `FindPeerByPlayerName` 0x41A170,
`FindPeerByPlayerId` 0x41A270, `FindPlayerById` 0x41A2E0, `PostChat` 0x41A310, `PostSystemChat` 0x41A490, `PostSystemChatToNonHosts`
0x41A500, `KickPeer` 0x41A550, `BanPeer` 0x41A620, `v11_OnPeerTimeout` 0x41A700, `v12_OnPeerDisconnect` 0x41A770, `v15_FillServerInfo`
0x41A7E0, `SendMasterHeartbeat` 0x41AA10, `v10_OnMasterPacket` 0x41ACA0, `FindPairMessage` 0x41ACF0, `OnJoinUpdate` 0x41AD80, `OnJoin`
0x41ADB0, `OnLeave` 0x41B0C0, `OnChat` 0x41B1C0, `OnNamedEvent` 0x41B2A0, `OnChunkRequest` 0x41B690, `OnChunkRequestRemoved` 0x41B720,
`OnPairRecordAdded` 0x41B7A0, `ConfirmPlayerPair` 0x41B970, `OnPairRecordRemoved` 0x41BA30; glue `Lobby_StartServerOrClient` 0x57E740,
`Lobby_QueueMapFileTransfer` 0x5809D0, `Game_SaveReceivedMapFile` 0x5813A0, `Lobby_SendNamedEvent` 0x57FB60, `Lobby_QuestMapFinished` 0x57FB90,
`Lobby_FormatSystemLine` 0x5F8C90, `Session_SetMapName` 0x57ED60.

Lobby client: `ctor` 0x41BC40 / `dtor` 0x41CA10, `v14_PerFrame` 0x41CCD0, `ApplyPlayerEvents` 0x41CD80, `v13_GatherReplicated` 0x41D050,
`QueueTimedObject` 0x41D1A0, `SendNamedEvent` 0x41D260, `v15_OnConnected` 0x41D410, `v16_NewServerInfo` 0x41D420, `OnServerInfoPacket`
0x41D600, `FillParty` 0x41DA50, `LoadPartnerParty` 0x41DDC0, `ConfirmPair` 0x41E0A0, `RetractExchange` 0x41E180, `FindPartnerRecord` 0x41E220,
`CreateExchangeRecord` 0x41E270, `ReleaseExchangeRecord` 0x41E3E0, `OnOwnPeerRecord` 0x41E430, `OnPlayerRecordAdded` 0x41E490 /
`Removed` 0x41E520, `OnChatLine` 0x41E5D0 / `OnChatLineRemoved` 0x41E690, 0x41E730 / 0x41E760 (kind 3), `OnQuestStateAdded` 0x41E790 /
`Removed` 0x41E850, 0x41E900 / 0x41E960 (kind 9), `OnVariableAdded` 0x41E990 / `Removed` 0x41EA20, `OnFileTransferAdded` 0x41EAC0 /
`Removed` 0x41EBF0, `OnChunkReceived` 0x41ECC0 / `OnChunkRemoved` 0x41EF10, `OnPlayerEventReceived` 0x41EF90, `OnExchangeRecordAdded`
0x41EFF0 / `Removed` 0x41F0D0, `OnExchangePairAdded` 0x41F1E0 / 0x41F270 / 0x41F3A0; `Lobby_Create` 0x57F390, `Lobby_IsAlive` 0x57F440,
`Lobby_ResetMpGameState` 0x57F280, `CConnectProgressWin` 0x61E2F0 / `Update` 0x61E4E0, `CLobbyScreen::StartConnect` 0x61D0D0.

Messages: `JoinMessage` 0x413350 ctor / 0x4136B0 Write / 0x413750 Read; `TwoStringMessage` 0x414770 / 0x414820; `ServerInfo` 0x412370 ctor /
0x412990 Write / 0x412A30 Read; `ObjListMessage` 0x412AC0 / 0x412B70 / `Reset` 0x4177A0; `ObjPairMessage` 0x412C80 / 0x412CE0; `BlobMessage`
0x417560 / 0x4175B0 / `Clone` 0x417420; `TwoRefMessage::Read` 0x414210; the single-string classes 0x413B50-0x4171A0; `ObjRef_Release` 0x413270.

Streams and codecs: `ConnectToLobbyHost` 0x566D80, `AcceptPlayers` 0x58DAF0, `CWorld::ResetNetDriver` 0x55F410, `RefListPair::v6_MoveRefsToList`
0x47A230, `CTcpNetDriver::SyncPeers` 0x479E30 / `ConnectBlocking` 0x47A080 (dead: `AcceptNewClient` 0x47CFA0, `ConnectAsync` 0x47C8E0,
`TakeConnectResult` 0x47CAA0, `TcpConnectThread` 0x47C680, `TcpDriver_TickPeerTimeouts_dead` 0x47C010); `PeerMsgStream::ctor_body` 0x47A030 /
`v4_IsAlive` 0x479780 / `v10_PullFromPeer` 0x4797A0 / `v11_FlushToPeer` 0x479810 / `v13_Write` 0x4796B0 / `dtor` 0x479C10; `MsgStreamBase::
v6_HasPacketHeader` 0x47B9F0 / `v7_CountPackets` 0x47BA40 / `v8_SendBuffered` 0x47BAD0 / `v9_PopPacket` 0x47BB70; `MsgStream_MoveTo` 0x47B9B0;
`FileMsgStream::Create` 0x4780B0 / `CreateChain` 0x4782E0 / `v9_PopPacket` 0x478270; `DualFileMsgStream::Open` 0x478960 / `v9_PopPacket`
0x478C70; `CFileStream::ctor` 0x478470 / `v1_Read` 0x477F00 / `v2_Write` 0x477F30 / `v3_Seek` 0x477F50 / `v4_GetSize` 0x477F70 / `v5_GetPos`
0x477F80 / `v6_Clear` 0x477EE0; `MsgStream::ctor_base` 0x477E60 / `dtor_base` 0x478050; `MsgStream_Compress` 0x47D7F0 / `Decompress` 0x47D970
/ `WriteLenPrefixed` 0x47DA80 / `ReadLenPrefixed` 0x47DAE0; `CPlayerClient::FlushSendStream` 0x667810 / `CPlayerServer::FlushSendStream`
0x66D100 / `CPlayerClient::Update` 0x667A30; `LzCodec_InitTables` 0x42FAE0, `LzCompressor::Reset` 0x42FCA0 / `EmitMatch` 0x42FD10 /
`EndOutput` 0x41D960, `LzCompress` 0x430450, `LzDecompress` 0x430960, `LzBitWriter_PutBits` 0x4301E0 / `End` 0x430F90, `LzBitReader_GetBits`
0x430FE0 / `GetBit` 0x431040, `LzInputAdapter::v6_SetEnd` 0x47D930; `GameBuffer::ctor` 0x4DA7F0 / `v4_WriteGrow` 0x4DA740 / `v6_Seek` 0x4DA860
/ `Write` 0x436C60 / `WriteRaw` 0x436C30 / `CommitCursor` 0x419CC0 / `WriteLength` 0x4DA380 / `ReadLength` 0x4DA200 / `ReadProgressRatio`
0x41D9A0; `NetCursor::ctor` 0x4DA490 / `WriteBytes` 0x430930 / `ReadBytes` 0x430F60; `Stream_WriteEmbeddedStream` 0x4A5250; `CMemStream::ctor`
0x41DD70 / `SetSize` 0x4A54D0 / `Clear` 0x417350 / `dtor` 0x4173D0; `BitWriter_PutBit` 0x47DFC0 / `BitStream_BeginByte` 0x4A5610 /
`BitReader_GetBit` 0x47EAE0; delta: `DeltaCodec::ctor` 0x66A300, `DeltaWriter::WriteBytes` 0x47DCA0 / `WriteRaw` 0x47E140 / `End` 0x47DC20,
`DeltaWriter_Begin` 0x47FA40 / `Begin2` 0x47DB70, `DeltaReader_Begin` 0x47FAA0 / `Begin2` 0x47DBD0 / `End` 0x47FAF0, `DeltaReader::Read` 0x47E000,
`WriteStreamLenPrefixed` 0x47E190 / `ReadLenPrefixed` 0x47E240, `WritePos3D` 0x47E3A0 / `ReadPos3D` 0x47E930, `WriteBytesVsPrev` 0x47E5E0 /
`WriteBytesRaw` 0x47E6D0 / `WriteBytes` 0x47E740 / `ReadBytes` 0x47EB20, `WriteVec2Q64` 0x47EBF0 / `ReadVec2Q64` 0x47EE00, `WriteFloatRange`
0x47EF80 / `ReadFloatRange` 0x47F110, `WriteFloat16` 0x47F1E0 / `ReadFloat16` 0x47F3D0, `WriteVec3F16` 0x47F4D0 / `ReadVec3F16` 0x47F7F0,
`DeltaCodecSet::Reset` 0x47FB70 / `BeginSlot` 0x47FBF0 / `SetMode` 0x47FD50 / `BeginRead` 0x47FD80 / `Pack` 0x47FE90; ChunkFile 0x42BEC0-0x42D270
(5.3); dead crypto `KeyHash_Derive` 0x431300, `BigNum_ModExp` 0x431370, `BigNum_DivMod` 0x4315B0, `Ripemd128_Compress` 0x4319E0; Huffman
0x47A710-0x47ACA0; `FatalError` 0x480080, `FatalErrorHr` 0x4800E0, `GameException::ctor` 0x480250 / `ctor_hr` 0x480300, `NetFatalError` 0x664B70.

# Evil Islands multiplayer protocol (game.exe, Russian 1.06 build)

What the game sends over UDP port 8888, both ways, as reverse-engineered from game.exe and confirmed with
captures and with um-bot (a standalone client that joins, enters a quest, walks, follows and attacks).
Addresses are game.exe's. The research notes behind this page: `_cpr/claude-re/net/README.md` (captures,
tests, dead ends) and `_cpr/claude-re/net/engine/README.md` (the engine-side map: classes, layouts, the
re-implementation in um-engine.dll). Items marked **unknown** are still to be reversed.

Layers, from the wire up:

1. **Datagrams**: one UDP socket each side, 2048-byte packets, the first byte is the packet type.
2. **Updates** (type 0): the only packet after the handshake. Sequence numbers, acknowledgements, one chunk
   of the byte stream, and the reliable messages.
3. **Reliable messages**: replicated objects of the lobby (players, quests, chat, events...), kinds 1-17.
4. **The byte stream**: an ordered, compressed stream in each direction carrying the world: the hero upload,
   the world snapshot, unit updates, orders.

All integers are little-endian. `str` = a NUL-terminated string. `f32` = IEEE float. `ref` = a reference to a
replicated object: u8 0 for none, else u8 kind + u16 id (0x4355B0; a flag bit goes with it).

## 1. Datagrams

| byte | packet | direction | payload |
|---|---|---|---|
| 0x00 | update | both | see section 2 |
| 0x01 | disconnect | both | u32 client id |
| 0x03 | get info | client -> server | u32 token (echoed back) |
| 0x04 | login | client -> server | u32 KEY, u32 cookie, u32 12500, 20 zero bytes (33 bytes in all) |
| 0x05 | info | server -> client | u32 token, u32 KEY, str host name, 5 zero bytes, u8 players, u8 max players, u8 0 (during a quest the quest title follows the host name) |
| 0x06 | accepted | server -> client | u32 cookie (the client's own, checked against what it sent) |
| 0x07 | rejected | server -> client | u32, u8 reason |
| 0xFF | master server | | the internet lobby list; **unknown**, unused in LAN play |

Transport (module A of game.exe, 0x434000-0x444000): `sendto` 0x440B90 / `recvfrom` 0x440BE0 (um-engine's
net_udp.cpp reproduces both). The receive loop 0x437370 reads every waiting datagram, switches on the type byte
(vtable slots of the connection class: 0 update 0x437080, 1 disconnect, 3 get info, 4 login, 5 info,
6 accepted, 7 rejected, 0xFF master), then sweeps the clients for timeouts (0x433BD0: "TIMEOUT client, drop").
A zero-length datagram is logged and ignored. The net log (console `net server 1` / `net client 1`, ostream at
0x79B91C) prints these messages; they are quoted below in CAPITALS.

### Handshake (tested against the game's lobby server)

```
C->S  03 token                      get info
S->C  05 token KEY host\0 00x5 players max 00
C->S  04 KEY cookie 12500 00x20     login (the KEY from the info answer; cookie = any value the client picks)
S->C  06 cookie                     accepted (07 ... = rejected)
C->S  00 ...                        updates, from now on
```

Server side (0x443080, `OnLogin`): the KEY must match the server's (else ignored); a login from an address
that already has an active session is logged "ERROR: Ignoring login request for active session from" and
ignored; a known cookie from a new address is "Reaccepting client from" (the session moves to the new
address); otherwise "Accepting client from" creates the client link ("ADD client" 0x436DD0) and answers 06. A
server past its accept period answers nothing ("Accept period elapsed", 0x443590). "LinkID mismatch" is the
rejection when the cookie does not match a re-accept. The u32 12500 after the cookie: with 0 the server treats
the link differently (the stream ran from the lobby on); 12500 as the real client sends it is right; its
meaning (a bandwidth or a timeout?) is **unknown**. The 20 zero bytes are **unknown** (never non-zero in captures).

Client side: `OnInfo` 0x441D40 stores KEY and the host's description, `OnAccepted` 0x441940 checks the cookie
(client+0x965*4) and switches the game to its MP state ("z.MPGame1", 0x41D410: nothing is sent there),
`OnRejected` 0x441B40, `OnLost` 0x4420E0 ("CONNECTION LOST server link is lost, entering idle"), "Connection
failed" when nothing answers (0x442AD0). After that the client's per-frame loop (0x41CCD0) sends updates.

The server answers get-info during a game too; it is what the lobby browser polls.

## 2. Update packets (type 0)

```
00  u16 seq  u16 ack  u32 ackBits  u16 chunkOffset  u8 chunkLen  chunk[chunkLen]  u8 n  n reliable messages
```

Everything here is re-implemented in um-engine.dll (net_update.cpp, net_message.cpp) from the game's code; the
rules in short:

- **seq**: 16-bit on the wire, unwrapped to 32 bits by the receiver (+/- 0x10000 when the distance to the last
  received sequence crosses 0x8000, 0x436470). The sender's counter starts anywhere (the server above 0x8000).
- **AcceptSeq** (0x434E50): a packet older than the last received by more than 32 is dropped ("SKIP too old
  update, dif="); a sequence seen in the 32-wide window is dropped ("DUPLICATE update received from"). An
  accepted sequence enters the window and becomes the highest when it is.
- **ack / ackBits** (written by 0x434C20, read by 0x434D70): `ack` = the highest sequence received from the
  peer, bit i-1 of `ackBits` = "the packet ack - i was received too". The reader keeps a mask of the bits it
  already saw (shifted by the signed distance between the new and the last ack) and acknowledges each fresh
  one (0x434CE0 AckOne: the sent packet leaves the waiting lists, RTT statistics 0x433FA0 with factor 0.93).
  The client MUST start acking from the server's own first sequence: acking from 0 makes the server resend old
  data and the stream stalls after ~20 s (a bot bug found the hard way).
- **chunk**: a piece of the byte stream (section 4): `chunkOffset` = its offset in the stream (u16, wrapping),
  up to 204 bytes. Queued when `offset - expected <= 0x8000` and non-empty; the queue is drained in order into
  the incoming stream buffer (0x434AE0). A packet with no stream data carries len 0 and offset = the current
  stream position.
- **n reliable messages**: section 3. The packet's unwrapped sequence is the *version* stamped on the objects
  the messages change. A message that cannot be read (no factory for its kind) drops the sender ("ERROR: client
  ... sent incorrect update", 0x437080).
- **Sending** (0x435A00 SendUpdate, per client link, called from 0x4368D0 / 0x436A50 each frame): the resend
  sweep 0x4346D0 first (a sent packet past its time is lost, 0x433F20 re-marks its messages as to be sent), then
  the header, acks, one chunk taken from the outgoing stream (0x434990, <= 255 bytes, kept in `outChunks`
  until acked), then the messages from the 11 send lists (by channel / priority, +0x1074..+0x10EC), each
  packet recorded with the messages it carried (list +0x10F8, record +0xC). The resend timing constants are
  **unknown** (not needed by a client: the server tolerates a packet every 20 ms, and keep-alives every 0.3 s
  keep a client in the lobby).
- Packets are 228 bytes on the wire in the lobby: past the data, old buffer bytes (the game sends its buffer,
  not just what it filled).

An empty keep-alive from a client: `00 seq ack ackBits 0000 00 00` with `ack`/`ackBits` tracking the server.

## 3. Reliable messages: the replicated objects (lobby, kinds 1-17)

Each reliable message creates, updates or removes one replicated object identified by `kind << 16 | id`:

```
[flag bit]  u8 kind  u16 id  payload (when the flag bit is 1; nothing when 0 = removed)
```

The flag bits come from a flag byte read before every 8 messages, LSB first (the reader's bit buffer; 0x435F30
ReadMessage). Rules (per link, object table at link+0x1100, versions = packet sequences): an unknown key with
the flag set is created (factory 0x438930 by kind, payload read through the object's vtable +0x14, the
"created" callbacks run); a known one with a lower version than the packet is updated ("updated" callbacks);
an older packet is skipped ("SKIP update, newer version of object is already here") but its payload is still
consumed by a throwaway object. A clear flag removes the object ("removed" callbacks) or plants a tombstone
holding the packet's version so older updates in flight cannot resurrect it. Channels when sending (0x4377D0
argument): 0 created, 1 updated, 2 removed.

The classes are registered with their factory ids at 0x4142F0 (id = the kind byte; vtable slot 4 = write, 5 =
read); 0x4355B0 writes a `ref`.

| kind | object | payload (write order) | sent by | handled by |
|---|---|---|---|---|
| 1 | player record | str name ("Name \| CLAN"), str unit ("Human Hadagan NPC Rebel Smith -1" = the character's model line), u32 state, u8 host flag, u32 unit id, u32 tail | server (one per player, id = slot; the host's record has the flag 1 and tail 0, clients' flag 0 and a tail (the unit id's low byte + 1?); checked 2026-10-20 with a host and two bots: the host is not always slot 1 in a client's view) | client 0x41E430.. (the player list, the faces) |
| 2 | system line | u32 code, str text | server | client (chat: "Quest accepted: ...", "Player X connected to the game"; a player's typed chat line arrives here as `^1Name^7: text`, which um-bot reads for its chat commands) |
| 3 | address line | str address ("172.16.0.200:8000"), str map | server, when the host enters a quest | client 0x41E5D0 -> 0x5F82F0 -> 0x5E2A30: a coloured chat line, nothing else (the "8000" port is never opened: **unknown** meaning) |
| 4 | | ref, no other payload | | client |
| 5 | JOIN | str name, str unit, u32 state (+0x28), u8 (+0x24), u32 unit id (+0x2C) | client (its own object, id 1 at client+0x2600; re-sent on channel 1 when a field changes and the version +8 is bumped) | server 0x41ADB0 (create: makes the kind-1 record, "lmp_player_connected" in chat), 0x41AD80 (update: copies state and unit id into the record), 0x41B0C0 (remove: "lmp_player_disconnected") |
| 6 | CHAT | str | client | server 0x41B1C0: broadcast as "^%i%s^7: %s" |
| 7 | named event | u32 kind, str name | both (0x41D260) | server 0x41B2A0: kind 1 = a name ending in `_1` / `_2` / `_3` -> 0x4186F0 / 0x418CC0 / 0x418ED0 (quest events from scripts, 0x592090) |
| 8 | quest state | str ("z3xq1_2") | server | client: `<quest>_2` = the quest's zone in the lobby map is open (the host is inside) |
| 9 | chosen quest | str ("z3xq1") | server | client |
| 10 | quest variable | str name ("q.z3xq1.z3xq1.1"), f32 value, u32 | server | client |
| 11 | quest of the session | u16 index, u32, str file ("z3xq1.mq") | server, the quest list at join | client |
| 12 | | ref, u16, 64 bytes | | **unknown** (world helpers 0x427A62..0x4289B0) |
| 13 | | ref | client | server 0x41B690: sets the sender's ref to the current world (0x79B924), then 0x420610 / 0x4229B0 |
| 14 | | u32, u32, str | server | client (seen in captures, meaning **unknown**) |
| 15 | party | u32 n, n refs (the kind-16 members), u32, u8, u32, u32, ref, ref | client (client+0x260C, filled by 0x41DA50) | server: the heroes' list |
| 16 | hero blob | u16 len, bytes (the member's own serializer, vtable +0x28: NOT the .mp format) | client, one per party member | server |
| 17 | | ref, ref, u8, u32, u32 | | **unknown** |

Kinds 15 and 16 are re-sent by the camp UI (0x62C975, 0x62D16D, 0x62D1BC) every time the party's equipment
changes in the lobby, so the server always holds the current heroes; for entering a quest the stream upload of
section 4 is what counts (um-bot never sends 15/16 and plays fine).

Player states (record u32 state = JOIN +0x28, written by the client's MP state machine): 4 just created,
1 in the lobby, 3 loading the quest map, 0 in the quest world. The unit id is the .mp member's u32
(0x3B9AF5CE in the shipped characters: a constant, not unique) in every state; the world unit's id is another
number (section 4).

Chat line keys printed by 0x5F8C90 (table 0x5F8DC8): 0 lmp_player_connected, 1 lmp_player_disconnected,
2 kicked, 3 banned, 4 lost connection.

### The bit buffer, and swaps (trades) between players (decoded 2026-10-20, live with a host and um-bot)

The flag byte after a packet's message count is a **bit buffer** read bit by bit, a new byte being read from the stream whenever a bit is needed and none is left: every message takes
one flag bit (1 = a payload follows, 0 = the object is removed), and **every object ref inside a payload takes one more bit**, in reading order (`NetPeer::ReadObjectRef`
0x435820: the bit is read before the ref's kind byte and u16 id). A ref's bit 0 = the id is in the sender's own namespace (an object it created), 1 = the id is one the receiver
assigned (e.g. a client referring to a player record the server created: slot ids as the kind-1 messages gave them). A ref written with the wrong bit resolves to a new, blank object
(the "Player  is offering you a swap" line with an empty name).

A swap starts when a player clicks another player's head in the lobby (the clicked head then shows "Swap?" for the other, "Swap!" for the clicker):

| kind | object | payload | notes |
|---|---|---|---|
| 15 | swap record (`ObjListMessage`) | u32 n, n member refs (kind 16: the items offered), u32 +0x18 (**the money offered**: verified 2026-10-20, the host typed 20 in "Amount you offered for a swap" and the field read 20; the exchange moves it to the partner's purse), u8 +0x1c (**confirmed**), u32 +0x20 (**the partner's nonce this side confirmed**), u32 +0x24 (**this record's nonce**, 1, raised by 1 on every change of the offer), ref +0x28, ref +0x2c (kind 1 + player slot) | the clicker creates one (+0x28 = itself, +0x2c = the clicked); the server forwards it to the partner with the chat line "Player X is offering you a swap" |
| 16 | offered item | u16 length (72), the item: u32 id (-1), u32 kind (0x3007 ...), u16 a, u16 b, 4 bytes, the .mp detail's tail (56 bytes: floats, then the **count offered** (a stack: 1 of 5), ...) | one per item the partner puts on the counter; `Detail size + 8` bytes |
| 17 | pair (`ObjPairMessage`) | ref, ref (the two kind-15 records), u8 **confirmed**, u32, u32 | the server creates it when the partner answers with the mirrored record (the same two refs, swapped); the clients open the swap screen |

Answering = clicking the sender's head: send your own kind-15 record with the refs swapped (bits 1,1 for refs to server-made players). Agreeing: set byte +0x1c = 1 and +0x20 = the partner's current +0x24
(redo it whenever the partner's nonce changes: it changes with every item added or removed). The server (`LobbyServer::ConfirmPlayerPair` 0x41B970) marks the pair message confirmed (kind 17, byte = 1,
nonces copied) when both records have +0x1c set and +0x20 equal to the other's +0x24. Both clients then move the offered items between their parties themselves (`CUIScreen740D18::Update` 0x62C930,
`Party_MoveAllItems`): the server never moves items, and it trusts the party a client uploads (0x50 / 0x08). The record is withdrawn afterwards (flag bit 0 for kind 15 id 1). um-bot does all of this
(`Client::ReadMessages` cases 15 / 16 / 17, `AnswerSwap`) and adds the received items to its character (`AddSwappedItems`).

### A session, from the lobby's point of view

1. Join: login, then JOIN (kind 5, state 1, unit id -1 then 0x3B9AF5CE). The server answers with the quests
   (11), the quest states (8), every player's record (1) and a kind-2 line "Player ... connected to the game".
   The lobby is a list: players do not see each other's units there.
2. The host picks a quest: kind 9 (the name), kind 2 "Quest accepted: ...", kind 10 variables, kind 8
   `<quest>_2` when the host entered it, kind 3 (address + map), its record goes 1 -> 3 -> 0 with its hero's
   world id (0x3B9ACA0D).
3. A player enters: JOIN state 3, the stream upload (section 4), JOIN state 0 with the world id the snapshot
   gave. Leaving a quest: the players must leave the quest area before the host (else the host cannot return);
   JOIN back to state 1 with the .mp id.

## 4. The byte stream (the world link, module C 0x664000-0x673000, codec module D 0x47D000-0x481000)

The chunks of section 2 rebuild one ordered byte stream per direction. The stream is a sequence of
**messages**:

```
u32 compressedLength  u32 uncompressedLength  compressed bytes (LZ, the .mp compression)
```

`04 00 00 00 00 00 00 00` is an empty message (keep-alive of the world link). The compression is the one of
.mp files (docs/file-formats/mp-format.md: bit 0 literal / bit 1 match, length 2..226, offset 0..5473, 1 KB
window) with one difference: the server's stream keeps ONE LZ window across all its messages (decoding each
message with a fresh window gives mostly zeros). A client may send literal-only data (flag bytes of zeros).
Decoder in game.exe: 0x430960.

A decompressed message holds **sub-messages**, each starting with a type byte. The handlers are tables of
function pointers: the server's (what a client sends) built at 0x66CB24, the client's (what the server sends)
at 0x667347. The dispatch reads sub-messages until the message ends; the length of each is implied by its
handler, so an unknown type stops the parse (and sending a wrong one gets the next type read as something
else: the bot's zero bytes were read as identity blocks).

### Client -> server sub-messages (server table 0x66CB24)

| type | handler | payload | meaning |
|---|---|---|---|
| 0x00 | 0x66F890 | 136 bytes: u32 100 (client version 1.00), char name[64], char password[64] ("Very top secret" = the game's default), u32 0 | identity: link state 0 -> 1; a wrong version answers 0x11 "Wrong Client Version. Req. version 1.0" |
| 0x01 | 0x66FA90 | **unknown** | |
| 0x02 | 0x66FAA0 | **unknown** | |
| 0x03-0x07 | 0x670C20.. | **unknown** | |
| 0x08 | 0x670F50 | the character: the .mp file's decompressed content (mp-format.md "Data", zone := the quest's name) read by 0x6618C0, then u8 0, then the 0x00 identity block above | the hero upload. The real client appends u32 100 + "Tango\0" and uninitialised bytes; the bot sends only the content + 0 + identity and is accepted |
| 0x09 | 0x671060 | none | READY: link state 4 -> 1, then 0x590700(player) spawns the hero in the world. Sent by the real client with 0x40 right behind it |
| 0x10 | 0x66FAC0 | **unknown** | |
| 0x30 | 0x66FFF0 -> 0x5D39E0 | u32 n, n x u32 unit id, u32 flags (0), f32 x, f32 y, f32 z (0) | MOVE to a point (a ground click) for the listed units |
| 0x31 | 0x670190 -> 0x5D3C30 (order 4) | u32 n, n x u32 unit id, u32 0, u32 target unit id, u32 0 | ATTACK (confirmed: the bot gained EXP). Also used on objects near a body (loot?) |
| 0x32 | 0x6708D0 | **decoded**: u32 spell, u32 caster unit id, f32 x, f32 y, f32 z, u8 flag (21 bytes) | cast at a point (`CPlayerServer::OnMsg_CastAt` -> `AiLogicPlayer::CastAt` -> `CUnitServer::QueueCastAt`). Client sender `CPlayerClient::SendMsg32` 0x66ACD0 |
| 0x33 | 0x670820 -> 0x5D3D20 | **decoded**: u32 spell, u32 caster unit id, u32 target object id, u8 flag (13 bytes) | cast on a target (`OnMsg_CastOn` -> `AiLogicPlayer::CastOn` -> `CUnitServer::QueueCastOn`). Client sender `SendMsg33` 0x66AC70. The spell is NOT an object id: without bit 31 it is an index into the hero's quick-item array (`CUnit::GetItemSpell`); with bit 31 (`slot \| 0x80000000`, `SendMsg33_UseWeapon` 0x66AD40) it is a weapon-slot index and the item there (`CItemQuick` subtype 5 / 8) is used at once: its attached spell is cast with `CWorldServer::CastSpellMulti` and a charge is consumed |
| 0x34 | 0x670960 | u32 unit id, u8 value | `CUnitServer::SetOwnerField270` (the unit's +0x270; sent by `SendMsg34` from key handlers) |
| 0x35 | 0x6709D0 | works as the pace / walk-run switch (um-bot) | |
| 0x36 | 0x670310 | u32 n, n x u32 unit ids, u32 0, **u32 interaction id (0x18A95 = loot)**, u32 -1, u32 target unit id (decoded from the handler's six reads, 2026-10-20; the captured x, y floats after it are not read) | the interaction order (`OnLootOrder`): the nearest listed unit is ordered to go to the target and interact; for 0x18A95 on a dead unit `CUnitServer::ExecuteInteraction` 0x54F000 calls `CWorldServer::LootUnit` (everything the body holds goes into the looter's party at once) and removes the body |
| 0x37 | 0x670810 | none | no-op |
| 0x38 | 0x670A40 | **unknown** | |
| 0x39 | 0x670A90 | **unknown** | |
| 0x3A, 0x3B | | **unknown** | container / item orders (not mapped) |
| 0x40 | 0x670B20 | f32 x, f32 y (map units) | the client's view point (its camera / hero), ~2 per second; stored, not a move |
| 0x50 | 0x670B60 | **unknown** | |

### Server -> client sub-messages (client table 0x667347)

| type | handler | payload | meaning |
|---|---|---|---|
| 0x00 | 0x667DA0 | first the world snapshot (5-6 KB: "\0\x07\0\0\0\x80..." + the quest name, items, every player's party, the map files zone3xobr.mpr / -lmp.mob, "CMPGame1" x5, the unit ids), later 42-56-byte ticks (the game time "gtime"; bit-packed, mostly zero) | the world state / tick |
| 0x02 | 0x669010 | **unknown** | |
| 0x03 | 0x6690F0 | **unknown** | |
| 0x04 | 0x667C20 | **unknown** | |
| 0x05 | 0x6691C0 | per player: name ... "91" + f32 x, f32 y | the players' ARRIVAL points (sent right after the snapshot; the host's too) |
| 0x06 | 0x669490 | **unknown** | |
| 0x07 | 0x669D50 | **unknown** | |
| 0x0A | 0x669290 | **unknown** | |
| 0x0B | 0x667DF0 | u32 unit id, u32 0xFF | after a death: the new hero's unit id (sent with 0x09) |
| 0x0E | | f32 x, f32 y, u32 unit id (seen after a move order; length by heuristics) | |
| 0x0F | 0x667D10 | **unknown** | |
| 0x11 | | str | an error text ("Wrong Client Version. Req. version 1.0") |
| 0x12 | 0x667E20 | **unknown** | |
| 0x13 | 0x667E60 | **unknown** | |
| 0x40 | 0x667EC0 | **decoded: see "Unit creation" below**: u32 id, u32 kind, u32 prototype, u32 flags, the complexion, figure / texture / label strings, the part names, the name, then the unit's whole state as a delta coded 0x41 body | a unit appears (creation) |
| 0x41 | 0x6687B0 -> unit vmethod +0xC4 (0x52EF90, base 0x50EC30) | u32 unit id, then the delta-coded fields below | unit update |
| 0x42 | 0x668850 | 47 bytes, zero in captures | **unknown** |
| 0x50 | 0x668940 | **unknown** | |
| 0x51 | 0x668A30 | **unknown** | |
| 0x52 | 0x668AA0 | **unknown** | |
| 0x60 | 0x668C50 | a list (ids) | a resync list of unit ids seen once (0x12A0 0x1320 0x3B9ACA0D 0x3B9ACA10) |
| 0x70-0x72 | 0x669760.. | **unknown** | |
| 0x80 | 0x669A30 | the player's party: a length (2 bytes = size × 2; 4 bytes = size × 2 + 1 from 15000) + the character in the .mp's unpacked layout (zone name, u32, party, variables) | sent at the start of the session and every 900-1800 ticks (`SendMsg80_Party` 0x66EF80); um-bot writes it back into the character's .mp (checked 2026-10-20: it parses and re-serializes identically) |
| 0x81 | 0x669850 | str; when it starts with "q." -> 0x5F7FC0 / 0x565380 | quest-related (can embed a 0x40 record) |
| 0x82 | 0x669960 | **unknown** | |
| 0x83 | 0x669C90 | the same party blob as 0x80 | sent when the player leaves the world (`World::PlayerOut` 0x590A40, after the picked-up items are moved into the party): the final save; the client message 0x04 + u32 0 triggers it (checked live 2026-10-20: the reply holds a 0x83 and then a 0x80); entering again after it needs a new connection (JOIN 3 + upload got no snapshot) |

### The zone state and the zone exits (read from game.exe 2026-10-21, not yet checked live)

The server's first big message (`CWorldServer::SendZoneState` 0x58C890, read by `CWorldClient::LoadZone` 0x567AB0) starts with: u32 tick - 1, the zone name and the map name (zero-terminated, no length),
u32 exit count, then per exit (`CEffectKey::Write` 0x4E58A0 is really the exit record writer): 9 x u32 (+0 .. +0x20; **+0x14 / +0x18 / +0x1C / +0x20 are the rectangle: x min, y min, x max, y max** in
world units, the same as the units' positions), the exit's name (zero-terminated; "none" = unused), u32 entrance, u32, u8, u32, and a float (the rectangle's rotation about its centre, 0 = none), then
u32 (+0xE0) and the unit hashes. A hero inside an exit (`CRect2F::Contains` 0x4E59F0) can leave the zone through it: the client sends the zone id (`CPlayerServer::OnMsg_ChangeMapById` 0x670C20
-> `CPlayerServer::ChangeMap`); an exit shows its glow only when the player's named value `<prefix><exit name>` is 1.0 (`UpdateZoneExitEffectStates` 0x567680). um-bot (zone_exits.hpp, `make test`) parses
the records to know when the host stands in one: the host cannot leave while other players are in the zone, so the bot leaves first (the leave message 0x04 + u32 0).

### The 0x41 unit update (decoded, um-bot UnitUpdate / unit41.py)

After the u32 unit id: a u16 mask of the fields present (fields 0..13). Every field is **delta coded**
(0x47E000) against the unit's previous value of that field: bits are taken low-first from bytes pulled from
the stream when needed, a new bit byte at each field; a bit 1 = "unchanged" (the whole value), else per 4-byte
chunk a bit 1 = unchanged / 0 = 4 raw bytes, then the n % 4 remaining bytes raw. So a client needs the full
state from the unit's creation (0x40) to decode absolute values; um-bot decodes what it can relative to zero.

| field | size | meaning |
|---|---|---|
| 0 | 24 B (12 x i16) | [2] / [3] = health current / max for monsters (the host's values differ: **to map**) |
| 1 | 12 B | position vec3 (sent while walking) |
| 2 | 1, 12, 16 | **unknown** |
| 3 | 1 (n) + n x 5 | **unknown** |
| 4 | 264 B | the unit's stats: u16 pairs at +72, +100, +128, +156, +184, +212 = the 6 body parts (current / max?), floats at +20 / +32 |
| 5 | 11 x 4 | **unknown** |
| 6 | 4, 4, 4 | **unknown** |
| 7 | 1 | **unknown** |
| 8 | 1, 1, 4, 1 | **unknown** |
| 9 | str | **unknown** |
| 10 | the ORDER (0x5C7FC0): u8 mode, 2 bytes (count), 3 x u16 (two through 0x47EE00: v * 0.0166 - 32, one through 0x47F110), 4 x f32, then count steps as u16 (bits 0-1 / 2-3 = dx / dy: 0 -1, 1 0, 2 +1, 3 = an absolute u16 follows; the rest **unknown**) | mode 0 none; 2 = move to a point: the 2nd / 3rd floats = the destination x2, the steps = the path in half-unit cells, the first step = where the unit is; 3 = attack: the 4 u32 = tick, TARGET UNIT ID, tick, tick, no steps |
| 11 | 4, 1, 4 | **unknown** |
| 12 | 20 B | sent every tick ("00 10 01" when idle); +8 a float rising to 3.0 then falling (an action timer) |
| 13 | 4 | **unknown** |

Checked on two recordings: every update ends exactly where the next one starts (2106 / 2116 and 574 / 574).

### Entering a quest world (the full sequence a client sends; um-bot does exactly this)

```
JOIN state 1 (unit -1), JOIN state 1 (unit 0x3B9AF5CE)      lobby
JOIN state 3                                                 loading
stream: 08 + the .mp content (zone := the quest) + 00 + identity block (u32 100, name, password, u32 0)
<- the server's stream: 4 empty messages, then the snapshot (its new unit id for us: 0x3B9ACA12...)
stream: 09 (ready) [+ 40 x y]                                -> the hero is spawned at the arrival point
JOIN state 0 with the .mp id (the real client keeps 0x3B9AF5CE in the JOIN; um-bot sends the world id)
stream: 40 x y about twice a second; 30 / 31 / 36 orders when wanted
```

Respawn (server 0x66F4C0): the money penalty, a new hero id = old + 1000000 (next free), the hero copied
(0x5947E0), the player taken out and put back (0x590A40 / 0x590700), then stream 0x09 and 0x0B (new id, 0xFF)
to the client. A real client then re-enters: JOIN 3, 08 upload, 09, JOIN 0.

Ids: world unit ids of players start at 0x3B9ACA0D (1 000 000 013); monsters and NPCs have small ids
(0x11A5, 0x12A0, 0x1320...). um.dll's UNITS ids are the game's internal ones, not these.

### Death, respawn and looting the body (verified live 2026-10-20)

- A dead hero stays a body unit (record flag 0x80000, negative health) for as long as the player stays connected. Leaving the world (0x04, or a reconnect) removes the body: a body of an earlier connection cannot be looted.
- On death the party loses the belt potions, the items in the backpack and about 75 % of the money; worn weapons and armor and the **equipped spells (the member's list 4) stay** (checked: a character with an equipped Healing spell had it, with its container, after the death). A carried (unequipped) spell container is lost with the backpack.
- Respawn on the same connection: the client sends JOIN 3 and the upload (08). The server runs `WorldServer::Respawn` (0x66F4C0: the penalty, a reserved copy of the hero with id + 1 000 000, the player out and in, a party 0x83 / 0x80) and answers with a world snapshot that holds only the reserved id (1 001 000 0xx). A second JOIN 3 + upload gets the real new hero: a snapshot with its id (1 000 000 0xx, the next free one), then 09 and JOIN 0 with it. The new hero stands at the arrival point; the host's portrait and minimap show it. (Entering with the stale hero id of the character file in the JOIN, or never answering the first snapshot's id, leaves the portrait without health bar.)
- Looting a body: message 0x36 as above, with the hero and the body in the same connection: the hero walks to the body and `ExecuteInteraction` (kind 6, 0x18A95) calls `CWorldServer::LootUnit`, then the body is removed. Seen working in the bot (the log "our body is gone: looted"); a body that lies behind rocks or trees is not reachable and stays. The host loots a body by clicking it (a body can clip into the ground and be hard to see but stays clickable).

### Unit creation (0x40) and the net-state sections (decoded 2026-10-19, checked against um.dll's UNITS on a live host)

Server -> client 0x40 (client handler 0x667EC0, `WorldClient::OnUnitCreated`), in a stream message after the world snapshot and whenever a unit comes into view:

```
u32 id            players' heroes 0x3B9ACA0D.. (1 000 000 013..), monsters / NPCs the same ids um.dll's UNITS lists (4517 = 0x11A5 ...)
u32 kind          0x32 a player's hero, 0x33 / 0x34 monsters and animals (units; other kinds are objects: 0x3C torches / levers, shaped differently)
u32 prototype     the unit's row in the units DB (97 = a human hero, 146 = boar, 267 = hare)
u32 flags         the object record's flags: 0x8000 resting, 0x2 active, 0x80000 a BODY (dead), 0x20000000 / 0x8000 other
f32 x3            the complexion (morph): 0.5 0.5 0.2
CString           empty
CString figure    "unhuma", "unanwibo"
CString texture   "", "boar02"
CString label     "", "mainMenu00labels", "default0"
u16 n, n x CString  the figure's part names (hd bd hp lh1 ...)
CString name      a hero's name, empty for monsters
u32               the prototype row again (monsters) / a small number (heroes)
then CMapObject::UnpackNetState / CUnit::ReadNetState: u16 mask of the sections present, each section delta coded against the empty state
```

The sections (CMapObject::ReadNetState 0x50EC30, CUnit::ReadNetState 0x52EF90; the writer is CUnit::WriteNetState 0x52EC70, called when the server packs a 0x41 update too). The mask bit N = section N:

| section | size / content |
|---|---|
| 0 | 24 bytes: u16 ?, u16 hp max?, **i16 HP**, **i16 mana**, i16 mana max ... (hero HP 1569 / mana 1255 at creation; monsters send zeros here: their HP is not in the stream) |
| 1 | 12 bytes: the complexion again |
| 2 | u8 flag, **f32 x, y, z** (z is not sent: 0 means "on the ground"), 16 bytes rotation quaternion |
| 3 | u8 n, then n x 5 raw bytes: the effects (spells) on the unit |
| 4 | 264 bytes: the large stats block (exp total at +4 as an integer: 500 for the test bot; level at +8; attributes at +0xC) |
| 5 | 11 x u32: equipment / item ids |
| 6 | 3 x u32 (+0x264, +0x268, +0x26C) |
| 7 | 1 byte: four packed fields (stance, ...) |
| 8 | u8, **u8 action code (0xFE idle, 0x07 fighting, 0xFF dead)**, u32 tick, u8 |
| 9 | a CString |
| 10 | the order / walk (ReadUnitOrder 0x5C7FC0): u8 mode (0 none, 1 move, 2 move to a point, 3 attack), u8 hi, u8 lo (steps = hi * 256 + lo), Vec2Q64 (2 x u16), a float range (u16), 4 x u32 (in mode 3 the 2nd is the target's id), then the steps: u16 codes (bits 0-1 / 2-3 = x / y -1, 0, +1 from the last cell, 3 = an absolute u16 follows; bits 4-5, 6-15 a speed class and a value) |
| 11 | **u32 mask of the sides this unit is hostile to**, **u8 side**, u32 owner id (-1 none). Monsters: mask 0x01 (hostile to side 0 = the players), side 7; hares: mask 0 (they do not attack), side 14; heroes: mask 0x3FFE, side 0 |
| 12 | 20 bytes |
| 13 | 4 bytes |

Delta coding (`DeltaReader::Read` 0x47E000): every value is read from the byte stream with its bits taken from the same stream: a bit byte is fetched when the 8 bits of the last one are used (low bit first); a 1 bit = unchanged against the previous state, else per dword a 1 bit (unchanged) or 0 + 4 raw bytes, the remainder raw. The bits restart with a new byte at each section (checked: 99 of 99 creation records end exactly at the next sub-message with that rule, 5 of 64 without).
The order section of some stationary monsters is shaped differently than the walking ones (a creation of a boar in mode 0 did not decode with the section's own rules); um-bot then takes the position, the hostility mask and the side from the record's start and tail (the next record's header marks the end).

Other server -> client sub-messages seen in a quest world (lengths known):

| type | payload |
|---|---|
| 0x41 | u32 id, then the u16 mask and the sections above (delta coded against the unit's previous state): the unit update |
| 0x42, 0x43 | u32 id: the unit leaves the view / its body is gone |
| 0x81 | CString name, f32 value: a named game value ("gtime" = the day time; "q.<quest>..." quest values) |
| 0x60 | u8, u32 -1, u32 n, n x u32 ids: a resync list of the units the server sees as known |
| 0x04 | u32 id, u32 tick, u8 1: an event on a unit (seen with a hit / a death) |
| 0x0B | u32 id, u32 code (0x29, 0x2F, 0x31 ...): a unit event |
| 0x18 | u32 hero id, f32 **experience share** (`CPlayerServer::SendMsg18`, sent by `CWorldServer::GiveExperience` 0x5911C0 once per player's first hero on every kill in a session) |
| 0x0E | a path / move confirmation: f32 x, f32 y, u32 unit id, then a walk record (length not decoded) |

Killing a monster: the monster's section 8 action goes 0xFF (and its header flags get 0x80000 when it is created again), then 0x43 removes it. An attack order (client 0x31) is **refused** by the server unless the target is in the attacking hero's known list: `WorldServer::OrderOnTarget` 0x5D3C30 returns without an order unless `AiMemory::IsFriendOrKnown` (0x5CB7F0: same side, same player, or the target is in the unit's seen list) holds; the list is filled by the vision update (`AiMemory::ForceSee` 0x5CBC80, `CAIMap` vision). Measured: orders from 9, 8, 7 and 5 map units were refused, one from about 3 was accepted, and so was any order on a monster that had attacked the hero first. The order is accepted when the hero's own 0x41 update then shows section 10 mode 3 with the target id. Orders on a body or a dead unit never start (what the old "attack again" loop did).

**Experience in a multiplayer session** (`CUnitServer::TakeHit` 0x553F40 -> `GiveExperience`): when a unit dies and its killer belongs to a player, in a session (`Lobby_IsAlive`) the kill's experience (the victim prototype's +0x54) is not given to the killer alone: every player's first hero gets `xp * sqrt(exp_i) / sum_j sqrt(exp_j)` (the weight is sqrt of the stats object's exp total + its accumulator, +0x1E4 / +0x8E0), whoever struck the blow, and each share is announced with a 0x18 message. Outside a session the xp is split equally among the killer's own heroes (message 0x15). Measured on the test rig: 5.0 per boar, host (74.7 million exp) 4.9871, bot (500 exp) 0.0129 = 0.26 %, exactly sqrt(500) / (sqrt(500) + sqrt(74.7 million)). Control run (same day): with the 500 exp character "Useless Chud" as host the same kills split 2.5000 / 2.5000 (two boars: bot +5.0005, host +4.9995), so the host's experience is what changes the bot's share, not the bot's missing database (the amount, 5.0, comes from the victim's prototype row on the server). So the "0 EXP after a kill" is the game's own rule, not a handshake problem: a bot next to a much stronger host levels almost never; next to a host of about its own exp it gets about half.

Client orders, completed from the handlers (2026-10-19; `CPlayerServer::OnMsg_*` / `WorldServer::On*` table at 0x66CB24): 0x30 move (`OnMoveOrder`), 0x31 attack (`OnAttackOrder`), 0x32 / 0x33 cast (above), 0x34 `SetOwnerField270` (u32 unit, u8), 0x35 `SetAttackMode` (u32 unit, u8: the same message um-bot uses as "pace"; it also changes the unit's +0x268), 0x36 interaction order (`OnLootOrder`: loot, talk, use on), 0x38 `OnMsg_Unit4`, 0x39 `SetLeader` (2 unit ids), 0x3A `OnMsg_MoveTo` (count, unit ids, target Vec3, flag: a move with formation offsets), 0x3B (u32 unit, u8): `CUnit::SetField438` (net section 13), 0x40 view point, 0x50 party upload.
The server's unit order kinds (`CUnitServer` +0x190 current / +0x1B4 queued): 1 move, 2 move to a point, 3 attack, 4 cast at, 5 cast on / use, 6 interaction (talk 0x18A95, skills), 9 none, 10 use a consumable quick item (subtype 2, `CompleteOrder`). Server -> client 0x0B (u32 unit id, u32 code) is `CPlayerServer::SendMsg0B`, sent by about 20 order handlers (an order acknowledged or refused; code 0x13 = a cast refused, 0x30 = a body hit by a player, 0x0F ...).
Verified on the live rig: um-bot's `0x33` with `0x80000000 | 0` makes the server run `QueueCastOn` -> `ValidateQueuedOrder` -> `CastSpellMulti` -> `CastSpell` once (breakpoints); the healing effect of the bot's belt potion was NOT observed (the bot's health did not rise after two attempts while being attacked). Open: which id the real client sends for the belt potion (a real UI click sent a `0x33` once, its payload was not captured). Verified 2026-10-20: `0x33` with `0x80000000 | belt index` on the bot itself heals it (32 -> 38 within 0.2 s, the potion is gone after one use). A spell id without bit 31 is an index into the hero's **spell items** (the .mp member's 4th list, `GetItemSpell`: party hero record +0x760): the item there is a spell container whose block holds its spell object's id at +40; the hero's magic skill (Astral Magic for Healing) must reach the spell's level (else player message 0x0B code 0x13) and the mana must cover the cost (spell cost x 25 / Intelligence; else code 0x0F). The first order of a series only switches the hero's stance (`ExecuteCombatOrder` drops it), so it must be sent again ~1.2 s later; the cast then takes ~1.6 s. Verified live: Healing on itself 5 -> 38 in 0.8 s for ~44 of 57 mana; on the host the cast runs (CastOrderSpell / CastSpellMulti hit), its effect on a hurt host not seen. On death the hero's party loses its belt potions, spell containers and spells (they stay on the body) and most of its money (500 -> 125 seen; the worn weapon and armor stay), and the server's 0x83 final save shows the stripped party. A dead hero is not respawned by the server (nothing changed for 140 s); a fresh connection gets a new living hero.

## 5. Open questions (what a complete client or server still needs)

- The payloads of the **unknown** sub-message types in both tables (0x01, 0x02, 0x03-0x07, 0x10, 0x32-0x34,
  0x38-0x3B, 0x50 client-side; 0x02-0x04, 0x06, 0x07, 0x0A, 0x0F, 0x12, 0x13, 0x42, 0x50-0x52, 0x70-0x72,
  0x80, 0x82, 0x83 server-side): the handlers are listed above; each is a decompile away.
- Left of the unit state: the order section of stationary monsters (see above), the stats block's fields (section 4: attributes, skills), sections 5, 6, 12, 13's meaning, a monster's HP (not in the stream: only the hero's is), 
- The world snapshot's layout (0x00 first message) and the tick (0x00 later): the game time, the day.
- Reliable kinds 4, 12, 14, 17; the kind-16 hero blob's serializer (vtable +0x28 of the member class).
- The resend timing (0x4346D0 / 0x433F20), the login's u32 12500 and 20 zero bytes, the master server 0xFF.
- Cast / potion / container orders (0x32-0x3B), the pace field of the host (#46), hostility (#67).

## 6. Tools and captures

`_cpr/claude-re/net/`: probe.py / login.py (handshake), keepalive.py, join5.py (JOIN), enterquest2.py (the
whole entry), unit41.py (the 0x41 decoder), decode.py / clientstream.py (stream dumps), capture.py; captures
cap_lobby2.txt, cap_quest.txt, cap_inquest.txt, cap_remote.pcapng (a real second client), respawn.pcapng.
um-bot (`tools/um-bot/net.hpp`) is the reference implementation of everything marked tested; its research
switches (`UM_BOT_DUMP=<dir>` writes every stream message) record new captures. um-engine.dll
(`resources/universal-mod/um-engine/net_*.cpp`) re-implements the update codec and ReadMessage on the game's
own structures.

## Where a unit is (checked against um.dll's UNITS, 2026-10-21, um-bot 0.1.29)
A replay of captured streams (`research/poscheck.cpp`: every decoded position against the true ones sampled while the capture was taken) showed: the positions from the walk (section 10: the first step, in half-unit cells) always followed the unit (122 of 122 within 1 unit); the position in section 2 (3 floats) is only a SNAPSHOT: monsters lag it by ~2 units and a host moved by a research poke kept its spawn point (23,267) in every snapshot, so believing it made the bot think the host went back; section 1 (3 floats) is no position at all (0.2 / 0.5 on units standing at x = 30..60). um-bot believes a snapshot only for a unit with no walk position of the last 6 s and ignores section 1. Rare garbage still comes from the walk (unit 4651: x = 5899.25, y = 0.25: both codes absolute with a wrong value; the delta state of a unit goes stale when one of its updates did not decode): the bot ignores positions outside 4..1024 and now logs which section they came from.

### A standing player is not where the join says (rig, 2026-10-24, um-bot 0.1.31)
A client that enters a zone gets the section-2 snapshot (`f2`) of every player unit, and for all of them it is the zone's ARRIVAL POINT (23.0, 267.0 in z3xq1), not the unit's place: a host that had walked away and stood still when the bot entered stayed at the arrival point in the bot's idea until it walked again (compared two join dumps with the host at (34.0, 263.0) and (43.8, 257.8): neither coordinate pair occurs anywhere in the stream; the byte patterns for half units / u16 cells are absent too). The same happens to a host that respawned (a new unit, id + 1 000 000). um-bot therefore marks a position `real` only when it came from a walk (section 10), a creation record or an inference, and uses the host's position (leash, help, exit watch, corpse trips) only when it is real.
Fix: **0x39 `SetLeader` (u32 unit, u32 leader) works across players**: with the host as the leader the SERVER walks our hero after it with the host's true position (`Brain::TickFollow`: walks when farther than FollowMaxDist 4, stops inside FollowMinDist 2; it kept within 2-4 units of a walking host and within about 10 of a sprinting one on the rig, from a join 40 units away, and after the host died and respawned once the link was re-sent to the new unit). The bot sends it for the host's unit, and again for a new host unit or its own new hero; "bot stay" clears it (`SetLeader(self, self)`). When the server's follow stops our hero (field 10 mode move -> none) the host is within ~2 units of it: that is taken as the host's position. The 2 s-cadence move orders of 0.1.30 (steps of 6 units, none at all while the host stood still: the bot stood 12 units away for 40 s in the user's log) remain only for the leash and as the fallback (`UM_BOT_NOLEADER=1`).
A fresh unit's pace field (section 6) is 0 until its first stance change (a respawned host walked while it said crawl): the first value 0 of a unit is not believed; a stance change of a standing host is believed (0.1.30 compared it with the speed learned from the previous walk and ignored it: "the host's pace value is 1 (not believed: it moves at 4.39 u/s)" while the host stood).

## Game speed x2 in a multiplayer game (um-engine 0.9.0, `mp_speed.cpp`)

`main` (0x456440) is a fixed-timestep loop with two accumulators: the server one (g_0x79BC64) runs `CWorldServer::v6` (0x58DFF0) once per g_0x78FF34 ms of real time, the client one
(g_0x79BC68) runs `CWorldClient::v6` (0x5641F0) the same way (scaled by a catch-up factor g_0x784758 that stays 1.0 in practice). g_0x78FF34 is 55 at normal speed and 27 at x2 (tick
counter g_0x7AF61C 18.4 / s, 37 / s). The speed buttons and keys (CBattleBar::OnPauseButton 0x6490A0 = x2, OnNormalSpeedButton 0x649100, CBattleBar2::OnLButtonUp 0x648D70 cases 1 / 2,
CUIScreen740730::OnKeyDown 0x63A560) write it only when `Lobby_IsAlive()` (0x57F440) is false, i.e. never in multiplayer.

Tested with two real game.exe (rig): the value set on the host alone doubles the host's server ticks (18.5 -> 36.9 / s) but the clients keep 18.2 / s and their heroes walk at 1.87
units / s; set on the host and on the client both counters run at 37 / s in step (418 / 417 in 11 s) and the client's hero walks at 3.79 units / s.

So um-engine (keys `MpSpeed::HostButtons`, `MpSpeed::Sync` in um-engine.cfg; both on by default, nothing happens until the host presses x2): the six speed gates ask a function that answers
"no lobby" for the host (the one that has a lobby server object, g_0x79B5DC); once a frame (the first `Net_TickAll` call of `main`) the host compares g_0x78FF34 with the last value it sent and
sends every player of its world the named value `um.speed` (2 = x2, 0 = normal; message 0x81 through `CPlayerServer::SendMsg81_NamedValue` 0x66EE10, the one that carries "gtime"; repeated
every second while x2 so a late joiner follows; the host's own copy in the player's variables is removed again); the client's 0x81 handler (`WorldClient::OnQuestString` 0x669850, the
`StringFloatMap_Set` call at 0x6698DF) takes `um.speed` out (not stored in the character) and the client keeps g_0x78FF34 at the host's value every frame (the game resets it to 55 when a
screen closes or a zone loads), back to 55 when the host says normal or the game is left. A player without um-engine does not follow: all players need it before the host uses x2.
Only the players of the host's current world get the message: the bases are separate worlds (a player in the base is not told), the speed buttons only exist in a zone.

## Ghost units: the other players in your base (um-engine 0.10.0, `mp_ghost.cpp`, key `MpGhost`; PENDING #145)

Every player's base is a world of its own, so another player never has a unit in yours. um-engine shows each of them as a **ghost**: a hero unit that exists only in the local world, made through the game's own
unit creation handler, fed by a hidden chat line. Verified in the rig with two real game.exe (a host and a client in their bases): each sees the other's hero walk about with its armour and weapon, the ghost
follows the walk, goes when the player enters a quest ("On map") and nothing crashes in a 3 minute stress walk; clicking a ghost does nothing.

**Transport.** The lobby's chat is the only channel every player has in a base: a line typed by a client goes to the server (`LobbyClient::AddKeyedRef2604`, as `CUIMessageLog::EditOnKeyDown` does) and comes back to everybody
as a system line (`LobbyClient::OnAddressLine` 0x41E5D0 -> `ChatSystemLine` 0x5F82F0). Sending: a `ChatMessage` (`ctor2` 0x4171A0, text set with `ChatMessage::Read` 0x413B50) handed to `AddKeyedRef2604(client, msg, 60.0f)`. Receiving: the
call of ChatSystemLine at 0x41E656 is replaced; a line containing `um!g|` is taken out (not shown) and parsed. Lines (one marker `um!g|`, then a letter): `A|<session>|<name>|proto|figure|c0|c1|c2|weaponSlot|<11 item codes in hex>` the appearance (weapons 4 then armour 7 = the client hero's senses +0xBC..+0xC8 and +0xA0..+0xB8), sent when it changes and every 8 s;
`P|<session>|x|y|vx|vy|yaw|animCurrent|animQueued|pack|pack2` the motion (~60 characters), sent only when the walk differs from what the others are already following (dead reckoning: the sender measures its velocity, the ghost goes on along
the last announced velocity and eases to it; a click-walk costs a start line and a stop line) and every 5 s as a heartbeat; `Q|<session>` "send me your appearance" (a motion line came from somebody whose looks are unknown: everybody answers with `A`);
`bye|<session>` when a player leaves the base. A ghost with no news for 15 s goes. Measured with 3 players clicking a new walk every 1.5 s each: 3.9 lines a second reach each machine (7.8 when positions were streamed 3 times a second); a normal
player costs about 0.3 lines a second. Real chat keeps working under that traffic (checked: a typed line reaches both clients at once; the marker lines are never shown). Players without the DLL see the lines in their chat and no ghosts (all players need
um-engine; a new lobby message kind instead would make the game drop a client that has no factory for it, see `UmNetReadMessage`). Only in a multiplayer base (lobby alive, no quest, the local hero 1000011214 present).

**Making a ghost** (`SpawnGhost`): run `WorldClient::OnUnitCreated` (0x667EC0) on our own bytes: its `this` is the `CPlayerClient` = `[[0x7AF5B8 + 0x20C] + 0x20]`; the embedded input stream (`+0x34`) gets a copy of its vtable whose slot 1
(Read) reads our buffer, and the byte at `+0x39` is set to 1 for the call (the handler stops at once while it is 0). The record is the 0x40 creation record of "Unit creation" above with kind 0x32, a free id (1000000100 + n), the
sender's proto row, figure, complexion, part names (hp bd hd hr lh1-3 rh1-3 ll1-3 rl1-3), name, and the net state: u16 section mask 0x0FE4 (sections 2, 5, 6, 7, 8, 9, 10, 11), every value delta coded as "changed"
(`DeltaWriter`: a bit byte is taken when its 8 bits are used and each section starts a new one; a wrong layout shows as bytes left over or read beyond the end of the buffer). Section 2 position + rotation; **5 the equipment as item
codes** (row << 16 | material: 4 weapons, 7 armour pieces; they give the skin layers, the armour meshes and the weapon in hand: the ghost looks like its player); 6 stance and weapon slot; 8 animation (0xFE idle); 9 the name; **10 the order**:
mode 0 with one step (without it the unit is put back on the origin every frame by `CUnitClientSpecific::AdvanceTime`; the unit then stands at 2 * v + 48 for the order's point v, so v = (x - 48) / 2); 11 hostile mask / side.
**Removing:** `CPlayerClient::OnMsg_DetachUnit` (0x668850, a u32 id) through the same stream.

**Moving it:** the unit's client object (`senses`, obj + 0x240) holds the position in half units as two floats at +0x3C / +0x40 (write 2x, 2y: the object follows in a frame, z from the terrain), the rotation quaternion is at obj + 0x34, and
the animation bytes at obj + 0x254 (current) / 0x255 (queued) with the start tick at +0x258 and the packed stance fields at +0x25C / +0x260: copying the hero's own values (idle 0xFF / 0xFE, walk 0x26 / 0x68, run 0x23 / 0x72
with the crossbow) gives the ghost its walking pose. The game's own update message (0x41 handler 0x6687B0) was tried first with the same writer: sections 2 and 10 alone read right, section 8 reads 4 bytes beyond: not understood, not needed.

**Hardening (tested 2026-10-23).** A line comes from another machine and its values go into the game's own lookups, which crash on an index they do not have, so the receiver refuses a whole line unless: the unit-table row is below
the table size (578 in the mod), every item code is empty (0xFFFFFFFF) or its row is below the weapon table (64, slots 1-4) or armour table (192, slots 5-11) size and its material below the material table size (46), the figure is `unhuma`, `unhufe`
or the local hero's own, the name is printable and 1-40 characters, complexion in 0..1, weapon slot -1..3, the position inside the map (terrain + 0x10C / 0x110 are floats: 128.0), finite speed / yaw, stance fields 0..8, and at most
16 other players are shown (a flood of invented sessions makes no crowd). Ghost ids start at **2 100 000 000**: `IsHeroId` (0x4613A0) is true for 1e9..2e9, so ids in that range are taken for player heroes (hero list, party lists). Tests (a build with
`-DUM_GHOST_FUZZ` that feeds lines to the receiver): 600 random / garbage / out-of-range lines and 51 boundary cases (the last valid index accepted, the first invalid refused, all as predicted) with 50 ghosts alive: no crash;
every animation code 0..255 written into a ghost: no crash; the craftsman's shop screen and its tabs opened with ghosts around: fine; a player's game killed without a goodbye: its ghost goes after 15 s and the others keep running.
Not tested: saving with ghosts present, swapping items with a player in the base, other mods' databases (a different table size makes the receiver refuse the line, so such a player shows no ghost).

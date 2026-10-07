// The game's multiplayer transport objects, as game.exe lays them out (the Russian EIStarter build).
// Every struct here mirrors memory the game owns: um-engine code reads and writes these objects in place,
// so the offsets must stay exact (static_asserts below). RE notes: _cpr/claude-re/net/engine/README.md.
#pragma once
#include <winsock2.h>

#include <cstddef>
#include <cstdint>

// ---- GameBuffer (vtable 0x73D624): a growable byte stream, the base of every packet / stream buffer ----
struct GameBuffer {
    void** vtable;      // [1] grow(bytes) (+0x04)  [4] write(data, len) (+0x10)  [6] seek(offset from data) (+0x18)
    uint8_t* data;      // +0x04 start of the bytes
    uint8_t* write;     // +0x08 write position (also "read" for a received datagram)
    uint8_t* capacity;  // +0x0C end of the allocated space
    uint8_t* end;       // +0x10 end of the content (high-water mark)
    uintptr_t bias;     // +0x14 an offset bias, 0 in every buffer seen: the content size is bias - data + end
    uint32_t flags;     // +0x18 bit 0 = written, bit 3 = zero-filled
};
typedef void(__attribute__((thiscall)) * GameBufferWriteFn)(GameBuffer*, const void* data, uint32_t len);
typedef void(__attribute__((thiscall)) * GameBufferSeekFn)(GameBuffer*, uint32_t offsetFromData);
inline void GameBufferWrite(GameBuffer* b, const void* data, uint32_t len) { reinterpret_cast<GameBufferWriteFn>(b->vtable[4])(b, data, len); }
inline void GameBufferSeek(GameBuffer* b, uint32_t offset) { reinterpret_cast<GameBufferSeekFn>(b->vtable[6])(b, offset); }

// ---- MSVC 6 std::list node (sentinel-based): [next][prev][value...] ----
struct ListNode {
    ListNode* next;
    ListNode* prev;
};
struct ListHead { ListNode* sentinel; };  // the list object itself holds a pointer to its sentinel node

// A stream chunk as queued in the connection's chunk lists (0x10C-byte node, allocated by 0x43A950)
struct StreamChunk {
    ListNode* next;
    ListNode* prev;
    uint16_t offset;  // +0x08 the chunk's position in the byte stream
    uint8_t length;   // +0x0A up to 255 bytes
    uint8_t data[0x101];  // +0x0B
};

// A sent update awaiting its ack (list this+0x10F8); the record at +8 is handled by the game's own code
struct SentPacket {
    ListNode* next;
    ListNode* prev;
    uint8_t record[8];  // +0x08 the packet record (0x43A1D0 destroys it); +0x10 = u16 seq (below)
    uint16_t seq;       // +0x10
};

// ---- NetConnection (vtable 0x73BF5C, servers 0x73BB70/0x73BBB4, clients 0x73BDEC/0x73BE30) ----
// Only the fields the re-implemented functions touch are named; the rest stays opaque padding.
struct NetConnection {
    void** vtable;              // +0x0000
    uint32_t unknown04;         // +0x0004
    uint16_t nextSendSeq;       // +0x0008 the next outgoing update's sequence number
    uint16_t pad0A;
    uint32_t lastReceivedSeq;   // +0x000C highest received sequence (unwrapped: + wrapBase)
    uint32_t wrapBase;          // +0x0010 added to the 16-bit sequence read from the wire (grows by 0x10000)
    uint32_t* recentBegin;      // +0x0014 vector<u32> of the sequences received inside the 32-wide window
    uint32_t* recentEnd;        // +0x0018
    uint32_t* recentCapacity;   // +0x001C
    uint8_t pad20[0x0C];        // +0x0020 (double at +0x20: time)
    sockaddr_in peer;           // +0x002C the other side's address
    uint8_t pad3C[0x10F8 - 0x3C];
    ListHead sentPackets;       // +0x10F8 updates sent, waiting for their ack (SentPacket nodes)
    ListHead sentPackets2;      // +0x10FC a second list searched by AckOne (resends?)
    uint8_t pad1100[0x1118 - 0x1100];
    uint16_t streamSendOffset;  // +0x1118 the outgoing byte stream's next offset
    uint16_t streamExpected;    // +0x111A the incoming byte stream's next expected offset
    ListHead outChunks;         // +0x111C outgoing chunks pending (resend)
    ListHead inChunks;          // +0x1120 incoming chunks received out of order (StreamChunk nodes)
    uint32_t ackMask;           // +0x1124 bits of the sequences before lastAckSeq already acknowledged
    uint16_t lastAckSeq;        // +0x1128 the last ack sequence received
    uint16_t pad112A;
    GameBuffer outStream;       // +0x112C the outgoing byte stream
    GameBuffer inStream;        // +0x1148 the incoming byte stream (the chunks land here in order)
};
static_assert(offsetof(NetConnection, lastReceivedSeq) == 0x0C, "layout");
static_assert(offsetof(NetConnection, peer) == 0x2C, "layout");
static_assert(offsetof(NetConnection, sentPackets) == 0x10F8, "layout");
static_assert(offsetof(NetConnection, streamSendOffset) == 0x1118, "layout");
static_assert(offsetof(NetConnection, inChunks) == 0x1120, "layout");
static_assert(offsetof(NetConnection, ackMask) == 0x1124, "layout");
static_assert(offsetof(NetConnection, outStream) == 0x112C, "layout");
static_assert(offsetof(NetConnection, inStream) == 0x1148, "layout");
static_assert(offsetof(StreamChunk, offset) == 8 && offsetof(StreamChunk, data) == 0x0B && sizeof(StreamChunk) == 0x10C, "layout");

// ---- game.exe globals and helpers the transport uses ----
namespace game {
// The net log: an ostream pointer, NULL unless the console enabled "net server/client 1".
inline void* NetLog() { return *reinterpret_cast<void**>(0x0079B91C); }
// The connection whose update is being parsed (read by the message handlers), 0x79B924.
inline NetConnection*& ParsingConnection() { return *reinterpret_cast<NetConnection**>(0x0079B924); }

typedef void*(__attribute__((thiscall)) * OstreamTextFn)(void* stream, const char* text);
typedef void*(__attribute__((thiscall)) * OstreamIntFn)(void* stream, int value);
typedef void*(__attribute__((thiscall)) * OstreamCharFn)(void* stream, int c);
typedef void*(__attribute__((thiscall)) * OstreamApplyFn)(void* stream, void* manipulator);
// Writes one line to the net log exactly as the game does: text << number? << '\n' << flush.
inline void NetLogLine(const char* text, const char* text2 = nullptr, bool withNumber = false, int number = 0) {
    void* s = NetLog();
    if (!s) return;
    s = reinterpret_cast<OstreamTextFn>(0x006FC31D)(s, text);
    if (text2) s = reinterpret_cast<OstreamTextFn>(0x006FC31D)(s, text2);
    if (withNumber) s = reinterpret_cast<OstreamIntFn>(0x006FC14E)(s, number);
    s = reinterpret_cast<OstreamCharFn>(0x006FC0B0)(s, '\n');
    reinterpret_cast<OstreamApplyFn>(0x00434FE0)(s, reinterpret_cast<void*>(0x00435000));
}

// vector<u32>::insert(where, &value, 1) (0x43B850): keeps the game's allocator for the vector's storage
typedef void(__attribute__((thiscall)) * VectorInsertFn)(void* vector, uint32_t* where, const uint32_t* value, uint32_t count);
inline void RecentSeqPush(NetConnection* c, uint32_t seq) {
    if (c->recentEnd != c->recentCapacity) { *c->recentEnd++ = seq; return; }
    reinterpret_cast<VectorInsertFn>(0x0043B850)(&c->recentBegin, c->recentEnd, &seq, 1);
}
// list<StreamChunk>::erase (0x438660): unlinks, frees the node, returns the next node
typedef ListNode**(__attribute__((thiscall)) * ListEraseFn)(ListHead* list, ListNode** result, ListNode* node);
inline ListNode* ChunkErase(ListHead* list, ListNode* node) {
    ListNode* next = nullptr;
    reinterpret_cast<ListEraseFn>(0x00438660)(list, &next, node);
    return next;
}
// A new StreamChunk node from a chunk value (0x43A950: operator new(0x10C) + the 0x104-byte copy)
typedef StreamChunk*(__attribute__((stdcall))* ChunkNodeNewFn)(const void* value);  // ret 4
inline StreamChunk* ChunkNodeNew(const void* value) { return reinterpret_cast<ChunkNodeNewFn>(0x0043A950)(value); }
// list<SentPacket>::erase (0x437F30): destroys the record (0x43A1D0), frees the node, returns the next node
inline void SentPacketErase(ListHead* list, ListNode* node) {
    ListNode* next = nullptr;
    reinterpret_cast<ListEraseFn>(0x00437F30)(list, &next, node);
}
// 0x433FA0: a sent update was acknowledged: round-trip statistics + the replicated objects' acked versions
typedef void(__attribute__((thiscall)) * PacketAckedFn)(NetConnection*, void* record);
inline void PacketAcked(NetConnection* c, void* record) { reinterpret_cast<PacketAckedFn>(0x00433FA0)(c, record); }
}  // namespace game

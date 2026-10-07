// The transport's update packets (type 0): sequence numbers, acknowledgements and the byte stream chunks,
// re-implemented from game.exe (RE: _cpr/claude-re/net/engine/README.md, "Update packet codec").
// Wire format of an update: u8 0, u16 seq, u16 ack, u32 ackBits, u16 chunkOffset, u8 chunkLen, chunk bytes,
// u8 messageCount, the reliable messages (flag byte before every 8, then u8 kind, u16 id, payload).
#include <winsock2.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "net_types.hpp"

// 0x434C20 NetConnection::WriteAcks(uint8_t** cursor): writes u16 lastReceivedSeq and the 32-bit ack history
// (bit i-1 = "the update lastReceivedSeq - i was received"), then forgets the sequences that left the window.
extern "C" void __attribute__((thiscall)) UmNetWriteAcks(NetConnection* self, uint8_t** cursor) {
    const uint32_t last = self->lastReceivedSeq;
    uint32_t bits = 0;
    uint32_t probe = last;
    for (int i = 1; i <= 32; ++i) {
        --probe;
        for (const uint32_t* p = self->recentBegin; p != self->recentEnd; ++p)
            if (*p == probe) { bits |= 1u << (i - 1); break; }
    }
    uint8_t* out = *cursor;
    const uint16_t last16 = static_cast<uint16_t>(last);
    std::memcpy(out, &last16, 2);
    std::memcpy(out + 2, &bits, 4);
    *cursor = out + 6;
    // prune, from the back, the sequences older than the window (the game's exact test: seq + 0x20 < last)
    for (int idx = static_cast<int>(self->recentEnd - self->recentBegin) - 1; idx >= 0; --idx) {
        if (self->recentBegin[idx] + 0x20 >= last) continue;
        uint32_t* p = self->recentBegin + idx;
        for (uint32_t* q = p + 1; q != self->recentEnd; ++q) *(q - 1) = *q;
        --self->recentEnd;
    }
}

// 0x434CE0 NetConnection::AckOne(u16 seq): the sent update `seq` was acknowledged: statistics, then it leaves
// the waiting lists (the first match in sentPackets, else in sentPackets2).
extern "C" void __attribute__((thiscall)) UmNetAckOne(NetConnection* self, uint16_t seq) {
    ListHead* lists[2] = {&self->sentPackets, &self->sentPackets2};
    for (ListHead* list : lists) {
        for (ListNode* n = list->sentinel->next; n != list->sentinel; n = n->next) {
            SentPacket* packet = reinterpret_cast<SentPacket*>(n);
            if (packet->seq != seq) continue;
            game::PacketAcked(self, packet->record);
            game::SentPacketErase(list, n);
            return;
        }
    }
}

// 0x434D70 NetConnection::ReadAcks(uint8_t** cursor): reads u16 ackSeq + u32 ackBits and acknowledges every
// sent update not acknowledged before (ackMask remembers the bits already seen, relative to lastAckSeq).
extern "C" void __attribute__((thiscall)) UmNetReadAcks(NetConnection* self, uint8_t** cursor) {
    uint16_t ackSeq;
    uint32_t ackBits;
    std::memcpy(&ackSeq, *cursor, 2);
    std::memcpy(&ackBits, *cursor + 2, 4);
    *cursor += 6;
    int diff = static_cast<uint16_t>(ackSeq - self->lastAckSeq);
    if (diff > 0x8000) diff -= 0x10000;
    uint32_t mask = self->ackMask;
    if (diff > 0) {
        mask = (mask << 1) | 1;
        --diff;
        mask = diff < 32 ? mask << diff : 0;
    } else if (diff >= -32) {
        mask >>= static_cast<unsigned>(-diff) & 31;  // x86 shifts by cl & 31: -32 shifts by 0, as in the game
    } else {
        mask = 0;
    }
    const uint32_t fresh = ackBits & ~mask;
    self->lastAckSeq = ackSeq;
    self->ackMask = mask | fresh;
    UmNetAckOne(self, ackSeq);
    for (int i = 1; i <= 32; ++i)
        if (fresh & (1u << (i - 1))) UmNetAckOne(self, static_cast<uint16_t>(ackSeq - i));
}

// 0x434E50 NetConnection::AcceptSeq(u32 seq): true when the received update is new (inside the 32-wide
// window and not a duplicate); it becomes the highest received sequence when it is.
extern "C" bool __attribute__((thiscall)) UmNetAcceptSeq(NetConnection* self, uint32_t seq) {
    if (seq + 0x20 < self->lastReceivedSeq) {
        game::NetLogLine("SKIP too old update, dif=", nullptr, true, static_cast<int>(self->lastReceivedSeq - seq));
        return false;
    }
    if (self->lastReceivedSeq <= seq) self->lastReceivedSeq = seq;
    for (const uint32_t* p = self->recentBegin; p != self->recentEnd; ++p) {
        if (*p != seq) continue;
        if (game::NetLog()) {
            char address[64];
            const uint8_t* ip = reinterpret_cast<const uint8_t*>(&self->peer.sin_addr);
            std::snprintf(address, sizeof address, "%i.%i.%i.%i:%i", ip[0], ip[1], ip[2], ip[3], ntohs(self->peer.sin_port));
            game::NetLogLine("DUPLICATE update received from ", address);
        }
        return false;
    }
    game::RecentSeqPush(self, seq);
    return true;
}

// 0x434AE0 NetConnection::DrainChunks(): moves every queued chunk whose offset is the next expected one into
// the incoming byte stream, in order, until none fits.
extern "C" void __attribute__((thiscall)) UmNetDrainChunks(NetConnection* self) {
    ListHead* list = &self->inChunks;
    for (bool progressed = true; progressed;) {
        progressed = false;
        for (ListNode* n = list->sentinel->next; n != list->sentinel;) {
            StreamChunk* chunk = reinterpret_cast<StreamChunk*>(n);
            if (chunk->offset != self->streamExpected) { n = n->next; continue; }
            GameBuffer* b = &self->inStream;
            if (b->write > b->end) b->end = b->write;
            GameBufferSeek(b, static_cast<uint32_t>(b->bias - reinterpret_cast<uintptr_t>(b->data) + reinterpret_cast<uintptr_t>(b->end)));
            const uint32_t len = chunk->length;
            if (b->write + len <= b->capacity) {
                b->flags |= 1;
                std::memcpy(b->write, chunk->data, len);
                b->write += len;
            } else {
                GameBufferWrite(b, chunk->data, len);
            }
            self->streamExpected = static_cast<uint16_t>(self->streamExpected + len);
            n = game::ChunkErase(list, n);
            progressed = true;
        }
    }
}

// 0x436470 NetConnection::ParseUpdate(uint8_t** cursor): one received update after its type byte. False when
// a reliable message could not be read (the caller drops the client).
typedef bool(__attribute__((thiscall)) * ReadMessageFn)(NetConnection*, uint8_t** cursor, uint32_t wrapBase);
extern "C" bool __attribute__((thiscall)) UmNetParseUpdate(NetConnection* self, uint8_t** cursor) {
    game::ParsingConnection() = self;
    *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(self) + 0x44) = 0;
    uint16_t seq16;
    std::memcpy(&seq16, *cursor, 2);
    *cursor += 2;
    uint32_t seq = seq16 + self->wrapBase;
    if (self->lastReceivedSeq == 0) self->lastReceivedSeq = seq;
    const int32_t distance = static_cast<int32_t>(seq - self->lastReceivedSeq);
    if (distance > 0x8000) {
        seq -= 0x10000;
    } else if (distance < -0x8000) {
        seq += 0x10000;
        self->wrapBase += 0x10000;
    }
    if (!UmNetAcceptSeq(self, seq)) { game::ParsingConnection() = nullptr; return true; }
    UmNetReadAcks(self, cursor);
    // the stream chunk: u16 offset, u8 length, the bytes
    StreamChunk value;
    std::memcpy(&value.offset, *cursor, 2);
    value.length = (*cursor)[2];
    *cursor += 3;
    std::memcpy(value.data, *cursor, value.length);
    *cursor += value.length;
    if (static_cast<uint16_t>(value.offset - self->streamExpected) <= 0x8000 && value.length != 0) {
        ListNode* node = reinterpret_cast<ListNode*>(game::ChunkNodeNew(&value.offset));  // push_front
        ListNode* sentinel = self->inChunks.sentinel;
        node->next = sentinel->next;
        node->prev = sentinel;
        sentinel->next->prev = node;
        sentinel->next = node;
        UmNetDrainChunks(self);
    }
    const uint8_t count = **cursor;
    *cursor += 1;
    for (uint8_t i = 0; i < count; ++i) {
        if (!reinterpret_cast<ReadMessageFn>(0x00435F30)(self, cursor, self->wrapBase)) {
            game::ParsingConnection() = nullptr;
            return false;
        }
    }
    game::ParsingConnection() = nullptr;
    return true;
}

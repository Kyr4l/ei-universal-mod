// The reliable messages of an update: each one creates, updates or removes a replicated object of the
// connection's object table (RE: _cpr/claude-re/net/engine/README.md, "Object table").
#include <winsock2.h>

#include <cstdint>
#include <cstring>

#include "net_types.hpp"

extern "C" void UmEngineLog(const char* format, ...);  // um_engine.cpp

// A table entry for `key` with no object yet: inserted before the hint given by lower_bound (as the game does).
static ObjectEntry* NewEntry(NetConnection* self, uint32_t key, uint32_t version) {
    MapHead* map = ObjectMap(self);
    ObjectEntry* hint = game::ObjectLowerBound(map, key);
    ObjectEntry* entry = hint;
    if (hint == map->header || static_cast<int32_t>(key) < static_cast<int32_t>(hint->key)) {
        const ObjectPair pair = {key, nullptr, 0};
        entry = game::ObjectInsert(map, hint, pair);
    }
    entry->version = version;
    return entry;
}

// 0x435F30 NetConnection::ReadMessage(reader, packetSeq): one reliable message. False = the payload could not be
// read (no factory for that kind): the caller drops the client.
// `self` is the LINK (one per peer: its object table +0x1100, id map +0x110C, 0x4352F0's `this`); the callback
// lists, like the factory, live on the serviced connection [0x79B920] (the asm reads all three from there).
extern "C" bool __attribute__((thiscall)) UmNetReadMessage(NetConnection* self, NetReader* reader, uint32_t packetSeq) {
    NetConnection* owner = game::ServicedConnection();
    if (reader->bitsLeft == 0) {
        reader->bitBuffer = *reader->cursor++;
        reader->bitsLeft = 8;
    }
    const bool present = (reader->bitBuffer & 1) != 0;
    reader->bitBuffer >>= 1;
    --reader->bitsLeft;
    uint8_t kind = 0;
    uint16_t id = 0;
    game::ReaderRead(reader, &kind, 1);
    game::ReaderRead(reader, &id, 2);
    const uint32_t key = (static_cast<uint32_t>(kind) << 16) | id;
    MapHead* map = ObjectMap(self);
    ObjectEntry* entry = game::ObjectFind(map, key);
    const bool unknown = entry == map->header;
    UmEngineLog("ReadMessage conn=%p serviced=%p seq=%u kind=%u id=%u present=%d unknown=%d entry=%p version=%u object=%p",
                static_cast<void*>(self), static_cast<void*>(game::ServicedConnection()), packetSeq, kind, id, present, unknown,
                static_cast<void*>(entry), unknown ? 0u : entry->version, unknown ? nullptr : static_cast<void*>(entry->object));

    if (!present) {  // the object is removed
        if (unknown) {
            NewEntry(self, key, packetSeq);  // a tombstone: the removal outranks older updates still in flight
        } else if (entry->version < packetSeq) {
            if (entry->object) {
                game::ObjectUnregisterId(self, entry->object);
                game::RunCallbacks(CallbacksRemoved(owner), kind, entry->object);
            }
            if (entry->object) {
                game::PtrRelease(&entry->object);
                game::PtrSet(&entry->object, nullptr);
            }
            entry->version = packetSeq;
        } else {
            game::NetLogLine("SKIP remove, newer version of object is already here");
        }
        return true;
    }

    if (unknown) {  // created
        entry = NewEntry(self, key, packetSeq);
        NetObject* object = game::ObjectCreate(kind);
        UmEngineLog("  created: entry=%p key=%X object=%p", static_cast<void*>(entry), entry->key, static_cast<void*>(object));
        if (!object) return false;
        game::PtrAssign(&entry->object, object);
        entry->version = 0;
        game::ObjectRegisterId(self, object, kind, key);
        game::ObjectRead(object, reader);
        entry->version = packetSeq;
        const CallbackList& l = CallbacksCreated(owner)[kind];
        UmEngineLog("  read ok: held=%p refs=%u flags=%02X %02X; created callbacks: %d", static_cast<void*>(entry->object), entry->object ? entry->object->refCount : 0u,
                    entry->object ? entry->object->flags06 : 0u, entry->object ? entry->object->flags07 : 0u, static_cast<int>(l.end - l.begin));
        game::RunCallbacks(CallbacksCreated(owner), kind, object);
        return true;
    }
    if (entry->version < packetSeq) {  // updated (or created when the entry had no object yet)
        if (!entry->object) {
            NetObject* object = game::ObjectCreate(kind);
            if (object) {
                game::PtrAssign(&entry->object, object);
                entry->version = 0;
                game::ObjectRegisterId(self, object, kind, key);
            }
            if (object != entry->object) {
                game::PtrRelease(&entry->object);
                game::PtrSet(&entry->object, object);
            }
            if (!entry->object) return false;
        }
        NetObject* object = entry->object;
        game::ObjectRead(object, reader);
        game::RunCallbacks(entry->version == 0 ? CallbacksCreated(owner) : CallbacksUpdated(owner), kind, object);
        entry->version = packetSeq;
        return true;
    }
    // an older packet: skip it, but its payload must still be consumed: a throwaway object reads it
    game::NetLogLine("SKIP update, newer version of object is already here");
    NetObject* scratch = nullptr;
    game::PtrSet(&scratch, game::ObjectCreate(kind));
    game::ObjectRead(scratch, reader);
    game::PtrRelease(&scratch);
    return true;
}

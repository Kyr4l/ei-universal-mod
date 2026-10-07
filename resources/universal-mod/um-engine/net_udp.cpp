// The game's UDP socket layer (multiplayer, docs: PENDING #78): the two methods of its socket object that send and
// receive one datagram. Both are __thiscall: ECX = the socket object (its first field is the SOCKET), then
// (address, buffer) on the stack, popped by the callee (ret 8).
#include <winsock2.h>

#include <cstdint>
#include <cstring>

#include "net_types.hpp"

typedef void(__attribute__((thiscall)) * BufferOverflowFn)(GameBuffer*, int);
static const BufferOverflowFn kBufferOverflow = reinterpret_cast<BufferOverflowFn>(0x004DA5C0);

// 0x440B90: sends the buffer's content to `to`; true when all of it went out.
extern "C" bool __attribute__((thiscall)) UmNetSendTo(SOCKET* self, sockaddr* to, GameBuffer* buffer) {
    if (buffer->end < buffer->write) buffer->end = buffer->write;
    const int size = static_cast<int>(buffer->bias - reinterpret_cast<uintptr_t>(buffer->data) + reinterpret_cast<uintptr_t>(buffer->end));
    const int sent = sendto(*self, reinterpret_cast<const char*>(buffer->data), size, 0, to, 0x10);
    return sent == size;
}

// 0x440BE0: receives one datagram (up to 2048 bytes) into the buffer, the sender into `from`.
extern "C" bool __attribute__((thiscall)) UmNetReceiveFrom(SOCKET* self, sockaddr_in* from, GameBuffer* buffer) {
    GameBufferSeek(buffer, 0x800);
    int fromLength = 0x10;
    const int got = recvfrom(*self, reinterpret_cast<char*>(buffer->data), 0x800, 0, reinterpret_cast<sockaddr*>(from), &fromLength);
    if (got >= 0) {
        from->sin_family = AF_INET;
        std::memset(from->sin_zero, 0, 8);
        buffer->write = buffer->data;
        buffer->end = buffer->data + static_cast<uint32_t>(got);
        if (buffer->end > buffer->capacity) kBufferOverflow(buffer, got);
    }
    return got >= 0;
}

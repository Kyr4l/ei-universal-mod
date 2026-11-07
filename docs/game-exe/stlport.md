# STLport in game.exe (MSVC 6.0, STLport 4.x; RTTI names `.?AV...@stlport@@`) — layouts to recognise in phase 1

game.exe links STLport statically, and MFC 6 too (0x6E0000..0x70E000: CString, CArchive, CMemFile, CDC / CGdiObject wrappers,
CWnd, CWinThread / AfxBeginThread, AfxMessageBox, the thread / module state code). No CWinApp is ever constructed (CWinApp::CWinApp
0x70D654 has no caller) and no CArchive either (CArchive::CArchive 0x7086C0 is unused) [V, T7 lib05]. The streams are NOT
STLport: the 8 log streams (g_0x7C55A8..g_0x7C5810, LogStream vt 0x740034) and the net log g_0x79B91C are the classic Microsoft
iostream `ostream` over a `streambuf` subclass (ios 0x50 bytes: bp +4, state +8, x_tie +0x20, x_flags +0x24, precision +0x28, fill
+0x2C, width +0x30, lock +0x34 / +0x38; streambuf 0x4C bytes: base +0x10 .. egptr +0x2C, lock +0x30 / +0x34; ctors / operators
0x6FC0B0..0x6FD013) [V, T7 lib03]. Every container
below appears inline in classes, so its layout IS the class layout at that offset. Sizes are for x86-32.

| STLport 4.x type | size | layout (offsets from the member) |
|---|---|---|
| `string` (basic_string<char>) | 12 | +0 `_M_start` (char*), +4 `_M_finish` (char*, points at the NUL), +8 `_M_end_of_storage` (char*). No short-string buffer in 4.x by default. Empty string: start==finish, 1 byte allocated. |
| `vector<T>` | 12 | +0 `_M_start`, +4 `_M_finish`, +8 `_M_end_of_storage`. size = (finish-start)/sizeof(T). Growth (`_M_insert_overflow`, e.g. 0x408060): new capacity = size + max(size, n) [V]. |
| `list<T>` | 4 | +0 `_M_node` -> sentinel node. Node: +0 next, +4 prev, +8 data. Empty: node->next == node. |
| `slist<T>` | 4 | +0 head node ptr (`_M_head._M_next`). |
| `deque<T>` | 32 | +0 map (T**), +4 map_size, +8 start iterator {cur, first, last, node} (16), +24 finish iterator (16). (Total 40 with the iterators: check per use.) |
| `map` / `set` / `multimap` (`_Rb_tree`) | 8 | +0 `_M_header` -> header node (color, parent=root, left=leftmost, right=rightmost; value unused), +4 `_M_node_count`. Node: +0 color (int), +4 parent, +8 left, +12 right, +16 value (key, then mapped). |
| `hash_map` / `hash_set` (`hashtable`) | 20 | +0 hasher/equal (empty, 0 or 1 bytes padded), then `_M_buckets` vector (12: start, finish, end_of_storage), `_M_num_elements` (4). Node: +0 next, +4 value. |
| `pair<K,V>` | — | K then V, natural alignment. |
| `allocator<T>` | 0/1 | empty; the node allocator is a static class (below). |

**The node allocator (`__node_alloc`, used by every container for blocks <= 128 bytes):**
- `_S_chunk_alloc(size, &nobjs)` = **0x406B70** (408 raw callers: `_S_refill` is inlined into most `allocate` instantiations; the out-of-line `_S_refill` is
  `PoolAlloc_Refill` 0x4060E0, the out-of-line `allocate` is `PoolAlloc` 0x405950) [V].
- Fixed node pools seen inline: free list 0x79B228 = 16-byte nodes (hashtable / list<dword> nodes), 0x79B22C = 24-byte nodes (the
  archive section list); `__stl_prime_list` 0x73B914 (28 primes, 0xFFFFFFFB past the end), used by `_M_next_size` 0x40AE00 and every
  `resize` / `_M_initialize_buckets` [V].
- The acks.db records are STLport template instances in 0x6E0000..0x6E6000: `HashSet_CopyFrom_6E00F0` = hashtable::_M_copy_from,
  `HashSet_Assign_6E14E0` = hashtable::operator=, and per record type (AcksAnswer 0x1C4, AcksCry 0x5C, AcksOther 0x38, AcksLine 0x28 =
  {weight, 3 strings}, AcksLine2C 0x2C) the std::copy (forward), copy_backward, uninitialized_copy / fill_n and the vector operator= /
  resize / erase / _M_fill_insert / _M_insert_overflow; the old *_CopyBackward names were forward copies [V, T7 lib02].
- Globals: `_S_free_list[16]` at **0x79B224** (lists for 8, 16, ..., 128 bytes), `_S_start_free` **0x79B268**, `_S_end_free` **0x79B26C**,
  `_S_heap_size` **0x79B264**. Blocks > 128 bytes go straight to `malloc` = **0x7005AF** (1966 callers) / `free` = **0x7005D8**.
- Pattern of an inlined `allocate(n)`: `if (n > 128) malloc(n) else { list = &free_list[(n-1)>>3]; p = *list; if (p) *list = *p; else p = refill(n) }`.
  Pattern of `deallocate(p, n)`: `if (n > 128) free(p) else { *p = *list; *list = p; }`.

**Idioms that give the class away:** a `string` member is written through `_M_start.._M_end_of_storage` triples; `vector<CUnit*>` loops
`for (p = start; p != finish; p += 4)`; `map` lookups walk `node = header->parent` with `key < node->value` descents; a destructor
that frees `_M_start` with size `(end_of_storage - start)` is a vector/string member destructor.

Reference: STLport 4.0 / 4.5 sources (`stl/_string.h`, `_vector.h`, `_list.h`, `_tree.h`, `_hashtable.h`, `_alloc.h`,
`_alloc.c`) — all public, the game's version is the 2000-2001 one shipped with MSVC 6 projects.

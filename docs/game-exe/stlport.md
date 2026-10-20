# STLport in game.exe (MSVC 6.0, STLport 4.x; RTTI names `.?AV...@stlport@@`) — layouts to recognise in phase 1

game.exe links STLport statically (no MFC: the 15 'afx/mfc' string hits are STLport/CRT noise). Every container
below appears inline in classes, so its layout IS the class layout at that offset. Sizes are for x86-32.

| STLport 4.x type | size | layout (offsets from the member) |
|---|---|---|
| `string` (basic_string<char>) | 12 | +0 `_M_start` (char*), +4 `_M_finish` (char*, points at the NUL), +8 `_M_end_of_storage` (char*). No short-string buffer in 4.x by default. Empty string: start==finish, 1 byte allocated. |
| `vector<T>` | 12 | +0 `_M_start`, +4 `_M_finish`, +8 `_M_end_of_storage`. size = (finish-start)/sizeof(T). Growth: max(2*cap, n). |
| `list<T>` | 4 | +0 `_M_node` -> sentinel node. Node: +0 next, +4 prev, +8 data. Empty: node->next == node. |
| `slist<T>` | 4 | +0 head node ptr (`_M_head._M_next`). |
| `deque<T>` | 32 | +0 map (T**), +4 map_size, +8 start iterator {cur, first, last, node} (16), +24 finish iterator (16). (Total 40 with the iterators: check per use.) |
| `map` / `set` / `multimap` (`_Rb_tree`) | 8 | +0 `_M_header` -> header node (color, parent=root, left=leftmost, right=rightmost; value unused), +4 `_M_node_count`. Node: +0 color (int), +4 parent, +8 left, +12 right, +16 value (key, then mapped). |
| `hash_map` / `hash_set` (`hashtable`) | 20 | +0 hasher/equal (empty, 0 or 1 bytes padded), then `_M_buckets` vector (12: start, finish, end_of_storage), `_M_num_elements` (4). Node: +0 next, +4 value. |
| `pair<K,V>` | — | K then V, natural alignment. |
| `allocator<T>` | 0/1 | empty; the node allocator is a static class (below). |

**The node allocator (`__node_alloc`, used by every container for blocks <= 128 bytes):**
- `_S_chunk_alloc(size, &nobjs)` = **0x406B70** (406 callers because `_S_refill` is inlined into every `allocate` instantiation).
- Globals: `_S_free_list[16]` at **0x79B224** (lists for 8, 16, ..., 128 bytes), `_S_start_free` **0x79B268**, `_S_end_free` **0x79B26C**,
  `_S_heap_size` **0x79B264**. Blocks > 128 bytes go straight to `malloc` = **0x7005AF** (1966 callers) / `free` = **0x7005D8**.
- Pattern of an inlined `allocate(n)`: `if (n > 128) malloc(n) else { list = &free_list[(n-1)>>3]; p = *list; if (p) *list = *p; else p = refill(n) }`.
  Pattern of `deallocate(p, n)`: `if (n > 128) free(p) else { *p = *list; *list = p; }`.

**Idioms that give the class away:** a `string` member is written through `_M_start.._M_end_of_storage` triples; `vector<CUnit*>` loops
`for (p = start; p != finish; p += 4)`; `map` lookups walk `node = header->parent` with `key < node->value` descents; a destructor
that frees `_M_start` with size `(end_of_storage - start)` is a vector/string member destructor.

Reference: STLport 4.0 / 4.5 sources (`stl/_string.h`, `_vector.h`, `_list.h`, `_tree.h`, `_hashtable.h`, `_alloc.h`,
`_alloc.c`) — all public, the game's version is the 2000-2001 one shipped with MSVC 6 projects.

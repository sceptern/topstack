# topstack

Goose-style data stacks for C++20: memory that is freed a whole scope at a time,
arrays that grow in place without ever moving, and 4- or 2-byte links between objects.
Header-only.

> **Status: v0.1, experimental.** Tested on Linux (x86-64) with GCC 13 and Clang 18,
> including AddressSanitizer, UndefinedBehaviorSanitizer and ThreadSanitizer builds.
> The Windows code paths are written but have not been compiled yet. The API may change.

topstack brings the memory model of [Goose](https://github.com/aardappel/goose) to C++ as a library.
Goose is a language whose values live on a few *data stacks* instead of a general heap:
allocation is a pointer bump, a scope frees everything it allocated by moving the
top back, and arrays grow in place because **only the topmost value on a stack may
grow**. Goose enforces that rule in its compiler; topstack enforces it at runtime and
hands out stacks automatically so you rarely have to think about it.

## Contents

- [Why](#why)
- [A first example](#a-first-example)
- [The rules](#the-rules)
- [The pieces](#the-pieces)
- [Building and testing](#building-and-testing)
- [Performance](#performance)
- [How it works](#how-it-works)
- [Limitations](#limitations)
- [Roadmap](#roadmap)
- [Credits and license](#credits-and-license)

## Why

Programs that build a lot of linked data and then throw it away at once (parsers,
compilers, interpreters, per-frame game state) spend much of their time in `malloc`
and `free`: a call per node to create it, a walk over the whole structure to destroy
it, and a header plus size-class rounding on every node. Arenas fix the allocation
side, but the usual C++ arenas still leave you with:

- **growable arrays that move.** A `std::vector` copies its elements when it grows,
  so pointers into it break. topstack's `GrowBuf` grows in place: pointers to its
  elements stay valid for its whole life, with no `reserve` and no size known in advance.
- **8-byte pointers everywhere.** topstack's `Ref<T>` is a 4-byte link, `Ref16<T>` a
  2-byte one and `Link8<T>` a 1-byte one, which shrinks pointer-heavy nodes to a half,
  a quarter or less.
- **manual plumbing.** Which arena does this array go in? When is it freed? topstack
  gives each growing array its own stack automatically, and a `Scope` frees
  everything made inside it, running destructors where needed.

## A first example

```cpp
#include <topstack/grow_buf.hpp>
#include <topstack/ref.hpp>
#include <cstdio>

struct Node {
    int value;
    topstack::OptRef<Node> left, right;   // 4-byte links that may be null: the node is 12 bytes
};

Node* build(int depth) {
    if (depth == 0) return topstack::make<Node>(1, nullptr, nullptr);
    Node* l = build(depth - 1);
    Node* r = build(depth - 1);
    return topstack::make<Node>(depth, l, r);
}

long sum(const Node* n) {
    return n ? n->value + sum(n->left.get()) + sum(n->right.get()) : 0;
}

int main() {
    {
        topstack::Scope scope;              // everything made below is freed at the }
        Node* root = build(20);             // two million nodes, no malloc
        std::printf("%ld\n", sum(root));

        topstack::GrowBuf<long> squares;    // gets its own stack automatically
        long* first = squares.push(0L);
        for (long i = 1; i < 1'000'000; ++i) {
            squares.push(i * i);            // grows in place: nothing moves
        }
        std::printf("%ld %ld\n", *first, squares[999'999]);   // first is still valid
    }                                       // one rewind frees all of it
}
```

No stack was created by hand: the calling thread has a default set of stacks,
created on first use. You can also create and pass stacks explicitly (see below).

## The rules

topstack is fast because it doesn't track objects individually. In return, there are
a few rules. Breaking one stops the program with a message that says what happened,
instead of corrupting memory.

1. **Only the top of a stack grows.** A `GrowBuf` owns the top of its stack while it
   grows. If something else is allocated on the same stack afterwards, the buffer is
   *sealed*; pushing to it again before that allocation is freed is an error. Buffers
   that get their stack from a `StackSet` (the default) each have their own stack, so
   this only comes up when you put a buffer on a `Stack&` yourself.
2. **Scopes free in order.** A `Scope` frees everything allocated on its stack since it
   was opened. Don't use those objects afterwards, and don't let a buffer that was
   created *outside* a scope grow *inside* it on the same stack (that would free part
   of the buffer).
3. **Build objects that contain links in place.** A link (`Ref`, `OptRef`, `Ref16`,
   `OptRef16`) stores the distance from itself to its target, so it must live near its
   target, on the same stack. Make such objects with `make` or `push(args...)`, not as
   local variables or temporaries:
   ```cpp
   topstack::make<Node>(1, l, r);   // fine: built on the stack
   buf.push(1, l, r);               // fine
   buf.push(Node{1, l, r});         // stops (debug): the temporary is on the native stack
   ```
4. **Don't link across stacks or threads.** Each thread has its own stacks, and they
   are released when the thread ends. Don't hand pointers into them to other threads.

## The pieces

| Header | What it gives you |
|---|---|
| `topstack/stack.hpp` | `Stack`, `Scope`, and `make` / `alloc` on the thread's default stack |
| `topstack/stack_set.hpp` | `StackSet`, `default_set()`, `plain_stack()` (included by `stack.hpp`) |
| `topstack/grow_buf.hpp` | `GrowBuf<T>` |
| `topstack/ref.hpp` | `Ref<T>`, `OptRef<T>`, `Packed<T>` |
| `topstack/ref16.hpp` | `Ref16<T>`, `OptRef16<T>` |
| `topstack/link8.hpp` | `Link8<T>`, `OptLink8<T>` |

### `Stack`: one data stack

A `Stack` reserves a range of address space (2 GB by default, which is also the
maximum) and commits memory as it is used, so a large reservation costs nothing until
it is touched.

```cpp
auto [stack, err] = topstack::Stack::create();          // or create(reserve, commit_step)
if (!stack) std::puts(topstack::to_string(err));

int*    one  = stack->make<int>(42);                    // constructs in place
double* many = stack->make_array<double>(1000);
void*   raw  = stack->alloc(256, 64);                   // bytes, alignment

auto [p, e] = stack->try_make<std::string>("hello");    // try_: returns the error too
```

- `make` and `alloc` return `nullptr` if the stack is out of space; the `try_` versions
  return `{pointer, error}` so you can tell *why* (`OutOfReserve` or `CommitFailed`).
- Objects with destructors (like `std::string`) get a cleanup note, and their
  destructors run, newest first, when their scope ends.
- `make_array` is for types without destructors.

### `Scope`: free everything made since it opened

```cpp
{
    topstack::Scope scope(*stack);      // or Scope scope; for the thread's plain stack
    ...                                 // allocate freely
}                                       // destructors run, then the top moves back
```

Underneath, a scope is `stack.mark()` when it opens and `stack.rewind(mark)` when it
closes; you can call those yourself. In debug builds, freed memory is overwritten with
`0xDD` so use-after-free bugs show up quickly.

### `GrowBuf<T>`: an array that grows in place

```cpp
topstack::GrowBuf<Token> tokens;              // on a stack lent by the thread's set
topstack::GrowBuf<Token> mine(*stack);        // or on a stack you choose

Token* t = tokens.push(kind, start, length);  // constructs in place; nullptr if out of space
tokens.size(); tokens[3]; for (Token& t : tokens) { ... }

topstack::GrowBuf<char> text;
char* p = text.extend(n);                     // n more elements at once, uninitialized
text.clear();                                 // empty it; refills from the same place
```

- Elements never move, so pointers and references to them stay valid until the buffer
  is destroyed.
- Destroying the buffer destroys its elements and gives its space back.
- A buffer can be moved, for example returned from a function, and keeps growing in its
  new home. A moved-from buffer may only be destroyed or assigned to.
- A push is a compare and a store; it only leaves that fast path to commit more memory
  or to resume after being sealed.
- `extend(n)` adds `n` elements in one step without initializing them (for element
  types that need no constructor, such as bytes); it is the way to write variable-size
  records into a byte buffer. It returns `nullptr` if out of space.
- `clear()` destroys the elements and keeps the buffer's place, so it can be refilled
  without giving its stack back.

### `StackSet` and the default stacks

A `StackSet` owns up to 64 stacks. Stack 0 is for plain allocations; stacks 1 to 63 are
lent to growing buffers, the lowest free one first, and taken back when the buffer is
destroyed. Freed stacks keep their memory for the next buffer.

Every thread gets its own set the first time it needs one (`topstack::default_set()`),
and the stack-free forms use it: `GrowBuf<T> buf;`, `topstack::make<T>(...)`,
`topstack::alloc(...)`, `topstack::Scope scope;`. You can create your own set too:

```cpp
topstack::StackSet set(std::size_t(1) << 30, std::size_t(256) << 20);   // 1 GB plain, 256 MB buffers
topstack::GrowBuf<int> xs(set);
```

More than 63 buffers growing at once on one set is treated as a bug (most likely
buffers that are never destroyed) and stops the program. If the operating system
refuses to reserve a stack, the buffer has no stack and every push returns `nullptr`.

### `Ref<T>`, `OptRef<T>` and `Packed<T>`: 4-byte links, packed nodes

```cpp
struct Expr { std::uint8_t tag; };
struct Bin  : Expr { topstack::Ref<Expr> l, r; };            // 9 bytes
struct Num  : Expr { topstack::Packed<std::int32_t> v; };    // 5 bytes

Bin* b = topstack::make<Bin>(Expr{1}, left, right);
Expr* l = b->l.get();                 // also b->l->..., *b->l, b->l == p
```

- A `Ref<T>` is 4 bytes: the signed distance from the link to its target, as in Goose's
  self-relative references. Following it is one addition.
- **`Ref<T>` is never null, `OptRef<T>` may be,** like Goose's `T&` and `T&?`. Use
  `OptRef` where a link can be missing (a leaf's children, the end of a list) and `Ref`
  everywhere else: it skips the null check on every read and write, which makes
  link-heavy code faster (an LRU cache 7%, the interp benchmark 11% with GCC).
  `link = nullptr` and `if (link)` only compile for an `OptRef`; in debug builds, reading
  a `Ref` before it was set, or setting it to null through a pointer, stops the program.
- The target must be within 2 GB of the link, which is always true within one stack.
- Copying a link (or a struct containing one) recomputes the offset for the new place;
  copying raw bytes with `memcpy` does not.
- Links and `Packed<T>` have alignment 1, so a node made of a tag, links and `Packed`
  fields has no padding, and such nodes sit back to back on the stack.
- **Checks:** storing a target that is too far away (or at the link's own address) is
  checked in debug builds, like `assert`. Define `TOPSTACK_CHECKED_REFS` to keep the
  checks in release builds too. On the benchmarks they cost 0 to 5%, except bintrees
  (8% with GCC, 23% with Clang), where storing links is almost all the work.

### `Ref16<T>` and `OptRef16<T>`: 2-byte links for big structures

```cpp
struct Bin : Expr { topstack::Ref16<Expr> l, r; };          // 5 bytes instead of 9
```

`Ref16<T>` is used exactly like `Ref<T>`, in half the space, and `OptRef16<T>` like
`OptRef<T>`:

- A target within 16 KB of the link is stored directly in the 2 bytes.
- A farther target gets a *relay*: a 4-byte slot that holds the real offset. Each 64 KB
  region of a stack has its own relay table, found from the link's own address, so a
  `Ref16` needs no extra arguments. Relays are freed together with the data they belong
  to, including by a `Scope`.
- In structures built bottom-up, like trees whose children are made before their
  parent, almost every link is near; the benchmark trees end up with a few hundred
  relays among millions of links.

### `Link8<T>` and `OptLink8<T>`: 1-byte links, set once

```cpp
struct Tree { topstack::Packed<std::int32_t> v; topstack::OptLink8<Tree> left, right; };  // 6 bytes
Tree* t = topstack::make<Tree>(7, l, r);
t->left->v;                                     // read like a Ref
```

- A target within 64 bytes of the link is stored directly in its one byte.
- A farther target gets an *anchor*: a 4-byte `Ref` to the target, made on the stack
  right after the node, and the link's byte points at the anchor. A `Scope` frees
  anchors together with their nodes.
- An anchor can only be put right after a node while that node is the newest thing on
  its stack, so **a `Link8` gets its target when its node is made, and never changes
  after that**: it can't be assigned or copied. The node it points to can still be
  changed. Make such nodes with `make`, keep them small (an anchor has to be within 63
  bytes of its link), and don't put far links in `GrowBuf` elements; each of these is
  checked, and breaking it stops the program with a message.
- `Link8<T>` is never null, `OptLink8<T>` may be, as with `Ref` and `OptRef`.

Use it for data that is built once, bottom-up, and then read: trees, parsed
expressions, syntax trees. Children made just before their parent are almost always
within 64 bytes, so nearly every link takes one byte.

### Which link to use

Smaller links take a few more instructions to store and follow. When the data fits in
the CPU's cache, that shows and wider links are faster; when it doesn't, smaller nodes
mean fewer cache misses and can be faster too. Measured on this machine (faster
compiler of the two):

| Benchmark | Data | `Ref` (4 bytes) | `Ref16` (2 bytes) | `Link8` (1 byte) |
|---|---|---|---|---|
| bintrees | trees of at most a few MB, rebuilt millions of times | **394 ms**, 17.9 MB | 492 ms, 9.5 MB | 477 ms, **8.3 MB** |
| tree | one tree of 33 million nodes | 1120 ms, 386 MB | **900 ms**, 259 MB | 1087 ms, **202 MB** |
| interp | one expression tree, about 22 MB | 103 ms, 21.6 MB | 97 ms, 15.8 MB | **94 ms, 13.2 MB** |
| sexp | a parsed text, about 380 MB, much of it the text | **1688 ms**, 379 MB | 1763 ms, **323 MB** | (not ported) |

So: `Ref` for small, hot data or data that changes; `Ref16` for big structures;
`Link8` for the least memory on data that is built once, where it is also the fastest
on interp. On tree, `Link8` uses 22% less memory than `Ref16` but is 21% slower here
(on another machine it was faster), so measure on yours.

## Building and testing

topstack is header-only: add `include/` to your include path. It needs C++20.

```sh
./test.sh
```

builds each test twice, once normally and once with AddressSanitizer and
UndefinedBehaviorSanitizer, then builds two small programs that check plain-stack-only
use and a GrowBuf in a global variable, and runs them all, stopping at the first failure:

```
tests/stack_test.cpp       Stack, alloc, marks, Scope, make, cleanups
tests/grow_buf_test.cpp    GrowBuf: growth, sealing, ownership, moving
tests/stack_set_test.cpp   StackSet, default stacks, threads, plain allocations
tests/ref_test.cpp         Ref and Packed
tests/ref16_test.cpp       Ref16: near and far links, relays across scopes, slot reuse
tests/link8_test.cpp       Link8: near links, anchors, freeing, the rules
tests/plain_only.cpp       Scope() and make() with only stack.hpp included
tests/global_buf.cpp       a GrowBuf in a global variable exits cleanly
```

The tests that check that rule violations stop the program use `fork`, so they are
POSIX-only. GitHub Actions runs `./test.sh` on every push.

## Performance

All numbers are from one machine and one run: WSL2 (Linux 6.6) on a 16-core x86-64 PC,
GCC 13.3 and Clang 18.1, `-O2` (`bench/results/2026-11-27_1543.txt`). Each program ran
7 times; the first 2 runs are warm-ups and the tables show the fastest of the rest.
Memory is peak resident memory. Every program printed exactly what Goose's own version
prints.

The benchmarks are all 16 of [Goose's own](https://github.com/aardappel/goose/tree/master/bench)
(`bench/` in the Goose repository), at the largest sizes Goose's runner uses. To keep
the comparison fair:

- **Every side is built with both GCC and Clang**, Goose included, and the faster
  build counts. (Which compiler wins varies a lot: Goose's blur runs 5x faster with
  Clang, its bintrees 20% faster with GCC.)
- **Every version each side has counts.** For Goose that includes its alternative
  versions (a CSR graph, a streaming respond, a dictionary for words) and, for the
  four tree benchmarks, Goose's programs rebuilt with its smallest links
  (`&<varint>`, 1 byte for a near target), which match topstack's `Link8` in memory.
  For topstack it includes all link widths. For C++, Goose's "expert" versions; for
  Rust, all of Goose's Rust versions.
- **Speed compares each side's fastest version; memory each side's smallest.**
- **Huge pages are off** for everyone, topstack included (it asks for them by default;
  the benchmark scripts build it with `TOPSTACK_NO_HUGE_PAGES`). What they add is in
  [Huge pages](#huge-pages) below.

### Summary

How many times faster topstack is / how many times less memory it uses. Above 1.00x,
topstack wins.

| Benchmark | vs Goose | vs expert C++ | vs Rust |
|---|---|---|---|
| bintrees | 0.69x / 1.00x | 1.06x / **4.05x** | 1.16x / **3.12x** |
| push | 0.98x / 1.00x | 0.98x / 1.00x | 1.03x / 1.00x |
| tree | **1.38x** / 0.97x | **1.32x / 1.91x** | **1.52x / 1.91x** |
| interp | 1.07x / 0.98x | **2.04x / 3.74x** | **2.13x / 2.82x** |
| sexp | 1.05x / 0.89x | **1.74x / 2.96x** | **1.40x / 2.05x** |
| sum | 1.00x / 1.00x | 1.00x / 1.00x | 1.03x / 1.00x |
| strlist | 0.99x / 1.12x | 1.17x / 1.00x | 1.09x / 1.14x |
| records | 0.98x / 1.00x | **2.09x / 2.70x** | **2.40x / 4.29x** |
| graph | 0.23x / 1.00x | 0.26x / 1.00x | 0.29x / 1.00x |
| words | 0.95x / 0.92x | **1.49x / 1.47x** | **1.41x / 1.22x** |
| particles | 1.00x / 1.01x | 0.77x / 1.00x | 1.00x / 1.00x |
| lru | 0.95x / 1.00x | 0.98x / 1.02x | 1.00x / 1.00x |
| scene | 1.01x / 0.98x | **1.30x / 1.32x** | 0.99x / 1.00x |
| calc | 0.98x / 1.00x | 1.03x / 1.00x | 0.91x / 1.00x |
| respond | 0.80x / 1.00x | 1.02x / 1.00x | **1.20x** / 1.00x |
| blur | 0.62x / 1.00x | 0.78x / 1.00x | 0.68x / 1.00x |
| **geometric mean** | **0.87x / 0.99x** | **1.09x / 1.47x** | **1.10x / 1.40x** |

What it shows:

- **Against Goose, Goose is ahead overall** when each side brings its best: topstack
  runs at 0.87x Goose's speed with the same memory. Most of the gap is in three places:
  - **Designs topstack has no port of:** Goose's CSR graph (4x faster than the linked
    graph that topstack, like Goose's main version, builds) and Goose's streaming
    respond. Comparing only the same data structures, topstack is at **0.96x Goose's
    speed and 1.01x its memory**: even.
  - **bintrees** (0.69x): the calling convention, below.
  - **blur** (0.62x): Goose's loop, built with Clang, beats topstack's. topstack's
    version uses the same loop as the expert C++ one and is still 29% slower than it
    on this machine (on another machine the two were even), so this is under
    investigation.
- **Memory against Goose is even.** With its smallest links, Goose matches topstack's
  1-byte `Link8` (bintrees 8.3 MB each, tree 196 against 202 MB, interp 13.0 against
  13.2 MB) and is smaller on sexp (288 against 323 MB). topstack is faster at the same
  memory on tree (1087 against 1394 ms) and interp (94 against 101 ms).
- **Against expert C++ and Rust: about 1.1x faster with 1.4 to 1.5x less memory**,
  and up to 2.4x faster (records) and 4.3x less memory (records against Rust).
  topstack loses where they use a different design (CSR graph; structure-of-arrays
  particles) and on blur.

### Times and memory

Each cell: time, peak memory, and which version. **Bold** marks the best in a row.

| Benchmark | Goose | topstack | Expert C++ | Rust |
|---|---|---|---|---|
| **bintrees**, depth 19 | **271 ms**, 25.5 MB; `&<varint>`: 460 ms, **8.3 MB** | 394 ms, 17.9 MB (`OptRef`); 477 ms, **8.3 MB** (`OptLink8`) | 419 ms, 33.6 MB (vector arena) | 458 ms, 25.9 MB (Vec arena) |
| **push**, 64M items | **557 ms**, 498 MB | 570 ms, 498 MB | 560 ms, 498 MB (vector + exact reserve) | 587 ms, 498 MB (Vec + reserve) |
| **tree**, depth 24 | 1238 ms, 386 MB; `&<varint>`: 1394 ms, **196 MB** | **900 ms**, 259 MB (`OptRef16`); 1087 ms, 202 MB (`OptLink8`) | 1191 ms, 386 MB (vector arena) | 1372 ms, 386 MB (Vec arena) |
| **interp**, depth 24 | 107 ms, 21.8 MB; `&<varint>`: 101 ms, **13.0 MB** | **94 ms**, 13.2 MB (`Link8`) | 192 ms, 49.4 MB (tagged union + arena) | 199 ms, 37.2 MB (Vec arena) |
| **sexp**, N = 300,000 | 1865 ms, 370 MB; `&<varint>`: 1773 ms, **288 MB** | **1688 ms**, 379 MB (`OptRef`); 1763 ms, 323 MB (`OptRef16`) | 2943 ms, 955 MB (arena + string_view) | 2359 ms, 663 MB (Vec arena) |
| **sum**, 128M items | **843 ms**, 490 MB | 844 ms, 490 MB | 846 ms, 490 MB (vector + reserve) | 871 ms, 490 MB |
| **strlist**, 8M words | **517 ms**, 215 MB | 520 ms, **192 MB** | 608 ms, 193 MB (flat offsets) | 566 ms, 219 MB (`Vec<&str>`) |
| **records**, 16M events | **539 ms, 143 MB** | 548 ms, 143 MB | 1143 ms, 386 MB (variant + fixed buffer) | 1313 ms, 612 MB (enum + String) |
| **graph**, 2M vertices | linked: 5443 ms, 154 MB; CSR: **1296 ms**, 215 MB | linked: 5655 ms, **154 MB** | CSR: 1475 ms, 215 MB; linked: 5038 ms, 154 MB | CSR: 1663 ms, 215 MB |
| **words**, 16M words | **1961 ms**, 529 MB; dictionary: 2120 ms, **417 MB** | 2066 ms, 452 MB | 3079 ms, 664 MB (open addressing) | 2919 ms, 668 MB (open addressing) |
| **particles**, 4M | 1761 ms, 93.5 MB | 1757 ms, 93.4 MB | **1348 ms**, 93.4 MB (structure of arrays) | 1766 ms, 93.2 MB |
| **lru**, 32M operations | **3059 ms**, 127 MB | 3205 ms, 127 MB (`Ref`) | 3153 ms, 130 MB (arena + open addressing) | 3220 ms, 127 MB (open addressing) |
| **scene**, depth 16 | 707 ms, **180 MB** | 699 ms, 184 MB (`Ref`) | 908 ms, 242 MB (arena + indices) | **690 ms**, 184 MB (arena) |
| **calc**, 1.6M inputs | 663 ms | 674 ms (`Ref16`) | 694 ms (arena + error codes) | **611 ms** (arena + Result) |
| **respond**, 1.6M requests | DTO: 697 ms; streaming: **545 ms** | DTO: 680 ms | streaming: 693 ms | streaming: 818 ms |
| **blur**, 8192×8192 | **466 ms** | 749 ms | 581 ms (row pointers) | 513 ms |

calc and respond use 8.3 to 8.4 MB in every version, blur 130 MB. words' C++ and Rust
versions hash with FNV-1a, one byte at a time; Goose and topstack use Goose's own hash,
8 bytes at a time.

### Huge pages

topstack asks the system for huge pages (2 MB instead of 4 KB) for each stack's memory
beyond its first 2 MB, so big data needs far fewer TLB entries, and turns them off for
the first 2 MB, so small stacks stay small. `TOPSTACK_NO_HUGE_PAGES` turns this off.
Goose turns them off for the first 2 MB too, but never asks for them beyond it, and
glibc's `malloc` only asks when told to (`GLIBC_TUNABLES=glibc.malloc.hugetlb=1`).

What a program gets depends on the system setting
(`/sys/kernel/mm/transparent_hugepage/enabled`). `bench/compare_hugepages.sh` runs
everything under both (`bench/results/2026-11-27_1933.txt`); best of GCC and Clang.

**`madvise`** (Ubuntu's default): only programs that ask get huge pages, so Goose runs
without them. Huge pages off → on:

| Benchmark | topstack | expert C++ (with the tunable) | Goose |
|---|---|---|---|
| tree | 880 → **730 ms** (−17%) | 1189 → 899 ms (−24%) | 1223 ms |
| sexp | 1813 → **1449 ms** (−20%) | 3344 → 1899 ms (−43%) | 1887 ms |
| records | 606 → **475 ms** (−22%) | 1211 → 704 ms (−42%) | 554 ms |
| graph | 5678 → 5048 ms (−11%) | 5132 → **4684 ms** (−9%) | 5386 ms |
| words | 2235 → **1930 ms** (−14%) | 3245 → 2314 ms (−29%) | 2022 ms |
| lru | 3394 → 3033 ms (−11%) | 3365 → **2963 ms** (−12%) | 3185 ms |
| scene | 792 → **631 ms** (−20%) | 993 → 702 ms (−29%) | 779 ms |

**`always`**: every program gets huge pages beyond the first 2 MB without asking, Goose
and plain C++ included, so this is like for like:

| Benchmark | topstack | expert C++ | Goose |
|---|---|---|---|
| tree | **757 ms** | 977 ms | 988 ms |
| sexp | **1575 ms** | 2002 ms | 1752 ms |
| records | 490 ms | 709 ms | **487 ms** |
| graph | 5071 ms | **4541 ms** | 4857 ms |
| words | 2054 ms | 2283 ms | **1726 ms** |
| lru | 3020 ms | 2886 ms | **2831 ms** |
| scene | **616 ms** | 679 ms | 675 ms |

- **topstack's default is 11 to 22% faster** on these benchmarks, for 0 to 1.1% more
  memory (each stack's last huge page is backed in full), with nothing to set.
- **Under `madvise`, topstack is the fastest on five of the seven;** C++ with the
  tunable wins graph and lru. Part of that lead is that Goose doesn't ask.
- **With huge pages for everyone (`always`), topstack is the fastest on three** (tree,
  sexp, scene), even with Goose on records (0.6%), and behind on three: Goose wins
  words (by 19%) and lru (7%), C++ wins graph (12%). Goose gains 7 to 19% from huge
  pages, C++ 9 to 43% (`malloc` spreads small objects over more memory, so it misses
  the TLB more often to begin with).
- The main tables keep huge pages off for everyone. (With `defrag` at `madvise`, as
  here, a program that doesn't ask only gets huge pages when they're free at once; one
  that asks may wait for the system to make them.)

### A real library: cJSON

[cJSON](https://github.com/DaveGamble/cJSON) ported three ways, on the three files of
the nativejson-benchmark (`bench/compare_json.sh`), against cJSON itself and
[RapidJSON](https://rapidjson.org/) 1.1.0:

- **Level 1, allocator hooks** (`bench/json/cjson.cpp -DHOOKS=1`): cJSON's own
  `cJSON.c`, unchanged, given a topstack stack through `cJSON_InitHooks`. `free` only
  takes back the newest allocation, and a `Scope` frees each document without
  `cJSON_Delete`.
- **Level 2, topstack layout** (`bench/json/topstack.cpp`): cJSON's parser and printer,
  same algorithm and the same output byte for byte, with a 5-byte header per value
  (`OptRef` links), keys and strings inline, no padding. cJSON's node is 64 bytes plus
  a `malloc` per string.
- **Level 3, `Link8`** (`bench/json/link8.cpp`): level 2 with 1-byte links, built
  bottom-up; parse-only (the tree can be read and printed, not edited).
- **Fast** (`-DFAST=1`): levels 2 and 3 with faster parsing code and the same output:
  `std::from_chars` and `std::to_chars` for numbers instead of cJSON's `strtod`,
  `sprintf` and `sscanf`, and 8 bytes at a time when scanning strings and whitespace.
  This code isn't topstack's; it would speed up cJSON too. Separate rows, so the layout
  and the code are measured apart.
- RapidJSON: `Parse`, `ParseInsitu`, and its default number mode, which is faster but
  reads 13% of canada's numbers one or two steps off the nearest double (a 64-bit
  integer times a power of ten, rounded twice). Every other version reads numbers
  exactly; the script checks that all of them agree.

From `bench/results/2026-11-27_2050.txt` (AMD Ryzen 9 6900HX); best of GCC and Clang;
200, 100 and 10 rounds. **Parse and walk**, time and peak memory (the peak includes the
input text and about 1.2 MB for the program itself):

| | twitter (632 KB) | citm_catalog (1.7 MB) | canada (2.3 MB) |
|---|---|---|---|
| cJSON | 332 ms, 4.0 MB | 372 ms, 6.9 MB | 232 ms, 16.5 MB |
| **level 1** (hooks) | 231 ms, 3.5 MB | 275 ms, 6.0 MB | 182 ms, 13.9 MB |
| **level 2** (`OptRef`) | 142 ms, **2.6 MB** | 171 ms, 3.9 MB | 131 ms, 5.6 MB |
| **level 3** (`Link8`) | 157 ms, **2.6 MB** | 190 ms, **3.6 MB** | 134 ms, **4.9 MB** |
| **level 2, fast** | **91 ms**, 3.0 MB | **95 ms**, 4.1 MB | **48 ms**, 6.0 MB |
| **level 3, fast** | 103 ms, 3.0 MB | 110 ms, 3.9 MB | 52 ms, 5.4 MB |
| RapidJSON `Parse` | 343 ms, 3.5 MB | 287 ms, 5.8 MB | 117 ms, 8.7 MB |
| RapidJSON `ParseInsitu` | 170 ms, 3.1 MB | 211 ms, 5.8 MB | 109 ms, 8.6 MB |
| RapidJSON, inexact numbers | 176 ms, 3.1 MB | 204 ms, 5.8 MB | 66 ms, 8.7 MB |

**The tree itself**, the bytes one parsed document uses (the same on any machine, and
for the fast builds):

| | twitter | citm_catalog | canada |
|---|---|---|---|
| cJSON | 1.87 MB | 3.88 MB | 13.4 MB |
| level 1 | 1.38 MB | 2.86 MB | 10.7 MB |
| level 2 | 0.48 MB | 0.64 MB | 1.95 MB |
| **level 3** | **0.44 MB** | **0.47 MB** | **1.28 MB** |
| RapidJSON `Parse` | 0.76 MB | 1.09 MB | 2.68 MB |
| RapidJSON `ParseInsitu` | 0.44 MB + input | 1.02 MB + input | 2.68 MB + input |

(In situ, the strings stay in the input text, which has to be kept as long as the tree.)

**Parse, walk and print:**

| | twitter | citm_catalog | canada |
|---|---|---|---|
| cJSON | 662 ms | 616 ms | 953 ms |
| level 1 | 462 ms | 461 ms | 867 ms |
| level 2 | 323 ms | 303 ms | 787 ms |
| level 3 | 348 ms | 336 ms | 810 ms |
| level 2, fast | **226 ms** | **172 ms** | 209 ms |
| level 3, fast | 242 ms | 193 ms | 219 ms |
| RapidJSON, best exact | 290 ms | 261 ms | 186 ms |
| RapidJSON, inexact numbers | 279 ms | 253 ms | **142 ms** |

- **Hooks alone make cJSON 1.3 to 1.4x faster**, with no change to cJSON: allocation
  becomes a bump and freeing a document a single step. The tree also shrinks by 20 to
  26% (no `malloc` headers). Printing uses a bit more memory than with `malloc`, because
  cJSON grows its print buffer by copying it and a stack can't take back the old copies.
- **The topstack layout makes it 1.8 to 2.3x faster and 4 to 7x smaller** than cJSON,
  with the same parsing code: the gain is the memory layout alone. That is already
  1.2x faster than RapidJSON's fastest mode on twitter and citm_catalog.
- **Faster parsing code makes level 2 another 1.6 to 2.7x faster**, 3.7 to 4.8x faster
  than cJSON in all. cJSON's own code was its bottleneck: glibc's `strtod` on a copy of
  each number (60% of the instructions on canada), scanning one byte at a time, and
  printing each number with `sprintf`, then reading it back with `sscanf` to check it.
- **Level 2, fast, parses 1.4 to 2.2x faster than every RapidJSON mode**, including
  its inexact one, with less peak memory (3 to 30% less). With printing it is faster on
  twitter (1.2x) and citm_catalog (1.5x); RapidJSON prints canada faster, because
  cJSON's number format, kept here byte for byte, can take three conversions per number.
- **Level 3's tree is 42 to 57% smaller than RapidJSON's**, and levels 2 and 3 have the
  lowest peak memory on every file.
- `Link8` saves 8 to 34% over `OptRef` and costs 2 to 11% of the parse time (9 to 17%
  with the fast code; building bottom-up looks at each key twice).
- The fast builds' peak memory is about 0.4 MB higher: the code for `from_chars` and
  `to_chars`, linked in statically. Their trees are the same size.

### Other allocators, on bintrees (depth 19)

Same run and same section as the tables above (Goose: 271 ms):

| | Time | Memory |
|---|---|---|
| **topstack, `OptRef`** | **394 ms** | 17.9 MB |
| **topstack, `OptLink8`** | 477 ms | **8.3 MB** |
| **topstack, `OptRef16`** | 492 ms | 9.5 MB |
| `std::pmr::monotonic_buffer_resource`, one reused buffer | 416 ms | 49.8 MB |
| C++ vector arena with 32-bit indices (Goose's "expert" C++) | 419 ms | 33.6 MB |
| topstack, 8-byte pointers | 437 ms | 33.9 MB |
| Rust `Vec` arena | 458 ms | 25.9 MB |
| `std::pmr::monotonic_buffer_resource`, one per tree | 463 ms | 33.5 MB |
| LLVM `BumpPtrAllocator` | 901 to 1050 ms | 33.8 MB |
| C++ new/delete | 2268 ms | 65.6 MB |
| Rust `Box` | 2799 ms | 65.9 MB |

Per-node allocation with faster `malloc`s, from the run's malloc section (Goose:
285 ms):

| | Time | Memory |
|---|---|---|
| C++ new/delete with mimalloc / tcmalloc / jemalloc / glibc | 1411 / 1761 / 2284 / 2397 ms | 34 / 43 / 42 / 66 MB |
| Rust `Box` with mimalloc | 1705 ms | 34.2 MB |
| LLVM `BumpPtrAllocator` with tcmalloc / mimalloc | 472 / 500 ms | 40.5 / 42.1 MB |

- With 8-byte pointer nodes, topstack is about even with the other arenas; its lead
  comes from 4-, 2- and 1-byte links, which only work because its memory never moves.
- LLVM's allocator gets and frees its chunks through `malloc`; with glibc's `malloc` on
  this machine that halves its speed, with tcmalloc it is on par with the others.
- A better `malloc` makes per-node allocation up to 1.7x faster, but it stays more than
  3x slower than an arena: freeing a tree node by node is the cost, not finding memory.
- Rust's `bumpalo`, from the run's bumpalo section: bintrees 467 ms / 33.8 MB, tree
  2259 ms / 770 MB, interp 287 ms / 73 MB, sexp 2197 ms / 838 MB. Its 8-byte references
  and doubling chunks cost both time and memory.
- Google's protobuf `Arena`, from the run's protobuf section (Goose 292 ms, topstack
  `OptRef` 431 ms): 641 ms and 50 MB with one arena reused for every tree, and 2090 to
  2370 ms (34 MB) with a new arena or a `Reset()` per tree, about as slow as plain
  new/delete. It is the slowest arena tried. (`bench/compare_protobuf.sh` runs it; it
  needs `libprotobuf-dev`.)

### Where the rest of Goose's speed comes from

On `bintrees`, the remaining gap is not the allocator. Goose passes the stack's top
into each function and returns it, so it stays in a CPU register for the whole
recursion; a library can only keep it in memory, which costs a store and a load around
every call. Written that way by hand (`bench/bintrees_thread.cpp`), the same benchmark
runs in **199 ms with GCC, 26% faster than Goose** (271 ms in the same run). With Clang
it doesn't help, because Clang doesn't inline recursive calls. Getting this
automatically needs compiler support; see the roadmap.

### Reproducing

From `bench/`, with Goose built at `~/programming/goose/build/goose` (or set `GOOSE=`):

```sh
./report.sh
```

runs everything (about an hour and a half) and saves the output in `bench/results/`;
`./report.sh hugepages json` runs just those two scripts, and so on. The individual
scripts are `compare.sh` (bintrees, push, std::pmr, Goose's C++ and Rust),
`compare_extra.sh` (LLVM's allocator and `pinned_vector`), `compare_malloc.sh`
(mimalloc, jemalloc, tcmalloc), `compare_more.sh` (tree, interp, sexp),
`compare_bumpalo.sh` (Rust's bumpalo), `compare_suite.sh` (the other 11 benchmarks;
`./compare_suite.sh records` runs one), `compare_protobuf.sh` (protobuf's Arena),
`compare_hugepages.sh` (huge pages on and off, under both system settings; it asks for
sudo to switch the setting and puts it back) and `compare_json.sh` (cJSON and RapidJSON;
it downloads cJSON and its three JSON files).
Requirements beyond GCC and Clang:

```sh
sudo apt install llvm-18-dev libmimalloc2.0 libjemalloc2 libtcmalloc-minimal4t64 libprotobuf-dev rapidjson-dev
git clone https://github.com/mknejp/vmcontainer.git ~/programming/vmcontainer
curl --proto '=https' -sSf https://sh.rustup.rs | sh        # for the Rust rows
```

C++ programs are linked with `-static-libstdc++` so that loading the C++ standard
library (about 2.4 MB) doesn't count against them; Goose and Rust programs don't pay
it either. Each section of a run measures Goose again, and the machine's speed drifts
by up to 10% over a run, so compare rows within one section. Every script builds Goose
and the C++ versions with both GCC and Clang, and Goose's tree benchmarks also with
`&<varint>` links (`*_goose_varint`).

## How it works

- **Every stack is a 16 GB block of address space aligned to 16 GB:** up to 2 GB of
  data at the start, and room for `Ref16` relay tables after it. Because of the
  alignment, any address inside a stack finds its block's start by masking off the low
  bits. Only address space is reserved; nothing uses memory until it is written.
- **Committing** happens in steps that start at 64 KB and double, up to 64 MB at a
  time, so a big stack needs few system calls and a small one stays small. Committed
  memory only uses RAM once it is written.
- **Allocation** is an aligned bump of the top, compared against a limit. Everything
  rare (committing, alignment above 4 KB, huge requests) is in a separate cold
  function, so the fast path stays a few instructions.
- **Freeing** moves the top back to a mark. Objects with destructors are recorded in a
  small list of cleanup notes, run newest first. Relay tables of the 64 KB regions that
  were freed completely are cleared at the same time.
- **GrowBuf** keeps its own copy of the top while it grows, and the stack's limit is
  set to zero so any other allocation on that stack takes the slow path and seals the
  buffer first. That is what makes "only the top grows" cheap to enforce.
- **Stacks are lent by number:** a 64-bit mask of which stacks are busy, and
  `countr_zero` to find the lowest free one, which is Goose's assignment rule done at
  runtime.
- **`Ref16` relays:** a far link stores a slot number; the slot lives in the table of
  the 64 KB region the *link* is in. The first 32 slots of every region are packed
  together (256 bytes per region); a region that needs more gets its own table. When a
  region's table is full, a slot whose link no longer refers to it is reused.

## Limitations

- **Windows is untested.** The code paths exist, including the aligned reservation;
  they have never been compiled.
- **A stack holds at most 2 GB**, and reserves 16 GB of address space. That is
  per-process virtual address space, not memory, and it doesn't affect other programs,
  but tools that cap a process's address space (Valgrind, `ulimit -v`, some containers)
  may refuse to create stacks. On Windows, each stack's small relay area (8 MB) counts
  toward the system commit limit, though it uses no RAM until written.
- **Rule violations are found at runtime,** not compile time as in Goose. Link misuse
  in release builds is only caught with `TOPSTACK_CHECKED_REFS`; `Ref16` far links are
  always checked. AddressSanitizer doesn't yet know when a `Scope` frees memory, so it
  can't catch use after a rewind the way it catches use after `free`.
- **Variable-size records are packed by hand.** Goose's variable-size enums store each
  record at its own size; in topstack that takes `GrowBuf::extend` and `memcpy` at
  hand-written offsets (see `bench/records_topstack.cpp`).
- **No shrinking or freeing out of order.** There is no `pop` for `GrowBuf` yet (only
  `clear`), and
  no pools for objects that don't die in stack order (Goose's `slot_pool` and
  `slice_pool`).
- **Memory isn't returned to the OS** while a stack is alive, only when it is
  destroyed (a `trim()` is planned).
- **Strings and containers inside nodes** still use `malloc` for their contents.
- **One machine, one OS** for the performance numbers so far, and results differ between
  machines (`Link8` against `Ref16` on tree, topstack's blur against C++). GCC and Clang
  also differ a lot on some benchmarks, up to 5x where one vectorizes a loop and the
  other doesn't; the tables show the faster of the two.

## Roadmap

- **Compiler tools.** A Clang plugin that checks topstack's rules at compile time,
  starting with links in local variables and temporaries, then "only the top grows",
  then narrow links where they can be proven to stay near. After that, an LLVM pass that
  passes the stack's top through calls in registers, as Goose does.
- **Safety:** tell AddressSanitizer about rewinds, so use after a `Scope` is caught
  like use after `free`.
- **Speed:** find why topstack's blur is slower than the same loop in C++ on some machines;
  ports of the designs Goose is faster with (a CSR graph, a streaming respond).
- **Library:** a typed writer and reader for packed variable-size records (the closest
  C++ gets to Goose's variable-size enums), `GrowBuf::pop` and `resize`, slot and slice
  pools, `trim()`, and a `std::pmr` adapter so strings and vectors inside nodes can live
  on a stack too.
- **Real programs:** cJSON is ported (see "A real library: cJSON"); next, a small C
  compiler (chibicc).
- **Windows** builds and tests.

## Credits and license

topstack's design follows [Goose](https://github.com/aardappel/goose)'s data stacks,
relative references and stack assignment; the benchmark programs and their checksums
come from Goose's benchmark suite. Files copied from Goose (`bench/bench.h`, the
`.goose` programs and the C++ benchmark sources) are under the Apache License 2.0;
`NOTICE` lists them and the files based on them. See `bench/LICENSE-goose`.

topstack itself is released under the Apache License 2.0; see `LICENSE` and `NOTICE`.

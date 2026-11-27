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
- **8-byte pointers everywhere.** topstack's `Ref<T>` is a 4-byte link and `Ref16<T>`
  a 2-byte one, which shrinks pointer-heavy nodes to a half or a quarter.
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

**Which one to use:** storing and following a `Ref16` takes a few more instructions
than a `Ref`. When the data fits in the CPU's cache, that shows, and `Ref` is faster.
When it doesn't, the smaller nodes mean fewer cache misses, and `Ref16` is faster too.
Measured on this machine:

| Benchmark | Data | 4-byte links | 2-byte links | Use |
|---|---|---|---|---|
| bintrees | trees of at most a few MB, rebuilt millions of times | **390 ms**, 17.9 MB | 492 ms, **9.5 MB** | 4-byte (or 2-byte for memory) |
| tree | one tree of 33 million nodes | 1147 ms, 386 MB | **857 ms, 259 MB** | 2-byte |
| interp | one expression tree, about 22 MB | 98 ms, 21.6 MB | **94 ms, 15.8 MB** | 2-byte |
| sexp | a parsed text, about 380 MB, much of it the text | **1697 ms**, 379 MB | 1712 ms, **323 MB** | either |

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
tests/plain_only.cpp       Scope() and make() with only stack.hpp included
tests/global_buf.cpp       a GrowBuf in a global variable exits cleanly
```

The tests that check that rule violations stop the program use `fork`, so they are
POSIX-only. GitHub Actions runs `./test.sh` on every push.

## Performance

All numbers below are from one machine: WSL2 (Linux 6.6) on a 16-core x86-64 PC, GCC
13.3 and Clang 18.1, `-O2 -DNDEBUG`, all from one run
(`bench/results/2026-11-27_1113.txt`). Each program ran 7 times; the first 2 runs are
warm-ups and the tables show the fastest of the rest. Memory is peak resident memory.
Every program printed the same output as Goose's own version.

The benchmarks are all 16 of [Goose's own](https://github.com/aardappel/goose/tree/master/bench)
(`bench/` in the Goose repository), at the largest sizes Goose's runner uses, compared
against Goose and against the C++ and Rust versions that come with them. Goose's runner
labels its C++ versions *idiomatic* or *expert*; the comparison here is against the
expert ones, the hardest to beat.

### Summary

How many times faster topstack is / how many times less memory it uses (above 1.00x:
topstack wins). Each column uses the fastest version of each: topstack with the better
link width and compiler, Goose's version of the same data structure, the fastest expert
C++ version, and the fastest Rust version.

| Benchmark | vs Goose | vs expert C++ | vs Rust |
|---|---|---|---|
| bintrees | 0.67x / 1.42x | 1.06x / 1.88x | 1.16x / 1.45x |
| push | 1.04x / 1.00x | 1.01x / 1.00x | 1.03x / 1.00x |
| tree | **1.26x / 1.49x** | **1.35x / 1.49x** | **1.47x / 1.49x** |
| interp | 1.08x / 1.38x | **1.87x / 3.13x** | **2.02x / 2.35x** |
| sexp | 1.12x / 0.98x | **1.74x / 2.52x** | 1.26x / 1.75x |
| sum | 0.99x / 1.00x | 1.02x / 1.00x | 0.98x / 1.00x |
| strlist | 0.96x / 1.12x | 1.23x / 1.00x | 1.12x / 1.14x |
| records | 0.97x / 1.00x | **2.08x / 2.70x** | **2.60x / 4.28x** |
| graph | 1.01x / 1.00x | 0.30x / 1.40x | 0.31x / 1.40x |
| words | 0.66x / 1.17x | 1.05x / 1.47x | 1.08x / 1.48x |
| particles | 1.06x / 1.00x | 0.79x / 1.00x | 0.97x / 1.00x |
| lru | 0.98x / 1.00x | 1.01x / 1.02x | 1.01x / 1.00x |
| scene | **1.31x** / 0.98x | **1.32x / 1.31x** | 1.00x / 1.00x |
| calc | 0.99x / 1.00x | 1.02x / 1.00x | 0.91x / 1.00x |
| respond | 0.90x / 1.00x | 0.91x / 1.00x | 1.06x / 1.00x |
| blur | 3.44x / 1.00x | 0.81x / 1.00x | 0.68x / 1.00x |
| **geometric mean** | **1.06x / 1.08x** | **1.07x / 1.38x** | **1.07x / 1.32x** |
| without graph and blur | 0.98x / 1.10x | 1.20x / 1.41x | 1.20x / 1.35x |

What it shows:

- **Memory: about the same as Goose, and much less than C++ and Rust** wherever the
  data has structure: up to 2.7x less than expert C++ (records) and 4.3x less than Rust.
  topstack never uses more than 2% more memory than Goose, and uses less on bintrees,
  tree, interp, strlist and words. On the benchmarks that are a flat array of numbers
  (push, sum, particles, blur), every version uses the same memory.
- **Speed: even with Goose overall** (0.98x leaving out graph and blur, explained
  below). topstack is faster on tree, scene, sexp and interp; Goose is faster on
  bintrees (the calling convention, see below), words (34%) and respond (10%).
- **Against expert C++ and Rust: about 1.2x faster on average**, and up to 2.1x
  (records), 1.9x (interp) and 1.7x (sexp), while using less memory.
- **graph** isn't like for like in the C++ and Rust columns. topstack and Goose's main
  version build linked edge lists; the fastest C++ and Rust versions use a different
  layout (CSR, built in two passes), which is about 3x faster in every language,
  Goose's own CSR version included. Against the same linked layout, topstack ties.
- **blur and particles** are arithmetic on flat arrays, where the compiler decides:
  Clang vectorizes blur and GCC (at `-O2`) and Goose don't; structure-of-arrays C++ is
  faster on particles. topstack's memory model doesn't change these.
- **GCC is slower than Clang on topstack code that writes bytes one at a time** (words
  24%, records 15%). The tables show the faster compiler.

### Times and memory

| Benchmark | Goose | topstack | Fastest expert C++ | Fastest Rust |
|---|---|---|---|---|
| **bintrees**, depth 19 | **263 ms**, 25.5 MB | 390 ms, 17.9 MB (`OptRef`); 492 ms, **9.5 MB** (`OptRef16`) | 412 ms, 33.6 MB (vector arena) | 451 ms, 25.9 MB (Vec arena) |
| **push**, 64M items | 572 ms, 498 MB | **551 ms**, 498 MB | 559 ms, 498 MB (vector + exact reserve) | 570 ms, 498 MB (Vec + reserve) |
| **tree**, depth 24 | 1079 ms, 386 MB | **857 ms, 259 MB** (`OptRef16`) | 1153 ms, 386 MB (vector arena) | 1256 ms, 386 MB (Vec arena) |
| **interp**, depth 24 | 101 ms, 21.8 MB | **94 ms, 15.8 MB** (`Ref16`) | 176 ms, 49.4 MB (tagged union + arena) | 190 ms, 37.2 MB (Vec arena) |
| **sexp**, N = 300,000 | 1896 ms, **370 MB** | **1697 ms**, 379 MB (`OptRef`); 1712 ms, 323 MB (`OptRef16`) | 2951 ms, 955 MB (arena + string_view) | 2140 ms, 663 MB (Vec arena) |
| **sum**, 128M items | 985 ms, 490 MB | 997 ms, 490 MB | 1019 ms, 490 MB (vector + reserve) | **981 ms**, 490 MB |
| **strlist**, 8M words | **586 ms**, 215 MB | 609 ms, **192 MB** | 751 ms, 193 MB (flat offsets) | 679 ms, 219 MB (`Vec<&str>`) |
| **records**, 16M events | **648 ms, 143 MB** | 668 ms, 143 MB | 1393 ms, 386 MB (variant + fixed buffer) | 1736 ms, 612 MB (enum + String) |
| **graph**, 2M vertices | 6894 ms, 154 MB (linked); 2152 ms, 215 MB (CSR) | 6834 ms, 154 MB (linked, `OptRef`) | **2037 ms**, 215 MB (CSR) | 2131 ms, 215 MB (CSR) |
| **words**, 16M words | **2231 ms**, 529 MB | 3367 ms, **452 MB** | 3541 ms, 664 MB (open addressing) | 3652 ms, 668 MB (open addressing) |
| **particles**, 4M | 1995 ms, 93.5 MB | 1878 ms, 93.4 MB | **1476 ms**, 93.4 MB (structure of arrays) | 1830 ms, 93.2 MB |
| **lru**, 32M operations | **3253 ms**, 127 MB | 3315 ms, 127 MB (`Ref`) | 3350 ms, 130 MB (arena + open addressing) | 3352 ms, 127 MB (open addressing) |
| **scene**, depth 16 | 980 ms, **180 MB** | 748 ms, 184 MB (`Ref`) | 986 ms, 242 MB (arena + indices) | **745 ms**, 184 MB (arena) |
| **calc**, 1.6M inputs | 683 ms | 691 ms (`Ref16`) | 704 ms (arena + error codes) | **627 ms** (arena + Result) |
| **respond**, 1.6M requests | 694 ms (DTO); **555 ms** (streaming) | 774 ms (DTO) | 702 ms (streaming) | 823 ms (streaming) |
| **blur**, 8192×8192 | 2486 ms | 722 ms | 584 ms (row pointers) | **494 ms** |

calc and respond use 8.3 MB in every version, blur 130 MB.

### Other allocators, on bintrees (depth 19)

Same run and same section as the tables above (Goose: 263 ms):

| | Time | Memory |
|---|---|---|
| **topstack, `OptRef`** | **390 ms** | 17.9 MB |
| **topstack, `OptRef16`** | 492 ms | **9.5 MB** |
| C++ vector arena with 32-bit indices (Goose's "expert" C++) | 412 ms | 33.6 MB |
| `std::pmr::monotonic_buffer_resource`, one reused buffer | 412 ms | 49.8 MB |
| topstack, 8-byte pointers | 427 ms | 33.9 MB |
| Rust `Vec` arena | 451 ms | 25.9 MB |
| `std::pmr::monotonic_buffer_resource`, one per tree | 453 ms | 33.5 MB |
| LLVM `BumpPtrAllocator` | 878 to 1025 ms | 33.8 MB |
| C++ new/delete | 2203 ms | 65.6 MB |
| Rust `Box` | 2621 ms | 65.9 MB |

Per-node allocation with faster `malloc`s, from the run's malloc section (Goose:
265 ms):

| | Time | Memory |
|---|---|---|
| C++ new/delete with mimalloc / tcmalloc / jemalloc / glibc | 1306 / 1631 / 2082 / 2190 ms | 34 / 41 / 42 / 66 MB |
| Rust `Box` with mimalloc | 1568 ms | 34.2 MB |
| LLVM `BumpPtrAllocator` with tcmalloc / mimalloc | 443 / 463 ms | 40.5 / 42.1 MB |

- With 8-byte pointer nodes, topstack is about even with the other arenas; its lead
  comes from 4- and 2-byte links, which only work because its memory never moves.
- LLVM's allocator gets and frees its chunks through `malloc`; with glibc's `malloc` on
  this machine that halves its speed, with tcmalloc it is on par with the others.
- A better `malloc` makes per-node allocation up to 1.7x faster, but it stays more than
  3x slower than an arena: freeing a tree node by node is the cost, not finding memory.
- Rust's `bumpalo`, from the run's bumpalo section: bintrees 468 ms / 33.8 MB, tree
  2299 ms / 770 MB, interp 276 ms / 73 MB, sexp 2187 ms / 838 MB. Its 8-byte references
  and doubling chunks cost both time and memory.
- Google's protobuf `Arena`, measured in a separate run on the same machine
  (`bench/results/2026-11-27_protobuf.txt`; Goose 275 ms, topstack `OptRef` 405 ms):
  670 ms and 50 MB with one arena reused for every tree, and 1990 to 2260 ms (34 MB) with
  a new arena or a `Reset()` per tree, about as slow as plain new/delete. It is the
  slowest arena tried. (`bench/compare_protobuf.sh` runs it; it needs `libprotobuf-dev`.)

### Where the rest of Goose's speed comes from

On `bintrees`, the remaining gap is not the allocator. Goose passes the stack's top
into each function and returns it, so it stays in a CPU register for the whole
recursion; a library can only keep it in memory, which costs a store and a load around
every call. Written that way by hand (`bench/bintrees_thread.cpp`), the same benchmark
runs in **199 ms with GCC, 24% faster than Goose** (263 ms in the same run). With Clang
it doesn't help, because Clang doesn't inline recursive calls. Getting this
automatically needs compiler support; see the roadmap.

### Reproducing

From `bench/`, with Goose built at `~/programming/goose/build/goose` (or set `GOOSE=`):

```sh
./report.sh
```

runs everything (about an hour) and saves the output in `bench/results/`. The
individual scripts are `compare.sh` (bintrees, push, std::pmr, Goose's C++ and Rust),
`compare_extra.sh` (LLVM's allocator and `pinned_vector`), `compare_malloc.sh`
(mimalloc, jemalloc, tcmalloc), `compare_more.sh` (tree, interp, sexp),
`compare_bumpalo.sh` (Rust's bumpalo), `compare_suite.sh` (the other 11 benchmarks;
`./compare_suite.sh records` runs one) and `compare_protobuf.sh` (protobuf's Arena).
Requirements beyond GCC and Clang:

```sh
sudo apt install llvm-18-dev libmimalloc2.0 libjemalloc2 libtcmalloc-minimal4t64 libprotobuf-dev
git clone https://github.com/mknejp/vmcontainer.git ~/programming/vmcontainer
curl --proto '=https' -sSf https://sh.rustup.rs | sh        # for the Rust rows
```

C++ programs are linked with `-static-libstdc++` so that loading the C++ standard
library (about 2.4 MB) doesn't count against them; Goose and Rust programs don't pay
it either. Each section of a run measures Goose again, and the machine's speed drifts
by up to 10% over a run, so compare rows within one section.

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
- **One machine, one OS** for the performance numbers so far. Some differences of up to
  10% between GCC and Clang come from where the compilers place code in memory, not from
  the instructions themselves.

## Roadmap

- **Compiler tools.** A Clang plugin that checks topstack's rules at compile time,
  starting with links in local variables and temporaries, then "only the top grows",
  then narrow links where they can be proven to stay near. After that, an LLVM pass that
  passes the stack's top through calls in registers, as Goose does.
- **Safety:** tell AddressSanitizer about rewinds, so use after a `Scope` is caught
  like use after `free`.
- **Library:** a typed writer and reader for packed variable-size records (the closest
  C++ gets to Goose's variable-size enums), `GrowBuf::pop` and `resize`, slot and slice
  pools, `trim()`, and a `std::pmr` adapter so strings and vectors inside nodes can live
  on a stack too.
- **Real programs:** porting a JSON parser (cJSON) and a small C compiler (chibicc) to
  topstack, to measure more than benchmarks.
- **Windows** builds and tests.

## Credits and license

topstack's design follows [Goose](https://github.com/aardappel/goose)'s data stacks,
relative references and stack assignment; the benchmark programs and their checksums
come from Goose's benchmark suite. Files copied from Goose (`bench/bench.h`, the
`.goose` programs and the C++ benchmark sources) are under the Apache License 2.0;
`NOTICE` lists them and the files based on them. See `bench/LICENSE-goose`.

topstack itself is released under the Apache License 2.0; see `LICENSE` and `NOTICE`.

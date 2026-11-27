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
    topstack::Ref<Node> left, right;      // 4-byte links: the node is 12 bytes
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
3. **Build objects that contain links in place.** A `Ref` or `Ref16` stores the
   distance from itself to its target, so it must live near its target, on the same
   stack. Make such objects with `make` or `push(args...)`, not as local variables or
   temporaries:
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
| `topstack/ref.hpp` | `Ref<T>`, `Packed<T>` |
| `topstack/ref16.hpp` | `Ref16<T>` |

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
```

- Elements never move, so pointers and references to them stay valid until the buffer
  is destroyed.
- Destroying the buffer destroys its elements and gives its space back.
- A buffer can be moved, for example returned from a function, and keeps growing in its
  new home. A moved-from buffer may only be destroyed or assigned to.
- A push is a compare and a store; it only leaves that fast path to commit more memory
  or to resume after being sealed.

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

### `Ref<T>` and `Packed<T>`: 4-byte links, packed nodes

```cpp
struct Expr { std::uint8_t tag; };
struct Bin  : Expr { topstack::Ref<Expr> l, r; };            // 9 bytes
struct Num  : Expr { topstack::Packed<std::int32_t> v; };    // 5 bytes

Bin* b = topstack::make<Bin>(Expr{1}, left, right);
Expr* l = b->l.get();                 // also b->l->..., *b->l, if (b->l), b->l == p
```

- A `Ref<T>` is 4 bytes: the signed distance from the link to its target, as in Goose's
  self-relative references. Following it is one addition. Null is offset 0.
- The target must be within 2 GB of the link, which is always true within one stack.
- Copying a `Ref` (or a struct containing one) recomputes the offset for the new place;
  copying raw bytes with `memcpy` does not.
- `Ref` and `Packed<T>` have alignment 1, so a node made of a tag, `Ref`s and `Packed`
  fields has no padding, and such nodes sit back to back on the stack.
- **Checks:** storing a target that is too far away (or at the link's own address) is
  checked in debug builds, like `assert`. Define `TOPSTACK_CHECKED_REFS` to keep the
  checks in release builds too. On the benchmarks they cost 0 to 3%, except bintrees
  with Clang (about 25%), where storing links is almost all the work.

### `Ref16<T>`: 2-byte links for big structures

```cpp
struct Bin : Expr { topstack::Ref16<Expr> l, r; };          // 5 bytes instead of 9
```

`Ref16<T>` is used exactly like `Ref<T>`, in half the space:

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

| Benchmark | Data | `Ref` | `Ref16` | Use |
|---|---|---|---|---|
| bintrees | trees of at most a few MB, rebuilt millions of times | **416 ms**, 17.8 MB | 509 ms, **9.5 MB** | `Ref` (or `Ref16` for memory) |
| tree | one tree of 33 million nodes | 1231 ms, 386 MB | **914 ms, 259 MB** | `Ref16` |
| interp | one expression tree, about 22 MB | 116 ms, 22.0 MB | **98 ms, 15.8 MB** | `Ref16` |
| sexp | a parsed text, about 380 MB, much of it the text | **1747 ms**, 379 MB | 1838 ms, **323 MB** | either |

## Building and testing

topstack is header-only: add `include/` to your include path. It needs C++20.

```sh
./test.sh
```

builds every test twice, once normally and once with AddressSanitizer and
UndefinedBehaviorSanitizer, and runs them, stopping at the first failure:

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
(`bench/results/2026-11-26_2347.txt`). Each program ran 7 times; the first 2 runs are
warm-ups and the tables show the fastest of the rest. Memory is peak resident memory.
Every program printed the same output as Goose's own version. Where both compilers were
measured, the better one is shown.

The benchmarks are [Goose's own](https://github.com/aardappel/goose/tree/master/bench)
(`bench/` in the Goose repository), compared against Goose itself and against its C++
and Rust versions, plus more baselines below.

| Benchmark | Goose | topstack, `Ref` | topstack, `Ref16` | Best other C++ | Best other Rust |
|---|---|---|---|---|---|
| **bintrees**, depth 19 | **282 ms**, 25.5 MB | 416 ms, 17.8 MB | 509 ms, **9.5 MB** | 431 ms, 49.8 MB (`std::pmr`) | 477 ms, 25.9 MB (Vec arena) |
| **tree**, depth 24 | 1241 ms, 386 MB | 1231 ms, 386 MB | **914 ms, 259 MB** | 1254 ms, 386 MB (vector arena) | 1467 ms, 386 MB (Vec arena) |
| **interp**, depth 24 | 107 ms, 21.8 MB | 116 ms, 22.0 MB | **98 ms, 15.8 MB** | 187 ms, 49.4 MB (variant arena) | 201 ms, 37.2 MB (Vec arena) |
| **sexp**, N = 300,000 | 1939 ms, 370 MB | **1747 ms**, 379 MB | 1838 ms, **323 MB** | 3056 ms, 955 MB (arena + string_view) | 2144 ms, 663 MB (Vec arena) |
| **push**, 64M items | **581 ms**, 498 MB | 584 ms, 497 MB | | 596 ms, 498 MB (vector + exact reserve) | 599 ms, 498 MB (Vec + reserve) |

What it shows:

- **Against Goose:** with the right link type, topstack is **faster and uses less
  memory** on `tree` (26% faster, 33% less memory), `interp` (8% faster, 27% less) and
  `sexp` (10% faster with `Ref`, or 13% less memory with `Ref16`). `push` is a tie.
  On `bintrees` Goose is 1.5x faster; topstack uses 30% less memory with `Ref`, 63% less
  with `Ref16`. Why Goose wins there is explained below.
- **Against the C++ and Rust versions:** topstack is faster on every tree benchmark and
  uses less memory, often far less. On `push` it ties with a `std::vector` that has an
  exact `reserve`, which only works when the final size is known in advance and can't
  keep pointers to elements; the C++ container that can (`std::deque`) was 15 to 50%
  slower in earlier runs. `pinned_vector`, a vector that grows inside reserved address
  space like `GrowBuf`, took 575 ms: the same within noise.

### Other allocators, on bintrees (depth 19)

Same run and same section as the table above (Goose: 282 ms):

| | Time | Memory |
|---|---|---|
| **topstack, `Ref`** | **416 ms** | 17.8 MB |
| **topstack, `Ref16`** | 509 ms | **9.5 MB** |
| `std::pmr::monotonic_buffer_resource`, one reused buffer | 431 ms | 49.8 MB |
| C++ vector arena with 32-bit indices (Goose's "expert" C++) | 438 ms | 33.6 MB |
| topstack, 8-byte pointers | 465 ms | 33.8 MB |
| Rust `Vec` arena | 477 ms | 25.9 MB |
| `std::pmr::monotonic_buffer_resource`, one per tree | 481 ms | 33.5 MB |
| LLVM `BumpPtrAllocator` | 932 to 1077 ms | 33.8 MB |
| C++ new/delete | 2349 ms | 65.6 MB |
| Rust `Box` | 2817 ms | 66.0 MB |

Per-node allocation with faster `malloc`s, from the run's malloc section (the machine
was slower then: Goose 316 ms, topstack with 32-bit links 442 ms):

| | Time | Memory |
|---|---|---|
| C++ new/delete with mimalloc / tcmalloc / jemalloc / glibc | 1674 / 2020 / 2677 / 2753 ms | 34 / 41 / 42 / 66 MB |
| Rust `Box` with mimalloc | 2042 ms | 34.2 MB |
| LLVM `BumpPtrAllocator` with tcmalloc / mimalloc | 519 / 570 ms | 40.5 / 42.1 MB |

- With 8-byte pointer nodes, topstack is about even with the other arenas; its lead
  comes from 4- and 2-byte links, which only work because its memory never moves.
- LLVM's allocator gets and frees its chunks through `malloc`; with glibc's `malloc` on
  this machine that halves its speed, with tcmalloc it is on par with the others.
- A better `malloc` makes per-node allocation up to 1.6x faster, but it stays almost 4x
  slower than an arena: freeing a tree node by node is the cost, not finding memory.
- Rust's `bumpalo`, from the run's bumpalo section: bintrees 505 ms / 33.8 MB, tree
  2349 ms / 770 MB, interp 302 ms / 73 MB, sexp 2385 ms / 838 MB. Its 8-byte references
  and doubling chunks cost both time and memory.
- Google's protobuf `Arena` has so far only been measured on the development machine,
  where it was the slowest arena tried: about 30% slower than topstack's pointer version
  at the same memory. (`bench/compare_protobuf.sh` runs it.)

### Where the rest of Goose's speed comes from

On `bintrees`, the remaining gap is not the allocator. Goose passes the stack's top
into each function and returns it, so it stays in a CPU register for the whole
recursion; a library can only keep it in memory, which costs a store and a load around
every call. Written that way by hand (`bench/bintrees_thread.cpp`), the same benchmark
runs in **209 ms with GCC, 26% faster than Goose** (282 ms in the same run). With Clang
it doesn't help, because Clang doesn't inline recursive calls. Getting this
automatically needs compiler support; see the roadmap.

### Reproducing

From `bench/`, with Goose built at `~/programming/goose/build/goose` (or set `GOOSE=`):

```sh
./report.sh
```

runs everything (about 25 minutes) and saves the output in `bench/results/`. The
individual scripts are `compare.sh` (bintrees, push, std::pmr, Goose's C++ and Rust),
`compare_extra.sh` (LLVM's allocator and `pinned_vector`), `compare_malloc.sh`
(mimalloc, jemalloc, tcmalloc), `compare_more.sh` (tree, interp, sexp),
`compare_bumpalo.sh` (Rust's bumpalo) and `compare_protobuf.sh` (protobuf's Arena).
Requirements beyond GCC and Clang:

```sh
sudo apt install llvm-18-dev libmimalloc2.0 libjemalloc2 libtcmalloc-minimal4t64 libprotobuf-dev
git clone https://github.com/mknejp/vmcontainer.git ~/programming/vmcontainer
curl --proto '=https' -sSf https://sh.rustup.rs | sh        # for the Rust rows
```

C++ programs are linked with `-static-libstdc++` so that loading the C++ standard
library (about 2.4 MB) doesn't count against them; Goose and Rust programs don't pay
it either. Each section of a run measures Goose again, and the machine's speed drifts
by up to 10% over a 25-minute run, so compare rows within one section.

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
- **Rule violations are found at runtime,** not compile time as in Goose. `Ref` misuse
  in release builds is only caught with `TOPSTACK_CHECKED_REFS`; `Ref16` far links are
  always checked.
- **No shrinking or freeing out of order.** There is no `pop` for `GrowBuf` yet, and
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
- **Library:** `GrowBuf::pop` and `resize`, slot and slice pools, `trim()`, and a
  `std::pmr` adapter so strings and vectors inside nodes can live on a stack too.
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

# topstack

Goose-style data stacks for C++20: memory that is freed a whole scope at a time,
arrays that grow in place without ever moving, and 4-byte links between objects.
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
- **8-byte pointers everywhere.** topstack's `Ref<T>` is a 4-byte link, which halves
  the size of pointer-heavy nodes.
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
3. **Build objects that contain `Ref`s in place.** A `Ref` stores the distance from
   itself to its target, so it must live near its target, on the same stack. Make such
   objects with `make` or `push(args...)`, not as local variables or temporaries:
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

### `Stack`: one data stack

A `Stack` reserves a range of address space (2 GB by default) and commits memory as it
is used, so a large reservation costs nothing until it is touched.

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
topstack::StackSet set(std::size_t(64) << 30);   // a 64 GB plain stack, 2 GB buffer stacks
topstack::GrowBuf<int> xs(set);
```

More than 63 buffers growing at once on one set is treated as a bug (most likely
buffers that are never destroyed) and stops the program. If the operating system
refuses to reserve a stack, the buffer has no stack and every push returns `nullptr`.

### `Ref<T>` and `Packed<T>`: small links, packed nodes

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
  checks in release builds too. On the benchmarks they cost 0 to 4%, except bintrees
  with Clang (about 25%), where storing links is almost all the work.

## Building and testing

topstack is header-only: add `include/` to your include path. It needs C++20.

```sh
./test.sh
```

builds every test twice, once normally and once with AddressSanitizer and
UndefinedBehaviorSanitizer, and runs them:

```
tests/stack_test.cpp       Stack, alloc, marks, Scope, make, cleanups
tests/grow_buf_test.cpp    GrowBuf: growth, sealing, ownership, moving
tests/stack_set_test.cpp   StackSet, default stacks, threads, plain allocations
tests/ref_test.cpp         Ref and Packed
tests/plain_only.cpp       Scope() and make() with only stack.hpp included
tests/global_buf.cpp       a GrowBuf in a global variable exits cleanly
```

The tests that check that rule violations stop the program use `fork`, so they are
POSIX-only.

## Performance

All numbers below are from one machine: WSL2 (Linux 6.6) on a 16-core x86-64 PC, GCC
13.3 and Clang 18.1, `-O2 -DNDEBUG`. Each program ran 7 times; the first 2 runs are
warm-ups and the table shows the fastest of the rest. Memory is peak resident memory.
Every program printed the same output as Goose's own version. Where both compilers
were measured, the better one is shown.

The benchmarks are [Goose's own](https://github.com/aardappel/goose/tree/master/bench) (`bench/` in the Goose
repository), compared against Goose itself and against its C++ and Rust versions,
plus a few more baselines. topstack's versions use `Ref<T>` links. The bintrees and push
rows come from one run, the other rows from another run on the same machine; the raw
output of both is in `bench/results/`.

| Benchmark | Goose | **topstack** | Best other C++ | Best other Rust |
|---|---|---|---|---|
| **bintrees**, depth 19 | 296 ms, 25.5 MB | **423 ms, 17.8 MB** | 458 ms, 33.6 MB (vector arena) | 501 ms, 25.9 MB (Vec arena) |
| **tree**, depth 24 | 1174 ms, 386 MB | **1189 ms, 386 MB** | 1224 ms, 386 MB (vector arena) | 1290 ms, 386 MB (Vec arena) |
| **interp**, depth 24 | 104 ms, 21.8 MB | **116 ms, 21.6 MB** | 187 ms, 49.4 MB (variant arena) | 194 ms, 37.2 MB (Vec arena) |
| **sexp**, N = 300,000 | 1922 ms, 370 MB | **1700 ms, 379 MB** | 2947 ms, 955 MB (arena + string_view) | 2080 ms, 663 MB (Vec arena) |
| **push**, 64M items | 615 ms, 498 MB | **589 ms, 497 MB** | 578 ms, 498 MB (vector + exact reserve) | 600 ms, 498 MB (Vec + reserve) |

What it shows:

- **Against Goose:** a tie on `tree` and `push`, faster on `sexp`, 1.1x on `interp`,
  and 1.4x on `bintrees`. Memory is the same or lower everywhere except `sexp` (+2%);
  on `bintrees` it is 30% lower.
- **Against the C++ and Rust versions:** topstack is faster on `bintrees`, `tree`,
  `interp` and `sexp`, and uses the same or less memory, often far less. On `push` it
  is within 2% of a `std::vector` with an exact `reserve`, which only works when the
  final size is known in advance and can't keep pointers to elements; the C++ container
  that can (`std::deque`) is 15 to 50% slower.

### Other allocators, on bintrees (depth 19)

| | Time | Memory |
|---|---|---|
| Goose, for reference | 296 ms | 25.5 MB |
| **topstack, `Ref<T>` links** | **423 ms** | **17.8 MB** |
| `std::pmr::monotonic_buffer_resource`, one reused buffer | 473 ms | 49.8 MB |
| `std::pmr::monotonic_buffer_resource`, one per tree | 526 ms | 33.5 MB |
| topstack, 8-byte pointers | 489 ms | 33.8 MB |
| LLVM `BumpPtrAllocator` | 970 to 1086 ms | 33.8 MB |
| LLVM `BumpPtrAllocator`, with tcmalloc \* | 461 ms | 40.5 MB |
| Rust `bumpalo` \* | 478 ms | 33.8 MB |
| C++ new/delete with mimalloc \* | 1373 ms | 33.9 MB |
| C++ new/delete with tcmalloc / jemalloc / glibc \* | 1718 / 2192 / 2338 ms | 41 / 42 / 66 MB |

\* From the other run, in which Goose took 276 ms, so these times are about 7% lower
than they would be in this table's run.

- With 8-byte pointer nodes, topstack is about even with the other arenas; its lead
  comes from 4-byte links, which only work because its memory never moves.
- LLVM's allocator gets and frees its chunks through `malloc`; with glibc's `malloc` on
  this machine that costs it half its speed, with tcmalloc it is on par with the others.
- A better `malloc` makes per-node allocation up to 1.7x faster, but it stays more than
  3x slower than an arena: freeing a tree node by node is the cost, not finding memory.
- Rust's `bumpalo`, on the other tree benchmarks: `tree` 2302 ms / 770 MB, `interp`
  283 ms / 73 MB, `sexp` 2283 ms / 838 MB. Its 8-byte references and doubling chunks
  cost both time and memory.

### Where the rest of Goose's speed comes from

On `bintrees`, the remaining gap is not the allocator. Goose passes the stack's top
into each function and returns it, so it stays in a CPU register for the whole
recursion; a library can only keep it in memory, which costs a store and a load around
every call. Written that way by hand (`bench/bintrees_thread.cpp`), the same benchmark
runs in **214 ms with GCC, 28% faster than Goose** (296 ms in the same run). With Clang it doesn't help, because
Clang doesn't inline recursive calls. Getting this automatically needs compiler support;
see the roadmap.

### Reproducing

From `bench/`, with Goose built at `~/programming/goose/build/goose` (or set `GOOSE=`):

```sh
./report.sh
```

runs everything (about 25 minutes) and saves the output in `bench/results/`. The
individual scripts are `compare.sh` (bintrees, push, std::pmr, Goose's C++ and Rust),
`compare_extra.sh` (LLVM's allocator and `pinned_vector`), `compare_malloc.sh`
(mimalloc, jemalloc, tcmalloc), `compare_more.sh` (tree, interp, sexp) and
`compare_bumpalo.sh` (Rust's bumpalo). Requirements beyond GCC and Clang:

```sh
sudo apt install llvm-18-dev libmimalloc2.0 libjemalloc2 libtcmalloc-minimal4t64
git clone https://github.com/mknejp/vmcontainer.git ~/programming/vmcontainer
curl --proto '=https' -sSf https://sh.rustup.rs | sh        # for the Rust rows
```

C++ programs are linked with `-static-libstdc++` so that loading the C++ standard
library (about 2.4 MB) doesn't count against them; Goose and Rust programs don't pay
it either.

## How it works

- **Reserve, then commit.** Each stack reserves its whole range up front
  (`mmap(PROT_NONE)` on Linux, `VirtualAlloc(MEM_RESERVE)` on Windows), so its base
  address never changes and nothing ever moves. Memory is committed in steps that start
  at 64 KB and double, up to 64 MB at a time, so a big stack needs few system calls
  and a small one stays small. Committed memory only uses RAM once it is written.
- **Allocation** is an aligned bump of the top, compared against a limit. Everything
  rare (committing, alignment above 4 KB, huge requests) is in a separate cold
  function, so the fast path stays a few instructions.
- **Freeing** moves the top back to a mark. Objects with destructors are recorded in a
  small list of cleanup notes, run newest first.
- **GrowBuf** keeps its own copy of the top while it grows, and the stack's limit is
  set to zero so any other allocation on that stack takes the slow path and seals the
  buffer first. That is what makes "only the top grows" cheap to enforce.
- **Stacks are lent by number:** a 64-bit mask of which stacks are busy, and
  `countr_zero` to find the lowest free one, which is Goose's assignment rule done at
  runtime.

## Limitations

- **Windows is untested.** The code paths exist; they have never been compiled.
- **Rule violations are found at runtime,** not compile time as in Goose. `Ref`
  misuse in release builds is only caught with `TOPSTACK_CHECKED_REFS`.
- **No shrinking or freeing out of order.** There is no `pop` for `GrowBuf` yet, and
  no pools for objects that don't die in stack order (Goose's `slot_pool` and
  `slice_pool`).
- **Memory isn't returned to the OS** while a stack is alive, only when it is
  destroyed (a `trim()` is planned).
- **Strings and containers inside nodes** still use `malloc` for their contents.
- **One machine, one OS** for the performance numbers so far.

## Roadmap

- **Compiler tools.** A Clang plugin that checks topstack's rules at compile time,
  starting with `Ref`s in local variables and temporaries, then "only the top grows".
  After that, an LLVM pass that passes the stack's top through calls in registers, as
  Goose does.
- **Library:** `GrowBuf::pop` and `resize`, slot and slice pools, `trim()`, a
  `std::pmr` adapter so strings and vectors inside nodes can live on a stack too, and
  narrow (2-byte) links where they can be proven safe.
- **Real programs:** porting a JSON parser (cJSON) and a small C compiler (chibicc) to
  topstack, to measure more than benchmarks.
- **2-byte links** (`Ref16`): a prototype with relays for far targets cuts node memory
  by a further 28 to 45% on the tree benchmarks.
- **Windows** builds and tests.

## Credits and license

topstack's design follows [Goose](https://github.com/aardappel/goose)'s data stacks, relative
references and stack assignment; the benchmark programs and their checksums come from
Goose's benchmark suite. Files copied from Goose (`bench/bench.h`, the `.goose`
programs and the C++ benchmark sources) are under the Apache License 2.0; `NOTICE`
lists them and the files based on them. See
`bench/LICENSE-goose`.

topstack itself is released under the Apache License 2.0; see `LICENSE` and `NOTICE`.

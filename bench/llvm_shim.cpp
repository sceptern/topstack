// Stand-ins for the only three functions LLVM's BumpPtrAllocator needs from the
// LLVM library (libLLVM), so bintrees_llvm.cpp can be built without linking it.
// Loading libLLVM costs several MB of memory that has nothing to do with the
// allocator; without it, the memory column compares the allocators alone. These
// do what LLVM's own versions do (lib/Support/MemAlloc.cpp, SmallVector.cpp).
#include <llvm/ADT/SmallVector.h>
#include <llvm/Support/MemAlloc.h>

#include <cstdlib>
#include <cstring>
#include <new>

void* llvm::allocate_buffer(size_t Size, size_t Alignment) {
    return ::operator new(Size, std::align_val_t(Alignment));
}

void llvm::deallocate_buffer(void* Ptr, size_t Size, size_t Alignment) {
    (void)Size;   // the sized form needs -fsized-deallocation on clang 18
    ::operator delete(Ptr, std::align_val_t(Alignment));
}

// The allocator keeps its list of chunks in a SmallVector; this grows it.
template <class Size_T>
void llvm::SmallVectorBase<Size_T>::grow_pod(void* FirstEl, size_t MinSize, size_t TSize) {
    size_t cap = 2 * size_t(this->Capacity) + 1;
    if (cap < MinSize) cap = MinSize;
    if (cap > SizeTypeMax()) cap = SizeTypeMax();
    void* elts;
    if (this->BeginX == FirstEl) {
        elts = std::malloc(cap * TSize);
        if (elts) std::memcpy(elts, this->BeginX, size_t(this->Size) * TSize);
    } else {
        elts = std::realloc(this->BeginX, cap * TSize);
    }
    if (!elts) std::abort();
    this->BeginX = elts;
    this->Capacity = static_cast<Size_T>(cap);
}
template class llvm::SmallVectorBase<uint32_t>;

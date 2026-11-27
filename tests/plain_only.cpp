// Only stack.hpp: Scope() and topstack::make must still work.
#include <topstack/stack.hpp>
int main() {
    topstack::Scope scope;
    int* x = topstack::make<int>(5);
    return *x == 5 ? 0 : 1;
}

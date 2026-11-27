// A GrowBuf in a global variable, using the default set. C++ destroys the main
// thread's thread_local objects (the default set) before its globals, so this
// used to stop at exit with a rule violation. It must now exit cleanly.
#include <topstack/grow_buf.hpp>

topstack::GrowBuf<int> global_buf;

int main() {
    for (int i = 0; i < 1000; ++i) (void)global_buf.push(i);
    return global_buf[999] == 999 ? 0 : 1;
}

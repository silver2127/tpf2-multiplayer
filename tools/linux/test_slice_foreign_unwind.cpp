// Deliberately uses the dynamic C++ runtime, like the game. The harness has a
// hidden static runtime, like tpf2_slice.so. Exceptions must cross its DoStep
// wrapper without entering the harness's exception personality or cleanups.
#include <stdexcept>
extern "C" void SliceTestForeignThrow()
{
    throw std::runtime_error("foreign game exception");
}
extern "C" bool SliceTestForeignCatch(void (*callback)())
{
    try { callback(); }
    catch (const std::runtime_error&) { return true; }
    return false;
}

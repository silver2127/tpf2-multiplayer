// Deliberately dynamic C++ runtime, as in the game. Its owner destructor calls
// the adapter's lifecycle cleanup on both normal and exceptional paths.
#include <stdexcept>
extern "C" void PersonMapForeignThrow()
{
    throw std::runtime_error("foreign person-map allocation failure");
}
extern "C" bool PersonMapForeignCatch(void (*callback)())
{
    try { callback(); }
    catch (const std::runtime_error&) { return true; }
    return false;
}
extern "C" void PersonMapForeignScope(void (*callback)(), void (*cleanup)(void*), void* data)
{
    struct Owner {
        void (*cleanup)(void*);
        void* data;
        ~Owner() { cleanup(data); }
    } owner{cleanup, data};
    callback();
}

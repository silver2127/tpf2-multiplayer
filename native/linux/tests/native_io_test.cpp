#include "../src/menu_game_linux.cpp"
#include <cassert>

namespace panel { bool SetActionsHeld(bool) { return true; } }
static bool originalDoneCalled=false;
static void OriginalDone(void*,const bool*,const void*) { originalDoneCalled=true; }
static void* Factory(void* out,void*,void*,void* raw,void*,bool automatic,bool flag) {
    auto& s=*static_cast<GStr*>(raw);
    assert(std::string(s.p,s.len)=="mp_saved" && !automatic && flag);
    return out;
}
int main() {
    using namespace NativeIo;
    assert(ValidName("mp_a") && ValidName("mp_123456789012"));
    for(const auto name:{"save","../mp_a","mp_a/b","mp_","mp_1234567890123"})assert(!ValidName(name));
    assert(!PauseAndDrain("pause") && !HasWorld() && !Busy());
    // libstdc++ owns copies of the closure; our manager retains the ticket
    // until all copies have been destroyed by the engine.
    auto* ticket=new Ticket;ticket->world=1;ticket->id="pause";
    Function a{},b{};a.data[0]=ticket;
    Manager(&b,&a,2); assert(ticket->refs==2 && b.data[0]==ticket);
    void* target=nullptr;Manager(&target,&a,1);assert(target==&a);
    Manager(&b,&b,3);assert(ticket->refs==1);Manager(&a,&a,3);
    SaveCall call{};call.name.p=call.name.buf;call.name.len=8;memcpy(call.name.buf,"mp_saved",9);
    ownSave=&call;saveFactoryOriginal=reinterpret_cast<void*>(&Factory);
    assert(SaveFactoryHook(&call,nullptr,nullptr,nullptr,nullptr,true,true)==&call && call.captured);
    ownSave=nullptr;
    saveDoneOriginal=reinterpret_cast<void*>(&OriginalDone);
    C().state=State::Saving;C().operation="save";C().requestedWorld=42;
    uintptr_t closure=41;bool ok=true;
    SaveDoneHook(&closure,&ok,nullptr);assert(originalDoneCalled && Busy());
    closure=42;SaveDoneHook(&closure,&ok,nullptr);assert(!Busy());
    Event e;assert(Poll(e)&&e.operation=="save"&&e.step=="saved"&&e.success);
    assert(!Poll(e));
    // A pause is acknowledged from the engine callback, only after its FIFO
    // command list is empty, never when the request was merely enqueued.
    alignas(8) unsigned char world[0x450]{},game[0x160]{},queue[8]{},data[24]{},command[0x38]{};
    Field<uintptr_t>(uintptr_t(world),0x448)=uintptr_t(game);
    Field<uintptr_t>(uintptr_t(game),0x158)=uintptr_t(queue);
    Field<uintptr_t>(uintptr_t(queue),0)=uintptr_t(data);
    C().world=uintptr_t(world);C().state=State::Pausing;C().operation="pause";
    ticket=new Ticket;ticket->world=C().world;ticket->id="pause";ticket->pass=64;
    command[0x30]=1;PauseDone(&ticket,command);
    assert(Poll(e)&&e.step=="paused"&&e.success&&!Busy());
    C().state=State::Pausing;Field<uintptr_t>(uintptr_t(data),8)=8;
    PauseDone(&ticket,command);
    assert(Poll(e)&&!e.success&&e.detail=="Command producers did not become idle");
    delete ticket;
}

// Drives the actual Add detour against fake game functions. This checks the
// commit ordering, not just a decoder's mocked cancellation contract.
#include "../../src/hook.h"
static bool InstallerHook(uintptr_t, void*, int, void**);
#define InstallHook InstallerHook
#include "../src/slice/slice_install.cpp"
#undef InstallHook
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <cassert>
#include <string>
#include <stdexcept>
#include <vector>

static uintptr_t testFailedHook;
static std::vector<void*> testTrampolines;
static bool InstallerHook(uintptr_t target, void* detour, int steal, void** trampoline)
{
    if (target==testFailedHook) {
        // A failed patch may retain a trampoline; that pointer is not success.
        *trampoline=reinterpret_cast<void*>(uintptr_t(1));
        return false;
    }
    const bool installed=InstallHook(target,detour,steal,trampoline);
    if (*trampoline) testTrampolines.push_back(*trampoline);
    return installed;
}

static int testNative, testPrepared, testFired, testAfter;
static bool testCanCommit;
static bool testNativeThrows, testOverrideResult;
static void* testDoneSeen;
static SliceOutcome testOutcome;
static void* TestNative(void* ret, void*, void*, void* done, void*)
{ ++testNative; testDoneSeen = done; if(testNativeThrows) throw std::runtime_error("native Add failed"); return ret; }
static void TestDone(void*, void*) { ++testFired; }
static void TestManager(void*, const void*, int) {}
static bool TestPrepare(const SliceAddCall&, void*) { ++testPrepared; return testCanCommit; }
static void TestLanded(const SliceAddCall*, SliceOutcome outcome, void*) { testOutcome = outcome; }
static void TestAfter(void*, void*) { ++testAfter; }
static void TestFactory(const SliceFactoryCall&, void*) {}
static uintptr_t testAlternate[4] = {};
static void* testOverrideCandidate = testAlternate;
static void TestObserve(const SliceAddCall& a, void*)
{ testOverrideResult=SliceOverrideAddDone(a, testOverrideCandidate, TestAfter); }

static void TestArm(void* cmd)
{
    t_arm = {};
    t_arm.active = true; t_arm.cmd = cmd; t_arm.tag = 3;
    t_arm.factory = "test";
    t_arm.arm = {"test", SliceDone::Required, false, nullptr, TestLanded, nullptr, TestPrepare};
    testNative = testPrepared = testFired = testAfter = 0;
}

static void TestInstallReadiness()
{
    // Only these pages receive bytes; the rest is an anonymous address range
    // matching the real RVAs. No copied game function is ever executed.
    const size_t size=kSliceRvaConnectionCtor+4096;
    auto* image=static_cast<uint8_t*>(mmap(nullptr,size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
    assert(image!=MAP_FAILED);
    const auto savedEnv=SliceCoreEnvGet();
    auto& env=const_cast<SliceCoreEnv&>(SliceCoreEnvGet());
    env.base=uintptr_t(image);env.exec[0]={env.base,env.base+size};env.nExec=1;
    const auto* factory=SliceFactoryByRva(0x15eb600);
    assert(factory && !factory->stackArgs);
    const uintptr_t auxiliaryRva=0x2000000;
    uint8_t auxiliary[14];memset(auxiliary,0x90,sizeof(auxiliary));
    enum Case {Good, BadConnection, BadAdd, BadFactory, BadAuxiliary, FailedAdd, FailedFactory,
               FailedAuxiliary, RefusedFactory, RefusedObserver, RefusedAuxiliary, RegistrationOpen, NoReads};
    for (Case which : {Good,BadConnection,BadAdd,BadFactory,BadAuxiliary,FailedAdd,FailedFactory,
                       FailedAuxiliary,RefusedFactory,RefusedObserver,RefusedAuxiliary,RegistrationOpen,NoReads}) {
        assert(mprotect(image,size,PROT_READ|PROT_WRITE)==0);
        memcpy(image+kSliceRvaConnectionCtor,kSliceConnectionCtorBytes,sizeof(kSliceConnectionCtorBytes));
        memcpy(image+kSliceRvaAdd,kSliceAddPrologue,sizeof(kSliceAddPrologue));
        memcpy(image+factory->rva,factory->prologue,factory->steal);
        memcpy(image+auxiliaryRva,auxiliary,sizeof(auxiliary));
        g_phase=kPhaseIdle;g_registrationFailures=0;g_nHooks=g_nObservers=0;
        g_addInstalled=false;g_connectionOk=false;g_addTramp=nullptr;
        for (auto& slot:g_slots) {
            slot.nHandlers=0;slot.anyArms=slot.anyReturn=false;slot.installed=false;slot.tramp=nullptr;
        }
        SliceCoreBeginRegistration();
        assert(SliceOnFactory({"test",factory->rva,TestFactory,nullptr,nullptr,true,0}));
        void* trampoline=nullptr;
        const SliceHookSpec spec={"test","required auxiliary",-1,auxiliaryRva,auxiliary,sizeof(auxiliary),
                                  14,reinterpret_cast<void*>(TestDone),&trampoline,true};
        assert(SliceRegisterHook(spec));
        if(which==RefusedFactory) assert(!SliceOnFactory({"test",UINTPTR_MAX,TestFactory,nullptr,nullptr,true,0}));
        if(which==RefusedObserver) assert(!SliceOnAdd("test",nullptr,nullptr));
        if(which==RefusedAuxiliary) assert(!SliceRegisterHook(spec)); // overlapping duplicate
        if(which!=RegistrationOpen) SliceCoreEndRegistration();
        if(which==BadConnection) image[kSliceRvaConnectionCtor]^=0xff;
        if(which==BadAdd) image[kSliceRvaAdd]^=0xff;
        if(which==BadFactory) image[factory->rva]^=0xff;
        if(which==BadAuxiliary) image[auxiliaryRva]^=0xff;
        assert(mprotect(image,size,PROT_READ|PROT_EXEC)==0);
        testFailedHook=which==FailedAdd ? env.base+kSliceRvaAdd :
            which==FailedFactory ? env.base+factory->rva : which==FailedAuxiliary ? env.base+auxiliaryRva : 0;
        const int reads=SliceReadMechanism();
        if(which==NoReads) SliceReadSelect(kSliceReadNone);
        const auto report=SliceCoreInstall();
        assert(report.ready==(which==Good));
        if(which==Good) {
            assert(report.connectionOk && report.addInstalled && report.factoriesInstalled==1 && report.hooksInstalled==1);
            assert(SliceHookInstalled(auxiliaryRva));
            assert(!SliceCoreInstall().ready); // An ignored second install is not a successful report.
        }
        if(which==BadConnection || which==NoReads) assert(!report.connectionOk && !report.addInstalled);
        if(which==BadAdd || which==FailedAdd) assert(report.connectionOk && !report.addInstalled);
        if(which==BadFactory || which==FailedFactory) assert(report.factoriesSkipped==1 && report.factoriesInstalled==0);
        if(which==BadAuxiliary || which==FailedAuxiliary) {
            assert(report.hooksSkipped==1 && !SliceHookInstalled(auxiliaryRva));
            if(which==FailedAuxiliary) assert(trampoline==reinterpret_cast<void*>(uintptr_t(1)));
        }
        if(which==RefusedFactory || which==RefusedObserver || which==RefusedAuxiliary) {
            assert(g_registrationFailures==1 && report.factoriesInstalled==1 && report.hooksInstalled==1);
        }
        SliceReadSelect(reads);
        for (void* p:testTrampolines) assert(munmap(p,4096)==0);
        testTrampolines.clear();
    }
    testFailedHook=0;g_nHooks=g_nObservers=0;
    env=savedEnv;
    assert(munmap(image,size)==0);
}

static void TestMissingLockRefusal()
{
    char temporary[]="/tmp/tpf2-nolock.XXXXXX";
    assert(mkdtemp(temporary));
    const std::string dir=std::string(temporary)+"/";
    // An unusable lock path with an otherwise writable log directory must
    // refuse startup. Run the core-open seam in a child to preserve its parent's
    // existing process-lifetime lock and log descriptors.
    assert(mkdir((dir+"tpf2_slice.lock").c_str(),0700)==0);
    const pid_t child=fork();
    assert(child>=0);
    if(!child) {
        auto env=SliceCoreEnvGet();env.dataDir=dir.c_str();env.rootDir=temporary;
        _exit(SliceCoreOpen(env)==SliceOpenResult::NoLock ? 0 : 1);
    }
    int status=0;
    assert(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);
    assert(unlink((dir+"tpf2_slice.log").c_str())==0);
    assert(rmdir((dir+"tpf2_slice.lock").c_str())==0 && rmdir(temporary)==0);
}

int main()
{
    char temp[] = "/tmp/tpf2-cancel.XXXXXX";
    assert(mkdtemp(temp));
    const std::string dataDir = std::string(temp) + "/";
    auto* ctor = static_cast<unsigned char*>(mmap(nullptr, 8192, PROT_READ | PROT_WRITE,
                                                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    assert(ctor != MAP_FAILED);
    // Connection constructor stand-in: write a nonempty return slot.
    const unsigned char instructions[] = {0x48,0xc7,0x07,1,0,0,0,0xc3};
    memcpy(ctor, instructions, sizeof(instructions));
    memset(ctor+128,0x90,16);
    assert(mprotect(ctor, 4096, PROT_READ | PROT_EXEC) == 0);
    assert(mprotect(ctor+4096,4096,PROT_NONE)==0);
    SliceCoreEnv env{};
    env.base = (uintptr_t)ctor - kSliceRvaConnectionCtor;
    env.buildOk = true; env.dataDir = dataDir.c_str(); env.rootDir = temp;
    env.exec[0]={(uintptr_t)ctor,(uintptr_t)ctor+8192};env.nExec=1;
    env.code[0] = {(uintptr_t)&TestDone, (uintptr_t)&TestDone + 128};
    env.code[1] = {(uintptr_t)&TestManager, (uintptr_t)&TestManager + 128};env.nCode = 2;
    assert(SliceCoreOpen(env) == SliceOpenResult::Ok);
    assert(SliceReadInit());
    g_addInstalled = true; g_connectionOk = true; g_addTramp = (void*)&TestNative;
    alignas(16) unsigned char payload[0xd50] = {};
    payload[0xd48] = 3;
    uintptr_t command[7] = {(uintptr_t)payload};
    uintptr_t done[4] = {0,0,1,(uintptr_t)&TestDone};
    uintptr_t ret = 0;

    TestArm(command); testCanCommit = false;
    assert(AddDetour(&ret, nullptr, command, done, nullptr) == &ret);
    assert(testPrepared == 1 && testNative == 1 && testFired == 0 && ret == 0);
    assert(testOutcome == SliceOutcome::RanNatively);

    TestArm(command); testCanCommit = true;
    AddDetour(&ret, nullptr, command, done, nullptr);
    assert(testPrepared == 1 && testNative == 0 && testFired == 1 && ret == 1);
    assert(testOutcome == SliceOutcome::CancelledFired);

    TestArm(command); done[2] = 0;
    AddDetour(&ret, nullptr, command, done, nullptr);
    assert(testPrepared == 0 && testNative == 1 && testFired == 0);
    TestArm(command); done[2] = 1; done[3] = 1;
    AddDetour(&ret, nullptr, command, done, nullptr);
    assert(testPrepared == 0 && testNative == 1 && testFired == 0);
    TestArm(command); done[3] = (uintptr_t)&TestDone; payload[0xd48] = 4;
    AddDetour(&ret, nullptr, command, done, nullptr);
    assert(testPrepared == 0 && testNative == 1 && testFired == 0);
    payload[0xd48] = 3;

    // A script callback override changes only the argument seen by Add.
    t_arm = {}; g_nObservers = 1;
    g_observers[0] = {"test", TestObserve, nullptr};
    memcpy(testAlternate, done, sizeof(done));
    testAlternate[2]=(uintptr_t)&TestManager;
    AddDetour(&ret, nullptr, command, done, nullptr);
    assert(testOverrideResult && testDoneSeen == testAlternate && testAfter == 1 && done[2] == 1);
    SliceAddCall outside{};
    assert(!SliceOverrideAddDone(outside, testAlternate));

    // The override validates both executable function pointers and the complete
    // _Any_data, not only the readable manager/invoker pair at its tail.
    testAfter=0;testAlternate[2]=1;
    AddDetour(&ret,nullptr,command,done,nullptr);
    assert(!testOverrideResult && testAfter==0 && testDoneSeen==done);
    testAlternate[2]=(uintptr_t)&TestManager;
    auto* split=static_cast<unsigned char*>(mmap(nullptr,8192,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
    assert(split!=MAP_FAILED);
    memcpy(split+4096-16,testAlternate,32);
    assert(mprotect(split,4096,PROT_NONE)==0);
    testOverrideCandidate=split+4096-16;
    AddDetour(&ret,nullptr,command,done,nullptr);
    assert(!testOverrideResult && testAfter==0 && testDoneSeen==done);
    munmap(split,8192);testOverrideCandidate=testAlternate;

    TestArm(command);testCanCommit=true;
    AddDetour(&ret,nullptr,command,done,nullptr);
    assert(!testOverrideResult && testAfter==0 && testOutcome==SliceOutcome::CancelledFired);
    t_arm={};testNativeThrows=true;
    try {AddDetour(&ret,nullptr,command,done,nullptr);assert(false);}catch(const std::runtime_error&){}
    assert(testOverrideResult && testAfter==0 && t_observedAdd==nullptr && t_overrideDone==nullptr);
    testNativeThrows=false;
    AddDetour(&ret,nullptr,command,done,nullptr);assert(testAfter==1);

    // Registration cannot wrap back into an owned target or dereference an
    // unreadable expected buffer. Guarded live comparison refuses PROT_NONE.
    SliceCoreBeginRegistration();
    const uint8_t nop[5]={0x90,0x90,0x90,0x90,0x90};void* auxiliaryTrampoline=(void*)1;
    const uintptr_t auxiliaryRva=kSliceRvaConnectionCtor+128;
    SliceHookSpec spec={"test","readiness",-1,auxiliaryRva,nop,sizeof(nop),5,(void*)&TestDone,&auxiliaryTrampoline,false};
    assert(SliceRegisterHook(spec));assert(!SliceHookInstalled(auxiliaryRva));
    spec.rva=UINTPTR_MAX;assert(!SliceRegisterHook(spec));
    spec.rva=auxiliaryRva+32;spec.expectedLen=SIZE_MAX;assert(!SliceRegisterHook(spec));
    spec.expectedLen=sizeof(nop);spec.expected=(const uint8_t*)1;assert(!SliceRegisterHook(spec));
    assert(LiveBytesMatch(auxiliaryRva,nop,sizeof(nop)));
    assert(!LiveBytesMatch(auxiliaryRva,(const uint8_t*)1,sizeof(nop)));
    assert(!LiveBytesMatch(kSliceRvaConnectionCtor+4096,nop,sizeof(nop)));
    g_phase.store(kPhaseFrozen);
    assert(auxiliaryTrampoline && !SliceHookInstalled(auxiliaryRva)); // retained failed-patch trampoline
    g_hooks[0].installed.store(true);assert(SliceHookInstalled(auxiliaryRva));
    assert(!SliceHookInstalled(auxiliaryRva+1));

    // Early roster is live even before the first peer heartbeat (0.4.22).
    FILE* f = fopen((dataDir + "tpf2_instance.txt").c_str(), "w");
    assert(f); fprintf(f, "a\npid=%d\n", (int)getpid()); fclose(f);
    f = fopen((dataDir + "lockstep_status_a.txt").c_str(), "w");
    assert(f); fputs("t=0  peer=?  mp=2\n", f); fclose(f);
    SliceSessionCacheReset(); assert(SliceSessionLive());
    // A missing/empty/stale status or peer timeout must never permit native work.
    assert(unlink((dataDir + "lockstep_status_a.txt").c_str()) == 0);
    SliceSessionCacheReset(); assert(SliceSessionLive());
    f = fopen((dataDir + "lockstep_status_a.txt").c_str(), "w");
    assert(f); fclose(f);
    SliceSessionCacheReset(); assert(SliceSessionLive());

    for (const char* partial : {"t=100  peer=?  skew=?", "t=100  peer=?  mp=", "t=100  peer=?  mp=1junk"}) {
        f = fopen((dataDir + "lockstep_status_a.txt").c_str(), "w");
        assert(f); fputs(partial, f); fclose(f);
        SliceSessionCacheReset(); assert(SliceSessionLive());
    }

    g_nObservers = 0;
    done[2] = (uintptr_t)&TestManager; done[3] = (uintptr_t)&TestDone;
    TestArm(command); testCanCommit = false;
    AddDetour(&ret, nullptr, command, done, nullptr);
    assert(testPrepared == 1 && testNative == 0 && testOutcome == SliceOutcome::Blocked);
    TestArm(command); done[3] = 1;
    AddDetour(&ret, nullptr, command, done, nullptr);
    assert(testPrepared == 0 && testNative == 0 && testFired == 0 && testOutcome == SliceOutcome::Blocked);
    done[3] = (uintptr_t)&TestDone;
    TestArm(command); payload[0xd48] = 4;
    AddDetour(&ret, nullptr, command, done, nullptr);
    assert(testPrepared == 0 && testNative == 0 && testOutcome == SliceOutcome::Blocked);
    payload[0xd48] = 3;

    // The default barrier works without a decoder or a registered factory hook.
    t_arm = {}; testNative = testFired = 0;
    assert(SlicePlayerAddSite(0xe34895)); // ConstructionBuilder
    assert(SlicePlayerAddSite(0xfcf4c8)); // loan click (Book has stack arguments)
    AddDispatch(&ret, nullptr, command, done, nullptr, env.base + 0xe34895);
    assert(testNative == 0 && testFired == 0);
    // Script replays and deterministic engine work bypass the player barrier.
    AddDispatch(&ret, nullptr, command, done, nullptr, env.base + 0xa2f5c2);
    assert(testNative == 1);
    assert(!SlicePlayerAddSite(0x2fbb309));
    AddDispatch(&ret, nullptr, command, done, nullptr, env.base + 0x2fbb309);
    assert(testNative == 2);

    // The existing fresh solo status releases the latch without new Lua fields.
    f = fopen((dataDir + "lockstep_status_a.txt").c_str(), "w");
    assert(f); fputs("t=0  peer=?  mp=1\n", f); fclose(f);
    SliceSessionCacheReset(); assert(!SliceSessionLive());
    AddDispatch(&ret, nullptr, command, done, nullptr, env.base + 0xe34895);
    assert(testNative == 3);

    SliceRecord record{};
    SliceRecordPrintf(&record, "VREV 12345\n");
    assert(SliceInjectWrite(record, SliceArmedLine::Zero) == SliceInjectResult::NotWritten);
    assert(SliceInjectWrite(record, SliceArmedLine::One) == SliceInjectResult::Written);
    SliceRecordFree(&record);
    TestInstallReadiness();
    TestMissingLockRefusal();
    puts("PASS: strict barrier, session latch, callback ownership and required-hook installer readiness");
    munmap(ctor, 8192);
}

# dev 192ecd4d: CrossOver loader fix and native Vulkan scope

The Windows change `91bc601` relocates RIP-relative instructions when hooking
Wine's `vkGetDeviceProcAddr`. It patches a loader DLL, not TransportFever2.exe.
Native Linux does not compile `native/src/hook.cpp` or patch that loader entry.
No equivalent decoder or loader trampoline needs adding to Linux.

## Existing native mapping, rechecked

Read-only inspection of the lab's `native/game/TransportFever2` confirmed GNU
build-id `3a0e156390b0e6f1e372051c24802c8493ae454a`, Steam build 35924.
The existing mapping is explained in [DEV_84058DA7.md](DEV_84058DA7.md).
`native/linux/src/overlay_vk_linux.cpp` redirects the game's dispatcher-init
call and then replaces validated function-pointer slots. It never copies
instructions from `vkGetDeviceProcAddr`, so the loader's prologue is irrelevant.

Fresh objdump evidence (PIE-relative virtual addresses):

- `0x35096d0`: `48 8b b5 40 ec ff ff`, load device into rsi.
- `0x35096de`: `48 8d 3d 5b 40 63 02`, load dispatcher `0x5b3d740` into rdi.
- `0x35096e9`: `e8 e2 d0 00 00`, call `0x35167d0`.
- `0x35167da`: `49 89 f4`, preserve device in r12;
  `0x35167e5`: `48 89 fb`, preserve dispatcher in rbx.
- `0x35167e8`: `4c 89 e7`, device argument back into rdi;
  `0x35167eb`: `ff 93 98 07 00 00`, call `[rbx+0x798]`.
  rsi holds the function-name string loaded at `0x35167dd`.

This is the existing SysV `void init(void* dispatcher, VkDevice device)`
contract, followed by `vkGetDeviceProcAddr(device, name)`. No Windows object
layout, MSVC string/vector representation, or new offset is introduced.
`Tpf2mpRedirectCall` checks opcode E8 and the decoded destination before
writing the displacement, checks near-stub range, and fails closed.
`InitDeviceDetour` calls the real initializer and verifies the present,
swapchain and queue slots against fresh `gdpa` lookups before replacing them.
These guards remain unchanged.

Disassembly transcript: `.git/port-192ecd4-disassembly.log` in this clone.
No game launch or gdb probe was needed: this integration adds no native patch
or ownership contract. The static check establishes applicability, not a new
live overlay result. No CrossOver execution or Mac validation was performed.

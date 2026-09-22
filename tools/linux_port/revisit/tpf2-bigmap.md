Left unported by the static-only runs of 2026-09-16 (`docs/linux/PORT.md`, "partial"):

1. **The native minimap** (Windows: `ImageView::SetImage` detour serving a native texture to the embedded Lua
   GUI; the Lua GUI, embedding tool and tests are merged, `minimap=1` logs "unsupported"). Open: the Linux
   `ImageView:setImage` Lua binding's string overload (the candidate failed the single-string check) and its
   delegation to the resource overload; the UI+0x450 -> terrain accessor (prove the virtual slot and terrain
   offset on Linux instead of importing Windows vtable slot 1 / state+0x20); terrain origin, height scale and
   water field; raw texture upload lifetime. Live plan: run the native lab instance with the Lua GUI installed,
   break in the setImage binding when the script calls it with the minimap token, read the string and the
   call chain, then follow the UI object to the terrain under gdb; implement the guarded Linux hooks and watch
   the minimap render.

Out of scope: nothing else is open.

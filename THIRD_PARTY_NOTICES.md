# Third-party notices

This project is licensed under the MIT License (see `LICENSE`). It includes or
depends on the following third-party material.

## Vendored

- **Vulkan headers** (`bridge/src/vk/`) — Copyright 2015-2026 The Khronos
  Group Inc. Licensed under `Apache-2.0 OR MIT` (SPDX identifiers are in each
  file). Used unmodified by the in-game overlay.

## Dependencies (not vendored; installed via `netpunch/requirements.txt` or
bundled into the frozen `netpunch.exe` by PyInstaller)

- **pystun3** — MIT License. STUN client used for NAT discovery.
- **miniupnpc** — BSD 3-Clause License. UPnP port mapping.
- **PyInstaller** (build-time only) — GPL-2.0 with the bootloader exception;
  the frozen executable is not subject to the GPL.

## Not included

- **Transport Fever 2** (Urban Games) is not part of this project and is not
  redistributed. The installer modifies the player's own installation (it
  renames the stock `alut.dll` to `alut_real.dll` and installs a forwarding
  proxy); Steam's "Verify integrity of game files" restores the original.
  This project is not affiliated with or endorsed by Urban Games.
- `alut.dll` shipped with the game is **freealut** (LGPL). The proxy forwards
  every export to the original library and contains no freealut code.

# zstd 1.5.7 (vendored)

`lib/` of the official release `zstd-1.5.7.tar.gz`
(<https://github.com/facebook/zstd/releases/tag/v1.5.7>, SHA-256
`eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3`), unmodified:
`common/`, `compress/`, `decompress/` (without the x86-64 `.S` Huffman decoder, which
MSVC cannot assemble; `ZSTD_DISABLE_ASM` selects the C one), `zstd.h`, `zstd_errors.h`.
BSD licence, [LICENSE](LICENSE).

Used only by the Windows `tpf2_slice.dll`, built with `ZSTD_MULTITHREAD`: the game's
save stream is compressed on worker threads (`native/src/slice/save_zstd.inl`). The
Linux build loads the system `libzstd.so.1` instead. `decompress/` serves the build's
round-trip self-test.

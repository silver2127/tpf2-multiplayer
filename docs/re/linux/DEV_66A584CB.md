# Steam rate and quality diagnostics: dev 66a584cb

The incoming commit changes configuration values and diagnostic formatting only.
There are no new engine addresses, instruction patterns, game structures,
hooks or calling conventions to derive. The shared Linux Steam flat-C adapter
and exported-symbol mapping from [1eb30002](DEV_1EB30002.md) remain applicable.

The vendored `native/third_party/steam/steamnetworkingtypes.h`, lines 1232–1239,
defines SendRateMin=10 and SendRateMax=11 as int32 bytes/sec settings and
documents equal clamps for a manually configured rate. Both native values are
now 16*1024*1024. The existing global setter uses scope=1, object=0, type=1;
the native fixture checks these arguments, the allowed IDs and all four values
after tunnel shutdown, in both transport modes.

The shared Messages adapter reads `m_flConnectionQualityLocal` and
`m_flConnectionQualityRemote` from the existing vendored
`SteamNetConnectionRealTimeStatus_t` returned by GetSessionConnectionInfo.
It passes them through normal C++ float-to-double varargs promotion to %.3f.
There are no hand-coded offsets or MSVC objects crossing a new ABI boundary.
The adapter fixture supplies 0.875 and 0.625 respectively and checks the
actual formatted log, alongside existing state/queue mapping assertions.

The lab launch failed before game exec (bubblewrap uid-map permission error).
There is no new live Steam observation or throughput result. This does not
leave an engine patch or lifetime contract unresolved: none is added by this
commit. Real-peer rate and quality behavior remains unverified.

# Phase 6 Debug Test Guidance

Keep Launch catalog, DAP codec, session, adapter-process, and native Debug UI
proofs separate. Passing a lower layer is not evidence for a higher layer.

- Launch catalog tests cover whole-catalog atomic validation, bounded counts and
  identifiers, duplicate names/references, unknown compound references,
  last-good retention, generation/revision fencing, explicit Clear, and Stop.
- Codec tests feed arbitrary chunk boundaries and multiple frames; cover UTF-8,
  strict JSON, header/body/depth/count limits, invalid envelopes, no
  resynchronization, completed-before-failure retention, canonical round trips,
  Stop, and explicit Reset.
- Session tests must use an injected fake transport and cover sequence
  allocation, response correlation, server requests/events, pending capacity,
  caller-driven timeout/cancel, fragmented input, transport/codec failure,
  concurrent physical-send serialization, sequence exhaustion,
  reentrant/throwing listeners, callback-originated deferred Stop, external
  callback-drain waits, and exact once-only close/finalization.
- Debug Console tests cover strict UTF-8/enums, generation fencing, operation
  replay/conflict, sequence exhaustion, transactional resource exhaustion,
  caller-driven expiry, bounded/saturated drops, retained retry ownership on
  DisposeSession failure, and external/reentrant Stop.
- Add process/pipe tests only with bounded startup and shutdown deadlines.
  After every run, prove no adapter, test, or editor process survived.

The 2026-07-31 pure backend cohort
`LaunchConfigurationCatalog.*:DapProtocolCodec.*:DapSession.*:
DebugConsoleModel.*` passes 44/44. The DAP session uses a fake byte transport
and the Debug Console is a pure model; this does not claim a production adapter
process/pipe, initialize/launch handshake, debug-state controller, evaluation
adapter, or native Debug Console.

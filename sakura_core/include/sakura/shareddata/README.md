# Shared-data capability contract

`DLLSHAREDATA` remains a legacy cross-process mapping. It has one physical
owner, `CShareData`, which maps and unmaps it with the process lifetime. The
capability objects in this directory are non-owning, short-lived views: callers
must not retain one beyond that mapping lifetime or across `CShareData` shutdown.

| Field group | State owner | Reader snapshot | Writer | Terminal/lifetime rule |
| --- | --- | --- | --- | --- |
| Mapping header | `CShareData` mapping setup | `SharedDataHeaderSnapshot` | none | Created before clients open the mapping; immutable afterwards. |
| Macro flags | macro command lifecycle | `SharedDataMacroSnapshot` | `SharedDataMacroWriter` | `StartRecording` establishes owner; `StopRecording` clears flag and endpoint together. |
| Window endpoints | control/tray process lifecycle | `SharedDataWindowEndpointSnapshot` | `SharedDataWindowEndpointWriter` | Facade stores opaque values only; it never creates, closes, or dispatches a window. |
| Type and print-lock counters | `CShareData` settings lifecycle | `SharedDataSettingsSnapshot` | `SharedDataSettingsWriter` | Type count is explicitly assigned; print locks are only incremented/decremented under `CShareDataLockCounter`'s mutex. |

`TryOpenSharedDataCapabilities()` is the sole optional-open boundary. It makes
the uninitialized terminal state visible to callers without creating a general
application context or service locator. Existing paths that require an
initialized mapping retain their explicit failure behavior.

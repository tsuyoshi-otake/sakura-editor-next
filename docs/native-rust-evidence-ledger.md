# Native Rust migration evidence ledger

Append new runs and decisions with their source, configuration, command, and
artifact identities. Record corrections in new entries and link the entry
they amend. Keep raw evidence references free of document content.

## #239 / #267 G0 disposition

C++ owns production dispatch for all nine UTF-16 scan cells. Rust production
adoption is **NO-GO**. The Rust kernels are retained for differential tests and
explicit non-production MSVC actual-caller comparisons.

| Operation | AVX128 | AVX2 | AVX-512BW |
| --- | --- | --- | --- |
| CR/LF | C++ / Rust NO-GO | C++ / Rust NO-GO | C++ / Rust NO-GO |
| Markdown inline-special | C++ / Rust NO-GO | C++ / Rust NO-GO | C++ / Rust NO-GO |
| Find-char | C++ / Rust NO-GO | C++ / Rust NO-GO | C++ / Rust NO-GO |

Commit `ecd7018f9` bundled the historical Rust-only MSVC switch with Workbench
changes. Commit `2c0036f2a` restored C++ production authority and an explicit
Rust comparison path. This disposition contains no adoption flip. G0 closes
on the existing NO-GO evidence. The clean hosted A/B rerun and physical AMD
and Intel reruns are deferred to any future Rust UTF-16 adoption review.
[#267 G0](https://github.com/tsuyoshi-otake/sakura-editor-next/issues/267)

### Historical evidence

| Field | #239 Phase 8 corrected comparison | #239 Phase 7 production package |
| --- | --- | --- |
| Entry ID | 239-phase-8-corrected | 239-phase-7-package |
| Original run IDs | unavailable (not in #239) | unavailable (not in #239) |
| Revision | unavailable (not in #239) | unavailable (not in #239) |
| Artifact and SHA-256 | unavailable (not in #239) | `sakura.exe`, `5C13047BE874906CFD6437248A86D6FA8C60456838B355F7C30BB82609446599` in staging, ZIP and installer payload |
| Configuration | unavailable (not in #239) | x64 Release |
| Backend/provider | C++ and Rust comparison | `SAKURA_UTF16_BACKEND=cpp` |
| Host, CPU model, microcode and OS build | AVX-512-capable host. Exact metadata unavailable (not in #239). | unavailable (not in #239) |
| Raw CPU feature and OS-state masks | unavailable (not in #239) | unavailable (not in #239) |
| Toolchain for the run | unavailable (not in #239) | unavailable (not in #239) |
| Exact command | unavailable (not in #239) | unavailable (not in #239) |
| Raw non-confidential artifact location | unavailable (not in #239) | unavailable (not in #239) |
| Reported summary | 98,658 direct samples from nine pinned processes, with 100% caller-histogram weight coverage in every cell. 522 actual-caller samples from nine reversed-order C++/Rust process pairs. Pair-level median **+1.83%**, p95 **+19.03%**. No cell met its gate. | The three application copies matched. The executable imported no Rust runtime DLL and passed isolated package launch smoke. Phase 8/9 did not rerun full packaging, so this hash identifies the Phase 7 payload. |

The caller median passed the ≤2% regression limit and p95 missed the ≤5% limit.
Every cell failed the combined direct requirements of at least 5% weighted
median improvement, a 95% confidence interval excluding parity, and no
material reversal. The AVX-512BW candidate also lost to Rust AVX2 on the
measured host for CR/LF (−33.33%), Markdown inline-special (−41.93%), and
find-char (−20.56%). The final local review identifies Rust 1.96.0 for the
audited static library, with nine versioned scan symbols.
[#239 final local review](https://github.com/tsuyoshi-otake/sakura-editor-next/issues/239#issuecomment-5365951669)

## 2026-11-22 / #433 G2.4 byte CR/LF policy

The three Rust byte CR/LF exports are an accepted G2 exception for
comparison-only use. They use the C++-owned CPU/OS capability snapshot.
`CpuDispatch` selects C++ scanners for production byte CR/LF dispatch and
exposes the Rust adapters through `CpuDispatch::Testing` for comparisons.
Before any production dispatch through these exports, add a versioned byte
operation policy with an operation ID and minimum length. The existing ABI V1
policy slots cover the three UTF-16 operations.
The source guard is `test_byte_simd_candidates_are_comparison_only` in
`src/test/py/test_native_rust_exports.py`.
[#433](https://github.com/tsuyoshi-otake/sakura-editor-next/issues/433)

## 2026-11-23 / #435 item 3 / Option A: native worker lifecycle

The Rust worker-thread ABI (`sakura_worker_thread_start_v2` and
`sakura_worker_thread_join_v2`) is production infrastructure in all six build
contexts. On MinGW, `native-worker-lifecycle` is the sole Rust production
authority domain. The same ABI is linked from the native archive for each
toolchain.

The three C++ service owners own their queues, stop signals, cancellation, and
callback context lifetimes. They are `CControlSenpComposition` (control-process
SENP worker), `CSenpGitHubToolExecutor` (GitHub tool execution worker), and
`CSenpControlToolReads` (editor-process control tool reads worker).
`CNativeWorkerThread` owns the move-only token and consumes it on join or
destruction. Rust owns the thread-handle registry, claims each handle exactly
once, yields one join terminal per token, and reports body failures as redacted
outcomes. A self-join detaches and returns `WouldDeadlock`.

The build-model guard limits MinGW Rust production authority to this domain.
The model tests cover one production provider and its archive in every context,
and reject a Rust production provider for a different MinGW domain. Hosted
MinGW verification checks the worker suite in `Verify native worker thread execution`
and the ABI allowlist in `Verify native Rust C ABI exports (GNU nm)`. The hosted
run IDs are recorded in #435.

The UTF-16, Output, and URI authority policies are unchanged. This decision
makes no performance claim.

## 2026-11-25 / #441 A2 encoded EOL candidate (comparison-only)

C++ keeps production authority. The Rust encoded EOL scanner is a
`candidate-shadow` provider of the `document-file-codec` domain
(`sakura-encoded-eol-rust-candidate` for the four MSVC contexts and
`sakura-encoded-eol-rust-gnu-candidate` for the two MinGW contexts), while
`sakura-encoded-eol-cpp-provider` stays `legacy-production` in all six. Nothing
outside the tests calls the Rust provider. It reports the delimiter offsets,
widths and kinds (CRLF, LF, CR, NEL, LS, PS) of UTF-16LE/BE and UTF-32LE/BE text
through two checked exports, `sakura_document_codec_scan_eol_measure_v1` and
`sakura_document_codec_scan_eol_write_v1`, with 16-byte records and at most
1,048,576 records per call. The native C ABI allowlist grows from 32 to 34 names.
Adopting any Rust scan for production needs its own review per operation and
format (#267 G2.5); this entry decides nothing of the kind.

Correctness is recorded apart from performance. One shared corpus,
`rust/native/fixtures/encoded-eol-corpus.tsv` (59 rows in 13 classes, with 104
end-of-region terminal cases), is read by the C++ `EncodedEolContract` suite
(7 tests) and by the safe Rust crate `sakura_document_codec`
(`forbid(unsafe_code)`), which also checks an independent model on 3,364,428
small regions. The `EncodedEolRustCandidate` suite (10 tests) compares the Rust
exports with the production scan on direct, guard-page and partition-replay
cases: 8,872 compared ranges and 3,984 refused starts.

Execution evidence. MSVC: run 45414147222 (commit `1e551f72a`) ran both suites
in Debug and Release; `tools/verify-gtest-suite-executed.py` on the hosted
`test-results-x64-<configuration>` artifacts reports 7 executed and 0 failed for
`EncodedEolContract` and 10 executed and 0 failed for `EncodedEolRustCandidate`
in each configuration, with the two counters above recorded in both. MinGW:
runs 45428222387 (commit `1b6267937`) and 45453344382 (commit `a55b88631`)
executed the same suites in both legs, 7 and 10 executed with 0 failed in all
four legs, through the `Verify encoded EOL contract execution` and `Verify
encoded EOL candidate execution` steps. Run 45453344382 also passed the GNU nm
allowlist check.

Rebuild trace (the MinGW/CMake trace that #267 G1 item 5 deferred). In run
45453344382 each leg rebuilt after the unit tests with no source change, then
after touching `rust/native/sakura_document_codec/src/encoded_eol.rs`
(modification time only). With no change, the archive, `sakura.exe`, `tests1.exe`
and both language DLLs kept their modification time and digest. After the touch
the archive, `sakura.exe` and `tests1.exe` were rebuilt (strictly later
modification time) and both language DLLs stayed identical; both legs report
PASS. The first build after the unit tests is not quiet: it restores the package
closure again and relinks, because the CTest `pytest` test writes bytecode into a
declared package input and changes the package plan hash (#442). The step
therefore runs that build once before measuring; run 45428222387 shows the
no-change phase failing on both legs without it. The scan of other changed
binaries lists one informational entry,
`build/MinGW/tests1_resources/test-dllplugin/dll_plugin1.dll`.

This entry makes no performance claim, says nothing about MSVC rebuild behavior
(the trace runs on MinGW only), and adopts nothing for production.
[#441](https://github.com/tsuyoshi-otake/sakura-editor-next/issues/441),
[#442](https://github.com/tsuyoshi-otake/sakura-editor-next/issues/442)

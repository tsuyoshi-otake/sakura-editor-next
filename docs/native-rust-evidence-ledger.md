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

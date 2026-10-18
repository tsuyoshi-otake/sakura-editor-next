/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#pragma once

#include <windows.h>

namespace senp {

//! The job limits one SENP child process (v1/v2 extension host or package
//! tool) is configured with before it is created. Every SENP child also runs
//! under the shared set the launcher cannot opt out of: kill-on-close,
//! active-process 1, die-on-unhandled-exception and the full basic UI
//! restriction set. `processMemoryBytes == 0` requests no per-process memory
//! cap.
struct ChildJobLimits final {
	SIZE_T processMemoryBytes = 0;
};

//! Both extension host protocols compile and run untrusted Wasm inside a
//! 512 MiB per-process budget on top of the Wasm Store limits (#340 parity).
inline constexpr ChildJobLimits kExtensionHostJobLimits{
	.processMemoryBytes = 512u * 1024u * 1024u,
};

//! The package tool inflates attacker-supplied archives, so it gets its own cap
//! sized from its bounded worst case. Install holds the archive snapshot
//! (Rust `MAX_ARCHIVE_BYTES`, 64 MiB), its verified entries
//! (`MAX_EXPANDED_BYTES`, 64 MiB) and the re-read of the staged copy at once.
//! Measured 2026-10-18 with a 62 MiB incompressible package (the largest the
//! limits admit): install-dev and same-digest repair peak at 187 MiB of
//! committed memory, verify at 125 MiB and list at 63 MiB, leaving ~69 MiB of
//! headroom under 256 MiB.
inline constexpr ChildJobLimits kPackageToolJobLimits{
	.processMemoryBytes = 256u * 1024u * 1024u,
};

//! Applies the shared SENP child limits to `job` and then reads every limit
//! back through QueryInformationJobObject. Any API failure or read-back
//! mismatch returns false; callers must treat false as a launch failure that
//! abandons the job (its KILL_ON_JOB_CLOSE flag then kills any already
//! assigned child), so no unbounded child may run.
[[nodiscard]] bool ConfigureChildJob(HANDLE job, const ChildJobLimits& limits) noexcept;

namespace detail {

//! Test seam for the read-back half of ConfigureChildJob: `query` has the
//! exact signature of `QueryInformationJobObject`, so production passes the
//! API itself while a test injects a tampered or failing read-back.
using QueryJobInformationFn = BOOL(WINAPI*)(HANDLE, JOBOBJECTINFOCLASS, LPVOID, DWORD, LPDWORD);
[[nodiscard]] bool VerifyChildJobLimits(HANDLE job, const ChildJobLimits& limits,
	QueryJobInformationFn query) noexcept;

} // namespace detail

} // namespace senp

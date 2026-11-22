/* C ABI declarations for Rust-owned worker threads. */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/

#pragma once

#include <cstdint>

constexpr std::uint32_t SAKURA_WORKER_THREAD_ABI_VERSION_V2 = 2;

enum class SakuraWorkerThreadStatus : std::uint32_t {
	Ok = 0,
	InvalidArgument = 1,
	SpawnFailed = 2,
	NotFound = 3,
	InternalError = 4,
	WouldDeadlock = 5,
};

//! Redacted callback outcome. Exception text and types stay in C++.
enum class SakuraWorkerBodyOutcome : std::uint32_t {
	Completed = 0,
	ThrewException = 1,
	ThrewUnknown = 2,
};

/*!
	One-shot launch: one successful start runs one body on its own thread and
	issues a token that can be consumed for exactly one join terminal.

	C++ owns the body and keeps its context valid for as long as the body uses
	it, including after a self-join detaches. The entry must contain any exceptions
	and return only one of the declared outcomes. Rust never reads the context.
*/
using SakuraWorkerThreadEntryV2 = SakuraWorkerBodyOutcome (*)(void* context) noexcept;

extern "C"
{
//! Starts one worker and writes its token.  The token slot is cleared before
//! any fallible work, so a refused start never leaves a stale handle behind.
SakuraWorkerThreadStatus sakura_worker_thread_start_v2(
	SakuraWorkerThreadEntryV2 entry,
	void* context,
	std::uint64_t* token) noexcept;

//! Claims the token once and clears its slot before waiting. A self-join
//! detaches and returns WouldDeadlock. Other joins of a claimed token return
//! NotFound. Use independent writable slots for each call. Only Ok writes the
//! body outcome. A Rust worker failure returns InternalError.
SakuraWorkerThreadStatus sakura_worker_thread_join_v2(
	std::uint64_t* token, SakuraWorkerBodyOutcome* outcome) noexcept;
}

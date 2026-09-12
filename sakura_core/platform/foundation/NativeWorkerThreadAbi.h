/* C ABI declarations for Rust-owned worker threads. */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/

#pragma once

#include <cstdint>

constexpr std::uint32_t SAKURA_WORKER_THREAD_ABI_VERSION_V1 = 1;

enum class SakuraWorkerThreadStatus : std::uint32_t {
	Ok = 0,
	InvalidArgument = 1,
	SpawnFailed = 2,
	NotFound = 3,
	InternalError = 4,
};

/*!
	The body one worker runs exactly once, on its own thread.

	C++ owns the body and Rust owns the thread.  The pointer type is `noexcept`
	so the "must not throw across the boundary" obligation is checked by the
	compiler at every call site instead of being written in a comment.
*/
using SakuraWorkerThreadEntryV1 = void (*)(void* context) noexcept;

extern "C"
{
//! Starts one worker and writes its token.  The token slot is cleared before
//! any fallible work, so a refused start never leaves a stale handle behind.
SakuraWorkerThreadStatus sakura_worker_thread_start_v1(
	SakuraWorkerThreadEntryV1 entry,
	void* context,
	std::uint64_t* token) noexcept;

//! Waits for the worker body to return, then clears the token slot.  The slot
//! is cleared before the wait blocks, so the caller's handle is released even
//! while a slow worker is still finishing.
SakuraWorkerThreadStatus sakura_worker_thread_join_v1(std::uint64_t* token) noexcept;
}

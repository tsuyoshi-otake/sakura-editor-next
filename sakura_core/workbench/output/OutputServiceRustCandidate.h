/*!
 * @file
 * @brief Observational Rust shadow attached to the authoritative OutputService.
 *
 * This candidate never supplies Output state, notifications, or operation
 * results.  The C++ OutputService remains the sole production authority.
 */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#pragma once

#include "workbench/output/OutputService.h"
#include "workbench/output/OutputServiceRustShadowAbi.h"

#include <cstdint>
#include <memory>

namespace workbench::output {

enum class EOutputServiceRustCandidateAvailability : std::uint8_t {
	Unavailable,
	Available,
};

enum class EOutputServiceRustCandidateState : std::uint8_t {
	Unavailable,
	Attaching,
	Live,
	Faulted,
	Stopped,
};

//! A terminal diagnostic category.  `Stopped` is a normal terminal state and
//! therefore has no corresponding fault value.
enum class EOutputServiceRustCandidateFault : std::uint8_t {
	None,
	Unavailable,
	BootstrapMismatch,
	SubscribeFailed,
	FfiFailure,
	ResultMismatch,
	CursorMismatch,
	Gap,
	SnapshotMismatch,
	CallbackException,
	DestroyFailure,
};

//! A copied diagnostic snapshot.  No pointer or view into the Rust model or
//! OutputService is exposed after the call returns.
struct OutputServiceRustCandidateDiagnostics final {
	EOutputServiceRustCandidateAvailability availability{
		EOutputServiceRustCandidateAvailability::Unavailable };
	EOutputServiceRustCandidateState state{ EOutputServiceRustCandidateState::Unavailable };
	std::uint64_t lastCursor{};
	std::uint64_t appliedCommitCount{};
	SakuraOutputShadowStatus lastFfiStatus{ SakuraOutputShadowStatus::InternalError };
	SakuraOutputShadowOperationStatus lastOperationStatus{ SakuraOutputShadowOperationStatus::Stopped };
	SakuraOutputShadowReason lastOperationReason{ SakuraOutputShadowReason::None };
	std::uint64_t lastOperationRevision{};
	EOutputServiceRustCandidateFault fault{ EOutputServiceRustCandidateFault::Unavailable };
};

//! Observational live-feed adapter for the replay-only Rust OutputService
//! model.  Construct it before any production mutation.  The constructor
//! captures a future-only accepted-commit feed atomically with its bootstrap;
//! it accepts only the initial empty revision-one state and never replays or
//! transfers a non-empty snapshot.
class OutputServiceRustCandidate final {
public:
	OutputServiceRustCandidate(OutputService& service, const OutputServiceLimits& limits) noexcept;
	~OutputServiceRustCandidate() noexcept;

	OutputServiceRustCandidate(const OutputServiceRustCandidate&) = delete;
	OutputServiceRustCandidate& operator=(const OutputServiceRustCandidate&) = delete;
	OutputServiceRustCandidate(OutputServiceRustCandidate&&) = delete;
	OutputServiceRustCandidate& operator=(OutputServiceRustCandidate&&) = delete;

	[[nodiscard]] static constexpr bool IsCompiledIn() noexcept
	{
#if defined(SAKURA_UTF16_RUST_CANDIDATE)
		return true;
#else
		return false;
#endif
	}

	[[nodiscard]] bool IsAvailable() const noexcept;
	[[nodiscard]] OutputServiceRustCandidateDiagnostics Diagnostics() const noexcept;

	//! Compares the Rust canonical snapshot with the authoritative snapshot.
	//! The advisory C++ dropped-notification counter is intentionally ignored.
	//! This is an on-demand check only; it is never run for every mutation.
	[[nodiscard]] bool VerifySnapshot() noexcept;
	[[nodiscard]] bool CompareSnapshot() noexcept { return VerifySnapshot(); }

	//! The service's Stop() is the lifetime fence for an active callback.  Call
	//! OutputService::Stop() first, then this method (or destroy this object).
	//! Standalone callers must follow that same order; this method never stops
	//! the authoritative service.
	void ShutdownAfterOutputServiceStop() noexcept;
	void Shutdown() noexcept { ShutdownAfterOutputServiceStop(); }

private:
	struct Control;

	std::shared_ptr<Control> m_control;
	OutputServiceRustCandidateDiagnostics m_fallbackDiagnostics{};
};

} // namespace workbench::output

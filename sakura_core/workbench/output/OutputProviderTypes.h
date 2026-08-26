/*! @file
 * @brief Provider-neutral Output authority selection and health types.
 */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#pragma once

#include "workbench/output/OutputServiceTypes.h"

#include <cstdint>

namespace workbench::output {

//! The authority is selected once for one runtime lifetime. This selector is
//! independent from CpuDispatch and all UTF-16 backend macros.
enum class EOutputProviderKind : std::uint8_t {
	Cpp,
	Rust,
};

//! Construction is separate from provider operation results. In particular,
//! Rust unavailability is not a request to fall back to C++.
enum class EOutputProviderFactoryStatus : std::uint8_t {
	Created,
	Unavailable,
	InitializationFailed,
	InvalidSelection,
	NotAttempted,
};

enum class EOutputProviderLifecycle : std::uint8_t {
	NotAttempted,
	Unavailable,
	Ready,
	Faulted,
	Stopped,
};

//! The furthest initialization boundary reached by the selected provider.
enum class EOutputProviderInitializationStage : std::uint8_t {
	NotStarted,
	FactorySelection,
	ProviderConstruction,
	AbiCreate,
	Ready,
};

//! Payload-free attribution for the most recent ABI call and terminal fault.
enum class EOutputProviderBoundary : std::uint8_t {
	None,
	Factory,
	Create,
	Apply,
	SnapshotMeasure,
	SnapshotWrite,
	SnapshotDecode,
	ActiveChannelQuery,
	Stop,
	Destroy,
};

enum class EOutputProviderFault : std::uint8_t {
	None,
	Unavailable,
	Initialization,
	AbiContract,
	Ffi,
	Snapshot,
	Callback,
	Destroy,
};

enum class EOutputProviderBoundaryStatus : std::uint8_t {
	NotCalled,
	Ok,
	InvalidArgument,
	InvalidHandle,
	Stopped,
	InsufficientCapacity,
	InternalError,
};

//! All counters saturate at UINT64_MAX. They contain no operation, owner,
//! channel, path, label, text, log, or other user-controlled payload.
struct OutputProviderHealthCounters final {
	std::uint64_t initializationAttempts{};
	std::uint64_t ffiCalls{};
	//! Counts every public mutation attempt, including preflight rejection.
	std::uint64_t mutationCalls{};
	//! Operation outcome counters exclude Stop; Stop has its own counter below.
	std::uint64_t acceptedOperations{};
	std::uint64_t replayedOperations{};
	std::uint64_t rejectedOperations{};
	std::uint64_t snapshotCalls{};
	std::uint64_t activeChannelCalls{};
	//! Includes initial Stop and every retry after a deferred/faulted Stop.
	std::uint64_t stopCalls{};
	std::uint64_t destroyCalls{};
	std::uint64_t boundaryFailures{};
	std::uint64_t advisoryListenerFailures{};
	std::uint64_t advisoryDroppedNotifications{};
};

//! Copied provider health exposed through IWorkbenchRuntime. Advisory observer
//! failures remain counters and never become an authority fault.
struct OutputProviderHealthSnapshot final {
	EOutputProviderKind kind{ EOutputProviderKind::Cpp };
	EOutputProviderFactoryStatus factoryStatus{ EOutputProviderFactoryStatus::NotAttempted };
	EOutputProviderLifecycle lifecycle{ EOutputProviderLifecycle::NotAttempted };
	EOutputProviderInitializationStage initializationStage{
		EOutputProviderInitializationStage::NotStarted };
	EOutputProviderFault fault{ EOutputProviderFault::None };
	EOutputProviderBoundary lastBoundary{ EOutputProviderBoundary::None };
	EOutputProviderBoundary failureBoundary{ EOutputProviderBoundary::None };
	EOutputProviderBoundaryStatus lastBoundaryStatus{
		EOutputProviderBoundaryStatus::NotCalled };
	bool compiledIn{};
	bool available{};
	//! True only for explicitly injected test creators. Production evidence must
	//! reject a run carrying this bit.
	bool testOverrideActive{};
	std::uint32_t abiVersion{};
	bool hasLastOperation{};
	EOutputOperationStatus lastOperationStatus{ EOutputOperationStatus::Rejected };
	EOutputOperationReason lastOperationReason{ EOutputOperationReason::None };
	std::uint64_t lastOperationRevision{};
	std::uint64_t currentRevision{};
	OutputProviderHealthCounters counters{};
};

} // namespace workbench::output

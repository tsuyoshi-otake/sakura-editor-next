/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "workbench/outline/OutlineDocumentSnapshot.h"

#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace workbench::outline {

//! Phase measurements are value-owned and safe to publish with a result.
struct OutlinePhaseTimings final {
	// Each value is an exclusive phase unless explicitly marked inclusive below.
	std::uint64_t snapshotCaptureUs = 0;
	std::uint64_t queueWaitUs = 0;
	std::uint64_t parserUs = 0;
	std::uint64_t dtoConstructionUs = 0;
	std::uint64_t layoutProjectionUs = 0;
	std::uint64_t lineMarkProjectionUs = 0;
	std::uint64_t nativeTreeBuildUs = 0;
	// Inclusive UI projection wall time: SetData and its appearance/expansion
	// pass.  Do not add the subphases above or appearance/expansion again.
	std::uint64_t nativeCommitUs = 0;
	std::uint64_t appearanceUs = 0;
	std::uint64_t expansionUs = 0;
	std::uint64_t selectionUs = 0;
	// Capture-start to UI selection completion, including result delivery delay.
	std::uint64_t totalUs = 0;
};

//! Logical, value-owned symbol data returned by a background parser.
struct OutlineSymbolDto final {
	int logicalLine = 0;
	int logicalColumn = 0;
	std::wstring name;
	std::wstring fileName;
	int info = 0;
	int depth = 0;
};

struct OutlineParseResult final {
	OutlineDocumentVersion documentVersion{};
	int outlineType = 0;
	int listType = 0;
	std::wstring filePath;
	std::wstring titleOverride;
	std::vector<OutlineSymbolDto> symbols;
	std::map<int, std::wstring> appendText;
	OutlinePhaseTimings timings{};
};

enum class OutlineWorkerTerminal : std::uint8_t {
	Parsed,
	Cancelled,
	Failed,
	Superseded,
	Closed,
};

struct OutlineWorkerResult final {
	std::uint64_t generation = 0;
	OutlineWorkerTerminal terminal = OutlineWorkerTerminal::Failed;
	OutlineParseResult parse;
};

enum class OutlineWorkerRequestStatus : std::uint8_t {
	Started,
	Queued,
	PendingReplaced,
	ActiveDeduplicated,
	PendingDeduplicated,
	InvalidSnapshot,
	GenerationExhausted,
	Closed,
};

//! The status and generation are assigned together while the scheduler mutex
//! is held.  Callers must retain this generation; reconstructing it from a
//! later state snapshot races the worker promotion/completion path.
struct OutlineWorkerSubmitResult final {
	OutlineWorkerRequestStatus status = OutlineWorkerRequestStatus::Closed;
	std::uint64_t generation = 0;
};

struct OutlineWorkerCancellationResult final {
	bool activeCancelled = false;
	bool pendingDiscarded = false;
};

struct OutlineWorkerStateSnapshot final {
	bool acceptingNotifications = false;
	bool closed = false;
	bool active = false;
	bool pending = false;
	std::uint64_t activeGeneration = 0;
	std::uint64_t pendingGeneration = 0;
	bool resultPending = false;
	bool resultMessagePosted = false;
	OutlineDocumentVersion activeVersion{};
	OutlineDocumentVersion pendingVersion{};
	std::uint64_t startedCount = 0;
	std::uint64_t completedCount = 0;
	std::uint64_t cancelledCount = 0;
	std::uint64_t failedCount = 0;
	std::uint64_t supersededCount = 0;
	std::uint64_t lastGeneration = 0;
	OutlineWorkerTerminal lastTerminal = OutlineWorkerTerminal::Closed;
	OutlinePhaseTimings lastTimings{};
};

//! One-worker/latest-pending scheduler for snapshot-safe built-in Outline parsing.
class OutlineParserWorker final {
public:
	using Snapshot = std::shared_ptr<const OutlineDocumentSnapshot>;
	using CancelToken = std::shared_ptr<std::atomic_bool>;
	using ParseFunction = std::function<OutlineParseResult(
		const OutlineDocumentSnapshot&, int outlineType, int listType, const CancelToken&)>;

	static constexpr UINT kWorkerResultMessage = WM_APP + 0x572;

	OutlineParserWorker();
	explicit OutlineParserWorker( ParseFunction parser );
	~OutlineParserWorker();
	OutlineParserWorker( const OutlineParserWorker& ) = delete;
	OutlineParserWorker& operator=( const OutlineParserWorker& ) = delete;

	//! Gate is armed only while the dialog HWND owns the result message.
	void SetNotificationWindow( HWND window, bool accepting ) noexcept;

	[[nodiscard]] OutlineWorkerSubmitResult Submit(
		Snapshot snapshot,
		int outlineType,
		int listType,
		std::uint64_t snapshotCaptureUs );

	//! Cancels queued/running work that cannot produce the current document version.
	//! This is intentionally snapshot-free so an edit can shed stale O(N) work
	//! immediately while the replacement snapshot remains debounced.
	[[nodiscard]] OutlineWorkerCancellationResult CancelObsolete(
		OutlineDocumentVersion currentVersion ) noexcept;

	//! Deterministic test seam for the pre-promotion pending-only window.
	//! Production callers leave this disabled.
	void SetPromotionPausedForTest( bool paused ) noexcept;

	//! Deterministic boundary seam; production callers never seed generations.
	void SetNextGenerationForTest( std::uint64_t nextGeneration ) noexcept;

	//! Takes ownership of a result previously queued by PostMessage.
	[[nodiscard]] std::unique_ptr<OutlineWorkerResult> TakePendingResult(
		OutlineWorkerResult* raw ) noexcept;

	[[nodiscard]] OutlineWorkerStateSnapshot GetStateSnapshot() const noexcept;

	//! Stop accepting notifications, cancel/wake, join, then release queued results.
	void Close() noexcept;

	//! Snapshot-only parser entry point used by production and deterministic tests.
	[[nodiscard]] static OutlineParseResult ParseSnapshot(
		const OutlineDocumentSnapshot& snapshot,
		int outlineType,
		int listType,
		const CancelToken& cancelToken );

private:
	struct Job final {
		Snapshot snapshot;
		int outlineType = 0;
		int listType = 0;
		std::uint64_t generation = 0;
		std::uint64_t submittedAtUs = 0;
		std::uint64_t snapshotCaptureUs = 0;
		CancelToken cancelToken;
	};

	struct NotificationGate;
	struct SharedState;

	void WorkerMain() noexcept;
	void PostResult( std::unique_ptr<OutlineWorkerResult> result ) noexcept;

	std::shared_ptr<SharedState> m_shared;
	std::shared_ptr<NotificationGate> m_gate;
	ParseFunction m_parser;
	std::thread::id m_ownerThread;
	std::thread m_worker;
};

} // namespace workbench::outline

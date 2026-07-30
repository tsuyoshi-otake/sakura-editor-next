/*! @file
	@brief VS Code compatible document synchronization and UI dispatch models
*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

//! Convert a native path to the canonical URI representation used by the extension host.
[[nodiscard]] std::wstring ExtensionFileUriFromPath(const std::filesystem::path& path);
//! Decode a canonical extension-host file URI. Non-file and malformed URIs are rejected.
[[nodiscard]] std::optional<std::filesystem::path> ExtensionFilePathFromUri(std::wstring_view uri);

struct SExtensionDocumentId {
	std::uint32_t editorProcessId = 0;
	std::uint32_t localDocumentId = 0;

	[[nodiscard]] bool IsValid() const noexcept { return editorProcessId != 0 && localDocumentId != 0; }
	[[nodiscard]] std::string ToString() const;
	auto operator<=>(const SExtensionDocumentId&) const = default;
};

namespace std {
template<>
struct hash<SExtensionDocumentId> {
	std::size_t operator()(const SExtensionDocumentId& value) const noexcept
	{
		const auto first = std::hash<std::uint32_t>{}(value.editorProcessId);
		const auto second = std::hash<std::uint32_t>{}(value.localDocumentId);
		return first ^ (second + static_cast<std::size_t>(0x9e3779b9U) + (first << 6) + (first >> 2));
	}
};
} // namespace std

struct SExtensionTextPosition {
	std::uint32_t line = 0;
	std::uint32_t character = 0;
	auto operator<=>(const SExtensionTextPosition&) const = default;
};

struct SExtensionTextRange {
	SExtensionTextPosition start;
	SExtensionTextPosition end;
};

struct SExtensionTextEdit {
	SExtensionTextRange range;
	std::wstring newText;
};

struct SExtensionDocumentSnapshot {
	SExtensionDocumentId id;
	std::wstring uri;
	std::wstring languageId;
	std::wstring text;
	std::uint64_t version = 0;
	bool dirty = false;
};

enum class EExtensionDocumentUpdateResult {
	Applied,
	UnknownDocument,
	StaleVersion,
	VersionGap,
	InvalidSnapshot,
};

class CExtensionDocumentSync final {
public:
	[[nodiscard]] EExtensionDocumentUpdateResult Open(SExtensionDocumentSnapshot snapshot);
	[[nodiscard]] EExtensionDocumentUpdateResult Change(SExtensionDocumentSnapshot snapshot);
	[[nodiscard]] EExtensionDocumentUpdateResult Save(SExtensionDocumentSnapshot snapshot);
	[[nodiscard]] bool Close(const SExtensionDocumentId& id);
	[[nodiscard]] std::optional<SExtensionDocumentSnapshot> Snapshot(const SExtensionDocumentId& id) const;
	[[nodiscard]] std::vector<SExtensionDocumentSnapshot> Snapshots() const;
	void Clear();

private:
	friend class CExtensionApplyEdit;
	[[nodiscard]] static bool IsValidSnapshot(const SExtensionDocumentSnapshot& snapshot);
	std::unordered_map<SExtensionDocumentId, SExtensionDocumentSnapshot> m_documents;
};

struct SExtensionAggregatedDocumentEvent {
	SExtensionDocumentSnapshot snapshot;
	bool snapshotOnly = false;
	std::size_t coalescedChanges = 0;
};

/*! Coalesces hot document changes without extending latency beyond sixteen milliseconds. */
class CExtensionEventAggregator final {
public:
	using Clock = std::chrono::steady_clock;

	explicit CExtensionEventAggregator(
		std::size_t maximumChanges = 256,
		std::size_t maximumBytes = 4 * 1024 * 1024,
		std::chrono::milliseconds minimumDelay = std::chrono::milliseconds(8),
		std::chrono::milliseconds maximumDelay = std::chrono::milliseconds(16));

	void Enqueue(SExtensionDocumentSnapshot snapshot, Clock::time_point now = Clock::now());
	[[nodiscard]] std::vector<SExtensionAggregatedDocumentEvent> DrainReady(Clock::time_point now = Clock::now());
	[[nodiscard]] std::optional<Clock::time_point> NextReadyTime() const noexcept;
	[[nodiscard]] std::size_t PendingDocumentCount() const noexcept { return m_pending.size(); }
	void Clear();

private:
	struct Pending {
		SExtensionDocumentSnapshot snapshot;
		Clock::time_point first;
		Clock::time_point ready;
		Clock::time_point deadline;
		std::size_t changes = 0;
		std::size_t bytes = 0;
		bool snapshotOnly = false;
	};

	std::size_t m_maximumChanges;
	std::size_t m_maximumBytes;
	std::chrono::milliseconds m_minimumDelay;
	std::chrono::milliseconds m_maximumDelay;
	std::unordered_map<SExtensionDocumentId, Pending> m_pending;
};

/*! Bounded cross-thread queue; overload is an explicit terminal outcome for each posted task. */
class CExtensionUiDispatcher final {
public:
	enum class PostResult { Queued, Overloaded, Stopped };
	using Task = std::function<void()>;

	explicit CExtensionUiDispatcher(std::size_t capacity = 1024) : m_capacity(capacity) {}
	[[nodiscard]] PostResult Post(Task task);
	[[nodiscard]] std::size_t Drain(std::size_t maximumTasks = 256);
	void Stop() noexcept;
	[[nodiscard]] std::size_t Size() const;

private:
	const std::size_t m_capacity;
	mutable std::mutex m_mutex;
	std::deque<Task> m_tasks;
	bool m_stopped = false;
};

struct SExtensionDocumentEdit {
	SExtensionDocumentId documentId;
	std::uint64_t expectedVersion = 0;
	std::vector<SExtensionTextEdit> edits;
	//! Requested document-wide line ending: false=LF, true=CRLF.
	std::optional<bool> crlf;
};

enum class EExtensionApplyEditStatus {
	Applied,
	UnknownDocument,
	VersionMismatch,
	InvalidRange,
	OverlappingEdits,
	CommandReentry,
};

struct SExtensionApplyEditResult {
	EExtensionApplyEditStatus status = EExtensionApplyEditStatus::InvalidRange;
	std::uint64_t undoUnit = 0;
	[[nodiscard]] bool Applied() const noexcept { return status == EExtensionApplyEditStatus::Applied; }
};

/*! Pure all-or-nothing workspace edit engine used before mutating the native editor. */
class CExtensionApplyEdit final {
public:
	explicit CExtensionApplyEdit(CExtensionDocumentSync& documents) : m_documents(documents) {}
	[[nodiscard]] SExtensionApplyEditResult Apply(
		const std::vector<SExtensionDocumentEdit>& documents,
		bool commandDispatchActive = false);
	[[nodiscard]] std::size_t UndoUnitCount() const noexcept { return m_undoUnits.size(); }

private:
	struct UndoUnit {
		std::uint64_t id = 0;
		std::vector<SExtensionDocumentSnapshot> before;
	};
	struct PreparedEdit {
		SExtensionDocumentId id;
		std::wstring text;
	};

	[[nodiscard]] static EExtensionApplyEditStatus Prepare(
		const SExtensionDocumentSnapshot& snapshot,
		const SExtensionDocumentEdit& requested,
		PreparedEdit& prepared);

	CExtensionDocumentSync& m_documents;
	std::uint64_t m_nextUndoUnit = 1;
	std::vector<UndoUnit> m_undoUnits;
};

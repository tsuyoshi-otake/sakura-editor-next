/*! @file
 * @brief `problems.json`: the current markers, readable outside the window.
 */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#pragma once

#include "workbench/problems/MarkerService.h"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace workbench::logs {

class ILogFileSystem;

inline constexpr std::wstring_view kProblemsSnapshotFileName = L"problems.json";

struct ProblemsSnapshotFileLimits final {
	std::size_t maximumMarkers{ 10'000 };
	std::size_t maximumBytes{ 8U << 20 };
};

/*!
	`{"version": 1, "revision": N, "markerCount": M, "truncated": false,
	"markers": [...]}`. Each marker is the object the Problems view's Copy
	produces (`SerializeProblemsViewMarker`: resource, owner, code, severity
	8/4/2/1, message, source, 1-based start/end line and column,
	relatedInformation), in the Problems view's order. `markerCount` is the
	total; when the limits cut the list, `truncated` is true and `markers`
	holds the first ones.
*/
[[nodiscard]] std::string EncodeProblemsSnapshotFile(
	const problems::ProblemsSnapshot& snapshot, const ProblemsSnapshotFileLimits& limits = {});

/*!
	Keeps `problems.json` equal to the marker service, a fork extension: VS
	Code has no file form of Problems. A marker change marks the file dirty;
	the worker waits `coalesce`, reads one snapshot, and replaces the file
	atomically, so a burst of changes is one write and a reader never sees a
	partial file. A failed write is reported through `onFailingChanged` and
	retried on its own with backoff (see kMaximumRetryDelay). Stop
	unsubscribes, then joins the worker; the last written file stays.
*/
class CProblemsSnapshotFileWriter final {
public:
	using FailingChanged = std::function<void(bool failing)>;

	CProblemsSnapshotFileWriter(problems::MarkerService& markers, std::filesystem::path file,
		std::shared_ptr<ILogFileSystem> files, std::chrono::milliseconds coalesce = std::chrono::milliseconds(200),
		ProblemsSnapshotFileLimits limits = {}, FailingChanged onFailingChanged = {});
	~CProblemsSnapshotFileWriter();
	CProblemsSnapshotFileWriter(const CProblemsSnapshotFileWriter&) = delete;
	CProblemsSnapshotFileWriter& operator=(const CProblemsSnapshotFileWriter&) = delete;

	//! Subscribes, starts the worker, and writes the current (usually empty)
	//! snapshot. Returns false, leaving nothing running, when either fails.
	[[nodiscard]] bool Start() noexcept;
	void Stop() noexcept;

	[[nodiscard]] const std::filesystem::path& File() const noexcept { return m_file; }
	[[nodiscard]] std::uint64_t WrittenRevision() const noexcept;

	//! After a failed write the file is retried on its own, 500 ms first and
	//! doubling up to this, so a reader that briefly blocked the rename does
	//! not leave a stale file until some unrelated marker change.
	static constexpr std::chrono::milliseconds kMaximumRetryDelay{ 30'000 };

private:
	//! Shared with the marker listener. MarkerService::Unsubscribe does not
	//! wait for a delivery that already copied the listener, so the listener
	//! holds this state, never the writer.
	struct State final {
		std::mutex mutex;
		std::condition_variable wake;
		bool dirty{};
		bool stopping{};
	};

	void WorkerMain() noexcept;

	problems::MarkerService& m_markers;
	const std::filesystem::path m_file;
	const std::shared_ptr<ILogFileSystem> m_files;
	const std::chrono::milliseconds m_coalesce;
	const ProblemsSnapshotFileLimits m_limits;
	const FailingChanged m_onFailingChanged;
	const std::shared_ptr<State> m_state;
	//! Worker-written, read under m_state->mutex.
	bool m_failing{};
	std::uint64_t m_writtenRevision{};
	std::optional<problems::MarkerSubscriptionId> m_subscription;
	std::thread m_worker;
};

} // namespace workbench::logs

/*! @file
 * @brief File-backed Log channels: VS Code's log line format and layout, a
 * bounded background writer, and the IOutputService decorator that feeds it.
 */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#pragma once

#include "workbench/logs/LogSession.h"
#include "workbench/output/IOutputService.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace workbench::logs {

class ILogFileSystem;

//! `2026-11-15 06:41:22.477 [info] message` followed by a line feed, the
//! format of VS Code's log files (lowercase level: trace, debug, info,
//! warning, error).
[[nodiscard]] std::string FormatOutputLogLine(
	const LogTimestamp& timestamp, output::EOutputLogLevel level, std::string_view message);

//! Where a Log channel's file lives under the window folder, following VS
//! Code's layout: a core channel (`core.*` owner) sits in the window folder
//! itself, an extension channel under `exthost/<extension id>/`. The built-in
//! Git integration (`workbench.scm.git`) is upstream's `vscode.git`, so its
//! channel is `exthost/vscode.git/Git.log` exactly as upstream writes it. The
//! label becomes the file name with characters Windows forbids replaced; a
//! label ending in `.<digits>` has that dot replaced too, so no channel's file
//! can be another channel's `.1.log` rotation backup.
[[nodiscard]] std::filesystem::path OutputLogRelativePath(std::string_view ownerId, std::string_view label);

struct OutputLogFileWriterLimits final {
	//! A file that would grow past this is moved to `<name>.1.log` (replacing
	//! the previous backup) and started again, so a channel keeps at most twice
	//! this on disk.
	std::uint64_t maximumFileBytes{ 5ULL << 20 };
	//! Bytes waiting for the worker. Appends beyond it are dropped and counted.
	std::size_t maximumPendingBytes{ 4U << 20 };
};

struct OutputLogFileWriterDiagnostics final {
	std::uint64_t writtenBytes{};
	std::uint64_t droppedBytes{};
	std::uint64_t failedWrites{};
	std::uint64_t rotations{};
	bool failing{};
};

/*!
	Writes log bytes under one window folder on its own worker thread, so no
	caller ever waits for the disk. A failed write drops that batch, counts it,
	and reports the failing state once through `onFailingChanged` (and again
	when a later write succeeds); later batches are still attempted. Stop
	writes what is pending, then joins the worker.
*/
class COutputLogFileWriter final {
public:
	using FailingChanged = std::function<void(bool failing)>;

	COutputLogFileWriter(std::filesystem::path windowFolder, std::shared_ptr<ILogFileSystem> files,
		OutputLogFileWriterLimits limits = {}, FailingChanged onFailingChanged = {});
	~COutputLogFileWriter();
	COutputLogFileWriter(const COutputLogFileWriter&) = delete;
	COutputLogFileWriter& operator=(const COutputLogFileWriter&) = delete;

	//! Receives a predicate that turns true once Stop was requested.
	using BeforeFirstWrite = std::function<void(const std::function<bool()>& stopRequested)>;

	//! Starts the worker. `beforeFirstWrite` runs on the worker before any
	//! queued write (the session's retention sweep) and should return early
	//! once its predicate is true; an exception from it is swallowed. Returns
	//! false when the thread cannot be created; the writer then accepts nothing.
	[[nodiscard]] bool Start(BeforeFirstWrite beforeFirstWrite = {}) noexcept;
	//! Queues `bytes` for `relativePath`. Never blocks on I/O.
	void Append(const std::filesystem::path& relativePath, std::string bytes) noexcept;
	void Stop() noexcept;

	[[nodiscard]] const std::filesystem::path& WindowFolder() const noexcept { return m_windowFolder; }
	[[nodiscard]] OutputLogFileWriterDiagnostics Diagnostics() const noexcept;

private:
	struct PendingWrite final {
		std::filesystem::path relativePath;
		std::string bytes;
	};

	struct WriteOutcome final {
		bool written{};
		bool rotated{};
	};

	void WorkerMain() noexcept;
	[[nodiscard]] WriteOutcome WriteFile(const std::filesystem::path& relativePath, const std::string& bytes) noexcept;
	//! One failing-state transition per batch, not per file, so one bad file
	//! beside a good one does not flip the diagnostic back and forth.
	void RecordBatch(bool succeeded, std::uint64_t writtenBytes, std::uint64_t failedWrites,
		std::uint64_t rotations) noexcept;

	const std::filesystem::path m_windowFolder;
	const std::shared_ptr<ILogFileSystem> m_files;
	const OutputLogFileWriterLimits m_limits;
	const FailingChanged m_onFailingChanged;
	mutable std::mutex m_mutex;
	std::condition_variable m_wake;
	std::vector<PendingWrite> m_pending;
	std::size_t m_pendingBytes{};
	bool m_started{};
	bool m_stopping{};
	std::atomic_bool m_stopRequested{};
	std::thread m_worker;
	OutputLogFileWriterDiagnostics m_diagnostics;
	//! Worker-only: directories already created.
	std::vector<std::filesystem::path> m_createdDirectories;
};

/*!
	The Output service the workbench borrows when session logs are enabled. It
	forwards every call to the selected provider unchanged and, for each Log
	channel, mirrors accepted log entries to that channel's file with the time
	the provider accepted them. Only a fresh `Succeeded` append is mirrored: a
	replayed operation was already written once, and a rejected one never
	entered the channel. Output (plain text) channels are not file-backed,
	matching the issue's scope. The file keeps the full message even when the
	in-memory channel truncated it.
*/
class CFileBackedOutputService final : public output::IOutputService {
public:
	using Clock = std::function<LogTimestamp()>;

	CFileBackedOutputService(output::IOutputService& provider, COutputLogFileWriter& writer,
		Clock clock = CurrentLocalLogTimestamp);

	[[nodiscard]] output::OutputOperationResult CreateChannel(const output::OutputCreateChannelRequest& request) override;
	[[nodiscard]] output::OutputOperationResult AppendOutput(const output::OutputTextMutationRequest& request) override;
	[[nodiscard]] output::OutputOperationResult ReplaceOutput(const output::OutputTextMutationRequest& request) override;
	[[nodiscard]] output::OutputOperationResult AppendLog(const output::OutputLogMutationRequest& request) override;
	[[nodiscard]] output::OutputOperationResult Clear(const output::OutputChannelMutationRequest& request) override;
	[[nodiscard]] output::OutputOperationResult Show(const output::OutputShowChannelRequest& request) override;
	[[nodiscard]] output::OutputOperationResult Hide(const output::OutputChannelMutationRequest& request) override;
	[[nodiscard]] output::OutputOperationResult Dispose(const output::OutputChannelMutationRequest& request) override;
	[[nodiscard]] output::OutputOperationResult DisposeOwner(const output::OutputDisposeOwnerRequest& request) override;
	[[nodiscard]] output::OutputOperationResult Stop() noexcept override;
	[[nodiscard]] output::OutputProviderHealthSnapshot Health() const noexcept override;
	[[nodiscard]] output::OutputServiceSnapshot Snapshot() const override;
	[[nodiscard]] std::optional<output::OutputServiceSubscriptionId> Subscribe(output::OutputServiceListener listener) override;
	void Unsubscribe(output::OutputServiceSubscriptionId subscriptionId) noexcept override;

	//! Waits for any append in flight to reach the writer, then accepts no more
	//! file copies. The runtime calls it after the provider stopped and before
	//! the writer stops, so a line the provider accepted is never dropped
	//! between the two.
	void Quiesce() noexcept;

	//! The file backing `channelId` of `ownerId`, or nothing when that channel
	//! is not a live Log channel. The file exists once a line was written.
	[[nodiscard]] std::optional<std::filesystem::path> LogFileFor(
		std::string_view ownerId, std::string_view channelId) const;

private:
	using ChannelKey = std::pair<std::string, std::string>;

	output::IOutputService& m_provider;
	COutputLogFileWriter& m_writer;
	const Clock m_clock;
	//! Held across the provider call and the queueing so the file order is the
	//! channel order. Recursive: a provider listener may append from inside.
	mutable std::recursive_mutex m_mutex;
	std::map<ChannelKey, std::filesystem::path> m_logChannels;
	bool m_quiesced{};
};

} // namespace workbench::logs

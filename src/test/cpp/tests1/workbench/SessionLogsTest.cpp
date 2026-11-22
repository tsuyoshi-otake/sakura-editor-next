/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#include "pch.h"

#include <gtest/gtest.h>

#include <Windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "workbench/logs/LogFileSystem.h"
#include "workbench/logs/LogSession.h"
#include "workbench/logs/OutputLogFiles.h"
#include "workbench/logs/ProblemsSnapshotFile.h"
#include "workbench/output/OutputService.h"
#include "workbench/problems/MarkerService.h"

namespace workbench::logs {
namespace {

using namespace std::chrono_literals;

//! In-memory ILogFileSystem. Paths are compared as generic strings.
class FakeLogFileSystem final : public ILogFileSystem {
public:
	bool CreateDirectories(const std::filesystem::path& directory) noexcept override
	{
		std::lock_guard lock(mutex);
		++createDirectoryCalls;
		if (failDirectories) return false;
		for (auto current = directory; !current.empty() && current != current.root_path();
			current = current.parent_path()) {
			directories.insert(current.generic_wstring());
		}
		return true;
	}
	bool Append(const std::filesystem::path& file, std::string_view bytes) noexcept override
	{
		std::lock_guard lock(mutex);
		++appendCalls;
		if (failAppends) return false;
		files[file.generic_wstring()] += bytes;
		return true;
	}
	std::optional<std::uint64_t> FileSize(const std::filesystem::path& file) noexcept override
	{
		std::lock_guard lock(mutex);
		const auto found = files.find(file.generic_wstring());
		if (found == files.end()) return std::nullopt;
		return found->second.size();
	}
	bool MoveReplace(const std::filesystem::path& from, const std::filesystem::path& to) noexcept override
	{
		std::lock_guard lock(mutex);
		if (failMoves) return false;
		const auto found = files.find(from.generic_wstring());
		if (found == files.end()) return false;
		files[to.generic_wstring()] = std::move(found->second);
		files.erase(from.generic_wstring());
		return true;
	}
	bool ReplaceContent(const std::filesystem::path& file, std::string_view bytes) noexcept override
	{
		std::lock_guard lock(mutex);
		++replaceCalls;
		if (failReplace) return false;
		files[file.generic_wstring()] = std::string(bytes);
		return true;
	}
	std::optional<std::vector<std::wstring>> ListDirectories(const std::filesystem::path& directory) noexcept override
	{
		std::lock_guard lock(mutex);
		std::vector<std::wstring> names;
		const auto prefix = directory.generic_wstring() + L"/";
		for (const auto& entry : directories) {
			if (entry.starts_with(prefix) && entry.find(L'/', prefix.size()) == std::wstring::npos) {
				names.push_back(entry.substr(prefix.size()));
			}
		}
		return names;
	}
	bool RemoveTree(const std::filesystem::path& directory) noexcept override
	{
		std::lock_guard lock(mutex);
		const auto root = directory.generic_wstring();
		removed.push_back(root);
		std::erase_if(directories, [&](const std::wstring& entry) {
			return entry == root || entry.starts_with(root + L"/");
		});
		return true;
	}

	[[nodiscard]] std::optional<std::string> File(const std::filesystem::path& file) const
	{
		std::lock_guard lock(mutex);
		const auto found = files.find(file.generic_wstring());
		if (found == files.end()) return std::nullopt;
		return found->second;
	}
	void SetFailAppends(bool value)
	{
		std::lock_guard lock(mutex);
		failAppends = value;
	}
	void SetFailReplace(bool value)
	{
		std::lock_guard lock(mutex);
		failReplace = value;
	}
	[[nodiscard]] int ReplaceCalls() const
	{
		std::lock_guard lock(mutex);
		return replaceCalls;
	}

	mutable std::mutex mutex;
	std::set<std::wstring> directories;
	std::map<std::wstring, std::string> files;
	std::vector<std::wstring> removed;
	bool failDirectories{};
	bool failAppends{};
	bool failReplace{};
	bool failMoves{};
	int createDirectoryCalls{};
	int appendCalls{};
	int replaceCalls{};
};

LogTimestamp Timestamp()
{
	return { .year = 2026, .month = 11, .day = 15, .hour = 6, .minute = 41, .second = 22, .millisecond = 7 };
}

template <class Predicate>
bool WaitFor(Predicate predicate, std::chrono::milliseconds timeout = 5000ms)
{
	const auto deadline = std::chrono::steady_clock::now() + timeout;
	while (std::chrono::steady_clock::now() < deadline) {
		if (predicate()) return true;
		std::this_thread::sleep_for(5ms);
	}
	return predicate();
}

// ---- Line format and layout -------------------------------------------------

TEST(SessionLogs, FormatsALineTheWayVsCodeLogFilesDo)
{
	EXPECT_EQ("2026-11-15 06:41:22.007 [info] fetched origin\n",
		FormatOutputLogLine(Timestamp(), output::EOutputLogLevel::Info, "fetched origin"));
	EXPECT_EQ("2026-11-15 06:41:22.007 [warning] w\n",
		FormatOutputLogLine(Timestamp(), output::EOutputLogLevel::Warning, "w"));
	EXPECT_EQ("2026-11-15 06:41:22.007 [error] e\n",
		FormatOutputLogLine(Timestamp(), output::EOutputLogLevel::Error, "e"));
	EXPECT_EQ("2026-11-15 06:41:22.007 [trace] t\n",
		FormatOutputLogLine(Timestamp(), output::EOutputLogLevel::Trace, "t"));
	EXPECT_EQ("2026-11-15 06:41:22.007 [debug] d\n",
		FormatOutputLogLine(Timestamp(), output::EOutputLogLevel::Debug, "d"));
}

TEST(SessionLogs, PlacesCoreAndExtensionChannelsWhereVsCodeDoes)
{
	EXPECT_EQ(std::filesystem::path(L"Tasks.log"), OutputLogRelativePath("core.tasks", "Tasks"));
	EXPECT_EQ(std::filesystem::path(L"exthost") / L"vscode.git" / L"Git.log",
		OutputLogRelativePath("workbench.scm.git", "Git"));
	EXPECT_EQ(std::filesystem::path(L"exthost") / L"publisher.sample" / L"My_Channel_.log",
		OutputLogRelativePath("publisher.sample", "My:Channel?"));
}

TEST(SessionLogs, NeverProducesAFileNameWindowsRefuses)
{
	EXPECT_EQ(std::filesystem::path(L"_CON.log"), OutputLogRelativePath("core.x", "CON"));
	EXPECT_EQ(std::filesystem::path(L"_com1.x.log"), OutputLogRelativePath("core.x", "com1.x"));
	EXPECT_EQ(std::filesystem::path(L"name.log"), OutputLogRelativePath("core.x", "name. ."));
	EXPECT_EQ(std::filesystem::path(L"output.log"), OutputLogRelativePath("core.x", ""));
	EXPECT_EQ(std::filesystem::path(L"a_b_c.log"), OutputLogRelativePath("core.x", "a/b\\c"));
	EXPECT_EQ(std::filesystem::path(L"exthost") / L"extension" / L"x.log", OutputLogRelativePath("..", "x"));
	const auto longName = OutputLogRelativePath("core.x", std::string(300, 'a'));
	EXPECT_EQ(96U + 4U, longName.wstring().size());
	EXPECT_EQ(std::filesystem::path(L"_NUL .x.log"), OutputLogRelativePath("core.x", "NUL .x"));
	EXPECT_EQ(std::filesystem::path(L"_conin$.log"), OutputLogRelativePath("core.x", "conin$"));
	// UTF-8 "COM" + SUPERSCRIPT ONE and "LPT" + SUPERSCRIPT THREE.
	EXPECT_EQ(std::filesystem::path(L"_COM\u00B9.log"), OutputLogRelativePath("core.x", "COM\xC2\xB9"));
	EXPECT_EQ(std::filesystem::path(L"_lpt\u00B3.txt.log"), OutputLogRelativePath("core.x", "lpt\xC2\xB3.txt"));
	EXPECT_EQ(std::filesystem::path(L"COM\u00B9x.log"), OutputLogRelativePath("core.x", "COM\xC2\xB9x"));
}

TEST(SessionLogs, NoChannelFileCanBeAnotherChannelsRotationBackup)
{
	EXPECT_EQ(std::filesystem::path(L"Foo_1.log"), OutputLogRelativePath("core.x", "Foo.1"));
	EXPECT_EQ(std::filesystem::path(L"v2_10.log"), OutputLogRelativePath("core.x", "v2.10"));
	EXPECT_EQ(std::filesystem::path(L"a.b.log"), OutputLogRelativePath("core.x", "a.b"));
	EXPECT_EQ(std::filesystem::path(L"a1.log"), OutputLogRelativePath("core.x", "a1"));
}

// ---- Session folders and retention ------------------------------------------

TEST(SessionLogs, NamesASessionFolderByStartTimeAndProcess)
{
	const auto name = FormatLogSessionFolderName(Timestamp(), 4242);
	EXPECT_EQ(L"20261115T064122-4242", name);
	EXPECT_TRUE(IsLogSessionFolderName(name));
	EXPECT_TRUE(IsLogSessionFolderName(FormatLogSessionFolderName(Timestamp(), 4294967295U)));
	EXPECT_FALSE(IsLogSessionFolderName(L"20261115T064122"));
	EXPECT_FALSE(IsLogSessionFolderName(L"20261115T064122-"));
	EXPECT_FALSE(IsLogSessionFolderName(L"20261115X064122-1"));
	EXPECT_FALSE(IsLogSessionFolderName(L"2026111aT064122-1"));
	EXPECT_FALSE(IsLogSessionFolderName(L"20261115T064122-1a"));
	EXPECT_FALSE(IsLogSessionFolderName(L"notes"));
}

TEST(SessionLogs, KeepsTheNewestTenSessionsCountingTheCurrentOne)
{
	std::vector<std::wstring> names;
	for (int day = 1; day <= 12; ++day) {
		names.push_back(FormatLogSessionFolderName({ .year = 2026, .month = 11, .day = static_cast<std::uint16_t>(day) }, 1));
	}
	names.push_back(L"keep-me");
	const auto current = names[11];
	const auto expired = SelectExpiredLogSessions(names, current);
	// 12 sessions, current is the newest: 9 others stay, 2 oldest go.
	ASSERT_EQ(2U, expired.size());
	EXPECT_EQ(names[1], expired[0]);
	EXPECT_EQ(names[0], expired[1]);
	EXPECT_TRUE(SelectExpiredLogSessions({ names[0], names[1] }, names[1]).empty());
	EXPECT_EQ(1U, SelectExpiredLogSessions({ names[0], names[1] }, names[1], 1).size());
}

TEST(SessionLogs, PrepareCreatesTheWindowFolderAndRemovesOnlyExpiredSessions)
{
	FakeLogFileSystem files;
	const std::filesystem::path root = L"C:/profile/logs";
	for (int day = 1; day <= 11; ++day) {
		ASSERT_TRUE(files.CreateDirectories(
			root / FormatLogSessionFolderName({ .year = 2026, .month = 10, .day = static_cast<std::uint16_t>(day) }, 7)));
	}
	ASSERT_TRUE(files.CreateDirectories(root / L"unrelated"));
	const LogSessionLayout layout{ .root = root, .sessionName = FormatLogSessionFolderName(Timestamp(), 9) };
	const auto oldest = FormatLogSessionFolderName({ .year = 2026, .month = 10, .day = 1 }, 7);
	const auto secondOldest = FormatLogSessionFolderName({ .year = 2026, .month = 10, .day = 2 }, 7);
	std::vector<std::wstring> asked;
	ASSERT_TRUE(PrepareLogSession(files, layout, [&](std::wstring_view name) {
		asked.emplace_back(name);
		// The oldest session's window is still open.
		return name == oldest;
	}));
	EXPECT_TRUE(files.directories.contains(layout.WindowFolder().generic_wstring()));
	// 11 old + current = 12; the two oldest are expired, but the live one stays.
	ASSERT_EQ(1U, files.removed.size());
	EXPECT_EQ((root / secondOldest).generic_wstring(), files.removed.front());
	EXPECT_TRUE(files.directories.contains((root / oldest).generic_wstring()));
	EXPECT_EQ(2U, asked.size());
	EXPECT_TRUE(files.directories.contains((root / L"unrelated").generic_wstring()));
	EXPECT_EQ(layout.root / layout.sessionName / L"window1", layout.WindowFolder());

	FakeLogFileSystem refusing;
	refusing.failDirectories = true;
	EXPECT_FALSE(PrepareLogSession(refusing, layout));
}

TEST(SessionLogs, PrepareGivesUpTheSweepOnceStopIsRequested)
{
	FakeLogFileSystem files;
	const std::filesystem::path root = L"C:/profile/logs";
	for (int day = 1; day <= 12; ++day) {
		ASSERT_TRUE(files.CreateDirectories(root / FormatLogSessionFolderName({ .year = 2026, .month = 9, .day = day }, 7)));
	}
	const LogSessionLayout layout{ .root = root, .sessionName = FormatLogSessionFolderName(Timestamp(), 9) };
	ASSERT_TRUE(PrepareLogSession(files, layout, [](std::wstring_view) { return false; }, [] { return true; }));
	EXPECT_TRUE(files.removed.empty());
	EXPECT_TRUE(files.directories.contains(layout.WindowFolder().generic_wstring()));
}

TEST(SessionLogs, ASessionIsLiveOnlyWhileItsOwnProcessRuns)
{
	FILETIME created{}, exited{}, kernel{}, user{};
	ASSERT_TRUE(::GetProcessTimes(::GetCurrentProcess(), &created, &exited, &kernel, &user));
	FILETIME localCreated{};
	SYSTEMTIME started{};
	ASSERT_TRUE(::FileTimeToLocalFileTime(&created, &localCreated));
	ASSERT_TRUE(::FileTimeToSystemTime(&localCreated, &started));
	const LogTimestamp startedAt{ .year = started.wYear, .month = started.wMonth, .day = started.wDay,
		.hour = started.wHour, .minute = started.wMinute, .second = started.wSecond };
	const auto self = static_cast<std::uint32_t>(::GetCurrentProcessId());
	// This process, named when it started (or later): live.
	EXPECT_TRUE(IsLogSessionProcessRunning(FormatLogSessionFolderName(startedAt, self)));
	EXPECT_TRUE(IsLogSessionProcessRunning(FormatLogSessionFolderName(CurrentLocalLogTimestamp(), self)));
	// The same PID in a folder older than this process: a reused PID.
	EXPECT_FALSE(IsLogSessionProcessRunning(FormatLogSessionFolderName({ .year = 2001, .month = 1, .day = 1 }, self)));
	// PIDs are multiples of four, so 3 never names a process.
	EXPECT_FALSE(IsLogSessionProcessRunning(FormatLogSessionFolderName(Timestamp(), 3)));
	EXPECT_FALSE(IsLogSessionProcessRunning(L"unrelated"));
}

// ---- Writer ------------------------------------------------------------------

TEST(SessionLogs, WriterAppendsInOrderAndStopWritesWhatIsQueued)
{
	auto files = std::make_shared<FakeLogFileSystem>();
	std::atomic_bool preludeRan{};
	COutputLogFileWriter writer(L"C:/logs/s/window1", files);
	ASSERT_TRUE(writer.Start([&](const std::function<bool()>& stopRequested) {
		EXPECT_FALSE(stopRequested());
		preludeRan = true;
	}));
	for (int index = 0; index < 50; ++index) writer.Append(L"Tasks.log", "line " + std::to_string(index) + "\n");
	writer.Append(L"exthost/vscode.git/Git.log", "git\n");
	// The prelude runs on the worker and Stop() sets its predicate, so wait for the
	// prelude first; otherwise a main thread that reaches Stop() first makes it true (#438).
	ASSERT_TRUE(WaitFor([&] { return preludeRan.load(); }));
	writer.Stop();
	EXPECT_TRUE(preludeRan);
	std::string expected;
	for (int index = 0; index < 50; ++index) expected += "line " + std::to_string(index) + "\n";
	EXPECT_EQ(expected, files->File(L"C:/logs/s/window1/Tasks.log"));
	EXPECT_EQ("git\n", files->File(L"C:/logs/s/window1/exthost/vscode.git/Git.log"));
	const auto diagnostics = writer.Diagnostics();
	EXPECT_EQ(expected.size() + 4, diagnostics.writtenBytes);
	EXPECT_FALSE(diagnostics.failing);
	// After Stop nothing is accepted.
	writer.Append(L"Tasks.log", "late\n");
	EXPECT_EQ(expected, files->File(L"C:/logs/s/window1/Tasks.log"));
}

TEST(SessionLogs, WriterRotatesAFullFileToOneBackup)
{
	auto files = std::make_shared<FakeLogFileSystem>();
	COutputLogFileWriter writer(L"C:/w", files, { .maximumFileBytes = 10, .maximumPendingBytes = 1024 });
	ASSERT_TRUE(writer.Start());
	writer.Append(L"a.log", "12345678\n");
	ASSERT_TRUE(WaitFor([&] { return writer.Diagnostics().writtenBytes == 9; }));
	writer.Append(L"a.log", "abc\n");
	ASSERT_TRUE(WaitFor([&] { return writer.Diagnostics().writtenBytes == 13; }));
	writer.Append(L"a.log", "defghijk\n");
	writer.Stop();
	EXPECT_EQ("defghijk\n", files->File(L"C:/w/a.log"));
	EXPECT_EQ("abc\n", files->File(L"C:/w/a.1.log"));
	EXPECT_EQ(2U, writer.Diagnostics().rotations);
}

TEST(SessionLogs, WriterKeepsAppendingWhenAReaderBlocksTheRotation)
{
	auto files = std::make_shared<FakeLogFileSystem>();
	COutputLogFileWriter writer(L"C:/w", files, { .maximumFileBytes = 10, .maximumPendingBytes = 1024 });
	ASSERT_TRUE(writer.Start());
	writer.Append(L"a.log", "12345678\n");
	ASSERT_TRUE(WaitFor([&] { return writer.Diagnostics().writtenBytes == 9; }));
	{
		std::lock_guard lock(files->mutex);
		files->failMoves = true;
	}
	writer.Append(L"a.log", "abc\n");
	writer.Stop();
	EXPECT_EQ("12345678\nabc\n", files->File(L"C:/w/a.log"));
	EXPECT_FALSE(files->File(L"C:/w/a.1.log"));
	EXPECT_EQ(0U, writer.Diagnostics().rotations);
	EXPECT_FALSE(writer.Diagnostics().failing);
}

TEST(SessionLogs, WriterCreatesTheFolderAgainAfterAFailedWrite)
{
	auto files = std::make_shared<FakeLogFileSystem>();
	COutputLogFileWriter writer(L"C:/w", files);
	ASSERT_TRUE(writer.Start());
	writer.Append(L"exthost/x/a.log", "one\n");
	ASSERT_TRUE(WaitFor([&] { return writer.Diagnostics().writtenBytes == 4; }));
	const auto createdOnce = [&] {
		std::lock_guard lock(files->mutex);
		return files->createDirectoryCalls;
	}();
	writer.Append(L"exthost/x/a.log", "two\n");
	ASSERT_TRUE(WaitFor([&] { return writer.Diagnostics().writtenBytes == 8; }));
	{
		std::lock_guard lock(files->mutex);
		// A cached folder is not created again on every write.
		EXPECT_EQ(createdOnce, files->createDirectoryCalls);
		files->failAppends = true;
	}
	writer.Append(L"exthost/x/a.log", "lost\n");
	ASSERT_TRUE(WaitFor([&] { return writer.Diagnostics().failedWrites == 1; }));
	files->SetFailAppends(false);
	writer.Append(L"exthost/x/a.log", "three\n");
	writer.Stop();
	std::lock_guard lock(files->mutex);
	EXPECT_EQ(createdOnce + 1, files->createDirectoryCalls);
}

TEST(SessionLogs, WriterReportsOneFailingStateForABatchWithAGoodAndABadFile)
{
	// bad.log always fails. The first append blocks until released, so every
	// later append queues into one batch holding both files.
	class HalfFailing final : public ILogFileSystem {
	public:
		bool CreateDirectories(const std::filesystem::path&) noexcept override { return true; }
		bool Append(const std::filesystem::path& file, std::string_view) noexcept override
		{
			if (file.filename() == L"prime.log") {
				std::unique_lock lock(mutex);
				entered = true;
				changed.notify_all();
				changed.wait(lock, [this] { return released; });
			}
			return file.filename() != L"bad.log";
		}
		std::mutex mutex;
		std::condition_variable changed;
		bool entered{};
		bool released{};
		std::optional<std::uint64_t> FileSize(const std::filesystem::path&) noexcept override { return std::nullopt; }
		bool MoveReplace(const std::filesystem::path&, const std::filesystem::path&) noexcept override { return false; }
		bool ReplaceContent(const std::filesystem::path&, std::string_view) noexcept override { return false; }
		std::optional<std::vector<std::wstring>> ListDirectories(const std::filesystem::path&) noexcept override
		{
			return std::nullopt;
		}
		bool RemoveTree(const std::filesystem::path&) noexcept override { return false; }
	};
	std::mutex mutex;
	std::vector<bool> transitions;
	const auto files = std::make_shared<HalfFailing>();
	COutputLogFileWriter writer(L"C:/w", files, {}, [&](bool failing) {
		std::lock_guard lock(mutex);
		transitions.push_back(failing);
	});
	ASSERT_TRUE(writer.Start());
	writer.Append(L"prime.log", "p\n");
	{
		std::unique_lock lock(files->mutex);
		ASSERT_TRUE(files->changed.wait_for(lock, 5s, [&] { return files->entered; }));
	}
	for (int index = 0; index < 5; ++index) {
		writer.Append(L"good.log", "g\n");
		writer.Append(L"bad.log", "b\n");
	}
	{
		std::lock_guard lock(files->mutex);
		files->released = true;
	}
	files->changed.notify_all();
	writer.Stop();
	EXPECT_EQ(1U, writer.Diagnostics().failedWrites);
	EXPECT_TRUE(writer.Diagnostics().failing);
	std::lock_guard lock(mutex);
	EXPECT_EQ((std::vector<bool>{ true }), transitions);
}

TEST(SessionLogs, WriterReportsFailingOnceAndRecovers)
{
	auto files = std::make_shared<FakeLogFileSystem>();
	std::mutex mutex;
	std::vector<bool> transitions;
	COutputLogFileWriter writer(L"C:/w", files, {}, [&](bool failing) {
		std::lock_guard lock(mutex);
		transitions.push_back(failing);
	});
	ASSERT_TRUE(writer.Start());
	files->SetFailAppends(true);
	writer.Append(L"a.log", "lost 1\n");
	ASSERT_TRUE(WaitFor([&] { return writer.Diagnostics().failedWrites == 1; }));
	writer.Append(L"a.log", "lost 2\n");
	ASSERT_TRUE(WaitFor([&] { return writer.Diagnostics().failedWrites == 2; }));
	files->SetFailAppends(false);
	writer.Append(L"a.log", "kept\n");
	writer.Stop();
	EXPECT_EQ("kept\n", files->File(L"C:/w/a.log"));
	std::lock_guard lock(mutex);
	EXPECT_EQ((std::vector<bool>{ true, false }), transitions);
	EXPECT_FALSE(writer.Diagnostics().failing);
}

TEST(SessionLogs, WriterDropsAndCountsBytesBeyondThePendingBound)
{
	auto files = std::make_shared<FakeLogFileSystem>();
	COutputLogFileWriter writer(L"C:/w", files, { .maximumFileBytes = 1 << 20, .maximumPendingBytes = 8 });
	// Not started: nothing is accepted, nothing is counted as written.
	writer.Append(L"a.log", "x\n");
	EXPECT_EQ(0U, writer.Diagnostics().writtenBytes);
	ASSERT_TRUE(writer.Start());
	writer.Append(L"a.log", std::string(9, 'z'));
	writer.Stop();
	EXPECT_EQ(9U, writer.Diagnostics().droppedBytes);
	EXPECT_FALSE(files->File(L"C:/w/a.log"));
}

TEST(SessionLogs, WriterWithoutAFileSystemNeverStarts)
{
	COutputLogFileWriter writer(L"C:/w", nullptr);
	EXPECT_FALSE(writer.Start());
	writer.Append(L"a.log", "x\n");
	writer.Stop();
}

// ---- Decorator ---------------------------------------------------------------

output::OutputOwner Owner(const char* id)
{
	return { .ownerId = id, .generation = 1 };
}

output::OutputCreateChannelRequest Create(std::string operationId, const char* owner, std::string channelId,
	std::string label, output::EOutputChannelKind kind)
{
	return {
		.operation = { .operationId = std::move(operationId) },
		.owner = Owner(owner),
		.channelId = std::move(channelId),
		.label = std::move(label),
		.kind = kind,
	};
}

output::OutputLogMutationRequest Log(std::string operationId, const char* owner, std::string channelId,
	std::vector<output::OutputLogEntry> entries)
{
	return {
		.operation = { .operationId = std::move(operationId) },
		.owner = Owner(owner),
		.channelId = std::move(channelId),
		.entries = std::move(entries),
	};
}

TEST(SessionLogs, DecoratorMirrorsAcceptedLogEntriesOnly)
{
	auto files = std::make_shared<FakeLogFileSystem>();
	output::OutputService provider;
	COutputLogFileWriter writer(L"C:/w", files);
	ASSERT_TRUE(writer.Start());
	CFileBackedOutputService service(provider, writer, [] { return Timestamp(); });

	using Kind = output::EOutputChannelKind;
	using Status = output::EOutputOperationStatus;
	ASSERT_EQ(Status::Succeeded, service.CreateChannel(Create("c1", "workbench.scm.git", "git", "Git", Kind::Log)).status);
	ASSERT_EQ(Status::Succeeded, service.CreateChannel(Create("c2", "core.text", "plain", "Plain", Kind::Output)).status);
	EXPECT_EQ(std::filesystem::path(L"C:/w") / L"exthost" / L"vscode.git" / L"Git.log",
		service.LogFileFor("workbench.scm.git", "git"));
	EXPECT_FALSE(service.LogFileFor("core.text", "plain"));
	EXPECT_FALSE(service.LogFileFor("workbench.scm.git", "other"));

	const auto request = Log("l1", "workbench.scm.git", "git",
		{ { .level = output::EOutputLogLevel::Info, .message = "> git status" },
			{ .level = output::EOutputLogLevel::Error, .message = "fatal" } });
	ASSERT_EQ(Status::Succeeded, service.AppendLog(request).status);
	// The same operation again is a replay: the channel does not grow and
	// neither does the file.
	EXPECT_EQ(Status::Replayed, service.AppendLog(request).status);
	// A rejected append (unknown channel) never reaches a file.
	EXPECT_NE(Status::Succeeded, service.AppendLog(Log("l2", "workbench.scm.git", "missing",
		{ { .message = "nope" } })).status);
	// Clear empties the view, never the file.
	ASSERT_EQ(Status::Succeeded, service.Clear({ .operation = { .operationId = "x1" },
		.owner = Owner("workbench.scm.git"), .channelId = "git" }).status);
	ASSERT_EQ(Status::Succeeded, service.Dispose({ .operation = { .operationId = "d1" },
		.owner = Owner("workbench.scm.git"), .channelId = "git" }).status);
	EXPECT_FALSE(service.LogFileFor("workbench.scm.git", "git"));
	writer.Stop();

	EXPECT_EQ("2026-11-15 06:41:22.007 [info] > git status\n2026-11-15 06:41:22.007 [error] fatal\n",
		files->File(L"C:/w/exthost/vscode.git/Git.log"));
	EXPECT_EQ(1U, files->files.size());
	EXPECT_EQ(provider.Snapshot().revision, service.Snapshot().revision);
}

TEST(SessionLogs, DisposeOwnerForgetsEveryChannelOfThatOwner)
{
	auto files = std::make_shared<FakeLogFileSystem>();
	output::OutputService provider;
	COutputLogFileWriter writer(L"C:/w", files);
	ASSERT_TRUE(writer.Start());
	CFileBackedOutputService service(provider, writer, [] { return Timestamp(); });
	using Kind = output::EOutputChannelKind;
	ASSERT_TRUE(service.CreateChannel(Create("c1", "pub.ext", "one", "One", Kind::Log)).Succeeded());
	ASSERT_TRUE(service.CreateChannel(Create("c2", "pub.ext", "two", "Two", Kind::Log)).Succeeded());
	ASSERT_TRUE(service.CreateChannel(Create("c3", "pub.other", "three", "Three", Kind::Log)).Succeeded());
	ASSERT_TRUE(service.DisposeOwner({ .operation = { .operationId = "d" }, .owner = Owner("pub.ext") }).Succeeded());
	EXPECT_FALSE(service.LogFileFor("pub.ext", "one"));
	EXPECT_FALSE(service.LogFileFor("pub.ext", "two"));
	EXPECT_TRUE(service.LogFileFor("pub.other", "three"));
	writer.Stop();
}

TEST(SessionLogs, DecoratorWritesNoFileCopyOnceQuiesced)
{
	auto files = std::make_shared<FakeLogFileSystem>();
	output::OutputService provider;
	COutputLogFileWriter writer(L"C:/w", files);
	ASSERT_TRUE(writer.Start());
	CFileBackedOutputService service(provider, writer, [] { return Timestamp(); });
	ASSERT_TRUE(service.CreateChannel(Create("c", "core.x", "x", "X", output::EOutputChannelKind::Log)).Succeeded());
	ASSERT_TRUE(service.AppendLog(Log("a1", "core.x", "x", { { .message = "kept" } })).Succeeded());
	service.Quiesce();
	// The provider still accepts it; only the file copy stops.
	ASSERT_TRUE(service.AppendLog(Log("a2", "core.x", "x", { { .message = "view only" } })).Succeeded());
	writer.Stop();
	EXPECT_EQ("2026-11-15 06:41:22.007 [info] kept\n", files->File(L"C:/w/X.log"));
}

// ---- problems.json -------------------------------------------------------------

platform::uri::Uri Resource(const wchar_t* value)
{
	auto parsed = platform::uri::Uri::Parse(value);
	EXPECT_TRUE(parsed);
	return std::move(*parsed.value);
}

problems::ReplaceMarkersRequest Markers(const wchar_t* resource, std::vector<std::string> messages)
{
	problems::ReplaceMarkersRequest request{
		.collection = { { "tasks.problemMatcher", 1 }, "tsc" },
		.resource = Resource(resource),
	};
	std::uint32_t line = 0;
	for (auto& message : messages) {
		request.markers.push_back({ .range = { line, 0, line, 3 }, .severity = problems::EMarkerSeverity::Error,
			.message = std::move(message) });
		++line;
	}
	return request;
}

TEST(SessionLogs, EncodesAnEmptyAndATruncatedProblemsFile)
{
	problems::MarkerService markers;
	const auto empty = EncodeProblemsSnapshotFile(markers.Snapshot());
	EXPECT_NE(std::string::npos, empty.find("\"version\": 1"));
	EXPECT_NE(std::string::npos, empty.find("\"markerCount\": 0"));
	EXPECT_NE(std::string::npos, empty.find("\"truncated\": false"));
	EXPECT_NE(std::string::npos, empty.find("\"markers\": []"));

	ASSERT_EQ(problems::EMarkerOperationStatus::Replaced,
		markers.Replace(Markers(L"file:///C:/Work/a.ts", { "first", "second", "third" })).status);
	const auto all = EncodeProblemsSnapshotFile(markers.Snapshot());
	EXPECT_NE(std::string::npos, all.find("\"markerCount\": 3"));
	EXPECT_NE(std::string::npos, all.find("first"));
	EXPECT_NE(std::string::npos, all.find("third"));

	const auto cut = EncodeProblemsSnapshotFile(markers.Snapshot(), { .maximumMarkers = 2, .maximumBytes = 1 << 20 });
	EXPECT_NE(std::string::npos, cut.find("\"markerCount\": 3"));
	EXPECT_NE(std::string::npos, cut.find("\"truncated\": true"));
	EXPECT_NE(std::string::npos, cut.find("second"));
	EXPECT_EQ(std::string::npos, cut.find("third"));

	const auto tiny = EncodeProblemsSnapshotFile(markers.Snapshot(), { .maximumMarkers = 100, .maximumBytes = 10 });
	EXPECT_NE(std::string::npos, tiny.find("\"truncated\": true"));
	EXPECT_NE(std::string::npos, tiny.find("\"markers\": []"));
}

TEST(SessionLogs, ProblemsFileFollowsTheMarkerServiceAndCoalescesBursts)
{
	problems::MarkerService markers;
	auto files = std::make_shared<FakeLogFileSystem>();
	const std::filesystem::path file = L"C:/w/problems.json";
	CProblemsSnapshotFileWriter writer(markers, file, files, 100ms);
	ASSERT_TRUE(writer.Start());
	ASSERT_TRUE(WaitFor([&] { return files->File(file).has_value(); }));
	EXPECT_NE(std::string::npos, files->File(file)->find("\"markerCount\": 0"));
	const int initialWrites = files->ReplaceCalls();

	for (int index = 0; index < 20; ++index) {
		ASSERT_EQ(problems::EMarkerOperationStatus::Replaced,
			markers.Replace(Markers(L"file:///C:/Work/a.ts", { "burst " + std::to_string(index) })).status);
	}
	const auto revision = markers.Snapshot().revision;
	ASSERT_TRUE(WaitFor([&] { return writer.WrittenRevision() == revision; }));
	EXPECT_NE(std::string::npos, files->File(file)->find("burst 19"));
	// Twenty changes inside one coalescing window are far fewer writes.
	EXPECT_LE(files->ReplaceCalls() - initialWrites, 3);

	writer.Stop();
	const int afterStop = files->ReplaceCalls();
	ASSERT_EQ(problems::EMarkerOperationStatus::Replaced,
		markers.Replace(Markers(L"file:///C:/Work/b.ts", { "after stop" })).status);
	std::this_thread::sleep_for(250ms);
	EXPECT_EQ(afterStop, files->ReplaceCalls());
	EXPECT_EQ(std::string::npos, files->File(file)->find("after stop"));
}

TEST(SessionLogs, ProblemsFileReportsAFailedWriteAndRetriesOnTheNextChange)
{
	problems::MarkerService markers;
	auto files = std::make_shared<FakeLogFileSystem>();
	files->SetFailReplace(true);
	std::mutex mutex;
	std::vector<bool> transitions;
	const std::filesystem::path file = L"C:/w/problems.json";
	CProblemsSnapshotFileWriter writer(markers, file, files, 10ms, {}, [&](bool failing) {
		std::lock_guard lock(mutex);
		transitions.push_back(failing);
	});
	ASSERT_TRUE(writer.Start());
	ASSERT_TRUE(WaitFor([&] {
		std::lock_guard lock(mutex);
		return transitions.size() == 1;
	}));
	files->SetFailReplace(false);
	// No marker changes: the writer retries on its own.
	ASSERT_TRUE(WaitFor([&] { return files->File(file).has_value(); }));
	ASSERT_TRUE(WaitFor([&] {
		std::lock_guard lock(mutex);
		return transitions.size() == 2;
	}));
	ASSERT_EQ(problems::EMarkerOperationStatus::Replaced,
		markers.Replace(Markers(L"file:///C:/Work/a.ts", { "retry" })).status);
	ASSERT_TRUE(WaitFor([&] { return writer.WrittenRevision() == markers.Snapshot().revision; }));
	writer.Stop();
	std::lock_guard lock(mutex);
	EXPECT_EQ((std::vector<bool>{ true, false }), transitions);
}

// ---- Real files ----------------------------------------------------------------

class SessionLogsOnDisk : public ::testing::Test {
protected:
	void SetUp() override
	{
		wchar_t temp[MAX_PATH]{};
		ASSERT_NE(0U, ::GetTempPathW(MAX_PATH, temp));
		root = std::filesystem::path(temp) / (L"sakura-session-logs-" + std::to_wstring(::GetCurrentProcessId())
			+ L"-" + std::to_wstring(::GetTickCount64()));
		std::error_code error;
		std::filesystem::remove_all(root, error);
	}
	void TearDown() override
	{
		std::error_code error;
		std::filesystem::remove_all(root, error);
	}
	static std::string Read(const std::filesystem::path& file)
	{
		std::ifstream stream(file, std::ios::binary);
		std::ostringstream content;
		content << stream.rdbuf();
		return content.str();
	}
	std::filesystem::path root;
};

TEST_F(SessionLogsOnDisk, Win32FileSystemDoesWhatTheContractSays)
{
	const auto files = CreateWin32LogFileSystem();
	ASSERT_TRUE(files);
	const auto folder = root / L"20261115T064122-1" / L"window1";
	ASSERT_TRUE(files->CreateDirectories(folder / L"exthost"));
	ASSERT_TRUE(files->CreateDirectories(folder / L"exthost"));
	const auto log = folder / L"Tasks.log";
	ASSERT_TRUE(files->Append(log, "one\n"));
	{
		// A reader holding the file open does not stop the next append.
		std::ifstream reader(log, std::ios::binary);
		ASSERT_TRUE(reader.is_open());
		ASSERT_TRUE(files->Append(log, "two\n"));
	}
	EXPECT_EQ("one\ntwo\n", Read(log));
	EXPECT_EQ(8U, files->FileSize(log));
	EXPECT_FALSE(files->FileSize(folder / L"missing.log"));

	ASSERT_TRUE(files->MoveReplace(log, folder / L"Tasks.1.log"));
	ASSERT_TRUE(files->Append(log, "three\n"));
	ASSERT_TRUE(files->Append(folder / L"Tasks.2.log", "x"));
	ASSERT_TRUE(files->MoveReplace(folder / L"Tasks.2.log", folder / L"Tasks.1.log"));
	EXPECT_EQ("x", Read(folder / L"Tasks.1.log"));
	EXPECT_EQ("three\n", Read(log));

	const auto json = folder / L"problems.json";
	ASSERT_TRUE(files->ReplaceContent(json, "{}"));
	ASSERT_TRUE(files->ReplaceContent(json, "{\"a\":1}"));
	EXPECT_EQ("{\"a\":1}", Read(json));
	EXPECT_FALSE(std::filesystem::exists(json.wstring() + L".tmp"));

	ASSERT_TRUE(files->CreateDirectories(root / L"20261001T000000-2"));
	const auto names = files->ListDirectories(root);
	ASSERT_TRUE(names);
	EXPECT_EQ(2U, names->size());
	ASSERT_TRUE(files->RemoveTree(root / L"20261115T064122-1"));
	EXPECT_FALSE(std::filesystem::exists(folder));
	EXPECT_FALSE(files->ListDirectories(root / L"missing"));
}

TEST_F(SessionLogsOnDisk, AGitChannelLandsInItsVsCodeFile)
{
	const auto files = CreateWin32LogFileSystem();
	const LogSessionLayout layout{ .root = root, .sessionName = FormatLogSessionFolderName(Timestamp(), 77) };
	ASSERT_TRUE(files->CreateDirectories(layout.WindowFolder()));
	output::OutputService provider;
	COutputLogFileWriter writer(layout.WindowFolder(), files);
	ASSERT_TRUE(writer.Start([files, layout](const std::function<bool()>& stop) { (void)PrepareLogSession(*files, layout, IsLogSessionProcessRunning, stop); }));
	CFileBackedOutputService service(provider, writer, [] { return Timestamp(); });
	ASSERT_TRUE(service.CreateChannel(Create("c", "workbench.scm.git", "git", "Git", output::EOutputChannelKind::Log)).Succeeded());
	ASSERT_TRUE(service.AppendLog(Log("a", "workbench.scm.git", "git", { { .message = "> git fetch" } })).Succeeded());
	writer.Stop();
	const auto file = service.LogFileFor("workbench.scm.git", "git");
	ASSERT_TRUE(file);
	EXPECT_EQ(root / L"20261115T064122-77" / L"window1" / L"exthost" / L"vscode.git" / L"Git.log", *file);
	EXPECT_EQ("2026-11-15 06:41:22.007 [info] > git fetch\n", Read(*file));
}

} // namespace
} // namespace workbench::logs

/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#include "pch.h"

#include "config/CConfigurationService.h"
#include "config/ConfigurationFileSourceController.h"
#include "config/ConfigurationFileWatchController.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace {

using config::CConfigurationFileSourceController;
using config::CConfigurationFileWatchController;
using config::ConfigurationFileWatchRequest;
using config::EConfigurationFileWatchChange;
using config::CConfigurationService;
using config::ConfigurationDescriptor;
using config::ConfigurationSource;
using config::ConfigurationTarget;
using config::ConfigurationValue;
using config::EConfigurationFileSourceControllerStatus;
using config::EConfigurationOutcome;
using config::EConfigurationScope;
using platform::filesystem::DirectoryEntry;
using platform::filesystem::EFileResultStatus;
using platform::filesystem::FileBytes;
using platform::filesystem::FileReadOptions;
using platform::filesystem::FileResult;
using platform::filesystem::FileStat;
using platform::filesystem::IFileService;
using platform::filesystem::IFileWatch;
using platform::filesystem::FileWatchEvent;
using platform::filesystem::EFileWatchEventType;
using platform::uri::Uri;

Uri Resource()
{
	auto parsed = Uri::Parse(L"file:///C:/Profile/settings.json");
	EXPECT_TRUE(parsed.value.has_value());
	return *parsed.value;
}

Uri TestWorkspaceFolder()
{
	auto parsed = Uri::Parse(L"file:///C:/Workspace");
	EXPECT_TRUE(parsed.value.has_value());
	return *parsed.value;
}

Uri WorkspaceConfiguration()
{
	auto parsed = Uri::Parse(L"file:///C:/Workspace/project.code-workspace");
	EXPECT_TRUE(parsed.value.has_value());
	return *parsed.value;
}

ConfigurationSource Source(const char* sourceId = "profile-settings")
{
	ConfigurationTarget target;
	target.profileId = L"profile-1";
	return { EConfigurationScope::Profile, std::move(target), sourceId, 0 };
}

CConfigurationService Service()
{
	return CConfigurationService({
		{ "editor.tabSize", ConfigurationValue(4), { EConfigurationScope::Profile, EConfigurationScope::LanguageOverride } },
	});
}

std::int64_t TabSize(const CConfigurationService& service, const wchar_t* language = nullptr)
{
	ConfigurationTarget target;
	target.profileId = L"profile-1";
	if (language) target.languageId = language;
	auto result = service.GetValue("editor.tabSize", target);
	EXPECT_EQ(EConfigurationOutcome::Applied, result.outcome);
	return std::get<std::int64_t>(result.value->Value());
}

class FakeFileService final : public IFileService {
public:
	class FakeWatch final : public IFileWatch {
	public:
		explicit FakeWatch(Uri watchRoot) : root(std::move(watchRoot)) {}
		FileResult<void> Cancel() override
		{
			std::lock_guard lock(mutex);
			if (!cancelled) {
				cancelled = true;
				events.push_back({ .type = EFileWatchEventType::Disposed, .uri = root });
			}
			ready.notify_all();
			return FileResult<void>::Success();
		}
		FileResult<FileWatchEvent> Next() override
		{
			std::unique_lock lock(mutex);
			ready.wait(lock, [this] { return !events.empty(); });
			auto event = std::move(events.front());
			events.pop_front();
			if (event.type == EFileWatchEventType::Disposed) return FileResult<FileWatchEvent>::Success(std::move(event));
			return FileResult<FileWatchEvent>::Success(std::move(event));
		}
		void Push(FileWatchEvent event)
		{
			std::lock_guard lock(mutex);
			events.push_back(std::move(event));
			ready.notify_one();
		}
		bool WaitUntilCancelled()
		{
			std::unique_lock lock(mutex);
			return ready.wait_for(lock, std::chrono::seconds(2), [this] { return cancelled; });
		}

		Uri root;
	private:
		std::mutex mutex;
		std::condition_variable ready;
		std::deque<FileWatchEvent> events;
		bool cancelled = false;
	};

	FileResult<FileStat> Stat(const Uri&) override { return FileResult<FileStat>::Failure(EFileResultStatus::Unsupported); }
	FileResult<std::vector<DirectoryEntry>> Enumerate(const Uri&) override { return FileResult<std::vector<DirectoryEntry>>::Failure(EFileResultStatus::Unsupported); }
	FileResult<FileBytes> Read(const Uri&, const FileReadOptions& options) override
	{
		++readCalls;
		lastMaximumBytes = options.maximumBytes;
		return nextRead;
	}
	FileResult<std::unique_ptr<IFileWatch>> Watch(const Uri& root, const platform::filesystem::FileWatchOptions&) override
	{
		if (!watchEnabled) return FileResult<std::unique_ptr<IFileWatch>>::Failure(EFileResultStatus::Unsupported);
		bool reject = false;
		bool overflow = false;
		{
			std::lock_guard lock(watchMutex);
			reject = rejectVscodeWatch;
			overflow = m_overflowEveryWatch;
		}
		if (reject && root.Path().ends_with(L"/.vscode")) {
			return FileResult<std::unique_ptr<IFileWatch>>::Failure(EFileResultStatus::NotFound);
		}
		auto watch = std::make_unique<FakeWatch>(root);
		if (overflow) watch->Push({ .type = EFileWatchEventType::Overflow, .uri = root });
		{
			std::lock_guard lock(watchMutex);
			watchRoots.push_back(root);
			watches.push_back(watch.get());
		}
		return FileResult<std::unique_ptr<IFileWatch>>::Success(std::move(watch));
	}
	std::size_t WatchCount()
	{
		std::lock_guard lock(watchMutex);
		return watchRoots.size();
	}
	//! Models a watch source that invalidates its topology as soon as it is
	//! armed, e.g. a directory whose change buffer overflows continuously.
	void OverflowEveryWatch()
	{
		std::lock_guard lock(watchMutex);
		m_overflowEveryWatch = true;
	}

	FileResult<FileBytes> nextRead = FileResult<FileBytes>::Failure(EFileResultStatus::NotFound);
	int readCalls = 0;
	std::size_t lastMaximumBytes = 0;
	bool watchEnabled = false;
	bool rejectVscodeWatch = false;
	std::mutex watchMutex;
	std::vector<Uri> watchRoots;
	std::vector<FakeWatch*> watches;

private:
	bool m_overflowEveryWatch = false;
};

FileResult<FileBytes> Bytes(std::string document)
{
	return FileResult<FileBytes>::Success(FileBytes(document.begin(), document.end()));
}

class CountingConfigurationService final : public config::IConfigurationService {
public:
	config::ConfigurationLookupResult GetValue(const std::string&, const ConfigurationTarget&) const override { return {}; }
	config::ConfigurationReadSnapshotResult ReadSnapshot(const std::vector<std::string>&, const ConfigurationTarget&) const override { return {}; }
	config::ConfigurationInspection Inspect(const std::string&, const ConfigurationTarget&) const override { return {}; }
	config::ConfigurationResult Update(const config::ConfigurationUpdate&) override { return {}; }
	config::ConfigurationResult ReplaceSource(const config::ConfigurationReplaceSource&) override { return {}; }
	config::ConfigurationBatchResult ReplaceSources(const config::ConfigurationReplaceSources&) override
	{
		++replaceCalls;
		return response;
	}
	config::ConfigurationSubscription Subscribe(config::ConfigurationListener) override { return {}; }

	int replaceCalls = 0;
	config::ConfigurationBatchResult response { EConfigurationOutcome::Conflict, {}, {} };
};

//! Starts watch workers on real threads but, once armed, lets a fixed number
//! of further starts succeed and then throws what std::thread throws when the
//! system cannot create a thread (#397). It counts the workers still running.
class FaultingWorkerStarter final {
public:
	CConfigurationFileWatchController::WorkerStarter Starter()
	{
		return [this](std::function<void()> body) -> std::thread {
			{
				std::lock_guard lock(m_mutex);
				if (m_successesBeforeFailure) {
					if (*m_successesBeforeFailure == 0) {
						throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));
					}
					--*m_successesBeforeFailure;
				}
			}
			m_running.fetch_add(1);
			return std::thread([this, body = std::move(body)] {
				body();
				m_running.fetch_sub(1);
			});
		};
	}
	void FailAfter(int successes)
	{
		std::lock_guard lock(m_mutex);
		m_successesBeforeFailure = successes;
	}
	void Heal()
	{
		std::lock_guard lock(m_mutex);
		m_successesBeforeFailure.reset();
	}
	int Running() const noexcept { return m_running.load(); }

private:
	std::mutex m_mutex;
	std::optional<int> m_successesBeforeFailure;
	std::atomic_int m_running = 0;
};

} // namespace

TEST(ConfigurationFileSourceController, AppliesReloadsWithCasAndClearsMissingLanguageContributions)
{
	FakeFileService files;
	auto service = Service();
	CConfigurationFileSourceController controller(files, service);
	files.nextRead = Bytes(R"json({ "editor.tabSize": 2, "[cpp]": { "editor.tabSize": 3 } })json");

	auto first = controller.Reload("profile-settings", Source(), Resource());
	ASSERT_EQ(EConfigurationFileSourceControllerStatus::Applied, first.status);
	EXPECT_EQ(config::CJsoncConfigurationSource::kMaximumInputBytes, files.lastMaximumBytes);
	EXPECT_EQ(2, TabSize(service));
	EXPECT_EQ(3, TabSize(service, L"cpp"));

	files.nextRead = Bytes(R"json({ "editor.tabSize": 7 })json");
	auto second = controller.Reload("profile-settings", Source(), Resource());
	ASSERT_EQ(EConfigurationFileSourceControllerStatus::Applied, second.status);
	EXPECT_EQ(7, TabSize(service));
	EXPECT_EQ(7, TabSize(service, L"cpp"));

	files.nextRead = FileResult<FileBytes>::Failure(EFileResultStatus::NotFound);
	auto missing = controller.Reload("profile-settings", Source(), Resource());
	EXPECT_TRUE(missing.Succeeded());
	EXPECT_TRUE(missing.resourceWasMissing);
	ASSERT_TRUE(missing.fileStatus.has_value());
	EXPECT_EQ(EFileResultStatus::NotFound, *missing.fileStatus);
	EXPECT_EQ(4, TabSize(service));
}

TEST(ConfigurationFileSourceController, ParseAndReadErrorsPreserveLastAcceptedModelWithoutServiceMutation)
{
	FakeFileService files;
	auto service = Service();
	CConfigurationFileSourceController controller(files, service);
	files.nextRead = Bytes(R"json({ "editor.tabSize": 2 })json");
	ASSERT_TRUE(controller.Reload("profile-settings", Source(), Resource()).Succeeded());

	files.nextRead = Bytes(R"json({ "editor.tabSize": 3, "editor.tabSize": 4 })json");
	auto parse = controller.Reload("profile-settings", Source(), Resource());
	EXPECT_EQ(EConfigurationFileSourceControllerStatus::ParseFailed, parse.status);
	EXPECT_EQ(2, TabSize(service));

	files.nextRead = FileResult<FileBytes>::Failure(EFileResultStatus::PermissionDenied);
	auto read = controller.Reload("profile-settings", Source(), Resource());
	EXPECT_EQ(EConfigurationFileSourceControllerStatus::ReadFailed, read.status);
	EXPECT_EQ(2, TabSize(service));
}

TEST(ConfigurationFileSourceController, RemoveOnlyForgetsTrackingAfterSuccessfulClearAndRejectsIdentityChanges)
{
	FakeFileService files;
	auto service = Service();
	CConfigurationFileSourceController controller(files, service);
	files.nextRead = Bytes(R"json({ "editor.tabSize": 2 })json");
	ASSERT_TRUE(controller.Reload("profile-settings", Source(), Resource()).Succeeded());

	auto mismatch = controller.Reload("profile-settings", Source("different-source"), Resource());
	EXPECT_EQ(EConfigurationFileSourceControllerStatus::IdentityConflict, mismatch.status);
	EXPECT_EQ(2, TabSize(service));

	auto removed = controller.Remove("profile-settings");
	EXPECT_TRUE(removed.Succeeded());
	EXPECT_EQ(4, TabSize(service));
	EXPECT_EQ(EConfigurationFileSourceControllerStatus::NotTracked, controller.Deactivate("profile-settings").status);
}

TEST(ConfigurationFileSourceController, ReturnsConflictWithoutChangingTrackedState)
{
	FakeFileService files;
	CountingConfigurationService service;
	CConfigurationFileSourceController controller(files, service);
	files.nextRead = Bytes(R"json({ "editor.tabSize": 2 })json");

	auto conflict = controller.Reload("profile-settings", Source(), Resource());
	EXPECT_EQ(EConfigurationFileSourceControllerStatus::Conflict, conflict.status);
	EXPECT_EQ(EConfigurationOutcome::Conflict, conflict.configurationOutcome);
	EXPECT_EQ(1, service.replaceCalls);
}

TEST(ConfigurationFileSourceController, ParseAndNonMissingReadFailuresNeverCallTheConfigurationService)
{
	FakeFileService files;
	CountingConfigurationService service;
	service.response = { EConfigurationOutcome::Applied, {}, {} };
	CConfigurationFileSourceController controller(files, service);

	files.nextRead = Bytes(R"json({ "editor.tabSize": 2, "editor.tabSize": 3 })json");
	EXPECT_EQ(EConfigurationFileSourceControllerStatus::ParseFailed,
		controller.Reload("profile-settings", Source(), Resource()).status);
	EXPECT_EQ(0, service.replaceCalls);

	files.nextRead = FileResult<FileBytes>::Failure(EFileResultStatus::PermissionDenied);
	EXPECT_EQ(EConfigurationFileSourceControllerStatus::ReadFailed,
		controller.Reload("profile-settings", Source(), Resource()).status);
	EXPECT_EQ(0, service.replaceCalls);
}

TEST(ConfigurationFileSourceController, IndependentControllersUseDistinctGlobalOperationIds)
{
	FakeFileService firstFiles;
	FakeFileService secondFiles;
	auto service = Service();
	CConfigurationFileSourceController first(firstFiles, service);
	CConfigurationFileSourceController second(secondFiles, service);
	firstFiles.nextRead = Bytes(R"json({ "editor.tabSize": 2 })json");
	secondFiles.nextRead = Bytes(R"json({ "editor.tabSize": 3 })json");

	EXPECT_TRUE(first.Reload("first-profile-settings", Source("first-source"), Resource()).Succeeded());
	const auto secondResult = second.Reload(
		"second-profile-settings", Source("second-source"), Resource());
	EXPECT_TRUE(secondResult.Succeeded());
	EXPECT_NE(EConfigurationFileSourceControllerStatus::OperationIdConflict, secondResult.status);
}

TEST(ConfigurationFileWatchController, WatchesProfileWorkspaceAndTwoLevelInitiallyMissingVscodeTopology)
{
	FakeFileService files;
	files.watchEnabled = true;
	files.rejectVscodeWatch = true;
	CConfigurationFileWatchController controller(files);
	std::mutex callbackMutex;
	std::condition_variable callbackReady;
	std::vector<EConfigurationFileWatchChange> changes;
	ASSERT_EQ(config::EConfigurationFileWatchStatus::Started, controller.Start({
		.profileSettings = Resource(),
		.workspaceFolders = { TestWorkspaceFolder(), TestWorkspaceFolder() },
		.workspaceConfiguration = WorkspaceConfiguration(),
	}, [&](EConfigurationFileWatchChange change) {
		std::lock_guard lock(callbackMutex);
		changes.push_back(change);
		callbackReady.notify_all();
	}).status);

	{
		std::lock_guard lock(files.watchMutex);
		// The initially-missing `.vscode` does not prevent its parent folder
		// lifecycle watch from being installed.
		EXPECT_EQ(2u, files.watchRoots.size());
		ASSERT_GE(files.watches.size(), 2u);
		files.rejectVscodeWatch = false;
		files.watches[1]->Push({ .type = EFileWatchEventType::Created,
			.uri = Uri::Parse(L"file:///C:/Workspace/.vscode").value.value() });
	}
	{
		std::unique_lock lock(callbackMutex);
		ASSERT_TRUE(callbackReady.wait_for(lock, std::chrono::seconds(2), [&] { return !changes.empty(); }));
		EXPECT_EQ(EConfigurationFileWatchChange::FullRescan, changes.front());
	}
	{
		std::lock_guard lock(files.watchMutex);
		// Rebuild installs the now-available `.vscode/settings.json` member watch.
		EXPECT_EQ(5u, files.watchRoots.size());
	}
	EXPECT_EQ(config::EConfigurationFileWatchStatus::Stopped, controller.Stop().status);
}

TEST(ConfigurationFileWatchController, RejectsInvalidResourcesAndUnboundedFolderTopologiesBeforeStarting)
{
	FakeFileService files;
	CConfigurationFileWatchController controller(files);
	auto nonFile = Uri::Parse(L"https://example.invalid/settings.json");
	ASSERT_TRUE(nonFile.value.has_value());
	EXPECT_EQ(config::EConfigurationFileWatchStatus::InvalidRequest,
		controller.Start({ .profileSettings = *nonFile.value }, {}).status);
	auto queried = Uri::Parse(L"file:///C:/Profile/settings.json?unexpected=true");
	ASSERT_TRUE(queried.value.has_value());
	EXPECT_EQ(config::EConfigurationFileWatchStatus::InvalidRequest,
		controller.Start({ .profileSettings = *queried.value }, {}).status);
	ConfigurationFileWatchRequest oversized { .profileSettings = Resource() };
	oversized.workspaceFolders.assign(65, TestWorkspaceFolder());
	EXPECT_EQ(config::EConfigurationFileWatchStatus::CapacityExceeded,
		controller.Start(std::move(oversized), {}).status);
	EXPECT_EQ(config::EConfigurationFileWatchStatus::NotStarted, controller.Stop().status);
}

TEST(ConfigurationFileWatchController, OverflowRebuildsAndStopsWithoutPostStopApply)
{
	FakeFileService files;
	files.watchEnabled = true;
	CConfigurationFileWatchController controller(files);
	std::mutex callbackMutex;
	std::condition_variable callbackReady;
	int callbacks = 0;
	ASSERT_EQ(config::EConfigurationFileWatchStatus::Started, controller.Start({ .profileSettings = Resource() }, [&](EConfigurationFileWatchChange change) {
		if (change == EConfigurationFileWatchChange::FullRescan) {
			std::lock_guard lock(callbackMutex);
			++callbacks;
			callbackReady.notify_all();
		}
	}).status);
	{
		std::lock_guard lock(files.watchMutex);
		ASSERT_FALSE(files.watches.empty());
		files.watches.front()->Push({ .type = EFileWatchEventType::Overflow, .uri = files.watchRoots.front() });
	}
	{
		std::unique_lock lock(callbackMutex);
		ASSERT_TRUE(callbackReady.wait_for(lock, std::chrono::seconds(2), [&] { return callbacks == 1; }));
	}
	EXPECT_EQ(config::EConfigurationFileWatchStatus::Stopped, controller.Stop().status);
	{
		std::lock_guard lock(callbackMutex);
		EXPECT_EQ(1, callbacks);
	}
}

TEST(ConfigurationFileWatchController, ConcurrentStopsHaveOneJoinOwnerAndRejectDispatcherReentrancy)
{
	FakeFileService files;
	files.watchEnabled = true;
	CConfigurationFileWatchController controller(files);
	std::mutex callbackMutex;
	std::condition_variable callbackReady;
	bool callbackEntered = false;
	bool releaseCallback = false;
	config::ConfigurationFileWatchResult callbackStop;
	ASSERT_EQ(config::EConfigurationFileWatchStatus::Started,
		controller.Start({ .profileSettings = Resource() }, [&](EConfigurationFileWatchChange) {
			callbackStop = controller.Stop();
			std::unique_lock lock(callbackMutex);
			callbackEntered = true;
			callbackReady.notify_all();
			callbackReady.wait(lock, [&] { return releaseCallback; });
		}).status);

	FakeFileService::FakeWatch* watch = nullptr;
	{
		std::lock_guard lock(files.watchMutex);
		ASSERT_FALSE(files.watches.empty());
		watch = files.watches.front();
		watch->Push({ .type = EFileWatchEventType::Changed, .uri = Resource() });
	}
	{
		std::unique_lock lock(callbackMutex);
		ASSERT_TRUE(callbackReady.wait_for(lock, std::chrono::seconds(2), [&] { return callbackEntered; }));
	}
	EXPECT_EQ(config::EConfigurationFileWatchStatus::ReentrantStopDenied, callbackStop.status);

	config::ConfigurationFileWatchResult firstStop;
	config::ConfigurationFileWatchResult secondStop;
	std::thread first([&] { firstStop = controller.Stop(); });
	const bool firstCancelled = watch->WaitUntilCancelled();
	std::atomic_bool secondStarted = false;
	std::thread second([&] {
		secondStarted.store(true, std::memory_order_release);
		secondStop = controller.Stop();
	});
	while (!secondStarted.load(std::memory_order_acquire)) std::this_thread::yield();
	{
		std::lock_guard lock(callbackMutex);
		releaseCallback = true;
	}
	callbackReady.notify_all();
	first.join();
	second.join();

	ASSERT_TRUE(firstCancelled);
	EXPECT_EQ(config::EConfigurationFileWatchStatus::Stopped, firstStop.status);
	EXPECT_EQ(config::EConfigurationFileWatchStatus::NotStarted, secondStop.status);
}

// #395: CWorkbenchRuntime answers every FullRescan by reloading settings and
// then calling Update with the request derived from its unchanged workspace.
// When an unchanged Update rebuilt the topology and announced another
// FullRescan, the pair recreated every watch thread each 25 ms debounce for the
// life of the window. One rebuild must stay one rebuild.
TEST(ConfigurationFileWatchController, UnchangedTopologyUpdateFromRescanCallbackDoesNotRebuildAgain)
{
	FakeFileService files;
	files.watchEnabled = true;
	CConfigurationFileWatchController controller(files);
	const ConfigurationFileWatchRequest request {
		.profileSettings = Resource(),
		.workspaceFolders = { TestWorkspaceFolder() },
	};
	std::mutex callbackMutex;
	std::condition_variable callbackReady;
	int rescans = 0;
	std::vector<config::EConfigurationFileWatchStatus> refreshes;
	ASSERT_EQ(config::EConfigurationFileWatchStatus::Started, controller.Start(request, [&](EConfigurationFileWatchChange change) {
		if (change != EConfigurationFileWatchChange::FullRescan) return;
		const auto refreshed = controller.Update(request);
		std::lock_guard lock(callbackMutex);
		++rescans;
		refreshes.push_back(refreshed.status);
		callbackReady.notify_all();
	}).status);

	// Profile parent, workspace folder, and its `.vscode` directory.
	const auto initialWatches = files.WatchCount();
	ASSERT_EQ(3u, initialWatches);
	{
		std::lock_guard lock(files.watchMutex);
		files.watches.front()->Push({ .type = EFileWatchEventType::Overflow, .uri = files.watchRoots.front() });
	}
	{
		std::unique_lock lock(callbackMutex);
		ASSERT_TRUE(callbackReady.wait_for(lock, std::chrono::seconds(2), [&] { return rescans >= 1; }));
		// A loop delivers the next FullRescan within one debounce interval, so
		// 500 ms of silence spans many would-be cycles.
		EXPECT_FALSE(callbackReady.wait_for(lock, std::chrono::milliseconds(500), [&] { return rescans > 1; }));
		EXPECT_EQ(1, rescans);
		for (const auto status : refreshes) EXPECT_EQ(config::EConfigurationFileWatchStatus::Updated, status);
	}
	// Exactly one rebuild: the overflow recreated each watch once.
	EXPECT_EQ(initialWatches * 2, files.WatchCount());
	EXPECT_EQ(config::EConfigurationFileWatchStatus::Stopped, controller.Stop().status);
}

TEST(ConfigurationFileWatchController, UpdateRebuildsOnlyWhenTheTopologyIdentityChanges)
{
	FakeFileService files;
	files.watchEnabled = true;
	CConfigurationFileWatchController controller(files);
	std::mutex callbackMutex;
	std::condition_variable callbackReady;
	int rescans = 0;
	ASSERT_EQ(config::EConfigurationFileWatchStatus::Started, controller.Start({ .profileSettings = Resource() },
		[&](EConfigurationFileWatchChange change) {
			if (change != EConfigurationFileWatchChange::FullRescan) return;
			std::lock_guard lock(callbackMutex);
			++rescans;
			callbackReady.notify_all();
		}).status);
	ASSERT_EQ(1u, files.WatchCount());

	// A differently-cased spelling of the same local file is the same identity,
	// so this Update is idempotent: no rebuild and no notification.
	auto respelled = Uri::Parse(L"file:///c:/profile/SETTINGS.json");
	ASSERT_TRUE(respelled.value.has_value());
	EXPECT_EQ(config::EConfigurationFileWatchStatus::Updated, controller.Update({ .profileSettings = *respelled.value }).status);
	{
		std::unique_lock lock(callbackMutex);
		EXPECT_FALSE(callbackReady.wait_for(lock, std::chrono::milliseconds(300), [&] { return rescans > 0; }));
	}
	EXPECT_EQ(1u, files.WatchCount());

	// Adding a folder is a real topology change: rebuild once and announce it.
	EXPECT_EQ(config::EConfigurationFileWatchStatus::Updated, controller.Update({
		.profileSettings = Resource(),
		.workspaceFolders = { TestWorkspaceFolder() },
	}).status);
	{
		std::unique_lock lock(callbackMutex);
		ASSERT_TRUE(callbackReady.wait_for(lock, std::chrono::seconds(2), [&] { return rescans == 1; }));
	}
	EXPECT_EQ(1u + 3u, files.WatchCount());
	EXPECT_EQ(config::EConfigurationFileWatchStatus::Stopped, controller.Stop().status);
}

// A watch source that invalidates the topology every time it is armed is a
// genuine, repeated rescan. It must still be rebuilt (recovery is not
// abandoned), but consecutive rebuilds back off instead of recreating every
// watch thread at the debounce rate, and Stop must not wait out a backoff.
TEST(ConfigurationFileWatchController, RepeatedTopologyInvalidationBacksOffAndStopInterruptsTheWait)
{
	FakeFileService files;
	files.watchEnabled = true;
	files.OverflowEveryWatch();
	CConfigurationFileWatchController controller(files);
	std::atomic_int rescans = 0;
	ASSERT_EQ(config::EConfigurationFileWatchStatus::Started, controller.Start({ .profileSettings = Resource() },
		[&](EConfigurationFileWatchChange change) {
			if (change == EConfigurationFileWatchChange::FullRescan) rescans.fetch_add(1, std::memory_order_relaxed);
		}).status);

	std::this_thread::sleep_for(std::chrono::milliseconds(1200));
	const auto watchesAfterWindow = files.WatchCount();
	// Without backoff, a 25 ms cycle recreates the single watch about 45 times
	// in this window. The bound is an upper bound only, so a slow machine can
	// make it easier to meet but never harder.
	EXPECT_LE(watchesAfterWindow, 8u);
	EXPECT_GE(watchesAfterWindow, 2u);
	EXPECT_GE(rescans.load(std::memory_order_relaxed), 1);

	const auto stopBegan = std::chrono::steady_clock::now();
	EXPECT_EQ(config::EConfigurationFileWatchStatus::Stopped, controller.Stop().status);
	EXPECT_LT(std::chrono::steady_clock::now() - stopBegan, std::chrono::milliseconds(500));
}

// A rebuild runs on the dispatcher inside noexcept code. When it cannot start
// its workers it must not terminate the editor (#397): it joins the workers it
// did start, reports WatchUnavailable, and retries under the rebuild backoff.
TEST(ConfigurationFileWatchController, RebuildThatCannotStartItsWorkersReportsUnavailableJoinsThemAndRetries)
{
	FakeFileService files;
	files.watchEnabled = true;
	FaultingWorkerStarter starter;
	// Declared before the controller, whose destructor may still run the
	// dispatcher's last callback after an early ASSERT return.
	std::mutex callbackMutex;
	std::condition_variable callbackReady;
	std::vector<EConfigurationFileWatchChange> changes;
	CConfigurationFileWatchController controller(files, starter.Starter());
	// Profile parent, workspace folder, and its .vscode: three workers.
	ASSERT_EQ(config::EConfigurationFileWatchStatus::Started, controller.Start({
		.profileSettings = Resource(),
		.workspaceFolders = { TestWorkspaceFolder() },
	}, [&](EConfigurationFileWatchChange change) {
		std::lock_guard lock(callbackMutex);
		changes.push_back(change);
		callbackReady.notify_all();
	}).status);
	ASSERT_EQ(3, starter.Running());

	// The rebuild starts one worker, then fails on the second.
	starter.FailAfter(1);
	{
		std::lock_guard lock(files.watchMutex);
		ASSERT_FALSE(files.watches.empty());
		files.watches.front()->Push({ .type = EFileWatchEventType::Overflow, .uri = files.watchRoots.front() });
	}
	{
		std::unique_lock lock(callbackMutex);
		ASSERT_TRUE(callbackReady.wait_for(lock, std::chrono::seconds(2), [&] { return !changes.empty(); }));
		EXPECT_EQ(EConfigurationFileWatchChange::WatchUnavailable, changes.front());
		// The callback runs after the started worker was cancelled and joined.
		EXPECT_EQ(0, starter.Running());
	}

	// Once threads can be created again, the retried rebuild watches the
	// whole topology and announces it with FullRescan.
	starter.Heal();
	{
		std::unique_lock lock(callbackMutex);
		ASSERT_TRUE(callbackReady.wait_for(lock, std::chrono::seconds(5), [&] {
			return changes.back() == EConfigurationFileWatchChange::FullRescan;
		}));
		for (std::size_t index = 0; index + 1 < changes.size(); ++index) {
			EXPECT_EQ(EConfigurationFileWatchChange::WatchUnavailable, changes[index]);
		}
	}
	EXPECT_EQ(3, starter.Running());

	const auto stopBegan = std::chrono::steady_clock::now();
	EXPECT_EQ(config::EConfigurationFileWatchStatus::Stopped, controller.Stop().status);
	EXPECT_LT(std::chrono::steady_clock::now() - stopBegan, std::chrono::milliseconds(500));
	EXPECT_EQ(0, starter.Running());
}

// A rebuild that keeps failing stays bounded by the rebuild backoff instead of
// retrying at the debounce rate, and Stop interrupts the wait.
TEST(ConfigurationFileWatchController, RebuildThatKeepsFailingToStartWorkersBacksOffAndStopsPromptly)
{
	FakeFileService files;
	files.watchEnabled = true;
	FaultingWorkerStarter starter;
	std::atomic_int unavailable = 0;
	CConfigurationFileWatchController controller(files, starter.Starter());
	ASSERT_EQ(config::EConfigurationFileWatchStatus::Started, controller.Start({ .profileSettings = Resource() },
		[&](EConfigurationFileWatchChange change) {
			if (change == EConfigurationFileWatchChange::WatchUnavailable) unavailable.fetch_add(1);
		}).status);
	starter.FailAfter(0);
	{
		std::lock_guard lock(files.watchMutex);
		ASSERT_FALSE(files.watches.empty());
		files.watches.front()->Push({ .type = EFileWatchEventType::Overflow, .uri = files.watchRoots.front() });
	}
	std::this_thread::sleep_for(std::chrono::milliseconds(1200));
	// 100 ms doubling with up to +25% jitter allows at most 5 attempts in the
	// window (0, ~125, ~375, ~875 ms plus debounce); the 25 ms debounce alone
	// would allow about 45. Like the existing backoff test, this is an upper
	// bound that a slow machine only makes easier to meet.
	EXPECT_GE(unavailable.load(), 1);
	EXPECT_LE(unavailable.load(), 6);
	EXPECT_EQ(0, starter.Running());

	const auto stopBegan = std::chrono::steady_clock::now();
	EXPECT_EQ(config::EConfigurationFileWatchStatus::Stopped, controller.Stop().status);
	EXPECT_LT(std::chrono::steady_clock::now() - stopBegan, std::chrono::milliseconds(500));
}

// Start already reports StartFailed for a worker it cannot create; it must
// also leave no worker running and keep the controller reusable.
TEST(ConfigurationFileWatchController, StartThatCannotStartItsWorkersFailsWithEveryStartedWorkerJoined)
{
	FakeFileService files;
	files.watchEnabled = true;
	FaultingWorkerStarter starter;
	CConfigurationFileWatchController controller(files, starter.Starter());
	starter.FailAfter(1);
	EXPECT_EQ(config::EConfigurationFileWatchStatus::StartFailed, controller.Start({
		.profileSettings = Resource(),
		.workspaceFolders = { TestWorkspaceFolder() },
	}, {}).status);
	EXPECT_EQ(0, starter.Running());
	EXPECT_EQ(config::EConfigurationFileWatchStatus::NotStarted, controller.Stop().status);

	starter.Heal();
	EXPECT_EQ(config::EConfigurationFileWatchStatus::Started,
		controller.Start({ .profileSettings = Resource() }, {}).status);
	EXPECT_EQ(1, starter.Running());
	EXPECT_EQ(config::EConfigurationFileWatchStatus::Stopped, controller.Stop().status);
	EXPECT_EQ(0, starter.Running());
}

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
#include <memory>
#include <mutex>
#include <string>
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
		{
			std::lock_guard lock(watchMutex);
			reject = rejectVscodeWatch;
		}
		if (reject && root.Path().ends_with(L"/.vscode")) {
			return FileResult<std::unique_ptr<IFileWatch>>::Failure(EFileResultStatus::NotFound);
		}
		auto watch = std::make_unique<FakeWatch>(root);
		{
			std::lock_guard lock(watchMutex);
			watchRoots.push_back(root);
			watches.push_back(watch.get());
		}
		return FileResult<std::unique_ptr<IFileWatch>>::Success(std::move(watch));
	}

	FileResult<FileBytes> nextRead = FileResult<FileBytes>::Failure(EFileResultStatus::NotFound);
	int readCalls = 0;
	std::size_t lastMaximumBytes = 0;
	bool watchEnabled = false;
	bool rejectVscodeWatch = false;
	std::mutex watchMutex;
	std::vector<Uri> watchRoots;
	std::vector<FakeWatch*> watches;
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

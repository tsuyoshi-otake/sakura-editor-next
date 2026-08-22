/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#include "pch.h"

#include "workbench/workspace/WorkspaceArtifactDocumentSourceController.h"

#include <sakura/serialization/JsoncDocument.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace {

using platform::filesystem::DirectoryEntry;
using platform::filesystem::EFileResultStatus;
using platform::filesystem::FileBytes;
using platform::filesystem::FileReadOptions;
using platform::filesystem::FileResult;
using platform::filesystem::FileStat;
using platform::filesystem::IFileService;
using platform::filesystem::IFileWatch;
using platform::uri::Uri;
using workbench::workspace::CWorkspaceArtifactDocumentService;
using workbench::workspace::CWorkspaceArtifactDocumentSourceController;
using workbench::workspace::EWorkspaceArtifactDocumentKind;
using workbench::workspace::EWorkspaceArtifactDocumentSourceStatus;
using workbench::workspace::EWorkspaceArtifactDocumentStatus;
using workbench::workspace::WorkspaceArtifactDocumentSourceRequest;
using workbench::workspace::WorkspaceArtifactDocumentSourceResult;

Uri Resource(const wchar_t* path)
{
	auto parsed = Uri::Parse(path);
	EXPECT_TRUE(parsed.value.has_value());
	return *parsed.value;
}

FileResult<FileBytes> Bytes(const char* text)
{
	return FileResult<FileBytes>::Success(FileBytes(text, text + std::char_traits<char>::length(text)));
}

class FakeFileService final : public IFileService {
public:
	class FakeWatch final : public IFileWatch {
	public:
		explicit FakeWatch(Uri rootValue, std::atomic<int>* cancelCountValue)
			: root(std::move(rootValue)), cancelCount(cancelCountValue) {}
		FileResult<void> Cancel() override
		{
			std::lock_guard lock(mutex);
			if (!cancelled) {
				cancelled = true;
				++*cancelCount;
				events.push_back({ .type = platform::filesystem::EFileWatchEventType::Disposed, .uri = root });
			}
			ready.notify_all();
			return FileResult<void>::Success();
		}
		FileResult<platform::filesystem::FileWatchEvent> Next() override
		{
			std::unique_lock lock(mutex);
			ready.wait(lock, [this] { return !events.empty(); });
			auto event = std::move(events.front());
			events.pop_front();
			return FileResult<platform::filesystem::FileWatchEvent>::Success(std::move(event));
		}
		void Push(platform::filesystem::FileWatchEvent event)
		{
			std::lock_guard lock(mutex);
			events.push_back(std::move(event));
			ready.notify_one();
		}

		Uri root;
	private:
		std::atomic<int>* cancelCount;
		std::mutex mutex;
		std::condition_variable ready;
		std::deque<platform::filesystem::FileWatchEvent> events;
		bool cancelled = false;
	};

	FileResult<FileStat> Stat(const Uri&) override { return FileResult<FileStat>::Failure(EFileResultStatus::Unsupported); }
	FileResult<std::vector<DirectoryEntry>> Enumerate(const Uri&) override { return FileResult<std::vector<DirectoryEntry>>::Failure(EFileResultStatus::Unsupported); }
	FileResult<FileBytes> Read(const Uri& resource, const FileReadOptions& options) override
	{
		std::unique_lock readLock(readMutex);
		lastMaximumBytes = options.maximumBytes;
		if (blockNextRead) {
			blockNextRead = false;
			readBlocked = true;
			readReady.notify_all();
			readReady.wait(readLock, [this] { return releaseBlockedRead; });
		}
		if (throwNextRead) {
			throwNextRead = false;
			throw std::runtime_error("scripted file provider failure");
		}
		const auto found = files.find(resource.Path());
		return found == files.end() ? FileResult<FileBytes>::Failure(EFileResultStatus::NotFound) : found->second;
	}
	FileResult<std::unique_ptr<IFileWatch>> Watch(const Uri& root, const platform::filesystem::FileWatchOptions&) override
	{
		if (!watchEnabled) return FileResult<std::unique_ptr<IFileWatch>>::Failure(EFileResultStatus::Unsupported);
		auto watch = std::make_unique<FakeWatch>(root, &watchCancelCount);
		{
			std::lock_guard lock(watchMutex);
			watches.push_back(watch.get());
		}
		return FileResult<std::unique_ptr<IFileWatch>>::Success(std::move(watch));
	}
	void BlockNextRead()
	{
		std::lock_guard lock(readMutex);
		blockNextRead = true;
		readBlocked = false;
		releaseBlockedRead = false;
	}
	bool WaitForBlockedRead()
	{
		std::unique_lock lock(readMutex);
		return readReady.wait_for(lock, std::chrono::seconds(2), [this] { return readBlocked; });
	}
	void ReleaseBlockedRead()
	{
		std::lock_guard lock(readMutex);
		releaseBlockedRead = true;
		readReady.notify_all();
	}
	void ThrowNextRead()
	{
		std::lock_guard lock(readMutex);
		throwNextRead = true;
	}

	std::map<std::wstring, FileResult<FileBytes>, std::less<>> files;
	std::size_t lastMaximumBytes = 0;
	bool watchEnabled = false;
	std::atomic<int> watchCancelCount = 0;
	std::mutex watchMutex;
	std::vector<FakeWatch*> watches;
	std::mutex readMutex;
	std::condition_variable readReady;
	bool blockNextRead = false;
	bool readBlocked = false;
	bool releaseBlockedRead = false;
	bool throwNextRead = false;
};

TEST(WorkspaceArtifactDocumentSourceController, ReadsWorkspaceMembersAndFolderArtifactsWithFolderPrecedence)
{
	auto filesOwner = std::make_shared<FakeFileService>();
	auto& files = *filesOwner;
	auto folder = Resource(L"file:///C:/Workspace");
	auto workspace = Resource(L"file:///C:/Workspace/project.code-workspace");
	files.files.emplace(workspace.Path(), Bytes(R"json({
  "tasks": { "version": "2.0.0", "tasks": [ { "label": "workspace" } ] },
  "launch": { "version": "0.2.0", "configurations": [ { "name": "workspace" } ] }
})json"));
	files.files.emplace(L"/C:/Workspace/.vscode/tasks.json", Bytes(R"json({ "version": "2.0.0", "tasks": [ { "label": "folder" } ] })json"));

	auto serviceOwner = std::make_shared<CWorkspaceArtifactDocumentService>();
	auto& service = *serviceOwner;
	CWorkspaceArtifactDocumentSourceController controller(filesOwner, serviceOwner);
	auto started = controller.Start({ .generation = 1, .workspaceFolders = { folder }, .workspaceConfiguration = workspace });
	ASSERT_EQ(EWorkspaceArtifactDocumentSourceStatus::Started, started.status);
	EXPECT_EQ(platform::serialization::CJsoncDocument::kMaximumInputBytes, files.lastMaximumBytes);
	ASSERT_TRUE(service.Tasks(folder).document.has_value());
	EXPECT_EQ(EWorkspaceArtifactDocumentKind::Tasks, service.Tasks(folder).document->kind);
	EXPECT_EQ(L"/C:/Workspace/.vscode/tasks.json", service.Tasks(folder).document->resource.Path());
	ASSERT_TRUE(service.Launch(folder).document.has_value());
	EXPECT_EQ(workspace.Path(), service.Launch(folder).document->resource.Path());
	EXPECT_EQ(EWorkspaceArtifactDocumentSourceStatus::Stopped, controller.Stop().status);
}

TEST(WorkspaceArtifactDocumentSourceController, CorruptOrInvalidBytesPreserveLastGoodDocument)
{
	auto filesOwner = std::make_shared<FakeFileService>();
	auto& files = *filesOwner;
	auto folder = Resource(L"file:///C:/Workspace");
	const std::wstring tasks = L"/C:/Workspace/.vscode/tasks.json";
	files.files.emplace(tasks, Bytes(R"json({ "version": "2.0.0", "tasks": [ { "label": "good" } ] })json"));
	auto serviceOwner = std::make_shared<CWorkspaceArtifactDocumentService>();
	auto& service = *serviceOwner;
	CWorkspaceArtifactDocumentSourceController controller(filesOwner, serviceOwner);
	ASSERT_EQ(EWorkspaceArtifactDocumentSourceStatus::Started, controller.Start({ .generation = 1, .workspaceFolders = { folder } }).status);
	ASSERT_TRUE(service.Tasks(folder).document.has_value());
	files.files[tasks] = Bytes(R"json({ "tasks": [], "tasks": [] })json");
	auto corrupt = controller.Reload();
	ASSERT_EQ(EWorkspaceArtifactDocumentSourceStatus::Reloaded, corrupt.status);
	ASSERT_EQ(2u, corrupt.documents.size());
	EXPECT_EQ(EWorkspaceArtifactDocumentStatus::DuplicateKey, corrupt.documents.front().status);
	ASSERT_TRUE(service.Tasks(folder).document.has_value());
	EXPECT_EQ(std::string::npos, service.Tasks(folder).document->rawJsonc.find("\"tasks\": [], \"tasks\": []"));
	files.files[tasks] = FileResult<FileBytes>::Success({ 0xc3U, 0x28U });
	auto invalidUtf8 = controller.Reload();
	ASSERT_EQ(EWorkspaceArtifactDocumentSourceStatus::Reloaded, invalidUtf8.status);
	ASSERT_EQ(2u, invalidUtf8.documents.size());
	EXPECT_EQ(EWorkspaceArtifactDocumentStatus::InvalidUtf8, invalidUtf8.documents.front().status);
	ASSERT_TRUE(service.Tasks(folder).document.has_value());
	files.files[tasks] = FileResult<FileBytes>::Failure(EFileResultStatus::PermissionDenied);
	auto unreadable = controller.Reload();
	EXPECT_EQ(EWorkspaceArtifactDocumentSourceStatus::ReadFailed, unreadable.status);
	ASSERT_TRUE(unreadable.fileStatus.has_value());
	EXPECT_EQ(EFileResultStatus::PermissionDenied, *unreadable.fileStatus);
	ASSERT_TRUE(service.Tasks(folder).document.has_value());
	EXPECT_EQ(EWorkspaceArtifactDocumentSourceStatus::Stopped, controller.Stop().status);
}

TEST(WorkspaceArtifactDocumentSourceController, ProviderExceptionIsReportedAndDoesNotStrandTheStopFence)
{
	auto filesOwner = std::make_shared<FakeFileService>();
	auto& files = *filesOwner;
	auto documentsOwner = std::make_shared<CWorkspaceArtifactDocumentService>();
	auto& documents = *documentsOwner;
	CWorkspaceArtifactDocumentSourceController controller(filesOwner, documentsOwner);
	const auto folder = Resource(L"file:///C:/workspace");

	ASSERT_EQ(EWorkspaceArtifactDocumentSourceStatus::Started,
		controller.Start({ .generation = 1, .workspaceFolders = { folder } }).status);
	files.ThrowNextRead();
	const auto reloaded = controller.Reload();
	EXPECT_EQ(EWorkspaceArtifactDocumentSourceStatus::ReadFailed, reloaded.status);
	ASSERT_TRUE(reloaded.fileStatus.has_value());
	EXPECT_EQ(EFileResultStatus::Failed, *reloaded.fileStatus);
	EXPECT_EQ(EWorkspaceArtifactDocumentSourceStatus::Stopped, controller.Stop().status);
}

TEST(WorkspaceArtifactDocumentSourceController, MissingFolderArtifactClearsOnlyFolderContributionAndRestoresWorkspaceFallback)
{
	auto filesOwner = std::make_shared<FakeFileService>();
	auto& files = *filesOwner;
	auto folder = Resource(L"file:///C:/Workspace");
	auto workspace = Resource(L"file:///C:/Workspace/project.code-workspace");
	files.files.emplace(workspace.Path(), Bytes(R"json({ "tasks": { "version": "2.0.0", "tasks": [ { "label": "workspace" } ] } })json"));
	const std::wstring tasks = L"/C:/Workspace/.vscode/tasks.json";
	files.files.emplace(tasks, Bytes(R"json({ "version": "2.0.0", "tasks": [ { "label": "folder" } ] })json"));
	auto serviceOwner = std::make_shared<CWorkspaceArtifactDocumentService>();
	auto& service = *serviceOwner;
	CWorkspaceArtifactDocumentSourceController controller(filesOwner, serviceOwner);
	ASSERT_EQ(EWorkspaceArtifactDocumentSourceStatus::Started, controller.Start({ .generation = 1, .workspaceFolders = { folder }, .workspaceConfiguration = workspace }).status);
	ASSERT_TRUE(service.Tasks(folder).document.has_value());
	EXPECT_EQ(tasks, service.Tasks(folder).document->resource.Path());
	files.files.erase(tasks);
	auto missing = controller.Reload();
	ASSERT_EQ(EWorkspaceArtifactDocumentSourceStatus::Reloaded, missing.status);
	ASSERT_TRUE(service.Tasks(folder).document.has_value());
	EXPECT_EQ(workspace.Path(), service.Tasks(folder).document->resource.Path());
	EXPECT_EQ(EWorkspaceArtifactDocumentSourceStatus::Stopped, controller.Stop().status);
}

TEST(WorkspaceArtifactDocumentSourceController, RejectsUnboundedOrNonAdvancingTopologyUpdates)
{
	auto filesOwner = std::make_shared<FakeFileService>();
	auto& files = *filesOwner;
	auto serviceOwner = std::make_shared<CWorkspaceArtifactDocumentService>();
	auto& service = *serviceOwner;
	CWorkspaceArtifactDocumentSourceController controller(filesOwner, serviceOwner);
	auto folder = Resource(L"file:///C:/Workspace");
	WorkspaceArtifactDocumentSourceRequest oversized { .generation = 1 };
	oversized.workspaceFolders.assign(65, folder);
	EXPECT_EQ(EWorkspaceArtifactDocumentSourceStatus::CapacityExceeded, controller.Start(std::move(oversized)).status);
	ASSERT_EQ(EWorkspaceArtifactDocumentSourceStatus::Started, controller.Start({ .generation = 3, .workspaceFolders = { folder } }).status);
	EXPECT_EQ(EWorkspaceArtifactDocumentSourceStatus::InvalidRequest, controller.Update({ .generation = 3, .workspaceFolders = { folder } }).status);
	EXPECT_EQ(EWorkspaceArtifactDocumentSourceStatus::Stopped, controller.Stop().status);
}

TEST(WorkspaceArtifactDocumentSourceController, AdmitsAWorkspaceBurstWithinTheFixedRetirementCapacity)
{
	auto filesOwner = std::make_shared<FakeFileService>();
	auto& files = *filesOwner;
	files.watchEnabled = true;
	auto serviceOwner = std::make_shared<CWorkspaceArtifactDocumentService>();
	auto& service = *serviceOwner;
	CWorkspaceArtifactDocumentSourceController controller(filesOwner, serviceOwner);
	WorkspaceArtifactDocumentSourceRequest request { .generation = 1 };
	request.workspaceFolders.reserve(64);
	for (std::size_t index = 0; index < 64; ++index) {
		const auto parsed = Uri::Parse(L"file:///C:/Workspace-" + std::to_wstring(index));
		ASSERT_TRUE(parsed.value.has_value());
		request.workspaceFolders.push_back(*parsed.value);
	}

	ASSERT_EQ(EWorkspaceArtifactDocumentSourceStatus::Started, controller.Start(std::move(request)).status);
	std::size_t admittedWatches = 0;
	{
		std::lock_guard lock(files.watchMutex);
		admittedWatches = files.watches.size();
	}
	// One slot is reserved for the dispatcher; every watch worker must have
	// been admitted before its thread was created, so this cannot exceed the
	// process-wide fixed retirement bound even for 64-folder input.
	EXPECT_LE(admittedWatches, workbench::WorkerRetirementService::kMaximumWorkers - 1);
	EXPECT_EQ(EWorkspaceArtifactDocumentSourceStatus::Stopped, controller.Stop().status);
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (!controller.IsRetirementFinalized() && std::chrono::steady_clock::now() < deadline) {
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	EXPECT_TRUE(controller.IsRetirementFinalized());
}

TEST(WorkspaceArtifactDocumentSourceController, RebuildsDeduplicatedWatchTopologyAndStopsWithoutFurtherCallback)
{
	auto filesOwner = std::make_shared<FakeFileService>();
	auto& files = *filesOwner;
	files.watchEnabled = true;
	auto folder = Resource(L"file:///C:/Workspace");
	auto serviceOwner = std::make_shared<CWorkspaceArtifactDocumentService>();
	auto& service = *serviceOwner;
	CWorkspaceArtifactDocumentSourceController controller(filesOwner, serviceOwner);
	std::mutex callbackMutex;
	std::condition_variable callbackReady;
	int callbacks = 0;
	ASSERT_EQ(EWorkspaceArtifactDocumentSourceStatus::Started, controller.Start({ .generation = 1, .workspaceFolders = { folder } },
		[&](const auto&) {
			std::lock_guard lock(callbackMutex);
			++callbacks;
			callbackReady.notify_all();
		}).status);
	{
		std::lock_guard lock(files.watchMutex);
		ASSERT_FALSE(files.watches.empty());
		files.watches.front()->Push({ .type = platform::filesystem::EFileWatchEventType::Created,
			.uri = Resource(L"file:///C:/Workspace/.vscode") });
	}
	{
		std::unique_lock lock(callbackMutex);
		ASSERT_TRUE(callbackReady.wait_for(lock, std::chrono::seconds(2), [&] { return callbacks == 1; }));
	}
	EXPECT_EQ(EWorkspaceArtifactDocumentSourceStatus::Stopped, controller.Stop().status);
	{
		std::lock_guard lock(callbackMutex);
		EXPECT_EQ(1, callbacks);
	}
}

TEST(WorkspaceArtifactDocumentSourceController, StopWaitsForAnInProgressStartBeforeItReturns)
{
	auto filesOwner = std::make_shared<FakeFileService>();
	auto& files = *filesOwner;
	files.watchEnabled = true;
	files.BlockNextRead();
	auto serviceOwner = std::make_shared<CWorkspaceArtifactDocumentService>();
	auto& service = *serviceOwner;
	auto controller = std::make_unique<CWorkspaceArtifactDocumentSourceController>(filesOwner, serviceOwner);
	std::mutex completionMutex;
	std::condition_variable completionReady;
	bool startFinished = false;
	bool stopEntered = false;
	bool stopFinished = false;
	workbench::workspace::WorkspaceArtifactDocumentSourceResult startResult;
	workbench::workspace::WorkspaceArtifactDocumentSourceResult stopResult;
	auto folder = Resource(L"file:///C:/Workspace");

	std::thread starter([&] {
		startResult = controller->Start({ .generation = 1, .workspaceFolders = { folder } });
		std::lock_guard lock(completionMutex);
		startFinished = true;
		completionReady.notify_all();
	});
	const bool readBlocked = files.WaitForBlockedRead();
	if (!readBlocked) {
		files.ReleaseBlockedRead();
		starter.join();
		FAIL() << "initial workspace artifact read did not enter the deterministic test gate";
		return;
	}
	std::thread stopper([&] {
		{
			std::lock_guard lock(completionMutex);
			stopEntered = true;
			completionReady.notify_all();
		}
		stopResult = controller->Stop();
		std::lock_guard lock(completionMutex);
		stopFinished = true;
		completionReady.notify_all();
	});
	bool stopWasObserved = false;
	{
		std::unique_lock lock(completionMutex);
		stopWasObserved = completionReady.wait_for(lock, std::chrono::seconds(2), [&] { return stopEntered; });
		// The old implementation completed Stop here, then Start resumed and
		// published a dispatcher after the terminal lifecycle had returned.
		if (stopWasObserved) {
			EXPECT_FALSE(completionReady.wait_for(lock, std::chrono::milliseconds(100), [&] { return stopFinished; }));
		}
	}
	files.ReleaseBlockedRead();
	bool bothFinished = false;
	{
		std::unique_lock lock(completionMutex);
		bothFinished = completionReady.wait_for(lock, std::chrono::seconds(2), [&] { return startFinished && stopFinished; });
	}
	starter.join();
	stopper.join();
	EXPECT_TRUE(stopWasObserved);
	EXPECT_TRUE(bothFinished);
	EXPECT_EQ(EWorkspaceArtifactDocumentSourceStatus::Started, startResult.status);
	EXPECT_EQ(EWorkspaceArtifactDocumentSourceStatus::Stopped, stopResult.status);
	EXPECT_EQ(2, files.watchCancelCount.load());
	EXPECT_EQ(EWorkspaceArtifactDocumentSourceStatus::NotStarted, controller->Reload().status);
	controller.reset();
}

TEST(WorkspaceArtifactDocumentSourceController, StopPreventsAReadThatCompletesLaterFromApplying)
{
	auto filesOwner = std::make_shared<FakeFileService>();
	auto& files = *filesOwner;
	auto folder = Resource(L"file:///C:/Workspace");
	const std::wstring tasks = L"/C:/Workspace/.vscode/tasks.json";
	files.files.emplace(tasks, Bytes(R"json({ "version": "2.0.0", "tasks": [ { "label": "before" } ] })json"));
	auto serviceOwner = std::make_shared<CWorkspaceArtifactDocumentService>();
	auto& service = *serviceOwner;
	CWorkspaceArtifactDocumentSourceController controller(filesOwner, serviceOwner);
	ASSERT_EQ(EWorkspaceArtifactDocumentSourceStatus::Started,
		controller.Start({ .generation = 1, .workspaceFolders = { folder } }).status);
	ASSERT_TRUE(service.Tasks(folder).document.has_value());
	files.files[tasks] = Bytes(R"json({ "version": "2.0.0", "tasks": [ { "label": "after" } ] })json");
	files.BlockNextRead();
	std::thread reloader([&] { (void)controller.Reload(); });
	const bool readBlocked = files.WaitForBlockedRead();
	if (!readBlocked) {
		files.ReleaseBlockedRead();
		(void)controller.Stop();
		reloader.join();
		FAIL() << "reload did not enter the deterministic test gate";
		return;
	}
	EXPECT_EQ(EWorkspaceArtifactDocumentSourceStatus::Stopped, controller.Stop().status);
	files.ReleaseBlockedRead();
	reloader.join();
	ASSERT_TRUE(service.Tasks(folder).document.has_value());
	EXPECT_NE(std::string::npos, service.Tasks(folder).document->rawJsonc.find("before"));
	EXPECT_EQ(std::string::npos, service.Tasks(folder).document->rawJsonc.find("after"));
}

TEST(WorkspaceArtifactDocumentSourceController, StopTransfersAStalledDispatcherWithoutWaitingForItsRead)
{
	auto filesOwner = std::make_shared<FakeFileService>();
	auto& files = *filesOwner;
	const std::weak_ptr<FakeFileService> filesLifetime = filesOwner;
	files.watchEnabled = true;
	const auto folder = Resource(L"file:///C:/Workspace");
	const auto tasks = Resource(L"file:///C:/Workspace/.vscode/tasks.json");
	files.files.emplace(tasks.Path(), Bytes(R"json({ "version": "2.0.0", "tasks": [] })json"));
	auto serviceOwner = std::make_shared<CWorkspaceArtifactDocumentService>();
	const std::weak_ptr<CWorkspaceArtifactDocumentService> serviceLifetime = serviceOwner;
	auto controller = std::make_unique<CWorkspaceArtifactDocumentSourceController>(filesOwner, serviceOwner);
	ASSERT_EQ(EWorkspaceArtifactDocumentSourceStatus::Started,
		controller->Start({ .generation = 1, .workspaceFolders = { folder } }).status);

	FakeFileService::FakeWatch* artifactWatch = nullptr;
	{
		std::lock_guard lock(files.watchMutex);
		for (auto* watch : files.watches) {
			if (watch->root.Path() == L"/C:/Workspace/.vscode") {
				artifactWatch = watch;
				break;
			}
		}
	}
	ASSERT_NE(nullptr, artifactWatch);

	files.BlockNextRead();
	artifactWatch->Push({ .type = platform::filesystem::EFileWatchEventType::Changed, .uri = tasks });
	ASSERT_TRUE(files.WaitForBlockedRead());

	std::mutex stopMutex;
	std::condition_variable stopReady;
	bool stopFinished = false;
	WorkspaceArtifactDocumentSourceResult stopResult;
	std::thread stopper([&] {
		const auto result = controller->Stop();
		{
			std::lock_guard lock(stopMutex);
			stopResult = result;
			stopFinished = true;
		}
		stopReady.notify_all();
	});
	bool completedBeforeRelease = false;
	{
		std::unique_lock lock(stopMutex);
		completedBeforeRelease = stopReady.wait_for(lock, std::chrono::milliseconds(250), [&] { return stopFinished; });
	}
	EXPECT_TRUE(completedBeforeRelease);
	if (!completedBeforeRelease) files.ReleaseBlockedRead();
	stopper.join();
	filesOwner.reset();
	serviceOwner.reset();
	auto retainedFiles = filesLifetime.lock();
	EXPECT_NE(nullptr, retainedFiles);
	if (retainedFiles) retainedFiles->ReleaseBlockedRead();

	EXPECT_EQ(EWorkspaceArtifactDocumentSourceStatus::Stopped, stopResult.status);
	EXPECT_TRUE(stopResult.retirementPending || stopResult.retirementFinalized);
	if (!stopResult.retirementFinalized) {
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
		while (!controller->IsRetirementFinalized() && std::chrono::steady_clock::now() < deadline) {
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
	}
	EXPECT_TRUE(controller->IsRetirementFinalized());
	retainedFiles.reset();
	controller.reset();
	EXPECT_TRUE(filesLifetime.expired());
	EXPECT_TRUE(serviceLifetime.expired());
}

TEST(WorkspaceArtifactDocumentSourceController, CallbackSelfStopRemainsExplicitlyDenied)
{
	auto filesOwner = std::make_shared<FakeFileService>();
	auto& files = *filesOwner;
	files.watchEnabled = true;
	auto folder = Resource(L"file:///C:/Workspace");
	auto serviceOwner = std::make_shared<CWorkspaceArtifactDocumentService>();
	auto& service = *serviceOwner;
	CWorkspaceArtifactDocumentSourceController controller(filesOwner, serviceOwner);
	std::mutex callbackMutex;
	std::condition_variable callbackReady;
	bool callbackFinished = false;
	EWorkspaceArtifactDocumentSourceStatus callbackStopStatus = EWorkspaceArtifactDocumentSourceStatus::ReadFailed;
	ASSERT_EQ(EWorkspaceArtifactDocumentSourceStatus::Started,
		controller.Start({ .generation = 1, .workspaceFolders = { folder } }, [&](const auto&) {
			callbackStopStatus = controller.Stop().status;
			std::lock_guard lock(callbackMutex);
			callbackFinished = true;
			callbackReady.notify_all();
		}).status);
	{
		std::lock_guard lock(files.watchMutex);
		ASSERT_FALSE(files.watches.empty());
		files.watches.front()->Push({ .type = platform::filesystem::EFileWatchEventType::Created,
			.uri = Resource(L"file:///C:/Workspace/.vscode") });
	}
	{
		std::unique_lock lock(callbackMutex);
		ASSERT_TRUE(callbackReady.wait_for(lock, std::chrono::seconds(2), [&] { return callbackFinished; }));
	}
	EXPECT_EQ(EWorkspaceArtifactDocumentSourceStatus::ReentrantStopDenied, callbackStopStatus);
	EXPECT_EQ(EWorkspaceArtifactDocumentSourceStatus::Stopped, controller.Stop().status);
}

} // namespace

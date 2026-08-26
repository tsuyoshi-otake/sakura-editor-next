/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#include "pch.h"

#include "config/BuiltinConfigurationDescriptors.h"
#include <sakura/filesystem/IFileService.h>
#include "platform/profiles/ProfileBootstrapSnapshot.h"
#include "platform/profiles/UserDataProfileBootstrap.h"
#include <sakura/uri/UriIdentity.h>
#include "workbench/CWorkbenchRuntime.h"
#include "workbench/layout/IWorkbenchLayoutMementoStore.h"
#include "workbench/layout/WorkbenchIds.h"
#include "workbench/output/OutputService.h"
#include "workbench/output/OutputServiceRustProvider.h"
#include "workbench/problems/MarkerService.h"
#include "workbench/statusbar/IStatusbarVisibilityMementoStore.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <future>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using config::ConfigurationTarget;
using config::ConfigurationValue;
using config::EConfigurationOutcome;
using config::EConfigurationScope;
using config::EWorkspaceContextOutcome;
using config::EWorkspaceKind;
using platform::filesystem::DirectoryEntry;
using platform::filesystem::EFileResultStatus;
using platform::filesystem::FileBytes;
using platform::filesystem::FileConditionalReplaceOptions;
using platform::filesystem::FileConditionalReplaceResult;
using platform::filesystem::FileContentSnapshot;
using platform::filesystem::FileReadOptions;
using platform::filesystem::FileResult;
using platform::filesystem::FileStat;
using platform::filesystem::FileVersionToken;
using platform::filesystem::IFileService;
using platform::filesystem::IFileWatch;
using platform::filesystem::FileWatchEvent;
using platform::filesystem::EFileWatchEventType;
using platform::profiles::ProfileBootstrapSnapshot;
using platform::profiles::ResolveProfileBootstrapSnapshot;
using platform::profiles::ResolveUserDataProfileBootstrap;
using platform::profiles::UserDataProfileBootstrapRequest;
using platform::profiles::UserDataProfileBootstrapSnapshot;
using platform::profiles::UserDataProfileRegistry;
using platform::profiles::UserDataProfileResourceRootMode;
using platform::uri::Uri;
using platform::uri::UriIdentityService;
using workbench::CWorkbenchRuntime;
using workbench::EWorkbenchRuntimeDiagnosticCode;
using workbench::EWorkbenchRuntimeDiagnosticSource;
using workbench::EWorkbenchRuntimeResultCode;
using workbench::EWorkbenchRuntimeState;
using workbench::ResolveWorkbenchBootstrapContext;
using workbench::WorkbenchBootstrapContext;
using workbench::WorkbenchBootstrapRequest;

static_assert(std::is_same_v<decltype(std::declval<workbench::IWorkbenchRuntime&>().Output()),
	workbench::output::IOutputService*>);
static_assert(std::is_same_v<decltype(std::declval<const workbench::IWorkbenchRuntime&>().Output()),
	const workbench::output::IOutputService*>);
static_assert(std::is_same_v<decltype(std::declval<CWorkbenchRuntime&>().Output()),
	workbench::output::IOutputService*>);
static_assert(std::is_same_v<decltype(std::declval<const CWorkbenchRuntime&>().Output()),
	const workbench::output::IOutputService*>);
static_assert(std::is_same_v<decltype(std::declval<const workbench::IWorkbenchRuntime&>().OutputProviderHealth()),
	workbench::output::OutputProviderHealthSnapshot>);
static_assert(std::is_same_v<decltype(std::declval<const CWorkbenchRuntime&>().OutputProviderHealth()),
	workbench::output::OutputProviderHealthSnapshot>);

using workbench::WorkbenchRuntimeDependencies;
namespace layout = workbench::layout;
namespace outputModel = workbench::output;
namespace problems = workbench::problems;
namespace statusbar = workbench::statusbar;
namespace tasks = workbench::tasks;

constexpr char kProfileId[] = "0123456789abcdef0123456789abcdef";

Uri Parse(const wchar_t* text)
{
	auto parsed = Uri::Parse(text);
	EXPECT_TRUE(parsed);
	return std::move(*parsed.value);
}

ProfileBootstrapSnapshot Profile()
{
	auto resolved = ResolveProfileBootstrapSnapshot(kProfileId, 7, L"C:\\Profiles\\Sakura");
	EXPECT_TRUE(resolved.Resolved());
	return std::move(*resolved.snapshot);
}

UserDataProfileBootstrapSnapshot UserDataProfile()
{
	UserDataProfileRegistry registry;
	UserDataProfileBootstrapRequest request {
		{ kProfileId, 7 }, L"C:\\Profiles\\Sakura", {},
		UserDataProfileResourceRootMode::LegacyControlRootForDefault,
	};
	auto resolved = ResolveUserDataProfileBootstrap(request, registry);
	EXPECT_TRUE(resolved.Resolved());
	return std::move(*resolved.snapshot);
}

WorkbenchBootstrapContext Bootstrap(
	std::optional<Uri> folder = std::nullopt,
	std::optional<Uri> initialDocument = std::nullopt)
{
	WorkbenchBootstrapRequest request {
		Profile(), UserDataProfile(), L"window-runtime-test", std::move(folder), std::nullopt, {},
		std::move(initialDocument), std::nullopt,
	};
	auto resolved = ResolveWorkbenchBootstrapContext(std::move(request));
	EXPECT_TRUE(resolved.Resolved());
	return std::move(*resolved.context);
}

WorkbenchBootstrapContext WorkspaceBootstrap(const Uri& workspaceConfig, std::vector<config::WorkspaceFolderDescriptor> folders)
{
	WorkbenchBootstrapRequest request {
		Profile(), UserDataProfile(), L"window-runtime-test", std::nullopt, workspaceConfig,
		std::move(folders), std::nullopt, std::nullopt,
	};
	auto resolved = ResolveWorkbenchBootstrapContext(std::move(request));
	EXPECT_TRUE(resolved.Resolved());
	return std::move(*resolved.context);
}

FileResult<FileBytes> Bytes(std::string value)
{
	return FileResult<FileBytes>::Success(FileBytes(value.begin(), value.end()));
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
			return FileResult<FileWatchEvent>::Success(std::move(event));
		}
		void Push(FileWatchEvent event)
		{
			std::lock_guard lock(mutex);
			events.push_back(std::move(event));
			ready.notify_one();
		}
		Uri root;
	private:
		std::mutex mutex;
		std::condition_variable ready;
		std::deque<FileWatchEvent> events;
		bool cancelled = false;
	};

	void Set(const Uri& resource, FileResult<FileBytes> result)
	{
		std::lock_guard lock(m_mutex);
		const auto identity = UriIdentityService::MakeComparisonKey(resource);
		if (result.Succeeded() && result.value) m_versions.insert_or_assign(identity, NextVersion());
		else m_versions.erase(identity);
		m_results.insert_or_assign(identity, std::move(result));
	}

	std::vector<std::wstring> Reads() const
	{
		std::lock_guard lock(m_mutex);
		return m_reads;
	}

	void BlockRead(const Uri& resource)
	{
		std::lock_guard lock(m_mutex);
		m_blockedResource = UriIdentityService::MakeComparisonKey(resource);
		m_readBlocked = false;
		m_releaseRead = false;
	}

	void WaitUntilReadIsBlocked()
	{
		std::unique_lock lock(m_mutex);
		m_readBlockedCondition.wait(lock, [this] { return m_readBlocked; });
	}

	void ReleaseBlockedRead()
	{
		{
			std::lock_guard lock(m_mutex);
			m_releaseRead = true;
		}
		m_releaseReadCondition.notify_all();
	}

	void EnableWatches() { std::lock_guard lock(m_mutex); m_watchEnabled = true; }
	void ConflictNextConditionalReplace() { std::lock_guard lock(m_mutex); m_conflictNextReplace = true; }
	void EmitFirstWatchEvent(FileWatchEvent event)
	{
		std::vector<FakeWatch*> watches;
		{
			std::lock_guard lock(m_mutex);
			watches = m_watches;
		}
		ASSERT_FALSE(watches.empty());
		// Runtime composition owns independent settings and artifact watch
		// topologies. Deliver a synthetic filesystem event to every active fake
		// watch so a test never depends on registration order.
		for (auto* watch : watches) {
			ASSERT_NE(nullptr, watch);
			watch->Push(event);
		}
	}
	bool WaitUntilReadCount(std::size_t count)
	{
		std::unique_lock lock(m_mutex);
		return m_readCountCondition.wait_for(lock, std::chrono::seconds(2), [this, count] { return m_reads.size() >= count; });
	}

	std::function<void()> onRead;

	FileResult<FileStat> Stat(const Uri&) override
	{
		return FileResult<FileStat>::Failure(EFileResultStatus::Unsupported);
	}

	FileResult<std::vector<DirectoryEntry>> Enumerate(const Uri&) override
	{
		return FileResult<std::vector<DirectoryEntry>>::Failure(EFileResultStatus::Unsupported);
	}

	FileResult<FileBytes> Read(const Uri& resource, const FileReadOptions&) override
	{
		std::function<void()> callback;
		FileResult<FileBytes> result;
		{
			std::unique_lock lock(m_mutex);
			const auto identity = UriIdentityService::MakeComparisonKey(resource);
			m_reads.push_back(resource.ToString());
			callback = onRead;
			if (m_blockedResource && *m_blockedResource == identity) {
				m_readBlocked = true;
				m_readBlockedCondition.notify_all();
				m_releaseReadCondition.wait(lock, [this] { return m_releaseRead; });
			}
			const auto found = m_results.find(identity);
			result = found == m_results.end()
				? FileResult<FileBytes>::Failure(EFileResultStatus::NotFound)
				: found->second;
		}
		m_readCountCondition.notify_all();
		if (callback) callback();
		return result;
	}

	FileResult<FileContentSnapshot> ReadVersioned(const Uri& resource, const FileReadOptions&) override
	{
		std::function<void()> callback;
		FileResult<FileContentSnapshot> result;
		{
			std::lock_guard lock(m_mutex);
			const auto identity = UriIdentityService::MakeComparisonKey(resource);
			m_reads.push_back(resource.ToString());
			callback = onRead;
			const auto found = m_results.find(identity);
			const auto version = m_versions.find(identity);
			if (found == m_results.end()) {
				result = FileResult<FileContentSnapshot>::Failure(EFileResultStatus::NotFound);
			} else if (!found->second.Succeeded() || !found->second.value) {
				result = FileResult<FileContentSnapshot>::Failure(found->second.status, found->second.diagnostic);
			} else if (version == m_versions.end()) {
				result = FileResult<FileContentSnapshot>::Failure(EFileResultStatus::Failed);
			} else {
				result = FileResult<FileContentSnapshot>::Success({ *found->second.value, version->second });
			}
		}
		m_readCountCondition.notify_all();
		if (callback) callback();
		return result;
	}

	FileConditionalReplaceResult ConditionalAtomicReplace(
		const Uri& resource, const FileBytes& bytes, const FileConditionalReplaceOptions& options) override
	{
		std::lock_guard lock(m_mutex);
		if (m_conflictNextReplace) {
			m_conflictNextReplace = false;
			return FileConditionalReplaceResult::Conflict();
		}
		const auto identity = UriIdentityService::MakeComparisonKey(resource);
		const auto found = m_results.find(identity);
		const bool exists = found != m_results.end() && found->second.Succeeded() && found->second.value;
		if (options.expectation == platform::filesystem::EFileConditionalReplaceExpectation::Missing) {
			if (exists) return FileConditionalReplaceResult::Conflict();
		} else {
			const auto version = m_versions.find(identity);
			if (!exists || version == m_versions.end() || version->second != options.expectedVersion) {
				return FileConditionalReplaceResult::Conflict();
			}
		}
		auto committed = NextVersion();
		m_results.insert_or_assign(identity, FileResult<FileBytes>::Success(bytes));
		m_versions.insert_or_assign(identity, committed);
		return FileConditionalReplaceResult::Success(std::move(committed));
	}

	FileResult<std::unique_ptr<IFileWatch>> Watch(
		const Uri& root,
		const platform::filesystem::FileWatchOptions&) override
	{
		std::lock_guard lock(m_mutex);
		if (!m_watchEnabled) return FileResult<std::unique_ptr<IFileWatch>>::Failure(EFileResultStatus::Unsupported);
		auto watch = std::make_unique<FakeWatch>(root);
		m_watches.push_back(watch.get());
		return FileResult<std::unique_ptr<IFileWatch>>::Success(std::move(watch));
	}

private:
	FileVersionToken NextVersion()
	{
		const std::uint64_t value = m_nextVersion++;
		const auto bytes = std::span<const std::uint8_t>(
			reinterpret_cast<const std::uint8_t*>(&value), sizeof(value));
		auto token = FileVersionToken::FromOpaqueBytes(bytes);
		EXPECT_TRUE(token.has_value());
		return std::move(*token);
	}
	mutable std::mutex m_mutex;
	std::map<std::wstring, FileResult<FileBytes>, std::less<>> m_results;
	std::map<std::wstring, FileVersionToken, std::less<>> m_versions;
	std::uint64_t m_nextVersion = 1;
	bool m_conflictNextReplace = false;
	std::vector<std::wstring> m_reads;
	std::optional<std::wstring> m_blockedResource;
	bool m_readBlocked = false;
	bool m_releaseRead = false;
	std::condition_variable m_readBlockedCondition;
	std::condition_variable m_releaseReadCondition;
	std::condition_variable m_readCountCondition;
	bool m_watchEnabled = false;
	std::vector<FakeWatch*> m_watches;
};

class FakeLayoutMementoStore final : public layout::IWorkbenchLayoutMementoStore {
public:
	layout::WorkbenchLayoutMementoLoadResult Load() override
	{
		++loadCalls;
		return loadResult;
	}

	layout::WorkbenchLayoutMementoSaveResult Save(
		const layout::WorkbenchLayoutStateSnapshot& snapshot) override
	{
		++saveCalls;
		lastSavedSnapshot = snapshot;
		return saveResult;
	}

	layout::WorkbenchLayoutMementoLoadResult loadResult{
		layout::EWorkbenchLayoutMementoLoadStatus::NotFound, std::nullopt, {}
	};
	layout::WorkbenchLayoutMementoSaveResult saveResult{
		layout::EWorkbenchLayoutMementoSaveStatus::Persisted, {}
	};
	std::size_t loadCalls = 0;
	std::size_t saveCalls = 0;
	std::optional<layout::WorkbenchLayoutStateSnapshot> lastSavedSnapshot;
};

class FakeStatusbarVisibilityMementoStore final : public statusbar::IStatusbarVisibilityMementoStore {
public:
	statusbar::StatusbarMementoLoadResult Load() override
	{
		++loadCalls;
		return loadResult;
	}

	statusbar::StatusbarMementoSaveResult Save(const std::vector<std::string>& hiddenIds) override
	{
		++saveCalls;
		lastSavedHiddenIds = hiddenIds;
		return saveResult;
	}

	statusbar::StatusbarMementoLoadResult loadResult{
		statusbar::EStatusbarMementoLoadStatus::NotFound, std::vector<std::string>{}, {}
	};
	statusbar::StatusbarMementoSaveResult saveResult{
		statusbar::EStatusbarMementoSaveStatus::Persisted, {}
	};
	std::size_t loadCalls = 0;
	std::size_t saveCalls = 0;
	std::vector<std::string> lastSavedHiddenIds;
};

struct RuntimeTaskSessionState final {
	explicit RuntimeTaskSessionState(tasks::TaskExecutionSessionCallbacks value)
		: callbacks(std::move(value))
	{
	}

	tasks::TaskExecutionSessionCallbacks callbacks;
	std::atomic_uint beginCloseCalls{};
	std::atomic_uint waitForCloseCalls{};
};

class RuntimeTaskSession final : public tasks::ITaskExecutionSession {
public:
	explicit RuntimeTaskSession(std::shared_ptr<RuntimeTaskSessionState> state)
		: m_state(std::move(state))
	{
	}

	tasks::TaskExecutionSessionStartResult Start(const tasks::TaskTerminalLaunchRequest&) override
	{
		return tasks::TaskExecutionSessionStartResult::Success();
	}

	void RequestCancel() noexcept override {}

	void BeginClose() noexcept override
	{
		++m_state->beginCloseCalls;
	}

	tasks::TaskExecutionSessionCloseResult WaitForClose(
		std::chrono::steady_clock::time_point) noexcept override
	{
		++m_state->waitForCloseCalls;
		return tasks::TaskExecutionSessionCloseResult::Closed();
	}

private:
	std::shared_ptr<RuntimeTaskSessionState> m_state;
};

class RuntimeTaskSessionFactory final : public tasks::ITaskExecutionSessionFactory {
public:
	std::unique_ptr<tasks::ITaskExecutionSession> Create(
		const tasks::TaskExecutionSessionCallbacks& callbacks) override
	{
		lastState = std::make_shared<RuntimeTaskSessionState>(callbacks);
		return std::make_unique<RuntimeTaskSession>(lastState);
	}

	std::shared_ptr<RuntimeTaskSessionState> lastState;
};

//! Test-only provider wrapper with a distinct dynamic type from OutputService.
//! It can optionally report a callback-drain deferral after entering the
//! stopped state. The distinct type lets Rust creator tests honor the factory
//! contract without returning the C++ authority object itself.
class DelegatingOutputProvider final : public outputModel::IOutputService {
public:
	explicit DelegatingOutputProvider(
		outputModel::OutputServiceLimits limits,
		bool deferFirstStop = false,
		outputModel::EOutputProviderKind reportedKind = outputModel::EOutputProviderKind::Rust)
		: m_delegate(std::move(limits))
		, m_deferFirstStop(deferFirstStop)
		, m_reportedKind(reportedKind)
	{
	}

	outputModel::OutputOperationResult CreateChannel(
		const outputModel::OutputCreateChannelRequest& request) override
	{
		return m_delegate.CreateChannel(request);
	}
	outputModel::OutputOperationResult AppendOutput(
		const outputModel::OutputTextMutationRequest& request) override
	{
		return m_delegate.AppendOutput(request);
	}
	outputModel::OutputOperationResult ReplaceOutput(
		const outputModel::OutputTextMutationRequest& request) override
	{
		return m_delegate.ReplaceOutput(request);
	}
	outputModel::OutputOperationResult AppendLog(
		const outputModel::OutputLogMutationRequest& request) override
	{
		return m_delegate.AppendLog(request);
	}
	outputModel::OutputOperationResult Clear(
		const outputModel::OutputChannelMutationRequest& request) override
	{
		return m_delegate.Clear(request);
	}
	outputModel::OutputOperationResult Show(
		const outputModel::OutputShowChannelRequest& request) override
	{
		return m_delegate.Show(request);
	}
	outputModel::OutputOperationResult Hide(
		const outputModel::OutputChannelMutationRequest& request) override
	{
		return m_delegate.Hide(request);
	}
	outputModel::OutputOperationResult Dispose(
		const outputModel::OutputChannelMutationRequest& request) override
	{
		return m_delegate.Dispose(request);
	}
	outputModel::OutputOperationResult DisposeOwner(
		const outputModel::OutputDisposeOwnerRequest& request) override
	{
		return m_delegate.DisposeOwner(request);
	}

	outputModel::OutputOperationResult Stop() noexcept override
	{
		++m_stopCalls;
		auto result = m_delegate.Stop();
		if (m_deferFirstStop && m_stopCalls == 1) result.callbackDrainDeferred = true;
		return result;
	}

	outputModel::OutputProviderHealthSnapshot Health() const noexcept override
	{
		auto health = m_delegate.Health();
		// This wrapper is a Rust test seam backed by an independent model. It
		// must identify the selected test authority without exposing the C++
		// concrete type to the factory.
		health.kind = m_reportedKind;
		return health;
	}

	outputModel::OutputServiceSnapshot Snapshot() const override
	{
		return m_delegate.Snapshot();
	}

	std::optional<outputModel::OutputServiceSubscriptionId> Subscribe(
		outputModel::OutputServiceListener listener) override
	{
		return m_delegate.Subscribe(std::move(listener));
	}

	void Unsubscribe(outputModel::OutputServiceSubscriptionId subscriptionId) noexcept override
	{
		m_delegate.Unsubscribe(subscriptionId);
	}

	[[nodiscard]] std::size_t StopCalls() const noexcept { return m_stopCalls; }

private:
	outputModel::OutputService m_delegate;
	bool m_deferFirstStop = false;
	outputModel::EOutputProviderKind m_reportedKind = outputModel::EOutputProviderKind::Rust;
	std::size_t m_stopCalls{};
};

struct RuntimeFixture final {
	explicit RuntimeFixture(
		WorkbenchBootstrapContext bootstrap,
		std::unique_ptr<FakeLayoutMementoStore> ownedLayoutStore = {},
		std::shared_ptr<workbench::tasks::ITaskExecutionSessionFactory> taskFactory = {},
		std::unique_ptr<FakeStatusbarVisibilityMementoStore> ownedStatusbarStore = {},
		std::optional<outputModel::EOutputProviderKind> outputProviderKind = std::nullopt,
		outputModel::OutputProviderCreator rustOutputProviderCreator = {},
		outputModel::OutputProviderCreator cppOutputProviderCreator = {})
	{
		auto ownedFiles = std::make_unique<FakeFileService>();
		files = ownedFiles.get();
		layoutStore = ownedLayoutStore.get();
		statusbarStore = ownedStatusbarStore.get();
		WorkbenchRuntimeDependencies dependencies;
		dependencies.fileService = std::move(ownedFiles);
		dependencies.layoutMementoStore = std::move(ownedLayoutStore);
		dependencies.taskExecutionSessionFactory = std::move(taskFactory);
		dependencies.statusbarVisibilityMementoStore = std::move(ownedStatusbarStore);
		if (outputProviderKind) dependencies.outputProviderKind = *outputProviderKind;
		dependencies.outputProviderFactory.testRustCreator = std::move(rustOutputProviderCreator);
		dependencies.outputProviderFactory.testCppCreator = std::move(cppOutputProviderCreator);
		runtime = std::make_unique<CWorkbenchRuntime>(
			std::move(bootstrap), config::BuiltinConfigurationDescriptors(), std::move(dependencies));
	}

	FakeFileService* files = nullptr;
	FakeLayoutMementoStore* layoutStore = nullptr;
	FakeStatusbarVisibilityMementoStore* statusbarStore = nullptr;
	std::unique_ptr<CWorkbenchRuntime> runtime;
};

layout::WorkbenchLayoutStateSnapshot VisiblePanelMemento()
{
	layout::WorkbenchContributionRegistry registry;
	layout::WorkbenchLayoutStateService state(registry.Snapshot());
	const auto shown = state.SetPartVisibility({
		.operation = { .operationId = "test.persisted.show-panel" },
		.partId = std::string(layout::ids::part::Panel),
		.visible = true,
	});
	EXPECT_EQ(layout::EWorkbenchLayoutOperationStatus::Succeeded, shown.status);
	return state.MementoSnapshot();
}

const layout::WorkbenchPartState& Part(
	const layout::WorkbenchLayoutStateSnapshot& snapshot, std::string_view id)
{
	const auto found = std::find_if(snapshot.parts.begin(), snapshot.parts.end(),
		[id](const auto& value) { return value.partId == id; });
	EXPECT_NE(snapshot.parts.end(), found);
	return *found;
}

ConfigurationTarget ProfileTarget(const WorkbenchBootstrapContext& bootstrap)
{
	ConfigurationTarget target;
	target.profileId = bootstrap.UserDataProfile().SelectedProfileId();
	return target;
}

std::wstring ShowTabs(CWorkbenchRuntime& runtime, const ConfigurationTarget& target)
{
	auto value = runtime.Configuration().GetValue("workbench.editor.showTabs", target);
	EXPECT_EQ(EConfigurationOutcome::Applied, value.outcome);
	EXPECT_TRUE(value.value.has_value());
	return std::get<std::wstring>(value.value->Value());
}

bool ContainsRead(const std::vector<std::wstring>& reads, std::wstring_view resource)
{
	return std::find(reads.begin(), reads.end(), resource) != reads.end();
}

} // namespace

TEST(CWorkbenchRuntime, OwnsMarkerAndOutputServicesOnlyWhileRunning)
{
	RuntimeFixture fixture(Bootstrap());
	const workbench::IWorkbenchRuntime& boundary = *fixture.runtime;
	EXPECT_EQ(nullptr, fixture.runtime->Markers());
	EXPECT_EQ(nullptr, fixture.runtime->Output());
	EXPECT_EQ(nullptr, boundary.Markers());
	EXPECT_EQ(nullptr, boundary.Output());

	ASSERT_TRUE(fixture.runtime->Start().IsUsable());
	auto* markers = fixture.runtime->Markers();
	workbench::output::IOutputService* output = fixture.runtime->Output();
	ASSERT_NE(nullptr, markers);
	ASSERT_NE(nullptr, output);
	EXPECT_EQ(markers, boundary.Markers());
	EXPECT_EQ(output, boundary.Output());

	std::size_t markerCallbacks = 0;
	const auto markerSubscription = markers->Subscribe([&markerCallbacks](const problems::MarkerChange&) {
		++markerCallbacks;
	});
	ASSERT_EQ(problems::EMarkerSubscriptionStatus::Subscribed, markerSubscription.status);
	const problems::MarkerOwner markerOwner{ .id = "runtime.marker-owner", .generation = 1 };
	const problems::MarkerCollectionIdentity collection{ .owner = markerOwner, .id = "runtime.collection" };
	const auto resource = Parse(L"file:///C:/Runtime/marker.cpp");
	EXPECT_EQ(problems::EMarkerOperationStatus::Replaced, markers->Replace({
		.collection = collection,
		.resource = resource,
		.markers = { { .range = { .endLine = 1 }, .message = "runtime marker" } },
	}).status);
	EXPECT_EQ(1U, markers->Snapshot().resources.size());
	EXPECT_EQ(1U, markerCallbacks);

	std::size_t outputCallbacks = 0;
	const auto outputSubscription = output->Subscribe([&outputCallbacks](const outputModel::OutputServiceChange&) {
		++outputCallbacks;
	});
	ASSERT_TRUE(outputSubscription.has_value());
	const outputModel::OutputOwner outputOwner{ .ownerId = "runtime.output-owner", .generation = 1 };
	EXPECT_EQ(outputModel::EOutputOperationStatus::Succeeded, output->CreateChannel({
		.operation = { .operationId = "runtime.output.create" },
		.owner = outputOwner,
		.channelId = "runtime.output",
		.label = "Runtime Output",
	}).status);
	EXPECT_EQ(outputModel::EOutputOperationStatus::Succeeded, output->AppendOutput({
		.operation = { .operationId = "runtime.output.append" },
		.owner = outputOwner,
		.channelId = "runtime.output",
		.text = "started",
	}).status);
	EXPECT_EQ(1U, output->Snapshot().channels.size());
	EXPECT_EQ(2U, outputCallbacks);

	EXPECT_EQ(EWorkbenchRuntimeResultCode::Stopped, fixture.runtime->Stop().code);
	EXPECT_EQ(nullptr, fixture.runtime->Markers());
	EXPECT_EQ(nullptr, fixture.runtime->Output());
	EXPECT_TRUE(markers->Snapshot().stopped);
	EXPECT_TRUE(markers->Snapshot().resources.empty());
	EXPECT_TRUE(output->Snapshot().stopped);
	EXPECT_TRUE(output->Snapshot().channels.empty());
	EXPECT_EQ(problems::EMarkerOperationStatus::Stopped, markers->Replace({
		.collection = collection,
		.resource = resource,
		.markers = { { .range = { .endLine = 1 }, .message = "after stop" } },
	}).status);
	EXPECT_EQ(outputModel::EOutputOperationStatus::Stopped, output->AppendOutput({
		.operation = { .operationId = "runtime.output.after-stop" },
		.owner = outputOwner,
		.channelId = "runtime.output",
		.text = "after stop",
	}).status);
	EXPECT_EQ(1U, markerCallbacks);
	EXPECT_EQ(2U, outputCallbacks);
	EXPECT_EQ(EWorkbenchRuntimeResultCode::Stopped, fixture.runtime->Stop().code);
}

TEST(CWorkbenchRuntime, CapturesTheCppOutputSelectionBeforeReadyAndBuildsItOnce)
{
	std::size_t buildCount = 0;
	RuntimeFixture fixture(Bootstrap(), {}, {}, {}, outputModel::EOutputProviderKind::Cpp,
		{}, [&buildCount](const outputModel::OutputServiceLimits& limits) {
			++buildCount;
			return std::make_unique<outputModel::OutputService>(limits);
		});

	EXPECT_EQ(outputModel::EOutputProviderKind::Cpp, fixture.runtime->OutputProviderKind());
	EXPECT_EQ(1U, buildCount);
	EXPECT_EQ(nullptr, fixture.runtime->Output());
	const auto selectedHealth = fixture.runtime->OutputProviderHealth();
	EXPECT_EQ(outputModel::EOutputProviderKind::Cpp, selectedHealth.kind);
	EXPECT_EQ(outputModel::EOutputProviderFactoryStatus::Created, selectedHealth.factoryStatus);
	EXPECT_EQ(outputModel::EOutputProviderLifecycle::Ready, selectedHealth.lifecycle);
	EXPECT_EQ(outputModel::EOutputProviderInitializationStage::Ready,
		selectedHealth.initializationStage);
	EXPECT_TRUE(selectedHealth.compiledIn);
	EXPECT_TRUE(selectedHealth.available);
	EXPECT_TRUE(selectedHealth.testOverrideActive);
	EXPECT_EQ(1U, selectedHealth.currentRevision);
	ASSERT_TRUE(fixture.runtime->Start().IsUsable());
	EXPECT_EQ(1U, buildCount);
	ASSERT_NE(nullptr, fixture.runtime->Output());

	const auto* borrowed = fixture.runtime->Output();
	ASSERT_EQ(EWorkbenchRuntimeResultCode::Stopped, fixture.runtime->Stop().code);
	ASSERT_NE(nullptr, borrowed);
	EXPECT_TRUE(borrowed->Snapshot().stopped);
	EXPECT_EQ(outputModel::EOutputProviderLifecycle::Stopped,
		fixture.runtime->OutputProviderHealth().lifecycle);
}

TEST(CWorkbenchRuntime, ExplicitRustInitializationFailureIsTerminalWithoutCppFallback)
{
	std::size_t rustBuildCount = 0;
	std::size_t cppBuildCount = 0;
	RuntimeFixture fixture(Bootstrap(), {}, {}, {}, outputModel::EOutputProviderKind::Rust,
		[&rustBuildCount](const outputModel::OutputServiceLimits&) {
			++rustBuildCount;
			return std::unique_ptr<outputModel::IOutputService>{};
		},
		[&cppBuildCount](const outputModel::OutputServiceLimits& limits) {
			++cppBuildCount;
			return std::make_unique<outputModel::OutputService>(limits);
		});

	EXPECT_EQ(outputModel::EOutputProviderKind::Rust, fixture.runtime->OutputProviderKind());
	EXPECT_EQ(1U, rustBuildCount);
	EXPECT_EQ(0U, cppBuildCount);
	EXPECT_EQ(nullptr, fixture.runtime->Output());
	const auto unavailableHealth = fixture.runtime->OutputProviderHealth();
	EXPECT_EQ(outputModel::EOutputProviderKind::Rust, unavailableHealth.kind);
	EXPECT_EQ(outputModel::EOutputProviderFactoryStatus::InitializationFailed,
		unavailableHealth.factoryStatus);
	EXPECT_EQ(outputModel::EOutputProviderLifecycle::Unavailable,
		unavailableHealth.lifecycle);
	EXPECT_EQ(outputModel::EOutputProviderInitializationStage::ProviderConstruction,
		unavailableHealth.initializationStage);
	EXPECT_EQ(outputModel::EOutputProviderFault::Initialization, unavailableHealth.fault);
	EXPECT_EQ(outputModel::EOutputProviderBoundary::Factory,
		unavailableHealth.failureBoundary);
	EXPECT_FALSE(unavailableHealth.available);
	EXPECT_TRUE(unavailableHealth.testOverrideActive);
	EXPECT_EQ(1U, unavailableHealth.counters.initializationAttempts);
	EXPECT_EQ(1U, unavailableHealth.counters.boundaryFailures);
	const auto failed = fixture.runtime->Start();
	EXPECT_EQ(EWorkbenchRuntimeResultCode::Failed, failed.code);
	EXPECT_EQ(EWorkbenchRuntimeState::Failed, failed.snapshot.state);
	EXPECT_EQ(nullptr, fixture.runtime->Output());
	EXPECT_EQ(1U, rustBuildCount);
	EXPECT_EQ(0U, cppBuildCount);
	ASSERT_FALSE(failed.snapshot.diagnostics.empty());
	EXPECT_NE(std::string::npos,
		failed.snapshot.diagnostics.front().message.find("Rust Output provider"));
	EXPECT_EQ(EWorkbenchRuntimeResultCode::Failed, fixture.runtime->Start().code);
	EXPECT_EQ(EWorkbenchRuntimeResultCode::Stopped, fixture.runtime->Stop().code);
}

TEST(CWorkbenchRuntime, RejectsAProviderCreatorThatMislabelsItsSelectedKind)
{
	std::size_t rustBuildCount = 0;
	std::size_t cppBuildCount = 0;
	RuntimeFixture fixture(Bootstrap(), {}, {}, {}, outputModel::EOutputProviderKind::Rust,
		[&rustBuildCount](const outputModel::OutputServiceLimits& limits) {
			++rustBuildCount;
			return std::make_unique<outputModel::OutputService>(limits);
		},
		[&cppBuildCount](const outputModel::OutputServiceLimits& limits) {
			++cppBuildCount;
			return std::make_unique<outputModel::OutputService>(limits);
		});

	EXPECT_EQ(1U, rustBuildCount);
	EXPECT_EQ(0U, cppBuildCount);
	EXPECT_EQ(nullptr, fixture.runtime->Output());
	const auto health = fixture.runtime->OutputProviderHealth();
	EXPECT_EQ(outputModel::EOutputProviderKind::Rust, health.kind);
	EXPECT_EQ(outputModel::EOutputProviderFactoryStatus::InvalidSelection,
		health.factoryStatus);
	EXPECT_EQ(outputModel::EOutputProviderLifecycle::Unavailable, health.lifecycle);
	EXPECT_EQ(outputModel::EOutputProviderFault::AbiContract, health.fault);
	EXPECT_FALSE(health.available);
	EXPECT_TRUE(health.testOverrideActive);
	EXPECT_EQ(EWorkbenchRuntimeResultCode::Failed, fixture.runtime->Start().code);
	EXPECT_EQ(EWorkbenchRuntimeResultCode::Stopped, fixture.runtime->Stop().code);
}

TEST(CWorkbenchRuntime, ExplicitRustOwnsOnlyItsProviderAndRetainsItsStoppedBorrow)
{
	std::size_t rustBuildCount = 0;
	std::size_t cppBuildCount = 0;
	RuntimeFixture fixture(Bootstrap(), {}, {}, {}, outputModel::EOutputProviderKind::Rust,
		[&rustBuildCount](const outputModel::OutputServiceLimits& limits) {
			++rustBuildCount;
			return std::make_unique<DelegatingOutputProvider>(limits);
		},
		[&cppBuildCount](const outputModel::OutputServiceLimits& limits) {
			++cppBuildCount;
			return std::make_unique<outputModel::OutputService>(limits);
		});

	EXPECT_EQ(1U, rustBuildCount);
	EXPECT_EQ(0U, cppBuildCount);
	EXPECT_FALSE(fixture.runtime->OutputCandidateAvailable());
	ASSERT_TRUE(fixture.runtime->Start().IsUsable());
	auto* const output = fixture.runtime->Output();
	ASSERT_NE(nullptr, output);
	EXPECT_FALSE(fixture.runtime->OutputCandidateAvailable());
	const auto readyHealth = fixture.runtime->OutputProviderHealth();
	EXPECT_EQ(outputModel::EOutputProviderKind::Rust, readyHealth.kind);
	EXPECT_EQ(outputModel::EOutputProviderLifecycle::Ready, readyHealth.lifecycle);
	EXPECT_TRUE(readyHealth.available);
	EXPECT_TRUE(readyHealth.testOverrideActive);

	const outputModel::OutputOwner owner{ .ownerId = "runtime.explicit-rust", .generation = 1 };
	ASSERT_EQ(outputModel::EOutputOperationStatus::Succeeded, output->CreateChannel({
		.operation = { .operationId = "runtime.explicit-rust.create" },
		.owner = owner,
		.channelId = "runtime.explicit-rust",
		.label = "Explicit Rust",
	}).status);
	EXPECT_EQ(2U, fixture.runtime->OutputProviderHealth().currentRevision);
	EXPECT_EQ(EWorkbenchRuntimeResultCode::Stopped, fixture.runtime->Stop().code);
	EXPECT_TRUE(output->Snapshot().stopped);
	EXPECT_EQ(outputModel::EOutputProviderLifecycle::Stopped,
		fixture.runtime->OutputProviderHealth().lifecycle);
	EXPECT_EQ(1U, rustBuildCount);
	EXPECT_EQ(0U, cppBuildCount);
}

TEST(CWorkbenchRuntime, CompileSelectedOutputProviderOwnsTheRuntimeLifecycle)
{
	RuntimeFixture fixture(Bootstrap());
	EXPECT_EQ(outputModel::DefaultOutputProviderKind(), fixture.runtime->OutputProviderKind());
	EXPECT_EQ(nullptr, fixture.runtime->Output());
	ASSERT_TRUE(fixture.runtime->Start().IsUsable());
	auto* const output = fixture.runtime->Output();
	ASSERT_NE(nullptr, output);
	const auto readyHealth = fixture.runtime->OutputProviderHealth();
	EXPECT_EQ(fixture.runtime->OutputProviderKind(), readyHealth.kind);
	EXPECT_EQ(outputModel::EOutputProviderFactoryStatus::Created, readyHealth.factoryStatus);
	EXPECT_EQ(outputModel::EOutputProviderLifecycle::Ready, readyHealth.lifecycle);
	EXPECT_TRUE(readyHealth.available);
	EXPECT_FALSE(readyHealth.testOverrideActive);

#if defined(SAKURA_OUTPUT_BACKEND_RUST)
	EXPECT_EQ(outputModel::EOutputProviderKind::Rust, fixture.runtime->OutputProviderKind());
	EXPECT_NE(nullptr, dynamic_cast<outputModel::OutputServiceRustProvider*>(output));
	EXPECT_EQ(nullptr, dynamic_cast<outputModel::OutputService*>(output));
	EXPECT_FALSE(fixture.runtime->OutputCandidateAvailable());
#else
	EXPECT_EQ(outputModel::EOutputProviderKind::Cpp, fixture.runtime->OutputProviderKind());
	EXPECT_NE(nullptr, dynamic_cast<outputModel::OutputService*>(output));
	EXPECT_EQ(nullptr, dynamic_cast<outputModel::OutputServiceRustProvider*>(output));
#endif

	const outputModel::OutputOwner owner{ .ownerId = "runtime.compile-selected", .generation = 1 };
	ASSERT_EQ(outputModel::EOutputOperationStatus::Succeeded, output->CreateChannel({
		.operation = { .operationId = "runtime.compile-selected.create" },
		.owner = owner,
		.channelId = "runtime.compile-selected",
		.label = "Compile-selected provider",
	}).status);
	EXPECT_EQ(1U, output->Snapshot().channels.size());
	const auto mutatedHealth = fixture.runtime->OutputProviderHealth();
	EXPECT_TRUE(mutatedHealth.hasLastOperation);
	EXPECT_EQ(outputModel::EOutputOperationStatus::Succeeded,
		mutatedHealth.lastOperationStatus);
	EXPECT_EQ(1U, mutatedHealth.counters.mutationCalls);
	EXPECT_EQ(1U, mutatedHealth.counters.acceptedOperations);
#if defined(SAKURA_OUTPUT_BACKEND_RUST)
	EXPECT_EQ(SAKURA_OUTPUT_PROVIDER_ABI_VERSION_V1, mutatedHealth.abiVersion);
#endif
	EXPECT_EQ(EWorkbenchRuntimeResultCode::Stopped, fixture.runtime->Stop().code);
	EXPECT_EQ(nullptr, fixture.runtime->Output());
	EXPECT_TRUE(output->Snapshot().stopped);
	EXPECT_EQ(outputModel::EOutputOperationStatus::Stopped, output->AppendOutput({
		.operation = { .operationId = "runtime.compile-selected.after-stop" },
		.owner = owner,
		.channelId = "runtime.compile-selected",
		.text = "after stop",
	}).status);
	const auto stoppedHealth = fixture.runtime->OutputProviderHealth();
	EXPECT_EQ(outputModel::EOutputProviderLifecycle::Stopped, stoppedHealth.lifecycle);
	EXPECT_EQ(1U, stoppedHealth.counters.stopCalls);
	EXPECT_TRUE(stoppedHealth.hasLastOperation);
	EXPECT_EQ(outputModel::EOutputOperationStatus::Stopped,
		stoppedHealth.lastOperationStatus);
}

TEST(CWorkbenchRuntime, ComposesRustOutputCandidateBeforePublishingRuntime)
{
	RuntimeFixture fixture(Bootstrap(), {}, {}, {}, outputModel::EOutputProviderKind::Cpp);
	EXPECT_EQ(nullptr, fixture.runtime->Output());
	const auto beforeStart = fixture.runtime->OutputCandidateDiagnostics();
	if (!fixture.runtime->OutputCandidateAvailable()) {
		EXPECT_EQ(outputModel::EOutputServiceRustCandidateAvailability::Unavailable, beforeStart.availability);
		EXPECT_EQ(outputModel::EOutputServiceRustCandidateState::Unavailable, beforeStart.state);
		EXPECT_EQ(outputModel::EOutputServiceRustCandidateFault::Unavailable, beforeStart.fault);
		ASSERT_TRUE(fixture.runtime->Start().IsUsable());
		EXPECT_NE(nullptr, fixture.runtime->Output());
		EXPECT_EQ(EWorkbenchRuntimeResultCode::Stopped, fixture.runtime->Stop().code);
		return;
	}

	EXPECT_EQ(outputModel::EOutputServiceRustCandidateAvailability::Available, beforeStart.availability);
	EXPECT_EQ(outputModel::EOutputServiceRustCandidateState::Live, beforeStart.state);
	EXPECT_EQ(outputModel::EOutputServiceRustCandidateFault::None, beforeStart.fault);
	EXPECT_EQ(0U, beforeStart.appliedCommitCount);
	EXPECT_TRUE(fixture.runtime->OutputCandidateMatchesAuthority());
	ASSERT_TRUE(fixture.runtime->Start().IsUsable());
	EXPECT_NE(nullptr, fixture.runtime->Output());
	EXPECT_TRUE(fixture.runtime->OutputCandidateMatchesAuthority());
	EXPECT_EQ(EWorkbenchRuntimeResultCode::Stopped, fixture.runtime->Stop().code);
}

TEST(CWorkbenchRuntime, RustOutputCandidateObservesAcceptedLiveCommitsWithoutReplacingCppAuthority)
{
	RuntimeFixture fixture(Bootstrap(), {}, {}, {}, outputModel::EOutputProviderKind::Cpp);
	if (!fixture.runtime->OutputCandidateAvailable()) {
		ASSERT_TRUE(fixture.runtime->Start().IsUsable());
		EXPECT_NE(nullptr, fixture.runtime->Output());
		EXPECT_EQ(EWorkbenchRuntimeResultCode::Stopped, fixture.runtime->Stop().code);
		return;
	}
	ASSERT_TRUE(fixture.runtime->Start().IsUsable());
	auto* const output = fixture.runtime->Output();
	ASSERT_NE(nullptr, output);
	const workbench::IWorkbenchRuntime& boundary = *fixture.runtime;
	EXPECT_EQ(output, boundary.Output());

	std::size_t notificationCount = 0;
	const auto subscription = output->Subscribe([&notificationCount](const outputModel::OutputServiceChange&) {
		++notificationCount;
	});
	ASSERT_TRUE(subscription.has_value());
	const outputModel::OutputOwner owner{ .ownerId = "runtime.candidate-live", .generation = 1 };
	const auto created = output->CreateChannel({
		.operation = { .operationId = "runtime.candidate-live.create" },
		.owner = owner,
		.channelId = "runtime.candidate-live",
		.label = "Candidate live",
	});
	EXPECT_EQ(outputModel::EOutputOperationStatus::Succeeded, created.status);
	ASSERT_EQ(1U, output->Snapshot().channels.size());
	EXPECT_TRUE(fixture.runtime->OutputCandidateMatchesAuthority());
	EXPECT_EQ(1U, fixture.runtime->OutputCandidateDiagnostics().appliedCommitCount);

	const auto appended = output->AppendOutput({
		.operation = { .operationId = "runtime.candidate-live.append" },
		.owner = owner,
		.channelId = "runtime.candidate-live",
		.text = "accepted live text",
	});
	EXPECT_EQ(outputModel::EOutputOperationStatus::Succeeded, appended.status);
	const auto authority = output->Snapshot();
	ASSERT_EQ(1U, authority.channels.size());
	EXPECT_EQ("accepted live text", authority.channels.front().text);
	EXPECT_EQ(2U, notificationCount);
	EXPECT_TRUE(fixture.runtime->OutputCandidateMatchesAuthority());
	EXPECT_EQ(2U, fixture.runtime->OutputCandidateDiagnostics().appliedCommitCount);
	EXPECT_EQ(EWorkbenchRuntimeResultCode::Stopped, fixture.runtime->Stop().code);
}

TEST(CWorkbenchRuntime, RustOutputCandidateExcludesReplayAndRejectedOperations)
{
	RuntimeFixture fixture(Bootstrap(), {}, {}, {}, outputModel::EOutputProviderKind::Cpp);
	if (!fixture.runtime->OutputCandidateAvailable()) {
		ASSERT_TRUE(fixture.runtime->Start().IsUsable());
		EXPECT_EQ(EWorkbenchRuntimeResultCode::Stopped, fixture.runtime->Stop().code);
		return;
	}
	ASSERT_TRUE(fixture.runtime->Start().IsUsable());
	auto* const output = fixture.runtime->Output();
	ASSERT_NE(nullptr, output);
	const outputModel::OutputOwner owner{ .ownerId = "runtime.candidate-filter", .generation = 1 };
	const outputModel::OutputCreateChannelRequest createRequest {
		.operation = { .operationId = "runtime.candidate-filter.create" },
		.owner = owner,
		.channelId = "runtime.candidate-filter",
		.label = "Candidate filter",
	};
	ASSERT_EQ(outputModel::EOutputOperationStatus::Succeeded, output->CreateChannel(createRequest).status);
	const auto acceptedCount = fixture.runtime->OutputCandidateDiagnostics().appliedCommitCount;
	ASSERT_EQ(1U, acceptedCount);

	EXPECT_EQ(outputModel::EOutputOperationStatus::Replayed, output->CreateChannel(createRequest).status);
	EXPECT_EQ(outputModel::EOutputOperationStatus::Rejected, output->AppendOutput({
		.operation = { .operationId = "runtime.candidate-filter.invalid-owner" },
		.owner = {},
		.channelId = "runtime.candidate-filter",
		.text = "rejected",
	}).status);
	EXPECT_EQ(outputModel::EOutputOperationStatus::Conflict, output->AppendOutput({
		.operation = { .operationId = "runtime.candidate-filter.wrong-generation" },
		.owner = { .ownerId = owner.ownerId, .generation = 2 },
		.channelId = "runtime.candidate-filter",
		.text = "conflict",
	}).status);

	EXPECT_EQ(acceptedCount, fixture.runtime->OutputCandidateDiagnostics().appliedCommitCount);
	EXPECT_TRUE(fixture.runtime->OutputCandidateMatchesAuthority());
	const auto authority = output->Snapshot();
	ASSERT_EQ(1U, authority.channels.size());
	EXPECT_TRUE(authority.channels.front().text.empty());
	EXPECT_EQ(EWorkbenchRuntimeResultCode::Stopped, fixture.runtime->Stop().code);
}

TEST(CWorkbenchRuntime, StopPublishesCppOutputTerminalBeforeRustCandidateTeardown)
{
	RuntimeFixture fixture(Bootstrap(), {}, {}, {}, outputModel::EOutputProviderKind::Cpp);
	const bool candidateAvailable = fixture.runtime->OutputCandidateAvailable();
	ASSERT_TRUE(fixture.runtime->Start().IsUsable());
	auto* const output = fixture.runtime->Output();
	ASSERT_NE(nullptr, output);
	const outputModel::OutputOwner owner{ .ownerId = "runtime.candidate-stop", .generation = 1 };
	ASSERT_EQ(outputModel::EOutputOperationStatus::Succeeded, output->CreateChannel({
		.operation = { .operationId = "runtime.candidate-stop.create" },
		.owner = owner,
		.channelId = "runtime.candidate-stop",
		.label = "Candidate stop",
	}).status);

	const auto stopped = fixture.runtime->Stop();
	EXPECT_EQ(EWorkbenchRuntimeResultCode::Stopped, stopped.code);
	EXPECT_EQ(nullptr, fixture.runtime->Output());
	EXPECT_TRUE(output->Snapshot().stopped);
	EXPECT_EQ(outputModel::EOutputOperationStatus::Stopped, output->AppendOutput({
		.operation = { .operationId = "runtime.candidate-stop.after-stop" },
		.owner = owner,
		.channelId = "runtime.candidate-stop",
		.text = "after stop",
	}).status);
	const auto diagnostics = fixture.runtime->OutputCandidateDiagnostics();
	if (candidateAvailable) {
		EXPECT_EQ(outputModel::EOutputServiceRustCandidateState::Stopped, diagnostics.state);
		EXPECT_EQ(outputModel::EOutputServiceRustCandidateFault::None, diagnostics.fault);
		EXPECT_EQ(1U, diagnostics.appliedCommitCount);
	} else {
		EXPECT_EQ(outputModel::EOutputServiceRustCandidateState::Unavailable, diagnostics.state);
	}
	EXPECT_EQ(EWorkbenchRuntimeResultCode::Stopped, fixture.runtime->Stop().code);
}

TEST(CWorkbenchRuntime, FailedStartStopsRustOutputCandidateAlongsideUnpublishedCppServices)
{
	RuntimeFixture fixture(Bootstrap(), {}, {}, {}, outputModel::EOutputProviderKind::Cpp);
	fixture.files->onRead = [] { throw std::runtime_error("forced candidate runtime bootstrap failure"); };
	const bool candidateAvailable = fixture.runtime->OutputCandidateAvailable();

	const auto failed = fixture.runtime->Start();
	EXPECT_EQ(EWorkbenchRuntimeResultCode::Failed, failed.code);
	EXPECT_EQ(EWorkbenchRuntimeState::Failed, failed.snapshot.state);
	EXPECT_EQ(nullptr, fixture.runtime->Output());
	const auto diagnostics = fixture.runtime->OutputCandidateDiagnostics();
	if (candidateAvailable) {
		EXPECT_EQ(outputModel::EOutputServiceRustCandidateState::Stopped, diagnostics.state);
		EXPECT_EQ(outputModel::EOutputServiceRustCandidateFault::None, diagnostics.fault);
	} else {
		EXPECT_EQ(outputModel::EOutputServiceRustCandidateState::Unavailable, diagnostics.state);
	}
	EXPECT_EQ(EWorkbenchRuntimeResultCode::Failed, fixture.runtime->Start().code);
	EXPECT_EQ(EWorkbenchRuntimeResultCode::Stopped, fixture.runtime->Stop().code);
}

TEST(CWorkbenchRuntime, OwnsTaskExecutionAndFolderCatalogsOnlyWhileRunning)
{
	const auto unknown = Parse(L"file:///C:/Unknown");
	RuntimeFixture fixture(Bootstrap());
	const workbench::IWorkbenchRuntime& boundary = *fixture.runtime;
	EXPECT_EQ(nullptr, fixture.runtime->TaskExecution());
	EXPECT_EQ(nullptr, boundary.TaskExecution());
	EXPECT_FALSE(fixture.runtime->TaskCatalogForFolder(unknown).has_value());

	ASSERT_TRUE(fixture.runtime->Start().IsUsable());
	auto* execution = fixture.runtime->TaskExecution();
	ASSERT_NE(nullptr, execution);
	EXPECT_EQ(execution, boundary.TaskExecution());
	EXPECT_FALSE(fixture.runtime->TaskCatalogForFolder(unknown).has_value());
	EXPECT_FALSE(execution->Snapshot().stopped);

	EXPECT_EQ(EWorkbenchRuntimeResultCode::Stopped, fixture.runtime->Stop().code);
	EXPECT_EQ(nullptr, fixture.runtime->TaskExecution());
	EXPECT_EQ(nullptr, boundary.TaskExecution());
	EXPECT_FALSE(fixture.runtime->TaskCatalogForFolder(unknown).has_value());
	EXPECT_TRUE(execution->Snapshot().stopped);
}

TEST(CWorkbenchRuntime, BuildsOneKnownFolderTaskCatalogWithoutInventingADefaultFolder)
{
	const auto folder = Parse(L"file:///C:/Repo");
	const auto tasks = Parse(L"file:///C:/Repo/.vscode/tasks.json");
	RuntimeFixture fixture(Bootstrap(folder));
	fixture.files->Set(tasks, Bytes(R"json({
		"version": "2.0.0",
		"tasks": [{
			"label": "build",
			"type": "process",
			"command": "cmd.exe",
			"args": ["/d", "/c", "exit /b 0"]
		}]
	})json"));

	ASSERT_TRUE(fixture.runtime->Start().IsUsable());
	const auto selected = fixture.runtime->TaskCatalogForFolder(folder);
	ASSERT_TRUE(selected.has_value());
	ASSERT_TRUE(selected->catalog.sourceUri.has_value());
	EXPECT_EQ(tasks.ToString(), selected->catalog.sourceUri->ToString());
	ASSERT_EQ(1U, selected->catalog.definitions.size());
	EXPECT_EQ(L"build", selected->catalog.definitions.front().label);
	EXPECT_EQ(workbench::tasks::ETaskExecutionKind::Process,
		selected->catalog.definitions.front().executionKind);
	EXPECT_FALSE(fixture.runtime->TaskCatalogForFolder(Parse(L"file:///C:/Other")).has_value());
}

TEST(CWorkbenchRuntime, KeepsMultiRootTaskOverridesAndFallbacksFolderScopedAcrossReorder)
{
	const auto workspaceConfig = Parse(L"file:///C:/Work/tasks.code-workspace");
	const auto first = Parse(L"file:///C:/One");
	const auto second = Parse(L"file:///C:/Two");
	const auto firstTasks = Parse(L"file:///C:/One/.vscode/tasks.json");
	RuntimeFixture fixture(WorkspaceBootstrap(
		workspaceConfig, { { first, L"one" }, { second, L"two" } }));
	fixture.files->Set(workspaceConfig, Bytes(R"json({
		"folders": [
			{ "uri": "file:///C:/One", "name": "one" },
			{ "uri": "file:///C:/Two", "name": "two" }
		],
		"tasks": {
			"version": "2.0.0",
			"tasks": [{ "label": "workspace", "type": "shell", "command": "Write-Output workspace" }]
		}
	})json"));
	fixture.files->Set(firstTasks, Bytes(R"json({
		"version": "2.0.0",
		"tasks": [{ "label": "first", "type": "shell", "command": "Write-Output first" }]
	})json"));

	ASSERT_TRUE(fixture.runtime->Start().IsUsable());
	const auto firstBefore = fixture.runtime->TaskCatalogForFolder(first);
	const auto secondBefore = fixture.runtime->TaskCatalogForFolder(second);
	ASSERT_TRUE(firstBefore.has_value());
	ASSERT_TRUE(secondBefore.has_value());
	ASSERT_TRUE(firstBefore->catalog.sourceUri.has_value());
	ASSERT_TRUE(secondBefore->catalog.sourceUri.has_value());
	EXPECT_EQ(firstTasks.ToString(), firstBefore->catalog.sourceUri->ToString());
	EXPECT_EQ(workspaceConfig.ToString(), secondBefore->catalog.sourceUri->ToString());
	ASSERT_EQ(1U, firstBefore->catalog.definitions.size());
	ASSERT_EQ(1U, secondBefore->catalog.definitions.size());
	EXPECT_EQ(L"first", firstBefore->catalog.definitions.front().label);
	EXPECT_EQ(L"workspace", secondBefore->catalog.definitions.front().label);
	const auto readsBeforeReorder = fixture.files->Reads();

	fixture.files->Set(workspaceConfig, Bytes(R"json({
		"folders": [
			{ "uri": "file:///C:/Two", "name": "two" },
			{ "uri": "file:///C:/One", "name": "one" }
		],
		"tasks": {
			"version": "2.0.0",
			"tasks": [{ "label": "workspace", "type": "shell", "command": "Write-Output workspace" }]
		}
	})json"));
	const auto reordered = fixture.runtime->WorkspaceContext().SetWorkspace({
		.operation = {
			.operationId = "test.tasks.reorder",
			.expectedRevision = fixture.runtime->WorkspaceContext().Snapshot().revision,
		},
		.workspaceConfigUri = workspaceConfig,
		.folders = { { second, L"two" }, { first, L"one" } },
	});
	ASSERT_EQ(EWorkspaceContextOutcome::Succeeded, reordered.outcome);
	const auto firstAfter = fixture.runtime->TaskCatalogForFolder(first);
	const auto secondAfter = fixture.runtime->TaskCatalogForFolder(second);
	ASSERT_TRUE(firstAfter.has_value());
	ASSERT_TRUE(secondAfter.has_value());
	EXPECT_EQ(firstBefore->catalog.generation, firstAfter->catalog.generation);
	EXPECT_EQ(firstBefore->catalog.revision, firstAfter->catalog.revision);
	EXPECT_EQ(secondBefore->catalog.generation, secondAfter->catalog.generation);
	EXPECT_EQ(secondBefore->catalog.revision, secondAfter->catalog.revision);
	// Settings may resnapshot on order changes; artifact resources must not be
	// cleared or re-read merely because Explorer folder order changed.
	const auto readsAfterReorder = fixture.files->Reads();
	EXPECT_EQ(std::count(readsBeforeReorder.begin(), readsBeforeReorder.end(), firstTasks.ToString()),
		std::count(readsAfterReorder.begin(), readsAfterReorder.end(), firstTasks.ToString()));
	EXPECT_GE(readsAfterReorder.size(), readsBeforeReorder.size());
}

TEST(CWorkbenchRuntime, AddsAndRemovesKnownEmptyTaskCatalogSlotsWithWorkspaceTopology)
{
	const auto folder = Parse(L"file:///C:/Dynamic");
	RuntimeFixture fixture(Bootstrap());
	const auto* const runtimeAddress = fixture.runtime.get();
	ASSERT_TRUE(fixture.runtime->Start().IsUsable());
	EXPECT_FALSE(fixture.runtime->TaskCatalogForFolder(folder).has_value());

	const auto opened = fixture.runtime->SwitchToFolderWorkspace(folder, L"Dynamic");
	ASSERT_EQ(EWorkspaceContextOutcome::Succeeded, opened.outcome);
	EXPECT_EQ(runtimeAddress, fixture.runtime.get());
	const auto knownEmpty = fixture.runtime->TaskCatalogForFolder(folder);
	ASSERT_TRUE(knownEmpty.has_value());
	EXPECT_TRUE(knownEmpty->catalog.definitions.empty());

	const auto emptied = fixture.runtime->WorkspaceContext().SetEmpty({
		.operationId = "test.tasks.empty",
		.expectedRevision = opened.revision,
	});
	ASSERT_EQ(EWorkspaceContextOutcome::Succeeded, emptied.outcome);
	EXPECT_FALSE(fixture.runtime->TaskCatalogForFolder(folder).has_value());
}

TEST(CWorkbenchRuntime, FailedStartRollsBackUnpublishedMarkerAndOutputServices)
{
	outputModel::OutputService* selectedOutput = nullptr;
	RuntimeFixture fixture(Bootstrap(), {}, {}, {}, outputModel::EOutputProviderKind::Cpp,
		{}, [&selectedOutput](const outputModel::OutputServiceLimits& limits) {
			auto provider = std::make_unique<outputModel::OutputService>(limits);
			selectedOutput = provider.get();
			return provider;
		});
	fixture.files->onRead = [] { throw std::runtime_error("forced runtime bootstrap failure"); };

	const auto failed = fixture.runtime->Start();
	EXPECT_EQ(EWorkbenchRuntimeResultCode::Failed, failed.code);
	EXPECT_EQ(EWorkbenchRuntimeState::Failed, failed.snapshot.state);
	ASSERT_NE(nullptr, selectedOutput);
	EXPECT_TRUE(selectedOutput->Snapshot().stopped);
	EXPECT_EQ(nullptr, fixture.runtime->Markers());
	EXPECT_EQ(nullptr, fixture.runtime->Output());
	EXPECT_EQ(EWorkbenchRuntimeResultCode::Failed, fixture.runtime->Start().code);
	const auto stopped = fixture.runtime->Stop();
	EXPECT_EQ(EWorkbenchRuntimeResultCode::Stopped, stopped.code);
	const auto repeatedStop = fixture.runtime->Stop();
	EXPECT_EQ(EWorkbenchRuntimeResultCode::Stopped, repeatedStop.code);
	EXPECT_EQ(stopped.snapshot.revision, repeatedStop.snapshot.revision);
	EXPECT_EQ(nullptr, fixture.runtime->Markers());
	EXPECT_EQ(nullptr, fixture.runtime->Output());
}

TEST(CWorkbenchRuntime, FailedStartRetainsDeferredOutputStopForExternalRetry)
{
	DelegatingOutputProvider* selectedOutput = nullptr;
	RuntimeFixture fixture(Bootstrap(), {}, {}, {}, outputModel::EOutputProviderKind::Cpp,
		{}, [&selectedOutput](const outputModel::OutputServiceLimits& limits) {
			auto provider = std::make_unique<DelegatingOutputProvider>(
				limits, true, outputModel::EOutputProviderKind::Cpp);
			selectedOutput = provider.get();
			return provider;
		});
	fixture.files->onRead = [] { throw std::runtime_error("forced deferred-stop bootstrap failure"); };

	const auto failed = fixture.runtime->Start();
	EXPECT_EQ(EWorkbenchRuntimeResultCode::Failed, failed.code);
	EXPECT_EQ(EWorkbenchRuntimeState::Failed, failed.snapshot.state);
	ASSERT_NE(nullptr, selectedOutput);
	EXPECT_EQ(1U, selectedOutput->StopCalls());
	EXPECT_TRUE(selectedOutput->Snapshot().stopped);
	EXPECT_EQ(nullptr, fixture.runtime->Output());

	const auto stopped = fixture.runtime->Stop();
	EXPECT_EQ(EWorkbenchRuntimeResultCode::Stopped, stopped.code);
	EXPECT_EQ(EWorkbenchRuntimeState::Stopped, stopped.snapshot.state);
	EXPECT_EQ(2U, selectedOutput->StopCalls());
	EXPECT_TRUE(selectedOutput->Snapshot().stopped);

	const auto repeatedStop = fixture.runtime->Stop();
	EXPECT_EQ(EWorkbenchRuntimeResultCode::Stopped, repeatedStop.code);
	EXPECT_EQ(stopped.snapshot.revision, repeatedStop.snapshot.revision);
	EXPECT_EQ(2U, selectedOutput->StopCalls());
}

TEST(CWorkbenchRuntime, MarkerCallbackStopDefersRuntimeTerminalPublicationUntilAnExternalStop)
{
	RuntimeFixture fixture(Bootstrap());
	ASSERT_TRUE(fixture.runtime->Start().IsUsable());
	auto* markers = fixture.runtime->Markers();
	auto* output = fixture.runtime->Output();
	ASSERT_NE(nullptr, markers);
	ASSERT_NE(nullptr, output);
	std::optional<workbench::WorkbenchRuntimeResult> callbackStop;
	const auto subscription = markers->Subscribe([&fixture, &callbackStop](const problems::MarkerChange&) {
		callbackStop = fixture.runtime->Stop();
	});
	ASSERT_EQ(problems::EMarkerSubscriptionStatus::Subscribed, subscription.status);

	const problems::MarkerOwner owner{ .id = "runtime.marker-callback-stop", .generation = 1 };
	EXPECT_EQ(problems::EMarkerOperationStatus::Replaced, markers->Replace({
		.collection = { .owner = owner, .id = "runtime.marker-callback-collection" },
		.resource = Parse(L"file:///C:/Runtime/marker-callback-stop.cpp"),
		.markers = { { .range = { .endLine = 1 }, .message = "callback stop" } },
	}).status);

	ASSERT_TRUE(callbackStop.has_value());
	EXPECT_EQ(EWorkbenchRuntimeResultCode::Busy, callbackStop->code);
	EXPECT_NE(EWorkbenchRuntimeState::Stopped, callbackStop->snapshot.state);
	EXPECT_TRUE(markers->Snapshot().stopped);
	EXPECT_TRUE(output->Snapshot().stopped);
	EXPECT_NE(EWorkbenchRuntimeState::Stopped, fixture.runtime->Snapshot().state);
	EXPECT_EQ(nullptr, fixture.runtime->Markers());
	EXPECT_EQ(nullptr, fixture.runtime->Output());
	EXPECT_EQ(EWorkbenchRuntimeResultCode::Stopped, fixture.runtime->Stop().code);
	EXPECT_EQ(EWorkbenchRuntimeState::Stopped, fixture.runtime->Snapshot().state);
}

TEST(CWorkbenchRuntime, OutputCallbackStopDefersRuntimeTerminalPublicationUntilAnExternalStop)
{
	RuntimeFixture fixture(Bootstrap());
	ASSERT_TRUE(fixture.runtime->Start().IsUsable());
	auto* markers = fixture.runtime->Markers();
	auto* output = fixture.runtime->Output();
	ASSERT_NE(nullptr, markers);
	ASSERT_NE(nullptr, output);
	std::optional<workbench::WorkbenchRuntimeResult> callbackStop;
	const auto subscription = output->Subscribe([&fixture, &callbackStop](const outputModel::OutputServiceChange&) {
		callbackStop = fixture.runtime->Stop();
	});
	ASSERT_TRUE(subscription.has_value());

	const outputModel::OutputOwner owner{ .ownerId = "runtime.output-callback-stop", .generation = 1 };
	EXPECT_EQ(outputModel::EOutputOperationStatus::Succeeded, output->CreateChannel({
		.operation = { .operationId = "runtime.output-callback-stop.create" },
		.owner = owner,
		.channelId = "runtime.output-callback-stop",
		.label = "Runtime callback stop",
	}).status);

	ASSERT_TRUE(callbackStop.has_value());
	EXPECT_EQ(EWorkbenchRuntimeResultCode::Busy, callbackStop->code);
	EXPECT_NE(EWorkbenchRuntimeState::Stopped, callbackStop->snapshot.state);
	EXPECT_TRUE(markers->Snapshot().stopped);
	EXPECT_TRUE(output->Snapshot().stopped);
	EXPECT_NE(EWorkbenchRuntimeState::Stopped, fixture.runtime->Snapshot().state);
	EXPECT_EQ(nullptr, fixture.runtime->Markers());
	EXPECT_EQ(nullptr, fixture.runtime->Output());
	EXPECT_EQ(EWorkbenchRuntimeResultCode::Stopped, fixture.runtime->Stop().code);
	EXPECT_EQ(EWorkbenchRuntimeState::Stopped, fixture.runtime->Snapshot().state);
}

TEST(CWorkbenchRuntime, TaskCallbackStopDefersRuntimeTerminalPublicationUntilTaskQuiescenceAndExternalStop)
{
	auto factory = std::make_shared<RuntimeTaskSessionFactory>();
	RuntimeFixture fixture(Bootstrap(), {}, factory);
	ASSERT_TRUE(fixture.runtime->Start().IsUsable());
	auto* execution = fixture.runtime->TaskExecution();
	ASSERT_NE(nullptr, execution);

	std::optional<workbench::WorkbenchRuntimeResult> callbackStop;
	const auto subscription = execution->Subscribe(
		[&fixture, &callbackStop](const tasks::TaskExecutionServiceChange& change) {
			if (change.kind == tasks::ETaskExecutionChangeKind::RunStarted && !callbackStop) {
				callbackStop = fixture.runtime->Stop();
			}
		});
	ASSERT_TRUE(subscription.has_value());

	tasks::TaskConfigurationDefinition definition;
	definition.label = L"runtime task stop";
	definition.executionKind = tasks::ETaskExecutionKind::Process;
	definition.command = L"C:\\Tools\\runtime-task-stop.exe";
	const auto started = execution->Start({
		.operation = { .operationId = "runtime.task-callback-stop.start" },
		.definition = std::move(definition),
		.initialSize = { 80, 24 },
	});
	EXPECT_EQ(tasks::ETaskExecutionOperationStatus::Started, started.status);

	ASSERT_TRUE(callbackStop.has_value());
	EXPECT_EQ(EWorkbenchRuntimeResultCode::Busy, callbackStop->code);
	EXPECT_NE(EWorkbenchRuntimeState::Stopped, callbackStop->snapshot.state);
	EXPECT_NE(EWorkbenchRuntimeState::Stopped, fixture.runtime->Snapshot().state);
	ASSERT_NE(nullptr, factory->lastState);
	EXPECT_TRUE(execution->Snapshot().stopped);
	EXPECT_EQ(1U, factory->lastState->beginCloseCalls.load());
	EXPECT_EQ(1U, factory->lastState->waitForCloseCalls.load());

	EXPECT_EQ(EWorkbenchRuntimeResultCode::Stopped, fixture.runtime->Stop().code);
	EXPECT_EQ(EWorkbenchRuntimeState::Stopped, fixture.runtime->Snapshot().state);
}

TEST(CWorkbenchRuntime, StopWaitsForAnInFlightOwnedMarkerCallbackBeforePublishingStopped)
{
	RuntimeFixture fixture(Bootstrap());
	ASSERT_TRUE(fixture.runtime->Start().IsUsable());
	auto* markers = fixture.runtime->Markers();
	ASSERT_NE(nullptr, markers);

	std::mutex callbackMutex;
	std::condition_variable callbackEntered;
	std::condition_variable callbackRelease;
	bool entered = false;
	bool release = false;
	bool exited = false;
	const auto subscription = markers->Subscribe([&](const problems::MarkerChange&) {
		std::unique_lock lock(callbackMutex);
		entered = true;
		callbackEntered.notify_all();
		callbackRelease.wait(lock, [&release] { return release; });
		exited = true;
	});
	ASSERT_EQ(problems::EMarkerSubscriptionStatus::Subscribed, subscription.status);

	const problems::MarkerOwner owner{ .id = "runtime.stop-callback", .generation = 1 };
	const problems::MarkerCollectionIdentity collection{ .owner = owner, .id = "runtime.stop-collection" };
	const auto resource = Parse(L"file:///C:/Runtime/stop-callback.cpp");
	std::optional<problems::MarkerOperationResult> mutationResult;
	std::thread mutation([&] {
		mutationResult = markers->Replace({
			.collection = collection,
			.resource = resource,
			.markers = { { .range = { .endLine = 1 }, .message = "wait for stop" } },
		});
	});
	{
		std::unique_lock lock(callbackMutex);
		callbackEntered.wait(lock, [&entered] { return entered; });
	}

	auto stopping = std::async(std::launch::async, [&fixture] { return fixture.runtime->Stop(); });
	EXPECT_EQ(std::future_status::timeout, stopping.wait_for(std::chrono::milliseconds(20)));
	{
		std::lock_guard lock(callbackMutex);
		release = true;
	}
	callbackRelease.notify_all();
	mutation.join();
	ASSERT_TRUE(mutationResult.has_value());
	EXPECT_EQ(problems::EMarkerOperationStatus::Replaced, mutationResult->status);
	const auto stopped = stopping.get();
	EXPECT_EQ(EWorkbenchRuntimeResultCode::Stopped, stopped.code);
	EXPECT_EQ(EWorkbenchRuntimeState::Stopped, stopped.snapshot.state);
	EXPECT_TRUE(exited);
}

TEST(CWorkbenchRuntime, EmptyLaunchLoadsOnlyProfileAndNeverInfersWorkspaceFromInitialDocument)
{
	auto initialDocument = Parse(L"file:///C:/Outside/readme.md");
	RuntimeFixture fixture(Bootstrap(std::nullopt, initialDocument));
	fixture.files->Set(fixture.runtime->Bootstrap().UserDataProfile().Resources().Settings(),
		Bytes(R"json({ "workbench.editor.showTabs": "single" })json"));

	const auto started = fixture.runtime->Start();
	EXPECT_EQ(EWorkbenchRuntimeResultCode::Ready, started.code);
	EXPECT_EQ(EWorkbenchRuntimeState::Ready, started.snapshot.state);
	EXPECT_EQ(EWorkspaceKind::Empty, fixture.runtime->WorkspaceContext().Snapshot().kind);
	EXPECT_EQ(L"single", ShowTabs(*fixture.runtime, ProfileTarget(fixture.runtime->Bootstrap())));

	const auto reads = fixture.files->Reads();
	ASSERT_EQ(1U, reads.size());
	EXPECT_EQ(fixture.runtime->Bootstrap().UserDataProfile().Resources().Settings().ToString(), reads.front());
	EXPECT_FALSE(ContainsRead(reads, L"file:///C:/Outside/.vscode/settings.json"));
}

TEST(CWorkbenchRuntime, AdvisoryProfileWatchResnapshotsThroughTheExistingFileSourceController)
{
	RuntimeFixture fixture(Bootstrap());
	const auto settings = fixture.runtime->Bootstrap().UserDataProfile().Resources().Settings();
	fixture.files->Set(settings, Bytes(R"json({ "workbench.editor.showTabs": "single" })json"));
	fixture.files->EnableWatches();
	ASSERT_TRUE(fixture.runtime->Start().IsUsable());
	EXPECT_EQ(L"single", ShowTabs(*fixture.runtime, ProfileTarget(fixture.runtime->Bootstrap())));

	fixture.files->Set(settings, Bytes(R"json({ "workbench.editor.showTabs": "multiple" })json"));
	fixture.files->EmitFirstWatchEvent({ .type = EFileWatchEventType::Changed, .uri = settings });
	ASSERT_TRUE(fixture.files->WaitUntilReadCount(2));
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (ShowTabs(*fixture.runtime, ProfileTarget(fixture.runtime->Bootstrap())) != L"multiple"
		&& std::chrono::steady_clock::now() < deadline) {
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	EXPECT_EQ(L"multiple", ShowTabs(*fixture.runtime, ProfileTarget(fixture.runtime->Bootstrap())));

	EXPECT_EQ(EWorkbenchRuntimeResultCode::Stopped, fixture.runtime->Stop().code);
}

TEST(CWorkbenchRuntime, OwnsCanonicalContributionsAndAnIndependentAuxiliaryBarLayoutState)
{
	RuntimeFixture fixture(Bootstrap());
	const auto contributions = fixture.runtime->Contributions().Snapshot();
	const auto layoutSnapshot = fixture.runtime->LayoutState().Snapshot();
	const auto findContainer = [&contributions](std::string_view id) {
		return std::find_if(contributions.viewContainers.begin(), contributions.viewContainers.end(), [id](const auto& value) {
			return value.descriptor.id == id;
		});
	};
	const auto findView = [&contributions](std::string_view id) {
		return std::find_if(contributions.views.begin(), contributions.views.end(), [id](const auto& value) {
			return value.descriptor.id == id;
		});
	};
	const auto findPart = [&layoutSnapshot](std::string_view id) {
		return std::find_if(layoutSnapshot.parts.begin(), layoutSnapshot.parts.end(), [id](const auto& value) { return value.partId == id; });
	};

	EXPECT_EQ(1U, contributions.revision);
	EXPECT_EQ(10U, contributions.viewContainers.size());
	EXPECT_NE(contributions.viewContainers.end(), findContainer(layout::ids::viewContainer::Search));
	EXPECT_NE(contributions.viewContainers.end(), findContainer(layout::ids::viewContainer::Extensions));
	EXPECT_NE(contributions.viewContainers.end(), findContainer(layout::ids::viewContainer::RunAndDebug));
	EXPECT_NE(contributions.viewContainers.end(), findContainer(layout::ids::viewContainer::Problems));
	EXPECT_NE(contributions.viewContainers.end(), findContainer(layout::ids::viewContainer::Output));
	EXPECT_NE(contributions.viewContainers.end(), findContainer(layout::ids::viewContainer::Terminal));
	EXPECT_NE(contributions.viewContainers.end(), findContainer(layout::ids::viewContainer::Ports));
	EXPECT_NE(contributions.viewContainers.end(), findContainer(layout::ids::viewContainer::DebugConsole));
	// VS Code's Secondary Side Bar is empty by default, so no built-in ViewContainer
	// may claim the AuxiliaryBar location.
	EXPECT_TRUE(std::none_of(contributions.viewContainers.begin(), contributions.viewContainers.end(),
		[](const auto& value) { return value.descriptor.location == layout::EViewContainerLocation::AuxiliaryBar; }));

	// The auxiliary bar is a physical workbench part on the right. Outline remains an
	// Explorer view, rather than the legacy tool's unrelated "right edge" alias.
	const auto auxiliaryPart = findPart(layout::ids::part::Auxiliarybar);
	ASSERT_NE(layoutSnapshot.parts.end(), auxiliaryPart);
	EXPECT_EQ(layout::EWorkbenchPartPosition::Right, auxiliaryPart->position);
	const auto outline = std::find_if(contributions.views.begin(), contributions.views.end(), [](const auto& value) {
		return value.descriptor.id == layout::ids::view::Outline;
	});
	ASSERT_NE(contributions.views.end(), outline);
	EXPECT_EQ(std::string(layout::ids::viewContainer::Explorer), outline->descriptor.containerId);

	// VS Code's current `workbench.scm` View is Changes. The native SCM host
	// renders the two sibling frames, but they remain unregistered until the
	// layout model can express their hide-by-default / provider conditions.
	const auto changes = findView(layout::ids::view::SourceControl);
	ASSERT_NE(contributions.views.end(), changes);
	EXPECT_EQ("Changes", changes->descriptor.title);
	EXPECT_EQ(std::string(layout::ids::viewContainer::SourceControl), changes->descriptor.containerId);
	EXPECT_EQ(10, changes->descriptor.order);
	EXPECT_EQ(std::string_view("workbench.scm.repositories"), layout::ids::view::SourceControlRepositories);
	EXPECT_EQ(std::string_view("workbench.scm.history"), layout::ids::view::SourceControlGraph);
	EXPECT_EQ(contributions.views.end(), findView(layout::ids::view::SourceControlRepositories));
	EXPECT_EQ(contributions.views.end(), findView(layout::ids::view::SourceControlGraph));
	const auto extensionsInstalled = findView(layout::ids::view::ExtensionsInstalled);
	ASSERT_NE(contributions.views.end(), extensionsInstalled);
	EXPECT_EQ("Installed", extensionsInstalled->descriptor.title);
	EXPECT_EQ(std::string(layout::ids::viewContainer::Extensions),
		extensionsInstalled->descriptor.containerId);
}


TEST(CWorkbenchRuntime, ExplicitFolderLoadsItsVscodeSettingsWithWorkspaceAndFolderIdentity)
{
	auto folder = Parse(L"file:///C:/Repo");
	RuntimeFixture fixture(Bootstrap(folder));
	fixture.files->Set(fixture.runtime->Bootstrap().UserDataProfile().Resources().Settings(),
		Bytes(R"json({ "workbench.editor.showTabs": "single" })json"));
	fixture.files->Set(Parse(L"file:///C:/Repo/.vscode/settings.json"),
		Bytes(R"json({ "workbench.editor.showTabs": "none" })json"));

	const auto started = fixture.runtime->Start();
	ASSERT_TRUE(started.IsUsable());
	EXPECT_EQ(EWorkspaceKind::Folder, fixture.runtime->WorkspaceContext().Snapshot().kind);
	ConfigurationTarget target = ProfileTarget(fixture.runtime->Bootstrap());
	target.workspaceUri = folder;
	target.folderUri = folder;
	EXPECT_EQ(L"none", ShowTabs(*fixture.runtime, target));

	const auto reads = fixture.files->Reads();
	EXPECT_TRUE(ContainsRead(reads, L"file:///C:/Repo/.vscode/settings.json"));
	EXPECT_GE(reads.size(), 2U);
}

TEST(CWorkbenchRuntime, ZeroBootstrapFolderWorkspaceLoadsDocumentFoldersAndKeepsNonSettingsTyped)
{
	auto workspaceConfig = Parse(L"file:///C:/Work/demo.code-workspace");
	auto first = Parse(L"file:///C:/One");
	auto second = Parse(L"file:///C:/Two");
	RuntimeFixture fixture(WorkspaceBootstrap(workspaceConfig, {}));
	fixture.files->Set(workspaceConfig, Bytes(R"json({
		"folders": [{ "path": "../One", "name": "one" }, { "uri": "file:///C:/Two", "name": "two" }],
		"settings": { "workbench.editor.showTabs": "single" },
		"tasks": { "version": "2.0.0" }, "launch": { "version": "0.2.0" }, "extensions": { "recommendations": [] }
	})json"));
	fixture.files->Set(Parse(L"file:///C:/One/.vscode/settings.json"),
		Bytes(R"json({ "workbench.editor.showTabs": "none" })json"));
	fixture.files->Set(Parse(L"file:///C:/Two/.vscode/settings.json"),
		Bytes(R"json({ "workbench.editor.showTabs": "none" })json"));

	const auto started = fixture.runtime->Start();
	ASSERT_EQ(EWorkbenchRuntimeResultCode::Ready, started.code);
	const auto context = fixture.runtime->WorkspaceContext().Snapshot();
	ASSERT_EQ(EWorkspaceKind::Workspace, context.kind);
	ASSERT_EQ(2U, context.folders.size());
	EXPECT_TRUE(UriIdentityService::IsEqual(first, context.folders.front().uri));
	const auto workspaceState = fixture.runtime->WorkspaceConfiguration();
	ASSERT_TRUE(workspaceState.document.has_value());
	ASSERT_TRUE(workspaceState.document->settings.has_value());
	EXPECT_EQ(2U, workspaceState.document->fileMembers.size());
	ASSERT_EQ(2U, workspaceState.folderResources.size());
	EXPECT_EQ(3U, workspaceState.folderResources.front().resources.size());

	ConfigurationTarget firstTarget = ProfileTarget(fixture.runtime->Bootstrap());
	firstTarget.workspaceUri = workspaceConfig;
	firstTarget.folderUri = first;
	ConfigurationTarget secondTarget = ProfileTarget(fixture.runtime->Bootstrap());
	secondTarget.workspaceUri = workspaceConfig;
	secondTarget.folderUri = second;
	EXPECT_EQ(L"none", ShowTabs(*fixture.runtime, firstTarget));
	EXPECT_EQ(L"none", ShowTabs(*fixture.runtime, secondTarget));
	ConfigurationTarget workspaceTarget = ProfileTarget(fixture.runtime->Bootstrap());
	workspaceTarget.workspaceUri = workspaceConfig;
	EXPECT_EQ(L"single", ShowTabs(*fixture.runtime, workspaceTarget));

	const auto reads = fixture.files->Reads();
	EXPECT_TRUE(ContainsRead(reads, workspaceConfig.ToString()));
	EXPECT_TRUE(ContainsRead(reads, L"file:///C:/One/.vscode/settings.json"));
	EXPECT_TRUE(ContainsRead(reads, L"file:///C:/Two/.vscode/settings.json"));
}

TEST(CWorkbenchRuntime, AddFolderAcceptsTheExactCasDocumentSynchronouslyAndDoesNotDoubleReload)
{
	auto workspaceConfig = Parse(L"file:///C:/Work/demo.code-workspace");
	auto first = Parse(L"file:///C:/One");
	auto added = Parse(L"file:///C:/Added");
	RuntimeFixture fixture(WorkspaceBootstrap(workspaceConfig, { { first, L"one" } }));
	fixture.files->Set(workspaceConfig, Bytes(R"json({
		"folders": [{ "uri": "file:///C:/One", "name": "one" }],
		"settings": { "workbench.editor.showTabs": "single" },
		"unknown": { "keep": true }
	})json"));
	ASSERT_TRUE(fixture.runtime->Start().IsUsable());
	const auto readsBefore = fixture.files->Reads();
	const auto workspaceReadsBefore = std::ranges::count(readsBefore, workspaceConfig.ToString());

	const auto edited = fixture.runtime->ReplaceCurrentWorkspaceFolders({
		workspaceConfig,
		workspaceConfig,
		{
			{ first, L"one" },
			{ added, std::nullopt },
		},
	});
	ASSERT_EQ(workbench::workspace::EWorkspaceEditingOutcome::Succeeded, edited.outcome);
	ASSERT_TRUE(edited.committedVersion.has_value());
	ASSERT_TRUE(edited.committedDocument.has_value());

	const auto context = fixture.runtime->WorkspaceContext().Snapshot();
	ASSERT_EQ(EWorkspaceKind::Workspace, context.kind);
	ASSERT_EQ(2U, context.folders.size());
	EXPECT_TRUE(UriIdentityService::IsEqual(first, context.folders[0].uri));
	EXPECT_TRUE(UriIdentityService::IsEqual(added, context.folders[1].uri));
	EXPECT_EQ(L"Added", context.folders[1].displayName);
	const auto accepted = fixture.runtime->WorkspaceConfiguration();
	ASSERT_TRUE(accepted.document.has_value());
	ASSERT_EQ(2U, accepted.document->folders.size());
	EXPECT_TRUE(accepted.document->settings.has_value());

	const auto readsAfter = fixture.files->Reads();
	const auto workspaceReadsAfter = std::ranges::count(readsAfter, workspaceConfig.ToString());
	// One versioned edit read plus at most one typed-artifact refresh. An
	// ordinary listener reload would add a third workspace-document read.
	EXPECT_LE(workspaceReadsAfter - workspaceReadsBefore, 2);
}

TEST(CWorkbenchRuntime, AddFolderCasConflictKeepsTheLastAcceptedSemanticWorkspace)
{
	auto workspaceConfig = Parse(L"file:///C:/Work/demo.code-workspace");
	auto first = Parse(L"file:///C:/One");
	auto added = Parse(L"file:///C:/Added");
	RuntimeFixture fixture(WorkspaceBootstrap(workspaceConfig, { { first, L"one" } }));
	fixture.files->Set(workspaceConfig, Bytes(
		R"json({ "folders": [{ "uri": "file:///C:/One", "name": "one" }] })json"));
	ASSERT_TRUE(fixture.runtime->Start().IsUsable());
	const auto before = fixture.runtime->WorkspaceContext().Snapshot();
	fixture.files->ConflictNextConditionalReplace();

	const auto edited = fixture.runtime->ReplaceCurrentWorkspaceFolders({
		workspaceConfig,
		workspaceConfig,
		{ { first, L"one" }, { added, std::nullopt } },
	});
	EXPECT_EQ(workbench::workspace::EWorkspaceEditingOutcome::Failed, edited.outcome);
	const auto after = fixture.runtime->WorkspaceContext().Snapshot();
	ASSERT_EQ(before.folders.size(), after.folders.size());
	ASSERT_EQ(1U, after.folders.size());
	EXPECT_TRUE(UriIdentityService::IsEqual(first, after.folders.front().uri));
	const auto accepted = fixture.runtime->WorkspaceConfiguration();
	ASSERT_TRUE(accepted.document.has_value());
	ASSERT_EQ(1U, accepted.document->folders.size());
}

TEST(CWorkbenchRuntime, WorkspaceArtifactsPreferFolderDocumentsAndNeverBecomeConfiguration)
{
	auto workspaceConfig = Parse(L"file:///C:/Work/artifacts.code-workspace");
	auto folder = Parse(L"file:///C:/Repo");
	auto folderTasks = Parse(L"file:///C:/Repo/.vscode/tasks.json");
	RuntimeFixture fixture(WorkspaceBootstrap(workspaceConfig, { { folder, L"repo" } }));
	fixture.files->Set(workspaceConfig, Bytes(R"json({
		"folders": [{ "uri": "file:///C:/Repo", "name": "repo" }],
		"tasks": { "version": "2.0.0", "tasks": [{ "label": "workspace" }] }
	})json"));
	fixture.files->Set(folderTasks, Bytes(R"json({ "version": "2.0.0", "tasks": [{ "label": "folder" }] })json"));

	ASSERT_TRUE(fixture.runtime->Start().IsUsable());
	const auto fallback = fixture.runtime->WorkspaceArtifacts().Tasks();
	ASSERT_TRUE(fallback.document.has_value());
	EXPECT_EQ(workspaceConfig.ToString(), fallback.document->resource.ToString());
	const auto overridden = fixture.runtime->WorkspaceArtifacts().Tasks(folder);
	ASSERT_TRUE(overridden.document.has_value());
	EXPECT_EQ(folderTasks.ToString(), overridden.document->resource.ToString());

	ConfigurationTarget target = ProfileTarget(fixture.runtime->Bootstrap());
	target.workspaceUri = workspaceConfig;
	const auto configurationTask = fixture.runtime->Configuration().GetValue("tasks", target);
	EXPECT_EQ(EConfigurationOutcome::InvalidKey, configurationTask.outcome);
	EXPECT_FALSE(configurationTask.value.has_value());
}

TEST(CWorkbenchRuntime, ArtifactWatchReloadRetainsLastGoodDocumentAfterInvalidContent)
{
	auto folder = Parse(L"file:///C:/Repo");
	auto tasks = Parse(L"file:///C:/Repo/.vscode/tasks.json");
	RuntimeFixture fixture(Bootstrap(folder));
	fixture.files->Set(tasks, Bytes(R"json({ "version": "2.0.0", "tasks": [{ "label": "good" }] })json"));
	fixture.files->EnableWatches();
	ASSERT_TRUE(fixture.runtime->Start().IsUsable());
	const auto accepted = fixture.runtime->WorkspaceArtifacts().Tasks(folder);
	ASSERT_TRUE(accepted.document.has_value());
	const auto readsBeforeChange = fixture.files->Reads().size();

	fixture.files->Set(tasks, Bytes(R"json({ "tasks": [)json"));
	fixture.files->EmitFirstWatchEvent({ .type = EFileWatchEventType::Changed, .uri = tasks });
	ASSERT_TRUE(fixture.files->WaitUntilReadCount(readsBeforeChange + 1U));
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (fixture.runtime->Snapshot().state != EWorkbenchRuntimeState::ReadyWithDiagnostics
		&& std::chrono::steady_clock::now() < deadline) {
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	const auto retained = fixture.runtime->WorkspaceArtifacts().Tasks(folder);
	ASSERT_TRUE(retained.document.has_value());
	EXPECT_EQ(accepted.document->rawJsonc, retained.document->rawJsonc);
	const auto snapshot = fixture.runtime->Snapshot();
	EXPECT_TRUE(std::any_of(snapshot.diagnostics.begin(), snapshot.diagnostics.end(), [](const auto& diagnostic) {
		return diagnostic.source == workbench::EWorkbenchRuntimeDiagnosticSource::WorkspaceArtifacts
			&& diagnostic.code == EWorkbenchRuntimeDiagnosticCode::ParseFailed
			&& diagnostic.message.find("C:/") == std::string::npos;
	}));
}

TEST(CWorkbenchRuntime, ArtifactTopologyTransitionsClearDocumentsAndAdvanceSemanticGeneration)
{
	RuntimeFixture fixture(Bootstrap());
	ASSERT_TRUE(fixture.runtime->Start().IsUsable());
	// Trust is context state, so those resolutions advance the semantic revision on
	// their own. This test therefore pins the artifact-to-revision relationship and
	// the direction of travel, never a literal revision count, and it passes no
	// expectedRevision because a trust commit may legitimately land between reading
	// a revision and using it.
	const auto settled = [&fixture] {
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
		while (std::chrono::steady_clock::now() < deadline) {
			const auto revision = fixture.runtime->WorkspaceContext().Snapshot().revision;
			if (fixture.runtime->WorkspaceArtifacts().Snapshot().generation == revision + 1U) return true;
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		return false;
	};
	ASSERT_TRUE(settled());
	const auto startedGeneration = fixture.runtime->WorkspaceArtifacts().Snapshot().generation;

	auto folder = Parse(L"file:///C:/Artifacts");
	const auto opened = fixture.runtime->WorkspaceContext().SetFolder({
		.operation = { .operationId = "test.artifacts.open" },
		.folderUri = folder, .displayName = L"Artifacts",
	});
	ASSERT_EQ(EWorkspaceContextOutcome::Succeeded, opened.outcome);
	ASSERT_TRUE(settled());
	const auto openedGeneration = fixture.runtime->WorkspaceArtifacts().Snapshot().generation;
	EXPECT_LT(startedGeneration, openedGeneration);

	const auto emptied = fixture.runtime->WorkspaceContext().SetEmpty({
		.operationId = "test.artifacts.empty",
	});
	ASSERT_EQ(EWorkspaceContextOutcome::Succeeded, emptied.outcome);
	ASSERT_TRUE(settled());
	EXPECT_LT(openedGeneration, fixture.runtime->WorkspaceArtifacts().Snapshot().generation);
	EXPECT_FALSE(fixture.runtime->WorkspaceArtifacts().Tasks(folder).document.has_value());
}

TEST(CWorkbenchRuntime, StopJoinsArtifactSourcesBeforeStoppingTheOwnedService)
{
	auto folder = Parse(L"file:///C:/Repo");
	RuntimeFixture fixture(Bootstrap(folder));
	fixture.files->EnableWatches();
	ASSERT_TRUE(fixture.runtime->Start().IsUsable());
	EXPECT_FALSE(fixture.runtime->WorkspaceArtifacts().Snapshot().stopped);
	EXPECT_EQ(EWorkbenchRuntimeResultCode::Stopped, fixture.runtime->Stop().code);
	const auto artifacts = fixture.runtime->WorkspaceArtifacts().Snapshot();
	EXPECT_TRUE(artifacts.stopped);
	EXPECT_EQ(0U, artifacts.acceptedDocuments);
}

TEST(CWorkbenchRuntime, MalformedWorkspaceReloadRetainsTheLastAcceptedWorkspaceSettings)
{
	auto workspaceConfig = Parse(L"file:///C:/Work/retained.code-workspace");
	auto first = Parse(L"file:///C:/One");
	auto second = Parse(L"file:///C:/Two");
	RuntimeFixture fixture(WorkspaceBootstrap(workspaceConfig, {}));
	fixture.files->Set(workspaceConfig,
		Bytes(R"json({ "folders": [{ "uri": "file:///C:/One", "name": "one" }], "settings": { "workbench.editor.showTabs": "single" } })json"));
	ASSERT_TRUE(fixture.runtime->Start().IsUsable());
	fixture.files->Set(workspaceConfig, Bytes(R"json({ "folders": [)json"));
	const auto changed = fixture.runtime->WorkspaceContext().SetWorkspace({
		.operation = { .operationId = "test.workspace.malformed-reload", .expectedRevision = fixture.runtime->WorkspaceContext().Snapshot().revision },
		.workspaceConfigUri = workspaceConfig,
		.folders = { { second, L"two" } },
	});
	ASSERT_EQ(EWorkspaceContextOutcome::Succeeded, changed.outcome);
	const auto restoredContext = fixture.runtime->WorkspaceContext().Snapshot();
	ASSERT_EQ(1U, restoredContext.folders.size());
	EXPECT_TRUE(UriIdentityService::IsEqual(first, restoredContext.folders.front().uri));
	ConfigurationTarget target = ProfileTarget(fixture.runtime->Bootstrap());
	target.workspaceUri = workspaceConfig;
	EXPECT_EQ(L"single", ShowTabs(*fixture.runtime, target));
	const auto snapshot = fixture.runtime->Snapshot();
	EXPECT_EQ(EWorkbenchRuntimeState::ReadyWithDiagnostics, snapshot.state);
	EXPECT_TRUE(std::any_of(snapshot.diagnostics.begin(), snapshot.diagnostics.end(), [](const auto& diagnostic) {
		return diagnostic.code == EWorkbenchRuntimeDiagnosticCode::ParseFailed
			&& diagnostic.message.find("C:/") == std::string::npos;
	}));
}

TEST(CWorkbenchRuntime, LeavingWorkspaceClearsTheWorkspaceSettingsSource)
{
	auto workspaceConfig = Parse(L"file:///C:/Work/leave.code-workspace");
	RuntimeFixture fixture(WorkspaceBootstrap(workspaceConfig, {}));
	fixture.files->Set(workspaceConfig,
		Bytes(R"json({ "settings": { "workbench.editor.showTabs": "none" } })json"));
	ASSERT_TRUE(fixture.runtime->Start().IsUsable());
	ConfigurationTarget target = ProfileTarget(fixture.runtime->Bootstrap());
	target.workspaceUri = workspaceConfig;
	EXPECT_EQ(L"none", ShowTabs(*fixture.runtime, target));
	const auto emptied = fixture.runtime->WorkspaceContext().SetEmpty({
		.operationId = "test.workspace.leave",
		.expectedRevision = fixture.runtime->WorkspaceContext().Snapshot().revision,
	});
	ASSERT_EQ(EWorkspaceContextOutcome::Succeeded, emptied.outcome);
	EXPECT_EQ(L"multiple", ShowTabs(*fixture.runtime, target));
}

TEST(CWorkbenchRuntime, DuplicateWorkspaceFoldersWarnOnceWithoutARefreshLoop)
{
	auto workspaceConfig = Parse(L"file:///C:/Work/duplicate.code-workspace");
	auto first = Parse(L"file:///C:/One");
	RuntimeFixture fixture(WorkspaceBootstrap(workspaceConfig, {}));
	fixture.files->Set(workspaceConfig,
		Bytes(R"json({ "folders": [{ "uri": "file:///C:/One", "name": "first" }, { "uri": "file:///C:/One", "name": "second" }] })json"));
	ASSERT_EQ(EWorkbenchRuntimeResultCode::ReadyWithDiagnostics, fixture.runtime->Start().code);
	const auto context = fixture.runtime->WorkspaceContext().Snapshot();
	ASSERT_EQ(1U, context.folders.size());
	EXPECT_EQ(L"first", context.folders.front().displayName);
	const auto reads = fixture.files->Reads();
	EXPECT_GE(std::count(reads.begin(), reads.end(), workspaceConfig.ToString()), 1U);
	EXPECT_EQ(1U, std::count(reads.begin(), reads.end(), Parse(L"file:///C:/One/.vscode/settings.json").ToString()));
	const auto snapshot = fixture.runtime->Snapshot();
	EXPECT_TRUE(std::any_of(snapshot.diagnostics.begin(), snapshot.diagnostics.end(), [](const auto& diagnostic) {
		return diagnostic.code == EWorkbenchRuntimeDiagnosticCode::WorkspaceFolderDuplicate;
	}));
}

TEST(CWorkbenchRuntime, ParseFailureIsUsableAndRetainsATypedPathFreeDiagnostic)
{
	RuntimeFixture fixture(Bootstrap());
	fixture.files->Set(fixture.runtime->Bootstrap().UserDataProfile().Resources().Settings(),
		Bytes(R"json({ "workbench.editor.showTabs": "single", "workbench.editor.showTabs": "none" })json"));

	const auto started = fixture.runtime->Start();
	EXPECT_EQ(EWorkbenchRuntimeResultCode::ReadyWithDiagnostics, started.code);
	EXPECT_TRUE(started.IsUsable());
	EXPECT_EQ(EWorkbenchRuntimeState::ReadyWithDiagnostics, started.snapshot.state);
	ASSERT_EQ(1U, started.snapshot.diagnostics.size());
	EXPECT_EQ(EWorkbenchRuntimeDiagnosticCode::ParseFailed, started.snapshot.diagnostics.front().code);
	EXPECT_EQ(std::string::npos, started.snapshot.diagnostics.front().message.find("C:/"));
	EXPECT_EQ(L"multiple", ShowTabs(*fixture.runtime, ProfileTarget(fixture.runtime->Bootstrap())));
}

TEST(CWorkbenchRuntime, WorkspaceTransitionsReplaceSourcesAndStopDisposesRefreshCallbacks)
{
	RuntimeFixture fixture(Bootstrap());
	auto first = Parse(L"file:///C:/First");
	auto second = Parse(L"file:///C:/Second");
	fixture.files->Set(Parse(L"file:///C:/First/.vscode/settings.json"),
		Bytes(R"json({ "workbench.editor.showTabs": "none" })json"));
	fixture.files->Set(Parse(L"file:///C:/Second/.vscode/settings.json"),
		Bytes(R"json({ "workbench.editor.showTabs": "single" })json"));
	ASSERT_TRUE(fixture.runtime->Start().IsUsable());

	auto opened = fixture.runtime->SwitchToFolderWorkspace(first, L"First");
	ASSERT_EQ(EWorkspaceContextOutcome::Succeeded, opened.outcome);
	ConfigurationTarget firstTarget = ProfileTarget(fixture.runtime->Bootstrap());
	firstTarget.workspaceUri = first;
	firstTarget.folderUri = first;
	EXPECT_EQ(L"none", ShowTabs(*fixture.runtime, firstTarget));

	auto emptied = fixture.runtime->WorkspaceContext().SetEmpty(
		{ .operationId = "test.empty", .expectedRevision = opened.revision });
	ASSERT_EQ(EWorkspaceContextOutcome::Succeeded, emptied.outcome);
	EXPECT_EQ(L"multiple", ShowTabs(*fixture.runtime, firstTarget));

	const auto readsBeforeStop = fixture.files->Reads().size();
	const auto stopped = fixture.runtime->Stop();
	EXPECT_EQ(EWorkbenchRuntimeResultCode::Stopped, stopped.code);
	EXPECT_EQ(EWorkbenchRuntimeState::Stopped, stopped.snapshot.state);
	auto afterStop = fixture.runtime->SwitchToFolderWorkspace(second, L"Second");
	ASSERT_EQ(EWorkspaceContextOutcome::Failed, afterStop.outcome);
	EXPECT_EQ(EWorkspaceKind::Empty, afterStop.snapshot.kind);
	EXPECT_EQ(readsBeforeStop, fixture.files->Reads().size());
	EXPECT_EQ(EWorkbenchRuntimeResultCode::Stopped, fixture.runtime->Start().code);
}

TEST(CWorkbenchRuntime, WorkspaceFolderDocumentsKeepStableControllerIdentityAcrossMembershipTransitions)
{
	auto workspaceConfig = Parse(L"file:///C:/Work/stable-folders.code-workspace");
	auto first = Parse(L"file:///C:/One");
	auto second = Parse(L"file:///C:/Two");
	RuntimeFixture fixture(WorkspaceBootstrap(workspaceConfig, { { first, L"one" } }));
	fixture.files->Set(workspaceConfig,
		Bytes(R"json({ "folders": [{ "uri": "file:///C:/One", "name": "one" }] })json"));
	fixture.files->Set(Parse(L"file:///C:/One/.vscode/settings.json"),
		Bytes(R"json({ "workbench.editor.showTabs": "none" })json"));
	fixture.files->Set(Parse(L"file:///C:/Two/.vscode/settings.json"),
		Bytes(R"json({ "workbench.editor.showTabs": "single" })json"));
	ASSERT_TRUE(fixture.runtime->Start().IsUsable());

	auto replaceFolders = [&](std::vector<workbench::workspace::WorkspaceFolderEdit> folders) {
		return fixture.runtime->ReplaceCurrentWorkspaceFolders({
			.source = workspaceConfig,
			.target = workspaceConfig,
			.folders = std::move(folders),
		});
	};
	ASSERT_EQ(workbench::workspace::EWorkspaceEditingOutcome::Succeeded,
		replaceFolders({ { first, L"one" }, { second, L"two" } }).outcome);
	ASSERT_EQ(workbench::workspace::EWorkspaceEditingOutcome::Succeeded,
		replaceFolders({ { second, L"two" }, { first, L"one" } }).outcome);

	ConfigurationTarget firstTarget = ProfileTarget(fixture.runtime->Bootstrap());
	firstTarget.workspaceUri = workspaceConfig;
	firstTarget.folderUri = first;
	const auto concurrentSourceUpdate = fixture.runtime->Configuration().ReplaceSource({
		.source = { EConfigurationScope::Folder, firstTarget, "workspace.folder.settings", 0 },
		.entries = { { "workbench.editor.showTabs", ConfigurationValue(L"multiple") } },
		.operationId = "test.workspace.concurrent-folder-source-update",
	});
	ASSERT_EQ(EConfigurationOutcome::Applied, concurrentSourceUpdate.outcome);
	EXPECT_EQ(L"multiple", ShowTabs(*fixture.runtime, firstTarget));

	fixture.files->Set(Parse(L"file:///C:/One/.vscode/settings.json"),
		Bytes(R"json({ "workbench.editor.showTabs": "single" })json"));
	fixture.files->Set(workspaceConfig, Bytes(R"json({ "folders": [
		{ "uri": "file:///C:/One", "name": "one" }, { "uri": "file:///C:/Two", "name": "two" }
	] })json"));
	const auto beforeReorder = fixture.runtime->WorkspaceContext().Snapshot();
	const auto reorderedBack = fixture.runtime->WorkspaceContext().SetWorkspace({
		.operation = { .operationId = "test.workspace.reorder-back",
			.expectedRevision = beforeReorder.revision },
		.workspaceConfigUri = workspaceConfig,
		.folders = { { first, L"one" }, { second, L"two" } },
	});
	ASSERT_EQ(EWorkspaceContextOutcome::Succeeded, reorderedBack.outcome);

	// The retained controller remembers the revision accepted before the direct
	// source update, detects the stale CAS on reload, and leaves that newer value
	// intact. Recreating the controller would lose the expected revision and
	// incorrectly overwrite "multiple" with the file's "single" value.
	EXPECT_EQ(L"multiple", ShowTabs(*fixture.runtime, firstTarget));
	// A concurrent notification drainer may already own delivery when the
	// reorder is committed. SetWorkspace then returns after enqueueing this
	// revision, so wait for the bounded asynchronous terminal observation
	// instead of sampling the intermediate Ready state.
	const auto diagnosticDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (fixture.runtime->Snapshot().state != EWorkbenchRuntimeState::ReadyWithDiagnostics
		&& std::chrono::steady_clock::now() < diagnosticDeadline) {
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	const auto snapshot = fixture.runtime->Snapshot();
	EXPECT_EQ(EWorkbenchRuntimeState::ReadyWithDiagnostics, snapshot.state);
	EXPECT_TRUE(std::ranges::any_of(snapshot.diagnostics, [](const auto& diagnostic) {
		return diagnostic.source == EWorkbenchRuntimeDiagnosticSource::WorkspaceSettings
			&& diagnostic.code == EWorkbenchRuntimeDiagnosticCode::ApplyFailed
			&& diagnostic.message == "configuration document changed concurrently";
	}));
}

TEST(CWorkbenchRuntime, DifferentUnreadableWorkspaceClearsPreviousWorkspaceAndFolderSources)
{
	auto previousConfig = Parse(L"file:///C:/Work/previous.code-workspace");
	auto replacementConfig = Parse(L"file:///C:/Work/replacement.code-workspace");
	auto first = Parse(L"file:///C:/One");
	RuntimeFixture fixture(WorkspaceBootstrap(previousConfig, { { first, L"one" } }));
	fixture.files->Set(previousConfig, Bytes(R"json({
		"folders": [{ "uri": "file:///C:/One", "name": "one" }],
		"settings": { "workbench.editor.showTabs": "none" }
	})json"));
	fixture.files->Set(Parse(L"file:///C:/One/.vscode/settings.json"),
		Bytes(R"json({ "workbench.editor.showTabs": "single" })json"));
	ASSERT_TRUE(fixture.runtime->Start().IsUsable());
	ConfigurationTarget previousWorkspace = ProfileTarget(fixture.runtime->Bootstrap());
	previousWorkspace.workspaceUri = previousConfig;
	ConfigurationTarget previousFolder = previousWorkspace;
	previousFolder.folderUri = first;
	EXPECT_EQ(L"none", ShowTabs(*fixture.runtime, previousWorkspace));
	EXPECT_EQ(L"single", ShowTabs(*fixture.runtime, previousFolder));

	fixture.files->Set(replacementConfig, Bytes(R"json({ "folders": [)json"));
	const auto changed = fixture.runtime->WorkspaceContext().SetWorkspace({
		.operation = { .operationId = "test.workspace.unreadable-replacement",
			.expectedRevision = fixture.runtime->WorkspaceContext().Snapshot().revision },
		.workspaceConfigUri = replacementConfig, .folders = {},
	});
	ASSERT_EQ(EWorkspaceContextOutcome::Succeeded, changed.outcome);
	EXPECT_EQ(L"multiple", ShowTabs(*fixture.runtime, previousWorkspace));
	EXPECT_EQ(L"multiple", ShowTabs(*fixture.runtime, previousFolder));
	EXPECT_FALSE(fixture.runtime->WorkspaceConfiguration().resource.has_value());
}

TEST(CWorkbenchRuntime, ReentrantStartReturnsBusyBeforeItCanDuplicateBootstrapSideEffects)
{
	RuntimeFixture fixture(Bootstrap());
	std::optional<workbench::WorkbenchRuntimeResult> reentrant;
	fixture.files->onRead = [&] {
		reentrant = fixture.runtime->Start();
		fixture.files->onRead = {};
	};

	EXPECT_EQ(EWorkbenchRuntimeResultCode::Ready, fixture.runtime->Start().code);
	ASSERT_TRUE(reentrant.has_value());
	EXPECT_EQ(EWorkbenchRuntimeResultCode::Busy, reentrant->code);
	EXPECT_EQ(1U, fixture.files->Reads().size());
}

TEST(CWorkbenchRuntime, StopRequestCancelsABlockedFolderReadBeforeConfigurationApply)
{
	RuntimeFixture fixture(Bootstrap());
	ASSERT_TRUE(fixture.runtime->Start().IsUsable());
	auto folder = Parse(L"file:///C:/Blocked");
	auto settings = Parse(L"file:///C:/Blocked/.vscode/settings.json");
	fixture.files->Set(settings, Bytes(R"json({ "workbench.editor.showTabs": "none" })json"));
	fixture.files->BlockRead(settings);
	std::optional<workbench::WorkbenchRuntimeResult> requestedStop;
	fixture.files->onRead = [&] { requestedStop = fixture.runtime->Stop(); };
	std::thread transition([&] {
		const auto changed = fixture.runtime->WorkspaceContext().SetFolder({
			.operation = { .operationId = "test.workspace.stop-during-read",
				.expectedRevision = fixture.runtime->WorkspaceContext().Snapshot().revision },
			.folderUri = folder, .displayName = L"blocked",
		});
		EXPECT_EQ(EWorkspaceContextOutcome::Succeeded, changed.outcome);
	});
	fixture.files->WaitUntilReadIsBlocked();
	fixture.files->ReleaseBlockedRead();
	transition.join();

	ASSERT_TRUE(requestedStop.has_value());
	EXPECT_EQ(EWorkbenchRuntimeResultCode::Busy, requestedStop->code);
	EXPECT_EQ(EWorkbenchRuntimeState::Stopped, fixture.runtime->Snapshot().state);
	ConfigurationTarget target = ProfileTarget(fixture.runtime->Bootstrap());
	target.workspaceUri = folder;
	target.folderUri = folder;
	EXPECT_EQ(L"multiple", ShowTabs(*fixture.runtime, target));
}

TEST(CWorkbenchRuntime, RestoresAndImmediatelyPersistsProfileStatusbarVisibility)
{
	auto store = std::make_unique<FakeStatusbarVisibilityMementoStore>();
	store->loadResult = {
		statusbar::EStatusbarMementoLoadStatus::Loaded,
		std::vector<std::string>{ "status.editor.eol" },
		{}
	};
	RuntimeFixture fixture(Bootstrap(), {}, {}, std::move(store));

	ASSERT_EQ(EWorkbenchRuntimeResultCode::Ready, fixture.runtime->Start().code);
	EXPECT_EQ(1U, fixture.statusbarStore->loadCalls);
	EXPECT_FALSE(fixture.runtime->StatusbarState().IsVisible("status.editor.eol", true));
	ASSERT_TRUE(fixture.runtime->StatusbarState().SetHidden("status.editor.encoding", true));

	const auto persisted = fixture.runtime->PersistStatusbarVisibility();
	EXPECT_EQ(statusbar::EStatusbarMementoSaveStatus::Persisted, persisted.status);
	ASSERT_EQ(1U, fixture.statusbarStore->saveCalls);
	EXPECT_EQ((std::vector<std::string>{ "status.editor.encoding", "status.editor.eol" }),
		fixture.statusbarStore->lastSavedHiddenIds);
}

TEST(CWorkbenchRuntime, RestoresAValidLayoutBeforeReadyAndSkipsAnUnchangedShutdownWrite)
{
	auto store = std::make_unique<FakeLayoutMementoStore>();
	store->loadResult = {
		layout::EWorkbenchLayoutMementoLoadStatus::Loaded, VisiblePanelMemento(), {}
	};
	RuntimeFixture fixture(Bootstrap(), std::move(store));

	const auto started = fixture.runtime->Start();
	ASSERT_EQ(EWorkbenchRuntimeResultCode::Ready, started.code);
	EXPECT_TRUE(Part(fixture.runtime->LayoutState().Snapshot(), layout::ids::part::Panel).visible);
	EXPECT_EQ(1U, fixture.layoutStore->loadCalls);

	EXPECT_EQ(EWorkbenchRuntimeResultCode::Stopped, fixture.runtime->Stop().code);
	EXPECT_EQ(0U, fixture.layoutStore->saveCalls);
}

TEST(CWorkbenchRuntime, PersistsOneDirtyLayoutSnapshotOnTheFirstOrderlyStop)
{
	auto store = std::make_unique<FakeLayoutMementoStore>();
	RuntimeFixture fixture(Bootstrap(), std::move(store));
	ASSERT_EQ(EWorkbenchRuntimeResultCode::Ready, fixture.runtime->Start().code);
	ASSERT_EQ(layout::EWorkbenchLayoutOperationStatus::Succeeded,
		fixture.runtime->LayoutState().SetPartVisibility({
			.operation = { .operationId = "test.runtime.show-panel" },
			.partId = std::string(layout::ids::part::Panel),
			.visible = true,
		}).status);

	const auto stopped = fixture.runtime->Stop();
	EXPECT_EQ(EWorkbenchRuntimeResultCode::Stopped, stopped.code);
	ASSERT_EQ(1U, fixture.layoutStore->saveCalls);
	ASSERT_TRUE(fixture.layoutStore->lastSavedSnapshot.has_value());
	EXPECT_TRUE(Part(*fixture.layoutStore->lastSavedSnapshot, layout::ids::part::Panel).visible);
	EXPECT_EQ(EWorkbenchRuntimeResultCode::Stopped, fixture.runtime->Stop().code);
	EXPECT_EQ(1U, fixture.layoutStore->saveCalls);
}

TEST(CWorkbenchRuntime, PreservesAnInvalidStoredMementoAndNeverOverwritesIt)
{
	auto store = std::make_unique<FakeLayoutMementoStore>();
	store->loadResult = {
		layout::EWorkbenchLayoutMementoLoadStatus::InvalidStoredMemento, std::nullopt,
		L"invalid durable value"
	};
	RuntimeFixture fixture(Bootstrap(), std::move(store));

	const auto started = fixture.runtime->Start();
	ASSERT_EQ(EWorkbenchRuntimeResultCode::ReadyWithDiagnostics, started.code);
	ASSERT_EQ(1U, started.snapshot.diagnostics.size());
	EXPECT_EQ(EWorkbenchRuntimeDiagnosticCode::LayoutRestoreFailed,
		started.snapshot.diagnostics.front().code);
	EXPECT_FALSE(Part(fixture.runtime->LayoutState().Snapshot(), layout::ids::part::Panel).visible);
	ASSERT_EQ(layout::EWorkbenchLayoutOperationStatus::Succeeded,
		fixture.runtime->LayoutState().SetPartVisibility({
			.operation = { .operationId = "test.runtime.invalid-show-panel" },
			.partId = std::string(layout::ids::part::Panel),
			.visible = true,
		}).status);

	EXPECT_EQ(EWorkbenchRuntimeResultCode::Stopped, fixture.runtime->Stop().code);
	EXPECT_EQ(0U, fixture.layoutStore->saveCalls);
}

TEST(CWorkbenchRuntime, ReportsAStorageConflictFromTheTerminalStoppedSnapshot)
{
	auto store = std::make_unique<FakeLayoutMementoStore>();
	store->saveResult = {
		layout::EWorkbenchLayoutMementoSaveStatus::Conflict, L"remote state won"
	};
	RuntimeFixture fixture(Bootstrap(), std::move(store));
	ASSERT_EQ(EWorkbenchRuntimeResultCode::Ready, fixture.runtime->Start().code);
	ASSERT_EQ(layout::EWorkbenchLayoutOperationStatus::Succeeded,
		fixture.runtime->LayoutState().SetPartVisibility({
			.operation = { .operationId = "test.runtime.conflict-show-panel" },
			.partId = std::string(layout::ids::part::Panel),
			.visible = true,
		}).status);

	const auto stopped = fixture.runtime->Stop();
	EXPECT_EQ(EWorkbenchRuntimeState::Stopped, stopped.snapshot.state);
	ASSERT_EQ(1U, fixture.layoutStore->saveCalls);
	ASSERT_EQ(1U, stopped.snapshot.diagnostics.size());
	EXPECT_EQ(EWorkbenchRuntimeDiagnosticCode::LayoutPersistenceConflict,
		stopped.snapshot.diagnostics.front().code);
}

TEST(CWorkbenchRuntime, StartAndStopAreIdempotentTerminalOperations)
{
	RuntimeFixture fixture(Bootstrap());
	EXPECT_EQ(EWorkbenchRuntimeResultCode::Ready, fixture.runtime->Start().code);
	EXPECT_EQ(EWorkbenchRuntimeResultCode::AlreadyReady, fixture.runtime->Start().code);
	EXPECT_EQ(EWorkbenchRuntimeResultCode::Stopped, fixture.runtime->Stop().code);
	EXPECT_EQ(EWorkbenchRuntimeResultCode::Stopped, fixture.runtime->Stop().code);
	EXPECT_EQ(EWorkbenchRuntimeState::Stopped, fixture.runtime->Snapshot().state);
}

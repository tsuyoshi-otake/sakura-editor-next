/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/

// Tests for the workspace-trust prompt wiring in CWorkbenchRuntime: how it
// restores/saves the per-workspace WorkspaceTrustMemento, how it turns that
// state plus the security.workspace.trust.* settings into a startup-prompt
// decision, how RecordWorkspaceTrustStartupPromptShown/RecordUntrustedFilesAccepted
// persist, and how WorkspaceTrustUntrustedFiles resolves.
//
// This file intentionally does not reuse RuntimeFixture from
// CWorkbenchRuntimeTest.cpp: that fixture has no way to inject a
// config::IWorkspaceTrustMementoStore, and CWorkbenchRuntimeTest.cpp is off
// limits for this change. The bootstrap/fake helpers below are therefore
// duplicated from that file's anonymous-namespace idioms rather than shared,
// since they are TU-local there.

#include "pch.h"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <sakura/filesystem/IFileService.h>
#include <sakura/uri/UriIdentity.h>

#include "config/BuiltinConfigurationDescriptors.h"
#include "config/ITrustedFoldersStore.h"
#include "config/IWorkspaceTrustMementoStore.h"
#include "config/WorkspaceTrustPromptPolicy.h"
#include "platform/profiles/ProfileBootstrapSnapshot.h"
#include "platform/profiles/UserDataProfileBootstrap.h"
#include "workbench/CWorkbenchRuntime.h"
#include "workbench/IWorkbenchRuntime.h"

namespace {

using platform::filesystem::DirectoryEntry;
using platform::filesystem::EFileResultStatus;
using platform::filesystem::EFileWatchEventType;
using platform::filesystem::FileBytes;
using platform::filesystem::FileConditionalReplaceOptions;
using platform::filesystem::FileConditionalReplaceResult;
using platform::filesystem::FileContentSnapshot;
using platform::filesystem::FileReadOptions;
using platform::filesystem::FileResult;
using platform::filesystem::FileStat;
using platform::filesystem::FileVersionToken;
using platform::filesystem::FileWatchEvent;
using platform::filesystem::IFileService;
using platform::filesystem::IFileWatch;
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
using workbench::ResolveWorkbenchBootstrapContext;
using workbench::WorkbenchBootstrapContext;
using workbench::WorkbenchBootstrapRequest;
using workbench::WorkbenchRuntimeDependencies;
using workbench::WorkspaceTrustStartupPromptModel;
using workbench::WorkspaceTrustUntrustedFilesModel;

// This helper set (kProfileId, Parse, Profile, UserDataProfile, Bootstrap,
// Bytes, FakeFileService, FakeTrustedFoldersStore) is deliberately copied
// verbatim from CWorkbenchRuntimeTest.cpp's anonymous-namespace idioms rather
// than shared: that file is off limits for this change, and the helpers are
// TU-local there.

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
		Profile(), UserDataProfile(), L"workspace-trust-prompt-test", std::move(folder), std::nullopt, {},
		std::move(initialDocument), std::nullopt,
	};
	auto resolved = ResolveWorkbenchBootstrapContext(std::move(request));
	EXPECT_TRUE(resolved.Resolved());
	return std::move(*resolved.context);
}

FileResult<FileBytes> Bytes(std::string value)
{
	return FileResult<FileBytes>::Success(FileBytes(value.begin(), value.end()));
}

//! In-memory IFileService, copied from CWorkbenchRuntimeTest.cpp's own fake.
//! Only Read is actually exercised by this file's tests (to seed and load
//! profile settings.json before Start()); the rest is implemented so the
//! class satisfies IFileService's pure-virtual surface exactly.
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
			std::lock_guard lock(m_mutex);
			const auto identity = UriIdentityService::MakeComparisonKey(resource);
			m_reads.push_back(resource.ToString());
			callback = onRead;
			const auto found = m_results.find(identity);
			result = found == m_results.end()
				? FileResult<FileBytes>::Failure(EFileResultStatus::NotFound)
				: found->second;
		}
		if (callback) callback();
		return result;
	}

	FileResult<FileContentSnapshot> ReadVersioned(const Uri& resource, const FileReadOptions&) override
	{
		std::lock_guard lock(m_mutex);
		const auto identity = UriIdentityService::MakeComparisonKey(resource);
		m_reads.push_back(resource.ToString());
		const auto found = m_results.find(identity);
		const auto version = m_versions.find(identity);
		if (found == m_results.end()) {
			return FileResult<FileContentSnapshot>::Failure(EFileResultStatus::NotFound);
		}
		if (!found->second.Succeeded() || !found->second.value) {
			return FileResult<FileContentSnapshot>::Failure(found->second.status, found->second.diagnostic);
		}
		if (version == m_versions.end()) {
			return FileResult<FileContentSnapshot>::Failure(EFileResultStatus::Failed);
		}
		return FileResult<FileContentSnapshot>::Success({ *found->second.value, version->second });
	}

	FileConditionalReplaceResult ConditionalAtomicReplace(
		const Uri& resource, const FileBytes& bytes, const FileConditionalReplaceOptions& options) override
	{
		std::lock_guard lock(m_mutex);
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
		const Uri&, const platform::filesystem::FileWatchOptions&) override
	{
		return FileResult<std::unique_ptr<IFileWatch>>::Failure(EFileResultStatus::Unsupported);
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
	std::vector<std::wstring> m_reads;
};

//! Fake config::ITrustedFoldersStore, copied from CWorkbenchRuntimeTest.cpp's
//! own fake. Composed whenever a test needs WorkspaceTrustPrompt()'s
//! persistenceReady to be true (a grantable option requires both a non-empty
//! prompt and a ready trusted-folders store).
class FakeTrustedFoldersStore final : public config::ITrustedFoldersStore {
public:
	config::TrustedFoldersLoadResult Load() override
	{
		++loadCalls;
		return loadResult;
	}

	config::TrustedFoldersSaveResult Save(const config::TrustedFoldersSnapshot& snapshot) override
	{
		++saveCalls;
		lastSavedSnapshot = snapshot;
		return saveResult;
	}

	// NotFound is the default because it is the normal, non-degraded starting
	// state for a profile that has never trusted a folder: it makes the store
	// ready without pre-seeding any entry.
	config::TrustedFoldersLoadResult loadResult{
		config::ETrustedFoldersLoadStatus::NotFound, std::nullopt, {}
	};
	config::TrustedFoldersSaveResult saveResult{
		config::ETrustedFoldersSaveStatus::Persisted, {}
	};
	std::size_t loadCalls = 0;
	std::size_t saveCalls = 0;
	std::optional<config::TrustedFoldersSnapshot> lastSavedSnapshot;
};

//! Fake config::IWorkspaceTrustMementoStore. Its default Load() answers
//! NotFound -- the "this workspace has never been asked" starting state --
//! and its default Save() answers Persisted.
class FakeWorkspaceTrustMementoStore final : public config::IWorkspaceTrustMementoStore {
public:
	config::WorkspaceTrustMementoLoadResult Load() override
	{
		++loadCalls;
		return loadResult;
	}

	config::WorkspaceTrustMementoSaveResult Save(const config::WorkspaceTrustMemento& memento) override
	{
		++saveCalls;
		lastSavedMemento = memento;
		return saveResult;
	}

	config::WorkspaceTrustMementoLoadResult loadResult{
		config::EWorkspaceTrustMementoLoadStatus::NotFound, std::nullopt, {}
	};
	config::WorkspaceTrustMementoSaveResult saveResult{ config::EWorkspaceTrustMementoSaveStatus::Persisted, {} };
	std::size_t loadCalls = 0;
	std::size_t saveCalls = 0;
	std::optional<config::WorkspaceTrustMemento> lastSavedMemento;
};

//! Local fixture, analogous to CWorkbenchRuntimeTest.cpp's RuntimeFixture but
//! extended with the workspace-trust-memento fake that fixture does not carry.
struct TrustFixture final {
	explicit TrustFixture(
		WorkbenchBootstrapContext bootstrap,
		std::unique_ptr<FakeTrustedFoldersStore> ownedTrustedFoldersStore = {},
		std::unique_ptr<FakeWorkspaceTrustMementoStore> ownedMementoStore = {})
	{
		auto ownedFiles = std::make_unique<FakeFileService>();
		files = ownedFiles.get();
		trustedFoldersStore = ownedTrustedFoldersStore.get();
		mementoStore = ownedMementoStore.get();

		WorkbenchRuntimeDependencies dependencies;
		dependencies.fileService = std::move(ownedFiles);
		dependencies.trustedFoldersStore = std::move(ownedTrustedFoldersStore);
		dependencies.workspaceTrustMementoStore = std::move(ownedMementoStore);

		runtime = std::make_unique<CWorkbenchRuntime>(
			std::move(bootstrap), config::BuiltinConfigurationDescriptors(), std::move(dependencies));
	}

	FakeFileService* files = nullptr;
	FakeTrustedFoldersStore* trustedFoldersStore = nullptr;
	FakeWorkspaceTrustMementoStore* mementoStore = nullptr;
	std::unique_ptr<CWorkbenchRuntime> runtime;
};

//! Seeds the profile-scoped settings.json this runtime's ReadSnapshot will
//! see at Start(). Settings are read live from configuration state at the
//! time of the call, so this must run before Start(), matching the idiom
//! CWorkbenchRuntimeTest.cpp uses for its own restricted-configuration tests.
void SetProfileSettings(TrustFixture& fixture, const std::string& json)
{
	fixture.files->Set(fixture.runtime->Bootstrap().UserDataProfile().Resources().Settings(), Bytes(json));
}

} // namespace

// ---------------------------------------------------------------------------
// A. Restoring the memento at Start().
// ---------------------------------------------------------------------------

TEST(CWorkbenchRuntimeWorkspaceTrustPrompt, StartupPromptShowsWhenNoMementoStoreIsComposed)
{
	auto folder = Parse(L"file:///C:/Repo");
	auto trustedFolders = std::make_unique<FakeTrustedFoldersStore>();
	TrustFixture fixture(Bootstrap(folder), std::move(trustedFolders));
	SetProfileSettings(fixture, R"json({ "security.workspace.trust.startupPrompt": "once" })json");
	ASSERT_TRUE(fixture.runtime->Start().IsUsable());
	ASSERT_EQ(config::EWorkspaceTrustState::Unknown, fixture.runtime->WorkspaceContext().Snapshot().trust);

	const auto model = fixture.runtime->WorkspaceTrustStartupPrompt();
	EXPECT_EQ(config::EWorkspaceTrustStartupPromptDecision::ShowRecordUnreadable, model.decision);
	EXPECT_TRUE(model.ShouldShow());
}

TEST(CWorkbenchRuntimeWorkspaceTrustPrompt, StartupPromptShowsWhenMementoStoreReportsNoWorkspaceScopeWithoutADiagnostic)
{
	// A genuine empty window can never reach a "show" decision: BuildTrustGrantEntries
	// has nothing to offer for EWorkspaceKind::Empty, so hasGrantableOption is always
	// false and the decision resolves to SkipNothingToGrant before the memento record
	// is even consulted. NoWorkspaceScope's "ask again" contract is exercised here on
	// a Folder workspace whose fake memento store is told to answer NoWorkspaceScope
	// directly, which is a unit test of RestoreWorkspaceTrustMemento's own handling of
	// that status code, not a claim that a real empty window produces this decision.
	auto folder = Parse(L"file:///C:/Repo");
	auto trustedFolders = std::make_unique<FakeTrustedFoldersStore>();
	auto memento = std::make_unique<FakeWorkspaceTrustMementoStore>();
	memento->loadResult = { config::EWorkspaceTrustMementoLoadStatus::NoWorkspaceScope, std::nullopt, {} };
	TrustFixture fixture(Bootstrap(folder), std::move(trustedFolders), std::move(memento));
	SetProfileSettings(fixture, R"json({ "security.workspace.trust.startupPrompt": "once" })json");

	const auto started = fixture.runtime->Start();
	ASSERT_TRUE(started.IsUsable());
	EXPECT_TRUE(started.snapshot.diagnostics.empty());

	const auto model = fixture.runtime->WorkspaceTrustStartupPrompt();
	EXPECT_EQ(config::EWorkspaceTrustStartupPromptDecision::ShowRecordUnreadable, model.decision);
	EXPECT_TRUE(model.ShouldShow());
}

TEST(CWorkbenchRuntimeWorkspaceTrustPrompt, StartupPromptSkipsAlreadyShownWhenMementoStoreReportsItWasShown)
{
	auto folder = Parse(L"file:///C:/Repo");
	auto trustedFolders = std::make_unique<FakeTrustedFoldersStore>();
	auto memento = std::make_unique<FakeWorkspaceTrustMementoStore>();
	memento->loadResult = {
		config::EWorkspaceTrustMementoLoadStatus::Loaded,
		config::WorkspaceTrustMemento{ .startupPromptShown = true, .untrustedFilesAccepted = false },
		{},
	};
	TrustFixture fixture(Bootstrap(folder), std::move(trustedFolders), std::move(memento));
	SetProfileSettings(fixture, R"json({ "security.workspace.trust.startupPrompt": "once" })json");
	ASSERT_TRUE(fixture.runtime->Start().IsUsable());

	const auto model = fixture.runtime->WorkspaceTrustStartupPrompt();
	EXPECT_EQ(config::EWorkspaceTrustStartupPromptDecision::SkipAlreadyShown, model.decision);
	EXPECT_FALSE(model.ShouldShow());
}

TEST(CWorkbenchRuntimeWorkspaceTrustPrompt, StartupPromptShowsAndRecordsADiagnosticWhenTheStoredMementoIsInvalid)
{
	auto folder = Parse(L"file:///C:/Repo");
	auto trustedFolders = std::make_unique<FakeTrustedFoldersStore>();
	auto memento = std::make_unique<FakeWorkspaceTrustMementoStore>();
	memento->loadResult = {
		config::EWorkspaceTrustMementoLoadStatus::InvalidStoredMemento, std::nullopt, L"corrupt memento"
	};
	TrustFixture fixture(Bootstrap(folder), std::move(trustedFolders), std::move(memento));
	SetProfileSettings(fixture, R"json({ "security.workspace.trust.startupPrompt": "once" })json");

	const auto started = fixture.runtime->Start();
	ASSERT_EQ(EWorkbenchRuntimeResultCode::ReadyWithDiagnostics, started.code);
	ASSERT_EQ(1U, started.snapshot.diagnostics.size());
	EXPECT_EQ(EWorkbenchRuntimeDiagnosticSource::WorkspaceTrust, started.snapshot.diagnostics.front().source);

	const auto model = fixture.runtime->WorkspaceTrustStartupPrompt();
	EXPECT_EQ(config::EWorkspaceTrustStartupPromptDecision::ShowRecordUnreadable, model.decision);
	EXPECT_TRUE(model.ShouldShow());
}

// ---------------------------------------------------------------------------
// B. Turning settings plus workspace state into a startup-prompt decision.
// ---------------------------------------------------------------------------

TEST(CWorkbenchRuntimeWorkspaceTrustPrompt, StartupPromptSkipsWhenSettingIsNever)
{
	auto folder = Parse(L"file:///C:/Repo");
	auto trustedFolders = std::make_unique<FakeTrustedFoldersStore>();
	TrustFixture fixture(Bootstrap(folder), std::move(trustedFolders));
	// No startupPrompt setting seeded: the built-in descriptor default is "never".
	ASSERT_TRUE(fixture.runtime->Start().IsUsable());
	ASSERT_EQ(config::EWorkspaceTrustState::Unknown, fixture.runtime->WorkspaceContext().Snapshot().trust);

	const auto model = fixture.runtime->WorkspaceTrustStartupPrompt();
	EXPECT_EQ(config::EWorkspaceTrustStartupPromptDecision::SkipSettingNever, model.decision);
	EXPECT_FALSE(model.ShouldShow());
}

TEST(CWorkbenchRuntimeWorkspaceTrustPrompt, StartupPromptSkipsWhenFeatureIsDisabled)
{
	auto folder = Parse(L"file:///C:/Repo");
	auto trustedFolders = std::make_unique<FakeTrustedFoldersStore>();
	TrustFixture fixture(Bootstrap(folder), std::move(trustedFolders));
	SetProfileSettings(fixture,
		R"json({ "security.workspace.trust.enabled": false, "security.workspace.trust.startupPrompt": "always" })json");
	ASSERT_TRUE(fixture.runtime->Start().IsUsable());

	const auto model = fixture.runtime->WorkspaceTrustStartupPrompt();
	EXPECT_EQ(config::EWorkspaceTrustStartupPromptDecision::SkipFeatureDisabled, model.decision);
	EXPECT_FALSE(model.ShouldShow());
}

TEST(CWorkbenchRuntimeWorkspaceTrustPrompt, StartupPromptSkipsWhenNothingIsGrantableAndReportsEmptyOptions)
{
	// No trustedFoldersStore is composed, so WorkspaceTrustPrompt()'s persistenceReady
	// is false and hasGrantableOption is false regardless of the folder or the setting.
	auto folder = Parse(L"file:///C:/Repo");
	TrustFixture fixture(Bootstrap(folder));
	SetProfileSettings(fixture, R"json({ "security.workspace.trust.startupPrompt": "always" })json");
	ASSERT_TRUE(fixture.runtime->Start().IsUsable());
	ASSERT_EQ(config::EWorkspaceTrustState::Unknown, fixture.runtime->WorkspaceContext().Snapshot().trust);

	const auto prompt = fixture.runtime->WorkspaceTrustPrompt();
	EXPECT_FALSE(prompt.persistenceReady);

	const auto model = fixture.runtime->WorkspaceTrustStartupPrompt();
	EXPECT_EQ(config::EWorkspaceTrustStartupPromptDecision::SkipNothingToGrant, model.decision);
	EXPECT_TRUE(model.prompt.options.empty());
}

TEST(CWorkbenchRuntimeWorkspaceTrustPrompt, PromptModelStaysDefaultConstructedWheneverTheDecisionDoesNotShow)
{
	const auto expectDefaultPrompt = [](const WorkspaceTrustStartupPromptModel& model) {
		EXPECT_FALSE(model.ShouldShow());
		EXPECT_TRUE(model.prompt.options.empty());
		EXPECT_FALSE(model.prompt.persistenceReady);
	};

	{
		// SkipFeatureDisabled.
		auto folder = Parse(L"file:///C:/Repo");
		auto trustedFolders = std::make_unique<FakeTrustedFoldersStore>();
		TrustFixture fixture(Bootstrap(folder), std::move(trustedFolders));
		SetProfileSettings(fixture, R"json({ "security.workspace.trust.enabled": false })json");
		ASSERT_TRUE(fixture.runtime->Start().IsUsable());
		const auto model = fixture.runtime->WorkspaceTrustStartupPrompt();
		ASSERT_EQ(config::EWorkspaceTrustStartupPromptDecision::SkipFeatureDisabled, model.decision);
		expectDefaultPrompt(model);
	}
	{
		// SkipSettingNever.
		auto folder = Parse(L"file:///C:/Repo");
		auto trustedFolders = std::make_unique<FakeTrustedFoldersStore>();
		TrustFixture fixture(Bootstrap(folder), std::move(trustedFolders));
		ASSERT_TRUE(fixture.runtime->Start().IsUsable());
		const auto model = fixture.runtime->WorkspaceTrustStartupPrompt();
		ASSERT_EQ(config::EWorkspaceTrustStartupPromptDecision::SkipSettingNever, model.decision);
		expectDefaultPrompt(model);
	}
	{
		// SkipNothingToGrant.
		auto folder = Parse(L"file:///C:/Repo");
		TrustFixture fixture(Bootstrap(folder));
		SetProfileSettings(fixture, R"json({ "security.workspace.trust.startupPrompt": "always" })json");
		ASSERT_TRUE(fixture.runtime->Start().IsUsable());
		const auto model = fixture.runtime->WorkspaceTrustStartupPrompt();
		ASSERT_EQ(config::EWorkspaceTrustStartupPromptDecision::SkipNothingToGrant, model.decision);
		expectDefaultPrompt(model);
	}
	{
		// SkipAlreadyShown.
		auto folder = Parse(L"file:///C:/Repo");
		auto trustedFolders = std::make_unique<FakeTrustedFoldersStore>();
		auto memento = std::make_unique<FakeWorkspaceTrustMementoStore>();
		memento->loadResult = {
			config::EWorkspaceTrustMementoLoadStatus::Loaded,
			config::WorkspaceTrustMemento{ .startupPromptShown = true, .untrustedFilesAccepted = false },
			{},
		};
		TrustFixture fixture(Bootstrap(folder), std::move(trustedFolders), std::move(memento));
		SetProfileSettings(fixture, R"json({ "security.workspace.trust.startupPrompt": "once" })json");
		ASSERT_TRUE(fixture.runtime->Start().IsUsable());
		const auto model = fixture.runtime->WorkspaceTrustStartupPrompt();
		ASSERT_EQ(config::EWorkspaceTrustStartupPromptDecision::SkipAlreadyShown, model.decision);
		expectDefaultPrompt(model);
	}
}

// ---------------------------------------------------------------------------
// C. Recording that the prompt was shown / that untrusted files were accepted.
// ---------------------------------------------------------------------------

TEST(CWorkbenchRuntimeWorkspaceTrustPrompt, RecordStartupPromptShownWithoutAStoreReturnsUnavailableAndStillShowsAfterward)
{
	auto folder = Parse(L"file:///C:/Repo");
	auto trustedFolders = std::make_unique<FakeTrustedFoldersStore>();
	TrustFixture fixture(Bootstrap(folder), std::move(trustedFolders));
	SetProfileSettings(fixture, R"json({ "security.workspace.trust.startupPrompt": "once" })json");
	ASSERT_TRUE(fixture.runtime->Start().IsUsable());

	const auto status = fixture.runtime->RecordWorkspaceTrustStartupPromptShown();
	EXPECT_EQ(config::EWorkspaceTrustMementoSaveStatus::Unavailable, status);

	const auto model = fixture.runtime->WorkspaceTrustStartupPrompt();
	EXPECT_TRUE(model.ShouldShow());
}

TEST(CWorkbenchRuntimeWorkspaceTrustPrompt, RecordStartupPromptShownPersistsAndSkipsOnTheNextOnceStartupPrompt)
{
	auto folder = Parse(L"file:///C:/Repo");
	auto trustedFolders = std::make_unique<FakeTrustedFoldersStore>();
	auto memento = std::make_unique<FakeWorkspaceTrustMementoStore>();
	TrustFixture fixture(Bootstrap(folder), std::move(trustedFolders), std::move(memento));
	SetProfileSettings(fixture, R"json({ "security.workspace.trust.startupPrompt": "once" })json");
	ASSERT_TRUE(fixture.runtime->Start().IsUsable());
	ASSERT_TRUE(fixture.runtime->WorkspaceTrustStartupPrompt().ShouldShow());

	const auto status = fixture.runtime->RecordWorkspaceTrustStartupPromptShown();
	EXPECT_EQ(config::EWorkspaceTrustMementoSaveStatus::Persisted, status);
	ASSERT_TRUE(fixture.mementoStore->lastSavedMemento.has_value());
	EXPECT_TRUE(fixture.mementoStore->lastSavedMemento->startupPromptShown);
	EXPECT_FALSE(fixture.mementoStore->lastSavedMemento->untrustedFilesAccepted);

	const auto loadCallsBefore = fixture.mementoStore->loadCalls;
	const auto model = fixture.runtime->WorkspaceTrustStartupPrompt();
	EXPECT_EQ(config::EWorkspaceTrustStartupPromptDecision::SkipAlreadyShown, model.decision);
	// The in-memory working copy answers the next prompt; the store is not re-read.
	EXPECT_EQ(loadCallsBefore, fixture.mementoStore->loadCalls);
}

TEST(CWorkbenchRuntimeWorkspaceTrustPrompt, RecordStartupPromptShownReturnsConflictVerbatimAndDoesNotAdvanceTheInMemoryRecord)
{
	auto folder = Parse(L"file:///C:/Repo");
	auto trustedFolders = std::make_unique<FakeTrustedFoldersStore>();
	auto memento = std::make_unique<FakeWorkspaceTrustMementoStore>();
	memento->saveResult = {
		config::EWorkspaceTrustMementoSaveStatus::Conflict, L"another window committed a different record"
	};
	TrustFixture fixture(Bootstrap(folder), std::move(trustedFolders), std::move(memento));
	SetProfileSettings(fixture, R"json({ "security.workspace.trust.startupPrompt": "once" })json");
	ASSERT_TRUE(fixture.runtime->Start().IsUsable());

	const auto status = fixture.runtime->RecordWorkspaceTrustStartupPromptShown();
	EXPECT_EQ(config::EWorkspaceTrustMementoSaveStatus::Conflict, status);

	// The load was readable (NotFound) and the failed save must not have advanced
	// the in-memory record, so the working copy still says the prompt was never shown.
	const auto model = fixture.runtime->WorkspaceTrustStartupPrompt();
	EXPECT_EQ(config::EWorkspaceTrustStartupPromptDecision::Show, model.decision);
	EXPECT_TRUE(model.ShouldShow());
}

TEST(CWorkbenchRuntimeWorkspaceTrustPrompt, RecordUntrustedFilesAcceptedSetsOnlyThatField)
{
	auto folder = Parse(L"file:///C:/Repo");
	auto trustedFolders = std::make_unique<FakeTrustedFoldersStore>();
	auto memento = std::make_unique<FakeWorkspaceTrustMementoStore>();
	TrustFixture fixture(Bootstrap(folder), std::move(trustedFolders), std::move(memento));
	ASSERT_TRUE(fixture.runtime->Start().IsUsable());

	const auto status = fixture.runtime->RecordUntrustedFilesAccepted();
	EXPECT_EQ(config::EWorkspaceTrustMementoSaveStatus::Persisted, status);
	ASSERT_TRUE(fixture.mementoStore->lastSavedMemento.has_value());
	EXPECT_TRUE(fixture.mementoStore->lastSavedMemento->untrustedFilesAccepted);
	EXPECT_FALSE(fixture.mementoStore->lastSavedMemento->startupPromptShown);
}

// ---------------------------------------------------------------------------
// D. Resolving the untrusted-files decision. These all grant trust to the
// folder first (via GrantWorkspaceTrust) so that WorkspaceTrustUntrustedFiles
// is exercised against a genuinely Trusted window; see this file's report
// for why that reading was chosen over a literally untrusted window.
// ---------------------------------------------------------------------------

TEST(CWorkbenchRuntimeWorkspaceTrustPrompt, UntrustedFilesPromptsByDefaultForATrustedWindowWithResourcesOutsideTrust)
{
	auto folder = Parse(L"file:///C:/Repo");
	auto trustedFolders = std::make_unique<FakeTrustedFoldersStore>();
	TrustFixture fixture(Bootstrap(folder), std::move(trustedFolders));
	ASSERT_TRUE(fixture.runtime->Start().IsUsable());
	const auto granted = fixture.runtime->GrantWorkspaceTrust(workbench::EWorkspaceTrustGrantScope::CurrentWorkspace);
	ASSERT_EQ(workbench::EWorkspaceTrustGrantStatus::Granted, granted.status);
	ASSERT_EQ(config::EWorkspaceTrustState::Trusted, fixture.runtime->WorkspaceContext().Snapshot().trust);

	// The built-in default for security.workspace.trust.untrustedFiles is "prompt".
	const auto model = fixture.runtime->WorkspaceTrustUntrustedFiles(/* allResourcesTrusted */ false);
	EXPECT_EQ(config::EWorkspaceTrustUntrustedFilesDecision::Prompt, model.decision);
	EXPECT_NE(config::EWorkspaceTrustUntrustedFilesDecision::Open, model.decision);
}

TEST(CWorkbenchRuntimeWorkspaceTrustPrompt, UntrustedFilesResolvesUnsupportedRatherThanOpenWhenSettingIsNewWindow)
{
	auto folder = Parse(L"file:///C:/Repo");
	auto trustedFolders = std::make_unique<FakeTrustedFoldersStore>();
	TrustFixture fixture(Bootstrap(folder), std::move(trustedFolders));
	SetProfileSettings(fixture, R"json({ "security.workspace.trust.untrustedFiles": "newWindow" })json");
	ASSERT_TRUE(fixture.runtime->Start().IsUsable());
	const auto granted = fixture.runtime->GrantWorkspaceTrust(workbench::EWorkspaceTrustGrantScope::CurrentWorkspace);
	ASSERT_EQ(workbench::EWorkspaceTrustGrantStatus::Granted, granted.status);
	ASSERT_EQ(config::EWorkspaceTrustState::Trusted, fixture.runtime->WorkspaceContext().Snapshot().trust);

	// This shell cannot honor "newWindow" with a genuinely restricted second window,
	// so it must resolve Unsupported -- never silently widen into Open, which is the
	// one outcome this setting exists to prevent. This is the most important
	// assertion in this file.
	const auto model = fixture.runtime->WorkspaceTrustUntrustedFiles(/* allResourcesTrusted */ false);
	EXPECT_EQ(config::EWorkspaceTrustUntrustedFilesDecision::Unsupported, model.decision);
	EXPECT_NE(config::EWorkspaceTrustUntrustedFilesDecision::Open, model.decision);
}

TEST(CWorkbenchRuntimeWorkspaceTrustPrompt, UntrustedFilesOpensWhenAllResourcesAreAlreadyTrusted)
{
	auto folder = Parse(L"file:///C:/Repo");
	auto trustedFolders = std::make_unique<FakeTrustedFoldersStore>();
	TrustFixture fixture(Bootstrap(folder), std::move(trustedFolders));
	ASSERT_TRUE(fixture.runtime->Start().IsUsable());
	const auto granted = fixture.runtime->GrantWorkspaceTrust(workbench::EWorkspaceTrustGrantScope::CurrentWorkspace);
	ASSERT_EQ(workbench::EWorkspaceTrustGrantStatus::Granted, granted.status);
	ASSERT_EQ(config::EWorkspaceTrustState::Trusted, fixture.runtime->WorkspaceContext().Snapshot().trust);

	const auto model = fixture.runtime->WorkspaceTrustUntrustedFiles(/* allResourcesTrusted */ true);
	EXPECT_EQ(config::EWorkspaceTrustUntrustedFilesDecision::Open, model.decision);
}

// ---------------------------------------------------------------------------
// E. WorkspaceTrustCoversResource reads only the durable Trusted Folders list.
// ---------------------------------------------------------------------------

TEST(CWorkbenchRuntimeWorkspaceTrustPrompt, WorkspaceTrustCoversResourceMatchesAnEntryAndItsDescendantsOnly)
{
	auto trustedFolders = std::make_unique<FakeTrustedFoldersStore>();
	trustedFolders->loadResult = {
		config::ETrustedFoldersLoadStatus::Loaded,
		config::TrustedFoldersSnapshot{ { config::WorkspaceTrustEntry{ Parse(L"file:///C:/Trusted"), true } } },
		{},
	};
	TrustFixture fixture(Bootstrap(), std::move(trustedFolders));
	ASSERT_TRUE(fixture.runtime->Start().IsUsable());

	EXPECT_TRUE(fixture.runtime->WorkspaceTrustCoversResource(Parse(L"file:///C:/Trusted")));
	EXPECT_TRUE(fixture.runtime->WorkspaceTrustCoversResource(Parse(L"file:///C:/Trusted/sub/file.txt")));
	EXPECT_FALSE(fixture.runtime->WorkspaceTrustCoversResource(Parse(L"file:///C:/Other/file.txt")));
}

// ---------------------------------------------------------------------------
// F. An empty window's untrusted-files acceptance is session-scoped, because
// SaveWorkspaceTrustMemento requires m_workspaceTrustMementoReadable, which an
// empty window never has (no workspace/folder to key the durable record on).
// ---------------------------------------------------------------------------

TEST(CWorkbenchRuntimeWorkspaceTrustPrompt, RecordUntrustedFilesAcceptedOnAnEmptyWindowOpensForTheRestOfTheSessionOnly)
{
	auto trustedFolders = std::make_unique<FakeTrustedFoldersStore>();
	auto memento = std::make_unique<FakeWorkspaceTrustMementoStore>();
	// The fake does not know the workspace kind on its own; seed NoWorkspaceScope to
	// match what the production store actually returns for an empty window.
	memento->loadResult = { config::EWorkspaceTrustMementoLoadStatus::NoWorkspaceScope, std::nullopt, {} };
	TrustFixture fixture(Bootstrap(), std::move(trustedFolders), std::move(memento));
	ASSERT_TRUE(fixture.runtime->Start().IsUsable());

	const auto before = fixture.runtime->WorkspaceTrustUntrustedFiles(/* allResourcesTrusted */ false);
	EXPECT_EQ(config::EWorkspaceTrustUntrustedFilesDecision::Prompt, before.decision);

	const auto recordStatus = fixture.runtime->RecordUntrustedFilesAccepted();
	EXPECT_EQ(config::EWorkspaceTrustMementoSaveStatus::Unavailable, recordStatus);

	const auto after = fixture.runtime->WorkspaceTrustUntrustedFiles(/* allResourcesTrusted */ false);
	EXPECT_EQ(config::EWorkspaceTrustUntrustedFilesDecision::Open, after.decision);
}

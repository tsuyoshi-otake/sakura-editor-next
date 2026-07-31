/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include "_main/ControlPlatformWorkingCopyPersistenceStore.h"

#include "workbench/editor/persistence/WorkingCopyPersistenceCodec.h"

#include <algorithm>
#include <deque>
#include <map>
#include <utility>
#include <vector>

namespace {

using platform::controlipc::EEditorControlStorageApplyCode;
using platform::controlipc::EEditorControlStorageCacheCoordinateCode;
using platform::controlipc::EditorControlStorageApplyResult;
using platform::controlipc::EditorControlStorageCacheCoordinateResult;
using platform::controlipc::EditorControlStorageCacheCoordinates;
using platform::storage::EStorageTarget;
using platform::storage::StorageAddress;
using platform::storage::StorageEntry;
using platform::storage::StorageMutationRequest;
using workbench::editor::persistence::CWorkingCopyPersistenceCodec;
using workbench::editor::persistence::ControlPlatformWorkingCopyPersistenceStoreDependencies;
using workbench::editor::persistence::EditorSessionInputDescriptor;
using workbench::editor::persistence::EditorSessionManifest;
using workbench::editor::persistence::EWorkingCopyEol;
using workbench::editor::persistence::EWorkingCopyPersistenceLoadStatus;
using workbench::editor::persistence::EWorkingCopyPersistenceWriteStatus;
using workbench::editor::persistence::EWorkingCopyTextEncoding;
using workbench::editor::persistence::WorkingCopyBackup;
using workbench::editor::persistence::WorkingCopyPersistenceIdentity;
using workbench::editor::persistence::WorkingCopyPersistenceScope;

constexpr char kProfile[] = "0123456789abcdef0123456789abcdef";

EditorControlStorageCacheCoordinateResult Coordinates(std::uint64_t revision)
{
	return { EEditorControlStorageCacheCoordinateCode::Ready,
		platform::controlipc::EEditorControlPlatformRuntimeState::Ready,
		EditorControlStorageCacheCoordinates{ kProfile, 7, revision }, {} };
}

WorkingCopyPersistenceScope ProfileScope()
{
	return { kProfile, std::nullopt };
}

WorkingCopyPersistenceScope WorkspaceScope()
{
	return { kProfile, std::string("workspace.logical.alpha") };
}

WorkingCopyPersistenceIdentity Identity(std::string opaque = "untitled.logical.alpha")
{
	return { "workbench.editor.text", std::nullopt, std::move(opaque) };
}

WorkingCopyBackup Backup(WorkingCopyPersistenceScope scope = ProfileScope(), std::uint64_t generation = 1,
	std::string content = "dirty text")
{
	WorkingCopyBackup backup;
	backup.scope = std::move(scope);
	backup.identity = Identity();
	backup.generation = generation;
	backup.contentVersion = generation;
	backup.checksum = CWorkingCopyPersistenceCodec::ComputeContentChecksum(content);
	backup.encoding = EWorkingCopyTextEncoding::Utf8;
	backup.eol = EWorkingCopyEol::Lf;
	backup.dirty = true;
	backup.content = std::move(content);
	return backup;
}

EditorSessionManifest Session(WorkingCopyPersistenceScope scope = ProfileScope(), std::uint64_t generation = 1)
{
	EditorSessionManifest session;
	session.scope = std::move(scope);
	session.generation = generation;
	session.logicalGroupId = "workbench.editor.group.main";
	session.activeInputId = "input-a";
	session.inputs = { { "input-a", "workbench.editor.text", Identity(), 1, "{}", generation } };
	return session;
}

struct Script final {
	std::uint64_t revision = 3;
	std::map<StorageAddress, StorageEntry> entries;
	std::deque<EEditorControlStorageApplyCode> outcomes;
	std::vector<StorageMutationRequest> requests;
};

ControlPlatformWorkingCopyPersistenceStoreDependencies Dependencies(Script& script)
{
	return {
		.storageCacheCoordinates = [&script] { return Coordinates(script.revision); },
		.find = [&script](const StorageAddress& address) -> std::optional<StorageEntry> {
			const auto found = script.entries.find(address);
			return found == script.entries.end() ? std::nullopt : std::optional<StorageEntry>(found->second);
		},
		.apply = [&script](const StorageMutationRequest& request) {
			script.requests.push_back(request);
			const auto code = script.outcomes.empty() ? EEditorControlStorageApplyCode::Succeeded : script.outcomes.front();
			if (!script.outcomes.empty()) script.outcomes.pop_front();
			EditorControlStorageApplyResult result;
			result.code = code;
			if (code == EEditorControlStorageApplyCode::Succeeded || code == EEditorControlStorageApplyCode::NotApplicable) {
				++script.revision;
				for (const auto& mutation : request.mutations) {
					if (mutation.value) script.entries[mutation.address] = { mutation.address, mutation.target, *mutation.value, script.revision };
					else script.entries.erase(mutation.address);
				}
				result.storageResult = { platform::storage::EStorageMutationStatus::Succeeded, script.revision, false, {}, std::nullopt };
			}
			return result;
		},
		.operationIdFactory = [] { return std::string("working-copy-test-random-factory"); },
	};
}

auto FindManifest(Script& script, const char* owner)
{
	return std::find_if(script.entries.begin(), script.entries.end(), [owner](const auto& pair) {
		return pair.first.owner == owner && pair.first.key.ends_with(".manifest");
	});
}

TEST(ControlPlatformWorkingCopyPersistenceStore, ProfileAndWorkspaceAddressesAreSeparatedAndRoundTrip)
{
	Script script;
	CControlPlatformWorkingCopyPersistenceStore store(kProfile, Dependencies(script));
	auto profile = Backup(ProfileScope());
	auto workspace = Backup(WorkspaceScope());
	ASSERT_EQ(EWorkingCopyPersistenceLoadStatus::NotFound, store.Load(profile.scope, profile.identity).status);
	ASSERT_EQ(EWorkingCopyPersistenceWriteStatus::Persisted, store.Save(profile, std::nullopt, "backup-profile-1").status);
	ASSERT_EQ(EWorkingCopyPersistenceLoadStatus::NotFound, store.Load(workspace.scope, workspace.identity).status);
	ASSERT_EQ(EWorkingCopyPersistenceWriteStatus::Persisted, store.Save(workspace, std::nullopt, "backup-workspace-1").status);
	EXPECT_NE(script.requests[0].mutations[0].address, script.requests[1].mutations[0].address);

	CControlPlatformWorkingCopyPersistenceStore reader(kProfile, Dependencies(script));
	EXPECT_EQ(EWorkingCopyPersistenceLoadStatus::Loaded, reader.Load(profile.scope, profile.identity).status);
	EXPECT_EQ(EWorkingCopyPersistenceLoadStatus::Loaded, reader.Load(workspace.scope, workspace.identity).status);
	EXPECT_EQ(EWorkingCopyPersistenceLoadStatus::NotFound, reader.Load(ProfileScope()).status);
	ASSERT_EQ(EWorkingCopyPersistenceWriteStatus::Persisted, reader.Save(Session(), std::nullopt, "session-profile-1").status);
	CControlPlatformWorkingCopyPersistenceStore sessionReader(kProfile, Dependencies(script));
	EXPECT_EQ(EWorkingCopyPersistenceLoadStatus::Loaded, sessionReader.Load(ProfileScope()).status);
}

TEST(ControlPlatformWorkingCopyPersistenceStore, LargePayloadIsOneAtomicMultiChunkRequest)
{
	Script script;
	CControlPlatformWorkingCopyPersistenceStore store(kProfile, Dependencies(script));
	auto backup = Backup(ProfileScope(), 1, std::string(60 * 1024, 'x'));
	ASSERT_EQ(EWorkingCopyPersistenceLoadStatus::NotFound, store.Load(backup.scope, backup.identity).status);
	ASSERT_EQ(EWorkingCopyPersistenceWriteStatus::Persisted, store.Save(backup, std::nullopt, "backup-chunks-1").status);
	ASSERT_EQ(1U, script.requests.size());
	EXPECT_GT(script.requests[0].mutations.size(), 2U);
	EXPECT_LT(script.requests[0].mutations[1].value->size(), platform::storage::kMaximumStorageStringBytes);
}

TEST(ControlPlatformWorkingCopyPersistenceStore, ReplacingLargeBackupDeletesObsoleteChunksInTheSameApply)
{
	Script script;
	CControlPlatformWorkingCopyPersistenceStore store(kProfile, Dependencies(script));
	auto first = Backup(ProfileScope(), 1, std::string(60 * 1024, 'x'));
	ASSERT_EQ(EWorkingCopyPersistenceLoadStatus::NotFound, store.Load(first.scope, first.identity).status);
	ASSERT_EQ(EWorkingCopyPersistenceWriteStatus::Persisted, store.Save(first, std::nullopt, "backup-large-1").status);
	auto second = Backup(ProfileScope(), 2, "small replacement");
	ASSERT_EQ(EWorkingCopyPersistenceWriteStatus::Persisted, store.Save(second, 1, "backup-small-2").status);
	ASSERT_EQ(2U, script.requests.size());
	EXPECT_TRUE(std::any_of(script.requests[1].mutations.begin(), script.requests[1].mutations.end(), [](const auto& mutation) {
		return !mutation.value.has_value();
	}));
}

TEST(ControlPlatformWorkingCopyPersistenceStore, AmbiguousApplyRetriesExactlyOnceWithTheSameImmutableRequest)
{
	Script script;
	script.outcomes = { EEditorControlStorageApplyCode::RetryWithSameOperationId, EEditorControlStorageApplyCode::Succeeded };
	CControlPlatformWorkingCopyPersistenceStore store(kProfile, Dependencies(script));
	auto backup = Backup();
	ASSERT_EQ(EWorkingCopyPersistenceLoadStatus::NotFound, store.Load(backup.scope, backup.identity).status);
	EXPECT_EQ(EWorkingCopyPersistenceWriteStatus::Persisted, store.Save(backup, std::nullopt, "backup-retry-1").status);
	ASSERT_EQ(2U, script.requests.size());
	EXPECT_EQ(script.requests[0], script.requests[1]);
}

TEST(ControlPlatformWorkingCopyPersistenceStore, StorageConflictAndGenerationMismatchAreTerminal)
{
	Script script;
	CControlPlatformWorkingCopyPersistenceStore store(kProfile, Dependencies(script));
	auto backup = Backup();
	ASSERT_EQ(EWorkingCopyPersistenceLoadStatus::NotFound, store.Load(backup.scope, backup.identity).status);
	script.outcomes = { EEditorControlStorageApplyCode::ConflictResnapshotScheduled };
	EXPECT_EQ(EWorkingCopyPersistenceWriteStatus::Conflict, store.Save(backup, std::nullopt, "backup-conflict-1").status);
	script.outcomes.clear();
	ASSERT_EQ(EWorkingCopyPersistenceWriteStatus::Persisted, store.Save(backup, std::nullopt, "backup-save-1").status);
	auto newer = Backup(ProfileScope(), 2);
	EXPECT_EQ(EWorkingCopyPersistenceWriteStatus::Conflict, store.Save(newer, 99, "backup-generation-1").status);
	EXPECT_EQ(EWorkingCopyPersistenceWriteStatus::Conflict, store.Delete(backup.scope, backup.identity, 0, "backup-old-delete-1").status);
}

TEST(ControlPlatformWorkingCopyPersistenceStore, CorruptEnvelopeMissingChunkChecksumAndWrongTargetAreSticky)
{
	for (const auto mode : { 0, 1, 2, 3 }) {
		Script script;
		CControlPlatformWorkingCopyPersistenceStore writer(kProfile, Dependencies(script));
		auto backup = Backup();
		ASSERT_EQ(EWorkingCopyPersistenceLoadStatus::NotFound, writer.Load(backup.scope, backup.identity).status);
		ASSERT_EQ(EWorkingCopyPersistenceWriteStatus::Persisted, writer.Save(backup, std::nullopt, "backup-corrupt-seed-" + std::to_string(mode)).status);
		auto manifest = FindManifest(script, "workbench.editor.backup");
		ASSERT_NE(script.entries.end(), manifest);
		if (mode == 0) manifest->second.value = "not an envelope";
		if (mode == 1) script.entries.erase(std::next(manifest)->first);
		if (mode == 2) {
			auto chunk = std::next(manifest);
			chunk->second.value += "!";
		}
		if (mode == 3) manifest->second.target = EStorageTarget::User;
		CControlPlatformWorkingCopyPersistenceStore reader(kProfile, Dependencies(script));
		EXPECT_EQ(EWorkingCopyPersistenceLoadStatus::InvalidStoredRecord, reader.Load(backup.scope, backup.identity).status);
		const auto requestCount = script.requests.size();
		EXPECT_EQ(EWorkingCopyPersistenceWriteStatus::Failed, reader.Save(backup, std::nullopt, "backup-invalid-save-" + std::to_string(mode)).status);
		EXPECT_EQ(EWorkingCopyPersistenceWriteStatus::Failed, reader.Delete(backup.scope, backup.identity, 1, "backup-invalid-delete-" + std::to_string(mode)).status);
		EXPECT_EQ(requestCount, script.requests.size());
	}
}

TEST(ControlPlatformWorkingCopyPersistenceStore, Sha256AddressIsBoundToDecodedIdentityAndScope)
{
	Script script;
	CControlPlatformWorkingCopyPersistenceStore writer(kProfile, Dependencies(script));
	auto first = Backup();
	auto second = Backup(WorkspaceScope());
	second.identity = Identity("untitled.logical.beta");
	ASSERT_EQ(EWorkingCopyPersistenceLoadStatus::NotFound, writer.Load(first.scope, first.identity).status);
	ASSERT_EQ(EWorkingCopyPersistenceWriteStatus::Persisted, writer.Save(first, std::nullopt, "backup-first-1").status);
	ASSERT_EQ(EWorkingCopyPersistenceLoadStatus::NotFound, writer.Load(second.scope, second.identity).status);
	ASSERT_EQ(EWorkingCopyPersistenceWriteStatus::Persisted, writer.Save(second, std::nullopt, "backup-second-1").status);
	ASSERT_EQ(2U, script.requests.size());
	ASSERT_EQ(script.requests[0].mutations.size(), script.requests[1].mutations.size());
	const auto& firstManifestKey = script.requests[0].mutations[0].address.key;
	ASSERT_EQ(std::string("backup.").size() + 64U + std::string(".manifest").size(), firstManifestKey.size());
	EXPECT_TRUE(std::all_of(firstManifestKey.begin() + std::string("backup.").size(),
		firstManifestKey.begin() + std::string("backup.").size() + 64, [](char character) {
			return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
		}));
	// Inject the first decoded record under the second record's deterministic
	// address. This models storage/address aliasing without weakening SHA-256.
	for (std::size_t index = 0; index < script.requests[0].mutations.size(); ++index) {
		const auto& from = script.entries.at(script.requests[0].mutations[index].address);
		auto copied = from;
		copied.address = script.requests[1].mutations[index].address;
		script.entries[copied.address] = std::move(copied);
	}
	CControlPlatformWorkingCopyPersistenceStore reader(kProfile, Dependencies(script));
	EXPECT_EQ(EWorkingCopyPersistenceLoadStatus::InvalidStoredRecord, reader.Load(second.scope, second.identity).status);
	EXPECT_EQ(EWorkingCopyPersistenceLoadStatus::Loaded, reader.Load(first.scope, first.identity).status);
	const auto requestCount = script.requests.size();
	EXPECT_EQ(EWorkingCopyPersistenceWriteStatus::Failed, reader.Save(second, std::nullopt, "backup-collision-save-1").status);
	EXPECT_EQ(EWorkingCopyPersistenceWriteStatus::Failed, reader.Delete(second.scope, second.identity, 1, "backup-collision-delete-1").status);
	EXPECT_EQ(requestCount, script.requests.size());
}

TEST(ControlPlatformWorkingCopyPersistenceStore, OversizeCodecPayloadIsRejectedBeforeApply)
{
	Script script;
	CControlPlatformWorkingCopyPersistenceStore store(kProfile, Dependencies(script));
	auto backup = Backup(ProfileScope(), 1, std::string(400 * 1024, 'z'));
	ASSERT_EQ(EWorkingCopyPersistenceLoadStatus::NotFound, store.Load(backup.scope, backup.identity).status);
	EXPECT_EQ(EWorkingCopyPersistenceWriteStatus::Failed, store.Save(backup, std::nullopt, "backup-oversize-1").status);
	EXPECT_TRUE(script.requests.empty());
}

} // namespace

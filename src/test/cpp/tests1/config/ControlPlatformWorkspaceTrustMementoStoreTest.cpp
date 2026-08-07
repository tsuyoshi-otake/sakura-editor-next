/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include "_main/ControlPlatformWorkspaceTrustMementoStore.h"
#include "config/WorkspaceTrustMementoCodec.h"

#include <deque>
#include <utility>
#include <vector>

namespace {

using config::ControlPlatformWorkspaceTrustMementoStoreDependencies;
using config::CWorkspaceTrustMementoCodec;
using config::EWorkspaceTrustMementoLoadStatus;
using config::EWorkspaceTrustMementoSaveStatus;
using config::WorkspaceTrustMemento;
using platform::controlipc::EEditorControlPlatformRuntimeState;
using platform::controlipc::EEditorControlStorageApplyCode;
using platform::controlipc::EEditorControlStorageCacheCoordinateCode;
using platform::controlipc::EditorControlStorageApplyResult;
using platform::controlipc::EditorControlStorageCacheCoordinateResult;
using platform::controlipc::EditorControlStorageCacheCoordinates;
using platform::storage::EStorageScope;
using platform::storage::EStorageTarget;
using platform::storage::StorageAddress;
using platform::storage::StorageEntry;
using platform::storage::StorageMutationRequest;

constexpr char kProfileId[] = "0123456789abcdef0123456789abcdef";
constexpr char kWorkspaceScopeId[] = "workspace-scope-0123456789abcdef";

StorageAddress WorkspaceTrustAddress()
{
	return { EStorageScope::Workspace, kWorkspaceScopeId, "workbench.trust", "workspaceMemento" };
}

WorkspaceTrustMemento Sample()
{
	WorkspaceTrustMemento memento;
	memento.startupPromptShown = true;
	memento.untrustedFilesAccepted = true;
	return memento;
}

std::string Payload(const WorkspaceTrustMemento& memento)
{
	const auto encoded = CWorkspaceTrustMementoCodec::Encode(memento);
	EXPECT_TRUE(encoded.Succeeded()) << encoded.diagnostic;
	return encoded.payload;
}

EditorControlStorageCacheCoordinateResult Coordinates(std::uint64_t revision)
{
	return { EEditorControlStorageCacheCoordinateCode::Ready, EEditorControlPlatformRuntimeState::Ready,
		EditorControlStorageCacheCoordinates{ kProfileId, 9, revision }, {} };
}

EditorControlStorageCacheCoordinateResult CoordinateFailureResult(EEditorControlStorageCacheCoordinateCode code)
{
	return { code, EEditorControlPlatformRuntimeState::Stopped, std::nullopt, {} };
}

struct Script final {
	std::deque<EditorControlStorageCacheCoordinateResult> coordinates;
	std::optional<StorageEntry> entry;
	std::deque<EditorControlStorageApplyResult> applyResults;
	std::vector<StorageMutationRequest> requests;
	int finds = 0;
};

ControlPlatformWorkspaceTrustMementoStoreDependencies Dependencies(Script& script)
{
	return {
		.storageCacheCoordinates = [&script] {
			EXPECT_FALSE(script.coordinates.empty());
			auto result = script.coordinates.front();
			script.coordinates.pop_front();
			return result;
		},
		.find = [&script](const StorageAddress& address) {
			++script.finds;
			EXPECT_EQ(WorkspaceTrustAddress(), address);
			return script.entry;
		},
		.apply = [&script](const StorageMutationRequest& request) {
			script.requests.push_back(request);
			EXPECT_FALSE(script.applyResults.empty());
			auto result = script.applyResults.front();
			script.applyResults.pop_front();
			return result;
		},
		.operationIdFactory = [] { return std::string("workbench.trust-test-operation-1"); },
	};
}

EditorControlStorageApplyResult ApplyResult(EEditorControlStorageApplyCode code, std::uint64_t revision = 0)
{
	EditorControlStorageApplyResult result;
	result.code = code;
	if (revision != 0) result.storageResult = platform::storage::StorageMutationResult{
		platform::storage::EStorageMutationStatus::Succeeded, revision, false, {}, std::nullopt };
	return result;
}

// ---------------------------------------------------------------------------
// Load
// ---------------------------------------------------------------------------

TEST(ControlPlatformWorkspaceTrustMementoStore, LoadWithNoWorkspaceScopeReturnsNoWorkspaceScope)
{
	Script script;
	CControlPlatformWorkspaceTrustMementoStore store(kProfileId, std::nullopt, Dependencies(script));

	const auto result = store.Load();
	EXPECT_EQ(EWorkspaceTrustMementoLoadStatus::NoWorkspaceScope, result.status);
	EXPECT_FALSE(result.memento.has_value());
	EXPECT_EQ(0, script.finds);
}

TEST(ControlPlatformWorkspaceTrustMementoStore, NotFoundIsReadableWithAskAgainDefaults)
{
	// Regression guard: only NotFound (not just Loaded) may be Readable(), or a
	// once-only prompt would ask forever on the first launch, when nothing has
	// ever been written yet.
	Script script;
	script.coordinates = { Coordinates(41), Coordinates(41) };
	script.entry = std::nullopt;
	CControlPlatformWorkspaceTrustMementoStore store(kProfileId, kWorkspaceScopeId, Dependencies(script));

	const auto result = store.Load();
	ASSERT_EQ(EWorkspaceTrustMementoLoadStatus::NotFound, result.status);
	EXPECT_TRUE(result.Readable());
	const auto value = result.ValueOrDefault();
	EXPECT_FALSE(value.startupPromptShown);
	EXPECT_FALSE(value.untrustedFilesAccepted);
}

TEST(ControlPlatformWorkspaceTrustMementoStore, InvalidStoredMementoIsNotReadable)
{
	Script script;
	script.coordinates = { Coordinates(41), Coordinates(41) };
	script.entry = StorageEntry{ WorkspaceTrustAddress(), EStorageTarget::Machine, "{not valid json", 41 };
	CControlPlatformWorkspaceTrustMementoStore store(kProfileId, kWorkspaceScopeId, Dependencies(script));

	const auto result = store.Load();
	ASSERT_EQ(EWorkspaceTrustMementoLoadStatus::InvalidStoredMemento, result.status);
	EXPECT_FALSE(result.Readable());
}

TEST(ControlPlatformWorkspaceTrustMementoStore, LoadRequiresOneCoherentGlobalStorageRevision)
{
	Script script;
	script.coordinates = { Coordinates(41), Coordinates(42) };
	script.entry = StorageEntry{ WorkspaceTrustAddress(), EStorageTarget::Machine, Payload(Sample()), 41 };
	CControlPlatformWorkspaceTrustMementoStore store(kProfileId, kWorkspaceScopeId, Dependencies(script));

	const auto result = store.Load();
	EXPECT_EQ(EWorkspaceTrustMementoLoadStatus::Unavailable, result.status);
	EXPECT_FALSE(result.memento.has_value());
	EXPECT_EQ(1, script.finds);
}

TEST(ControlPlatformWorkspaceTrustMementoStore, LoadMapsEveryUnavailableCoordinateCodeToUnavailable)
{
	for (const auto code : { EEditorControlStorageCacheCoordinateCode::Resynchronizing,
			 EEditorControlStorageCacheCoordinateCode::DegradedUnavailable,
			 EEditorControlStorageCacheCoordinateCode::Stopping,
			 EEditorControlStorageCacheCoordinateCode::Stopped }) {
		Script script;
		script.coordinates = { CoordinateFailureResult(code) };
		CControlPlatformWorkspaceTrustMementoStore store(kProfileId, kWorkspaceScopeId, Dependencies(script));

		const auto result = store.Load();
		EXPECT_EQ(EWorkspaceTrustMementoLoadStatus::Unavailable, result.status) << static_cast<int>(code);
		EXPECT_FALSE(result.memento.has_value());
		EXPECT_EQ(0, script.finds);
	}
}

TEST(ControlPlatformWorkspaceTrustMementoStore, LoadMapsFailedCoordinateCodeToFailed)
{
	Script script;
	script.coordinates = { CoordinateFailureResult(EEditorControlStorageCacheCoordinateCode::Failed) };
	CControlPlatformWorkspaceTrustMementoStore store(kProfileId, kWorkspaceScopeId, Dependencies(script));

	const auto result = store.Load();
	EXPECT_EQ(EWorkspaceTrustMementoLoadStatus::Failed, result.status);
	EXPECT_FALSE(result.memento.has_value());
	EXPECT_EQ(0, script.finds);
}

TEST(ControlPlatformWorkspaceTrustMementoStore, LoadDecodesStoredMementoRoundTrip)
{
	const auto sample = Sample();
	Script script;
	script.coordinates = { Coordinates(41), Coordinates(41) };
	script.entry = StorageEntry{ WorkspaceTrustAddress(), EStorageTarget::Machine, Payload(sample), 41 };
	CControlPlatformWorkspaceTrustMementoStore store(kProfileId, kWorkspaceScopeId, Dependencies(script));

	const auto result = store.Load();
	ASSERT_EQ(EWorkspaceTrustMementoLoadStatus::Loaded, result.status);
	ASSERT_TRUE(result.memento.has_value());
	EXPECT_TRUE(result.memento->startupPromptShown);
	EXPECT_TRUE(result.memento->untrustedFilesAccepted);
}

TEST(ControlPlatformWorkspaceTrustMementoStore, WrongTargetIsInvalidAndCannotBeOverwritten)
{
	Script script;
	script.coordinates = { Coordinates(41), Coordinates(41) };
	script.entry = StorageEntry{ WorkspaceTrustAddress(), EStorageTarget::User, Payload(Sample()), 41 };
	CControlPlatformWorkspaceTrustMementoStore store(kProfileId, kWorkspaceScopeId, Dependencies(script));

	EXPECT_EQ(EWorkspaceTrustMementoLoadStatus::InvalidStoredMemento, store.Load().status);
	EXPECT_EQ(EWorkspaceTrustMementoSaveStatus::Failed, store.Save(Sample()).status);
	EXPECT_TRUE(script.requests.empty());
}

TEST(ControlPlatformWorkspaceTrustMementoStore, CorruptPayloadIsPreservedAndCannotBeOverwritten)
{
	Script script;
	script.coordinates = { Coordinates(41), Coordinates(41) };
	script.entry = StorageEntry{ WorkspaceTrustAddress(), EStorageTarget::Machine, "{not valid json", 41 };
	CControlPlatformWorkspaceTrustMementoStore store(kProfileId, kWorkspaceScopeId, Dependencies(script));

	EXPECT_EQ(EWorkspaceTrustMementoLoadStatus::InvalidStoredMemento, store.Load().status);
	EXPECT_EQ(EWorkspaceTrustMementoSaveStatus::Failed, store.Save(Sample()).status);
	EXPECT_TRUE(script.requests.empty());
}

// ---------------------------------------------------------------------------
// Save
// ---------------------------------------------------------------------------

TEST(ControlPlatformWorkspaceTrustMementoStore, SaveWithNoWorkspaceScopeReturnsNoWorkspaceScope)
{
	Script script;
	CControlPlatformWorkspaceTrustMementoStore store(kProfileId, std::nullopt, Dependencies(script));

	const auto result = store.Save(Sample());
	EXPECT_EQ(EWorkspaceTrustMementoSaveStatus::NoWorkspaceScope, result.status);
	EXPECT_TRUE(script.requests.empty());
}

TEST(ControlPlatformWorkspaceTrustMementoStore, SaveWithoutPriorLoadIsFailed)
{
	Script script;
	CControlPlatformWorkspaceTrustMementoStore store(kProfileId, kWorkspaceScopeId, Dependencies(script));

	const auto result = store.Save(Sample());
	EXPECT_EQ(EWorkspaceTrustMementoSaveStatus::Failed, result.status);
	EXPECT_TRUE(script.requests.empty());
}

TEST(ControlPlatformWorkspaceTrustMementoStore, ExistingCanonicalPayloadIsNotDirty)
{
	const auto payload = Payload(Sample());
	Script script;
	script.coordinates = { Coordinates(41), Coordinates(41) };
	script.entry = StorageEntry{ WorkspaceTrustAddress(), EStorageTarget::Machine, payload, 41 };
	CControlPlatformWorkspaceTrustMementoStore store(kProfileId, kWorkspaceScopeId, Dependencies(script));

	ASSERT_EQ(EWorkspaceTrustMementoLoadStatus::Loaded, store.Load().status);
	EXPECT_EQ(EWorkspaceTrustMementoSaveStatus::NotDirty, store.Save(Sample()).status);
	EXPECT_TRUE(script.requests.empty());
}

TEST(ControlPlatformWorkspaceTrustMementoStore, PersistUsesCapturedGlobalRevisionCas)
{
	Script script;
	script.coordinates = { Coordinates(41), Coordinates(41) };
	script.applyResults = { ApplyResult(EEditorControlStorageApplyCode::Succeeded, 42) };
	CControlPlatformWorkspaceTrustMementoStore store(kProfileId, kWorkspaceScopeId, Dependencies(script));

	ASSERT_EQ(EWorkspaceTrustMementoLoadStatus::NotFound, store.Load().status);
	EXPECT_EQ(EWorkspaceTrustMementoSaveStatus::Persisted, store.Save(Sample()).status);
	ASSERT_EQ(1U, script.requests.size());
	ASSERT_TRUE(script.requests.front().expectedRevision.has_value());
	EXPECT_EQ(41U, *script.requests.front().expectedRevision);
	ASSERT_EQ(1U, script.requests.front().mutations.size());
	EXPECT_EQ(WorkspaceTrustAddress(), script.requests.front().mutations.front().address);
	EXPECT_EQ(EStorageTarget::Machine, script.requests.front().mutations.front().target);
}

TEST(ControlPlatformWorkspaceTrustMementoStore, ConflictIsTerminalAndDoesNotRetry)
{
	Script script;
	script.coordinates = { Coordinates(41), Coordinates(41) };
	script.applyResults = { ApplyResult(EEditorControlStorageApplyCode::ConflictResnapshotScheduled) };
	CControlPlatformWorkspaceTrustMementoStore store(kProfileId, kWorkspaceScopeId, Dependencies(script));

	ASSERT_EQ(EWorkspaceTrustMementoLoadStatus::NotFound, store.Load().status);
	EXPECT_EQ(EWorkspaceTrustMementoSaveStatus::Conflict, store.Save(Sample()).status);
	EXPECT_EQ(1U, script.requests.size());
}

TEST(ControlPlatformWorkspaceTrustMementoStore, AmbiguousRetryReusesTheExactImmutableRequestAndOperationId)
{
	Script script;
	script.coordinates = { Coordinates(41), Coordinates(41) };
	script.applyResults = {
		ApplyResult(EEditorControlStorageApplyCode::RetryWithSameOperationId),
		ApplyResult(EEditorControlStorageApplyCode::Succeeded, 42),
	};
	CControlPlatformWorkspaceTrustMementoStore store(kProfileId, kWorkspaceScopeId, Dependencies(script));

	ASSERT_EQ(EWorkspaceTrustMementoLoadStatus::NotFound, store.Load().status);
	EXPECT_EQ(EWorkspaceTrustMementoSaveStatus::Persisted, store.Save(Sample()).status);
	ASSERT_EQ(2U, script.requests.size());
	EXPECT_EQ(script.requests[0], script.requests[1]);
	EXPECT_EQ("workbench.trust-test-operation-1", script.requests[0].operationId);
	EXPECT_EQ(script.requests[0].operationId, script.requests[1].operationId);
}

TEST(ControlPlatformWorkspaceTrustMementoStore, ASecondAmbiguousOutcomeExhaustsTheSingleRetry)
{
	Script script;
	script.coordinates = { Coordinates(41), Coordinates(41) };
	script.applyResults = {
		ApplyResult(EEditorControlStorageApplyCode::RetryWithSameOperationId),
		ApplyResult(EEditorControlStorageApplyCode::RetryWithSameOperationId),
	};
	CControlPlatformWorkspaceTrustMementoStore store(kProfileId, kWorkspaceScopeId, Dependencies(script));

	ASSERT_EQ(EWorkspaceTrustMementoLoadStatus::NotFound, store.Load().status);
	EXPECT_EQ(EWorkspaceTrustMementoSaveStatus::RetryExhausted, store.Save(Sample()).status);
	ASSERT_EQ(2U, script.requests.size());
	EXPECT_EQ(script.requests[0], script.requests[1]);
}

TEST(ControlPlatformWorkspaceTrustMementoStore, ApplyNotReadyIsUnavailableAndDoesNotRetry)
{
	Script script;
	script.coordinates = { Coordinates(41), Coordinates(41) };
	script.applyResults = { ApplyResult(EEditorControlStorageApplyCode::NotReady) };
	CControlPlatformWorkspaceTrustMementoStore store(kProfileId, kWorkspaceScopeId, Dependencies(script));

	ASSERT_EQ(EWorkspaceTrustMementoLoadStatus::NotFound, store.Load().status);
	EXPECT_EQ(EWorkspaceTrustMementoSaveStatus::Unavailable, store.Save(Sample()).status);
	EXPECT_EQ(1U, script.requests.size());
}

TEST(ControlPlatformWorkspaceTrustMementoStore, ApplyStoppedIsStoppedAndDoesNotRetry)
{
	Script script;
	script.coordinates = { Coordinates(41), Coordinates(41) };
	script.applyResults = { ApplyResult(EEditorControlStorageApplyCode::Stopped) };
	CControlPlatformWorkspaceTrustMementoStore store(kProfileId, kWorkspaceScopeId, Dependencies(script));

	ASSERT_EQ(EWorkspaceTrustMementoLoadStatus::NotFound, store.Load().status);
	EXPECT_EQ(EWorkspaceTrustMementoSaveStatus::Stopped, store.Save(Sample()).status);
	EXPECT_EQ(1U, script.requests.size());
}

TEST(ControlPlatformWorkspaceTrustMementoStore, ApplyFailedIsFailedAndDoesNotRetry)
{
	Script script;
	script.coordinates = { Coordinates(41), Coordinates(41) };
	script.applyResults = { ApplyResult(EEditorControlStorageApplyCode::Failed) };
	CControlPlatformWorkspaceTrustMementoStore store(kProfileId, kWorkspaceScopeId, Dependencies(script));

	ASSERT_EQ(EWorkspaceTrustMementoLoadStatus::NotFound, store.Load().status);
	EXPECT_EQ(EWorkspaceTrustMementoSaveStatus::Failed, store.Save(Sample()).status);
	EXPECT_EQ(1U, script.requests.size());
}

} // namespace

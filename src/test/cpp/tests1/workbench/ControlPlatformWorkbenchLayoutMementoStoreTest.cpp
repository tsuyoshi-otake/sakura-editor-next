/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include "_main/ControlPlatformWorkbenchLayoutMementoStore.h"
#include "workbench/layout/WorkbenchLayoutMementoCodec.h"

#include <deque>
#include <utility>
#include <vector>

namespace {

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
using workbench::layout::ControlPlatformWorkbenchLayoutMementoStoreDependencies;
using workbench::layout::CWorkbenchLayoutMementoCodec;
using workbench::layout::EWorkbenchLayoutMementoLoadStatus;
using workbench::layout::EWorkbenchLayoutMementoSaveStatus;
using workbench::layout::WorkbenchLayoutStateSnapshot;

constexpr char kProfileId[] = "0123456789abcdef0123456789abcdef";

StorageAddress MementoAddress()
{
	return { EStorageScope::Profile, kProfileId, "workbench.layout", "state" };
}

WorkbenchLayoutStateSnapshot Sample()
{
	WorkbenchLayoutStateSnapshot snapshot;
	snapshot.parts = { { "workbench.parts.editor", true, workbench::layout::EWorkbenchPartPosition::Center, std::nullopt } };
	return snapshot;
}

std::string Payload(const WorkbenchLayoutStateSnapshot& snapshot)
{
	const auto encoded = CWorkbenchLayoutMementoCodec::Encode(snapshot);
	EXPECT_TRUE(encoded.Succeeded()) << encoded.diagnostic;
	return encoded.payload;
}

EditorControlStorageCacheCoordinateResult Coordinates(std::uint64_t revision)
{
	return { EEditorControlStorageCacheCoordinateCode::Ready,
		platform::controlipc::EEditorControlPlatformRuntimeState::Ready,
		EditorControlStorageCacheCoordinates{ kProfileId, 9, revision }, {} };
}

struct Script final {
	std::deque<EditorControlStorageCacheCoordinateResult> coordinates;
	std::optional<StorageEntry> entry;
	std::deque<EditorControlStorageApplyResult> applyResults;
	std::vector<StorageMutationRequest> requests;
	int finds = 0;
};

ControlPlatformWorkbenchLayoutMementoStoreDependencies Dependencies(Script& script)
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
			EXPECT_EQ(MementoAddress(), address);
			return script.entry;
		},
		.apply = [&script](const StorageMutationRequest& request) {
			script.requests.push_back(request);
			EXPECT_FALSE(script.applyResults.empty());
			auto result = script.applyResults.front();
			script.applyResults.pop_front();
			return result;
		},
		.operationIdFactory = [] { return std::string("workbench.layout-test-operation-1"); },
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

TEST(ControlPlatformWorkbenchLayoutMementoStore, LoadRequiresOneCoherentGlobalStorageRevision)
{
	Script script;
	script.coordinates = { Coordinates(41), Coordinates(42) };
	script.entry = StorageEntry{ MementoAddress(), EStorageTarget::Machine, Payload(Sample()), 41 };
	CControlPlatformWorkbenchLayoutMementoStore store(kProfileId, Dependencies(script));

	const auto result = store.Load();
	EXPECT_EQ(EWorkbenchLayoutMementoLoadStatus::Unavailable, result.status);
	EXPECT_FALSE(result.snapshot.has_value());
	EXPECT_EQ(1, script.finds);
}

TEST(ControlPlatformWorkbenchLayoutMementoStore, WrongTargetIsInvalidAndCannotBeOverwritten)
{
	Script script;
	script.coordinates = { Coordinates(41), Coordinates(41) };
	script.entry = StorageEntry{ MementoAddress(), EStorageTarget::User, Payload(Sample()), 41 };
	CControlPlatformWorkbenchLayoutMementoStore store(kProfileId, Dependencies(script));

	EXPECT_EQ(EWorkbenchLayoutMementoLoadStatus::InvalidStoredMemento, store.Load().status);
	EXPECT_EQ(EWorkbenchLayoutMementoSaveStatus::Failed, store.Save(Sample()).status);
	EXPECT_TRUE(script.requests.empty());
}

TEST(ControlPlatformWorkbenchLayoutMementoStore, CorruptPayloadIsPreservedAndCannotBeOverwritten)
{
	Script script;
	script.coordinates = { Coordinates(41), Coordinates(41) };
	script.entry = StorageEntry{ MementoAddress(), EStorageTarget::Machine, "{not valid json", 41 };
	CControlPlatformWorkbenchLayoutMementoStore store(kProfileId, Dependencies(script));

	EXPECT_EQ(EWorkbenchLayoutMementoLoadStatus::InvalidStoredMemento, store.Load().status);
	EXPECT_EQ(EWorkbenchLayoutMementoSaveStatus::Failed, store.Save(Sample()).status);
	EXPECT_TRUE(script.requests.empty());
}

TEST(ControlPlatformWorkbenchLayoutMementoStore, ExistingCanonicalPayloadIsNotDirty)
{
	const auto payload = Payload(Sample());
	Script script;
	script.coordinates = { Coordinates(41), Coordinates(41) };
	script.entry = StorageEntry{ MementoAddress(), EStorageTarget::Machine, payload, 41 };
	CControlPlatformWorkbenchLayoutMementoStore store(kProfileId, Dependencies(script));

	ASSERT_EQ(EWorkbenchLayoutMementoLoadStatus::Loaded, store.Load().status);
	EXPECT_EQ(EWorkbenchLayoutMementoSaveStatus::NotDirty, store.Save(Sample()).status);
	EXPECT_TRUE(script.requests.empty());
}

TEST(ControlPlatformWorkbenchLayoutMementoStore, PersistUsesCapturedGlobalRevisionCas)
{
	Script script;
	script.coordinates = { Coordinates(41), Coordinates(41) };
	script.applyResults = { ApplyResult(EEditorControlStorageApplyCode::Succeeded, 42) };
	CControlPlatformWorkbenchLayoutMementoStore store(kProfileId, Dependencies(script));

	ASSERT_EQ(EWorkbenchLayoutMementoLoadStatus::NotFound, store.Load().status);
	EXPECT_EQ(EWorkbenchLayoutMementoSaveStatus::Persisted, store.Save(Sample()).status);
	ASSERT_EQ(1U, script.requests.size());
	ASSERT_TRUE(script.requests.front().expectedRevision.has_value());
	EXPECT_EQ(41U, *script.requests.front().expectedRevision);
	ASSERT_EQ(1U, script.requests.front().mutations.size());
	EXPECT_EQ(EStorageTarget::Machine, script.requests.front().mutations.front().target);
}

TEST(ControlPlatformWorkbenchLayoutMementoStore, ConflictIsTerminalAndDoesNotRetry)
{
	Script script;
	script.coordinates = { Coordinates(41), Coordinates(41) };
	script.applyResults = { ApplyResult(EEditorControlStorageApplyCode::ConflictResnapshotScheduled) };
	CControlPlatformWorkbenchLayoutMementoStore store(kProfileId, Dependencies(script));

	ASSERT_EQ(EWorkbenchLayoutMementoLoadStatus::NotFound, store.Load().status);
	EXPECT_EQ(EWorkbenchLayoutMementoSaveStatus::Conflict, store.Save(Sample()).status);
	EXPECT_EQ(1U, script.requests.size());
}

TEST(ControlPlatformWorkbenchLayoutMementoStore, AmbiguousRetryReusesTheExactImmutableRequestAndOperationId)
{
	Script script;
	script.coordinates = { Coordinates(41), Coordinates(41) };
	script.applyResults = {
		ApplyResult(EEditorControlStorageApplyCode::RetryWithSameOperationId),
		ApplyResult(EEditorControlStorageApplyCode::Succeeded, 42),
	};
	CControlPlatformWorkbenchLayoutMementoStore store(kProfileId, Dependencies(script));

	ASSERT_EQ(EWorkbenchLayoutMementoLoadStatus::NotFound, store.Load().status);
	EXPECT_EQ(EWorkbenchLayoutMementoSaveStatus::Persisted, store.Save(Sample()).status);
	ASSERT_EQ(2U, script.requests.size());
	EXPECT_EQ(script.requests[0], script.requests[1]);
	EXPECT_EQ("workbench.layout-test-operation-1", script.requests[0].operationId);
}

TEST(ControlPlatformWorkbenchLayoutMementoStore, ASecondAmbiguousOutcomeExhaustsTheSingleRetry)
{
	Script script;
	script.coordinates = { Coordinates(41), Coordinates(41) };
	script.applyResults = {
		ApplyResult(EEditorControlStorageApplyCode::RetryWithSameOperationId),
		ApplyResult(EEditorControlStorageApplyCode::RetryWithSameOperationId),
	};
	CControlPlatformWorkbenchLayoutMementoStore store(kProfileId, Dependencies(script));

	ASSERT_EQ(EWorkbenchLayoutMementoLoadStatus::NotFound, store.Load().status);
	EXPECT_EQ(EWorkbenchLayoutMementoSaveStatus::RetryExhausted, store.Save(Sample()).status);
	ASSERT_EQ(2U, script.requests.size());
	EXPECT_EQ(script.requests[0], script.requests[1]);
}

} // namespace

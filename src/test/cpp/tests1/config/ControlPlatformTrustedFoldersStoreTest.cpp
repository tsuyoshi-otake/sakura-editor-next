/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include "_main/ControlPlatformTrustedFoldersStore.h"
#include "config/TrustedFoldersCodec.h"

#include <deque>
#include <utility>
#include <vector>

namespace {

using config::CTrustedFoldersCodec;
using config::ControlPlatformTrustedFoldersStoreDependencies;
using config::ETrustedFoldersLoadStatus;
using config::ETrustedFoldersSaveStatus;
using config::TrustedFoldersSnapshot;
using config::WorkspaceTrustEntry;
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
using platform::uri::Uri;

constexpr char kProfileId[] = "0123456789abcdef0123456789abcdef";

StorageAddress TrustedFoldersAddress()
{
	return { EStorageScope::Profile, kProfileId, "workbench.trust", "trustedFolders" };
}

Uri ParseUri(const wchar_t* text)
{
	auto parsed = Uri::Parse(text);
	EXPECT_TRUE(parsed);
	return std::move(*parsed.value);
}

WorkspaceTrustEntry Entry(const wchar_t* text, bool includesDescendants)
{
	return { ParseUri(text), includesDescendants };
}

TrustedFoldersSnapshot Sample()
{
	TrustedFoldersSnapshot snapshot;
	snapshot.entries.push_back(Entry(L"file:///c:/codes/app", true));
	return snapshot;
}

std::string Payload(const TrustedFoldersSnapshot& snapshot)
{
	const auto encoded = CTrustedFoldersCodec::Encode(snapshot);
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

ControlPlatformTrustedFoldersStoreDependencies Dependencies(Script& script)
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
			EXPECT_EQ(TrustedFoldersAddress(), address);
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

TEST(ControlPlatformTrustedFoldersStore, LoadRequiresOneCoherentGlobalStorageRevision)
{
	Script script;
	script.coordinates = { Coordinates(41), Coordinates(42) };
	script.entry = StorageEntry{ TrustedFoldersAddress(), EStorageTarget::Machine, Payload(Sample()), 41 };
	CControlPlatformTrustedFoldersStore store(kProfileId, Dependencies(script));

	const auto result = store.Load();
	EXPECT_EQ(ETrustedFoldersLoadStatus::Unavailable, result.status);
	EXPECT_FALSE(result.snapshot.has_value());
	EXPECT_EQ(1, script.finds);
}

TEST(ControlPlatformTrustedFoldersStore, LoadMapsEveryUnavailableCoordinateCodeToUnavailable)
{
	for (const auto code : { EEditorControlStorageCacheCoordinateCode::Resynchronizing,
			 EEditorControlStorageCacheCoordinateCode::DegradedUnavailable,
			 EEditorControlStorageCacheCoordinateCode::Stopping,
			 EEditorControlStorageCacheCoordinateCode::Stopped }) {
		Script script;
		script.coordinates = { CoordinateFailureResult(code) };
		CControlPlatformTrustedFoldersStore store(kProfileId, Dependencies(script));

		const auto result = store.Load();
		EXPECT_EQ(ETrustedFoldersLoadStatus::Unavailable, result.status) << static_cast<int>(code);
		EXPECT_FALSE(result.snapshot.has_value());
		EXPECT_EQ(0, script.finds);
	}
}

TEST(ControlPlatformTrustedFoldersStore, LoadMapsFailedCoordinateCodeToFailed)
{
	Script script;
	script.coordinates = { CoordinateFailureResult(EEditorControlStorageCacheCoordinateCode::Failed) };
	CControlPlatformTrustedFoldersStore store(kProfileId, Dependencies(script));

	const auto result = store.Load();
	EXPECT_EQ(ETrustedFoldersLoadStatus::Failed, result.status);
	EXPECT_FALSE(result.snapshot.has_value());
	EXPECT_EQ(0, script.finds);
}

TEST(ControlPlatformTrustedFoldersStore, LoadDecodesStoredEntriesIntoSnapshot)
{
	const auto snapshot = Sample();
	Script script;
	script.coordinates = { Coordinates(41), Coordinates(41) };
	script.entry = StorageEntry{ TrustedFoldersAddress(), EStorageTarget::Machine, Payload(snapshot), 41 };
	CControlPlatformTrustedFoldersStore store(kProfileId, Dependencies(script));

	const auto result = store.Load();
	ASSERT_EQ(ETrustedFoldersLoadStatus::Loaded, result.status);
	ASSERT_TRUE(result.snapshot.has_value());
	ASSERT_EQ(1U, result.snapshot->entries.size());
	EXPECT_EQ(snapshot.entries[0].uri.ToString(), result.snapshot->entries[0].uri.ToString());
	EXPECT_EQ(snapshot.entries[0].includesDescendants, result.snapshot->entries[0].includesDescendants);
}

TEST(ControlPlatformTrustedFoldersStore, WrongTargetIsInvalidAndCannotBeOverwritten)
{
	Script script;
	script.coordinates = { Coordinates(41), Coordinates(41) };
	script.entry = StorageEntry{ TrustedFoldersAddress(), EStorageTarget::User, Payload(Sample()), 41 };
	CControlPlatformTrustedFoldersStore store(kProfileId, Dependencies(script));

	EXPECT_EQ(ETrustedFoldersLoadStatus::InvalidStoredList, store.Load().status);
	EXPECT_EQ(ETrustedFoldersSaveStatus::Failed, store.Save(Sample()).status);
	EXPECT_TRUE(script.requests.empty());
}

TEST(ControlPlatformTrustedFoldersStore, CorruptPayloadIsPreservedAndCannotBeOverwritten)
{
	Script script;
	script.coordinates = { Coordinates(41), Coordinates(41) };
	script.entry = StorageEntry{ TrustedFoldersAddress(), EStorageTarget::Machine, "{not valid json", 41 };
	CControlPlatformTrustedFoldersStore store(kProfileId, Dependencies(script));

	EXPECT_EQ(ETrustedFoldersLoadStatus::InvalidStoredList, store.Load().status);
	EXPECT_EQ(ETrustedFoldersSaveStatus::Failed, store.Save(Sample()).status);
	EXPECT_TRUE(script.requests.empty());
}

// ---------------------------------------------------------------------------
// Save
// ---------------------------------------------------------------------------

TEST(ControlPlatformTrustedFoldersStore, ExistingCanonicalPayloadIsNotDirty)
{
	const auto payload = Payload(Sample());
	Script script;
	script.coordinates = { Coordinates(41), Coordinates(41) };
	script.entry = StorageEntry{ TrustedFoldersAddress(), EStorageTarget::Machine, payload, 41 };
	CControlPlatformTrustedFoldersStore store(kProfileId, Dependencies(script));

	ASSERT_EQ(ETrustedFoldersLoadStatus::Loaded, store.Load().status);
	EXPECT_EQ(ETrustedFoldersSaveStatus::NotDirty, store.Save(Sample()).status);
	EXPECT_TRUE(script.requests.empty());
}

TEST(ControlPlatformTrustedFoldersStore, PersistUsesCapturedGlobalRevisionCas)
{
	Script script;
	script.coordinates = { Coordinates(41), Coordinates(41) };
	script.applyResults = { ApplyResult(EEditorControlStorageApplyCode::Succeeded, 42) };
	CControlPlatformTrustedFoldersStore store(kProfileId, Dependencies(script));

	ASSERT_EQ(ETrustedFoldersLoadStatus::NotFound, store.Load().status);
	EXPECT_EQ(ETrustedFoldersSaveStatus::Persisted, store.Save(Sample()).status);
	ASSERT_EQ(1U, script.requests.size());
	ASSERT_TRUE(script.requests.front().expectedRevision.has_value());
	EXPECT_EQ(41U, *script.requests.front().expectedRevision);
	ASSERT_EQ(1U, script.requests.front().mutations.size());
	EXPECT_EQ(EStorageTarget::Machine, script.requests.front().mutations.front().target);
}

TEST(ControlPlatformTrustedFoldersStore, ConflictIsTerminalAndDoesNotRetry)
{
	Script script;
	script.coordinates = { Coordinates(41), Coordinates(41) };
	script.applyResults = { ApplyResult(EEditorControlStorageApplyCode::ConflictResnapshotScheduled) };
	CControlPlatformTrustedFoldersStore store(kProfileId, Dependencies(script));

	ASSERT_EQ(ETrustedFoldersLoadStatus::NotFound, store.Load().status);
	EXPECT_EQ(ETrustedFoldersSaveStatus::Conflict, store.Save(Sample()).status);
	EXPECT_EQ(1U, script.requests.size());
}

TEST(ControlPlatformTrustedFoldersStore, AmbiguousRetryReusesTheExactImmutableRequestAndOperationId)
{
	Script script;
	script.coordinates = { Coordinates(41), Coordinates(41) };
	script.applyResults = {
		ApplyResult(EEditorControlStorageApplyCode::RetryWithSameOperationId),
		ApplyResult(EEditorControlStorageApplyCode::Succeeded, 42),
	};
	CControlPlatformTrustedFoldersStore store(kProfileId, Dependencies(script));

	ASSERT_EQ(ETrustedFoldersLoadStatus::NotFound, store.Load().status);
	EXPECT_EQ(ETrustedFoldersSaveStatus::Persisted, store.Save(Sample()).status);
	ASSERT_EQ(2U, script.requests.size());
	EXPECT_EQ(script.requests[0], script.requests[1]);
	EXPECT_EQ("workbench.trust-test-operation-1", script.requests[0].operationId);
	EXPECT_EQ(script.requests[0].operationId, script.requests[1].operationId);
}

TEST(ControlPlatformTrustedFoldersStore, ASecondAmbiguousOutcomeExhaustsTheSingleRetry)
{
	Script script;
	script.coordinates = { Coordinates(41), Coordinates(41) };
	script.applyResults = {
		ApplyResult(EEditorControlStorageApplyCode::RetryWithSameOperationId),
		ApplyResult(EEditorControlStorageApplyCode::RetryWithSameOperationId),
	};
	CControlPlatformTrustedFoldersStore store(kProfileId, Dependencies(script));

	ASSERT_EQ(ETrustedFoldersLoadStatus::NotFound, store.Load().status);
	EXPECT_EQ(ETrustedFoldersSaveStatus::RetryExhausted, store.Save(Sample()).status);
	ASSERT_EQ(2U, script.requests.size());
	EXPECT_EQ(script.requests[0], script.requests[1]);
}

TEST(ControlPlatformTrustedFoldersStore, ApplyNotReadyIsUnavailableAndDoesNotRetry)
{
	Script script;
	script.coordinates = { Coordinates(41), Coordinates(41) };
	script.applyResults = { ApplyResult(EEditorControlStorageApplyCode::NotReady) };
	CControlPlatformTrustedFoldersStore store(kProfileId, Dependencies(script));

	ASSERT_EQ(ETrustedFoldersLoadStatus::NotFound, store.Load().status);
	EXPECT_EQ(ETrustedFoldersSaveStatus::Unavailable, store.Save(Sample()).status);
	EXPECT_EQ(1U, script.requests.size());
}

TEST(ControlPlatformTrustedFoldersStore, ApplyStoppedIsStoppedAndDoesNotRetry)
{
	Script script;
	script.coordinates = { Coordinates(41), Coordinates(41) };
	script.applyResults = { ApplyResult(EEditorControlStorageApplyCode::Stopped) };
	CControlPlatformTrustedFoldersStore store(kProfileId, Dependencies(script));

	ASSERT_EQ(ETrustedFoldersLoadStatus::NotFound, store.Load().status);
	EXPECT_EQ(ETrustedFoldersSaveStatus::Stopped, store.Save(Sample()).status);
	EXPECT_EQ(1U, script.requests.size());
}

TEST(ControlPlatformTrustedFoldersStore, ApplyFailedIsFailedAndDoesNotRetry)
{
	Script script;
	script.coordinates = { Coordinates(41), Coordinates(41) };
	script.applyResults = { ApplyResult(EEditorControlStorageApplyCode::Failed) };
	CControlPlatformTrustedFoldersStore store(kProfileId, Dependencies(script));

	ASSERT_EQ(ETrustedFoldersLoadStatus::NotFound, store.Load().status);
	EXPECT_EQ(ETrustedFoldersSaveStatus::Failed, store.Save(Sample()).status);
	EXPECT_EQ(1U, script.requests.size());
}

} // namespace

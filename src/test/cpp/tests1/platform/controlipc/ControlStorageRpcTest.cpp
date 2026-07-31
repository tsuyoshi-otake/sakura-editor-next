/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include "platform/controlipc/ControlStorageRpc.h"
#include "platform/storage/CInMemoryStorageService.h"

#include <stdexcept>
#include <utility>

namespace platform::controlipc {
namespace {

ControlIpcFrame Request(EControlIpcKind kind, std::uint64_t requestId, std::uint64_t generation,
	std::vector<std::uint8_t> payload = {})
{
	return { { kControlIpcMajorVersion, kControlIpcMinorVersion, kind, EControlIpcFlags::Request,
		requestId, generation }, std::move(payload) };
}

storage::StorageAddress Address(std::string key)
{
	return { storage::EStorageScope::Profile, "profile-a", "tests", std::move(key) };
}

storage::StorageMutationRequest Put(std::string operationId, std::string key, std::string value,
	std::optional<std::uint64_t> expected = std::nullopt)
{
	return { std::move(operationId), expected,
		{ { Address(std::move(key)), storage::EStorageTarget::User, std::move(value) } } };
}

ControlStorageRpcSessionIdentity Identity()
{
	return { "profile-a", 9 };
}

void Handshake(CControlStorageRpcSession& session)
{
	const auto hello = EncodeControlStorageHello("profile-a");
	ASSERT_TRUE(hello);
	const auto response = session.Process(Request(EControlIpcKind::Hello, 1, 0, *hello));
	EXPECT_EQ(EControlIpcKind::HelloAck, response.header.kind);
	EXPECT_EQ(9u, response.header.generation);
	EXPECT_TRUE(session.IsHandshaken());
}

class CThrowingStorage final : public storage::IStorageService {
public:
	storage::StorageMutationResult Apply(const storage::StorageMutationRequest&) override { throw std::runtime_error("apply"); }
	storage::StorageSnapshot Snapshot() const override { throw std::runtime_error("snapshot"); }
	std::unique_ptr<storage::IStorageChangeSubscription> Subscribe(storage::StorageChangeCallback) override { return nullptr; }
};

TEST(ControlStorageRpc, RequiresExactHelloProfileAndAuthoritativeGeneration)
{
	storage::CInMemoryStorageService storage(9);
	CControlStorageRpcSession session(Identity(), storage);
	const auto snapshotBeforeHello = session.Process(Request(EControlIpcKind::StorageSnapshotRequest, 51, 9));
	ASSERT_EQ(EControlIpcKind::Error, snapshotBeforeHello.header.kind);
	const auto beforeHelloError = DecodeControlIpcError(snapshotBeforeHello.payload);
	ASSERT_TRUE(beforeHelloError);
	EXPECT_EQ(EControlIpcTerminalStatus::InvalidRequest, beforeHelloError->status);

	const auto other = EncodeControlStorageHello("profile-b");
	ASSERT_TRUE(other);
	const auto mismatch = session.Process(Request(EControlIpcKind::Hello, 52, 0, *other));
	ASSERT_EQ(EControlIpcKind::Error, mismatch.header.kind);
	const auto mismatchError = DecodeControlIpcError(mismatch.payload);
	ASSERT_TRUE(mismatchError);
	EXPECT_EQ(EControlIpcTerminalStatus::ProfileMismatch, mismatchError->status);
	EXPECT_FALSE(session.IsHandshaken());

	Handshake(session);
	const auto stale = session.Process(Request(EControlIpcKind::StorageSnapshotRequest, 53, 8));
	ASSERT_EQ(EControlIpcKind::Error, stale.header.kind);
	const auto staleError = DecodeControlIpcError(stale.payload);
	ASSERT_TRUE(staleError);
	EXPECT_EQ(EControlIpcTerminalStatus::GenerationMismatch, staleError->status);
}

TEST(ControlStorageRpc, AcceptsForwardMinorHelloAndPinsTheCompletedSession)
{
	storage::CInMemoryStorageService storage(9);
	CControlStorageRpcSession session(Identity(), storage);
	const auto helloPayload = EncodeControlStorageHello("profile-a");
	ASSERT_TRUE(helloPayload);
	auto hello = Request(EControlIpcKind::Hello, 54, 0, *helloPayload);
	hello.header.minorVersion = kControlIpcMinorVersion + 1;
	const auto acknowledged = session.Process(hello);
	EXPECT_EQ(EControlIpcKind::HelloAck, acknowledged.header.kind);
	EXPECT_EQ(54u, acknowledged.header.requestId);
	EXPECT_EQ(9u, acknowledged.header.generation);
	EXPECT_TRUE(session.IsHandshaken());

	const auto second = session.Process(Request(EControlIpcKind::Hello, 55, 0, *helloPayload));
	ASSERT_EQ(EControlIpcKind::Error, second.header.kind);
	EXPECT_EQ(55u, second.header.requestId);
	EXPECT_EQ(9u, second.header.generation);
	const auto secondError = DecodeControlIpcError(second.payload);
	ASSERT_TRUE(secondError);
	EXPECT_EQ(EControlIpcTerminalStatus::InvalidRequest, secondError->status);
	EXPECT_TRUE(session.IsHandshaken());
	EXPECT_EQ(EControlIpcKind::StorageSnapshotResponse,
		session.Process(Request(EControlIpcKind::StorageSnapshotRequest, 56, 9)).header.kind);
}

TEST(ControlStorageRpc, RejectsInvalidLocalIdentityWithoutHandshaking)
{
	storage::CInMemoryStorageService storage(9);
	const auto helloPayload = EncodeControlStorageHello("profile-a");
	ASSERT_TRUE(helloPayload);
	CControlStorageRpcSession emptyProfile({ "", 9 }, storage);
	const auto emptyResponse = emptyProfile.Process(Request(EControlIpcKind::Hello, 57, 0, *helloPayload));
	ASSERT_EQ(EControlIpcKind::Error, emptyResponse.header.kind);
	EXPECT_EQ(9u, emptyResponse.header.generation);
	const auto emptyError = DecodeControlIpcError(emptyResponse.payload);
	ASSERT_TRUE(emptyError);
	EXPECT_EQ(EControlIpcTerminalStatus::InternalError, emptyError->status);
	EXPECT_FALSE(emptyProfile.IsHandshaken());

	CControlStorageRpcSession zeroGeneration({ "profile-a", 0 }, storage);
	const auto zeroResponse = zeroGeneration.Process(Request(EControlIpcKind::Hello, 58, 0, *helloPayload));
	ASSERT_EQ(EControlIpcKind::Error, zeroResponse.header.kind);
	EXPECT_NE(0u, zeroResponse.header.generation);
	EXPECT_FALSE(zeroGeneration.IsHandshaken());
}

TEST(ControlStorageRpc, SnapshotAndResponseCodecRoundTrip)
{
	storage::CInMemoryStorageService storage(9);
	ASSERT_EQ(storage::EStorageMutationStatus::Succeeded, storage.Apply(Put("seed", "window", "wide", 0)).status);
	CControlStorageRpcSession session(Identity(), storage);
	Handshake(session);
	const auto response = session.Process(Request(EControlIpcKind::StorageSnapshotRequest, 61, 9));
	EXPECT_EQ(EControlIpcKind::StorageSnapshotResponse, response.header.kind);
	EXPECT_EQ(61u, response.header.requestId);
	EXPECT_TRUE(HasFlag(response.header.flags, EControlIpcFlags::Terminal));
	const auto snapshot = DecodeControlStorageSnapshotResponse(response.payload);
	ASSERT_TRUE(snapshot);
	EXPECT_EQ(9u, snapshot->generation);
	ASSERT_EQ(1u, snapshot->entries.size());
	EXPECT_EQ("wide", snapshot->entries[0].value);
}

TEST(ControlStorageRpc, ApplyCarriesOperationReplayAndConflictAsApplicationResult)
{
	storage::CInMemoryStorageService storage(9);
	CControlStorageRpcSession session(Identity(), storage);
	Handshake(session);
	const auto firstPayload = EncodeControlStorageApplyRequest(Put("durable-op", "panel", "left", 0));
	ASSERT_TRUE(firstPayload);
	const auto first = session.Process(Request(EControlIpcKind::StorageApplyRequest, 71, 9, *firstPayload));
	ASSERT_EQ(EControlIpcKind::StorageApplyResponse, first.header.kind);
	const auto firstResult = DecodeControlStorageApplyResponse(first.payload);
	ASSERT_TRUE(firstResult);
	EXPECT_EQ(storage::EStorageMutationStatus::Succeeded, firstResult->status);
	EXPECT_FALSE(firstResult->replayed);

	const auto replay = session.Process(Request(EControlIpcKind::StorageApplyRequest, 72, 9, *firstPayload));
	const auto replayResult = DecodeControlStorageApplyResponse(replay.payload);
	ASSERT_TRUE(replayResult);
	EXPECT_TRUE(replayResult->replayed);
	EXPECT_EQ(firstResult->revision, replayResult->revision);

	const auto conflictPayload = EncodeControlStorageApplyRequest(Put("other-op", "panel", "right", 0));
	ASSERT_TRUE(conflictPayload);
	const auto conflict = session.Process(Request(EControlIpcKind::StorageApplyRequest, 73, 9, *conflictPayload));
	EXPECT_EQ(EControlIpcKind::StorageApplyResponse, conflict.header.kind);
	const auto conflictResult = DecodeControlStorageApplyResponse(conflict.payload);
	ASSERT_TRUE(conflictResult);
	EXPECT_EQ(storage::EStorageMutationStatus::Conflict, conflictResult->status);
}

TEST(ControlStorageRpc, RejectsMalformedDuplicateAndInvalidEnumPayloads)
{
	storage::CInMemoryStorageService storage(9);
	CControlStorageRpcSession session(Identity(), storage);
	Handshake(session);
	const auto valid = EncodeControlStorageApplyRequest(Put("one", "key", "value", 0));
	ASSERT_TRUE(valid);
	auto invalidEnum = *valid;
	// First TLV is StorageMutation. Its nested record begins at byte 6: u32 count then scope.
	ASSERT_GE(invalidEnum.size(), 11u);
	invalidEnum[10] = 0xff;
	const auto invalid = session.Process(Request(EControlIpcKind::StorageApplyRequest, 81, 9, invalidEnum));
	ASSERT_EQ(EControlIpcKind::Error, invalid.header.kind);
	const auto invalidError = DecodeControlIpcError(invalid.payload);
	ASSERT_TRUE(invalidError);
	EXPECT_EQ(EControlIpcTerminalStatus::InvalidRequest, invalidError->status);
	const auto malformed = session.Process(Request(EControlIpcKind::StorageApplyRequest, 811, 9, { 9, 0, 1 }));
	EXPECT_EQ(EControlIpcKind::Error, malformed.header.kind);
	std::vector<std::uint8_t> oversize(kControlIpcMaximumFrameBytes - kControlIpcHeaderBytes + 1);
	const auto tooLarge = session.Process(Request(EControlIpcKind::StorageApplyRequest, 812, 9, std::move(oversize)));
	EXPECT_EQ(EControlIpcKind::Error, tooLarge.header.kind);

	std::vector<std::uint8_t> duplicateWire{ 3, 0, 9, 0, 0, 0, 'p', 'r', 'o', 'f', 'i', 'l', 'e', '-', 'a',
		3, 0, 9, 0, 0, 0, 'p', 'r', 'o', 'f', 'i', 'l', 'e', '-', 'a' };
	CControlStorageRpcSession duplicateSession(Identity(), storage);
	const auto duplicate = duplicateSession.Process(Request(EControlIpcKind::Hello, 82, 0, duplicateWire));
	EXPECT_EQ(EControlIpcKind::Error, duplicate.header.kind);
	const auto duplicateError = DecodeControlIpcError(duplicate.payload);
	ASSERT_TRUE(duplicateError);
	EXPECT_EQ(EControlIpcTerminalStatus::InvalidRequest, duplicateError->status);
}

TEST(ControlStorageRpc, BoundsAndValidatesExternalProfileAndOperationIdentifiers)
{
	EXPECT_FALSE(EncodeControlStorageHello(""));
	EXPECT_FALSE(EncodeControlStorageHello(std::string("profile\0bad", 11)));
	EXPECT_FALSE(EncodeControlStorageHello(std::string(kControlStorageRpcMaximumStringBytes + 1, 'p')));
	const auto emptyProfileWire = EncodeControlIpcFields({ { static_cast<std::uint16_t>(EControlIpcFieldTag::ProfileId), {} } });
	ASSERT_TRUE(emptyProfileWire);
	EXPECT_FALSE(DecodeControlStorageHello(*emptyProfileWire));

	auto maximumOperation = Put(std::string(kControlStorageRpcMaximumStringBytes, 'o'), "key", "value", 0);
	EXPECT_TRUE(EncodeControlStorageApplyRequest(maximumOperation));
	EXPECT_FALSE(EncodeControlStorageApplyRequest(Put("", "key", "value", 0)));
	EXPECT_FALSE(EncodeControlStorageApplyRequest(Put(std::string("op\0id", 5), "key", "value", 0)));
	EXPECT_FALSE(EncodeControlStorageApplyRequest(Put(std::string(kControlStorageRpcMaximumStringBytes + 1, 'o'), "key", "value", 0)));

	const auto valid = EncodeControlStorageApplyRequest(Put("valid", "key", "value", 0));
	ASSERT_TRUE(valid);
	auto fields = DecodeControlIpcFields(*valid);
	ASSERT_EQ(EControlIpcFieldDecodeOutcome::Decoded, fields.outcome);
	for (auto& field : fields.fields) {
		if (field.tag == static_cast<std::uint16_t>(EControlIpcFieldTag::OperationId)) field.value = { 'o', 0, 'p' };
	}
	const auto nulOperationWire = EncodeControlIpcFields(fields.fields);
	ASSERT_TRUE(nulOperationWire);
	EXPECT_FALSE(DecodeControlStorageApplyRequest(*nulOperationWire));
}

TEST(ControlStorageRpc, MapsThrowingServiceAndSynchronousCancelToSingleTerminalError)
{
	CThrowingStorage storage;
	CControlStorageRpcSession session(Identity(), storage);
	Handshake(session);
	const auto snapshot = session.Process(Request(EControlIpcKind::StorageSnapshotRequest, 91, 9));
	ASSERT_EQ(EControlIpcKind::Error, snapshot.header.kind);
	EXPECT_EQ(91u, snapshot.header.requestId);
	const auto snapshotError = DecodeControlIpcError(snapshot.payload);
	ASSERT_TRUE(snapshotError);
	EXPECT_EQ(EControlIpcTerminalStatus::InternalError, snapshotError->status);

	const auto cancelPayload = EncodeControlStorageCancelRequest(123);
	ASSERT_TRUE(cancelPayload);
	const auto cancel = session.Process(Request(EControlIpcKind::CancelRequest, 92, 9, *cancelPayload));
	ASSERT_EQ(EControlIpcKind::Error, cancel.header.kind);
	EXPECT_EQ(92u, cancel.header.requestId);
	const auto cancelError = DecodeControlIpcError(cancel.payload);
	ASSERT_TRUE(cancelError);
	EXPECT_EQ(EControlIpcTerminalStatus::InvalidRequest, cancelError->status);
}

} // namespace
} // namespace platform::controlipc

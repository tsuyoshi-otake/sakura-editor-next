/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include "platform/controlipc/ControlStorageRpcServerAdapter.h"
#include "platform/storage/CInMemoryStorageService.h"

#include <memory>
#include <condition_variable>
#include <chrono>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>

namespace platform::controlipc {
namespace {

const ControlStorageRpcSessionIdentity kIdentity{ "profile-a", 9 };

ControlIpcFrame Request(EControlIpcKind kind, std::uint64_t requestId, std::uint64_t generation,
	std::vector<std::uint8_t> payload = {})
{
	return { { kControlIpcMajorVersion, kControlIpcMinorVersion, kind, EControlIpcFlags::Request,
		requestId, generation }, std::move(payload) };
}

storage::StorageAddress Address(std::string key)
{
	return { storage::EStorageScope::Profile, "profile-a", "adapter-tests", std::move(key) };
}

storage::StorageMutationRequest Put(std::string operationId, std::string key, std::string value,
	std::optional<std::uint64_t> expected = std::nullopt)
{
	return { std::move(operationId), expected,
		{ { Address(std::move(key)), storage::EStorageTarget::User, std::move(value) } } };
}

ControlIpcFrameDispatchResult Dispatch(IControlIpcSessionHandler& session, const ControlIpcFrame& request)
{
	auto result = session.HandleFrame({ 1, 42 }, request);
	EXPECT_EQ(1u, result.responseFrames.size());
	if (result.responseFrames.size() == 1) {
		EXPECT_EQ(request.header.requestId, result.responseFrames.front().header.requestId);
		EXPECT_TRUE(HasFlag(result.responseFrames.front().header.flags, EControlIpcFlags::Terminal));
	}
	return result;
}

void Handshake(IControlIpcSessionHandler& session, std::uint64_t requestId = 1)
{
	const auto payload = EncodeControlStorageHello("profile-a");
	ASSERT_TRUE(payload);
	const auto response = Dispatch(session, Request(EControlIpcKind::Hello, requestId, 0, *payload));
	ASSERT_EQ(1u, response.responseFrames.size());
	EXPECT_EQ(EControlIpcKind::HelloAck, response.responseFrames.front().header.kind);
	EXPECT_EQ(EControlIpcSessionDecision::KeepOpen, response.decision);
}

class CThrowingStorage final : public storage::IStorageAuthority {
public:
	storage::StorageAuthorityOpenResult Open() override
	{
		return { storage::EStorageAuthorityOpenStatus::AlreadyOpen, "test storage is already open" };
	}
	void Close() noexcept override {}
	[[nodiscard]] bool IsOpen() const noexcept override { return true; }
	storage::StorageMutationResult Apply(const storage::StorageMutationRequest&) override { throw std::runtime_error("apply"); }
	storage::StorageSnapshot Snapshot() const override { throw std::runtime_error("snapshot"); }
	std::unique_ptr<storage::IStorageChangeSubscription> Subscribe(storage::StorageChangeCallback) override { return nullptr; }
};

class CBlockingStorage final : public storage::IStorageAuthority {
public:
	storage::StorageAuthorityOpenResult Open() override
	{
		return { storage::EStorageAuthorityOpenStatus::AlreadyOpen, "test storage is already open" };
	}
	void Close() noexcept override {}
	[[nodiscard]] bool IsOpen() const noexcept override { return true; }
	storage::StorageMutationResult Apply(const storage::StorageMutationRequest&) override { return {}; }

	storage::StorageSnapshot Snapshot() const override
	{
		std::unique_lock lock(m_mutex);
		m_snapshotEntered = true;
		m_condition.notify_all();
		m_condition.wait(lock, [this] { return m_releaseSnapshot; });
		return {};
	}

	std::unique_ptr<storage::IStorageChangeSubscription> Subscribe(storage::StorageChangeCallback) override
	{
		return nullptr;
	}

	bool WaitUntilSnapshotEntered() const
	{
		std::unique_lock lock(m_mutex);
		return m_condition.wait_for(lock, std::chrono::seconds(5), [this] { return m_snapshotEntered; });
	}

	void ReleaseSnapshot()
	{
		{
			std::lock_guard lock(m_mutex);
			m_releaseSnapshot = true;
		}
		m_condition.notify_all();
	}

private:
	mutable std::mutex m_mutex;
	mutable std::condition_variable m_condition;
	mutable bool m_snapshotEntered = false;
	mutable bool m_releaseSnapshot = false;
};

TEST(ControlStorageRpcServerAdapter, ValidatesConstructorInputsBeforeSessionsCanBeCreated)
{
	auto storage = std::make_shared<storage::CInMemoryStorageService>(9);
	EXPECT_THROW(CControlStorageRpcServerAdapter({ "", 9 }, storage), std::invalid_argument);
	EXPECT_THROW(CControlStorageRpcServerAdapter({ "profile-a", 0 }, storage), std::invalid_argument);
	EXPECT_THROW(CControlStorageRpcServerAdapter(kIdentity, nullptr), std::invalid_argument);
}

TEST(ControlStorageRpcServerAdapter, CreatesIndependentSessionsWithIndependentHelloState)
{
	auto storage = std::make_shared<storage::CInMemoryStorageService>(9);
	CControlStorageRpcServerAdapter adapter(kIdentity, storage);
	auto first = adapter.CreateSession({ 101, 1001 });
	auto second = adapter.CreateSession({ 102, 1002 });
	ASSERT_NE(nullptr, first);
	ASSERT_NE(nullptr, second);

	const auto beforeHello = Dispatch(*first, Request(EControlIpcKind::StorageSnapshotRequest, 2, 9));
	ASSERT_EQ(1u, beforeHello.responseFrames.size());
	EXPECT_EQ(EControlIpcKind::Error, beforeHello.responseFrames.front().header.kind);
	EXPECT_EQ(EControlIpcSessionDecision::KeepOpen, beforeHello.decision);
	Handshake(*first, 3);

	const auto secondBeforeHello = Dispatch(*second, Request(EControlIpcKind::StorageSnapshotRequest, 4, 9));
	ASSERT_EQ(1u, secondBeforeHello.responseFrames.size());
	EXPECT_EQ(EControlIpcKind::Error, secondBeforeHello.responseFrames.front().header.kind);
	Handshake(*second, 5);
}

TEST(ControlStorageRpcServerAdapter, PreservesHelloSnapshotApplyReplayAndConflictProtocolMapping)
{
	auto storage = std::make_shared<storage::CInMemoryStorageService>(9);
	CControlStorageRpcServerAdapter adapter(kIdentity, storage);
	auto session = adapter.CreateSession({ 103, 1003 });
	ASSERT_NE(nullptr, session);
	Handshake(*session);

	const auto snapshot = Dispatch(*session, Request(EControlIpcKind::StorageSnapshotRequest, 11, 9));
	ASSERT_EQ(EControlIpcKind::StorageSnapshotResponse, snapshot.responseFrames.front().header.kind);
	ASSERT_TRUE(DecodeControlStorageSnapshotResponse(snapshot.responseFrames.front().payload));

	const auto request = EncodeControlStorageApplyRequest(Put("operation-a", "layout", "wide", 0));
	ASSERT_TRUE(request);
	const auto applied = Dispatch(*session, Request(EControlIpcKind::StorageApplyRequest, 12, 9, *request));
	ASSERT_EQ(EControlIpcKind::StorageApplyResponse, applied.responseFrames.front().header.kind);
	const auto appliedResult = DecodeControlStorageApplyResponse(applied.responseFrames.front().payload);
	ASSERT_TRUE(appliedResult);
	EXPECT_EQ(storage::EStorageMutationStatus::Succeeded, appliedResult->status);
	EXPECT_FALSE(appliedResult->replayed);

	const auto replay = Dispatch(*session, Request(EControlIpcKind::StorageApplyRequest, 13, 9, *request));
	const auto replayResult = DecodeControlStorageApplyResponse(replay.responseFrames.front().payload);
	ASSERT_TRUE(replayResult);
	EXPECT_EQ(storage::EStorageMutationStatus::Succeeded, replayResult->status);
	EXPECT_TRUE(replayResult->replayed);
	EXPECT_EQ(appliedResult->revision, replayResult->revision);

	const auto conflictRequest = EncodeControlStorageApplyRequest(Put("operation-b", "layout", "narrow", 0));
	ASSERT_TRUE(conflictRequest);
	const auto conflict = Dispatch(*session, Request(EControlIpcKind::StorageApplyRequest, 14, 9, *conflictRequest));
	const auto conflictResult = DecodeControlStorageApplyResponse(conflict.responseFrames.front().payload);
	ASSERT_TRUE(conflictResult);
	EXPECT_EQ(storage::EStorageMutationStatus::Conflict, conflictResult->status);
}

TEST(ControlStorageRpcServerAdapter, ClosingGateRejectsNewSessionsAndClosesExistingSessionsWithServerStopping)
{
	auto storage = std::make_shared<storage::CInMemoryStorageService>(9);
	CControlStorageRpcServerAdapter adapter(kIdentity, storage);
	auto session = adapter.CreateSession({ 104, 1004 });
	ASSERT_NE(nullptr, session);
	Handshake(*session);
	EXPECT_TRUE(adapter.BeginStopping());
	EXPECT_FALSE(adapter.IsAccepting());
	EXPECT_EQ(EControlStorageRpcServerAdapterState::Stopping, adapter.State());
	EXPECT_EQ(nullptr, adapter.CreateSession({ 105, 1005 }));

	const auto stopping = Dispatch(*session, Request(EControlIpcKind::StorageSnapshotRequest, 21, 9));
	ASSERT_EQ(1u, stopping.responseFrames.size());
	EXPECT_EQ(EControlIpcKind::Error, stopping.responseFrames.front().header.kind);
	EXPECT_EQ(EControlIpcSessionDecision::Close, stopping.decision);
	const auto error = DecodeControlIpcError(stopping.responseFrames.front().payload);
	ASSERT_TRUE(error);
	EXPECT_EQ(EControlIpcTerminalStatus::ServerStopping, error->status);

	adapter.Stop();
	EXPECT_EQ(EControlStorageRpcServerAdapterState::Stopped, adapter.State());
	EXPECT_FALSE(adapter.BeginStopping());
	EXPECT_EQ(nullptr, adapter.CreateSession({ 106, 1006 }));
}

TEST(ControlStorageRpcServerAdapter, BeginStoppingWaitsForAnActiveFrameBeforeReturning)
{
	auto storage = std::make_shared<CBlockingStorage>();
	CControlStorageRpcServerAdapter adapter(kIdentity, storage);
	auto session = adapter.CreateSession({ 109, 1009 });
	ASSERT_NE(nullptr, session);
	Handshake(*session);

	std::optional<ControlIpcFrameDispatchResult> frameResult;
	std::thread frameThread([&] {
		frameResult = session->HandleFrame({ 109, 1009 }, Request(EControlIpcKind::StorageSnapshotRequest, 51, 9));
	});
	const bool frameEntered = storage->WaitUntilSnapshotEntered();

	std::mutex stopMutex;
	std::condition_variable stopCondition;
	bool stopStarted = false;
	bool stopReturned = false;
	bool stopResult = false;
	std::thread stopThread([&] {
		{
			std::lock_guard lock(stopMutex);
			stopStarted = true;
		}
		stopCondition.notify_all();
		const bool result = adapter.BeginStopping();
		{
			std::lock_guard lock(stopMutex);
			stopResult = result;
			stopReturned = true;
		}
		stopCondition.notify_all();
	});

	bool stopStartedObserved = false;
	bool stopReturnedBeforeRelease = false;
	{
		std::unique_lock lock(stopMutex);
		stopStartedObserved = stopCondition.wait_for(lock, std::chrono::seconds(5), [&] { return stopStarted; });
		if (stopStartedObserved) {
			stopReturnedBeforeRelease = stopCondition.wait_for(lock, std::chrono::milliseconds(100), [&] { return stopReturned; });
		}
	}

	storage->ReleaseSnapshot();
	frameThread.join();
	stopThread.join();

	EXPECT_TRUE(frameEntered);
	EXPECT_TRUE(stopStartedObserved);
	EXPECT_FALSE(stopReturnedBeforeRelease);
	EXPECT_TRUE(stopResult);
	EXPECT_TRUE(stopReturned);
	ASSERT_TRUE(frameResult.has_value());
	ASSERT_EQ(1u, frameResult->responseFrames.size());
	EXPECT_EQ(EControlIpcKind::StorageSnapshotResponse, frameResult->responseFrames.front().header.kind);
	EXPECT_EQ(EControlIpcSessionDecision::KeepOpen, frameResult->decision);
	EXPECT_EQ(EControlStorageRpcServerAdapterState::Stopping, adapter.State());

	adapter.Stop();
}

TEST(ControlStorageRpcServerAdapter, DelegatesServiceExceptionsToTheRpcInternalErrorResponse)
{
	auto storage = std::make_shared<CThrowingStorage>();
	CControlStorageRpcServerAdapter adapter(kIdentity, storage);
	auto session = adapter.CreateSession({ 107, 1007 });
	ASSERT_NE(nullptr, session);
	Handshake(*session);
	const auto response = Dispatch(*session, Request(EControlIpcKind::StorageSnapshotRequest, 31, 9));
	ASSERT_EQ(1u, response.responseFrames.size());
	EXPECT_EQ(EControlIpcSessionDecision::KeepOpen, response.decision);
	EXPECT_EQ(EControlIpcKind::Error, response.responseFrames.front().header.kind);
	const auto error = DecodeControlIpcError(response.responseFrames.front().payload);
	ASSERT_TRUE(error);
	EXPECT_EQ(EControlIpcTerminalStatus::InternalError, error->status);
}

TEST(ControlStorageRpcServerAdapter, SessionOwnsItsLocalHandshakeAndServiceLifetime)
{
	auto storage = std::make_shared<storage::CInMemoryStorageService>(9);
	std::weak_ptr<storage::IStorageAuthority> weakStorage = storage;
	auto adapter = std::make_unique<CControlStorageRpcServerAdapter>(kIdentity, storage);
	auto session = adapter->CreateSession({ 108, 1008 });
	ASSERT_NE(nullptr, session);
	Handshake(*session);

	adapter.reset();
	storage.reset();
	EXPECT_FALSE(weakStorage.expired());
	const auto response = Dispatch(*session, Request(EControlIpcKind::StorageSnapshotRequest, 41, 9));
	ASSERT_EQ(1u, response.responseFrames.size());
	EXPECT_EQ(EControlIpcKind::Error, response.responseFrames.front().header.kind);
	EXPECT_EQ(EControlIpcSessionDecision::Close, response.decision);
	const auto error = DecodeControlIpcError(response.responseFrames.front().payload);
	ASSERT_TRUE(error);
	EXPECT_EQ(EControlIpcTerminalStatus::ServerStopping, error->status);

	session.reset();
	EXPECT_TRUE(weakStorage.expired());
}

} // namespace
} // namespace platform::controlipc

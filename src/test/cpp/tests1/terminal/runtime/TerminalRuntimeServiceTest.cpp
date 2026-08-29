/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include "terminal/runtime/TerminalRuntimeService.h"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>

namespace {

using namespace std::chrono_literals;

struct FakeBackendState final {
	std::mutex mutex;
	std::condition_variable condition;
	terminal::TerminalStartResult startResult = terminal::TerminalStartResult::Success();
	terminal::TerminalLaunchOptions launch;
	std::optional<terminal::TerminalBackendProcessIdentity> processIdentity;
	std::vector<std::uint8_t> input;
	bool closed{};
	std::size_t startCalls{};
};

class FakeTerminalBackend final : public terminal::ITerminalBackend {
public:
	explicit FakeTerminalBackend(std::shared_ptr<FakeBackendState> state)
		: m_state(std::move(state))
	{
	}

	terminal::TerminalStartResult Start(const terminal::TerminalLaunchOptions& options) override
	{
		const std::lock_guard lock(m_state->mutex);
		m_state->launch = options;
		++m_state->startCalls;
		return m_state->startResult;
	}

	terminal::TerminalBackendReadResult ReadOutput(
		std::span<std::uint8_t> destination, std::chrono::milliseconds timeout) override
	{
		std::unique_lock lock(m_state->mutex);
		m_state->condition.wait_for(lock, timeout, [&] { return m_state->closed; });
		if (m_state->closed) {
			return { terminal::TerminalBackendReadStatus::EndOfFile, 0, 0 };
		}
		static_cast<void>(destination);
		return {};
	}

	terminal::TerminalBackendWriteResult WriteInput(std::span<const std::uint8_t> source) override
	{
		const std::lock_guard lock(m_state->mutex);
		if (m_state->closed) {
			return { terminal::TerminalBackendWriteStatus::Closed, 0, ERROR_OPERATION_ABORTED };
		}
		m_state->input.insert(m_state->input.end(), source.begin(), source.end());
		m_state->condition.notify_all();
		return { terminal::TerminalBackendWriteStatus::Completed, source.size(), 0 };
	}

	terminal::TerminalBackendOperationResult Resize(terminal::TerminalSize) override
	{
		return { true, 0 };
	}

	void RequestGracefulClose() noexcept override
	{
		m_state->condition.notify_all();
	}

	terminal::TerminalBackendExitResult WaitForExit(std::chrono::milliseconds) noexcept override
	{
		return { terminal::TerminalBackendExitStatus::Exited, 0, 0 };
	}

	void ForceTerminate() noexcept override
	{
		m_state->condition.notify_all();
	}

	void Close() noexcept override
	{
		{
			const std::lock_guard lock(m_state->mutex);
			m_state->closed = true;
		}
		m_state->condition.notify_all();
	}

	std::optional<terminal::TerminalBackendProcessIdentity> GetProcessIdentity() const noexcept override
	{
		const std::lock_guard lock(m_state->mutex);
		return m_state->processIdentity;
	}

	bool OwnsProcess(
		const std::uint32_t processId, const std::uint64_t creationTime) const noexcept override
	{
		const std::lock_guard lock(m_state->mutex);
		return m_state->processIdentity
			&& m_state->processIdentity->processId == processId
			&& m_state->processIdentity->creationTime == creationTime;
	}

private:
	std::shared_ptr<FakeBackendState> m_state;
};

terminal::HarnessOperationId Operation(const std::uint8_t value)
{
	terminal::HarnessOperationId operation;
	operation.value[0] = value;
	return operation;
}

terminal::TerminalRuntimeSessionFactory Factory(const std::shared_ptr<FakeBackendState>& state)
{
	return [state](terminal::TerminalSessionCallbacks callbacks) {
		return std::make_unique<terminal::CTerminalSession>(
			std::make_unique<FakeTerminalBackend>(state), std::move(callbacks));
	};
}

terminal::TerminalCreateRequest CreateRequest(const std::uint8_t operation)
{
	terminal::TerminalCreateRequest request;
	request.operationId = Operation(operation);
	request.sessionId = terminal::TerminalSessionId{ 1 };
	request.launch.executablePath = L"fake-terminal.exe";
	request.launch.initialSize = { 80, 24 };
	return request;
}

bool WaitForInput(const std::shared_ptr<FakeBackendState>& state, const std::uint8_t value)
{
	std::unique_lock lock(state->mutex);
	return state->condition.wait_for(lock, 2s, [&] {
		return std::find(state->input.begin(), state->input.end(), value) != state->input.end();
	});
}

TEST(TerminalRuntimeService, DecoratorReceivesReservedIdentityAndInstanceIsOwned)
{
	auto backendState = std::make_shared<FakeBackendState>();
	terminal::TerminalRuntimeServiceDependencies dependencies;
	dependencies.createSession = Factory(backendState);
	terminal::TerminalInstanceId decoratedId;
	std::uint64_t decoratedGeneration{};
	dependencies.decorateLaunch = [&](const terminal::TerminalCreateRequest& request,
		terminal::TerminalLaunchOptions& launch) {
			decoratedId = request.instanceId;
			decoratedGeneration = request.instanceGeneration;
			launch.environmentOverrides.push_back({ L"SAKURA_TERMINAL_TARGET", L"runtime-test" });
	};

	terminal::CTerminalRuntimeService service(std::move(dependencies));
	const auto result = service.CreateInstance(CreateRequest(1));
	ASSERT_TRUE(result.Succeeded());
	EXPECT_EQ(result.instanceId, decoratedId);
	EXPECT_EQ(result.instanceGeneration, decoratedGeneration);
	EXPECT_NE(0U, decoratedGeneration);
	{
		const std::lock_guard lock(backendState->mutex);
		EXPECT_EQ(L"runtime-test", backendState->launch.environmentOverrides.front().value.value());
	}

	const auto snapshot = service.Snapshot({ service.Instance(result.instanceId)->Snapshot().coordinate });
	ASSERT_TRUE(snapshot.snapshot.has_value());
	EXPECT_EQ(result.instanceId, snapshot.snapshot->coordinate.instanceId);
	EXPECT_EQ(terminal::TerminalInstanceState::Running, snapshot.snapshot->state);

	service.BeginCloseInstance(result.instanceId);
	EXPECT_EQ(terminal::TerminalInstanceCloseWaitStatus::Closed,
		service.WaitForInstanceClose(result.instanceId, std::chrono::steady_clock::now() + 2s).status);
}

TEST(TerminalRuntimeService, FailedInitialStartRollsBackTopologyAndKeepsOutcome)
{
	auto backendState = std::make_shared<FakeBackendState>();
	backendState->startResult = terminal::TerminalStartResult::Failure(ERROR_FILE_NOT_FOUND, L"fake failure");
	terminal::TerminalRuntimeService service(Factory(backendState));

	terminal::TerminalSessionCreateRequest request;
	request.operationId = Operation(2);
	request.name = "failed";
	const auto result = service.CreateSession(request);
	EXPECT_EQ(terminal::TerminalRuntimeOperationCode::InternalError, result.code);
	ASSERT_TRUE(result.instanceId.has_value());
	const auto topology = service.CollectionSnapshot();
	ASSERT_TRUE(topology.has_value());
	EXPECT_TRUE(topology->sessions.empty());
	const auto* instance = service.Instance(*result.instanceId);
	ASSERT_NE(nullptr, instance);
	ASSERT_TRUE(instance->Outcome().has_value());
	EXPECT_EQ(terminal::TerminalInstanceOutcomeKind::StartFailed, instance->Outcome()->kind);
}

TEST(TerminalRuntimeService, ProcessIdentityIsScopedToCurrentTargetAndInstance)
{
	auto backendState = std::make_shared<FakeBackendState>();
	backendState->processIdentity = terminal::TerminalBackendProcessIdentity{ 4242, 987654321 };
	terminal::CTerminalRuntimeService service(Factory(backendState));
	const auto created = service.CreateInstance(CreateRequest(11));
	ASSERT_TRUE(created.Succeeded());
	const auto* instance = service.Instance(created.instanceId);
	ASSERT_NE(nullptr, instance);
	const auto target = instance->Snapshot().coordinate;

	const auto identity = service.GetProcessIdentity(target);
	ASSERT_TRUE(identity.has_value());
	EXPECT_EQ(4242U, identity->processId);
	EXPECT_EQ(987654321ULL, identity->creationTime);
	EXPECT_TRUE(service.OwnsProcess(target, 4242, 987654321));
	EXPECT_FALSE(service.OwnsProcess(target, 4242, 987654322));
	const auto instanceIdentity = service.GetProcessIdentity(created.instanceId);
	ASSERT_TRUE(instanceIdentity.has_value());
	EXPECT_EQ(identity->processId, instanceIdentity->processId);
	EXPECT_EQ(identity->creationTime, instanceIdentity->creationTime);
	EXPECT_TRUE(service.OwnsProcess(created.instanceId, 4242, 987654321));

	auto staleTarget = target;
	++staleTarget.instanceGeneration;
	EXPECT_FALSE(service.GetProcessIdentity(staleTarget).has_value());
	EXPECT_FALSE(service.OwnsProcess(staleTarget, 4242, 987654321));

	service.BeginCloseInstance(created.instanceId);
	EXPECT_EQ(terminal::TerminalInstanceCloseWaitStatus::Closed,
		service.WaitForInstanceClose(created.instanceId, std::chrono::steady_clock::now() + 2s).status);
	EXPECT_FALSE(service.GetProcessIdentity(target).has_value());
	EXPECT_FALSE(service.OwnsProcess(created.instanceId, 4242, 987654321));
}

TEST(TerminalRuntimeService, TopologySplitAndCloseKeepInstancesExplicitlyOwned)
{
	auto backendState = std::make_shared<FakeBackendState>();
	terminal::CTerminalRuntimeService service(Factory(backendState));

	terminal::TerminalSessionCreateRequest sessionRequest;
	sessionRequest.operationId = Operation(3);
	sessionRequest.name = "session";
	sessionRequest.launch = terminal::TerminalLaunchOptions{};
	sessionRequest.launch->executablePath = L"fake-terminal.exe";
	const auto session = service.CreateSession(sessionRequest);
	ASSERT_EQ(terminal::TerminalRuntimeOperationCode::Succeeded, session.code);
	ASSERT_TRUE(session.sessionId.has_value());
	ASSERT_TRUE(session.paneId.has_value());
	ASSERT_TRUE(session.instanceId.has_value());
	const auto firstInstance = *session.instanceId;

	terminal::TerminalPaneSplitRequest splitRequest;
	splitRequest.operationId = Operation(4);
	splitRequest.paneId = *session.paneId;
	splitRequest.orientation = terminal::TerminalPaneOrientation::Vertical;
	splitRequest.launch = terminal::TerminalLaunchOptions{};
	splitRequest.launch->executablePath = L"fake-terminal.exe";
	const auto split = service.SplitPane(splitRequest);
	ASSERT_EQ(terminal::TerminalRuntimeOperationCode::Succeeded, split.code);
	ASSERT_TRUE(split.paneId.has_value());
	ASSERT_TRUE(split.instanceId.has_value());
	EXPECT_NE(firstInstance, *split.instanceId);

	terminal::TerminalPaneCloseRequest paneClose;
	paneClose.operationId = Operation(5);
	paneClose.paneId = *split.paneId;
	EXPECT_EQ(terminal::TerminalRuntimeOperationCode::Succeeded, service.ClosePane(paneClose).code);
	EXPECT_EQ(terminal::TerminalInstanceCloseWaitStatus::Closed,
		service.WaitForInstanceClose(*split.instanceId, std::chrono::steady_clock::now() + 2s).status);
	ASSERT_TRUE(service.CollectionSnapshot()->sessions.size() == 1U);

	terminal::TerminalSessionCloseRequest sessionClose;
	sessionClose.operationId = Operation(6);
	sessionClose.sessionId = *session.sessionId;
	EXPECT_EQ(terminal::TerminalRuntimeOperationCode::Succeeded, service.CloseSession(sessionClose).code);
	EXPECT_EQ(terminal::TerminalInstanceCloseWaitStatus::Closed,
		service.WaitForInstanceClose(firstInstance, std::chrono::steady_clock::now() + 2s).status);
	EXPECT_TRUE(service.CollectionSnapshot()->sessions.empty());
}

TEST(TerminalRuntimeService, InputBatchIsAtomicAndCaptureUsesTheSharedExtractor)
{
	auto backendState = std::make_shared<FakeBackendState>();
	terminal::CTerminalRuntimeService service(Factory(backendState));
	const auto created = service.CreateInstance(CreateRequest(7));
	ASSERT_TRUE(created.Succeeded());
	const auto instance = service.Instance(created.instanceId);
	ASSERT_NE(nullptr, instance);
	const auto target = instance->Snapshot().coordinate;
	ASSERT_NE(nullptr, service.Model(created.instanceId));
	for (const auto codepoint : U"service") service.Model(created.instanceId)->Print(codepoint);

	terminal::TerminalInputBatch batch;
	batch.operationId = Operation(8);
	batch.target = target;
	batch.actions.push_back({ terminal::TerminalInputActionKind::LiteralText, u"x", {} });
	EXPECT_EQ(terminal::TerminalInputResultCode::Accepted, service.QueueInputBatch(batch).code);

	terminal::TerminalInputBatch invalid = batch;
	invalid.operationId = Operation(9);
	invalid.actions.push_back({ terminal::TerminalInputActionKind::NamedKey,
		u"", static_cast<terminal::TerminalNamedKey>(255) });
	EXPECT_EQ(terminal::TerminalInputResultCode::UnsupportedKey, service.QueueInputBatch(invalid).code);
	EXPECT_TRUE(WaitForInput(backendState, static_cast<std::uint8_t>('x')));

	terminal::TerminalCaptureRequest capture;
	capture.operationId = Operation(10);
	capture.target = target;
	capture.startLine = 0;
	capture.endLine = 0;
	const auto captured = service.Capture(capture);
	ASSERT_EQ(terminal::TerminalCaptureResultCode::Succeeded, captured.code);
	ASSERT_FALSE(captured.lines.empty());
	EXPECT_NE(std::u16string::npos, captured.lines.front().text.find(u"service"));

	{
		const std::lock_guard lock(backendState->mutex);
		EXPECT_EQ(1U, std::count(backendState->input.begin(), backendState->input.end(),
			static_cast<std::uint8_t>('x')));
	}
	service.BeginCloseInstance(created.instanceId);
	EXPECT_EQ(terminal::TerminalInstanceCloseWaitStatus::Closed,
		service.WaitForInstanceClose(created.instanceId, std::chrono::steady_clock::now() + 2s).status);
}

} // namespace

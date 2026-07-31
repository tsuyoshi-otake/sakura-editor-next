/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#include "pch.h"

#include <gtest/gtest.h>

#include "workbench/tasks/TaskExecutionService.h"

#include <memory>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace {

using workbench::tasks::ETaskExecutionKind;
using workbench::tasks::ETaskExecutionOperationReason;
using workbench::tasks::ETaskExecutionOperationStatus;
using workbench::tasks::ETaskExecutionRunState;
using workbench::tasks::ETaskExecutionSessionCloseKind;
using workbench::tasks::ETaskSessionExitKind;
using workbench::tasks::ITaskExecutionSession;
using workbench::tasks::ITaskExecutionSessionFactory;
using workbench::tasks::TaskConfigurationDefinition;
using workbench::tasks::TaskExecutionOperation;
using workbench::tasks::TaskExecutionOperationResult;
using workbench::tasks::TaskExecutionRunMutationRequest;
using workbench::tasks::TaskExecutionService;
using workbench::tasks::TaskExecutionServiceLimits;
using workbench::tasks::TaskExecutionSessionCallbacks;
using workbench::tasks::TaskExecutionSessionCloseResult;
using workbench::tasks::TaskExecutionSessionStartResult;
using workbench::tasks::TaskExecutionStartRequest;
using workbench::tasks::TaskSessionExit;
using workbench::tasks::TaskTerminalLaunchRequest;

struct FakeSessionState final {
	explicit FakeSessionState(TaskExecutionSessionCallbacks value) : callbacks(std::move(value)) {}

	void Exit(ETaskSessionExitKind kind, std::uint32_t exitCode = 0, std::wstring diagnostic = {})
	{
		callbacks.exited({ kind, exitCode, std::move(diagnostic) });
	}

	TaskExecutionSessionCallbacks callbacks;
	TaskExecutionSessionStartResult startResult = TaskExecutionSessionStartResult::Success();
	std::optional<TaskSessionExit> synchronousExit;
	std::optional<TaskTerminalLaunchRequest> launch;
	bool started{};
	unsigned int cancelRequests{};
	unsigned int closeRequests{};
	unsigned int closeWaits{};
	TaskExecutionSessionCloseResult closeResult = TaskExecutionSessionCloseResult::Closed();
	std::function<void()> onBeginClose;
	std::function<void(std::chrono::steady_clock::time_point)> onWaitForClose;
	std::function<void()> onStart;
};

class FakeSession final : public ITaskExecutionSession {
public:
	explicit FakeSession(std::shared_ptr<FakeSessionState> state) : m_state(std::move(state)) {}

	TaskExecutionSessionStartResult Start(const TaskTerminalLaunchRequest& request) override
	{
		m_state->started = true;
		m_state->launch = request;
		if (m_state->onStart) m_state->onStart();
		if (m_state->synchronousExit) m_state->callbacks.exited(*m_state->synchronousExit);
		return m_state->startResult;
	}
	void RequestCancel() noexcept override { ++m_state->cancelRequests; }
	void BeginClose() noexcept override
	{
		++m_state->closeRequests;
		if (m_state->onBeginClose) m_state->onBeginClose();
	}
	TaskExecutionSessionCloseResult WaitForClose(const std::chrono::steady_clock::time_point deadline) noexcept override
	{
		++m_state->closeWaits;
		if (m_state->onWaitForClose) m_state->onWaitForClose(deadline);
		return m_state->closeResult;
	}

private:
	std::shared_ptr<FakeSessionState> m_state;
};

class FakeFactory final : public ITaskExecutionSessionFactory {
public:
	std::unique_ptr<ITaskExecutionSession> Create(const TaskExecutionSessionCallbacks& callbacks) override
	{
		++createCount;
		if (returnNull) return {};
		lastState = std::make_shared<FakeSessionState>(callbacks);
		lastState->startResult = nextStartResult;
		if (onCreate) onCreate(lastState);
		auto session = std::make_unique<FakeSession>(lastState);
		states.push_back(lastState);
		if (synchronousCreateExit) callbacks.exited(*synchronousCreateExit);
		return session;
	}

	bool returnNull{};
	TaskExecutionSessionStartResult nextStartResult = TaskExecutionSessionStartResult::Success();
	std::optional<TaskSessionExit> synchronousCreateExit;
	unsigned int createCount{};
	std::shared_ptr<FakeSessionState> lastState;
	std::vector<std::shared_ptr<FakeSessionState>> states;
	std::function<void(const std::shared_ptr<FakeSessionState>&)> onCreate;
};

class BlockingGate final {
public:
	void Enter()
	{
		std::unique_lock lock(m_mutex);
		m_entered = true;
		m_condition.notify_all();
		m_condition.wait(lock, [this] { return m_released; });
	}
	[[nodiscard]] bool WaitUntilEntered()
	{
		std::unique_lock lock(m_mutex);
		return m_condition.wait_for(lock, std::chrono::seconds(1), [this] { return m_entered; });
	}
	void Release()
	{
		std::lock_guard lock(m_mutex);
		m_released = true;
		m_condition.notify_all();
	}

private:
	std::mutex m_mutex;
	std::condition_variable m_condition;
	bool m_entered{};
	bool m_released{};
};

TaskConfigurationDefinition Runnable(const ETaskExecutionKind kind = ETaskExecutionKind::Process)
{
	TaskConfigurationDefinition definition;
	definition.label = L"compile";
	definition.executionKind = kind;
	definition.command = kind == ETaskExecutionKind::Shell ? L"build --fast" : L"C:\\Tools\\ninja.exe";
	definition.arguments = { L"-C", L"", L"out folder" };
	definition.workingDirectory = L"C:\\Work\\project";
	return definition;
}

TaskExecutionStartRequest StartRequest(std::string operation, TaskConfigurationDefinition definition = Runnable())
{
	TaskExecutionStartRequest request { { std::move(operation), std::nullopt }, std::move(definition), { 140, 45 } };
	return request;
}

TEST(TaskExecutionService, StartsProcessAndShellWithoutCommandLineConcatenation)
{
	auto factory = std::make_shared<FakeFactory>();
	TaskExecutionService service(factory);

	const auto process = service.Start(StartRequest("task.process"));
	ASSERT_EQ(ETaskExecutionOperationStatus::Started, process.status);
	ASSERT_TRUE(process.runId);
	ASSERT_NE(nullptr, factory->lastState);
	ASSERT_TRUE(factory->lastState->launch);
	EXPECT_EQ(ETaskExecutionKind::Process, factory->lastState->launch->executionKind);
	EXPECT_EQ(L"C:\\Tools\\ninja.exe", factory->lastState->launch->terminalLaunchOptions.executablePath);
	EXPECT_EQ((std::vector<std::wstring> { L"-C", L"", L"out folder" }), factory->lastState->launch->terminalLaunchOptions.arguments);
	EXPECT_EQ(L"C:\\Work\\project", factory->lastState->launch->terminalLaunchOptions.workingDirectory);
	EXPECT_EQ(140U, factory->lastState->launch->terminalLaunchOptions.initialSize.columns);
	factory->lastState->Exit(ETaskSessionExitKind::Exited, 0);

	const auto shell = service.Start(StartRequest("task.shell", Runnable(ETaskExecutionKind::Shell)));
	ASSERT_TRUE(shell.Succeeded());
	ASSERT_TRUE(factory->lastState->launch);
	EXPECT_EQ(ETaskExecutionKind::Shell, factory->lastState->launch->executionKind);
	EXPECT_TRUE(factory->lastState->launch->terminalLaunchOptions.executablePath.empty());
	EXPECT_TRUE(factory->lastState->launch->terminalLaunchOptions.arguments.empty());
	EXPECT_EQ(L"build --fast", factory->lastState->launch->shellCommand);
	EXPECT_EQ((std::vector<std::wstring> { L"-C", L"", L"out folder" }), factory->lastState->launch->shellArguments);
}

TEST(TaskExecutionService, RejectsUnsupportedAndInvalidDefinitionsBeforeFactoryCreation)
{
	auto factory = std::make_shared<FakeFactory>();
	TaskExecutionService service(factory);
	auto unsupported = Runnable();
	unsupported.unsupportedCapabilities = workbench::tasks::ETaskUnsupportedCapability::Background;
	const auto rejectedUnsupported = service.Start(StartRequest("task.unsupported", unsupported));
	EXPECT_EQ(ETaskExecutionOperationStatus::Rejected, rejectedUnsupported.status);
	EXPECT_EQ(ETaskExecutionOperationReason::UnsupportedTask, rejectedUnsupported.reason);

	auto invalid = Runnable();
	invalid.command.clear();
	const auto rejectedInvalid = service.Start(StartRequest("task.invalid", invalid));
	EXPECT_EQ(ETaskExecutionOperationReason::InvalidTask, rejectedInvalid.reason);
	EXPECT_EQ(0U, factory->createCount);
}

TEST(TaskExecutionService, ReplaysExactStartAndFencesConflictingOperationIdentity)
{
	auto factory = std::make_shared<FakeFactory>();
	TaskExecutionService service(factory);
	const auto first = service.Start(StartRequest("task.one"));
	ASSERT_TRUE(first.runId);
	const auto replay = service.Start(StartRequest("task.one"));
	EXPECT_EQ(ETaskExecutionOperationStatus::Replayed, replay.status);
	EXPECT_EQ(first.runId, replay.runId);
	EXPECT_EQ(1U, factory->createCount);

	auto changed = Runnable();
	changed.command = L"C:\\Tools\\different.exe";
	const auto conflict = service.Start(StartRequest("task.one", changed));
	EXPECT_EQ(ETaskExecutionOperationStatus::Conflict, conflict.status);
	EXPECT_EQ(ETaskExecutionOperationReason::OperationIdConflict, conflict.reason);
	EXPECT_EQ(1U, factory->createCount);
}

TEST(TaskExecutionService, EnforcesActiveLimitAndCancellationReleasesCapacityOnTerminalCallback)
{
	auto factory = std::make_shared<FakeFactory>();
	TaskExecutionService service(factory, TaskExecutionServiceLimits { 1, 8, 8, 8, 8, std::chrono::seconds(1) });
	const auto first = service.Start(StartRequest("task.first"));
	ASSERT_TRUE(first.runId);
	const auto firstSession = factory->lastState;
	const auto limited = service.Start(StartRequest("task.second"));
	EXPECT_EQ(ETaskExecutionOperationReason::MaximumActiveRuns, limited.reason);

	ASSERT_TRUE(service.Cancel({ { "task.cancel", std::nullopt }, *first.runId }).Succeeded());
	EXPECT_EQ(1U, firstSession->cancelRequests);
	EXPECT_EQ(ETaskExecutionRunState::Cancelling, service.Snapshot().runs.front().state);
	firstSession->Exit(ETaskSessionExitKind::Cancelled, 9, L"cancelled");
	EXPECT_EQ(ETaskExecutionRunState::Cancelled, service.Snapshot().runs.front().state);
	EXPECT_TRUE(service.Start(StartRequest("task.third")).Succeeded());
}

TEST(TaskExecutionService, ReportsStartFailureExitAndCloseWithExplicitTerminalStates)
{
	auto factory = std::make_shared<FakeFactory>();
	TaskExecutionService service(factory);
	const auto start = service.Start(StartRequest("task.fail"));
	ASSERT_TRUE(start.runId);
	factory->lastState->Exit(ETaskSessionExitKind::Failed, 17, L"adapter failed");
	ASSERT_EQ(ETaskExecutionRunState::Failed, service.Snapshot().runs.front().state);
	EXPECT_EQ(17U, service.Snapshot().runs.front().exitCode);

	const auto second = service.Start(StartRequest("task.close"));
	ASSERT_TRUE(second.runId);
	const auto secondSession = factory->lastState;
	EXPECT_TRUE(service.Close({ { "task.close.operation", std::nullopt }, *second.runId }).Succeeded());
	EXPECT_EQ(1U, secondSession->closeRequests);
	const auto snapshot = service.Snapshot();
	ASSERT_EQ(2U, snapshot.runs.size());
	EXPECT_EQ(ETaskExecutionRunState::Closed, snapshot.runs[1].state);
	EXPECT_EQ(1U, secondSession->closeWaits);
}

TEST(TaskExecutionService, CloseSurfacesTimeoutAndHostFailureInsteadOfClaimingTerminalSuccess)
{
	auto timeoutFactory = std::make_shared<FakeFactory>();
	TaskExecutionService timeoutService(timeoutFactory, TaskExecutionServiceLimits { 16, 16, 16, 16, 16, std::chrono::milliseconds(5) });
	const auto timeoutStart = timeoutService.Start(StartRequest("task.close.timeout"));
	ASSERT_TRUE(timeoutStart.runId);
	timeoutFactory->lastState->closeResult = TaskExecutionSessionCloseResult::TimedOut(L"close deadline elapsed");
	const auto timedOut = timeoutService.Close({ { "task.close.timeout.operation", std::nullopt }, *timeoutStart.runId });
	EXPECT_EQ(ETaskExecutionOperationStatus::Rejected, timedOut.status);
	EXPECT_EQ(ETaskExecutionOperationReason::SessionCloseTimedOut, timedOut.reason);
	EXPECT_EQ(ETaskExecutionRunState::Failed, timeoutService.Snapshot().runs.front().state);
	EXPECT_EQ(L"close deadline elapsed", timeoutService.Snapshot().runs.front().diagnostic);

	auto hostFactory = std::make_shared<FakeFactory>();
	TaskExecutionService hostService(hostFactory);
	const auto hostStart = hostService.Start(StartRequest("task.close.host.failure"));
	ASSERT_TRUE(hostStart.runId);
	hostFactory->lastState->closeResult = TaskExecutionSessionCloseResult::HostFailure(1722, L"terminal host disconnected");
	const auto hostFailure = hostService.Close({ { "task.close.host.failure.operation", std::nullopt }, *hostStart.runId });
	EXPECT_EQ(ETaskExecutionOperationStatus::Rejected, hostFailure.status);
	EXPECT_EQ(ETaskExecutionOperationReason::SessionCloseHostFailure, hostFailure.reason);
	EXPECT_EQ(1722U, hostFailure.errorCode);
	EXPECT_EQ(L"terminal host disconnected", hostService.Snapshot().runs.front().diagnostic);
}

TEST(TaskExecutionService, StopFansOutBeforeWaitAndSharesOneAbsoluteDeadline)
{
	auto factory = std::make_shared<FakeFactory>();
	TaskExecutionService service(factory, TaskExecutionServiceLimits { 16, 16, 16, 16, 16, std::chrono::seconds(1) });
	ASSERT_TRUE(service.Start(StartRequest("task.stop.one")).runId);
	ASSERT_TRUE(service.Start(StartRequest("task.stop.two")).runId);
	ASSERT_EQ(2U, factory->states.size());
	std::optional<std::chrono::steady_clock::time_point> firstDeadline;
	factory->states[0]->onWaitForClose = [&] (const auto deadline) {
		EXPECT_EQ(1U, factory->states[1]->closeRequests) << "Stop must begin every close before its first wait";
		firstDeadline = deadline;
	};
	factory->states[1]->onWaitForClose = [&] (const auto deadline) {
		ASSERT_TRUE(firstDeadline);
		EXPECT_EQ(*firstDeadline, deadline) << "every stop wait must share one deadline";
	};

	EXPECT_TRUE(service.Stop().Succeeded());
	for (const auto& state : factory->states) {
		EXPECT_EQ(1U, state->closeRequests);
		EXPECT_EQ(1U, state->closeWaits);
	}
	const auto snapshot = service.Snapshot();
	EXPECT_TRUE(snapshot.stopped);
	ASSERT_EQ(2U, snapshot.runs.size());
	EXPECT_EQ(ETaskExecutionRunState::Closed, snapshot.runs[0].state);
	EXPECT_EQ(ETaskExecutionRunState::Closed, snapshot.runs[1].state);
}

TEST(TaskExecutionService, StopRejectsWhenAJoinedSessionTimesOut)
{
	auto factory = std::make_shared<FakeFactory>();
	TaskExecutionService service(factory, TaskExecutionServiceLimits { 16, 16, 16, 16, 16, std::chrono::milliseconds(5) });
	ASSERT_TRUE(service.Start(StartRequest("task.stop.timeout")).runId);
	factory->lastState->closeResult = TaskExecutionSessionCloseResult::TimedOut(L"shutdown join timed out");

	const auto stopped = service.Stop();
	EXPECT_EQ(ETaskExecutionOperationStatus::Rejected, stopped.status);
	EXPECT_EQ(ETaskExecutionOperationReason::SessionCloseTimedOut, stopped.reason);
	EXPECT_TRUE(service.Snapshot().stopped);
	ASSERT_EQ(1U, service.Snapshot().runs.size());
	EXPECT_EQ(ETaskExecutionRunState::Failed, service.Snapshot().runs.front().state);
	EXPECT_EQ(L"shutdown join timed out", service.Snapshot().runs.front().diagnostic);
}

TEST(TaskExecutionService, StopWaitsForBlockedFactoryOwnershipBeforeReturning)
{
	auto factory = std::make_shared<FakeFactory>();
	BlockingGate factoryGate;
	factory->onCreate = [&factoryGate](const auto&) { factoryGate.Enter(); };
	TaskExecutionService service(factory, TaskExecutionServiceLimits { 16, 16, 16, 16, 16, std::chrono::seconds(1) });
	auto startFuture = std::async(std::launch::async, [&] { return service.Start(StartRequest("task.stop.blocked.factory")); });
	if (!factoryGate.WaitUntilEntered()) {
		factoryGate.Release();
		(void)startFuture.get();
		FAIL() << "factory did not enter the deterministic blocking point";
		return;
	}
	auto stopFuture = std::async(std::launch::async, [&] { return service.Stop(); });
	EXPECT_EQ(std::future_status::timeout, stopFuture.wait_for(std::chrono::milliseconds(25)));
	factoryGate.Release();
	EXPECT_TRUE(stopFuture.get().Succeeded());
	EXPECT_EQ(ETaskExecutionOperationStatus::Stopped, startFuture.get().status);
	ASSERT_NE(nullptr, factory->lastState);
	EXPECT_EQ(1U, factory->lastState->closeRequests);
	EXPECT_EQ(1U, factory->lastState->closeWaits);
	EXPECT_TRUE(service.Snapshot().stopped);
	EXPECT_EQ(ETaskExecutionRunState::Closed, service.Snapshot().runs.front().state);
}

TEST(TaskExecutionService, StopWaitsForBlockedSessionStartAndClosesItsPublishedSession)
{
	auto factory = std::make_shared<FakeFactory>();
	BlockingGate startGate;
	factory->onCreate = [&startGate](const auto& state) { state->onStart = [&startGate] { startGate.Enter(); }; };
	TaskExecutionService service(factory, TaskExecutionServiceLimits { 16, 16, 16, 16, 16, std::chrono::seconds(1) });
	auto startFuture = std::async(std::launch::async, [&] { return service.Start(StartRequest("task.stop.blocked.start")); });
	if (!startGate.WaitUntilEntered()) {
		startGate.Release();
		(void)startFuture.get();
		FAIL() << "session Start did not enter the deterministic blocking point";
		return;
	}
	auto stopFuture = std::async(std::launch::async, [&] { return service.Stop(); });
	EXPECT_EQ(std::future_status::timeout, stopFuture.wait_for(std::chrono::milliseconds(25)));
	startGate.Release();
	EXPECT_TRUE(stopFuture.get().Succeeded());
	EXPECT_TRUE(startFuture.get().Succeeded());
	ASSERT_NE(nullptr, factory->lastState);
	EXPECT_EQ(1U, factory->lastState->closeRequests);
	EXPECT_EQ(1U, factory->lastState->closeWaits);
	EXPECT_EQ(ETaskExecutionRunState::Closed, service.Snapshot().runs.front().state);
}

TEST(TaskExecutionService, ConcurrentStopsJoinTheSameCompletionAndListenerReentryIsDeferred)
{
	auto factory = std::make_shared<FakeFactory>();
	TaskExecutionService service(factory, TaskExecutionServiceLimits { 16, 16, 16, 16, 16, std::chrono::seconds(1) });
	ASSERT_TRUE(service.Start(StartRequest("task.stop.concurrent")).runId);
	BlockingGate closeGate;
	factory->lastState->onWaitForClose = [&closeGate](const auto) { closeGate.Enter(); };
	std::optional<TaskExecutionOperationResult> reentrant;
	ASSERT_TRUE(service.Subscribe([&](const auto& change) {
		if (change.state == ETaskExecutionRunState::Closing) reentrant = service.Stop();
	}));

	auto firstStop = std::async(std::launch::async, [&] { return service.Stop(); });
	if (!closeGate.WaitUntilEntered()) {
		closeGate.Release();
		(void)firstStop.get();
		FAIL() << "first Stop did not reach the deterministic close wait";
		return;
	}
	auto secondStop = std::async(std::launch::async, [&] { return service.Stop(); });
	EXPECT_EQ(std::future_status::timeout, secondStop.wait_for(std::chrono::milliseconds(25)));
	closeGate.Release();
	const auto first = firstStop.get();
	const auto second = secondStop.get();
	EXPECT_EQ(first.status, second.status);
	EXPECT_EQ(first.reason, second.reason);
	EXPECT_EQ(first.revision, second.revision);
	ASSERT_TRUE(reentrant);
	EXPECT_EQ(ETaskExecutionOperationStatus::Deferred, reentrant->status);
	EXPECT_EQ(ETaskExecutionOperationReason::StopInProgress, reentrant->reason);
}

TEST(TaskExecutionService, ActiveNotificationCallbackDefersStopUntilDispatcherReturns)
{
	auto factory = std::make_shared<FakeFactory>();
	TaskExecutionService service(factory);
	std::optional<TaskExecutionOperationResult> callbackResult;
	ASSERT_TRUE(service.Subscribe([&](const auto& change) {
		if (change.state == ETaskExecutionRunState::Starting) callbackResult = service.Stop();
	}));

	const auto started = service.Start(StartRequest("task.stop.from.active.callback"));
	EXPECT_TRUE(started.Succeeded() || started.status == ETaskExecutionOperationStatus::Stopped);
	ASSERT_TRUE(callbackResult);
	EXPECT_EQ(ETaskExecutionOperationStatus::Deferred, callbackResult->status);
	EXPECT_EQ(ETaskExecutionOperationReason::StopInProgress, callbackResult->reason);
	EXPECT_TRUE(service.Snapshot().stopped);
	ASSERT_NE(nullptr, factory->lastState);
	EXPECT_EQ(1U, factory->lastState->closeRequests);
	EXPECT_EQ(1U, factory->lastState->closeWaits);
}

TEST(TaskExecutionService, CancelNotificationCallbackDefersStopUntilCancellationDispatchCompletes)
{
	auto factory = std::make_shared<FakeFactory>();
	TaskExecutionService service(factory);
	const auto started = service.Start(StartRequest("task.stop.from.cancel.callback"));
	ASSERT_TRUE(started.runId);
	const auto state = factory->lastState;
	std::optional<TaskExecutionOperationResult> callbackResult;
	ASSERT_TRUE(service.Subscribe([&](const auto& change) {
		if (change.state == ETaskExecutionRunState::Cancelling) callbackResult = service.Stop();
	}));

	EXPECT_TRUE(service.Cancel({ { "task.stop.from.cancel", std::nullopt }, *started.runId }).Succeeded());
	ASSERT_TRUE(callbackResult);
	EXPECT_EQ(ETaskExecutionOperationStatus::Deferred, callbackResult->status);
	EXPECT_EQ(ETaskExecutionOperationReason::StopInProgress, callbackResult->reason);
	EXPECT_EQ(1U, state->cancelRequests);
	EXPECT_TRUE(service.Snapshot().stopped);
	EXPECT_EQ(1U, state->closeRequests);
	EXPECT_EQ(1U, state->closeWaits);
}

TEST(TaskExecutionService, CloseNotificationCallbackDefersStopUntilCloseTerminalizes)
{
	auto factory = std::make_shared<FakeFactory>();
	TaskExecutionService service(factory);
	const auto started = service.Start(StartRequest("task.stop.from.close.callback"));
	ASSERT_TRUE(started.runId);
	const auto state = factory->lastState;
	std::optional<TaskExecutionOperationResult> callbackResult;
	ASSERT_TRUE(service.Subscribe([&](const auto& change) {
		if (change.state == ETaskExecutionRunState::Closing) callbackResult = service.Stop();
	}));

	EXPECT_TRUE(service.Close({ { "task.stop.from.close", std::nullopt }, *started.runId }).Succeeded());
	ASSERT_TRUE(callbackResult);
	EXPECT_EQ(ETaskExecutionOperationStatus::Deferred, callbackResult->status);
	EXPECT_EQ(ETaskExecutionOperationReason::StopInProgress, callbackResult->reason);
	EXPECT_TRUE(service.Snapshot().stopped);
	EXPECT_EQ(1U, state->closeRequests);
	EXPECT_EQ(1U, state->closeWaits);
	EXPECT_EQ(ETaskExecutionRunState::Closed, service.Snapshot().runs.front().state);
}

TEST(TaskExecutionService, SessionCompletionNotificationCallbackDefersStopUntilCompletionDispatchCompletes)
{
	auto factory = std::make_shared<FakeFactory>();
	TaskExecutionService service(factory);
	const auto started = service.Start(StartRequest("task.stop.from.completion.callback"));
	ASSERT_TRUE(started.runId);
	const auto state = factory->lastState;
	std::optional<TaskExecutionOperationResult> callbackResult;
	ASSERT_TRUE(service.Subscribe([&](const auto& change) {
		if (change.state == ETaskExecutionRunState::Exited) callbackResult = service.Stop();
	}));

	state->Exit(ETaskSessionExitKind::Exited, 0, L"completed");
	ASSERT_TRUE(callbackResult);
	EXPECT_EQ(ETaskExecutionOperationStatus::Deferred, callbackResult->status);
	EXPECT_EQ(ETaskExecutionOperationReason::StopInProgress, callbackResult->reason);
	EXPECT_TRUE(service.Snapshot().stopped);
	EXPECT_EQ(0U, state->closeRequests) << "the completion callback already guarantees host quiescence";
	EXPECT_EQ(ETaskExecutionRunState::Exited, service.Snapshot().runs.front().state);
}

TEST(TaskExecutionService, CallbackRacingCloseOwnsTheObservedTerminalOutcome)
{
	auto factory = std::make_shared<FakeFactory>();
	TaskExecutionService service(factory);
	const auto started = service.Start(StartRequest("task.close.race"));
	ASSERT_TRUE(started.runId);
	factory->lastState->onBeginClose = [state = factory->lastState] {
		state->Exit(ETaskSessionExitKind::Exited, 0, L"exited while close was requested");
	};

	const auto closed = service.Close({ { "task.close.race.operation", std::nullopt }, *started.runId });
	EXPECT_TRUE(closed.Succeeded());
	EXPECT_EQ(1U, factory->lastState->closeRequests);
	EXPECT_EQ(1U, factory->lastState->closeWaits);
	ASSERT_EQ(1U, service.Snapshot().runs.size());
	EXPECT_EQ(ETaskExecutionRunState::Exited, service.Snapshot().runs.front().state);
}

TEST(TaskExecutionService, FactoryAndSessionStartFailuresAreTerminalAndNeverReportSuccess)
{
	auto missingFactory = std::make_shared<FakeFactory>();
	missingFactory->returnNull = true;
	TaskExecutionService missingService(missingFactory);
	const auto missing = missingService.Start(StartRequest("task.missing.factory"));
	EXPECT_EQ(ETaskExecutionOperationStatus::Rejected, missing.status);
	EXPECT_EQ(ETaskExecutionOperationReason::FactoryUnavailable, missing.reason);
	EXPECT_FALSE(missing.Succeeded());
	ASSERT_EQ(1U, missingService.Snapshot().runs.size());
	EXPECT_EQ(ETaskExecutionRunState::Failed, missingService.Snapshot().runs.front().state);

	auto failingFactory = std::make_shared<FakeFactory>();
	failingFactory->nextStartResult = TaskExecutionSessionStartResult::Failure(31, L"spawn denied");
	TaskExecutionService failingService(failingFactory);
	const auto failed = failingService.Start(StartRequest("task.start.failure"));
	EXPECT_EQ(ETaskExecutionOperationStatus::Rejected, failed.status);
	EXPECT_EQ(ETaskExecutionOperationReason::SessionStartFailed, failed.reason);
	EXPECT_FALSE(failed.Succeeded());
	ASSERT_EQ(1U, failingService.Snapshot().runs.size());
	EXPECT_EQ(ETaskExecutionRunState::Failed, failingService.Snapshot().runs.front().state);
}

TEST(TaskExecutionService, SynchronousFactoryCallbackAndEarlyCloseNeverStartReturnedSession)
{
	auto completedFactory = std::make_shared<FakeFactory>();
	completedFactory->synchronousCreateExit = TaskSessionExit { ETaskSessionExitKind::Exited, 0, L"factory completed" };
	TaskExecutionService completedService(completedFactory);
	EXPECT_TRUE(completedService.Start(StartRequest("task.factory.completes")).Succeeded());
	ASSERT_NE(nullptr, completedFactory->lastState);
	EXPECT_FALSE(completedFactory->lastState->started);
	EXPECT_EQ(1U, completedFactory->lastState->closeRequests);
	EXPECT_EQ(ETaskExecutionRunState::Exited, completedService.Snapshot().runs.front().state);

	auto closingFactory = std::make_shared<FakeFactory>();
	TaskExecutionService closingService(closingFactory);
	ASSERT_TRUE(closingService.Subscribe([&closingService](const auto& change) {
		if (change.state == ETaskExecutionRunState::Starting) {
			EXPECT_TRUE(closingService.Close({ { "task.early.close", std::nullopt }, change.runId }).Succeeded());
		}
	}));
	EXPECT_TRUE(closingService.Start(StartRequest("task.closed.before.create")).Succeeded());
	ASSERT_NE(nullptr, closingFactory->lastState);
	EXPECT_FALSE(closingFactory->lastState->started);
	EXPECT_EQ(1U, closingFactory->lastState->closeRequests);
	EXPECT_EQ(ETaskExecutionRunState::Closed, closingService.Snapshot().runs.front().state);
}

TEST(TaskExecutionService, ContainsListenerFailuresAndAllowsReentrantCancellationBeforeSessionStart)
{
	auto factory = std::make_shared<FakeFactory>();
	TaskExecutionService service(factory);
	ASSERT_TRUE(service.Subscribe([](const auto&) { throw std::runtime_error("listener"); }));
	ASSERT_TRUE(service.Subscribe([&service](const auto& change) {
		if (change.state == ETaskExecutionRunState::Starting) {
			EXPECT_FALSE(service.Snapshot().runs.empty());
			EXPECT_EQ(ETaskExecutionOperationStatus::Replayed, service.Start(StartRequest("task.reentrant")).status);
			EXPECT_TRUE(service.Cancel({ { "task.reentrant.cancel", std::nullopt }, change.runId }).Succeeded());
		}
	}));

	const auto started = service.Start(StartRequest("task.reentrant"));
	ASSERT_TRUE(started.runId);
	ASSERT_NE(nullptr, factory->lastState);
	EXPECT_EQ(1U, factory->createCount);
	EXPECT_FALSE(factory->lastState->started);
	EXPECT_EQ(1U, factory->lastState->closeRequests);
	ASSERT_EQ(1U, service.Snapshot().runs.size());
	EXPECT_EQ(ETaskExecutionRunState::Cancelled, service.Snapshot().runs.front().state);
}

TEST(TaskExecutionService, StopClosesAllOwnedSessionsRejectsLaterWorkAndAllowsNoNewSubscriptions)
{
	auto factory = std::make_shared<FakeFactory>();
	TaskExecutionService service(factory);
	const auto started = service.Start(StartRequest("task.stop"));
	ASSERT_TRUE(started.runId);
	const auto session = factory->lastState;
	EXPECT_TRUE(service.Stop().Succeeded());
	EXPECT_EQ(1U, session->closeRequests);
	EXPECT_TRUE(service.Snapshot().stopped);
	EXPECT_EQ(ETaskExecutionRunState::Closed, service.Snapshot().runs.front().state);
	EXPECT_EQ(ETaskExecutionOperationStatus::Stopped, service.Start(StartRequest("task.after.stop")).status);
	EXPECT_FALSE(service.Subscribe([](const auto&) {}));
	session->Exit(ETaskSessionExitKind::Exited, 0);
	EXPECT_EQ(ETaskExecutionRunState::Closed, service.Snapshot().runs.front().state);
}

} // namespace

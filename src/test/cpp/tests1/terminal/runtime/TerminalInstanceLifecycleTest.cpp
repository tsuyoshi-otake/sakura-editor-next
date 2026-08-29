/*! @file */
#include "pch.h"

#include "terminal/runtime/TerminalInstance.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

namespace {

using namespace std::chrono_literals;

struct FakeBackendState final {
	std::mutex mutex;
	std::condition_variable condition;
	terminal::TerminalStartResult startResult = terminal::TerminalStartResult::Success();
	std::deque<std::vector<std::uint8_t>> output;
	bool closed{};
	std::size_t startCalls{};
	std::size_t closeCalls{};
};

class FakeTerminalBackend final : public terminal::ITerminalBackend {
public:
	explicit FakeTerminalBackend(std::shared_ptr<FakeBackendState> state)
		: m_state(std::move(state))
	{
	}

	terminal::TerminalStartResult Start(const terminal::TerminalLaunchOptions&) override
	{
		const std::lock_guard lock(m_state->mutex);
		++m_state->startCalls;
		return m_state->startResult;
	}

	terminal::TerminalBackendReadResult ReadOutput(
		std::span<std::uint8_t> destination, std::chrono::milliseconds timeout) override
	{
		std::unique_lock lock(m_state->mutex);
		m_state->condition.wait_for(lock, timeout, [&] {
			return m_state->closed || !m_state->output.empty();
		});
		if (m_state->output.empty()) {
			return m_state->closed
				? terminal::TerminalBackendReadResult{ terminal::TerminalBackendReadStatus::EndOfFile, 0, 0 }
				: terminal::TerminalBackendReadResult{};
		}
		auto bytes = std::move(m_state->output.front());
		m_state->output.pop_front();
		const auto count = (std::min)(destination.size(), bytes.size());
		std::copy_n(bytes.data(), count, destination.data());
		return { terminal::TerminalBackendReadStatus::Data, count, 0 };
	}

	terminal::TerminalBackendWriteResult WriteInput(std::span<const std::uint8_t> source) override
	{
		const std::lock_guard lock(m_state->mutex);
		return m_state->closed
			? terminal::TerminalBackendWriteResult{ terminal::TerminalBackendWriteStatus::Closed, 0, ERROR_OPERATION_ABORTED }
			: terminal::TerminalBackendWriteResult{ terminal::TerminalBackendWriteStatus::Completed, source.size(), 0 };
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
			if (m_state->closed) return;
			m_state->closed = true;
			++m_state->closeCalls;
		}
		m_state->condition.notify_all();
	}

private:
	std::shared_ptr<FakeBackendState> m_state;
};

terminal::TerminalCreateRequest MakeRequest(const std::uint64_t sessionId)
{
	terminal::TerminalCreateRequest request;
	request.origin = terminal::TerminalInstanceOrigin::Interactive;
	request.environmentPolicy = terminal::TerminalChildEnvironmentPolicy::InteractiveWithHarnessShim;
	request.sessionId.value = sessionId;
	request.launch.executablePath = L"fake-terminal.exe";
	request.launch.initialSize = { 80, 25 };
	return request;
}

terminal::TerminalRuntimeSessionFactory MakeFactory(
	const std::shared_ptr<FakeBackendState>& state,
	const std::shared_ptr<terminal::TerminalSessionCallbacks>& callbacksCopy = {})
{
	return [state, callbacksCopy](terminal::TerminalSessionCallbacks callbacks) {
		if (callbacksCopy) *callbacksCopy = callbacks;
		return std::make_unique<terminal::CTerminalSession>(
			std::make_unique<FakeTerminalBackend>(state), std::move(callbacks));
	};
}

terminal::TerminalInstance MakeInstance(
	const std::uint64_t id,
	const std::shared_ptr<FakeBackendState>& state,
	terminal::TerminalInstanceEventCallback callback = {})
{
	terminal::TerminalRuntimeGeneration runtimeGeneration{ 1 };
	terminal::TerminalInstanceDependencies dependencies;
	dependencies.createSession = MakeFactory(state);
	return terminal::TerminalInstance(
		terminal::TerminalInstanceId{ id }, runtimeGeneration, id, MakeRequest(id),
		std::move(dependencies), std::move(callback));
}

TEST(TerminalInstanceLifecycle, ReservedClosePublishesOneTerminalCancellation)
{
	auto backendState = std::make_shared<FakeBackendState>();
	auto instance = MakeInstance(1, backendState);

	EXPECT_EQ(terminal::TerminalInstanceState::Reserved, instance.State());
	instance.RequestCancel();

	ASSERT_EQ(terminal::TerminalInstanceState::Terminalized, instance.State());
	const auto outcome = instance.Outcome();
	ASSERT_TRUE(outcome.has_value());
	EXPECT_EQ(terminal::TerminalInstanceOutcomeKind::StartCancelled, outcome->kind);
	EXPECT_TRUE(outcome->IsQuiescent());
	EXPECT_EQ(0u, backendState->startCalls);
	EXPECT_TRUE(instance.Start().Cancelled());
}

TEST(TerminalInstanceLifecycle, StartFailureIsTypedAndQuiesced)
{
	auto backendState = std::make_shared<FakeBackendState>();
	backendState->startResult = terminal::TerminalStartResult::Failure(ERROR_FILE_NOT_FOUND, L"fake backend failure");
	std::atomic<unsigned> completed{};
	auto instance = MakeInstance(2, backendState, [&](const terminal::TerminalInstanceEvent& event) {
		if (event.kind == terminal::TerminalInstanceEventKind::Completed) completed.fetch_add(1);
	});

	const auto start = instance.Start();
	EXPECT_EQ(terminal::TerminalInstanceStartStatus::StartFailed, start.status);
	EXPECT_EQ(ERROR_FILE_NOT_FOUND, start.errorCode);
	EXPECT_EQ(terminal::TerminalInstanceState::Terminalized, instance.State());
	const auto outcome = instance.Outcome();
	ASSERT_TRUE(outcome.has_value());
	EXPECT_EQ(terminal::TerminalInstanceOutcomeKind::StartFailed, outcome->kind);
	EXPECT_EQ(ERROR_FILE_NOT_FOUND, outcome->platformErrorCode.value_or(0));
	EXPECT_TRUE(outcome->IsQuiescent());
	EXPECT_EQ(1u, completed.load());
}

TEST(TerminalInstanceLifecycle, CloseRequestedDuringReservationCancelsBeforeFactory)
{
	auto backendState = std::make_shared<FakeBackendState>();
	std::atomic<bool> requested{};
	terminal::TerminalInstance* current = nullptr;
	terminal::TerminalRuntimeSessionFactory factory = MakeFactory(backendState);
	terminal::TerminalInstanceDependencies dependencies;
	dependencies.createSession = [factory, &current, &requested](terminal::TerminalSessionCallbacks callbacks) {
		return factory(std::move(callbacks));
	};
	auto instance = std::make_unique<terminal::TerminalInstance>(
		terminal::TerminalInstanceId{ 3 }, terminal::TerminalRuntimeGeneration{ 1 }, 3,
		MakeRequest(3), std::move(dependencies), [&](const terminal::TerminalInstanceEvent& event) {
			if (event.kind == terminal::TerminalInstanceEventKind::StateChanged
				&& event.state == terminal::TerminalInstanceState::Starting
				&& !requested.exchange(true)) {
				current->RequestCancel();
			}
		});
	current = instance.get();

	const auto start = instance->Start();
	EXPECT_TRUE(start.Cancelled());
	EXPECT_EQ(terminal::TerminalInstanceState::Terminalized, instance->State());
	EXPECT_EQ(0u, backendState->startCalls);
	ASSERT_TRUE(instance->Outcome().has_value());
	EXPECT_EQ(terminal::TerminalInstanceOutcomeKind::StartCancelled, instance->Outcome()->kind);
}

TEST(TerminalInstanceLifecycle, ExplicitClosePublishesOneClosedOutcome)
{
	auto backendState = std::make_shared<FakeBackendState>();
	std::atomic<unsigned> completed{};
	auto instance = MakeInstance(4, backendState, [&](const terminal::TerminalInstanceEvent& event) {
		if (event.kind == terminal::TerminalInstanceEventKind::Completed) completed.fetch_add(1);
	});

	ASSERT_TRUE(instance.Start().Succeeded());
	const auto close = instance.WaitForClose(std::chrono::steady_clock::now() + 2s);
	EXPECT_EQ(terminal::TerminalInstanceCloseWaitStatus::Closed, close.status);
	ASSERT_TRUE(close.outcome.has_value());
	EXPECT_EQ(terminal::TerminalInstanceOutcomeKind::Closed, close.outcome->kind);
	EXPECT_TRUE(close.outcome->IsQuiescent());
	EXPECT_EQ(terminal::TerminalInstanceState::Terminalized, instance.State());
	EXPECT_EQ(1u, completed.load());
	EXPECT_EQ(1u, backendState->closeCalls);
	EXPECT_EQ(terminal::TerminalInstanceCloseWaitStatus::Closed,
		instance.WaitForClose(std::chrono::steady_clock::now() + 1s).status);
	EXPECT_EQ(1u, completed.load());
}

TEST(TerminalInstanceLifecycle, LateSessionCallbackIsFencedAfterInstanceDestruction)
{
	auto backendState = std::make_shared<FakeBackendState>();
	auto callbacksCopy = std::make_shared<terminal::TerminalSessionCallbacks>();
	std::atomic<unsigned> events{};
	terminal::TerminalInstanceDependencies dependencies;
	dependencies.createSession = MakeFactory(backendState, callbacksCopy);
	{
		terminal::TerminalInstance instance(
			terminal::TerminalInstanceId{ 5 }, terminal::TerminalRuntimeGeneration{ 1 }, 5,
			MakeRequest(5), std::move(dependencies), [&](const terminal::TerminalInstanceEvent&) {
				events.fetch_add(1);
			});
		ASSERT_TRUE(instance.Start().Succeeded());
		instance.Close();
	}
	const auto before = events.load();
	ASSERT_TRUE(callbacksCopy->outputAvailable);
	ASSERT_TRUE(callbacksCopy->stateChanged);
	ASSERT_TRUE(callbacksCopy->completed);
	callbacksCopy->outputAvailable();
	callbacksCopy->stateChanged(terminal::TerminalSessionState::Running, 0);
	callbacksCopy->completed({ terminal::TerminalSessionCompletionKind::Closed, 0, 0 });
	EXPECT_EQ(before, events.load());
}

} // namespace

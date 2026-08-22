/*! @file */
#include "pch.h"

#include "terminal/TerminalSessionRetirementService.h"
#include "terminal/TerminalWorkerRetirementService.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

namespace {

using namespace std::chrono_literals;

template <typename Predicate>
bool WaitUntil( Predicate&& predicate, const std::chrono::milliseconds timeout = 2s )
{
	const auto deadline = std::chrono::steady_clock::now() + timeout;
	while( std::chrono::steady_clock::now() < deadline ) {
		if( predicate() ) return true;
		std::this_thread::sleep_for(1ms);
	}
	return predicate();
}

struct BackendState final {
	std::mutex mutex;
	std::condition_variable condition;
	bool closed{};
	bool releaseExit{};
	bool waitEntered{};
	std::size_t closeCalls{};
};

class RetirementBackend final : public terminal::ITerminalBackend {
public:
	 explicit RetirementBackend( std::shared_ptr<BackendState> state )
		: m_state(std::move(state))
	{
	}

	terminal::TerminalStartResult Start( const terminal::TerminalLaunchOptions& ) override
	{
		return terminal::TerminalStartResult::Success();
	}

	terminal::TerminalBackendReadResult ReadOutput(
		std::span<std::uint8_t>, std::chrono::milliseconds timeout ) override
	{
		std::unique_lock lock(m_state->mutex);
		m_state->condition.wait_for(lock, std::min(timeout, 10ms), [&] { return m_state->closed; });
		return m_state->closed
			? terminal::TerminalBackendReadResult{ terminal::TerminalBackendReadStatus::EndOfFile, 0, 0 }
			: terminal::TerminalBackendReadResult{};
	}

	terminal::TerminalBackendWriteResult WriteInput(
		std::span<const std::uint8_t> source ) override
	{
		std::lock_guard lock(m_state->mutex);
		return m_state->closed
			? terminal::TerminalBackendWriteResult{ terminal::TerminalBackendWriteStatus::Closed, 0, ERROR_OPERATION_ABORTED }
			: terminal::TerminalBackendWriteResult{ terminal::TerminalBackendWriteStatus::Completed, source.size(), 0 };
	}

	terminal::TerminalBackendOperationResult Resize( terminal::TerminalSize ) override
	{
		return { true, 0 };
	}

	void RequestGracefulClose() noexcept override
	{
		m_state->condition.notify_all();
	}

	terminal::TerminalBackendExitResult WaitForExit( std::chrono::milliseconds ) noexcept override
	{
		std::unique_lock lock(m_state->mutex);
		m_state->waitEntered = true;
		m_state->condition.notify_all();
		m_state->condition.wait(lock, [&] { return m_state->releaseExit; });
		return { terminal::TerminalBackendExitStatus::Exited, 0, 0 };
	}

	void ForceTerminate() noexcept override
	{
		ReleaseExit();
	}

	void Close() noexcept override
	{
		{
			std::lock_guard lock(m_state->mutex);
			if( m_state->closed ) return;
			m_state->closed = true;
			++m_state->closeCalls;
		}
		m_state->condition.notify_all();
	}

	void ReleaseExit() noexcept
	{
		{
			std::lock_guard lock(m_state->mutex);
			m_state->releaseExit = true;
		}
		m_state->condition.notify_all();
	}

private:
	std::shared_ptr<BackendState> m_state;
};

terminal::TerminalLaunchOptions LaunchOptions()
{
	terminal::TerminalLaunchOptions options;
	options.executablePath = L"C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe";
	options.initialSize = { 80, 24 };
	return options;
}

struct ReleaseAtScopeExit final {
	std::shared_ptr<BackendState> state;
	~ReleaseAtScopeExit()
	{
		{
			std::lock_guard lock(state->mutex);
			state->releaseExit = true;
		}
		state->condition.notify_all();
	}
};

TEST(TerminalSessionRetirementService, UiHandoffDoesNotWaitForStalledBackend)
{
	auto& service = terminal::TerminalSessionRetirementService::Instance();
	ASSERT_TRUE(WaitUntil([&] { return service.ReservedOrPendingCount() == 0; }));

	auto reservation = service.TryReserve();
	ASSERT_TRUE(reservation.has_value());
	auto state = std::make_shared<BackendState>();
	ReleaseAtScopeExit release{ state };
	auto session = std::make_unique<terminal::CTerminalSession>(
		std::make_unique<RetirementBackend>(state));
	ASSERT_TRUE(session->Start(LaunchOptions()).succeeded);
	ASSERT_TRUE(reservation->Observation());

	// The fake backend intentionally stalls its exit wait. The handoff is the
	// exact operation performed by the UI-owned terminal tab manager.
	const auto begin = std::chrono::steady_clock::now();
	session->BeginClose();
	const auto result = service.Handoff(session, std::move(*reservation));
	const auto elapsed = std::chrono::steady_clock::now() - begin;
	EXPECT_TRUE(result.Accepted());
	EXPECT_FALSE(session);
	EXPECT_LT(elapsed, 100ms);
	ASSERT_TRUE(WaitUntil([&] {
		std::lock_guard lock(state->mutex);
		return state->waitEntered;
	}));
	EXPECT_EQ(terminal::TerminalSessionRetirementPhase::Joining,
		result.observation->Snapshot().phase);

	{
		std::lock_guard lock(state->mutex);
		state->releaseExit = true;
	}
	state->condition.notify_all();
	ASSERT_TRUE(WaitUntil([&] { return service.ReservedOrPendingCount() == 0; }));
	const auto snapshot = result.observation->Snapshot();
	EXPECT_EQ(terminal::TerminalSessionRetirementStatus::Retired, snapshot.status);
	EXPECT_EQ(terminal::TerminalSessionRetirementPhase::Completed, snapshot.phase);
	EXPECT_EQ(terminal::TerminalSessionCloseWaitStatus::Closed, snapshot.waitStatus);
	{
		std::lock_guard lock(state->mutex);
		EXPECT_EQ(1u, state->closeCalls);
	}
}

TEST(TerminalSessionRetirementService, AdmissionIsBoundedBeforeAnyHandoff)
{
	auto& service = terminal::TerminalSessionRetirementService::Instance();
	ASSERT_TRUE(WaitUntil([&] { return service.ReservedOrPendingCount() == 0; }));
	std::array<std::optional<terminal::TerminalSessionRetirementService::Reservation>,
		terminal::TerminalSessionRetirementService::kMaximumSessions> reservations;
	for( auto& reservation : reservations ) {
		reservation = service.TryReserve();
		ASSERT_TRUE(reservation.has_value());
	}
	EXPECT_FALSE(service.TryReserve().has_value());
	EXPECT_EQ(terminal::TerminalSessionRetirementService::kMaximumSessions,
		service.ReservedOrPendingCount());
	for( auto& reservation : reservations ) reservation.reset();
	EXPECT_EQ(0u, service.ReservedOrPendingCount());
}

TEST(TerminalWorkerRetirementService, JoinsAWorkerThroughTheBoundedReaper)
{
	auto& service = terminal::TerminalWorkerRetirementService::Instance();
	ASSERT_TRUE(WaitUntil([&] { return service.ReservedOrPendingCount() == 0; }));
	auto reservation = service.TryReserve();
	ASSERT_TRUE(reservation.has_value());
	std::atomic<bool> finished{ false };
	auto lifetime = std::make_shared<int>(42);
	std::thread worker([&] {
		std::this_thread::sleep_for(10ms);
		finished.store(true, std::memory_order_release);
	});

	const auto status = service.Retire(std::move(worker), std::move(*reservation), lifetime);
	EXPECT_EQ(terminal::TerminalWorkerRetirementStatus::Retired, status);
	ASSERT_TRUE(WaitUntil([&] { return finished.load(std::memory_order_acquire); }));
	ASSERT_TRUE(WaitUntil([&] { return service.ReservedOrPendingCount() == 0; }));
}

TEST(TerminalWorkerRetirementService, RunsFallbackTaskThroughBoundedReaper)
{
	auto& service = terminal::TerminalWorkerRetirementService::Instance();
	ASSERT_TRUE(WaitUntil([&] { return service.ReservedOrPendingCount() == 0; }));
	auto reservation = service.TryReserve();
	ASSERT_TRUE(reservation.has_value());
	std::atomic<bool> started{ false };
	std::atomic<bool> finished{ false };
	const auto status = service.RetireTask(
		std::move(*reservation),
		[&] {
			started.store(true, std::memory_order_release);
			std::this_thread::sleep_for(10ms);
			finished.store(true, std::memory_order_release);
		});
	EXPECT_EQ(terminal::TerminalWorkerRetirementStatus::Retired, status);
	ASSERT_TRUE(WaitUntil([&] { return started.load(std::memory_order_acquire); }));
	ASSERT_TRUE(WaitUntil([&] { return finished.load(std::memory_order_acquire); }));
	ASSERT_TRUE(WaitUntil([&] { return service.ReservedOrPendingCount() == 0; }));
}

TEST(TerminalWorkerRetirementService, SessionDestructionDoesNotWaitForBackendExit)
{
	auto& service = terminal::TerminalWorkerRetirementService::Instance();
	ASSERT_TRUE(WaitUntil([&] { return service.ReservedOrPendingCount() == 0; }));
	auto state = std::make_shared<BackendState>();
	auto session = std::make_unique<terminal::CTerminalSession>(
		std::make_unique<RetirementBackend>(state));
	ASSERT_TRUE(session->Start(LaunchOptions()).succeeded);

	const auto begin = std::chrono::steady_clock::now();
	session.reset();
	const auto elapsed = std::chrono::steady_clock::now() - begin;
	EXPECT_LT(elapsed, 100ms);
	ASSERT_TRUE(WaitUntil([&] {
		std::lock_guard lock(state->mutex);
		return state->waitEntered;
	}));

	{
		std::lock_guard lock(state->mutex);
		state->releaseExit = true;
	}
	state->condition.notify_all();
	ASSERT_TRUE(WaitUntil([&] { return service.ReservedOrPendingCount() == 0; }));
	{
		std::lock_guard lock(state->mutex);
		EXPECT_EQ(1u, state->closeCalls);
	}
}

} // namespace

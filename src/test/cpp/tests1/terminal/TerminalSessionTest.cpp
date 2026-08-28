/*! @file */
#include "pch.h"
#include "terminal/PowerShellLocator.h"
#include "terminal/session/TerminalSession.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string_view>
#include <thread>

namespace {

using namespace std::chrono_literals;

template<typename Predicate>
bool WaitUntil( Predicate predicate, std::chrono::milliseconds timeout = 2000ms )
{
	const auto deadline = std::chrono::steady_clock::now() + timeout;
	while( std::chrono::steady_clock::now() < deadline ) {
		if( predicate() ) return true;
		std::this_thread::sleep_for(1ms);
	}
	return predicate();
}

struct FakeTerminalBackendLifetimeCounters {
	std::atomic<std::size_t> startCalls{ 0 };
	std::atomic<std::size_t> closeCalls{ 0 };
};

class FakeTerminalBackend final : public terminal::ITerminalBackend {
public:
	explicit FakeTerminalBackend( std::shared_ptr<FakeTerminalBackendLifetimeCounters> lifetimeCounters = {} )
		: m_lifetimeCounters(std::move(lifetimeCounters))
	{
	}

	struct ReadEvent {
		terminal::TerminalBackendReadStatus status = terminal::TerminalBackendReadStatus::Data;
		std::vector<std::uint8_t> bytes;
		std::size_t offset = 0;
		std::uint32_t error = 0;
	};

	terminal::TerminalStartResult startResult = terminal::TerminalStartResult::Success();
	terminal::TerminalLaunchOptions launchOptions;
	bool blockWrites = false;
	bool failWrites = false;
	bool failResize = false;
	bool blockWaitForExit = false;
	std::deque<bool> waitResults;
	std::deque<terminal::TerminalBackendExitResult> exitResults;

	terminal::TerminalStartResult Start( const terminal::TerminalLaunchOptions& options ) override
	{
		const std::lock_guard lock(m_mutex);
		launchOptions = options;
		++startCalls;
		if( m_lifetimeCounters ) m_lifetimeCounters->startCalls.fetch_add(1, std::memory_order_relaxed);
		return startResult;
	}

	terminal::TerminalBackendReadResult ReadOutput( std::span<std::uint8_t> destination, std::chrono::milliseconds timeout ) override
	{
		std::unique_lock lock(m_mutex);
		m_cv.wait_for( lock, timeout, [&] { return closed || !readEvents.empty(); } );
		if( readEvents.empty() ) return closed ? terminal::TerminalBackendReadResult{ terminal::TerminalBackendReadStatus::EndOfFile, 0, 0 } : terminal::TerminalBackendReadResult{};
		auto& event = readEvents.front();
		if( event.status != terminal::TerminalBackendReadStatus::Data ) {
			const auto result = terminal::TerminalBackendReadResult{ event.status, 0, event.error };
			readEvents.pop_front();
			return result;
		}
		const auto count = std::min(destination.size(), event.bytes.size() - event.offset);
		std::copy_n(event.bytes.data() + event.offset, count, destination.data());
		event.offset += count;
		totalBytesRead += count;
		if( event.offset == event.bytes.size() ) readEvents.pop_front();
		return { terminal::TerminalBackendReadStatus::Data, count, 0 };
	}

	terminal::TerminalBackendWriteResult WriteInput( std::span<const std::uint8_t> source ) override
	{
		std::unique_lock lock(m_mutex);
		writeEntered = true;
		++writeCalls;
		m_cv.notify_all();
		m_cv.wait( lock, [&] { return !blockWrites || closed; } );
		if( closed ) return { terminal::TerminalBackendWriteStatus::Closed, 0, 109 };
		if( failWrites ) return { terminal::TerminalBackendWriteStatus::Failed, 0, 109 };
		writtenBytes.insert(writtenBytes.end(), source.begin(), source.end());
		return { terminal::TerminalBackendWriteStatus::Completed, source.size(), 0 };
	}

	terminal::TerminalBackendOperationResult Resize( terminal::TerminalSize size ) override
	{
		const std::lock_guard lock(m_mutex);
		resizes.push_back(size);
		return failResize ? terminal::TerminalBackendOperationResult{ false, 87 } : terminal::TerminalBackendOperationResult{ true, 0 };
	}

	void RequestGracefulClose() noexcept override
	{
		const std::lock_guard lock(m_mutex);
		++gracefulCloseCalls;
	}

	terminal::TerminalBackendExitResult WaitForExit( std::chrono::milliseconds timeout ) noexcept override
	{
		std::unique_lock lock(m_mutex);
		waitTimeouts.push_back(timeout);
		waitEntered = true;
		m_cv.notify_all();
		m_cv.wait(lock, [&] { return !blockWaitForExit; });
		if( !exitResults.empty() ) {
			const auto result = exitResults.front();
			exitResults.pop_front();
			if( result.status == terminal::TerminalBackendExitStatus::Exited ) {
				processExited = true;
				processExitCode = result.exitCode;
			}
			return result;
		}
		if( waitResults.empty() ) return processExited
			? terminal::TerminalBackendExitResult{ terminal::TerminalBackendExitStatus::Exited, processExitCode, 0 }
			: terminal::TerminalBackendExitResult{};
		const bool result = waitResults.front();
		waitResults.pop_front();
		if( result ) processExited = true;
		return result ? terminal::TerminalBackendExitResult{ terminal::TerminalBackendExitStatus::Exited, processExitCode, 0 }
			: terminal::TerminalBackendExitResult{};
	}

	void ForceTerminate() noexcept override
	{
		const std::lock_guard lock(m_mutex);
		++forceTerminateCalls;
		processExited = true;
	}

	void Close() noexcept override
	{
		{
			const std::lock_guard lock(m_mutex);
			if( closed ) return;
			closed = true;
			++closeCalls;
			if( m_lifetimeCounters ) m_lifetimeCounters->closeCalls.fetch_add(1, std::memory_order_relaxed);
		}
		m_cv.notify_all();
	}

	void PushData( std::vector<std::uint8_t> bytes )
	{
		{
			const std::lock_guard lock(m_mutex);
			readEvents.push_back({ terminal::TerminalBackendReadStatus::Data, std::move(bytes), 0, 0 });
		}
		m_cv.notify_all();
	}

	void PushEndOfFile()
	{
		{
			const std::lock_guard lock(m_mutex);
			readEvents.push_back({ terminal::TerminalBackendReadStatus::EndOfFile, {}, 0, 0 });
		}
		m_cv.notify_all();
	}

	void PushReadFailure( std::uint32_t error )
	{
		{
			const std::lock_guard lock(m_mutex);
			readEvents.push_back({ terminal::TerminalBackendReadStatus::Failed, {}, 0, error });
		}
		m_cv.notify_all();
	}

	void UnblockWrites()
	{
		{
			const std::lock_guard lock(m_mutex);
			blockWrites = false;
		}
		m_cv.notify_all();
	}

	void UnblockExitWait()
	{
		{
			const std::lock_guard lock(m_mutex);
			blockWaitForExit = false;
		}
		m_cv.notify_all();
	}

	std::size_t TotalBytesRead() const { const std::lock_guard lock(m_mutex); return totalBytesRead; }
	bool WriteEntered() const { const std::lock_guard lock(m_mutex); return writeEntered; }
	bool WaitEntered() const { const std::lock_guard lock(m_mutex); return waitEntered; }
	std::size_t WrittenByteCount() const { const std::lock_guard lock(m_mutex); return writtenBytes.size(); }
	std::vector<terminal::TerminalSize> Resizes() const { const std::lock_guard lock(m_mutex); return resizes; }
	std::size_t CloseCalls() const { const std::lock_guard lock(m_mutex); return closeCalls; }
	std::size_t StartCalls() const { const std::lock_guard lock(m_mutex); return startCalls; }
	std::size_t GracefulCloseCalls() const { const std::lock_guard lock(m_mutex); return gracefulCloseCalls; }
	std::size_t ForceTerminateCalls() const { const std::lock_guard lock(m_mutex); return forceTerminateCalls; }
	std::vector<std::chrono::milliseconds> WaitTimeouts() const { const std::lock_guard lock(m_mutex); return waitTimeouts; }

private:
	std::shared_ptr<FakeTerminalBackendLifetimeCounters> m_lifetimeCounters;
	mutable std::mutex m_mutex;
	std::condition_variable m_cv;
	std::deque<ReadEvent> readEvents;
	std::vector<std::uint8_t> writtenBytes;
	std::vector<terminal::TerminalSize> resizes;
	std::vector<std::chrono::milliseconds> waitTimeouts;
	std::size_t totalBytesRead = 0;
	std::size_t startCalls = 0;
	std::size_t writeCalls = 0;
	std::size_t closeCalls = 0;
	std::size_t gracefulCloseCalls = 0;
	std::size_t forceTerminateCalls = 0;
	bool writeEntered = false;
	bool waitEntered = false;
	bool processExited = false;
	std::uint32_t processExitCode = 0;
	bool closed = false;
};

terminal::TerminalLaunchOptions DefaultLaunchOptions()
{
	return { L"C:\\Program Files\\PowerShell\\7\\pwsh.exe", { L"-NoLogo" }, L"C:\\Work Folder\\Japanese 日本語", { 120, 30 } };
}

TEST(TerminalSession, StartFailureAlwaysEndsInFailedAndCloseIsIdempotent)
{
	auto backend = std::make_unique<FakeTerminalBackend>();
	auto* fake = backend.get();
	fake->startResult = terminal::TerminalStartResult::Failure(5, L"denied");
	std::vector<terminal::TerminalSessionState> states;
	terminal::CTerminalSession session( std::move(backend), { {}, [&](auto state, auto) { states.push_back(state); } } );

	const auto result = session.Start(DefaultLaunchOptions());
	EXPECT_FALSE(result.succeeded);
	EXPECT_EQ(terminal::TerminalSessionState::Failed, session.GetState());
	ASSERT_EQ(2u, states.size());
	EXPECT_EQ(terminal::TerminalSessionState::Starting, states[0]);
	EXPECT_EQ(terminal::TerminalSessionState::Failed, states[1]);
	session.Close();
	session.Close();
	EXPECT_EQ(1u, fake->CloseCalls());
}

TEST(TerminalSession, InvalidLaunchAlsoReachesFailed)
{
	auto backend = std::make_unique<FakeTerminalBackend>();
	terminal::CTerminalSession session(std::move(backend));
	auto options = DefaultLaunchOptions();
	options.executablePath.clear();
	EXPECT_FALSE(session.Start(options).succeeded);
	EXPECT_EQ(terminal::TerminalSessionState::Failed, session.GetState());
}

TEST(TerminalSession, EndOfFileTransitionsRunningSessionToExited)
{
	auto backend = std::make_unique<FakeTerminalBackend>();
	auto* fake = backend.get();
	std::atomic<bool> sawClosing{ false };
	terminal::CTerminalSession session( std::move(backend), { {}, [&](auto state, auto) {
		if( state == terminal::TerminalSessionState::Closing ) sawClosing.store(true);
	} } );
	ASSERT_TRUE(session.Start(DefaultLaunchOptions()).succeeded);
	fake->waitResults = { true };
	fake->PushEndOfFile();
	ASSERT_TRUE(WaitUntil([&] { return session.GetState() == terminal::TerminalSessionState::Exited; }));
	EXPECT_TRUE(sawClosing.load());
	EXPECT_EQ(1u, fake->CloseCalls());
	session.Close();
	EXPECT_EQ(1u, fake->CloseCalls());
}

TEST(TerminalSession, UnexpectedOutputPipeFailureTransitionsToFailed)
{
	auto backend = std::make_unique<FakeTerminalBackend>();
	auto* fake = backend.get();
	terminal::CTerminalSession session(std::move(backend));
	ASSERT_TRUE(session.Start(DefaultLaunchOptions()).succeeded);
	fake->PushReadFailure(109);
	ASSERT_TRUE(WaitUntil([&] { return session.GetState() == terminal::TerminalSessionState::Failed; }));
	EXPECT_EQ(109u, session.GetLastError());
	EXPECT_EQ(1u, fake->ForceTerminateCalls());
	EXPECT_EQ(1u, fake->CloseCalls());
}

TEST(TerminalSession, CloseUsesBoundedGraceThenJobFallbackAndIsIdempotent)
{
	auto backend = std::make_unique<FakeTerminalBackend>();
	auto* fake = backend.get();
	fake->waitResults = { false, true };
	std::vector<terminal::TerminalSessionState> states;
	terminal::CTerminalSession session( std::move(backend), { {}, [&](auto state, auto) { states.push_back(state); } } );
	ASSERT_TRUE(session.Start(DefaultLaunchOptions()).succeeded);
	session.Close();
	session.Close();
	EXPECT_EQ(terminal::TerminalSessionState::Exited, session.GetState());
	EXPECT_EQ(1u, fake->GracefulCloseCalls());
	EXPECT_EQ(1u, fake->ForceTerminateCalls());
	EXPECT_EQ(1u, fake->CloseCalls());
	const auto waits = fake->WaitTimeouts();
	ASSERT_EQ(2u, waits.size());
	EXPECT_EQ(terminal::CTerminalSession::kGracefulCloseTimeout, waits[0]);
	EXPECT_EQ(terminal::CTerminalSession::kForcedCloseTimeout, waits[1]);
	EXPECT_EQ((std::vector<terminal::TerminalSessionState>{ terminal::TerminalSessionState::Starting, terminal::TerminalSessionState::Running, terminal::TerminalSessionState::Closing, terminal::TerminalSessionState::Exited }), states);
}

class TerminalTraceTestDirectory final {
public:
	TerminalTraceTestDirectory()
	{
		static std::atomic<std::uint64_t> sequence{ 0 };
		m_path = std::filesystem::temp_directory_path() /
			(L"sakura-terminal-trace-test-" + std::to_wstring(::GetCurrentProcessId()) + L"-" +
				std::to_wstring(sequence.fetch_add(1, std::memory_order_relaxed) + 1));
		std::filesystem::create_directories(m_path);
	}

	~TerminalTraceTestDirectory()
	{
		std::error_code error;
		std::filesystem::remove_all(m_path, error);
	}

	[[nodiscard]] const std::filesystem::path& Path() const noexcept { return m_path; }

private:
	std::filesystem::path m_path;
};

std::string ReadTextFile( const std::filesystem::path& path )
{
	std::ifstream stream(path, std::ios::binary);
	std::ostringstream contents;
	contents << stream.rdbuf();
	return contents.str();
}

TEST(TerminalSession, BeginCloseIsNonblockingAndWaitReturnsOnlyAfterNormalQuiescence)
{
	auto backend = std::make_unique<FakeTerminalBackend>();
	auto* fake = backend.get();
	fake->blockWaitForExit = true;
	fake->waitResults = { true };
	terminal::CTerminalSession session(std::move(backend));
	ASSERT_TRUE(session.Start(DefaultLaunchOptions()).succeeded);

	const auto began = std::chrono::steady_clock::now();
	session.BeginClose();
	EXPECT_LT(std::chrono::steady_clock::now() - began, 100ms);
	ASSERT_TRUE(WaitUntil([&] { return fake->WaitEntered(); }));
	EXPECT_EQ(terminal::TerminalSessionState::Closing, session.GetState());

	fake->UnblockExitWait();
	const auto closed = session.WaitForClose(std::chrono::steady_clock::now() + 1s);
	EXPECT_EQ(terminal::TerminalSessionCloseWaitStatus::Closed, closed.status);
	EXPECT_TRUE(closed.IsQuiescent());
	EXPECT_EQ(terminal::TerminalSessionState::Exited, session.GetState());
	EXPECT_EQ(1u, fake->CloseCalls());
}

TEST(TerminalSession, EndOfFileWaitsForRealExitAndRetainsNonzeroRootCode)
{
	auto backend = std::make_unique<FakeTerminalBackend>();
	auto* fake = backend.get();
	std::mutex completionMutex;
	std::vector<terminal::TerminalSessionCompletionResult> completions;
	terminal::CTerminalSession session( std::move(backend), terminal::TerminalSessionCallbacks{ {}, {}, [&](const auto result) {
		const std::lock_guard lock(completionMutex);
		completions.push_back(result);
	} } );
	ASSERT_TRUE(session.Start(DefaultLaunchOptions()).succeeded);
	fake->exitResults = {
		{ terminal::TerminalBackendExitStatus::TimedOut, 0, 0 },
		{ terminal::TerminalBackendExitStatus::Exited, 37, 0 },
	};
	fake->PushEndOfFile();
	ASSERT_TRUE(WaitUntil([&] { return session.GetState() == terminal::TerminalSessionState::Exited; }));
	ASSERT_TRUE(WaitUntil([&] { const std::lock_guard lock(completionMutex); return completions.size() == 1; }));
	const auto waits = fake->WaitTimeouts();
	EXPECT_GE(waits.size(), 2u);
	const std::lock_guard lock(completionMutex);
	ASSERT_EQ(1u, completions.size());
	EXPECT_EQ(terminal::TerminalSessionCompletionKind::Exited, completions.front().kind);
	EXPECT_EQ(37u, completions.front().exitCode);
	EXPECT_EQ(0u, completions.front().errorCode);
}

TEST(TerminalSession, ExitObservationFailureBecomesOneFailedPostQuiescenceCompletion)
{
	auto backend = std::make_unique<FakeTerminalBackend>();
	auto* fake = backend.get();
	std::atomic<unsigned> completionCount{};
	terminal::TerminalSessionCompletionResult completion;
	terminal::CTerminalSession session( std::move(backend), terminal::TerminalSessionCallbacks{ {}, {}, [&](const auto result) {
		completion = result;
		completionCount.fetch_add(1);
	} } );
	ASSERT_TRUE(session.Start(DefaultLaunchOptions()).succeeded);
	fake->exitResults = { { terminal::TerminalBackendExitStatus::Failed, 0, ERROR_ACCESS_DENIED } };
	fake->PushEndOfFile();
	ASSERT_TRUE(WaitUntil([&] { return completionCount.load() == 1; }));
	EXPECT_EQ(terminal::TerminalSessionState::Failed, session.GetState());
	EXPECT_EQ(terminal::TerminalSessionCompletionKind::Failed, completion.kind);
	EXPECT_EQ(ERROR_ACCESS_DENIED, completion.errorCode);
	session.Close();
	EXPECT_EQ(1u, completionCount.load());
}

TEST(TerminalSession, RequestedForcedCloseCompletesOnceAfterBackendCloseAndPreservesObservedCode)
{
	auto backend = std::make_unique<FakeTerminalBackend>();
	auto* fake = backend.get();
	fake->exitResults = {
		{ terminal::TerminalBackendExitStatus::TimedOut, 0, 0 },
		{ terminal::TerminalBackendExitStatus::Exited, 1, 0 },
	};
	std::atomic<unsigned> completionCount{};
	std::atomic<bool> backendAlreadyClosed{};
	terminal::TerminalSessionCompletionResult completion;
	terminal::CTerminalSession session( std::move(backend), terminal::TerminalSessionCallbacks{ {}, {}, [&](const auto result) {
		completion = result;
		backendAlreadyClosed.store(fake->CloseCalls() == 1);
		completionCount.fetch_add(1);
	} } );
	ASSERT_TRUE(session.Start(DefaultLaunchOptions()).succeeded);
	EXPECT_TRUE(session.WaitForClose(std::chrono::steady_clock::now() + 2s).IsQuiescent());
	EXPECT_EQ(1u, fake->ForceTerminateCalls());
	EXPECT_EQ(1u, completionCount.load());
	EXPECT_TRUE(backendAlreadyClosed.load());
	EXPECT_EQ(terminal::TerminalSessionCompletionKind::Closed, completion.kind);
	EXPECT_EQ(1u, completion.exitCode);
	session.Close();
	EXPECT_EQ(1u, completionCount.load());
}

TEST(TerminalSession, CompletionCallbackMayDestroySessionAfterQuiescence)
{
	auto lifetimeCounters = std::make_shared<FakeTerminalBackendLifetimeCounters>();
	auto backend = std::make_unique<FakeTerminalBackend>(lifetimeCounters);
	std::unique_ptr<terminal::CTerminalSession> session;
	std::atomic<unsigned> completionCount{};
	std::atomic<bool> destroyed{};
	session = std::make_unique<terminal::CTerminalSession>(std::move(backend), terminal::TerminalSessionCallbacks{ {}, {}, [&](const auto) {
		completionCount.fetch_add(1);
		session.reset();
		destroyed.store(true);
	} });
	ASSERT_TRUE(session->Start(DefaultLaunchOptions()).succeeded);
	session->BeginClose();
	ASSERT_TRUE(WaitUntil([&] { return destroyed.load(); }));
	EXPECT_FALSE(session);
	EXPECT_EQ(1u, completionCount.load());
	EXPECT_EQ(1u, lifetimeCounters->closeCalls.load(std::memory_order_relaxed));
}

TEST(TerminalSession, WaitDeadlineReportsExceededOnlyAfterForcedCloseQuiesces)
{
	auto backend = std::make_unique<FakeTerminalBackend>();
	auto* fake = backend.get();
	fake->blockWaitForExit = true;
	fake->waitResults = { false, true };
	terminal::CTerminalSession session(std::move(backend));
	ASSERT_TRUE(session.Start(DefaultLaunchOptions()).succeeded);
	session.BeginClose();
	ASSERT_TRUE(WaitUntil([&] { return fake->WaitEntered(); }));

	fake->UnblockExitWait();
	const auto closed = session.WaitForClose(std::chrono::steady_clock::now() - 1ms);
	EXPECT_EQ(terminal::TerminalSessionCloseWaitStatus::DeadlineExceeded, closed.status);
	EXPECT_TRUE(closed.IsQuiescent());
	EXPECT_EQ(1u, fake->ForceTerminateCalls());
	EXPECT_EQ(terminal::TerminalSessionState::Exited, session.GetState());
}

TEST(TerminalSession, RepeatedConcurrentBeginCloseHasOneBackendCloseAndNoSurvivingWorkerOwnership)
{
	auto backend = std::make_unique<FakeTerminalBackend>();
	auto* fake = backend.get();
	terminal::CTerminalSession session(std::move(backend));
	ASSERT_TRUE(session.Start(DefaultLaunchOptions()).succeeded);

	std::array<std::thread, 8> callers;
	for( auto& caller : callers ) caller = std::thread([&] { session.BeginClose(); });
	for( auto& caller : callers ) caller.join();
	const auto closed = session.WaitForClose(std::chrono::steady_clock::now() + 2s);
	EXPECT_TRUE(closed.IsQuiescent());
	EXPECT_EQ(1u, fake->GracefulCloseCalls());
	EXPECT_EQ(1u, fake->CloseCalls());
	EXPECT_EQ(terminal::TerminalSessionState::Exited, session.GetState());
	// A second external waiter observes the completed ownership boundary rather
	// than joining/detaching a new worker.
	EXPECT_TRUE(session.WaitForClose(std::chrono::steady_clock::now() + 1s).IsQuiescent());
}

TEST(TerminalSession, WorkerCallbackWaitDoesNotSelfDeadlockAndRetainsExternalCloseOwnership)
{
	auto backend = std::make_unique<FakeTerminalBackend>();
	terminal::CTerminalSession* current = nullptr;
	terminal::TerminalSessionCloseResult callbackResult;
	std::atomic<bool> callbackObserved{ false };
	terminal::CTerminalSession session(std::move(backend), { {}, [&](const auto state, const auto) {
		if( state != terminal::TerminalSessionState::Closing || current == nullptr ) return;
		callbackResult = current->WaitForClose(std::chrono::steady_clock::now() + 1ms);
		callbackObserved.store(true);
	} });
	current = &session;
	ASSERT_TRUE(session.Start(DefaultLaunchOptions()).succeeded);
	session.BeginClose();
	ASSERT_TRUE(WaitUntil([&] { return callbackObserved.load(); }));
	EXPECT_EQ(terminal::TerminalSessionCloseWaitStatus::InProgress, callbackResult.status);
	const auto closed = session.WaitForClose(std::chrono::steady_clock::now() + 2s);
	EXPECT_TRUE(closed.IsQuiescent());
	EXPECT_EQ(terminal::TerminalSessionState::Exited, session.GetState());
}

TEST(TerminalSession, FastReaderCallbackPublishesItsIdentityBeforeWaitCanStartClose)
{
	auto backend = std::make_unique<FakeTerminalBackend>();
	auto* fake = backend.get();
	terminal::CTerminalSession* current = nullptr;
	terminal::TerminalSessionCloseResult callbackResult;
	std::atomic<bool> callbackObserved{ false };
	terminal::CTerminalSession session(std::move(backend), { [&] {
		callbackResult = current->WaitForClose(std::chrono::steady_clock::now() + 1ms);
		callbackObserved.store(true);
	}, {} });
	current = &session;
	ASSERT_TRUE(session.Start(DefaultLaunchOptions()).succeeded);
	fake->PushData({ 1, 2, 3 });
	ASSERT_TRUE(WaitUntil([&] { return callbackObserved.load(); }));
	EXPECT_EQ(terminal::TerminalSessionCloseWaitStatus::InProgress, callbackResult.status);
	EXPECT_TRUE(session.WaitForClose(std::chrono::steady_clock::now() + 2s).IsQuiescent());
}

TEST(TerminalSession, StartCallbackCloseWaitIsDeferredUntilLifecycleLockIsReleased)
{
	auto backend = std::make_unique<FakeTerminalBackend>();
	auto* fake = backend.get();
	terminal::CTerminalSession* current = nullptr;
	terminal::TerminalSessionCloseResult callbackResult;
	std::atomic<bool> callbackObserved{ false };
	terminal::CTerminalSession session(std::move(backend), { {}, [&](const auto state, const auto) {
		if( state != terminal::TerminalSessionState::Starting || current == nullptr ) return;
		current->BeginClose();
		callbackResult = current->WaitForClose(std::chrono::steady_clock::now() + 1ms);
		callbackObserved.store(true);
	} });
	current = &session;
	const auto start = session.Start(DefaultLaunchOptions());
	ASSERT_TRUE(callbackObserved.load());
	EXPECT_FALSE(start.succeeded);
	EXPECT_EQ(ERROR_CANCELLED, start.errorCode);
	EXPECT_EQ(0u, fake->StartCalls());
	EXPECT_EQ(terminal::TerminalSessionCloseWaitStatus::InProgress, callbackResult.status);
	EXPECT_TRUE(session.WaitForClose(std::chrono::steady_clock::now() + 2s).IsQuiescent());
}

TEST(TerminalSession, DestroyingSessionFromStartingCallbackAbortsBeforeBackendLaunch)
{
	auto lifetimeCounters = std::make_shared<FakeTerminalBackendLifetimeCounters>();
	auto backend = std::make_unique<FakeTerminalBackend>(lifetimeCounters);
	std::unique_ptr<terminal::CTerminalSession> session;
	std::atomic<bool> destroyed{ false };
	session = std::make_unique<terminal::CTerminalSession>(std::move(backend), terminal::TerminalSessionCallbacks{ {}, [&](const auto state, const auto) {
		if( state != terminal::TerminalSessionState::Starting || !session ) return;
		session.reset();
		destroyed.store(true);
	} });

	auto* const startingSession = session.get();
	const auto start = startingSession->Start(DefaultLaunchOptions());
	EXPECT_FALSE(start.succeeded);
	EXPECT_EQ(ERROR_CANCELLED, start.errorCode);
	EXPECT_TRUE(destroyed.load());
	EXPECT_FALSE(session);
	EXPECT_EQ(0u, lifetimeCounters->startCalls.load(std::memory_order_relaxed));
	ASSERT_TRUE(WaitUntil([&] { return lifetimeCounters->closeCalls.load(std::memory_order_relaxed) == 1; }));
}

TEST(TerminalSession, DestructionFromCloseCallbackRetainsImplUntilCloseWorkerCompletes)
{
	auto lifetimeCounters = std::make_shared<FakeTerminalBackendLifetimeCounters>();
	auto backend = std::make_unique<FakeTerminalBackend>(lifetimeCounters);
	std::unique_ptr<terminal::CTerminalSession> session;
	std::atomic<bool> destroyed{ false };
	session = std::make_unique<terminal::CTerminalSession>(std::move(backend), terminal::TerminalSessionCallbacks{ {}, [&](const auto state, const auto) {
		if( state != terminal::TerminalSessionState::Closing || !session ) return;
		session.reset();
		destroyed.store(true);
	} });
	ASSERT_TRUE(session->Start(DefaultLaunchOptions()).succeeded);
	session->BeginClose();
	ASSERT_TRUE(WaitUntil([&] { return destroyed.load(); }));
	ASSERT_TRUE(WaitUntil([&] { return lifetimeCounters->closeCalls.load(std::memory_order_relaxed) == 1; }));
	EXPECT_FALSE(session);
}

TEST(TerminalSession, ExternalCloseSurvivesDestructionFromClosingCallback)
{
	auto lifetimeCounters = std::make_shared<FakeTerminalBackendLifetimeCounters>();
	auto backend = std::make_unique<FakeTerminalBackend>(lifetimeCounters);
	std::unique_ptr<terminal::CTerminalSession> session;
	std::atomic<bool> destroyed{ false };
	std::atomic<bool> closeReturned{ false };
	session = std::make_unique<terminal::CTerminalSession>(std::move(backend), terminal::TerminalSessionCallbacks{ {}, [&](const auto state, const auto) {
		if( state != terminal::TerminalSessionState::Closing || !session ) return;
		session.reset();
		destroyed.store(true);
	} });
	ASSERT_TRUE(session->Start(DefaultLaunchOptions()).succeeded);

	std::thread closer([&] {
		auto* const closingSession = session.get();
		ASSERT_NE(nullptr, closingSession);
		closingSession->Close();
		closeReturned.store(true);
	});
	closer.join();
	EXPECT_TRUE(destroyed.load());
	EXPECT_TRUE(closeReturned.load());
	EXPECT_FALSE(session);
	EXPECT_EQ(1u, lifetimeCounters->closeCalls.load(std::memory_order_relaxed));
}

TEST(TerminalSession, PreservesExecutableArgumentsAndWorkingDirectoryWithSpaces)
{
	auto backend = std::make_unique<FakeTerminalBackend>();
	auto* fake = backend.get();
	terminal::CTerminalSession session(std::move(backend));
	const auto options = DefaultLaunchOptions();
	ASSERT_TRUE(session.Start(options).succeeded);
	EXPECT_EQ(options.executablePath, fake->launchOptions.executablePath);
	EXPECT_EQ(options.arguments, fake->launchOptions.arguments);
	EXPECT_EQ(options.workingDirectory, fake->launchOptions.workingDirectory);
}

TEST(TerminalSession, OutputQueueAppliesHighLowWaterBackpressureWithoutLoss)
{
	auto backend = std::make_unique<FakeTerminalBackend>();
	auto* fake = backend.get();
	terminal::CTerminalSession session(std::move(backend));
	ASSERT_TRUE(session.Start(DefaultLaunchOptions()).succeeded);
	std::vector<std::uint8_t> expected(terminal::CTerminalSession::kOutputHighWaterBytes + 256u * 1024u);
	for( std::size_t i = 0; i < expected.size(); ++i ) expected[i] = static_cast<std::uint8_t>(i % 251);
	fake->PushData(expected);
	fake->waitResults = { true };
	fake->PushEndOfFile();
	ASSERT_TRUE(WaitUntil([&] { return session.GetQueuedOutputBytes() == terminal::CTerminalSession::kOutputHighWaterBytes; }));
	EXPECT_EQ(terminal::CTerminalSession::kOutputHighWaterBytes, fake->TotalBytesRead());
	std::this_thread::sleep_for(20ms);
	EXPECT_EQ(terminal::CTerminalSession::kOutputHighWaterBytes, fake->TotalBytesRead());

	std::vector<std::uint8_t> actual;
	while( session.GetQueuedOutputBytes() > terminal::CTerminalSession::kOutputLowWaterBytes ) {
		auto drained = session.DrainOutput();
		actual.insert(actual.end(), drained.begin(), drained.end());
		EXPECT_LE(session.GetQueuedOutputBytes(), terminal::CTerminalSession::kOutputHighWaterBytes);
	}
	ASSERT_TRUE(WaitUntil([&] { return fake->TotalBytesRead() > terminal::CTerminalSession::kOutputHighWaterBytes; }));
	while( session.GetState() == terminal::TerminalSessionState::Running || session.GetQueuedOutputBytes() != 0 ) {
		auto drained = session.DrainOutput();
		actual.insert(actual.end(), drained.begin(), drained.end());
		EXPECT_LE(session.GetQueuedOutputBytes(), terminal::CTerminalSession::kOutputHighWaterBytes);
		if( drained.empty() ) std::this_thread::sleep_for(1ms);
	}
	EXPECT_EQ(expected, actual);
}

TEST(TerminalSession, MillionLineOutputStaysBoundedAndLossless)
{
	auto backend = std::make_unique<FakeTerminalBackend>();
	auto* fake = backend.get();
	terminal::CTerminalSession session(std::move(backend));
	ASSERT_TRUE(session.Start(DefaultLaunchOptions()).succeeded);

	constexpr std::size_t lineCount = 1'000'000;
	std::vector<std::uint8_t> expected;
	expected.reserve(lineCount * 3);
	for( std::size_t line = 0; line < lineCount; ++line ) {
		expected.push_back(static_cast<std::uint8_t>('x'));
		expected.push_back(static_cast<std::uint8_t>('\r'));
		expected.push_back(static_cast<std::uint8_t>('\n'));
	}
	fake->PushData(expected);
	fake->waitResults = { true };
	fake->PushEndOfFile();

	std::vector<std::uint8_t> actual;
	actual.reserve(expected.size());
	std::size_t maximumQueued = 0;
	const auto deadline = std::chrono::steady_clock::now() + 30s;
	while( std::chrono::steady_clock::now() < deadline
		&& (session.GetState() == terminal::TerminalSessionState::Running || session.GetQueuedOutputBytes() != 0) ) {
		maximumQueued = std::max(maximumQueued, session.GetQueuedOutputBytes());
		auto drained = session.DrainOutput();
		actual.insert(actual.end(), drained.begin(), drained.end());
		if( drained.empty() ) std::this_thread::sleep_for(1ms);
	}
	maximumQueued = std::max(maximumQueued, session.GetQueuedOutputBytes());
	ASSERT_LT(std::chrono::steady_clock::now(), deadline);
	EXPECT_LE(maximumQueued, terminal::CTerminalSession::kOutputHighWaterBytes);
	EXPECT_EQ(expected, actual);
	EXPECT_EQ(expected.size(), fake->TotalBytesRead());
}

TEST(TerminalSession, OutputNotificationHasOnlyOneMessageInFlight)
{
	auto backend = std::make_unique<FakeTerminalBackend>();
	auto* fake = backend.get();
	std::atomic<unsigned int> notifications{ 0 };
	terminal::CTerminalSession session( std::move(backend), { [&] { ++notifications; }, {} } );
	ASSERT_TRUE(session.Start(DefaultLaunchOptions()).succeeded);
	fake->PushData(std::vector<std::uint8_t>(terminal::CTerminalSession::kMaximumDrainBytes * 2, 42));
	ASSERT_TRUE(WaitUntil([&] { return session.GetQueuedOutputBytes() == terminal::CTerminalSession::kMaximumDrainBytes * 2; }));
	EXPECT_EQ(1u, notifications.load());
	EXPECT_EQ(terminal::CTerminalSession::kMaximumDrainBytes, session.DrainOutput().size());
	EXPECT_TRUE(session.IsOutputNotificationPending());
	EXPECT_EQ(2u, notifications.load());
}

TEST(TerminalSession, InputQueueRejectsOverflowWithoutDroppingAcceptedBytes)
{
	auto backend = std::make_unique<FakeTerminalBackend>();
	auto* fake = backend.get();
	fake->blockWrites = true;
	terminal::CTerminalSession session(std::move(backend));
	ASSERT_TRUE(session.Start(DefaultLaunchOptions()).succeeded);
	std::vector<std::uint8_t> input(terminal::CTerminalSession::kInputLimitBytes, 7);
	ASSERT_EQ(terminal::TerminalQueueInputResult::Accepted, session.QueueInput(input));
	ASSERT_TRUE(WaitUntil([&] { return fake->WriteEntered(); }));
	const std::uint8_t overflow = 8;
	EXPECT_EQ(terminal::TerminalQueueInputResult::QueueFull, session.QueueInput(std::span<const std::uint8_t>(&overflow, 1)));
	EXPECT_EQ(terminal::CTerminalSession::kInputLimitBytes, session.GetQueuedInputBytes());
	fake->UnblockWrites();
	ASSERT_TRUE(WaitUntil([&] { return session.GetQueuedInputBytes() == 0; }));
	EXPECT_EQ(input.size(), fake->WrittenByteCount());
}

TEST(TerminalSession, WriteFailureReachesFailedTerminalState)
{
	auto backend = std::make_unique<FakeTerminalBackend>();
	auto* fake = backend.get();
	fake->failWrites = true;
	terminal::CTerminalSession session(std::move(backend));
	ASSERT_TRUE(session.Start(DefaultLaunchOptions()).succeeded);
	const std::uint8_t value = 1;
	ASSERT_EQ(terminal::TerminalQueueInputResult::Accepted, session.QueueInput(std::span<const std::uint8_t>(&value, 1)));
	ASSERT_TRUE(WaitUntil([&] { return session.GetState() == terminal::TerminalSessionState::Failed; }));
	EXPECT_EQ(109u, session.GetLastError());
	EXPECT_EQ(1u, fake->ForceTerminateCalls());
	EXPECT_EQ(1u, fake->CloseCalls());
}

TEST(TerminalSession, RapidResizeRequestsCoalesceToLatestDimensions)
{
	auto backend = std::make_unique<FakeTerminalBackend>();
	auto* fake = backend.get();
	terminal::CTerminalSession session(std::move(backend));
	ASSERT_TRUE(session.Start(DefaultLaunchOptions()).succeeded);
	EXPECT_TRUE(session.RequestResize({ 80, 24 }));
	EXPECT_TRUE(session.RequestResize({ 100, 30 }));
	EXPECT_TRUE(session.RequestResize({ 132, 48 }));
	ASSERT_TRUE(WaitUntil([&] { return !fake->Resizes().empty(); }));
	std::this_thread::sleep_for(30ms);
	const auto resizes = fake->Resizes();
	ASSERT_EQ(1u, resizes.size());
	EXPECT_EQ(132u, resizes[0].columns);
	EXPECT_EQ(48u, resizes[0].rows);
}

TEST(TerminalSession, ResizeFailureReachesFailedTerminalState)
{
	auto backend = std::make_unique<FakeTerminalBackend>();
	auto* fake = backend.get();
	fake->failResize = true;
	terminal::CTerminalSession session(std::move(backend));
	ASSERT_TRUE(session.Start(DefaultLaunchOptions()).succeeded);
	ASSERT_TRUE(session.RequestResize({ 80, 24 }));
	ASSERT_TRUE(WaitUntil([&] { return session.GetState() == terminal::TerminalSessionState::Failed; }));
	EXPECT_EQ(87u, session.GetLastError());
	EXPECT_EQ(1u, fake->ForceTerminateCalls());
	ASSERT_TRUE(WaitUntil([&] { return fake->CloseCalls() == 1; }));
	EXPECT_EQ(1u, fake->CloseCalls());
}

TEST(TerminalSession, CloseBeforeStartEndsIdleSessionExplicitly)
{
	auto backend = std::make_unique<FakeTerminalBackend>();
	terminal::CTerminalSession session(std::move(backend));
	EXPECT_EQ(terminal::TerminalSessionState::Idle, session.GetState());
	session.Close();
	EXPECT_EQ(terminal::TerminalSessionState::Exited, session.GetState());
}

TEST(ConPtyTerminalBackend, LaunchesResizesExchangesDataAndClosesIdempotently)
{
	wchar_t systemDirectory[MAX_PATH]{};
	const UINT length = ::GetSystemDirectoryW(systemDirectory, static_cast<UINT>(std::size(systemDirectory)));
	ASSERT_GT(length, 0u);
	ASSERT_LT(length, std::size(systemDirectory));

	terminal::TerminalLaunchOptions options;
	options.executablePath.assign(systemDirectory, length);
	options.executablePath += L"\\cmd.exe";
	options.arguments = { L"/d", L"/q" };
	options.initialSize = { 80, 25 };

	auto backend = terminal::CreateConPtyTerminalBackend();
	ASSERT_NE(nullptr, backend);
	const auto start = backend->Start(options);
	ASSERT_TRUE(start.succeeded) << "Create ConPTY process failed with " << start.errorCode;
	const auto resize = backend->Resize({ 100, 30 });
	EXPECT_TRUE(resize.succeeded) << "ResizePseudoConsole failed with " << resize.errorCode;

	constexpr std::string_view command = "echo SAKURA_CONPTY_READY\r\nexit /b 23\r\n";
	const auto write = backend->WriteInput(std::span<const std::uint8_t>(
		reinterpret_cast<const std::uint8_t*>(command.data()), command.size()));
	ASSERT_EQ(terminal::TerminalBackendWriteStatus::Completed, write.status);
	ASSERT_EQ(command.size(), write.bytesTransferred);

	std::string output;
	std::array<std::uint8_t, 4096> buffer{};
	bool reachedEof = false;
	const auto deadline = std::chrono::steady_clock::now() + 5s;
	while (std::chrono::steady_clock::now() < deadline) {
		const auto read = backend->ReadOutput(buffer, 100ms);
		if (read.status == terminal::TerminalBackendReadStatus::Data) {
			output.append(reinterpret_cast<const char*>(buffer.data()), read.bytesTransferred);
			continue;
		}
		ASSERT_NE(terminal::TerminalBackendReadStatus::Failed, read.status)
			<< "ConPTY output read failed with " << read.errorCode;
		if (read.status == terminal::TerminalBackendReadStatus::EndOfFile) {
			reachedEof = true;
			break;
		}
	}

	EXPECT_NE(std::string::npos, output.find("SAKURA_CONPTY_READY"));
	EXPECT_TRUE(reachedEof);
	const auto exit = backend->WaitForExit(1s);
	EXPECT_EQ(terminal::TerminalBackendExitStatus::Exited, exit.status);
	EXPECT_EQ(23u, exit.exitCode);
	backend->Close();
	backend->Close();
}

TEST(ConPtyTerminalBackend, DiscoveredPowerShellExecutesTypedCommand)
{
	terminal::NativePowerShellLocatorProvider provider;
	terminal::PowerShellLocator locator(provider);
	const auto discovery = locator.Discover();
	if( !discovery.defaultCandidate.has_value() ) {
		GTEST_SKIP() << "No supported PowerShell installation was discovered.";
	}

	terminal::TerminalLaunchOptions options;
	options.executablePath = discovery.defaultCandidate->path;
	options.arguments = { L"-NoLogo", L"-NoProfile" };
	options.initialSize = { 100, 30 };

	auto backend = terminal::CreateConPtyTerminalBackend();
	ASSERT_NE(nullptr, backend);
	const auto start = backend->Start(options);
	ASSERT_TRUE(start.succeeded) << "Create ConPTY PowerShell process failed with " << start.errorCode;

	constexpr std::string_view command =
		"Write-Output 'SAKURA_POWERSHELL_COMMAND_READY'\r\nexit\r\n";
	const auto write = backend->WriteInput(std::span<const std::uint8_t>(
		reinterpret_cast<const std::uint8_t*>(command.data()), command.size()));
	ASSERT_EQ(terminal::TerminalBackendWriteStatus::Completed, write.status);
	ASSERT_EQ(command.size(), write.bytesTransferred);

	std::string output;
	std::array<std::uint8_t, 4096> buffer{};
	bool reachedEof = false;
	const auto deadline = std::chrono::steady_clock::now() + 8s;
	while( std::chrono::steady_clock::now() < deadline ) {
		const auto read = backend->ReadOutput(buffer, 100ms);
		if( read.status == terminal::TerminalBackendReadStatus::Data ) {
			output.append(reinterpret_cast<const char*>(buffer.data()), read.bytesTransferred);
			continue;
		}
		ASSERT_NE(terminal::TerminalBackendReadStatus::Failed, read.status)
			<< "ConPTY PowerShell output read failed with " << read.errorCode;
		if( read.status == terminal::TerminalBackendReadStatus::EndOfFile ) {
			reachedEof = true;
			break;
		}
	}

	EXPECT_NE(std::string::npos, output.find("SAKURA_POWERSHELL_COMMAND_READY"));
	EXPECT_TRUE(reachedEof);
	EXPECT_EQ(terminal::TerminalBackendExitStatus::Exited, backend->WaitForExit(1s).status);
	backend->Close();
}

TEST(TerminalSession, DiagnosticTraceCorrelatesPtyProtocolEvictionAndViewportWithoutRawContent)
{
	TerminalTraceTestDirectory traceDirectory;
	auto backend = std::make_unique<FakeTerminalBackend>();
	auto* fake = backend.get();
	terminal::TerminalDiagnosticOptions diagnostics;
	diagnostics.directory = traceDirectory.Path().wstring();
	std::wstring tracePath;
	{
		terminal::CTerminalSession session(std::move(backend), {}, diagnostics);
		tracePath = session.GetDiagnosticTracePath();
		ASSERT_FALSE(tracePath.empty());
		ASSERT_TRUE(session.Start(DefaultLaunchOptions()).succeeded);

		const std::string rawProbe = "terminal-private-probe-49317";
		fake->PushData(std::vector<std::uint8_t>(rawProbe.begin(), rawProbe.end()));
		ASSERT_TRUE(WaitUntil([&] { return session.GetQueuedOutputBytes() == rawProbe.size(); }));
		const auto drained = session.DrainOutput();
		ASSERT_EQ(rawProbe.size(), drained.size());

		const std::array<std::uint8_t, 4> protocolReply{ 0x1b, '[', '0', 'n' };
		ASSERT_EQ(terminal::TerminalQueueInputResult::Accepted,
			session.QueueInput(protocolReply, terminal::TerminalInputSource::Protocol));
		ASSERT_TRUE(WaitUntil([&] { return fake->WrittenByteCount() == protocolReply.size(); }));
		ASSERT_TRUE(session.RequestResize({ 132, 48 }));
		ASSERT_TRUE(WaitUntil([&] { return !fake->Resizes().empty(); }));

		session.RecordModelDiagnostic({
			.bytesDrained = drained.size(),
			.scrollbackAppended = 3,
			.scrollbackEvicted = 2,
			.scrollbackRows = 1000,
			.scrollbackLimit = 1000,
			.dirtyRows = 24,
			.columns = 132,
			.rows = 48,
		});
		session.RecordViewportDiagnostic({
			.scrollOffset = 7,
			.topRow = 1017,
			.totalRows = 1048,
			.visibleRows = 24,
		});
		session.Close();
	}

	const auto trace = ReadTextFile(tracePath);
	EXPECT_NE(std::string::npos, trace.find("\"raw_content\":false"));
	EXPECT_NE(std::string::npos, trace.find("\"kind\":\"pty_read\""));
	EXPECT_NE(std::string::npos, trace.find("\"kind\":\"pty_write\""));
	EXPECT_NE(std::string::npos, trace.find("\"source\":\"protocol\""));
	EXPECT_NE(std::string::npos, trace.find("\"kind\":\"resize_apply\""));
	EXPECT_NE(std::string::npos, trace.find("\"scrollback_evicted\":2"));
	EXPECT_NE(std::string::npos, trace.find("\"scrollback_limit\":1000"));
	EXPECT_NE(std::string::npos, trace.find("\"scroll_offset\":7"));
	EXPECT_NE(std::string::npos, trace.find("\"sha256\":\""));
	EXPECT_EQ(std::string::npos, trace.find("terminal-private-probe-49317"));
}

TEST(TerminalSession, DiagnosticTraceRotatesWithinTwoBoundedFiles)
{
	TerminalTraceTestDirectory traceDirectory;
	auto backend = std::make_unique<FakeTerminalBackend>();
	terminal::TerminalDiagnosticOptions diagnostics;
	diagnostics.directory = traceDirectory.Path().wstring();
	diagnostics.maximumFileBytes = 4096;
	diagnostics.maximumQueuedEvents = 64;
	std::filesystem::path tracePath;
	{
		terminal::CTerminalSession session(std::move(backend), {}, diagnostics);
		tracePath = session.GetDiagnosticTracePath();
		ASSERT_FALSE(tracePath.empty());
		for( std::size_t index = 0; index < 500; ++index ) {
			session.RecordModelDiagnostic({
				.bytesDrained = index,
				.scrollbackAppended = 1,
				.scrollbackEvicted = 1,
				.scrollbackRows = 1000,
				.scrollbackLimit = 1000,
			});
		}
	}

	const auto previousPath = tracePath.parent_path() /
		(tracePath.stem().wstring() + L".previous.jsonl");
	ASSERT_TRUE(std::filesystem::is_regular_file(tracePath));
	ASSERT_TRUE(std::filesystem::is_regular_file(previousPath));
	EXPECT_LE(std::filesystem::file_size(tracePath), diagnostics.maximumFileBytes);
	EXPECT_LE(std::filesystem::file_size(previousPath), diagnostics.maximumFileBytes);
	std::size_t fileCount = 0;
	for( const auto& entry : std::filesystem::directory_iterator(traceDirectory.Path()) ) {
		if( entry.is_regular_file() ) ++fileCount;
	}
	EXPECT_EQ(2u, fileCount);
}

TEST(TerminalSession, DiagnosticTraceCanBeActivatedAfterEvictionHasAlreadyStarted)
{
	TerminalTraceTestDirectory traceDirectory;
	auto backend = std::make_unique<FakeTerminalBackend>();
	std::filesystem::path tracePath;
	std::filesystem::path controlPath;
	{
		terminal::CTerminalSession session(
			std::move(backend), {}, terminal::TerminalDiagnosticOptions{});
		ASSERT_TRUE(session.Start(DefaultLaunchOptions()).succeeded);
		session.RecordModelDiagnostic({
			.scrollbackAppended = 9,
			.scrollbackEvicted = 9,
			.scrollbackRows = 1000,
			.scrollbackLimit = 1000,
		});
		EXPECT_TRUE(session.GetDiagnosticTracePath().empty());

		const auto controlDirectory = std::filesystem::temp_directory_path() / L"sakura-editor";
		std::filesystem::create_directories(controlDirectory);
		controlPath = controlDirectory /
			(L"terminal-trace-" + std::to_wstring(::GetCurrentProcessId()) + L".ini");
		ASSERT_TRUE(::WritePrivateProfileStringW(
			L"trace", L"directory", traceDirectory.Path().c_str(), controlPath.c_str()));
		const auto eventName = L"Local\\SakuraEditorNext.TerminalTrace." +
			std::to_wstring(::GetCurrentProcessId());
		HANDLE activationEvent = ::OpenEventW(EVENT_MODIFY_STATE, FALSE, eventName.c_str());
		ASSERT_NE(nullptr, activationEvent);
		ASSERT_TRUE(::SetEvent(activationEvent));
		::CloseHandle(activationEvent);

		std::this_thread::sleep_for(1100ms);
		session.RecordModelDiagnostic({
			.scrollbackAppended = 1,
			.scrollbackEvicted = 1,
			.scrollbackRows = 1000,
			.scrollbackLimit = 1000,
		});
		tracePath = session.GetDiagnosticTracePath();
		ASSERT_FALSE(tracePath.empty());
		EXPECT_EQ(traceDirectory.Path(), tracePath.parent_path());
		session.Close();
	}
	std::error_code cleanupError;
	std::filesystem::remove(controlPath, cleanupError);

	const auto trace = ReadTextFile(tracePath);
	EXPECT_NE(std::string::npos, trace.find("\"total_scrollback_evicted\":10"));
	EXPECT_EQ(std::string::npos, trace.find("\"first_eviction_us\":0"));
	EXPECT_NE(std::string::npos, trace.find("\"scrollback_limit\":1000"));
}

TEST(ConPtyTerminalBackend, DescribesInteractiveColorCapabilitiesToChildShell)
{
	terminal::NativePowerShellLocatorProvider provider;
	terminal::PowerShellLocator locator(provider);
	const auto discovery = locator.Discover();
	if( !discovery.defaultCandidate.has_value() ) {
		GTEST_SKIP() << "No supported PowerShell installation was discovered.";
	}

	terminal::TerminalLaunchOptions options;
	options.executablePath = discovery.defaultCandidate->path;
	options.arguments = { L"-NoLogo", L"-NoProfile" };
	options.initialSize = { 100, 30 };

	auto backend = terminal::CreateConPtyTerminalBackend();
	ASSERT_NE(nullptr, backend);
	const auto start = backend->Start(options);
	ASSERT_TRUE(start.succeeded) << "Create ConPTY PowerShell process failed with " << start.errorCode;

	constexpr std::string_view command =
		"Write-Output ('NO_COLOR=' + [string]$env:NO_COLOR); "
		"Write-Output ('TERM=' + [string]$env:TERM); "
		"Write-Output ('COLORTERM=' + [string]$env:COLORTERM); exit\r\n";
	const auto write = backend->WriteInput(std::span<const std::uint8_t>(
		reinterpret_cast<const std::uint8_t*>(command.data()), command.size()));
	ASSERT_EQ(terminal::TerminalBackendWriteStatus::Completed, write.status);

	std::string output;
	std::array<std::uint8_t, 4096> buffer{};
	bool reachedEof = false;
	const auto deadline = std::chrono::steady_clock::now() + 8s;
	while( std::chrono::steady_clock::now() < deadline ) {
		const auto read = backend->ReadOutput(buffer, 100ms);
		if( read.status == terminal::TerminalBackendReadStatus::Data ) {
			output.append(reinterpret_cast<const char*>(buffer.data()), read.bytesTransferred);
			continue;
		}
		ASSERT_NE(terminal::TerminalBackendReadStatus::Failed, read.status)
			<< "ConPTY PowerShell environment read failed with " << read.errorCode;
		if( read.status == terminal::TerminalBackendReadStatus::EndOfFile ) {
			reachedEof = true;
			break;
		}
	}

	EXPECT_NE(std::string::npos, output.find("NO_COLOR=\r\n"));
	EXPECT_NE(std::string::npos, output.find("TERM=xterm-256color\r\n"));
	EXPECT_NE(std::string::npos, output.find("COLORTERM=truecolor\r\n"));
	EXPECT_TRUE(reachedEof);
	EXPECT_EQ(terminal::TerminalBackendExitStatus::Exited, backend->WaitForExit(1s).status);
	backend->Close();
}

} // namespace

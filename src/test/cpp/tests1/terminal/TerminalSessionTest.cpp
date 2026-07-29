/*! @file */
#include "pch.h"
#include "terminal/session/TerminalSession.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
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

class FakeTerminalBackend final : public terminal::ITerminalBackend {
public:
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
	std::deque<bool> waitResults;

	terminal::TerminalStartResult Start( const terminal::TerminalLaunchOptions& options ) override
	{
		const std::lock_guard lock(m_mutex);
		launchOptions = options;
		++startCalls;
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
			if( event.status == terminal::TerminalBackendReadStatus::EndOfFile ) processExited = true;
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

	bool WaitForExit( std::chrono::milliseconds timeout ) noexcept override
	{
		const std::lock_guard lock(m_mutex);
		waitTimeouts.push_back(timeout);
		if( waitResults.empty() ) return processExited;
		const bool result = waitResults.front();
		waitResults.pop_front();
		if( result ) processExited = true;
		return result;
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

	std::size_t TotalBytesRead() const { const std::lock_guard lock(m_mutex); return totalBytesRead; }
	bool WriteEntered() const { const std::lock_guard lock(m_mutex); return writeEntered; }
	std::size_t WrittenByteCount() const { const std::lock_guard lock(m_mutex); return writtenBytes.size(); }
	std::vector<terminal::TerminalSize> Resizes() const { const std::lock_guard lock(m_mutex); return resizes; }
	std::size_t CloseCalls() const { const std::lock_guard lock(m_mutex); return closeCalls; }
	std::size_t GracefulCloseCalls() const { const std::lock_guard lock(m_mutex); return gracefulCloseCalls; }
	std::size_t ForceTerminateCalls() const { const std::lock_guard lock(m_mutex); return forceTerminateCalls; }
	std::vector<std::chrono::milliseconds> WaitTimeouts() const { const std::lock_guard lock(m_mutex); return waitTimeouts; }

private:
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
	bool processExited = false;
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

	constexpr std::string_view command = "echo SAKURA_CONPTY_READY\r\nexit\r\n";
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
	EXPECT_TRUE(backend->WaitForExit(1s));
	backend->Close();
	backend->Close();
}

} // namespace

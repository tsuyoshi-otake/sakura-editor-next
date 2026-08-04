/*! @file */
#include "pch.h"
#include "terminal/window/TerminalHeaderLayout.h"
#include "terminal/window/CTerminalTool.h"
#include "terminal/window/CTerminalWnd.h"
#include "workbench/extension/CExtensionBottomPanelTool.h"
#include "terminal/model/TerminalModel.h"
#include "terminal/input/SakuraTerminalInputAdapter.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>

namespace {

using namespace std::chrono_literals;

struct BackendState {
	std::atomic<int> startCalls{};
	std::atomic<int> gracefulCloseCalls{};
	std::atomic<int> forceTerminateCalls{};
	std::atomic<int> closeCalls{};
	std::atomic<bool> closed{};
	std::atomic<bool> blockWrites{};
	std::atomic<bool> failWrites{};
	std::atomic<bool> writeEntered{};
	std::atomic<std::size_t> outputOffset{};
	std::string scriptedOutput;
	std::wstring workingDirectory;
	terminal::TerminalSize initialSize{};
	bool failStart{};
};

class ToolFakeBackend final : public terminal::ITerminalBackend {
public:
	explicit ToolFakeBackend( std::shared_ptr<BackendState> state )
		: m_state(std::move(state))
	{
	}

	terminal::TerminalStartResult Start( const terminal::TerminalLaunchOptions& options ) override
	{
		++m_state->startCalls;
		m_state->workingDirectory = options.workingDirectory;
		m_state->initialSize = options.initialSize;
		if( m_state->failStart ) return terminal::TerminalStartResult::Failure(ERROR_ACCESS_DENIED, L"denied");
		return terminal::TerminalStartResult::Success();
	}

	terminal::TerminalBackendReadResult ReadOutput( std::span<std::uint8_t> destination, std::chrono::milliseconds timeout ) override
	{
		const auto offset = m_state->outputOffset.load();
		if( offset < m_state->scriptedOutput.size() ) {
			const auto count = std::min(destination.size(), m_state->scriptedOutput.size() - offset);
			std::memcpy(destination.data(), m_state->scriptedOutput.data() + offset, count);
			m_state->outputOffset.store(offset + count);
			return { terminal::TerminalBackendReadStatus::Data, count, 0 };
		}
		std::this_thread::sleep_for(std::min(timeout, 2ms));
		return m_state->closed ? terminal::TerminalBackendReadResult{ terminal::TerminalBackendReadStatus::EndOfFile, 0, 0 }
			: terminal::TerminalBackendReadResult{};
	}

	terminal::TerminalBackendWriteResult WriteInput( std::span<const std::uint8_t> source ) override
	{
		m_state->writeEntered.store(true);
		while( m_state->blockWrites.load() && !m_state->closed.load() ) std::this_thread::sleep_for(1ms);
		if( m_state->failWrites.load() ) return { terminal::TerminalBackendWriteStatus::Failed, 0, ERROR_BROKEN_PIPE };
		return { terminal::TerminalBackendWriteStatus::Completed, source.size(), 0 };
	}

	terminal::TerminalBackendOperationResult Resize( terminal::TerminalSize ) override { return { true, 0 }; }
	void RequestGracefulClose() noexcept override { ++m_state->gracefulCloseCalls; }
	terminal::TerminalBackendExitResult WaitForExit( std::chrono::milliseconds ) noexcept override { return { terminal::TerminalBackendExitStatus::Exited, 0, 0 }; }
	void ForceTerminate() noexcept override { ++m_state->forceTerminateCalls; }
	void Close() noexcept override
	{
		if( !m_state->closed.exchange(true) ) ++m_state->closeCalls;
	}

private:
	std::shared_ptr<BackendState> m_state;
};

struct ToolHarness {
	std::mutex mutex;
	std::vector<std::shared_ptr<BackendState>> backends;
	std::vector<std::wstring> resolvedWorkingDirectories;
	std::string scriptedOutput;
	bool failStart{};
	bool blockWrites{};

	terminal::TerminalTabManagerDependencies Dependencies()
	{
		terminal::TerminalTabManagerDependencies dependencies;
		dependencies.createSession = [this](terminal::TerminalSessionCallbacks callbacks) {
			auto state = std::make_shared<BackendState>();
			state->scriptedOutput = scriptedOutput;
			state->failStart = failStart;
			state->blockWrites.store(blockWrites);
			{
				const std::lock_guard lock(mutex);
				backends.push_back(state);
			}
			return std::make_unique<terminal::CTerminalSession>(std::make_unique<ToolFakeBackend>(state), std::move(callbacks));
		};
		dependencies.resolveLaunch = [this](terminal::TerminalSize size, std::wstring_view workingDirectory) {
			resolvedWorkingDirectories.emplace_back(workingDirectory);
			terminal::TerminalLaunchOptions options;
			options.executablePath = L"C:\\Program Files\\PowerShell\\7\\pwsh.exe";
			options.arguments = { L"-NoLogo" };
			options.workingDirectory.assign(workingDirectory);
			options.initialSize = size;
			return std::optional<terminal::TerminalLaunchOptions>(std::move(options));
		};
		return dependencies;
	}
};

HWND CreateHiddenParentWindow()
{
	return ::CreateWindowExW(0, L"STATIC", L"Terminal tool test parent", WS_OVERLAPPED,
		CW_USEDEFAULT, CW_USEDEFAULT, 800, 600, nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
}

class ScopedNeutralKeyboardState final {
public:
	ScopedNeutralKeyboardState()
	{
		m_restore = ::GetKeyboardState(m_saved.data()) != FALSE;
		std::array<BYTE, 256> neutral{};
		m_applied = ::SetKeyboardState(neutral.data()) != FALSE;
	}

	~ScopedNeutralKeyboardState()
	{
		if( m_restore ) static_cast<void>(::SetKeyboardState(m_saved.data()));
	}

	[[nodiscard]] bool Applied() const noexcept { return m_applied; }

private:
	std::array<BYTE, 256> m_saved{};
	bool m_restore{};
	bool m_applied{};
};

template<typename Predicate>
bool WaitUntil( Predicate&& predicate, std::chrono::milliseconds timeout = 500ms )
{
	const auto deadline = std::chrono::steady_clock::now() + timeout;
	while( std::chrono::steady_clock::now() < deadline ) {
		if( predicate() ) return true;
		std::this_thread::sleep_for(1ms);
	}
	return predicate();
}

TEST(TerminalTool, DefersFirstSessionUntilActivationAndKeepsItWhileDeactivated)
{
	ToolHarness harness;
	terminal::CTerminalTool tool(harness.Dependencies());
	EXPECT_EQ(0u, tool.TabCount());
	EXPECT_FALSE(tool.HasStartedAnySession());
	EXPECT_FALSE(tool.HasCreatedRenderer());
	tool.Deactivate();
	EXPECT_TRUE(harness.backends.empty());
	EXPECT_FALSE(tool.HasCreatedRenderer());

	tool.SetWorkingDirectory(L"C:\\work folder");
	tool.Activate();
	ASSERT_EQ(1u, tool.TabCount());
	ASSERT_EQ(1u, harness.backends.size());
	EXPECT_TRUE(tool.HasStartedAnySession());
	// Unit tests intentionally activate without an HWND.  Session startup must
	// still work, while renderer creation remains deferred until a visible host
	// has supplied a layout rectangle.
	EXPECT_FALSE(tool.HasCreatedRenderer());
	EXPECT_EQ(L"C:\\work folder", harness.backends[0]->workingDirectory);

	tool.Deactivate();
	EXPECT_EQ(0, harness.backends[0]->closeCalls.load());
	EXPECT_EQ(terminal::TerminalSessionState::Running, tool.Tabs()[0].state);
	tool.Close();
	EXPECT_EQ(1, harness.backends[0]->closeCalls.load());
}

TEST(TerminalTool, DefersRendererUntilItsFirstNonEmptyLayout)
{
	ToolHarness harness;
	const HWND parent = CreateHiddenParentWindow();
	ASSERT_NE(nullptr, parent);

	terminal::CTerminalTool tool(harness.Dependencies());
	ASSERT_TRUE(tool.Create(parent));
	EXPECT_FALSE(tool.HasCreatedRenderer());

	const RECT collapsed{};
	tool.Layout(collapsed, 96);
	EXPECT_FALSE(tool.HasCreatedRenderer());

	const RECT visible{ 0, 0, 480, 240 };
	tool.Layout(visible, 96);
	EXPECT_TRUE(tool.HasCreatedRenderer());

	tool.Close();
	::DestroyWindow(parent);
}

TEST(TerminalTool, RendererSizeInvalidatesBeforeItReceivesFocus)
{
	const HWND parent = CreateHiddenParentWindow();
	ASSERT_NE(nullptr, parent);
	terminal::CTerminalWnd renderer;
	ASSERT_TRUE(renderer.Create(parent, ::GetModuleHandleW(nullptr)));
	terminal::TerminalModel model(80, 24);
	renderer.SetModel(&model);

	const RECT visible{ 0, 0, 480, 240 };
	::SetWindowPos(parent, nullptr, -32000, -32000, 800, 600, SWP_NOACTIVATE | SWP_NOZORDER);
	::ShowWindow(parent, SW_SHOWNOACTIVATE);
	renderer.Layout(visible, 96);
	ASSERT_TRUE(::ValidateRect(renderer.GetHwnd(), nullptr));
	::SendMessageW(renderer.GetHwnd(), WM_SIZE, SIZE_RESTORED, MAKELPARAM(480, 240));
	RECT update{};
	EXPECT_TRUE(::GetUpdateRect(renderer.GetHwnd(), &update, FALSE));
	EXPECT_NE(renderer.GetHwnd(), ::GetFocus());

	renderer.Close();
	::DestroyWindow(parent);
}

TEST(TerminalTool, PrintableKeyDownFallsThroughToCharMessage)
{
	// This test directly constructs a MSG instead of retrieving it from the
	// thread queue.  Arrange the modifier state explicitly so a physically held
	// Ctrl/Shift key cannot turn the synthetic printable key into a VT command.
	ScopedNeutralKeyboardState keyboardState;
	ASSERT_TRUE(keyboardState.Applied());
	const HWND parent = CreateHiddenParentWindow();
	ASSERT_NE(nullptr, parent);
	terminal::CTerminalWnd renderer;
	ASSERT_TRUE(renderer.Create(parent, ::GetModuleHandleW(nullptr)));
	terminal::SakuraTerminalInputAdapter inputAdapter;
	renderer.SetInputAdapter(&inputAdapter);
	std::string received;
	renderer.SetInputSink([&received](std::span<const std::uint8_t> bytes) {
		received.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
		return terminal::TerminalQueueInputResult::Accepted;
	});

	MSG keyDown{ renderer.GetHwnd(), WM_KEYDOWN, static_cast<WPARAM>('A'), 1 };
	EXPECT_FALSE(renderer.PreTranslateMessage(keyDown));
	EXPECT_TRUE(received.empty());
	::SendMessageW(renderer.GetHwnd(), WM_CHAR, L'a', 1);
	EXPECT_EQ("a", received);

	renderer.Close();
	::DestroyWindow(parent);
}

TEST(TerminalTool, QueueFullInteractiveInputIsRetriedInsteadOfSilentlyDiscarded)
{
	const HWND parent = CreateHiddenParentWindow();
	ASSERT_NE(nullptr, parent);
	terminal::CTerminalWnd renderer;
	ASSERT_TRUE(renderer.Create(parent, ::GetModuleHandleW(nullptr)));
	terminal::SakuraTerminalInputAdapter inputAdapter;
	renderer.SetInputAdapter(&inputAdapter);
	std::string received;
	std::atomic<int> calls{};
	renderer.SetInputSink([&](std::span<const std::uint8_t> bytes) {
		if( calls.fetch_add(1) == 0 ) return terminal::TerminalQueueInputResult::QueueFull;
		received.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
		return terminal::TerminalQueueInputResult::Accepted;
	});

	::SendMessageW(renderer.GetHwnd(), WM_CHAR, L'x', 1);
	EXPECT_TRUE(received.empty());
	::SendMessageW(renderer.GetHwnd(), WM_TIMER, 0x5345, 0);
	EXPECT_EQ("x", received);
	EXPECT_GE(calls.load(), 2);

	renderer.Close();
	::DestroyWindow(parent);
}

TEST(TerminalTool, BottomPanelLayoutNeverInvertsContentWhileShrinking)
{
	using workbench::extension::CalculateExtensionBottomPanelVerticalLayout;

	const auto collapsed = CalculateExtensionBottomPanelVerticalLayout(12, 34, 28);
	EXPECT_EQ(12, collapsed.headerHeight);
	EXPECT_EQ(12, collapsed.contentTop);
	EXPECT_EQ(0, collapsed.contentHeight);
	EXPECT_EQ(0, collapsed.outputSelectorHeight);

	const auto visible = CalculateExtensionBottomPanelVerticalLayout(200, 34, 28);
	EXPECT_EQ(34, visible.headerHeight);
	EXPECT_EQ(34, visible.contentTop);
	EXPECT_EQ(166, visible.contentHeight);
	EXPECT_EQ(28, visible.outputSelectorHeight);
}

TEST(TerminalTool, RendererInvalidatesWhenRestoredFromZeroHeight)
{
	const HWND parent = CreateHiddenParentWindow();
	ASSERT_NE(nullptr, parent);
	terminal::CTerminalWnd renderer;
	ASSERT_TRUE(renderer.Create(parent, ::GetModuleHandleW(nullptr)));
	terminal::TerminalModel model(80, 24);
	renderer.SetModel(&model);
	::SetWindowPos(parent, nullptr, -32000, -32000, 800, 600, SWP_NOACTIVATE | SWP_NOZORDER);
	::ShowWindow(parent, SW_SHOWNOACTIVATE);

	renderer.Layout(RECT{ 0, 0, 480, 0 }, 96);
	renderer.Layout(RECT{ 0, 0, 480, 240 }, 96);
	RECT client{};
	ASSERT_TRUE(::GetClientRect(renderer.GetHwnd(), &client));
	EXPECT_EQ(480, client.right - client.left);
	EXPECT_EQ(240, client.bottom - client.top);
	RECT update{};
	EXPECT_TRUE(::GetUpdateRect(renderer.GetHwnd(), &update, FALSE));

	renderer.Close();
	::DestroyWindow(parent);
}

TEST(TerminalTool, SessionInputResetDropsBackpressuredBytesBeforeRebinding)
{
	const HWND parent = CreateHiddenParentWindow();
	ASSERT_NE(nullptr, parent);
	terminal::CTerminalWnd renderer;
	ASSERT_TRUE(renderer.Create(parent, ::GetModuleHandleW(nullptr)));
	std::string received;
	renderer.SetInputSink([](std::span<const std::uint8_t>) {
		return terminal::TerminalQueueInputResult::QueueFull;
	});
	::SendMessageW(renderer.GetHwnd(), WM_CHAR, L'旧', 1);

	renderer.ResetSessionInputState();
	renderer.SetInputSink([&received](std::span<const std::uint8_t> bytes) {
		received.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
		return terminal::TerminalQueueInputResult::Accepted;
	});
	::SendMessageW(renderer.GetHwnd(), WM_TIMER, 0x5345, 0);
	EXPECT_TRUE(received.empty());

	renderer.Close();
	::DestroyWindow(parent);
}

TEST(TerminalTool, ImeCharacterFallbackEncodesCommittedJapaneseAsUtf8Once)
{
	const HWND parent = CreateHiddenParentWindow();
	ASSERT_NE(nullptr, parent);
	terminal::CTerminalWnd renderer;
	ASSERT_TRUE(renderer.Create(parent, ::GetModuleHandleW(nullptr)));
	std::string received;
	renderer.SetInputSink([&received](std::span<const std::uint8_t> bytes) {
		received.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
		return terminal::TerminalQueueInputResult::Accepted;
	});

	::SendMessageW(renderer.GetHwnd(), WM_IME_CHAR, L'日', 1);
	::SendMessageW(renderer.GetHwnd(), WM_IME_CHAR, L'本', 1);
	EXPECT_EQ(std::string("\xe6\x97\xa5\xe6\x9c\xac", 6), received);

	renderer.Close();
	::DestroyWindow(parent);
}

TEST(TerminalTool, ImeCompositionCommitsTheCompleteJapaneseResultExactlyOnce)
{
	const HWND parent = CreateHiddenParentWindow();
	ASSERT_NE(nullptr, parent);
	int reads = 0;
	terminal::CTerminalWnd renderer([&reads](HWND, std::wstring& result) {
		++reads;
		result = L"日本語";
		return true;
	});
	ASSERT_TRUE(renderer.Create(parent, ::GetModuleHandleW(nullptr)));
	std::string received;
	renderer.SetInputSink([&received](std::span<const std::uint8_t> bytes) {
		received.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
		return terminal::TerminalQueueInputResult::Accepted;
	});

	::SendMessageW(renderer.GetHwnd(), WM_IME_COMPOSITION, 0, GCS_RESULTSTR);
	EXPECT_EQ(1, reads);
	EXPECT_EQ(std::string("\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e", 9), received);

	renderer.Close();
	::DestroyWindow(parent);
}

TEST(TerminalTool, VisibleLayoutBeforeActivationUsesViewportSizeForFirstSession)
{
	ToolHarness harness;
	const HWND parent = CreateHiddenParentWindow();
	ASSERT_NE(nullptr, parent);

	terminal::CTerminalTool tool(harness.Dependencies());
	ASSERT_TRUE(tool.Create(parent));
	const RECT visible{ 0, 0, 480, 240 };
	tool.Layout(visible, 96);
	ASSERT_TRUE(tool.HasCreatedRenderer());

	tool.Activate();
	ASSERT_EQ(1u, harness.backends.size());
	EXPECT_GT(harness.backends[0]->initialSize.columns, 1);
	EXPECT_GT(harness.backends[0]->initialSize.rows, 1);

	tool.Close();
	::DestroyWindow(parent);
}

TEST(TerminalTool, RestoredVisiblePanelStartsExactlyOneSessionWithoutTakingFocus)
{
	ToolHarness harness;
	const HWND parent = CreateHiddenParentWindow();
	ASSERT_NE(nullptr, parent);
	const HWND focusOwner = ::CreateWindowExW(0, L"STATIC", L"focus owner", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
		0, 0, 40, 20, parent, nullptr, ::GetModuleHandleW(nullptr), nullptr);
	ASSERT_NE(nullptr, focusOwner);
	::ShowWindow(parent, SW_SHOWNOACTIVATE);
	::SetFocus(focusOwner);
	const HWND focusBefore = ::GetFocus();

	terminal::CTerminalTool tool(harness.Dependencies());
	ASSERT_TRUE(tool.Create(parent));
	tool.Layout({ 0, 30, 480, 240 }, 96);
	ASSERT_TRUE(tool.EnsureSessionStarted());
	EXPECT_EQ(1u, tool.TabCount());
	EXPECT_EQ(1u, harness.backends.size());
	EXPECT_EQ(focusBefore, ::GetFocus());
	EXPECT_TRUE(tool.EnsureSessionStarted());
	EXPECT_EQ(1u, tool.TabCount());
	EXPECT_EQ(1u, harness.backends.size());
	EXPECT_EQ(focusBefore, ::GetFocus());

	tool.Close();
	::DestroyWindow(parent);
}

TEST(TerminalTool, FirstOutputDrainDoesNotWaitForFrameTimer)
{
	ToolHarness harness;
	harness.scriptedOutput = "\x1b]0;Immediate response\x07";
	const HWND parent = CreateHiddenParentWindow();
	ASSERT_NE(nullptr, parent);

	terminal::CTerminalTool tool(harness.Dependencies());
	ASSERT_TRUE(tool.Create(parent));
	tool.Layout({ 0, 0, 480, 240 }, 96);
	tool.Activate();
	ASSERT_EQ(1u, harness.backends.size());
	ASSERT_TRUE(WaitUntil([&] {
		return harness.backends[0]->outputOffset.load() == harness.scriptedOutput.size();
	}));

	// Dispatch only application messages. WM_TIMER is deliberately excluded so
	// this fails if the leading output drain is deferred to the 16 ms frame timer.
	MSG message{};
	while( ::PeekMessageW(&message, tool.GetHwnd(), WM_APP, 0xBFFF, PM_REMOVE) ) {
		::TranslateMessage(&message);
		::DispatchMessageW(&message);
	}
	ASSERT_EQ(1u, tool.Tabs().size());
	EXPECT_EQ(L"Immediate response", tool.Tabs().front().label);

	tool.Close();
	::DestroyWindow(parent);
}

TEST(TerminalTool, FirstSessionFailureIsReportedWithoutRemovingTheFailedTab)
{
	ToolHarness harness;
	harness.failStart = true;
	terminal::CTerminalTool tool(harness.Dependencies());

	EXPECT_FALSE(tool.EnsureSessionStarted());
	ASSERT_EQ(1u, tool.TabCount());
	ASSERT_EQ(1u, tool.Tabs().size());
	EXPECT_EQ(terminal::TerminalSessionState::Failed, tool.Tabs().front().state);
	EXPECT_EQ(ERROR_ACCESS_DENIED, tool.Tabs().front().errorCode);
	tool.Close();
}

TEST(TerminalTool, DrainReportsCompletedSynchronizedFrameEvenWhenNextFrameIsOpen)
{
	ToolHarness harness;
	harness.scriptedOutput = "\x1b[?2026hClaude ready\x1b[?2026l\x1b[?2026hnext";
	std::atomic<int> outputNotifications{};
	terminal::TerminalTabManager manager(harness.Dependencies(), [&outputNotifications](const terminal::TerminalTabEvent& event) {
		if( event.kind == terminal::TerminalTabEventKind::OutputAvailable ) ++outputNotifications;
	});
	const auto id = manager.Activate({ 80, 24 }, L"C:\\workspace");
	ASSERT_TRUE(id.has_value());
	for( int attempt = 0; attempt < 100 && outputNotifications.load() == 0; ++attempt ) std::this_thread::sleep_for(2ms);
	ASSERT_GT(outputNotifications.load(), 0);

	const auto drained = manager.DrainOutput(*id);
	EXPECT_TRUE(drained.found);
	EXPECT_TRUE(drained.active);
	EXPECT_TRUE(drained.synchronizedOutputCommitted);
	EXPECT_EQ(harness.scriptedOutput.size(), drained.bytesDrained);
	ASSERT_NE(nullptr, manager.Model(*id));
	EXPECT_TRUE(manager.Model(*id)->Modes().synchronizedOutput);
	manager.Close();
}

TEST(TerminalTool, OscTitleDoesNotReplaceStableHeaderProfileName)
{
	ToolHarness harness;
	harness.scriptedOutput = "\x1b]0;Claude Code\x07";
	std::atomic<int> outputNotifications{};
	terminal::TerminalTabManager manager(harness.Dependencies(), [&outputNotifications](const terminal::TerminalTabEvent& event) {
		if( event.kind == terminal::TerminalTabEventKind::OutputAvailable ) ++outputNotifications;
	});
	const auto id = manager.Activate({ 80, 24 }, L"C:\\workspace");
	ASSERT_TRUE(id.has_value());
	ASSERT_TRUE(WaitUntil([&] { return outputNotifications.load() > 0; }));

	const auto drained = manager.DrainOutput(*id);
	EXPECT_TRUE(drained.titleChanged);
	const auto tabs = manager.Snapshot();
	ASSERT_EQ(1u, tabs.size());
	EXPECT_EQ(L"Claude Code", tabs.front().label);
	EXPECT_EQ(L"pwsh", tabs.front().profileLabel);
	manager.Close();
}

TEST(TerminalTool, DeferredProtocolResponseFinalizesWhenTheSessionStops)
{
	ToolHarness harness;
	harness.scriptedOutput = "\x1b[6n";
	harness.blockWrites = true;
	terminal::TerminalTabManager manager(harness.Dependencies());
	const auto id = manager.Activate({ 80, 24 }, L"C:\\workspace");
	ASSERT_TRUE(id.has_value());
	ASSERT_EQ(1u, harness.backends.size());
	std::vector<std::uint8_t> fill(terminal::CTerminalSession::kInputLimitBytes, 0x41);
	ASSERT_EQ(terminal::TerminalQueueInputResult::Accepted, manager.QueueInput(*id, fill));
	ASSERT_TRUE(WaitUntil([&] { return harness.backends[0]->writeEntered.load(); }));

	const auto drained = manager.DrainOutput(*id);
	EXPECT_TRUE(drained.protocolInputPending);
	EXPECT_TRUE(manager.HasPendingProtocolInput(*id));
	harness.backends[0]->failWrites.store(true);
	harness.backends[0]->blockWrites.store(false);
	ASSERT_TRUE(WaitUntil([&] { return manager.Snapshot().front().state == terminal::TerminalSessionState::Failed; }));
	EXPECT_EQ(terminal::TerminalQueueInputResult::NotRunning, manager.FlushPendingProtocolInput(*id));
	EXPECT_FALSE(manager.HasPendingProtocolInput(*id));
	manager.Close();
}

TEST(TerminalTool, SupportsAddSelectRestartAndDeleteAcrossTabs)
{
	ToolHarness harness;
	terminal::CTerminalTool tool(harness.Dependencies());
	tool.Activate();
	const auto first = tool.ActiveTerminalId();
	ASSERT_TRUE(first.has_value());
	const auto second = tool.AddTerminal();
	ASSERT_TRUE(second.has_value());
	EXPECT_EQ(2u, tool.TabCount());
	EXPECT_EQ(second, tool.ActiveTerminalId());
	EXPECT_TRUE(tool.SelectTerminal(*first));
	EXPECT_EQ(first, tool.ActiveTerminalId());
	EXPECT_TRUE(tool.RestartTerminal(*first));
	EXPECT_EQ(3u, harness.backends.size());
	EXPECT_EQ(1, harness.backends[0]->closeCalls.load());
	EXPECT_TRUE(tool.DeleteTerminal(*second));
	EXPECT_EQ(1u, tool.TabCount());
	EXPECT_FALSE(tool.DeleteTerminal(*second));
	tool.Close();
}

TEST(TerminalTool, SplitsIntoIndependentLeftAndRightSessionsAndClosesRightCleanly)
{
	ToolHarness harness;
	terminal::CTerminalTool tool(harness.Dependencies());
	tool.SetWorkingDirectory(L"C:\\split workspace");
	tool.Activate();
	const auto left = tool.ActiveTerminalId();
	ASSERT_TRUE(left.has_value());

	ASSERT_TRUE(tool.SplitTerminalRight());
	EXPECT_TRUE(tool.HasTerminalSplit());
	EXPECT_EQ(2u, tool.TabCount());
	EXPECT_EQ(left, tool.ActiveTerminalId());
	ASSERT_EQ(2u, harness.backends.size());
	EXPECT_EQ(L"C:\\split workspace", harness.backends[1]->workingDirectory);
	EXPECT_FALSE(tool.SplitTerminalRight());

	EXPECT_TRUE(tool.CloseTerminalSplit());
	EXPECT_FALSE(tool.HasTerminalSplit());
	EXPECT_EQ(1u, tool.TabCount());
	EXPECT_EQ(1, harness.backends[1]->closeCalls.load());
	EXPECT_FALSE(tool.CloseTerminalSplit());
	tool.Close();
	EXPECT_EQ(1, harness.backends[0]->closeCalls.load());
}

TEST(TerminalTool, SplitCreatesTwoNativeViewportsAndDividerResizesThem)
{
	ToolHarness harness;
	const HWND parent = CreateHiddenParentWindow();
	ASSERT_NE(nullptr, parent);
	terminal::CTerminalTool tool(harness.Dependencies());
	ASSERT_TRUE(tool.Create(parent));
	const RECT visible{ 0, 0, 640, 320 };
	tool.Layout(visible, 96);
	tool.Activate();
	ASSERT_TRUE(tool.SplitTerminalRight());

	const HWND first = ::FindWindowExW(tool.GetHwnd(), nullptr, L"SakuraNativeTerminalWindow", nullptr);
	ASSERT_NE(nullptr, first);
	const HWND second = ::FindWindowExW(tool.GetHwnd(), first, L"SakuraNativeTerminalWindow", nullptr);
	ASSERT_NE(nullptr, second);
	EXPECT_EQ(nullptr, ::FindWindowExW(tool.GetHwnd(), second, L"SakuraNativeTerminalWindow", nullptr));
	RECT firstBefore{};
	RECT secondBefore{};
	ASSERT_TRUE(::GetWindowRect(first, &firstBefore));
	ASSERT_TRUE(::GetWindowRect(second, &secondBefore));
	EXPECT_LT(firstBefore.left, secondBefore.left);
	EXPECT_LE(firstBefore.right, secondBefore.left);
	EXPECT_GT(firstBefore.right - firstBefore.left, 200);
	EXPECT_GT(secondBefore.right - secondBefore.left, 200);

	POINT divider{ firstBefore.right, firstBefore.top + 20 };
	::ScreenToClient(tool.GetHwnd(), &divider);
	::SendMessageW(tool.GetHwnd(), WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(divider.x + 1, divider.y));
	::SendMessageW(tool.GetHwnd(), WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(divider.x + 80, divider.y));
	::SendMessageW(tool.GetHwnd(), WM_LBUTTONUP, 0, MAKELPARAM(divider.x + 80, divider.y));
	RECT firstAfter{};
	ASSERT_TRUE(::GetWindowRect(first, &firstAfter));
	EXPECT_GT(firstAfter.right - firstAfter.left, firstBefore.right - firstBefore.left);

	// A ratio selected on a wide panel must be clamped again when the frame is
	// narrowed; otherwise the right ConPTY can collapse to only a few columns.
	const RECT wide{ 0, 0, 1200, 320 };
	tool.Layout(wide, 96);
	ASSERT_TRUE(::GetWindowRect(first, &firstAfter));
	divider = { firstAfter.right, firstAfter.top + 20 };
	::ScreenToClient(tool.GetHwnd(), &divider);
	::SendMessageW(tool.GetHwnd(), WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(divider.x + 1, divider.y));
	::SendMessageW(tool.GetHwnd(), WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(1110, divider.y));
	::SendMessageW(tool.GetHwnd(), WM_LBUTTONUP, 0, MAKELPARAM(1110, divider.y));
	tool.Layout(visible, 96);
	RECT firstNarrow{};
	RECT secondNarrow{};
	ASSERT_TRUE(::GetWindowRect(first, &firstNarrow));
	ASSERT_TRUE(::GetWindowRect(second, &secondNarrow));
	EXPECT_GE(firstNarrow.right - firstNarrow.left, 80);
	EXPECT_GE(secondNarrow.right - secondNarrow.left, 80);
	EXPECT_EQ(4, secondNarrow.left - firstNarrow.right);
	EXPECT_EQ(0L, ::GetWindowLongPtrW(first, GWL_STYLE) & WS_VSCROLL);
	EXPECT_EQ(0L, ::GetWindowLongPtrW(second, GWL_STYLE) & WS_VSCROLL);

	EXPECT_TRUE(tool.CloseTerminalSplit());
	EXPECT_EQ(nullptr, ::FindWindowExW(tool.GetHwnd(), first, L"SakuraNativeTerminalWindow", nullptr));
	tool.Close();
	::DestroyWindow(parent);
}

TEST(TerminalTool, HeaderMaximizeAndCloseButtonsInvokeFrameOwnedActionsOnRelease)
{
	ToolHarness harness;
	const HWND parent = CreateHiddenParentWindow();
	ASSERT_NE(nullptr, parent);
	terminal::CTerminalTool tool(harness.Dependencies());
	ASSERT_TRUE(tool.Create(parent));
	int maximizeCalls = 0;
	int closeCalls = 0;
	bool maximized = false;
	tool.SetPanelActions({
		.closePanel = [&] { ++closeCalls; },
		.toggleMaximize = [&] {
			++maximizeCalls;
			maximized = !maximized;
		},
		.isMaximized = [&] { return maximized; },
	});
	const RECT visible{ 0, 0, 640, 320 };
	tool.Layout(visible, 96);

	const auto layout = terminal::CalculateTerminalHeaderLayout(visible, 96);
	const auto click = [&](terminal::TerminalHeaderTarget target) {
		const auto rect = layout.RectFor(target);
		const int x = rect.left + (rect.right - rect.left) / 2;
		const int y = rect.top + (rect.bottom - rect.top) / 2;
		::SendMessageW(tool.GetHwnd(), WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, y));
		::SendMessageW(tool.GetHwnd(), WM_LBUTTONUP, 0, MAKELPARAM(x, y));
	};
	click(terminal::TerminalHeaderTarget::Maximize);
	EXPECT_EQ(1, maximizeCalls);
	EXPECT_TRUE(maximized);
	click(terminal::TerminalHeaderTarget::Close);
	EXPECT_EQ(1, closeCalls);

	tool.Close();
	::DestroyWindow(parent);
}

TEST(TerminalTool, NewSessionsUseNewWorkspaceButExistingSessionsKeepOriginalCwd)
{
	ToolHarness harness;
	terminal::CTerminalTool tool(harness.Dependencies());
	tool.SetWorkingDirectory(L"C:\\first");
	tool.Activate();
	tool.SetWorkingDirectory(L"C:\\second");
	ASSERT_TRUE(tool.AddTerminal().has_value());
	ASSERT_EQ(2u, harness.backends.size());
	EXPECT_EQ(L"C:\\first", harness.backends[0]->workingDirectory);
	EXPECT_EQ(L"C:\\second", harness.backends[1]->workingDirectory);
	tool.Close();
}

TEST(TerminalTool, WorkspaceResetClosesAllTabsAndCreatesOneReplacementInNewCwd)
{
	ToolHarness harness;
	terminal::CTerminalTool tool(harness.Dependencies());
	tool.SetWorkingDirectory(L"C:\\first");
	tool.Activate();
	ASSERT_TRUE(tool.AddTerminal().has_value());
	ASSERT_TRUE(tool.SplitTerminalRight());
	ASSERT_EQ(3u, tool.TabCount());
	ASSERT_TRUE(tool.HasTerminalSplit());

	const auto reset = tool.ResetForWorkspace(L"C:\\second", true);
	EXPECT_EQ(terminal::TerminalWorkspaceResetOutcome::Restarted, reset.outcome);
	EXPECT_EQ(3u, reset.clearedTabCount);
	EXPECT_FALSE(reset.closeDeadlineExceeded);
	EXPECT_EQ(1u, tool.TabCount());
	EXPECT_FALSE(tool.HasTerminalSplit());
	ASSERT_EQ(4u, harness.backends.size());
	for( std::size_t index = 0; index < 3; ++index ) {
		EXPECT_EQ(1, harness.backends[index]->closeCalls.load());
	}
	EXPECT_EQ(L"C:\\second", harness.backends[3]->workingDirectory);
	EXPECT_EQ(0, harness.backends[3]->closeCalls.load());
	tool.Close();
}

TEST(TerminalTool, HiddenWorkspaceResetStaysEmptyUntilTerminalIsRevealed)
{
	ToolHarness harness;
	terminal::CTerminalTool tool(harness.Dependencies());
	tool.SetWorkingDirectory(L"C:\\first");
	tool.Activate();
	ASSERT_EQ(1u, tool.TabCount());

	const auto reset = tool.ResetForWorkspace(L"C:\\second", false);
	EXPECT_EQ(terminal::TerminalWorkspaceResetOutcome::Cleared, reset.outcome);
	EXPECT_EQ(1u, reset.clearedTabCount);
	EXPECT_EQ(0u, tool.TabCount());
	ASSERT_EQ(1u, harness.backends.size());

	EXPECT_TRUE(tool.EnsureSessionStarted());
	ASSERT_EQ(2u, harness.backends.size());
	EXPECT_EQ(L"C:\\second", harness.backends[1]->workingDirectory);
	tool.Close();
}

TEST(TerminalTool, WorkspaceReplacementStartFailureRemainsVisibleAndTyped)
{
	ToolHarness harness;
	terminal::CTerminalTool tool(harness.Dependencies());
	tool.SetWorkingDirectory(L"C:\\first");
	tool.Activate();
	harness.failStart = true;

	const auto reset = tool.ResetForWorkspace(L"C:\\second", true);
	EXPECT_EQ(terminal::TerminalWorkspaceResetOutcome::RestartFailed, reset.outcome);
	EXPECT_EQ(ERROR_ACCESS_DENIED, reset.errorCode);
	ASSERT_EQ(1u, tool.TabCount());
	EXPECT_EQ(terminal::TerminalSessionState::Failed, tool.Tabs()[0].state);
	ASSERT_EQ(2u, harness.backends.size());
	EXPECT_EQ(L"C:\\second", harness.backends[1]->workingDirectory);
	tool.Close();
}

} // namespace

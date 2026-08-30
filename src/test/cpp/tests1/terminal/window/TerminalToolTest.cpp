/*! @file */
#include "pch.h"
#include "terminal/window/TerminalHeaderLayout.h"
#include "terminal/window/TerminalTabPresentation.h"
#include "terminal/window/CTerminalTool.h"
#include "terminal/window/CTerminalWnd.h"
#include "terminal/window/TerminalFontMetrics.h"
#include "terminal/window/TerminalViewportGeometry.h"
#include "workbench/panel/CBottomPanelTool.h"
#include "terminal/model/TerminalModel.h"
#include "terminal/input/SakuraTerminalInputAdapter.h"
#include "theme/CThemeService.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <cstdlib>
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
		state->failStart = failStart;
		state->blockWrites.store(blockWrites);
		{
			const std::lock_guard lock(mutex);
			state->scriptedOutput = scriptedOutput;
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

	[[nodiscard]] bool SetDown( int virtualKey ) const
	{
		std::array<BYTE, 256> state{};
		if( !m_applied || ::GetKeyboardState(state.data()) == FALSE ) return false;
		state[virtualKey] |= 0x80;
		return ::SetKeyboardState(state.data()) != FALSE;
	}

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
	EXPECT_TRUE(WaitUntil([&] { return harness.backends[0]->closeCalls.load() == 1; }));
}

TEST(TerminalTool, ScreenPresetCreatesAndMovesBetweenTerminalGroups)
{
	ToolHarness harness;
	terminal::CTerminalTool tool(harness.Dependencies());
	tool.SetShortcutPreset(terminal::TerminalShortcutPreset::Screen);
	tool.Activate();

	ASSERT_EQ(1u, tool.TabCount());
	const auto first = tool.ActiveTerminalId();
	ASSERT_TRUE(first.has_value());

	const terminal::TerminalPresetKey prefix{ 'A', false, true, false };
	const terminal::TerminalPresetKey newTerminal{ 'C', false, false, false };
	const terminal::TerminalPresetKey nextTerminalBySpace{ VK_SPACE, false, false, false };
	const terminal::TerminalPresetKey nextTerminal{ 'N', false, false, false };
	const terminal::TerminalPresetKey previousTerminal{ 'P', false, false, false };

	ASSERT_TRUE(tool.DispatchShortcutPresetKey(prefix));
	ASSERT_TRUE(tool.DispatchShortcutPresetKey(newTerminal));
	ASSERT_EQ(2u, tool.TabCount());
	const auto second = tool.ActiveTerminalId();
	ASSERT_TRUE(second.has_value());
	EXPECT_NE(*first, *second);

	ASSERT_TRUE(tool.DispatchShortcutPresetKey(prefix));
	ASSERT_TRUE(tool.DispatchShortcutPresetKey(previousTerminal));
	EXPECT_EQ(first, tool.ActiveTerminalId());

	ASSERT_TRUE(tool.DispatchShortcutPresetKey(prefix));
	ASSERT_TRUE(tool.DispatchShortcutPresetKey(nextTerminalBySpace));
	EXPECT_EQ(second, tool.ActiveTerminalId());

	ASSERT_TRUE(tool.DispatchShortcutPresetKey(prefix));
	ASSERT_TRUE(tool.DispatchShortcutPresetKey(nextTerminal));
	EXPECT_EQ(first, tool.ActiveTerminalId());

	ASSERT_TRUE(tool.DispatchShortcutPresetKey(prefix));
	ASSERT_TRUE(tool.DispatchShortcutPresetKey(previousTerminal));
	EXPECT_EQ(second, tool.ActiveTerminalId());

	tool.Close();
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

TEST(TerminalTool, RendererSizesPtyFromTheGridInsideTheApportionedPadding)
{
	const HWND parent = CreateHiddenParentWindow();
	ASSERT_NE(nullptr, parent);
	terminal::CTerminalWnd renderer;
	ASSERT_TRUE(renderer.Create(parent, ::GetModuleHandleW(nullptr)));

	terminal::TerminalSize observed{};
	renderer.SetResizeSink([&observed](terminal::TerminalSize size) { observed = size; });
	constexpr unsigned int dpi = 144;
	const auto font = theme::CThemeService::FontSpec(theme::ThemeFontKind::Terminal);
	const auto metrics = terminal::CalculateTerminalFontMetrics(font.pointSize, dpi);
	const auto geometry = terminal::TerminalViewportGeometry::FromDpi(dpi);
	constexpr std::uint16_t expectedColumns = 10;
	constexpr std::uint16_t expectedRows = 4;
	const RECT bounds{ 0, 0,
		geometry.GridOriginX() + metrics.cellWidth * expectedColumns + geometry.padding,
		geometry.GridOriginY() + metrics.cellHeight * expectedRows + geometry.padding };

	renderer.Layout(bounds, dpi);
	EXPECT_EQ(expectedColumns, observed.columns);
	EXPECT_EQ(expectedRows, observed.rows);

	renderer.Close();
	::DestroyWindow(parent);
}

TEST(TerminalTool, PrintableKeyDownIsClaimedAndStillBecomesACharMessage)
{
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

	// Empty the thread queue so the pump below can only observe what this key
	// produced.
	MSG stale{};
	while( ::PeekMessageW(&stale, nullptr, 0, 0, PM_REMOVE) ) {}
	// This test directly constructs a MSG instead of retrieving it from the
	// thread queue. Arrange the modifier state after creating the HWND and
	// draining its queue so a later physical-key synchronization cannot turn the
	// synthetic printable key into a VT command.
	ScopedNeutralKeyboardState keyboardState;
	ASSERT_TRUE(keyboardState.Applied());

	// TranslateMessage needs the real scan code to produce a WM_CHAR.
	const LPARAM keyLParam = static_cast<LPARAM>(1 | (::MapVirtualKeyW('A', MAPVK_VK_TO_VSC) << 16));
	MSG keyDown{ renderer.GetHwnd(), WM_KEYDOWN, static_cast<WPARAM>('A'), keyLParam };
	// The terminal claims the printable key itself.  The frame runs
	// TranslateAccelerator after this hook against the legacy key-assignment
	// table, and a translated accelerator consumes the WM_KEYDOWN so that
	// TranslateMessage never runs and no WM_CHAR is ever produced.
	EXPECT_TRUE(renderer.PreTranslateMessage(keyDown));
	// Claiming it sends nothing by itself: TranslateMessage posts the WM_CHAR to
	// this thread's queue, and the shell bytes appear only once it is dispatched.
	EXPECT_TRUE(received.empty());

	bool sawCharMessage = false;
	MSG queued{};
	while( ::PeekMessageW(&queued, nullptr, 0, 0, PM_REMOVE) ) {
		if( queued.message == WM_CHAR && queued.hwnd == renderer.GetHwnd() ) sawCharMessage = true;
		::DispatchMessageW(&queued);
	}
	EXPECT_TRUE(sawCharMessage);
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
	using workbench::panel::CalculateBottomPanelVerticalLayout;

	const auto collapsed = CalculateBottomPanelVerticalLayout(12, 34, 28);
	EXPECT_EQ(12, collapsed.headerHeight);
	EXPECT_EQ(12, collapsed.contentTop);
	EXPECT_EQ(0, collapsed.contentHeight);
	EXPECT_EQ(0, collapsed.outputSelectorHeight);

	const auto visible = CalculateBottomPanelVerticalLayout(200, 34, 28);
	EXPECT_EQ(34, visible.headerHeight);
	EXPECT_EQ(34, visible.contentTop);
	EXPECT_EQ(166, visible.contentHeight);
	EXPECT_EQ(28, visible.outputSelectorHeight);
}

TEST(BottomPanelComposite, UsesCanonicalContainerIdsAndKeepsUserRequestsSeparateFromApply)
{
	using namespace workbench::panel;

	// These values come from WorkbenchIds, the repository's one accepted copy of
	// the upstream VS Code ViewContainer IDs. Terminal is deliberately `terminal`.
	EXPECT_EQ(std::string_view("terminal"), containerIds::Terminal);
	EXPECT_EQ(std::string_view("workbench.panel.markers"), containerIds::Problems);
	EXPECT_EQ(std::string_view("workbench.panel.output"), containerIds::Output);
	EXPECT_EQ(std::string_view("~remote.forwardedPortsContainer"), containerIds::Ports);
	EXPECT_EQ(std::string_view("workbench.panel.repl"), containerIds::DebugConsole);
	EXPECT_EQ(EBottomPanelContainerSupport::Supported,
		ClassifyBottomPanelContainer(containerIds::Terminal));
	EXPECT_EQ(EBottomPanelContainerSupport::Supported,
		ClassifyBottomPanelContainer(containerIds::Problems));
	EXPECT_EQ(EBottomPanelContainerSupport::Supported,
		ClassifyBottomPanelContainer(containerIds::Output));
	EXPECT_EQ(EBottomPanelContainerSupport::Unsupported,
		ClassifyBottomPanelContainer(containerIds::Ports));
	EXPECT_EQ(EBottomPanelContainerSupport::Unsupported,
		ClassifyBottomPanelContainer(containerIds::DebugConsole));
	EXPECT_EQ(EBottomPanelContainerSupport::Invalid,
		ClassifyBottomPanelContainer("workbench.panel.unknown"));

	CBottomPanelTool panel;
	EXPECT_EQ(containerIds::Terminal, panel.ActiveContainerId());
	EXPECT_FALSE(panel.AttachedContainerId().has_value());
	ASSERT_NE(nullptr, panel.Terminal());
	EXPECT_EQ(0u, panel.Terminal()->TabCount());

	int callbackCalls = 0;
	std::string_view lastRequested;
	panel.SetContainerSelectionCallback([&](const std::string_view containerId) {
		++callbackCalls;
		lastRequested = containerId;
		return true;
	});
	EXPECT_TRUE(panel.RequestContainerSelection(containerIds::Problems));
	EXPECT_EQ(1, callbackCalls);
	EXPECT_EQ(containerIds::Problems, lastRequested);
	// Accepted user intent does not optimistically apply selection or focus.
	EXPECT_EQ(containerIds::Terminal, panel.ActiveContainerId());
	EXPECT_EQ(0u, panel.Terminal()->TabCount());

	EXPECT_TRUE(panel.ApplyActiveContainer(containerIds::Problems));
	EXPECT_EQ(1, callbackCalls);
	EXPECT_EQ(containerIds::Problems, panel.ActiveContainerId());
	// A repeated valid user request is still delivered exactly once.
	EXPECT_TRUE(panel.RequestContainerSelection(containerIds::Problems));
	EXPECT_EQ(2, callbackCalls);
	panel.SetContainerSelectionCallback([&](const std::string_view containerId) {
		++callbackCalls;
		lastRequested = containerId;
		return false;
	});
	EXPECT_FALSE(panel.RequestContainerSelection(containerIds::Output));
	EXPECT_EQ(3, callbackCalls);
	EXPECT_EQ(containerIds::Output, lastRequested);
	EXPECT_EQ(containerIds::Problems, panel.ActiveContainerId());

	EXPECT_FALSE(panel.RequestContainerSelection(containerIds::Ports));
	EXPECT_FALSE(panel.RequestContainerSelection(containerIds::DebugConsole));
	EXPECT_FALSE(panel.RequestContainerSelection("workbench.panel.unknown"));
	EXPECT_FALSE(panel.ApplyActiveContainer(containerIds::Ports));
	EXPECT_FALSE(panel.ApplyActiveContainer("workbench.panel.unknown"));
	EXPECT_EQ(3, callbackCalls);
	EXPECT_EQ(containerIds::Problems, panel.ActiveContainerId());
	EXPECT_FALSE(panel.AttachedContainerId().has_value());
	EXPECT_EQ(0u, panel.Terminal()->TabCount());

	panel.Close();
}

TEST(BottomPanelComposite, DetachSwitchAndMessageRoutingPreserveTerminalUntilExplicitClose)
{
	using namespace workbench::panel;

	ToolHarness harness;
	const HWND parent = CreateHiddenParentWindow();
	ASSERT_NE(nullptr, parent);
	const HWND focusOwner = ::CreateWindowExW(0, L"STATIC", L"focus owner",
		WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 40, 20, parent, nullptr,
		::GetModuleHandleW(nullptr), nullptr);
	ASSERT_NE(nullptr, focusOwner);
	::ShowWindow(parent, SW_SHOWNOACTIVATE);

	CBottomPanelTool panel(harness.Dependencies());
	ASSERT_TRUE(panel.Create(parent));
	panel.Layout({ 0, 0, 640, 320 }, 96);
	workbench::win32::ProblemsPanelSnapshot problems;
	problems.revision = 3;
	problems.entries.push_back({ L"file:///C:/workspace/main.cpp", { 1, 2, 1, 8 },
		workbench::win32::EProblemsPanelSeverity::Error, L"error", L"compiler", L"main.cpp:2:3" });
	panel.SetProblemsSnapshot(std::move(problems));
	workbench::win32::OutputPanelSnapshot output;
	output.revision = 4;
	output.activeChannelId = "git";
	output.channels.push_back({ "git", L"Git", L"git status", true, true });
	panel.SetOutputSnapshot(std::move(output));
	ASSERT_EQ(std::optional<std::string>("git"), panel.SelectedOutputChannelId());
	ASSERT_EQ(std::optional<std::string_view>(containerIds::Terminal),
		panel.AttachedContainerId());
	panel.Activate();
	ASSERT_EQ(1u, harness.backends.size());
	ASSERT_EQ(1u, panel.Terminal()->TabCount());

	ScopedNeutralKeyboardState keyboard;
	ASSERT_TRUE(keyboard.Applied());
	ASSERT_TRUE(keyboard.SetDown(VK_CONTROL));
	ASSERT_TRUE(keyboard.SetDown(VK_SHIFT));
	MSG split{};
	split.hwnd = panel.Terminal()->GetHwnd();
	split.message = WM_KEYDOWN;
	split.wParam = '5';
	EXPECT_TRUE(panel.PreTranslateMessage(split));
	ASSERT_EQ(2u, harness.backends.size());
	EXPECT_EQ(2u, panel.Terminal()->TabCount());

	::SetFocus(focusOwner);
	ASSERT_EQ(focusOwner, ::GetFocus());
	EXPECT_FALSE(panel.RequestContainerSelection(containerIds::Ports));
	EXPECT_FALSE(panel.ApplyActiveContainer(containerIds::DebugConsole));
	EXPECT_FALSE(panel.RequestContainerSelection("workbench.panel.unknown"));
	EXPECT_EQ(containerIds::Terminal, panel.ActiveContainerId());
	EXPECT_EQ(focusOwner, ::GetFocus());
	EXPECT_EQ(0, harness.backends[0]->closeCalls.load());
	EXPECT_EQ(0, harness.backends[1]->closeCalls.load());
	EXPECT_TRUE(panel.ApplyActiveContainer(containerIds::Problems));
	EXPECT_EQ(focusOwner, ::GetFocus());
	EXPECT_FALSE(panel.PreTranslateMessage(split));
	EXPECT_EQ(2u, harness.backends.size());
	EXPECT_EQ(0, harness.backends[0]->closeCalls.load());
	EXPECT_EQ(0, harness.backends[1]->closeCalls.load());

	EXPECT_EQ(EBottomPanelPageDetachStatus::Detached, panel.DetachActivePage());
	EXPECT_EQ(EBottomPanelPageDetachStatus::AlreadyDetached, panel.DetachActivePage());
	EXPECT_FALSE(panel.AttachedContainerId().has_value());
	panel.Activate();
	EXPECT_EQ(focusOwner, ::GetFocus());
	EXPECT_EQ(0, harness.backends[0]->closeCalls.load());
	EXPECT_EQ(0, harness.backends[1]->closeCalls.load());

	EXPECT_EQ(EBottomPanelPageAttachStatus::Attached, panel.AttachActivePage());
	EXPECT_EQ(EBottomPanelPageAttachStatus::AlreadyAttached, panel.AttachActivePage());
	ASSERT_EQ(std::optional<std::string_view>(containerIds::Problems),
		panel.AttachedContainerId());
	panel.Activate();
	EXPECT_NE(focusOwner, ::GetFocus());

	EXPECT_TRUE(panel.ApplyActiveContainer(containerIds::Output));
	EXPECT_EQ(containerIds::Output, panel.ActiveContainerId());
	EXPECT_TRUE(panel.ApplyActiveContainer(containerIds::Terminal));
	EXPECT_EQ(containerIds::Terminal, panel.ActiveContainerId());
	EXPECT_EQ(0, harness.backends[0]->closeCalls.load());
	EXPECT_EQ(0, harness.backends[1]->closeCalls.load());

	EXPECT_EQ(EBottomPanelPageDetachStatus::Detached, panel.DetachActivePage());
	EXPECT_FALSE(panel.PreTranslateMessage(split));
	EXPECT_EQ(2u, harness.backends.size());
	EXPECT_EQ(EBottomPanelPageAttachStatus::Attached, panel.AttachActivePage());
	EXPECT_TRUE(panel.PreTranslateMessage(split));
	ASSERT_EQ(3u, harness.backends.size());
	EXPECT_EQ(3u, panel.Terminal()->TabCount());

	panel.Close();
	panel.Close();
	for (const auto& backend : harness.backends) {
		EXPECT_TRUE(WaitUntil([&] { return backend->closeCalls.load() == 1; }));
		EXPECT_EQ(1, backend->closeCalls.load());
	}
	EXPECT_EQ(EBottomPanelPageAttachStatus::Closed, panel.AttachActivePage());
	EXPECT_EQ(EBottomPanelPageDetachStatus::Closed, panel.DetachActivePage());
	::DestroyWindow(parent);
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
	harness.scriptedOutput = "\x1b]0;Claude Code\x07";
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
	// This test owns drain latency, not display policy: the raw OSC title is what
	// proves the leading drain ran, and the tab title is resolved elsewhere.
	EXPECT_EQ(L"Claude Code", tool.Tabs().front().sequenceTitle);
	EXPECT_EQ(L"Claude Code", tool.ActiveTerminalTitle());

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

TEST(TerminalTool, DrainPublishesScrollbackMutationExactlyOnce)
{
	ToolHarness harness;
	harness.scriptedOutput = "a\r\nb\r\n";
	std::atomic<int> outputNotifications{};
	terminal::TerminalTabManager manager(harness.Dependencies(), [&outputNotifications](const terminal::TerminalTabEvent& event) {
		if( event.kind == terminal::TerminalTabEventKind::OutputAvailable ) ++outputNotifications;
	});
	const auto id = manager.Activate({ 8, 1 }, L"C:\\workspace");
	ASSERT_TRUE(id.has_value());
	ASSERT_TRUE(WaitUntil([&] { return outputNotifications.load() > 0; }));

	const auto first = manager.DrainOutput(*id);
	EXPECT_EQ(2u, first.scrollbackChange.Appended());
	EXPECT_EQ(0u, first.scrollbackChange.Evicted());
	EXPECT_FALSE(first.scrollbackChange.Cleared());
	EXPECT_FALSE(manager.DrainOutput(*id).scrollbackChange.Changed());
	manager.Close();
}

TEST(TerminalTool, AppliesScrollbackLimitToExistingFutureAndRestartedModels)
{
	ToolHarness harness;
	terminal::TerminalTabManager manager(harness.Dependencies());
	EXPECT_EQ(terminal::TerminalModel::kDefaultScrollbackLines, manager.ScrollbackLimit());
	EXPECT_TRUE(manager.SetScrollbackLimit(7).empty());

	const auto first = manager.Activate({ 8, 2 }, L"C:\\workspace");
	ASSERT_TRUE(first.has_value());
	auto* firstModel = manager.Model(*first);
	ASSERT_NE(nullptr, firstModel);
	EXPECT_EQ(7u, firstModel->ScrollbackLimit());
	for( int line = 0; line < 5; ++line ) {
		for( const auto character : std::wstring_view(L"line") ) firstModel->Print(character);
		firstModel->ExecuteControl(L'\r');
		firstModel->ExecuteControl(L'\n');
	}
	static_cast<void>(firstModel->ConsumeScrollbackChange());

	const auto changes = manager.SetScrollbackLimit(2);
	ASSERT_EQ(1u, changes.size());
	EXPECT_EQ(*first, changes.front().tabId);
	EXPECT_EQ(2u, firstModel->ScrollbackLimit());
	EXPECT_EQ(2u, firstModel->ScrollbackSize());
	EXPECT_GT(changes.front().change.Evicted(), 0u);

	const auto second = manager.AddTab({ 8, 2 }, L"C:\\workspace");
	ASSERT_TRUE(second.has_value());
	ASSERT_NE(nullptr, manager.Model(*second));
	EXPECT_EQ(2u, manager.Model(*second)->ScrollbackLimit());
	ASSERT_TRUE(manager.RestartTab(*first, { 8, 2 }, L"C:\\workspace"));
	ASSERT_NE(nullptr, manager.Model(*first));
	EXPECT_EQ(2u, manager.Model(*first)->ScrollbackLimit());

	EXPECT_TRUE(manager.SetScrollbackLimit(terminal::TerminalModel::kMaxScrollbackLines + 1).empty());
	EXPECT_EQ(terminal::TerminalModel::kMaxScrollbackLines, manager.ScrollbackLimit());
	manager.Close();
}

//! The OSC title is stored raw. A recognized Agent CLI reaches the visible tab
//! presentation through terminal.integrated.tabs.allowAgentCliTitle, while the
//! stable process/profile identity remains available as its fallback.
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
	EXPECT_TRUE(drained.sequenceChanged);
	const auto tabs = manager.Snapshot();
	ASSERT_EQ(1u, tabs.size());
	EXPECT_EQ(L"Claude Code", tabs.front().sequenceTitle);
	EXPECT_EQ(L"pwsh", tabs.front().processName);
	EXPECT_EQ(L"pwsh", tabs.front().profileLabel);

	terminal::TerminalTabPresentationContext context;
	context.processName = tabs.front().processName;
	context.sequenceTitle = tabs.front().sequenceTitle;
	context.recognizedAgentCli = terminal::IsRecognizedAgentCliTitle(tabs.front().sequenceTitle);
	EXPECT_EQ(L"Claude Code", terminal::ResolveTerminalTabPresentation({}, context).title);

	terminal::TerminalTabPresentationSettings disallowed;
	disallowed.allowAgentCliTitle = false;
	EXPECT_EQ(L"pwsh", terminal::ResolveTerminalTabPresentation(disallowed, context).title);
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
	// New Terminal starts a new group; it must not implicitly split the
	// currently visible terminal.  Splitting is reserved for Split Terminal.
	EXPECT_FALSE(tool.HasTerminalSplit());
	EXPECT_EQ(second, tool.ActiveTerminalId());
	EXPECT_TRUE(tool.SelectTerminal(*first));
	EXPECT_EQ(first, tool.ActiveTerminalId());
	EXPECT_TRUE(tool.RestartTerminal(*first));
	EXPECT_EQ(3u, harness.backends.size());
	EXPECT_TRUE(WaitUntil([&] { return harness.backends[0]->closeCalls.load() == 1; }));
	EXPECT_TRUE(tool.DeleteTerminal(*second));
	EXPECT_EQ(1u, tool.TabCount());
	EXPECT_FALSE(tool.DeleteTerminal(*second));
	tool.Close();
}

TEST(TerminalTool, RestartClearsOldSequenceState)
{
	ToolHarness harness;
	harness.scriptedOutput = "\x1b]0;Old process title\x07";
	std::atomic<int> outputNotifications{};
	auto dependencies = harness.Dependencies();
	auto createSession = std::move(dependencies.createSession);
	std::atomic<std::size_t> sessionIndex{};
	dependencies.createSession = [&harness, createSession = std::move(createSession), &sessionIndex](
		terminal::TerminalSessionCallbacks callbacks) {
		if( sessionIndex.fetch_add(1) == 1 ) {
			const std::lock_guard lock(harness.mutex);
			harness.scriptedOutput.clear();
		}
		return createSession(std::move(callbacks));
	};
	terminal::TerminalTabManager manager(std::move(dependencies), [&outputNotifications](const terminal::TerminalTabEvent& event) {
		if( event.kind == terminal::TerminalTabEventKind::OutputAvailable ) ++outputNotifications;
	});
	const auto id = manager.Activate({ 80, 24 }, L"C:\\workspace");
	ASSERT_TRUE(id.has_value());
	ASSERT_TRUE(WaitUntil([&] { return outputNotifications.load() > 0; }));
	const auto drained = manager.DrainOutput(*id);
	ASSERT_TRUE(drained.sequenceChanged);
	ASSERT_EQ(L"Old process title", manager.Snapshot().front().sequenceTitle);

	ASSERT_TRUE(manager.RestartTab(*id, { 80, 24 }, L"C:\\workspace"));
	const auto restarted = manager.Snapshot();
	ASSERT_EQ(1u, restarted.size());
	EXPECT_EQ(L"", restarted.front().sequenceTitle);
	EXPECT_EQ(2u, harness.backends.size());
	manager.Close();
}

TEST(TerminalTool, SupportsArbitraryFlatSplitGroupsAndClosesTheFocusedPane)
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
	EXPECT_EQ(terminal::TerminalPaneOrientation::Horizontal, tool.ActivePaneOrientation());
	EXPECT_NE(left, tool.ActiveTerminalId());
	ASSERT_EQ(2u, harness.backends.size());
	EXPECT_EQ(L"C:\\split workspace", harness.backends[1]->workingDirectory);
	ASSERT_TRUE(tool.SplitTerminalRight());
	EXPECT_TRUE(tool.HasTerminalSplit());
	EXPECT_EQ(3u, tool.TabCount());
	ASSERT_EQ(3u, harness.backends.size());
	EXPECT_EQ(L"C:\\split workspace", harness.backends[2]->workingDirectory);
	ASSERT_TRUE(tool.SplitTerminalDown());
	EXPECT_EQ(4u, tool.TabCount());
	EXPECT_EQ(terminal::TerminalPaneOrientation::Vertical, tool.ActivePaneOrientation());
	ASSERT_EQ(4u, harness.backends.size());

	EXPECT_TRUE(tool.CloseTerminalSplit());
	EXPECT_TRUE(tool.HasTerminalSplit());
	EXPECT_EQ(3u, tool.TabCount());
	EXPECT_TRUE(WaitUntil([&] { return harness.backends[3]->closeCalls.load() == 1; }));
	EXPECT_TRUE(tool.CloseTerminalSplit());
	EXPECT_TRUE(tool.HasTerminalSplit());
	EXPECT_EQ(2u, tool.TabCount());
	EXPECT_TRUE(WaitUntil([&] { return harness.backends[2]->closeCalls.load() == 1; }));
	EXPECT_TRUE(tool.CloseTerminalSplit());
	EXPECT_FALSE(tool.HasTerminalSplit());
	EXPECT_EQ(1u, tool.TabCount());
	tool.Close();
	EXPECT_TRUE(WaitUntil([&] { return harness.backends[0]->closeCalls.load() == 1; }));
}

TEST(TerminalTool, SplitCreatesThreeNativeViewportsAndRightTerminalList)
{
	ToolHarness harness;
	const HWND parent = CreateHiddenParentWindow();
	ASSERT_NE(nullptr, parent);
	terminal::CTerminalTool tool(harness.Dependencies());
	ASSERT_TRUE(tool.Create(parent));
	const RECT visible{ 0, 0, 900, 320 };
	tool.Layout(visible, 96);
	tool.Activate();
	ASSERT_TRUE(tool.SplitTerminalRight());
	ASSERT_TRUE(tool.SplitTerminalRight());
	EXPECT_EQ(3u, tool.VisiblePaneCount());
	EXPECT_TRUE(tool.HasTerminalTabsList());

	const HWND first = ::FindWindowExW(tool.GetHwnd(), nullptr, L"SakuraNativeTerminalWindow", nullptr);
	ASSERT_NE(nullptr, first);
	const HWND second = ::FindWindowExW(tool.GetHwnd(), first, L"SakuraNativeTerminalWindow", nullptr);
	ASSERT_NE(nullptr, second);
	const HWND third = ::FindWindowExW(tool.GetHwnd(), second, L"SakuraNativeTerminalWindow", nullptr);
	ASSERT_NE(nullptr, third);
	EXPECT_EQ(nullptr, ::FindWindowExW(tool.GetHwnd(), third, L"SakuraNativeTerminalWindow", nullptr));
	RECT firstBefore{};
	RECT secondBefore{};
	RECT thirdBefore{};
	ASSERT_TRUE(::GetWindowRect(first, &firstBefore));
	ASSERT_TRUE(::GetWindowRect(second, &secondBefore));
	ASSERT_TRUE(::GetWindowRect(third, &thirdBefore));
	EXPECT_LT(firstBefore.left, secondBefore.left);
	EXPECT_LT(secondBefore.left, thirdBefore.left);
	EXPECT_LE(firstBefore.right, secondBefore.left);
	EXPECT_LE(secondBefore.right, thirdBefore.left);
	EXPECT_GT(firstBefore.right - firstBefore.left, 200);
	EXPECT_GT(secondBefore.right - secondBefore.left, 200);
	EXPECT_GT(thirdBefore.right - thirdBefore.left, 200);
	EXPECT_EQ(firstBefore.right - firstBefore.left, secondBefore.right - secondBefore.left);
	EXPECT_EQ(secondBefore.right - secondBefore.left, thirdBefore.right - thirdBefore.left);
	const RECT tabs = tool.TerminalTabsBounds();
	EXPECT_LT(tabs.left, tabs.right);
	EXPECT_EQ(120, tabs.right - tabs.left);
	POINT tabsOrigin{ tabs.left, tabs.top };
	::ClientToScreen(tool.GetHwnd(), &tabsOrigin);
	EXPECT_LE(thirdBefore.right, tabsOrigin.x);

	const auto snapshots = tool.Tabs();
	ASSERT_EQ(3u, snapshots.size());
	::SendMessageW(tool.GetHwnd(), WM_LBUTTONDOWN, MK_LBUTTON,
		MAKELPARAM(tabs.left + 4, tabs.top + 8));
	EXPECT_EQ(snapshots.front().id, tool.ActiveTerminalId());

	POINT divider{ firstBefore.right, firstBefore.top + 20 };
	::ScreenToClient(tool.GetHwnd(), &divider);
	::SendMessageW(tool.GetHwnd(), WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(divider.x + 1, divider.y));
	::SendMessageW(tool.GetHwnd(), WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(divider.x + 80, divider.y));
	::SendMessageW(tool.GetHwnd(), WM_LBUTTONUP, 0, MAKELPARAM(divider.x + 80, divider.y));
	RECT firstAfter{};
	ASSERT_TRUE(::GetWindowRect(first, &firstAfter));
	EXPECT_GT(firstAfter.right - firstAfter.left, firstBefore.right - firstBefore.left);

	// A narrow panel still gives every pane the VS Code 80 px minimum while the
	// right list remains at its 120 px default when it fits.
	const RECT narrow{ 0, 0, 420, 320 };
	tool.Layout(narrow, 96);
	RECT firstNarrow{};
	RECT secondNarrow{};
	RECT thirdNarrow{};
	ASSERT_TRUE(::GetWindowRect(first, &firstNarrow));
	ASSERT_TRUE(::GetWindowRect(second, &secondNarrow));
	ASSERT_TRUE(::GetWindowRect(third, &thirdNarrow));
	EXPECT_GE(firstNarrow.right - firstNarrow.left, 80);
	EXPECT_GE(secondNarrow.right - secondNarrow.left, 80);
	EXPECT_GE(thirdNarrow.right - thirdNarrow.left, 80);
	EXPECT_EQ(4, secondNarrow.left - firstNarrow.right);
	EXPECT_EQ(4, thirdNarrow.left - secondNarrow.right);
	EXPECT_EQ(0L, ::GetWindowLongPtrW(first, GWL_STYLE) & WS_VSCROLL);
	EXPECT_EQ(0L, ::GetWindowLongPtrW(second, GWL_STYLE) & WS_VSCROLL);
	EXPECT_EQ(0L, ::GetWindowLongPtrW(third, GWL_STYLE) & WS_VSCROLL);

	EXPECT_TRUE(tool.CloseTerminalSplit());
	EXPECT_EQ(2u, tool.VisiblePaneCount());
	EXPECT_TRUE(tool.HasTerminalTabsList());
	tool.Close();
	::DestroyWindow(parent);
}

TEST(TerminalTool, SplitDownOnlySplitsTheFocusedPane)
{
	ToolHarness harness;
	const HWND parent = CreateHiddenParentWindow();
	ASSERT_NE(nullptr, parent);
	terminal::CTerminalTool tool(harness.Dependencies());
	ASSERT_TRUE(tool.Create(parent));
	tool.Layout({ 0, 0, 900, 420 }, 96);
	tool.Activate();
	ASSERT_TRUE(tool.SplitTerminalRight());
	ASSERT_TRUE(tool.SplitTerminalDown());
	EXPECT_EQ(terminal::TerminalPaneOrientation::Vertical, tool.ActivePaneOrientation());

	const HWND first = ::FindWindowExW(tool.GetHwnd(), nullptr, L"SakuraNativeTerminalWindow", nullptr);
	ASSERT_NE(nullptr, first);
	const HWND second = ::FindWindowExW(tool.GetHwnd(), first, L"SakuraNativeTerminalWindow", nullptr);
	ASSERT_NE(nullptr, second);
	const HWND third = ::FindWindowExW(tool.GetHwnd(), second, L"SakuraNativeTerminalWindow", nullptr);
	ASSERT_NE(nullptr, third);
	RECT firstRect{};
	RECT secondRect{};
	RECT thirdRect{};
	ASSERT_TRUE(::GetWindowRect(first, &firstRect));
	ASSERT_TRUE(::GetWindowRect(second, &secondRect));
	ASSERT_TRUE(::GetWindowRect(third, &thirdRect));
	EXPECT_EQ(firstRect.top, secondRect.top);
	EXPECT_EQ(firstRect.bottom, thirdRect.bottom);
	EXPECT_LT(firstRect.right, secondRect.left);
	EXPECT_EQ(secondRect.left, thirdRect.left);
	EXPECT_EQ(secondRect.right, thirdRect.right);
	// An odd client width can leave the two integer pixel rectangles one pixel
	// apart even though the split weights are equal.
	EXPECT_LE(std::abs((firstRect.right - firstRect.left) - (secondRect.right - secondRect.left)), 1);
	EXPECT_LT(secondRect.bottom, thirdRect.top);
	EXPECT_EQ(secondRect.bottom - secondRect.top, thirdRect.bottom - thirdRect.top);

	tool.Close();
	::DestroyWindow(parent);
}

TEST(TerminalTool, SplitDownStacksNativeViewportsVertically)
{
	ToolHarness harness;
	const HWND parent = CreateHiddenParentWindow();
	ASSERT_NE(nullptr, parent);
	terminal::CTerminalTool tool(harness.Dependencies());
	ASSERT_TRUE(tool.Create(parent));
	tool.Layout({ 0, 0, 900, 420 }, 96);
	tool.Activate();
	ASSERT_TRUE(tool.SplitTerminalDown());
	ASSERT_TRUE(tool.SplitTerminalDown());
	EXPECT_EQ(3u, tool.VisiblePaneCount());
	EXPECT_EQ(terminal::TerminalPaneOrientation::Vertical, tool.ActivePaneOrientation());

	const HWND first = ::FindWindowExW(tool.GetHwnd(), nullptr, L"SakuraNativeTerminalWindow", nullptr);
	ASSERT_NE(nullptr, first);
	const HWND second = ::FindWindowExW(tool.GetHwnd(), first, L"SakuraNativeTerminalWindow", nullptr);
	ASSERT_NE(nullptr, second);
	const HWND third = ::FindWindowExW(tool.GetHwnd(), second, L"SakuraNativeTerminalWindow", nullptr);
	ASSERT_NE(nullptr, third);
	RECT firstRect{};
	RECT secondRect{};
	RECT thirdRect{};
	ASSERT_TRUE(::GetWindowRect(first, &firstRect));
	ASSERT_TRUE(::GetWindowRect(second, &secondRect));
	ASSERT_TRUE(::GetWindowRect(third, &thirdRect));
	EXPECT_LT(firstRect.top, secondRect.top);
	EXPECT_LT(secondRect.top, thirdRect.top);
	EXPECT_LE(firstRect.bottom, secondRect.top);
	EXPECT_LE(secondRect.bottom, thirdRect.top);
	EXPECT_EQ(firstRect.left, secondRect.left);
	EXPECT_EQ(secondRect.left, thirdRect.left);
	EXPECT_GE(firstRect.bottom - firstRect.top, 80);
	EXPECT_GE(secondRect.bottom - secondRect.top, 80);
	EXPECT_GE(thirdRect.bottom - thirdRect.top, 80);
	tool.Close();
	::DestroyWindow(parent);
}

TEST(TerminalTool, TabPresentationSettingsUseGroupVisibilityAndMoveListLeftWithoutRestart)
{
	ToolHarness harness;
	const auto parent = CreateHiddenParentWindow();
	ASSERT_NE(nullptr, parent);
	terminal::CTerminalTool tool(harness.Dependencies());
	ASSERT_TRUE(tool.Create(parent));
	tool.Layout({ 0, 0, 900, 320 }, 96);
	tool.Activate();
	ASSERT_TRUE(tool.SplitTerminalRight());
	ASSERT_TRUE(tool.HasTerminalTabsList());
	const auto startsBefore = harness.backends.size();

	terminal::TerminalTabPresentationSettings settings;
	settings.hideCondition = terminal::TerminalTabsHideCondition::SingleGroup;
	settings.location = terminal::TerminalTabsLocation::Left;
	tool.SetTabPresentationSettings(settings);
	EXPECT_FALSE(tool.HasTerminalTabsList());
	EXPECT_EQ(startsBefore, harness.backends.size());

	// New Terminal creates a second group. singleGroup must use group count,
	// not the three terminal instances now owned by the manager.
	ASSERT_TRUE(tool.AddTerminal().has_value());
	EXPECT_TRUE(tool.HasTerminalTabsList());
	const RECT tabs = tool.TerminalTabsBounds();
	EXPECT_EQ(0, tabs.left);
	EXPECT_EQ(120, tabs.right - tabs.left);
	EXPECT_GE(tabs.right, 0);
	EXPECT_EQ(startsBefore + 1, harness.backends.size());

	tool.Close();
	::DestroyWindow(parent);
}

TEST(TerminalTool, TerminalListFocusKeepsVsCodeSplitKeybindingInTheTerminalUi)
{
	ToolHarness harness;
	const HWND parent = CreateHiddenParentWindow();
	ASSERT_NE(nullptr, parent);
	terminal::CTerminalTool tool(harness.Dependencies());
	ASSERT_TRUE(tool.Create(parent));
	tool.Layout({ 0, 0, 900, 320 }, 96);
	tool.Activate();
	ASSERT_TRUE(tool.SplitTerminalRight());
	ASSERT_TRUE(tool.HasTerminalTabsList());

	// A single click places focus on the terminal list's host HWND. The VS Code
	// terminal commands must still apply there, not only to a native pane child.
	const RECT tabs = tool.TerminalTabsBounds();
	::SendMessageW(tool.GetHwnd(), WM_LBUTTONDOWN, MK_LBUTTON,
		MAKELPARAM(tabs.left + 4, tabs.top + 8));
	ScopedNeutralKeyboardState keyboardState;
	ASSERT_TRUE(keyboardState.Applied());
	ASSERT_TRUE(keyboardState.SetDown(VK_CONTROL));
	ASSERT_TRUE(keyboardState.SetDown(VK_SHIFT));
	MSG split{ tool.GetHwnd(), WM_KEYDOWN, static_cast<WPARAM>('5'), 0 };
	EXPECT_TRUE(tool.PreTranslateMessage(split));
	EXPECT_EQ(3u, tool.VisiblePaneCount());
	EXPECT_EQ(3u, tool.TabCount());

	tool.Close();
	::DestroyWindow(parent);
}

TEST(TerminalTool, TerminalListScrollsWhenArbitraryGroupsExceedItsHeight)
{
	ToolHarness harness;
	const HWND parent = CreateHiddenParentWindow();
	ASSERT_NE(nullptr, parent);
	terminal::CTerminalTool tool(harness.Dependencies());
	ASSERT_TRUE(tool.Create(parent));
	// The terminal content has only enough height for a few list rows.
	tool.Layout({ 0, 0, 900, 100 }, 96);
	tool.Activate();
	for( int count = 0; count < 4; ++count ) ASSERT_TRUE(tool.SplitTerminalRight());
	const auto snapshots = tool.Tabs();
	ASSERT_EQ(5u, snapshots.size());
	const RECT tabs = tool.TerminalTabsBounds();
	ASSERT_LT(tabs.top, tabs.bottom);

	POINT listPoint{ tabs.left + 4, tabs.top + 8 };
	::ClientToScreen(tool.GetHwnd(), &listPoint);
	::SendMessageW(tool.GetHwnd(), WM_MOUSEWHEEL,
		MAKEWPARAM(0, static_cast<WORD>(-3 * WHEEL_DELTA)), MAKELPARAM(listPoint.x, listPoint.y));
	::SendMessageW(tool.GetHwnd(), WM_LBUTTONDOWN, MK_LBUTTON,
		MAKELPARAM(tabs.left + 4, tabs.top + 8));
	// With 70 px of content height and 24-DIP rows only two rows fit. The first
	// visible row advanced to the fourth session, which was initially clipped.
	EXPECT_EQ(snapshots[3].id, tool.ActiveTerminalId());

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

TEST(TerminalTool, HeaderActionsStayEnabledForMultipleSplitPanes)
{
	ToolHarness harness;
	const HWND parent = CreateHiddenParentWindow();
	ASSERT_NE(nullptr, parent);
	terminal::CTerminalTool tool(harness.Dependencies());
	ASSERT_TRUE(tool.Create(parent));
	tool.Layout({ 0, 0, 640, 320 }, 96);
	tool.Activate();
	ASSERT_TRUE(tool.SplitTerminalRight());
	ASSERT_EQ(2u, tool.TabCount());
	ASSERT_TRUE(tool.HasTerminalSplit());

	const auto layout = terminal::CalculateTerminalHeaderLayout({ 0, 0, 640, 320 }, 96);
	const auto split = layout.RectFor(terminal::TerminalHeaderTarget::Split);
	ASSERT_LT(split.left, split.right);
	const int x = split.left + (split.right - split.left) / 2;
	const int y = split.top + (split.bottom - split.top) / 2;
	::SendMessageW(tool.GetHwnd(), WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, y));
	::SendMessageW(tool.GetHwnd(), WM_LBUTTONUP, 0, MAKELPARAM(x, y));

	// Two panes are still one terminal group, matching VS Code's action
	// visibility condition. If the header were disabled, the second click
	// would leave the count at two.
	EXPECT_EQ(3u, tool.TabCount());

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
		EXPECT_TRUE(WaitUntil([&, index] { return harness.backends[index]->closeCalls.load() == 1; }));
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

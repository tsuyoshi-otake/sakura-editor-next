/*! @file */
#include "pch.h"
#include "terminal/window/CTerminalTool.h"
#include "terminal/window/CTerminalWnd.h"
#include "terminal/model/TerminalModel.h"
#include "terminal/input/SakuraTerminalInputAdapter.h"

#include <atomic>
#include <chrono>
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
	std::wstring workingDirectory;
	terminal::TerminalSize initialSize{};
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
		return terminal::TerminalStartResult::Success();
	}

	terminal::TerminalBackendReadResult ReadOutput( std::span<std::uint8_t>, std::chrono::milliseconds timeout ) override
	{
		std::this_thread::sleep_for(std::min(timeout, 2ms));
		return m_state->closed ? terminal::TerminalBackendReadResult{ terminal::TerminalBackendReadStatus::EndOfFile, 0, 0 }
			: terminal::TerminalBackendReadResult{};
	}

	terminal::TerminalBackendWriteResult WriteInput( std::span<const std::uint8_t> source ) override
	{
		return { terminal::TerminalBackendWriteStatus::Completed, source.size(), 0 };
	}

	terminal::TerminalBackendOperationResult Resize( terminal::TerminalSize ) override { return { true, 0 }; }
	void RequestGracefulClose() noexcept override { ++m_state->gracefulCloseCalls; }
	bool WaitForExit( std::chrono::milliseconds ) noexcept override { return true; }
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

	terminal::TerminalTabManagerDependencies Dependencies()
	{
		terminal::TerminalTabManagerDependencies dependencies;
		dependencies.createSession = [this](terminal::TerminalSessionCallbacks callbacks) {
			auto state = std::make_shared<BackendState>();
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
	const HWND parent = CreateHiddenParentWindow();
	ASSERT_NE(nullptr, parent);
	terminal::CTerminalWnd renderer;
	ASSERT_TRUE(renderer.Create(parent, ::GetModuleHandleW(nullptr)));
	terminal::SakuraTerminalInputAdapter inputAdapter;
	renderer.SetInputAdapter(&inputAdapter);
	std::string received;
	renderer.SetInputSink([&received](std::span<const std::uint8_t> bytes) {
		received.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
	});

	MSG keyDown{ renderer.GetHwnd(), WM_KEYDOWN, static_cast<WPARAM>('A'), 1 };
	EXPECT_FALSE(renderer.PreTranslateMessage(keyDown));
	EXPECT_TRUE(received.empty());
	::SendMessageW(renderer.GetHwnd(), WM_CHAR, L'a', 1);
	EXPECT_EQ("a", received);

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

TEST(TerminalTool, SupportsAddSelectRestartAndDeleteWithoutPaneSplitting)
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

} // namespace

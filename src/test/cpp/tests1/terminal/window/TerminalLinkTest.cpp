/*! @file */
#include "pch.h"
#include "terminal/window/TerminalLink.h"
#include "terminal/window/CTerminalWnd.h"

#include <Windows.h>
#include <string_view>

namespace {

void Print( terminal::TerminalModel& model, std::u32string_view text )
{
	for( const auto ch : text ) {
		if( ch < 32 ) model.ExecuteControl(static_cast<wchar_t>(ch));
		else model.Print(ch);
	}
}

TEST(TerminalLink, DetectsExactHttpTargetsAndExcludesSurroundingPunctuation)
{
	terminal::TerminalModel model(160, 3);
	Print(model, U"Visit (https://example.com/a_(b)?x=1&y=2#part), http://localhost:3000/test.");
	const auto link = terminal::DetectTerminalWebLink(model, { 0, 10 });
	ASSERT_TRUE(link);
	EXPECT_EQ(L"https://example.com/a_(b)?x=1&y=2#part", link->uri);
	EXPECT_EQ(7U, link->start.column);
	EXPECT_FALSE(terminal::DetectTerminalWebLink(model, { 0, link->end.column }));
	const auto second = terminal::DetectTerminalWebLink(model, { 0, 50 });
	ASSERT_TRUE(second);
	EXPECT_EQ(L"http://localhost:3000/test", second->uri);
	EXPECT_FALSE(terminal::DetectTerminalWebLink(model, { 0, 1 }));
}

TEST(TerminalLink, JoinsSoftWrapsIncludingScrollback)
{
	terminal::TerminalModel model(12, 2);
	Print(model, U"https://example.com/path/to/resource?query=value#fragment");
	ASSERT_GT(model.ScrollbackSize(), 0U);
	const auto link = terminal::DetectTerminalWebLink(model, { 2, 5 });
	ASSERT_TRUE(link);
	EXPECT_EQ(L"https://example.com/path/to/resource?query=value#fragment", link->uri);
	EXPECT_EQ(0U, link->start.row);
	EXPECT_GT(link->end.row, 2U);
}

TEST(TerminalLink, DoesNotJoinHardLineBreaks)
{
	terminal::TerminalModel model(80, 3);
	Print(model, U"https://example.com/\r\nseparate");
	const auto link = terminal::DetectTerminalWebLink(model, { 0, 4 });
	ASSERT_TRUE(link);
	EXPECT_EQ(L"https://example.com/", link->uri);
	EXPECT_FALSE(terminal::DetectTerminalWebLink(model, { 1, 2 }));
}

TEST(TerminalLink, MapsWideGraphemesAndContinuationCells)
{
	terminal::TerminalModel model(80, 3);
	Print(model, U"\u65e5\u672c https://example.com/\u65e5\u672c\u8a9e");
	const auto link = terminal::DetectTerminalWebLink(model, { 0, 6 });
	ASSERT_TRUE(link);
	EXPECT_EQ(5U, link->start.column);
	EXPECT_EQ(L"https://example.com/\u65e5\u672c\u8a9e", link->uri);
	EXPECT_EQ(link, terminal::DetectTerminalWebLink(model, { 0, 26 }));
	EXPECT_EQ(link, terminal::DetectTerminalWebLink(model, { 0, 27 }));
	EXPECT_FALSE(terminal::DetectTerminalWebLink(model, { 0, 1 }));
}

TEST(TerminalLink, SupportsUppercaseSchemesIpv6AndAdjacentLinks)
{
	terminal::TerminalModel model(120, 2);
	Print(model, U"[HTTPS://[::1]:8080/a] <http://example.com/b>");
	const auto first = terminal::DetectTerminalWebLink(model, { 0, 3 });
	ASSERT_TRUE(first);
	EXPECT_EQ(L"HTTPS://[::1]:8080/a", first->uri);
	const auto second = terminal::DetectTerminalWebLink(model, { 0, 27 });
	ASSERT_TRUE(second);
	EXPECT_EQ(L"http://example.com/b", second->uri);
}

TEST(TerminalLink, RejectsUnsupportedMalformedAndOutOfRangeTargets)
{
	for( const auto text : { U"file:///C:/run.exe", U"javascript:alert(1)", U"vscode://file/a",
		U"https:///missing-host", U"http://:80", U"prefixhttps://example.com", U"http://" } ) {
		terminal::TerminalModel model(80, 2);
		Print(model, text);
		for( std::size_t column = 0; column < 50; ++column ) {
			EXPECT_FALSE(terminal::DetectTerminalWebLink(model, { 0, column }));
		}
		EXPECT_FALSE(terminal::DetectTerminalWebLink(model, { 99, 0 }));
		EXPECT_FALSE(terminal::DetectTerminalWebLink(model, { 0, 99 }));
	}
}

TEST(TerminalLink, BoundsOversizedLogicalLinesWithoutOpeningTruncatedUrls)
{
	terminal::TerminalModel model(80, 2, 200);
	Print(model, U"https://example.com/");
	Print(model, std::u32string(terminal::kTerminalLinkScanLimit, U'a'));
	EXPECT_FALSE(terminal::DetectTerminalWebLink(model, { 0, 5 }));
	EXPECT_FALSE(terminal::DetectTerminalWebLink(model, { 100, 5 }));
}

TEST(TerminalLink, AlternateScreenDoesNotReadMainScreenHistory)
{
	terminal::TerminalModel model(30, 2);
	Print(model, U"https://example.com/old\r\nfirst\r\nsecond");
	model.SetAlternateScreen(true);
	Print(model, U"http://localhost:3000/new");
	const auto link = terminal::DetectTerminalWebLink(model, { 0, 5 });
	ASSERT_TRUE(link);
	EXPECT_EQ(L"http://localhost:3000/new", link->uri);
}

TEST(TerminalLink, QuotesUnicodePunctuationAndCombiningTextKeepCellBoundaries)
{
	terminal::TerminalModel model(100, 2);
	Print(model, U"e\u0301: \u300chttps://example.com/path\u300d \"http://localhost:8000/\"");
	const auto link = terminal::DetectTerminalWebLink(model, { 0, 7 });
	ASSERT_TRUE(link);
	EXPECT_EQ(L"https://example.com/path", link->uri);
	EXPECT_FALSE(terminal::DetectTerminalWebLink(model, link->end));
	EXPECT_FALSE(terminal::DetectTerminalWebLink(model, { 0, 0 }));
}

class TerminalLinkWindow : public testing::Test {
protected:
	void SetUp() override
	{
		parent = ::CreateWindowExW(0, L"STATIC", L"Terminal link test", WS_OVERLAPPED,
			0, 0, 900, 500, nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
		ASSERT_NE(nullptr, parent);
		ASSERT_TRUE(window.Create(parent, ::GetModuleHandleW(nullptr)));
		window.Layout({ 0, 0, 800, 400 }, 96);
		window.SetResizeSink([this](terminal::TerminalSize size) { model.Resize(size.columns, size.rows); });
		Print(model, U"https://example.com/test");
		window.SetModel(&model);
		window.SetLinkOpener([this](std::wstring_view uri) { opened.emplace_back(uri); return openSucceeds; });
		window.SetInputSink([this](std::span<const std::uint8_t> bytes) {
			received.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
			return terminal::TerminalQueueInputResult::Accepted;
		});
	}
	void TearDown() override
	{
		window.Close();
		if( parent ) ::DestroyWindow(parent);
	}
	void Down( WPARAM keys = MK_CONTROL, int x = 7, int y = 7 )
	{
		::SendMessageW(window.GetHwnd(), WM_LBUTTONDOWN, keys | MK_LBUTTON, MAKELPARAM(x, y));
	}
	void Up( WPARAM keys = MK_CONTROL, int x = 7, int y = 7 )
	{
		::SendMessageW(window.GetHwnd(), WM_LBUTTONUP, keys, MAKELPARAM(x, y));
	}
	void MouseReporting()
	{
		model.SetMode(1000, true);
		model.SetMode(1006, true);
	}
	HWND parent{};
	terminal::TerminalModel model{ 80, 24 };
	terminal::CTerminalWnd window;
	std::vector<std::wstring> opened;
	std::string received;
	bool openSucceeds{ true };
};

TEST_F(TerminalLinkWindow, CtrlClickOpensOnReleaseExactlyOnceWithoutPtyMouseReports)
{
	MouseReporting();
	Down();
	EXPECT_TRUE(opened.empty());
	Up();
	ASSERT_EQ(1U, opened.size());
	EXPECT_EQ(L"https://example.com/test", opened.front());
	EXPECT_TRUE(received.empty());
	EXPECT_FALSE(window.HasSelection());
	EXPECT_NE(window.GetHwnd(), ::GetCapture());
}

TEST_F(TerminalLinkWindow, PlainDragStillSelectsAndPlainClickStillReportsToApplication)
{
	Down(0);
	Up(0, 80);
	EXPECT_TRUE(window.HasSelection());
	EXPECT_TRUE(opened.empty());
	MouseReporting();
	Down(0);
	Up(0);
	EXPECT_FALSE(received.empty());
	EXPECT_TRUE(opened.empty());
}

TEST_F(TerminalLinkWindow, DragAndReleasedControlCancelActivation)
{
	MouseReporting();
	Down();
	::SendMessageW(window.GetHwnd(), WM_MOUSEMOVE, MK_CONTROL | MK_LBUTTON, MAKELPARAM(80, 7));
	Up();
	EXPECT_TRUE(opened.empty());
	Down();
	Up(0);
	EXPECT_TRUE(opened.empty());
	EXPECT_TRUE(received.empty());
}

TEST_F(TerminalLinkWindow, CaptureLossAndCancellationConsumeThePendingRelease)
{
	MouseReporting();
	for( const auto message : { WM_CAPTURECHANGED, WM_CANCELMODE, WM_KILLFOCUS } ) {
		Down();
		::SendMessageW(window.GetHwnd(), message, 0, 0);
		Up();
		EXPECT_TRUE(opened.empty());
		EXPECT_TRUE(received.empty());
	}
}

TEST_F(TerminalLinkWindow, RebindingAndOutputPublicationCancelPendingActivation)
{
	MouseReporting();
	Down();
	window.ResetSessionInputState();
	Up();
	Down();
	window.SetModel(&model);
	Up();
	Down();
	static_cast<void>(window.NotifyFrameContent());
	Up();
	EXPECT_TRUE(opened.empty());
	EXPECT_TRUE(received.empty());
}

TEST_F(TerminalLinkWindow, PaddingAndReleaseOutsideLinkNeverActivate)
{
	Down(MK_CONTROL, 0, 0);
	Up(MK_CONTROL, 0, 0);
	Down();
	Up(MK_CONTROL, 750, 7);
	EXPECT_TRUE(opened.empty());
}

TEST_F(TerminalLinkWindow, FailedOpenerCompletesGestureAndDoesNotRetry)
{
	openSucceeds = false;
	MouseReporting();
	Down();
	Up();
	ASSERT_EQ(1U, opened.size());
	EXPECT_TRUE(received.empty());
	EXPECT_NE(window.GetHwnd(), ::GetCapture());
	openSucceeds = true;
	Down();
	Up();
	EXPECT_EQ(2U, opened.size());
}

TEST_F(TerminalLinkWindow, OpensWrappedContinuationThroughNativeGridCoordinates)
{
	model.Reset();
	model.Resize(12, window.GetTerminalSize().rows);
	Print(model, U"https://example.com/long/path");
	const int rowHeight = 390 / window.GetTerminalSize().rows;
	Down(MK_CONTROL, 7, 7 + rowHeight);
	Up(MK_CONTROL, 7, 7 + rowHeight);
	ASSERT_EQ(1U, opened.size());
	EXPECT_EQ(L"https://example.com/long/path", opened.front());
}

TEST_F(TerminalLinkWindow, OpensRetainedHistoryAfterScrollingToTheTop)
{
	for( std::size_t row = 0; row < model.RowCount() + 3; ++row ) Print(model, U"\r\noutput");
	ASSERT_GT(model.ScrollbackSize(), 0U);
	window.ApplyScrollbackChange(model.ConsumeScrollbackChange());
	for( int scroll = 0; scroll < 4; ++scroll ) {
		::SendMessageW(window.GetHwnd(), WM_MOUSEWHEEL, MAKEWPARAM(0, WHEEL_DELTA), 0);
	}
	ASSERT_EQ(0U, window.GetViewportDiagnostic().topRow);
	Down();
	Up();
	ASSERT_EQ(1U, opened.size());
	EXPECT_EQ(L"https://example.com/test", opened.front());
}

TEST_F(TerminalLinkWindow, ChangedTargetAndResizeCancelPendingActivation)
{
	MouseReporting();
	Down();
	model.SetCursorPosition(0, 0);
	Print(model, U"https://example.org/new!");
	Up();
	EXPECT_TRUE(opened.empty());
	Down();
	window.Layout({ 0, 0, 700, 350 }, 96);
	Up();
	EXPECT_TRUE(opened.empty());
	EXPECT_TRUE(received.empty());
}

TEST_F(TerminalLinkWindow, NewScrollbarGestureRetiresCancelledLinkOwnership)
{
	for( std::size_t row = 0; row < model.RowCount() + 3; ++row ) Print(model, U"\r\noutput");
	window.ApplyScrollbackChange(model.ConsumeScrollbackChange());
	for( int scroll = 0; scroll < 4; ++scroll ) {
		::SendMessageW(window.GetHwnd(), WM_MOUSEWHEEL, MAKEWPARAM(0, WHEEL_DELTA), 0);
	}
	Down();
	ASSERT_EQ(window.GetHwnd(), ::GetCapture());
	::SendMessageW(window.GetHwnd(), WM_CANCELMODE, 0, 0);
	// Simulate the cancelled click being released outside the viewport: there
	// is deliberately no mouse-up before the next scrollbar press.
	Down(0, 798, 30);
	ASSERT_EQ(window.GetHwnd(), ::GetCapture());
	::SendMessageW(window.GetHwnd(), WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(798, 300));
	EXPECT_GT(window.GetViewportDiagnostic().topRow, 0U);
	Up(0, 798, 300);
	EXPECT_NE(window.GetHwnd(), ::GetCapture());
	EXPECT_TRUE(opened.empty());
}

} // namespace

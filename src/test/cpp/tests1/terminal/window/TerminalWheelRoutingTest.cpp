/*! @file */
#include "pch.h"
#include "terminal/window/CTerminalWnd.h"
#include "terminal/input/SakuraTerminalInputAdapter.h"

#include <Windows.h>
#include <string>
#include <string_view>

namespace {

void Print( terminal::TerminalModel& model, std::u32string_view text )
{
	for( const auto ch : text ) {
		if( ch < 32 ) model.ExecuteControl(static_cast<wchar_t>(ch));
		else model.Print(ch);
	}
}

// Wheel routing acceptance for #445 step 1: with no mouse tracking and no
// scrollback the wheel must reach the application as cursor keys (xterm.js
// _handlePassiveWheel); with scrollback it scrolls the host viewport; with
// tracking it reports. The fixture drives a real CTerminalWnd through
// WM_MOUSEWHEEL and reads the bytes queued for the PTY.
class TerminalWheelRouting : public testing::Test {
protected:
	void SetUp() override
	{
		parent = ::CreateWindowExW(0, L"STATIC", L"Terminal wheel test", WS_OVERLAPPED,
			0, 0, 900, 500, nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
		ASSERT_NE(nullptr, parent);
		ASSERT_TRUE(window.Create(parent, ::GetModuleHandleW(nullptr)));
		window.Layout({ 0, 0, 800, 400 }, 96);
		window.SetResizeSink([this](terminal::TerminalSize size) { model.Resize(size.columns, size.rows); });
		window.SetModel(&model);
		window.SetInputAdapter(&inputAdapter);
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
	void Wheel( int delta, WPARAM keys = 0 )
	{
		::SendMessageW(window.GetHwnd(), WM_MOUSEWHEEL, MAKEWPARAM(keys, delta), 0);
	}
	void AlternateScreen()
	{
		model.SetAlternateScreen(true);
		inputAdapter.SetAlternateScreen(true);
	}
	// The conversion math assumes Chromium's 3-lines-per-notch default
	// (SPI_GETWHEELSCROLLLINES). Deterministic tests skip on machines whose
	// setting differs instead of depending on it. GTEST_SKIP must run in the
	// test body itself: inside a helper it would only return from the
	// helper and let the assertions run.
	unsigned long WheelLinesPerNotch()
	{
		unsigned long lines = 0;
		::SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
		return lines;
	}
	HWND parent{};
	terminal::TerminalModel model{ 80, 24 };
	terminal::SakuraTerminalInputAdapter inputAdapter;
	terminal::CTerminalWnd window;
	std::string received;
};

TEST_F(TerminalWheelRouting, AlternateScreenWheelUpSendsExactlyOneCursorUp)
{
	if( const auto linesPerNotch = WheelLinesPerNotch(); linesPerNotch != 3 ) {
		GTEST_SKIP() << "SPI_GETWHEELSCROLLLINES is " << linesPerNotch << ", expected 3";
	}
	AlternateScreen();
	Wheel(WHEEL_DELTA);
	EXPECT_EQ("\x1b[A", received);
	EXPECT_EQ(0U, window.GetViewportDiagnostic().topRow);
}

TEST_F(TerminalWheelRouting, AlternateScreenWheelDownSendsExactlyOneCursorDown)
{
	if( const auto linesPerNotch = WheelLinesPerNotch(); linesPerNotch != 3 ) {
		GTEST_SKIP() << "SPI_GETWHEELSCROLLLINES is " << linesPerNotch << ", expected 3";
	}
	AlternateScreen();
	Wheel(-WHEEL_DELTA);
	EXPECT_EQ("\x1b[B", received);
	EXPECT_EQ(0U, window.GetViewportDiagnostic().topRow);
}

TEST_F(TerminalWheelRouting, AlternateScreenApplicationCursorKeysSendSs3)
{
	if( const auto linesPerNotch = WheelLinesPerNotch(); linesPerNotch != 3 ) {
		GTEST_SKIP() << "SPI_GETWHEELSCROLLLINES is " << linesPerNotch << ", expected 3";
	}
	inputAdapter.SetMode(1, true);
	model.SetMode(1, true);
	AlternateScreen();
	Wheel(WHEEL_DELTA);
	EXPECT_EQ("\x1bOA", received);
}

TEST_F(TerminalWheelRouting, MultiNotchDeltaStillSendsOneCursorKey)
{
	if( const auto linesPerNotch = WheelLinesPerNotch(); linesPerNotch != 3 ) {
		GTEST_SKIP() << "SPI_GETWHEELSCROLLLINES is " << linesPerNotch << ", expected 3";
	}
	AlternateScreen();
	Wheel(3 * WHEEL_DELTA);
	EXPECT_EQ("\x1b[A", received);
}

TEST_F(TerminalWheelRouting, AlternateScrollDisableDoesNotGateConversion)
{
	if( const auto linesPerNotch = WheelLinesPerNotch(); linesPerNotch != 3 ) {
		GTEST_SKIP() << "SPI_GETWHEELSCROLLLINES is " << linesPerNotch << ", expected 3";
	}
	inputAdapter.SetMode(1007, false);
	model.SetMode(1007, false);
	AlternateScreen();
	Wheel(-WHEEL_DELTA);
	EXPECT_EQ("\x1b[B", received);
}

TEST_F(TerminalWheelRouting, ShiftWheelSendsNothingAndDoesNotScroll)
{
	AlternateScreen();
	Wheel(WHEEL_DELTA, MK_SHIFT);
	Wheel(-WHEEL_DELTA, MK_SHIFT);
	EXPECT_TRUE(received.empty());
	EXPECT_EQ(0U, window.GetViewportDiagnostic().topRow);
}

TEST_F(TerminalWheelRouting, SubNotchDeltasAccumulateAcrossMessages)
{
	if( const auto linesPerNotch = WheelLinesPerNotch(); linesPerNotch != 3 ) {
		GTEST_SKIP() << "SPI_GETWHEELSCROLLLINES is " << linesPerNotch << ", expected 3";
	}
	AlternateScreen();
	// One +40 sub-notch delta must not send a key yet: Chromium turns it into
	// ~33 px (at 3 lines/notch), and xterm.js damps sub-50-px deltas by 0.3,
	// so a whole row cannot accumulate from a single sub-notch message.
	Wheel(40);
	EXPECT_TRUE(received.empty());
	for( int index = 0; index < 5; ++index ) Wheel(40);
	// Accumulation must eventually emit, but six +40 messages are still far
	// short of one key per message: anything at all must be fewer than 6.
	EXPECT_FALSE(received.empty());
	std::size_t keys = 0;
	for( std::size_t at = 0; (at = received.find("\x1b[A", at)) != std::string::npos; ++at ) ++keys;
	EXPECT_GT(keys, 0U);
	EXPECT_LT(keys, 6U);
}

TEST_F(TerminalWheelRouting, MainScreenWithZeroScrollbackConvertsTheWheel)
{
	if( const auto linesPerNotch = WheelLinesPerNotch(); linesPerNotch != 3 ) {
		GTEST_SKIP() << "SPI_GETWHEELSCROLLLINES is " << linesPerNotch << ", expected 3";
	}
	model.SetScrollbackLimit(0);
	Wheel(WHEEL_DELTA);
	EXPECT_EQ("\x1b[A", received);
	EXPECT_EQ(0U, window.GetViewportDiagnostic().topRow);
}

// Regression guard: main screen with real scrollback keeps host scrolling.
TEST_F(TerminalWheelRouting, MainScreenWithScrollbackKeepsHostScrolling)
{
	for( std::size_t row = 0; row < model.RowCount() + 3; ++row ) Print(model, U"\r\noutput");
	ASSERT_GE(model.ScrollbackSize(), 3U);
	window.ApplyScrollbackChange(model.ConsumeScrollbackChange());
	const auto liveTop = window.GetViewportDiagnostic().topRow;
	Wheel(WHEEL_DELTA);
	EXPECT_TRUE(received.empty());
	EXPECT_EQ(liveTop - 3, window.GetViewportDiagnostic().topRow);
	Wheel(-WHEEL_DELTA);
	EXPECT_TRUE(received.empty());
	EXPECT_EQ(liveTop, window.GetViewportDiagnostic().topRow);
}

// The wheel unit must be a CSS-pixel row height, not the physical one:
// Chromium's DOM deltaY does not shrink with the display scale, so at 192 DPI
// (cell ~32 physical px, ~16 CSS px) two damped +40 deltas of 10/16 rows each
// must still produce one key.
TEST_F(TerminalWheelRouting, AlternateScreenAt192DpiAccumulatesOnCssRowHeight)
{
	if( const auto linesPerNotch = WheelLinesPerNotch(); linesPerNotch != 3 ) {
		GTEST_SKIP() << "SPI_GETWHEELSCROLLLINES is " << linesPerNotch << ", expected 3";
	}
	window.Layout({ 0, 0, 800, 400 }, 192);
	RecordProperty("paneRowsAt192Dpi", static_cast<int>(model.RowCount()));
	AlternateScreen();
	Wheel(40);
	EXPECT_TRUE(received.empty());
	Wheel(40);
	EXPECT_EQ("\x1b[A", received);
}

// The fractional-row remainder belongs to one terminal instance: a model
// rebind through SetModel must drop it (xterm.js keeps _wheelPartialScroll
// per instance).
TEST_F(TerminalWheelRouting, ModelRebindDropsTheFractionalRemainder)
{
	if( const auto linesPerNotch = WheelLinesPerNotch(); linesPerNotch != 3 ) {
		GTEST_SKIP() << "SPI_GETWHEELSCROLLLINES is " << linesPerNotch << ", expected 3";
	}
	AlternateScreen();
	Wheel(40);
	window.SetModel(nullptr);
	window.SetModel(&model);
	Wheel(40);
	EXPECT_TRUE(received.empty());
}

// Regression guard: tracking modes keep the report path on both screens.
TEST_F(TerminalWheelRouting, MouseTrackingKeepsTheReportPath)
{
	model.SetMode(1003, true);
	model.SetMode(1006, true);
	inputAdapter.SetMode(1003, true);
	inputAdapter.SetMode(1006, true);
	Wheel(WHEEL_DELTA);
	EXPECT_NE(std::string::npos, received.find("\x1b[<64;"));
	received.clear();
	AlternateScreen();
	Wheel(-WHEEL_DELTA);
	EXPECT_NE(std::string::npos, received.find("\x1b[<65;"));
}

} // namespace

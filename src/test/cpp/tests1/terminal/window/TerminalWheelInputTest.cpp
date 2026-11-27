/*! @file */
#include "pch.h"
#include "terminal/window/TerminalWheelInput.h"

#include <Windows.h>

namespace {

using terminal::TerminalWheelAccumulator;
using terminal::TerminalWheelScrollInput;
using terminal::TerminalConsumeWheel;

TerminalWheelScrollInput Input( int delta )
{
	TerminalWheelScrollInput input;
	input.delta = delta;
	input.linesPerNotch = 3;
	input.rowHeightPx = 16.0;
	input.bufferRows = 24;
	return input;
}

// Chromium -> DOM -> xterm.js: one notch = 3 lines * (100/3 px) = 100 px,
// 100/16 = 6.25 rows (web_input_event_builders_win.cc L299-335,
// MouseService.ts L467-480). Magnitude is the caller's concern; this unit
// returns the accumulated row count.
TEST(TerminalWheelInput, OneNotchAccumulatesItsRows)
{
	TerminalWheelAccumulator acc;
	EXPECT_EQ(6, TerminalConsumeWheel(acc, Input(120)));
	EXPECT_EQ(0, TerminalConsumeWheel(acc, Input(0)));
}

TEST(TerminalWheelInput, SignFollowsTheDelta)
{
	TerminalWheelAccumulator acc;
	EXPECT_EQ(-6, TerminalConsumeWheel(acc, Input(-120)));
}

TEST(TerminalWheelInput, ZeroDeltaAndShiftProduceNothing)
{
	TerminalWheelAccumulator acc;
	EXPECT_EQ(0, TerminalConsumeWheel(acc, Input(0)));
	auto shift = Input(120);
	shift.shift = true;
	EXPECT_EQ(0, TerminalConsumeWheel(acc, shift));
	EXPECT_DOUBLE_EQ(0.0, acc.partial);
}

TEST(TerminalWheelInput, SubNotchDeltasAccumulateFractionalRows)
{
	TerminalWheelAccumulator acc;
	// +40 -> (40/120)*3*(100/3) = 33.33 px < 50 -> damped *0.3 ->
	// 33.33/16*0.3 = 0.625 rows per message: partial runs
	// 0.625, 1.25, 0.875, 1.5, 1.125, 0.75.
	EXPECT_EQ(0, TerminalConsumeWheel(acc, Input(40)));
	EXPECT_EQ(1, TerminalConsumeWheel(acc, Input(40)));
	EXPECT_EQ(0, TerminalConsumeWheel(acc, Input(40)));
	EXPECT_EQ(1, TerminalConsumeWheel(acc, Input(40)));
	EXPECT_EQ(1, TerminalConsumeWheel(acc, Input(40)));
	EXPECT_EQ(0, TerminalConsumeWheel(acc, Input(40)));
	EXPECT_NEAR(0.75, acc.partial, 1e-9);
}

TEST(TerminalWheelInput, FractionalRemainderKeepsItsSign)
{
	TerminalWheelAccumulator acc;
	EXPECT_EQ(0, TerminalConsumeWheel(acc, Input(40)));
	EXPECT_EQ(1, TerminalConsumeWheel(acc, Input(40)));
	EXPECT_GT(acc.partial, 0.0);
	EXPECT_EQ(0, TerminalConsumeWheel(acc, Input(-40)));
	EXPECT_LT(acc.partial, 0.0);
}

TEST(TerminalWheelInput, FastModifierMultipliesRows)
{
	TerminalWheelAccumulator acc;
	auto fast = Input(120);
	fast.fastModifier = true;
	EXPECT_EQ(31, TerminalConsumeWheel(acc, fast)); // 100*5/16 = 31.25
}

TEST(TerminalWheelInput, PageModeReportsRowsWithoutAccumulating)
{
	TerminalWheelAccumulator acc;
	auto page = Input(120);
	page.linesPerNotch = WHEEL_PAGESCROLL;
	EXPECT_EQ(24, TerminalConsumeWheel(acc, page));
	EXPECT_DOUBLE_EQ(0.0, acc.partial);
}

TEST(TerminalWheelInput, PageModeKeepsSignForSubRowAmounts)
{
	// _handlePassiveWheel early-returns only when the page amount is exactly
	// 0, so a fractional page still sends a key with the delta's sign.
	TerminalWheelAccumulator acc;
	auto up = Input(1);
	up.linesPerNotch = WHEEL_PAGESCROLL;
	EXPECT_GT(TerminalConsumeWheel(acc, up), 0);
	auto down = Input(-1);
	down.linesPerNotch = WHEEL_PAGESCROLL;
	EXPECT_LT(TerminalConsumeWheel(acc, down), 0);
}

TEST(TerminalWheelInput, ZeroLinesPerNotchProducesNothing)
{
	TerminalWheelAccumulator acc;
	auto none = Input(120);
	none.linesPerNotch = 0;
	EXPECT_EQ(0, TerminalConsumeWheel(acc, none));
}

TEST(TerminalWheelInput, InvalidRowHeightProducesNothing)
{
	TerminalWheelAccumulator acc;
	auto bad = Input(120);
	bad.rowHeightPx = 0.0;
	EXPECT_EQ(0, TerminalConsumeWheel(acc, bad));
}

TEST(TerminalWheelInput, SensitivitySettingsScaleTheAmount)
{
	TerminalWheelAccumulator acc;
	auto scaled = Input(120);
	scaled.scrollSensitivity = 2.0;
	EXPECT_EQ(12, TerminalConsumeWheel(acc, scaled)); // 100*2/16 = 12.5
}

} // namespace

/*! @file */
#include "pch.h"
#include "terminal/input/TerminalShortcutPreset.h"

#include <Windows.h>

namespace {

using terminal::ResolveTerminalPresetKey;
using terminal::TerminalPresetAction;
using terminal::TerminalPresetKey;
using terminal::TerminalShortcutPreset;

TerminalPresetKey Key( std::uint32_t virtualKey, bool shift = false, bool control = false, bool alt = false )
{
	TerminalPresetKey key;
	key.virtualKey = virtualKey;
	key.shift = shift;
	key.control = control;
	key.alt = alt;
	return key;
}

TEST(TerminalShortcutPreset, DisabledPresetNeverClaimsAKey)
{
	const auto prefix = ResolveTerminalPresetKey(TerminalShortcutPreset::None, false, Key('B', false, true));
	EXPECT_FALSE(prefix.consumed);
	EXPECT_FALSE(prefix.prefixArmed);
	EXPECT_EQ(TerminalPresetAction::None, prefix.action);
}

TEST(TerminalShortcutPreset, TmuxArmsOnControlBAndSplitsOnPercent)
{
	const auto armed = ResolveTerminalPresetKey(TerminalShortcutPreset::Tmux, false, Key('B', false, true));
	ASSERT_TRUE(armed.consumed);
	ASSERT_TRUE(armed.prefixArmed);
	EXPECT_EQ(TerminalPresetAction::None, armed.action);

	const auto split = ResolveTerminalPresetKey(TerminalShortcutPreset::Tmux, true, Key('5', true));
	EXPECT_TRUE(split.consumed);
	EXPECT_FALSE(split.prefixArmed);
	EXPECT_EQ(TerminalPresetAction::SplitRight, split.action);
}

TEST(TerminalShortcutPreset, TmuxDoubleQuoteSplitsDownOnBothUsAndJisLayouts)
{
	for( const auto virtualKey : { static_cast<std::uint32_t>(VK_OEM_7), static_cast<std::uint32_t>('2') } ) {
		const auto resolution = ResolveTerminalPresetKey(TerminalShortcutPreset::Tmux, true, Key(virtualKey, true));
		EXPECT_TRUE(resolution.consumed);
		EXPECT_EQ(TerminalPresetAction::SplitDown, resolution.action);
	}
}

TEST(TerminalShortcutPreset, TmuxChordTableCoversThePanelActions)
{
	const struct { std::uint32_t virtualKey; bool shift; TerminalPresetAction action; } cases[]{
		{ 'C', false, TerminalPresetAction::NewTerminal },
		{ 'X', false, TerminalPresetAction::ClosePane },
		{ '7', true, TerminalPresetAction::CloseTerminal },
		{ 'O', false, TerminalPresetAction::NextPane },
		{ VK_OEM_1, false, TerminalPresetAction::PreviousPane },
		{ 'N', false, TerminalPresetAction::NextTerminal },
		{ 'P', false, TerminalPresetAction::PreviousTerminal },
		{ 'W', false, TerminalPresetAction::FocusTerminalList },
	};
	for( const auto& testCase : cases ) {
		const auto resolution = ResolveTerminalPresetKey(
			TerminalShortcutPreset::Tmux, true, Key(testCase.virtualKey, testCase.shift));
		EXPECT_TRUE(resolution.consumed);
		EXPECT_FALSE(resolution.prefixArmed);
		EXPECT_EQ(testCase.action, resolution.action);
	}
}

TEST(TerminalShortcutPreset, DigitSelectsTheTerminalGroupByIndex)
{
	const auto typed = ResolveTerminalPresetKey(TerminalShortcutPreset::Tmux, true, Key('3'));
	EXPECT_EQ(TerminalPresetAction::SelectTerminal, typed.action);
	EXPECT_EQ(std::size_t{ 3 }, typed.terminalIndex);

	const auto numpad = ResolveTerminalPresetKey(TerminalShortcutPreset::Screen, true, Key(VK_NUMPAD1));
	EXPECT_EQ(TerminalPresetAction::SelectTerminal, numpad.action);
	EXPECT_EQ(std::size_t{ 1 }, numpad.terminalIndex);
}

TEST(TerminalShortcutPreset, ScreenUsesItsOwnPrefixAndRegionChords)
{
	EXPECT_FALSE(ResolveTerminalPresetKey(TerminalShortcutPreset::Screen, false, Key('B', false, true)).consumed);
	const auto armed = ResolveTerminalPresetKey(TerminalShortcutPreset::Screen, false, Key('A', false, true));
	ASSERT_TRUE(armed.prefixArmed);

	const struct { std::uint32_t virtualKey; bool shift; TerminalPresetAction action; } cases[]{
		{ VK_OEM_5, true, TerminalPresetAction::SplitRight },
		{ 'S', true, TerminalPresetAction::SplitDown },
		{ 'C', false, TerminalPresetAction::NewTerminal },
		{ 'X', true, TerminalPresetAction::ClosePane },
		{ 'K', false, TerminalPresetAction::CloseTerminal },
		{ VK_TAB, false, TerminalPresetAction::NextPane },
		{ VK_OEM_7, true, TerminalPresetAction::FocusTerminalList },
	};
	for( const auto& testCase : cases ) {
		const auto resolution = ResolveTerminalPresetKey(
			TerminalShortcutPreset::Screen, true, Key(testCase.virtualKey, testCase.shift));
		EXPECT_TRUE(resolution.consumed);
		EXPECT_EQ(testCase.action, resolution.action);
	}
}

TEST(TerminalShortcutPreset, PressingThePrefixTwiceSendsTheLiteralControlByte)
{
	// send-prefix must reach the shell, otherwise a real tmux running inside this
	// terminal could never receive its own prefix key.
	const auto resolution = ResolveTerminalPresetKey(TerminalShortcutPreset::Tmux, true, Key('B', false, true));
	EXPECT_FALSE(resolution.consumed);
	EXPECT_FALSE(resolution.prefixArmed);
	EXPECT_EQ(TerminalPresetAction::None, resolution.action);
}

TEST(TerminalShortcutPreset, ArmedPrefixSurvivesAHeldModifierAndIsCancelledByEscape)
{
	const auto shiftDown = ResolveTerminalPresetKey(TerminalShortcutPreset::Tmux, true, Key(VK_SHIFT, true));
	EXPECT_FALSE(shiftDown.consumed);
	EXPECT_TRUE(shiftDown.prefixArmed);

	const auto escape = ResolveTerminalPresetKey(TerminalShortcutPreset::Tmux, true, Key(VK_ESCAPE));
	EXPECT_TRUE(escape.consumed);
	EXPECT_FALSE(escape.prefixArmed);
	EXPECT_EQ(TerminalPresetAction::None, escape.action);
}

TEST(TerminalShortcutPreset, UnboundChordIsSwallowedInsteadOfLeakingIntoTheShell)
{
	const auto resolution = ResolveTerminalPresetKey(TerminalShortcutPreset::Tmux, true, Key('Q'));
	EXPECT_TRUE(resolution.consumed);
	EXPECT_FALSE(resolution.prefixArmed);
	EXPECT_EQ(TerminalPresetAction::None, resolution.action);
}

TEST(TerminalShortcutPreset, UnarmedKeysAreNeverClaimed)
{
	EXPECT_FALSE(ResolveTerminalPresetKey(TerminalShortcutPreset::Tmux, false, Key('5', true)).consumed);
	EXPECT_FALSE(ResolveTerminalPresetKey(TerminalShortcutPreset::Tmux, false, Key('C', false, true)).consumed);
	EXPECT_FALSE(ResolveTerminalPresetKey(TerminalShortcutPreset::Screen, false, Key('A')).consumed);
}

TEST(TerminalShortcutPreset, IdentifiersRoundTripAndRejectUnknownValues)
{
	for( const auto preset : { TerminalShortcutPreset::None, TerminalShortcutPreset::Tmux, TerminalShortcutPreset::Screen } ) {
		EXPECT_EQ(preset, terminal::ParseTerminalShortcutPreset(terminal::TerminalShortcutPresetId(preset)));
	}
	EXPECT_FALSE(terminal::ParseTerminalShortcutPreset(L"Tmux").has_value());
	EXPECT_FALSE(terminal::ParseTerminalShortcutPreset(L"").has_value());
}

} // namespace

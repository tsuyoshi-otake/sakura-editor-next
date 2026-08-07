/*! @file */
#include "pch.h"
#include "terminal/input/SakuraTerminalInputAdapter.h"
#include "terminal/model/TerminalModel.h"
#include "terminal/parser/TerminalParser.h"
#include "terminal/window/TerminalInput.h"

#include <Windows.h>

namespace {

TEST(TerminalInput, EncodesSplitSurrogateTextAsUtf8)
{
	const std::wstring text = L"\u65e5\U0001f4bb";
	EXPECT_EQ(std::string("\xe6\x97\xa5\xf0\x9f\x92\xbb", 7), terminal::EncodeTerminalText(text));
}

TEST(TerminalInput, WrapsPasteOnlyWhenBracketedPasteIsEnabled)
{
	EXPECT_EQ("abc", terminal::EncodeTerminalPaste(L"abc", false));
	EXPECT_EQ("\x1b[200~abc\x1b[201~", terminal::EncodeTerminalPaste(L"abc", true));
}

TEST(TerminalInput, EncodesNavigationModifiersAndControlKeys)
{
	EXPECT_EQ("\x1b[A", terminal::EncodeTerminalKey({ VK_UP }));
	EXPECT_EQ("\x1b[1;6D", terminal::EncodeTerminalKey({ VK_LEFT, true, true, false }));
	EXPECT_EQ(std::string("\x03", 1), terminal::EncodeTerminalKey({ 'C', false, true, false }));
	EXPECT_EQ(std::string("\x1b\x03", 2), terminal::EncodeTerminalKey({ 'C', false, true, true }));
}

TEST(TerminalInput, ClaimsPlainTextKeysBeforeTheHostKeybindingTable)
{
	// Space も Shift+Space も端末側の文字入力。既定のキー割り当てが
	// F_INDENT_SPACE / F_UNINDENT_SPACE を持つため、ここで引き取らないと
	// アクセラレータに奪われて WM_CHAR が生成されない。
	EXPECT_TRUE(terminal::TerminalKeyNeedsTextDelivery({ VK_SPACE }, true));
	EXPECT_TRUE(terminal::TerminalKeyNeedsTextDelivery({ VK_SPACE, true }, true));
	EXPECT_TRUE(terminal::TerminalKeyNeedsTextDelivery({ 'A' }, true));

	// Ctrl / Alt 付きは制御シーケンス側の所有。文字を持たないキーも対象外。
	EXPECT_FALSE(terminal::TerminalKeyNeedsTextDelivery({ VK_SPACE, false, true, false }, true));
	EXPECT_FALSE(terminal::TerminalKeyNeedsTextDelivery({ VK_SPACE, false, false, true }, true));
	EXPECT_FALSE(terminal::TerminalKeyNeedsTextDelivery({ VK_APPS }, false));

	terminal::TerminalKeyEvent keyUp{ VK_SPACE };
	keyUp.keyDown = false;
	EXPECT_FALSE(terminal::TerminalKeyNeedsTextDelivery(keyUp, true));
}

TEST(TerminalInput, ResolvesTerminalClipboardAndInterruptShortcuts)
{
	EXPECT_EQ(terminal::TerminalShortcutAction::Copy,
		terminal::ResolveTerminalShortcut({ 'C', false, true, false }, true));
	EXPECT_EQ(terminal::TerminalShortcutAction::SendInterrupt,
		terminal::ResolveTerminalShortcut({ 'C', false, true, false }, false));
	EXPECT_EQ(terminal::TerminalShortcutAction::Copy,
		terminal::ResolveTerminalShortcut({ 'C', true, true, false }, false));
	EXPECT_EQ(terminal::TerminalShortcutAction::Paste,
		terminal::ResolveTerminalShortcut({ 'V', true, true, false }, false));
	EXPECT_EQ(terminal::TerminalShortcutAction::Paste,
		terminal::ResolveTerminalShortcut({ VK_INSERT, true, false, false }, false));
	EXPECT_EQ(terminal::TerminalShortcutAction::Copy,
		terminal::ResolveTerminalShortcut({ VK_INSERT, false, true, false }, true));
}

TEST(TerminalInput, ResolvesRightClickClipboardBehavior)
{
	EXPECT_EQ(terminal::TerminalRightClickAction::CopySelection,
		terminal::ResolveTerminalRightClick(true, false, false));
	EXPECT_EQ(terminal::TerminalRightClickAction::PasteClipboard,
		terminal::ResolveTerminalRightClick(false, false, false));
	EXPECT_EQ(terminal::TerminalRightClickAction::SendToApplication,
		terminal::ResolveTerminalRightClick(true, true, false));
	EXPECT_EQ(terminal::TerminalRightClickAction::CopySelection,
		terminal::ResolveTerminalRightClick(true, true, true));
	EXPECT_EQ(terminal::TerminalRightClickAction::PasteClipboard,
		terminal::ResolveTerminalRightClick(false, true, true));
}

TEST(TerminalInput, EncodesSgrMouseAndHonorsTrackingMode)
{
	terminal::TerminalModes modes;
	terminal::TerminalMouseEvent event{ terminal::TerminalMouseAction::Press, 0, 4, 2 };
	EXPECT_TRUE(terminal::EncodeTerminalMouse(event, modes).empty());
	modes.mouseButtonTracking = true;
	modes.mouseSgrEncoding = true;
	EXPECT_EQ("\x1b[<0;5;3M", terminal::EncodeTerminalMouse(event, modes));
	event.action = terminal::TerminalMouseAction::Release;
	EXPECT_EQ("\x1b[<3;5;3m", terminal::EncodeTerminalMouse(event, modes));
}

TEST(TerminalInput, AdapterSwitchesBetweenNormalAndApplicationCursorKeys)
{
	terminal::SakuraTerminalInputAdapter input;
	const terminal::TerminalKeyEvent up{ VK_UP };

	auto output = input.EncodeKey(up);
	ASSERT_TRUE(output.has_value());
	EXPECT_EQ("\x1b[A", *output);

	input.SetMode(1, true);
	output = input.EncodeKey(up);
	ASSERT_TRUE(output.has_value());
	EXPECT_EQ("\x1bOA", *output);

	input.SetMode(1, false);
	output = input.EncodeKey(up);
	ASSERT_TRUE(output.has_value());
	EXPECT_EQ("\x1b[A", *output);
}

TEST(TerminalInput, AdapterFocusModeDistinguishesUnhandledFromEncodedEvents)
{
	terminal::SakuraTerminalInputAdapter input;
	EXPECT_FALSE(input.EncodeFocus(true).has_value());

	input.SetMode(1004, true);
	auto focused = input.EncodeFocus(true);
	ASSERT_TRUE(focused.has_value());
	EXPECT_EQ("\x1b[I", *focused);
	auto unfocused = input.EncodeFocus(false);
	ASSERT_TRUE(unfocused.has_value());
	EXPECT_EQ("\x1b[O", *unfocused);

	input.SetMode(1004, false);
	EXPECT_FALSE(input.EncodeFocus(false).has_value());
}

TEST(TerminalInput, AdapterEncodesSgrMouseOnlyWhenTrackingAndSgrModesAreEnabled)
{
	terminal::SakuraTerminalInputAdapter input;
	terminal::TerminalMouseEvent event{ terminal::TerminalMouseAction::Press, 0, 4, 2 };
	EXPECT_FALSE(input.EncodeMouse(event).has_value());

	input.SetMode(1000, true);
	input.SetMode(1006, true);
	auto press = input.EncodeMouse(event);
	ASSERT_TRUE(press.has_value());
	EXPECT_EQ("\x1b[<0;5;3M", *press);

	event.action = terminal::TerminalMouseAction::Release;
	auto release = input.EncodeMouse(event);
	ASSERT_TRUE(release.has_value());
	EXPECT_EQ("\x1b[<0;5;3m", *release);
}

TEST(TerminalInput, AdapterReturnsAnEmptyValueForSuppressedKeyUp)
{
	terminal::SakuraTerminalInputAdapter input;
	terminal::TerminalKeyEvent keyUp{ VK_UP };
	keyUp.keyDown = false;

	auto output = input.EncodeKey(keyUp);
	ASSERT_TRUE(output.has_value());
	EXPECT_TRUE(output->empty());
}

TEST(TerminalInput, AdapterResetClearsInputModes)
{
	terminal::SakuraTerminalInputAdapter input;
	const terminal::TerminalKeyEvent up{ VK_UP };
	const terminal::TerminalMouseEvent mouse{ terminal::TerminalMouseAction::Press, 0, 4, 2 };
	input.SetMode(1, true);
	input.SetMode(1000, true);
	input.SetMode(1004, true);
	input.SetMode(1006, true);

	input.Reset();
	auto cursor = input.EncodeKey(up);
	ASSERT_TRUE(cursor.has_value());
	EXPECT_EQ("\x1b[A", *cursor);
	EXPECT_FALSE(input.EncodeFocus(true).has_value());
	EXPECT_FALSE(input.EncodeMouse(mouse).has_value());
}

TEST(TerminalInput, AdapterNeverEnablesWin32InputForRemoteMode9001)
{
	terminal::SakuraTerminalInputAdapter input;
	input.SetMode(9001, true);
	const terminal::TerminalKeyEvent character{ 'A', false, false, false, 0, 1, L'a' };

	auto output = input.EncodeKey(character);
	ASSERT_TRUE(output.has_value());
	EXPECT_EQ("a", *output);
	EXPECT_NE("\x1b[65;0;97;1;0;1_", *output);
}

TEST(TerminalInput, ParserSynchronizesPrivateCsiModesToInputAdapter)
{
	terminal::TerminalModel model(10, 1);
	terminal::SakuraTerminalInputAdapter input;
	terminal::TerminalParser parser(model, &input);
	parser.Feed("\x1b[?1h\x1b[?1000h\x1b[?1004h\x1b[?1006h");

	auto cursor = input.EncodeKey({ VK_UP });
	ASSERT_TRUE(cursor.has_value());
	EXPECT_EQ("\x1bOA", *cursor);
	auto focus = input.EncodeFocus(true);
	ASSERT_TRUE(focus.has_value());
	EXPECT_EQ("\x1b[I", *focus);
	auto mouse = input.EncodeMouse({ terminal::TerminalMouseAction::Press, 0, 4, 2 });
	ASSERT_TRUE(mouse.has_value());
	EXPECT_EQ("\x1b[<0;5;3M", *mouse);

	parser.Feed("\x1b[?1l\x1b[?1000l\x1b[?1004l\x1b[?1006l");
	cursor = input.EncodeKey({ VK_UP });
	ASSERT_TRUE(cursor.has_value());
	EXPECT_EQ("\x1b[A", *cursor);
	EXPECT_FALSE(input.EncodeFocus(true).has_value());
	EXPECT_FALSE(input.EncodeMouse({ terminal::TerminalMouseAction::Press, 0, 4, 2 }).has_value());
}

} // namespace

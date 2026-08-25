/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include <gtest/gtest.h>

#include "workbench/quickinput/CCommandPaletteOverlay.h"

#include <string>
#include <vector>

namespace {

constexpr wchar_t kOverlayClassName[] = L"SakuraEditor.Next.CommandPaletteOverlay";
constexpr wchar_t kScrollbarClassName[] = L"SakuraWorkbenchOverlayScrollbar";
constexpr int kInputControl = 100;
constexpr int kListControl = 101;
constexpr int kCloseControl = 102;

HWND CreateTestParent()
{
	return ::CreateWindowExW(0, L"STATIC", L"Quick Input test parent",
		WS_OVERLAPPEDWINDOW | WS_VISIBLE,
		0, 0, 900, 700, nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
}

std::vector<workbench::quickinput::CommandPaletteItem> TestItems()
{
	return {
		{ .id = L"dark", .label = L"Dark", .detail = L"Theme", .enabled = true },
		{ .id = L"light", .label = L"Light", .detail = L"Theme", .enabled = true },
		{ .id = L"disabled", .label = L"Disabled", .detail = L"Theme", .enabled = false },
	};
}

std::vector<workbench::quickinput::CommandPaletteItem> ManyTestItems()
{
	std::vector<workbench::quickinput::CommandPaletteItem> items;
	for (int index = 0; index < 20; ++index) {
		const auto suffix = std::to_wstring(index);
		items.push_back({ .id = L"item-" + suffix,
			.label = L"Command " + suffix, .detail = L"Sakura Editor", .enabled = true });
	}
	return items;
}

std::vector<workbench::quickinput::CommandPaletteItem> MixedHeightTestItems()
{
	std::vector<workbench::quickinput::CommandPaletteItem> items;
	for (int index = 0; index < 30; ++index) {
		const auto suffix = std::to_wstring(index);
		items.push_back({ .id = L"mixed-" + suffix,
			.label = L"Command " + suffix,
			.detail = index % 2 == 0 ? L"Two-line detail" : L"",
			.enabled = true });
	}
	return items;
}

TEST(CommandPaletteOverlayPureTest, ProviderPrefixIsStrippedOnlyAtTheInputStart)
{
	using workbench::quickinput::StripCommandPaletteProviderPrefix;
	EXPECT_EQ(std::wstring_view(L"dark"),
		StripCommandPaletteProviderPrefix(std::wstring_view(L">dark")));
	EXPECT_EQ(std::wstring_view(L"dark"),
		StripCommandPaletteProviderPrefix(std::wstring_view(L"dark")));
	EXPECT_EQ(std::wstring_view(L">dark"),
		StripCommandPaletteProviderPrefix(std::wstring_view(L">>dark")));
	EXPECT_TRUE(StripCommandPaletteProviderPrefix({}).empty());
}

TEST(CommandPaletteOverlayPureTest, GeometryStaysInsideNarrowClientsAtSupportedDpi)
{
	using namespace workbench::quickinput;
	for (const int dpi : { 96, 120, 144, 192 }) {
		const int parentWidth = ScaleQuickInputDip(240, dpi);
		const int parentHeight = ScaleQuickInputDip(420, dpi);
		const auto contentHeight = ScaleQuickInputDip(44, dpi);
		const auto palette = ComputeQuickInputLayout(
			parentWidth, parentHeight, dpi, contentHeight, false);
		EXPECT_GE(palette.x, 0);
		EXPECT_GE(palette.y, 0);
		EXPECT_LE(palette.x + palette.width, parentWidth);
		EXPECT_LE(palette.y + palette.height, parentHeight);
		EXPECT_EQ(ScaleQuickInputDip(6, dpi) + ScaleQuickInputDip(26, dpi)
			+ ScaleQuickInputDip(4, dpi), palette.headerHeight);
		EXPECT_EQ(palette.headerHeight, palette.listTop);
		EXPECT_EQ(ScaleQuickInputDip(7, dpi) + contentHeight, palette.listHeight);

		const auto input = ComputeQuickInputLayout(
			parentWidth, parentHeight, dpi, contentHeight, true);
		EXPECT_EQ(0, input.listHeight);
		EXPECT_EQ(input.headerHeight, input.height);
		EXPECT_LE(input.x + input.width, parentWidth);
	}
}

TEST(CommandPaletteOverlayPureTest, SingleLineEditorIsCenteredInsideThePaintedInputFrame)
{
	using namespace workbench::quickinput;
	for (const int dpi : { 96, 120, 144, 192 }) {
		const auto geometry = ComputeQuickInputRowGeometry(
			ScaleQuickInputDip(6, dpi), ScaleQuickInputDip(6, dpi),
			ScaleQuickInputDip(300, dpi), dpi);
		EXPECT_EQ(ScaleQuickInputDip(26, dpi),
			geometry.frame.bottom - geometry.frame.top);
		EXPECT_EQ(ScaleQuickInputDip(3, dpi),
			geometry.editor.top - geometry.frame.top);
		EXPECT_EQ(geometry.editor.top - geometry.frame.top,
			geometry.frame.bottom - geometry.editor.bottom);
		EXPECT_EQ(ScaleQuickInputDip(1, dpi),
			geometry.editor.left - geometry.frame.left);
		EXPECT_EQ(geometry.editor.left - geometry.frame.left,
			geometry.frame.right - geometry.editor.right);
	}
}

TEST(CommandPaletteOverlayPureTest, MousePressesDismissOnlyFromOutsideQuickInput)
{
	using namespace workbench::quickinput;
	EXPECT_TRUE(IsQuickInputDismissMouseMessage(WM_LBUTTONDOWN));
	EXPECT_TRUE(IsQuickInputDismissMouseMessage(WM_NCRBUTTONDOWN));
	EXPECT_FALSE(IsQuickInputDismissMouseMessage(WM_LBUTTONUP));
	EXPECT_FALSE(IsQuickInputDismissMouseMessage(WM_MOUSEMOVE));
}

class CommandPaletteOverlayTest : public testing::Test {
protected:
	void SetUp() override
	{
		m_parent = CreateTestParent();
		ASSERT_NE(nullptr, m_parent);
		ASSERT_TRUE(m_overlay.Create(m_parent));
	}

	void TearDown() override
	{
		m_overlay.Destroy();
		if (m_parent != nullptr) ::DestroyWindow(m_parent);
	}

	HWND OverlayWindow() const
	{
		return ::FindWindowExW(m_parent, nullptr, kOverlayClassName, nullptr);
	}

	HWND InputWindow() const { return ::GetDlgItem(OverlayWindow(), kInputControl); }
	HWND CloseWindow() const { return ::GetDlgItem(OverlayWindow(), kCloseControl); }
	HWND PromptWindow() const
	{
		return ::FindWindowExW(OverlayWindow(), nullptr, L"STATIC", L"Branch name");
	}
	HWND ListWindow() const { return ::GetDlgItem(OverlayWindow(), kListControl); }
	HWND ScrollbarWindow() const
	{
		return ::FindWindowExW(OverlayWindow(), nullptr, kScrollbarClassName, nullptr);
	}

	HWND m_parent{};
	workbench::quickinput::CCommandPaletteOverlay m_overlay;
};

TEST_F(CommandPaletteOverlayTest, PreviewsInitialKeyboardMouseAndFilteredSelections)
{
	std::vector<std::wstring> selected;
	m_overlay.SetSelectionCallback([&selected](std::wstring id) {
		selected.push_back(std::move(id));
	});
	m_overlay.SetSearchCallback([](std::wstring_view query) {
		if (query == L"dark") {
			return std::vector<workbench::quickinput::CommandPaletteItem>{
				{ .id = L"dark", .label = L"Dark", .detail = L"Theme", .enabled = true },
			};
		}
		return TestItems();
	});

	ASSERT_TRUE(m_overlay.Show(TestItems(), L"light"));
	ASSERT_EQ((std::vector<std::wstring>{ L"light" }), selected);

	// PreTranslateMessage reads the thread keyboard state just like the real
	// message loop. Isolate this test from modifier keys synthesized by earlier
	// native UI suites, then restore the exact state before making assertions.
	BYTE originalKeyboardState[256]{};
	ASSERT_TRUE(::GetKeyboardState(originalKeyboardState));
	BYTE neutralKeyboardState[256]{};
	for (std::size_t index = 0; index < 256; ++index) {
		neutralKeyboardState[index] = originalKeyboardState[index];
	}
	neutralKeyboardState[VK_CONTROL] = 0;
	neutralKeyboardState[VK_SHIFT] = 0;
	neutralKeyboardState[VK_MENU] = 0;
	ASSERT_TRUE(::SetKeyboardState(neutralKeyboardState));
	MSG down{ InputWindow(), WM_KEYDOWN, VK_DOWN, 0 };
	const bool handled = m_overlay.PreTranslateMessage(down);
	EXPECT_TRUE(::SetKeyboardState(originalKeyboardState));
	EXPECT_TRUE(handled);
	// The disabled third row is skipped and selection wraps to the first row.
	ASSERT_EQ((std::vector<std::wstring>{ L"light", L"dark" }), selected);

	ASSERT_NE(nullptr, ListWindow());
	::SendMessageW(ListWindow(), LB_SETCURSEL, 1, 0);
	::SendMessageW(OverlayWindow(), WM_COMMAND,
		MAKEWPARAM(kListControl, LBN_SELCHANGE), reinterpret_cast<LPARAM>(ListWindow()));
	ASSERT_EQ((std::vector<std::wstring>{ L"light", L"dark", L"light" }), selected);

	ASSERT_NE(nullptr, InputWindow());
	::SetWindowTextW(InputWindow(), L"dark");
	EXPECT_EQ((std::vector<std::wstring>{ L"light", L"dark", L"light", L"dark" }), selected);
}

TEST_F(CommandPaletteOverlayTest, CommandPaletteKeepsProviderPrefixInInputAndFiltersWithoutIt)
{
	std::vector<std::wstring> queries;
	m_overlay.SetSearchCallback([&queries](std::wstring_view query) {
		queries.emplace_back(query);
		return TestItems();
	});
	ASSERT_TRUE(m_overlay.Show(TestItems(), L"dark"));

	wchar_t value[32]{};
	ASSERT_GT(::GetWindowTextW(InputWindow(), value, static_cast<int>(std::size(value))), 0);
	EXPECT_EQ(L">", std::wstring(value));
	EXPECT_FALSE(::IsWindowVisible(PromptWindow()));
	EXPECT_FALSE(::IsWindowVisible(CloseWindow()));

	::SetWindowTextW(InputWindow(), L">dark");
	ASSERT_FALSE(queries.empty());
	EXPECT_EQ(L"dark", queries.back());
	// A direct native edit replacement is normalized back to a provider value,
	// while the callback still receives only the user query.
	::SetWindowTextW(InputWindow(), L"light");
	EXPECT_EQ(L"light", queries.back());
	ASSERT_GT(::GetWindowTextW(InputWindow(), value, static_cast<int>(std::size(value))), 0);
	EXPECT_EQ(L">light", std::wstring(value));
}

TEST_F(CommandPaletteOverlayTest, AcceptAndCancelHaveDistinctTerminalCallbacks)
{
	std::vector<std::wstring> accepted;
	int cancelled = 0;
	m_overlay.SetAcceptCallback([&accepted](std::wstring id) {
		accepted.push_back(std::move(id));
	});
	m_overlay.SetCancelCallback([&cancelled] { ++cancelled; });

	ASSERT_TRUE(m_overlay.Show(TestItems(), L"light"));
	MSG accept{ InputWindow(), WM_KEYDOWN, VK_RETURN, 0 };
	EXPECT_TRUE(m_overlay.PreTranslateMessage(accept));
	EXPECT_EQ((std::vector<std::wstring>{ L"light" }), accepted);
	EXPECT_EQ(0, cancelled);
	EXPECT_FALSE(m_overlay.IsVisible());

	ASSERT_TRUE(m_overlay.Show(TestItems(), L"dark"));
	MSG cancel{ InputWindow(), WM_KEYDOWN, VK_ESCAPE, 0 };
	EXPECT_TRUE(m_overlay.PreTranslateMessage(cancel));
	EXPECT_EQ((std::vector<std::wstring>{ L"light" }), accepted);
	EXPECT_EQ(1, cancelled);
	EXPECT_FALSE(m_overlay.IsVisible());
}

TEST_F(CommandPaletteOverlayTest, OutsideClickCancelsButStillDispatchesToItsWorkbenchTarget)
{
	int cancelled = 0;
	m_overlay.SetCancelCallback([&cancelled] { ++cancelled; });
	ASSERT_TRUE(m_overlay.Show(TestItems(), L"light"));

	MSG inside{ InputWindow(), WM_LBUTTONDOWN, MK_LBUTTON, 0 };
	(void)m_overlay.PreTranslateMessage(inside);
	EXPECT_TRUE(m_overlay.IsVisible());
	EXPECT_EQ(0, cancelled);

	MSG outside{ m_parent, WM_LBUTTONDOWN, MK_LBUTTON, 0 };
	EXPECT_FALSE(m_overlay.PreTranslateMessage(outside));
	EXPECT_FALSE(m_overlay.IsVisible());
	EXPECT_EQ(1, cancelled);
}

TEST_F(CommandPaletteOverlayTest, MouseClickAcceptsSelectedQuickPickItem)
{
	std::vector<std::wstring> accepted;
	m_overlay.SetAcceptCallback([&accepted](std::wstring id) {
		accepted.push_back(std::move(id));
	});
	ASSERT_TRUE(m_overlay.Show(TestItems(), L"dark"));

	RECT item{};
	ASSERT_NE(LB_ERR, ::SendMessageW(ListWindow(), LB_GETITEMRECT, 1,
		reinterpret_cast<LPARAM>(&item)));
	const int x = item.left + (item.right - item.left) / 2;
	const int y = item.top + (item.bottom - item.top) / 2;
	::SendMessageW(ListWindow(), WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, y));
	::SendMessageW(ListWindow(), WM_LBUTTONUP, 0, MAKELPARAM(x, y));

	EXPECT_EQ((std::vector<std::wstring>{ L"light" }), accepted);
	EXPECT_FALSE(m_overlay.IsVisible());
}

TEST_F(CommandPaletteOverlayTest, MissingSelectionCallbackKeepsCommandPaletteSideEffectFree)
{
	m_overlay.SetSelectionCallback({});
	ASSERT_TRUE(m_overlay.Show(TestItems(), L"dark"));

	MSG down{ InputWindow(), WM_KEYDOWN, VK_DOWN, 0 };
	EXPECT_TRUE(m_overlay.PreTranslateMessage(down));
	EXPECT_TRUE(m_overlay.IsVisible());
}

TEST_F(CommandPaletteOverlayTest, UsesOneLineInputAndSharedOverlayScrollbar)
{
	ASSERT_TRUE(m_overlay.Show(ManyTestItems(), L"item-0"));

	RECT inputRect{};
	ASSERT_TRUE(::GetWindowRect(InputWindow(), &inputRect));
	const int inputHeight = inputRect.bottom - inputRect.top;
	const int dpi = static_cast<int>(::GetDpiForWindow(OverlayWindow()));
	const int effectiveDpi = dpi == 0 ? 96 : dpi;
	const int expectedFrameHeight = ::MulDiv(26, effectiveDpi, 96);
	const int expectedVerticalInset = ::MulDiv(3, effectiveDpi, 96);
	const int expectedHeight = expectedFrameHeight - expectedVerticalInset * 2;
	EXPECT_EQ(expectedHeight, inputHeight);
	::MapWindowPoints(nullptr, OverlayWindow(),
		reinterpret_cast<POINT*>(&inputRect), 2);
	EXPECT_EQ(::MulDiv(6, effectiveDpi, 96) + expectedVerticalInset, inputRect.top);

	RECT listRect{};
	ASSERT_TRUE(::GetWindowRect(ListWindow(), &listRect));
	::MapWindowPoints(nullptr, OverlayWindow(),
		reinterpret_cast<POINT*>(&listRect), 2);
	// VS Code uses six DIP above its one-line input and four DIP below it.
	// Guard both the control height and the full 36-DIP header geometry.
	const int expectedTopInset = ::MulDiv(6, effectiveDpi, 96);
	const int expectedBottomInset = ::MulDiv(4, effectiveDpi, 96);
	EXPECT_EQ(expectedTopInset + expectedFrameHeight + expectedBottomInset, listRect.top);
	EXPECT_EQ(::MulDiv(36, effectiveDpi, 96), listRect.top);
	EXPECT_LT(listRect.top, ::MulDiv(52, effectiveDpi, 96));

	// The shared overlay owns the visible, themed scrollbar surface.  The native
	// LISTBOX may clear its style bit when USER hides the platform bar, but its
	// LB_* scroll contract remains available to the overlay.
	ASSERT_NE(nullptr, ScrollbarWindow());
	EXPECT_NE(0, ::GetWindowLongPtrW(ScrollbarWindow(), GWL_STYLE) & WS_VISIBLE);
}

TEST_F(CommandPaletteOverlayTest, ShortQuickPicksAndInputBoxesUseOnlyTheirContentHeight)
{
	const std::vector<workbench::quickinput::CommandPaletteItem> themes{
		{ .id = L"dark", .label = L"Sakura Default Dark", .enabled = true },
		{ .id = L"light", .label = L"Sakura Default Light", .enabled = true },
	};
	ASSERT_TRUE(m_overlay.Show(themes, L"dark"));

	const int dpi = static_cast<int>(::GetDpiForWindow(OverlayWindow()));
	const int effectiveDpi = dpi == 0 ? 96 : dpi;
	RECT overlay{};
	ASSERT_TRUE(::GetWindowRect(OverlayWindow(), &overlay));
	::MapWindowPoints(nullptr, m_parent, reinterpret_cast<POINT*>(&overlay), 2);
	const int expectedHeader = ::MulDiv(6, effectiveDpi, 96)
		+ ::MulDiv(26, effectiveDpi, 96) + ::MulDiv(4, effectiveDpi, 96);
	const int expectedBottom = ::MulDiv(7, effectiveDpi, 96);
	const int expectedThemeRows = ::MulDiv(22, effectiveDpi, 96) * 2;
	EXPECT_EQ(expectedHeader + expectedThemeRows + expectedBottom,
		overlay.bottom - overlay.top);
	EXPECT_EQ(::MulDiv(6, effectiveDpi, 96), overlay.top);

	ASSERT_TRUE(m_overlay.ShowInput(L"Branch name", L"Branch name", L"feature"));
	ASSERT_TRUE(::GetWindowRect(OverlayWindow(), &overlay));
	EXPECT_EQ(expectedHeader, overlay.bottom - overlay.top);
}

TEST_F(CommandPaletteOverlayTest, FilteringShrinksAndSynchronouslyRepaintsTheVacatedRegion)
{
	m_overlay.SetSearchCallback([](std::wstring_view query) {
		if (query == L"one") {
			return std::vector<workbench::quickinput::CommandPaletteItem>{
				{ .id = L"one", .label = L"One result", .enabled = true },
			};
		}
		return ManyTestItems();
	});
	ASSERT_TRUE(m_overlay.Show(ManyTestItems(), L"item-0"));
	RECT before{};
	ASSERT_TRUE(::GetWindowRect(OverlayWindow(), &before));
	(void)::RedrawWindow(m_parent, nullptr, nullptr,
		RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE | RDW_ALLCHILDREN);

	::SetWindowTextW(InputWindow(), L"one");
	RECT after{};
	ASSERT_TRUE(::GetWindowRect(OverlayWindow(), &after));
	EXPECT_LT(after.bottom - after.top, before.bottom - before.top);
	for (const HWND window : { m_parent, OverlayWindow(), InputWindow(), ListWindow() }) {
		ASSERT_NE(nullptr, window);
		RECT pending{};
		EXPECT_FALSE(::GetUpdateRect(window, &pending, FALSE));
	}
}

TEST_F(CommandPaletteOverlayTest, ShowCompletesTheFirstVisibleFrameSynchronously)
{
	ASSERT_TRUE(m_overlay.Show(ManyTestItems(), L"item-0"));
	EXPECT_NE(0, ::GetWindowLongPtrW(OverlayWindow(), GWL_STYLE) & WS_CLIPSIBLINGS);

	// Showing the overlay is a presentation terminal: callers must not observe a
	// visible-but-unpainted child while the parent editor continues its own paint.
	for (const HWND window : { OverlayWindow(), InputWindow(), ListWindow(), ScrollbarWindow() }) {
		ASSERT_NE(nullptr, window);
		RECT pending{};
		EXPECT_FALSE(::GetUpdateRect(window, &pending, FALSE));
	}
}

TEST_F(CommandPaletteOverlayTest, RoutesFocusedInputWheelToListAndCompletesRepaint)
{
	ASSERT_TRUE(m_overlay.Show(ManyTestItems(), L"item-0"));
	ASSERT_TRUE(::UpdateWindow(ListWindow()));
	ASSERT_EQ(0, ::SendMessageW(ListWindow(), LB_GETTOPINDEX, 0, 0));

	MSG wheelDown{ InputWindow(), WM_MOUSEWHEEL,
		MAKEWPARAM(0, static_cast<WORD>(-WHEEL_DELTA)), 0 };
	EXPECT_TRUE(m_overlay.PreTranslateMessage(wheelDown));
	const auto topAfterDown = static_cast<int>(
		::SendMessageW(ListWindow(), LB_GETTOPINDEX, 0, 0));
	EXPECT_GT(topAfterDown, 0);

	// Scroll handling repaints the complete visible viewport synchronously. A
	// pending partial update here can expose shifted owner-draw pixels until the
	// next unrelated paint.
	RECT pending{};
	EXPECT_FALSE(::GetUpdateRect(ListWindow(), &pending, FALSE));
	RECT firstVisible{};
	ASSERT_NE(LB_ERR, ::SendMessageW(ListWindow(), LB_GETITEMRECT,
		static_cast<WPARAM>(topAfterDown), reinterpret_cast<LPARAM>(&firstVisible)));
	EXPECT_LE(firstVisible.top, 0);
	EXPECT_GT(firstVisible.bottom, 0);

	MSG wheelUp{ InputWindow(), WM_MOUSEWHEEL,
		MAKEWPARAM(0, static_cast<WORD>(WHEEL_DELTA)), 0 };
	EXPECT_TRUE(m_overlay.PreTranslateMessage(wheelUp));
	EXPECT_LT(static_cast<int>(::SendMessageW(ListWindow(), LB_GETTOPINDEX, 0, 0)),
		topAfterDown);
}

TEST_F(CommandPaletteOverlayTest, OverlayThumbDragsMixedHeightRowsWithoutPendingPaint)
{
	ASSERT_TRUE(m_overlay.Show(MixedHeightTestItems(), L"mixed-0"));
	ASSERT_TRUE(::UpdateWindow(ListWindow()));
	const HWND scrollbar = ScrollbarWindow();
	ASSERT_NE(nullptr, scrollbar);
	RECT client{};
	ASSERT_TRUE(::GetClientRect(scrollbar, &client));
	const int x = (client.right - client.left) / 2;
	const int bottom = (std::max)(1, static_cast<int>(client.bottom - client.top) - 2);

	// Grab the initial thumb at the top and drag it to the end of the pixel
	// range. The explicit model snaps that pixel offset to a legal LISTBOX row.
	::SendMessageW(scrollbar, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, 2));
	::SendMessageW(scrollbar, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(x, bottom));
	::SendMessageW(scrollbar, WM_LBUTTONUP, 0, MAKELPARAM(x, bottom));
	const int top = static_cast<int>(::SendMessageW(ListWindow(), LB_GETTOPINDEX, 0, 0));
	EXPECT_GT(top, 0);
	EXPECT_LT(top, static_cast<int>(MixedHeightTestItems().size()));
	RECT firstVisible{};
	ASSERT_NE(LB_ERR, ::SendMessageW(ListWindow(), LB_GETITEMRECT,
		static_cast<WPARAM>(top), reinterpret_cast<LPARAM>(&firstVisible)));
	EXPECT_LE(firstVisible.top, 0);
	EXPECT_GT(firstVisible.bottom, 0);
	RECT pending{};
	EXPECT_FALSE(::GetUpdateRect(ListWindow(), &pending, FALSE));
}

TEST_F(CommandPaletteOverlayTest, CompactRowsReserveTwoLinesOnlyForDetail)
{
	const std::vector<workbench::quickinput::CommandPaletteItem> items{
		{ .id = L"compact", .label = L"Compact", .description = L"main", .enabled = true },
		{ .id = L"detail", .label = L"With detail", .description = L"main",
			.detail = L"commit abcdef0", .enabled = true },
	};
	ASSERT_TRUE(m_overlay.Show(items, L"compact"));

	RECT compact{};
	RECT detail{};
	ASSERT_NE(LB_ERR, ::SendMessageW(ListWindow(), LB_GETITEMRECT, 0,
		reinterpret_cast<LPARAM>(&compact)));
	ASSERT_NE(LB_ERR, ::SendMessageW(ListWindow(), LB_GETITEMRECT, 1,
		reinterpret_cast<LPARAM>(&detail)));
	const int dpi = static_cast<int>(::GetDpiForWindow(OverlayWindow()));
	const int effectiveDpi = dpi == 0 ? 96 : dpi;
	EXPECT_EQ(::MulDiv(22, effectiveDpi, 96), compact.bottom - compact.top);
	EXPECT_EQ(::MulDiv(44, effectiveDpi, 96), detail.bottom - detail.top);
}

TEST_F(CommandPaletteOverlayTest, ProgrammaticDismissalUsesCancelTerminal)
{
	int cancelled = 0;
	m_overlay.SetCancelCallback([&cancelled] { ++cancelled; });
	ASSERT_TRUE(m_overlay.Show(TestItems(), L"dark"));

	m_overlay.Cancel();
	EXPECT_EQ(1, cancelled);
	EXPECT_FALSE(m_overlay.IsVisible());

	// The terminal is idempotent once the palette is hidden.
	m_overlay.Cancel();
	EXPECT_EQ(1, cancelled);
}

TEST_F(CommandPaletteOverlayTest, SeparatorsAreVisibleButCannotBecomeAnswers)
{
	std::vector<std::wstring> accepted;
	m_overlay.SetAcceptCallback([&accepted](std::wstring id) { accepted.push_back(std::move(id)); });
	const std::vector<workbench::quickinput::CommandPaletteItem> items{
		{ .id = L"branch", .label = L"$(git-branch) main", .description = L"abcdef0", .enabled = true },
		{ .id = L"separator", .label = L"remote branches", .enabled = false, .separator = true },
		{ .id = L"remote", .label = L"$(cloud) origin/main", .description = L"Remote branch at abcdef0", .enabled = true },
	};
	ASSERT_TRUE(m_overlay.Show(items, L"branch"));
	ASSERT_EQ(L"remote branches", std::wstring([&] {
		const HWND list = ListWindow();
		const int length = static_cast<int>(::SendMessageW(list, LB_GETTEXTLEN, 1, 0));
		std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
		::SendMessageW(list, LB_GETTEXT, 1, reinterpret_cast<LPARAM>(text.data()));
		text.resize(static_cast<std::size_t>(length));
		return text;
	}()));
	::SendMessageW(ListWindow(), LB_SETCURSEL, 1, 0);
	::SendMessageW(OverlayWindow(), WM_COMMAND,
		MAKEWPARAM(kListControl, LBN_SELCHANGE), reinterpret_cast<LPARAM>(ListWindow()));
	MSG accept{ InputWindow(), WM_KEYDOWN, VK_RETURN, 0 };
	EXPECT_TRUE(m_overlay.PreTranslateMessage(accept));
	EXPECT_EQ((std::vector<std::wstring>{ L"branch" }), accepted);
}

TEST_F(CommandPaletteOverlayTest, InputModeUsesTheSharedNonModalTerminal)
{
	std::vector<std::wstring> accepted;
	int cancelled = 0;
	m_overlay.SetAcceptCallback([&accepted](std::wstring value) { accepted.push_back(std::move(value)); });
	m_overlay.SetCancelCallback([&cancelled] { ++cancelled; });
	ASSERT_TRUE(m_overlay.ShowInput(L"Branch name", L"Branch name", L"feature"));
	ASSERT_NE(nullptr, PromptWindow());
	RECT promptRect{};
	RECT inputRect{};
	ASSERT_TRUE(::GetWindowRect(PromptWindow(), &promptRect));
	ASSERT_TRUE(::GetWindowRect(InputWindow(), &inputRect));
	const int dpi = static_cast<int>(::GetDpiForWindow(OverlayWindow()));
	const int effectiveDpi = dpi == 0 ? 96 : dpi;
	const int expectedVerticalInset = ::MulDiv(3, effectiveDpi, 96);
	EXPECT_EQ(promptRect.top + expectedVerticalInset, inputRect.top);
	EXPECT_EQ(promptRect.bottom - expectedVerticalInset, inputRect.bottom);
	EXPECT_LT(promptRect.right, inputRect.left);
	RECT listRect{};
	ASSERT_TRUE(::GetWindowRect(ListWindow(), &listRect));
	::MapWindowPoints(nullptr, OverlayWindow(),
		reinterpret_cast<POINT*>(&listRect), 2);
	const int expectedTopInset = ::MulDiv(6, effectiveDpi, 96);
	const int expectedBottomInset = ::MulDiv(4, effectiveDpi, 96);
	const int expectedInputHeight = ::MulDiv(26, effectiveDpi, 96);
	EXPECT_EQ(expectedTopInset + expectedInputHeight + expectedBottomInset, listRect.top);
	EXPECT_EQ(static_cast<LONG_PTR>(0),
		static_cast<LONG_PTR>(::GetWindowLongPtrW(OverlayWindow(), GWL_STYLE) & WS_CAPTION));
	::SetWindowTextW(InputWindow(), L"feature/new");
	MSG accept{ InputWindow(), WM_KEYDOWN, VK_RETURN, 0 };
	EXPECT_TRUE(m_overlay.PreTranslateMessage(accept));
	EXPECT_EQ((std::vector<std::wstring>{ L"feature/new" }), accepted);
	EXPECT_FALSE(m_overlay.IsVisible());

	ASSERT_TRUE(m_overlay.ShowInput(L"Branch name", L"Branch name"));
	MSG cancel{ InputWindow(), WM_KEYDOWN, VK_ESCAPE, 0 };
	EXPECT_TRUE(m_overlay.PreTranslateMessage(cancel));
	EXPECT_EQ(1, cancelled);
	EXPECT_FALSE(m_overlay.IsVisible());
}

TEST_F(CommandPaletteOverlayTest, LightSelectionUsesThemeElevationInsteadOfAccentFill)
{
	const auto light = theme::CThemeService::PaletteFor(theme::ThemeMode::Light);
	m_overlay.SetPalette(light);
	ASSERT_TRUE(m_overlay.Show(TestItems(), L"dark"));

	HDC screen = ::GetDC(nullptr);
	ASSERT_NE(nullptr, screen);
	HDC dc = ::CreateCompatibleDC(screen);
	ASSERT_NE(nullptr, dc);
	HBITMAP bitmap = ::CreateCompatibleBitmap(screen, 240, 64);
	ASSERT_NE(nullptr, bitmap);
	const HGDIOBJ previous = ::SelectObject(dc, bitmap);
	ASSERT_NE(nullptr, previous);
	RECT row{ 0, 0, 240, 64 };
	DRAWITEMSTRUCT draw{};
	draw.CtlType = ODT_LISTBOX;
	draw.CtlID = kListControl;
	draw.itemID = 0;
	draw.itemState = ODS_SELECTED;
	draw.hwndItem = ListWindow();
	draw.hDC = dc;
	draw.rcItem = row;
	ASSERT_EQ(TRUE, ::SendMessageW(OverlayWindow(), WM_DRAWITEM, kListControl,
		reinterpret_cast<LPARAM>(&draw)));
	const COLORREF selectedPixel = ::GetPixel(dc, 120, 32);
	EXPECT_EQ(light.listActiveSelectionBackground.ToColorRef(), selectedPixel);
	EXPECT_NE(light.accent.ToColorRef(), selectedPixel);
	// The owner-draw row is rounded independently from the LISTBOX band.
	EXPECT_EQ(light.quickInputBackground.ToColorRef(), ::GetPixel(dc, 0, 0));

	::SelectObject(dc, previous);
	::DeleteObject(bitmap);
	::DeleteDC(dc);
	::ReleaseDC(nullptr, screen);
}

} // namespace

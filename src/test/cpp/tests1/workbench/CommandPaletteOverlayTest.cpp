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
constexpr int kInputControl = 100;
constexpr int kListControl = 101;

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
	HWND ListWindow() const { return ::GetDlgItem(OverlayWindow(), kListControl); }

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

	MSG down{ InputWindow(), WM_KEYDOWN, VK_DOWN, 0 };
	EXPECT_TRUE(m_overlay.PreTranslateMessage(down));
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

TEST_F(CommandPaletteOverlayTest, MissingSelectionCallbackKeepsCommandPaletteSideEffectFree)
{
	m_overlay.SetSelectionCallback({});
	ASSERT_TRUE(m_overlay.Show(TestItems(), L"dark"));

	MSG down{ InputWindow(), WM_KEYDOWN, VK_DOWN, 0 };
	EXPECT_TRUE(m_overlay.PreTranslateMessage(down));
	EXPECT_TRUE(m_overlay.IsVisible());
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

} // namespace

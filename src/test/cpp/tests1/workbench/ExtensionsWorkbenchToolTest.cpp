/*! @file */
/*
Copyright (C) 2026, Sakura Editor Organization

SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

#include "senp/SenpManagementService.h"
#include "senp/SenpRuntimeService.h"
#include "workbench/extensions/CExtensionsWorkbenchTool.h"

namespace {

class WindowGuard final {
public:
	explicit WindowGuard(HWND window) noexcept : m_window(window) {}
	~WindowGuard()
	{
		if (m_window != nullptr) ::DestroyWindow(m_window);
	}
	WindowGuard(const WindowGuard&) = delete;
	WindowGuard& operator=(const WindowGuard&) = delete;

private:
	HWND m_window = nullptr;
};

class MemorySurface final {
public:
	MemorySurface(int width, int height)
	{
		m_dc = ::CreateCompatibleDC(nullptr);
		if (m_dc == nullptr) return;
		BITMAPINFO info{};
		info.bmiHeader.biSize = sizeof(info.bmiHeader);
		info.bmiHeader.biWidth = width;
		info.bmiHeader.biHeight = -height;
		info.bmiHeader.biPlanes = 1;
		info.bmiHeader.biBitCount = 32;
		info.bmiHeader.biCompression = BI_RGB;
		void* bits = nullptr;
		m_bitmap = ::CreateDIBSection(m_dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
		if (m_bitmap != nullptr) m_previous = ::SelectObject(m_dc, m_bitmap);
	}
	~MemorySurface()
	{
		if (m_previous != nullptr) ::SelectObject(m_dc, m_previous);
		if (m_bitmap != nullptr) ::DeleteObject(m_bitmap);
		if (m_dc != nullptr) ::DeleteDC(m_dc);
	}
	MemorySurface(const MemorySurface&) = delete;
	MemorySurface& operator=(const MemorySurface&) = delete;

	[[nodiscard]] bool IsValid() const noexcept
	{
		return m_dc != nullptr && m_bitmap != nullptr && m_previous != nullptr;
	}
	[[nodiscard]] HDC Dc() const noexcept { return m_dc; }
	[[nodiscard]] COLORREF Pixel(int x, int y) const noexcept { return ::GetPixel(m_dc, x, y); }

private:
	HDC m_dc = nullptr;
	HBITMAP m_bitmap = nullptr;
	HGDIOBJ m_previous = nullptr;
};

class FakeManagementService final : public senp::ISenpManagementService {
public:
	[[nodiscard]] senp::ManagementOperationResult Start() override
	{
		return { senp::EManagementOperationStatus::Succeeded, snapshot };
	}
	[[nodiscard]] senp::ManagementOperationResult InstallDeveloperPackage(
		std::wstring_view, bool) override
	{
		return { senp::EManagementOperationStatus::Succeeded, snapshot };
	}
	[[nodiscard]] senp::ManagementOperationResult InstallBuiltInPackage(
		std::wstring_view extensionId) override
	{
		lastBuiltInInstall = std::wstring(extensionId);
		for (auto& extension : snapshot.extensions) {
			if (extension.id != extensionId) continue;
			extension.installed = true;
			extension.enabled = true;
		}
		return { senp::EManagementOperationStatus::Succeeded, snapshot };
	}
	[[nodiscard]] senp::ManagementOperationResult UninstallBuiltInPackage(
		std::wstring_view extensionId) override
	{
		lastBuiltInUninstall = std::wstring(extensionId);
		for (auto& extension : snapshot.extensions) {
			if (extension.id != extensionId) continue;
			extension.installed = false;
			extension.enabled = false;
		}
		return { senp::EManagementOperationStatus::Succeeded, snapshot };
	}
	[[nodiscard]] senp::ManagementOperationResult Refresh() override
	{
		return { senp::EManagementOperationStatus::Succeeded, snapshot };
	}
	void Stop() noexcept override {}
	[[nodiscard]] senp::ManagementSnapshot Snapshot() const override { return snapshot; }

	senp::ManagementSnapshot snapshot;
	std::wstring lastBuiltInInstall;
	std::wstring lastBuiltInUninstall;
};

TEST(ExtensionsWorkbenchTool, CreatesNativeViewDuringSynchronousWindowMessages)
{
	const HWND parent = ::CreateWindowExW(0, L"STATIC", L"Extensions test parent", WS_POPUP,
		0, 0, 800, 600, nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
	ASSERT_NE(nullptr, parent);
	WindowGuard parentGuard(parent);

	workbench::extensions::CExtensionsWorkbenchTool tool;
	const bool created = tool.Create(parent);
	EXPECT_TRUE(created) << "CreateWindowExW failed with error " << ::GetLastError();
	if (!created) return;

	const HWND view = tool.GetHwnd();
	ASSERT_NE(nullptr, view);
	EXPECT_TRUE(::IsWindow(view));
	EXPECT_EQ(parent, ::GetParent(view));
	wchar_t className[64]{};
	ASSERT_GT(::GetClassNameW(view, className, static_cast<int>(std::size(className))), 0);
	EXPECT_STREQ(L"SakuraSenpExtensionsView", className);

	tool.Layout(RECT{ 0, 0, 480, 320 }, 96);
	RECT bounds{};
	ASSERT_TRUE(::GetWindowRect(view, &bounds));
	EXPECT_EQ(480, bounds.right - bounds.left);
	EXPECT_EQ(320, bounds.bottom - bounds.top);

	tool.Close();
	EXPECT_EQ(nullptr, tool.GetHwnd());
	EXPECT_FALSE(::IsWindow(view));
}

TEST(ExtensionsWorkbenchTool, UsesOneCompactListWithoutSearchOrSectionHeaders)
{
	const HWND parent = ::CreateWindowExW(0, L"STATIC", L"Extensions theme test parent", WS_POPUP,
		0, 0, 800, 600, nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
	ASSERT_NE(nullptr, parent);
	WindowGuard parentGuard(parent);

	workbench::extensions::CExtensionsWorkbenchTool tool;
	ASSERT_TRUE(tool.Create(parent));
	tool.Layout(RECT{ 0, 0, 480, 320 }, 96);
	const HWND view = tool.GetHwnd();
	ASSERT_NE(nullptr, view);
	EXPECT_EQ(nullptr, ::GetDlgItem(view, 1));
	EXPECT_EQ(nullptr, ::GetDlgItem(view, 2));
	EXPECT_EQ(nullptr, ::FindWindowExW(view, nullptr, L"EDIT", nullptr));

	FakeManagementService service;
	service.snapshot.state = senp::EManagementState::Ready;
	service.snapshot.extensions.push_back({
		.id = L"sakura.indent-rainbow",
		.displayName = L"Indent Rainbow",
		.version = L"0.1.0",
		.publisher = L"sakura.builtin",
		.description = L"Colors indentation levels in the active editor.",
		.readme = L"# Indent Rainbow\n\nThis text belongs in the details surface.",
		.installed = true,
		.builtIn = true,
		.enabled = true,
	});
	tool.SetManagementService(&service);

	auto palette = theme::CThemeService::PaletteFor(theme::ThemeMode::Dark);
	palette.sideBar = { 0x10, 0x11, 0x12 };
	palette.raised = { 0x20, 0x21, 0x22 };
	palette.primaryText = { 0xD0, 0xD1, 0xD2 };
	palette.disabledText = { 0x70, 0x71, 0x72 };
	palette.accent = { 0x30, 0x90, 0xD0 };
	palette.border = { 0x61, 0x62, 0x63 };
	tool.SetPalette(palette);

	MemorySurface surface(480, 320);
	ASSERT_TRUE(surface.IsValid());
	ASSERT_EQ(0, ::SendMessageW(view, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(surface.Dc()), 0));
	// The first extension starts at the top of the ViewContainer body. There is
	// no search header, Built-in section, Installed section, or README card.
	EXPECT_EQ(palette.sideBar.ToColorRef(), surface.Pixel(2, 0));
	EXPECT_EQ(palette.sideBar.ToColorRef(), surface.Pixel(2, 71));
}

TEST(ExtensionsWorkbenchTool, InstallsAnAvailableBuiltInFromItsRowButton)
{
	const HWND parent = ::CreateWindowExW(0, L"STATIC", L"Extensions install test parent", WS_POPUP,
		0, 0, 800, 600, nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
	ASSERT_NE(nullptr, parent);
	WindowGuard parentGuard(parent);

	workbench::extensions::CExtensionsWorkbenchTool tool;
	ASSERT_TRUE(tool.Create(parent));
	tool.Layout(RECT{ 0, 0, 480, 320 }, 96);
	FakeManagementService service;
	service.snapshot.state = senp::EManagementState::Ready;
	service.snapshot.extensions.push_back({
		.id = L"sakura-indent-rainbow",
		.displayName = L"Indent Rainbow",
		.version = L"0.1.0",
		.publisher = L"sakura.builtin",
		.description = L"Colors indentation levels in the active editor.",
		.installed = false,
		.builtIn = true,
	});
	tool.SetManagementService(&service);
	int extensionsChanged = 0;
	tool.SetExtensionsChangedCallback([&extensionsChanged] { ++extensionsChanged; });

	auto palette = theme::CThemeService::PaletteFor(theme::ThemeMode::Dark);
	palette.buttonBackground = { 0x11, 0x82, 0xC4 };
	tool.SetPalette(palette);
	const HWND button = ::FindWindowExW(tool.GetHwnd(), nullptr, L"BUTTON", L"Install");
	ASSERT_NE(nullptr, button);
	EXPECT_NE(0L, ::GetWindowLongPtrW(button, GWL_STYLE) & WS_VISIBLE);
	EXPECT_NE(0L, ::GetWindowLongPtrW(button, GWL_STYLE) & WS_TABSTOP);
	EXPECT_EQ(BS_OWNERDRAW, ::GetWindowLongPtrW(button, GWL_STYLE) & BS_TYPEMASK);
	RECT bounds{};
	ASSERT_TRUE(::GetWindowRect(button, &bounds));
	::MapWindowPoints(nullptr, tool.GetHwnd(), reinterpret_cast<POINT*>(&bounds), 2);
	EXPECT_EQ(404, bounds.left);
	EXPECT_EQ(44, bounds.top);
	EXPECT_EQ(470, bounds.right);
	EXPECT_EQ(66, bounds.bottom);

	MemorySurface surface(66, 22);
	ASSERT_TRUE(surface.IsValid());
	ASSERT_EQ(1, ::SendMessageW(button, WM_ERASEBKGND,
		reinterpret_cast<WPARAM>(surface.Dc()), 0));
	EXPECT_EQ(palette.buttonBackground.ToColorRef(), surface.Pixel(2, 2));
	const DRAWITEMSTRUCT draw{ ODT_BUTTON, static_cast<UINT>(::GetDlgCtrlID(button)), 0,
		ODA_DRAWENTIRE, 0, button, surface.Dc(), RECT{ 0, 0, 66, 22 }, 0 };
	ASSERT_EQ(TRUE, ::SendMessageW(tool.GetHwnd(), WM_DRAWITEM, draw.CtlID,
		reinterpret_cast<LPARAM>(&draw)));
	EXPECT_EQ(palette.buttonBackground.ToColorRef(), surface.Pixel(2, 2));

	(void)::SendMessageW(button, BM_CLICK, 0, 0);
	EXPECT_EQ(L"sakura-indent-rainbow", service.lastBuiltInInstall);
	EXPECT_EQ(1, extensionsChanged);
	EXPECT_EQ(nullptr, ::FindWindowExW(tool.GetHwnd(), nullptr, L"BUTTON", L"Install"));
	EXPECT_NE(nullptr, ::FindWindowExW(tool.GetHwnd(), nullptr, L"BUTTON", L"Uninstall"));
}

TEST(ExtensionsWorkbenchTool, UninstallsAnInstalledBuiltInFromItsRowButton)
{
	const HWND parent = ::CreateWindowExW(0, L"STATIC", L"Extensions uninstall test parent", WS_POPUP,
		0, 0, 800, 600, nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
	ASSERT_NE(nullptr, parent);
	WindowGuard parentGuard(parent);

	workbench::extensions::CExtensionsWorkbenchTool tool;
	ASSERT_TRUE(tool.Create(parent));
	tool.Layout(RECT{ 0, 0, 480, 320 }, 96);
	FakeManagementService service;
	service.snapshot.state = senp::EManagementState::Ready;
	service.snapshot.extensions.push_back({
		.id = L"sakura-indent-rainbow",
		.displayName = L"Indent Rainbow",
		.version = L"0.1.0",
		.publisher = L"sakura.builtin",
		.description = L"Colors indentation levels in the active editor.",
		.installed = true,
		.builtIn = true,
		.enabled = true,
	});
	tool.SetManagementService(&service);
	int extensionsChanged = 0;
	tool.SetExtensionsChangedCallback([&extensionsChanged] { ++extensionsChanged; });

	const HWND button = ::FindWindowExW(tool.GetHwnd(), nullptr, L"BUTTON", L"Uninstall");
	ASSERT_NE(nullptr, button);
	(void)::SendMessageW(button, BM_CLICK, 0, 0);
	EXPECT_EQ(L"sakura-indent-rainbow", service.lastBuiltInUninstall);
	EXPECT_EQ(1, extensionsChanged);
	EXPECT_EQ(nullptr, ::FindWindowExW(tool.GetHwnd(), nullptr, L"BUTTON", L"Uninstall"));
	EXPECT_NE(nullptr, ::FindWindowExW(tool.GetHwnd(), nullptr, L"BUTTON", L"Install"));
}

TEST(ExtensionsWorkbenchTool, RuntimeInvalidationRequeuesLinesCachedBeforeInstall)
{
	FakeManagementService service;
	service.snapshot.state = senp::EManagementState::Ready;
	service.snapshot.revision = 1;
	auto runtime = senp::CreateWin32SenpRuntimeService(service);
	ASSERT_TRUE(runtime->Start());

	constexpr std::wstring_view line = L"        value";
	EXPECT_FALSE(runtime->RequestIndentDecorations(line, 4, 0).has_value());
	std::optional<std::vector<senp::IndentDecoration>> completed;
	const auto firstDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	do {
		completed = runtime->RequestIndentDecorations(line, 4, 0);
		if (!completed) std::this_thread::sleep_for(std::chrono::milliseconds(1));
	} while (!completed && std::chrono::steady_clock::now() < firstDeadline);
	ASSERT_TRUE(completed.has_value());
	EXPECT_TRUE(completed->empty());

	service.snapshot.revision = 2;
	runtime->NotifyExtensionsChanged();
	// The pre-install empty result cannot satisfy the first post-install paint.
	EXPECT_FALSE(runtime->RequestIndentDecorations(line, 4, 0).has_value());
	const auto secondDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	do {
		completed = runtime->RequestIndentDecorations(line, 4, 0);
		if (!completed) std::this_thread::sleep_for(std::chrono::milliseconds(1));
	} while (!completed && std::chrono::steady_clock::now() < secondDeadline);
	EXPECT_TRUE(completed.has_value());
	runtime->Stop();
}

TEST(ExtensionsWorkbenchTool, UsesSharedOverlayScrollbarForOverflowingViews)
{
	const HWND parent = ::CreateWindowExW(0, L"STATIC", L"Extensions scroll test parent", WS_POPUP,
		0, 0, 800, 600, nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
	ASSERT_NE(nullptr, parent);
	WindowGuard parentGuard(parent);

	workbench::extensions::CExtensionsWorkbenchTool tool;
	ASSERT_TRUE(tool.Create(parent));
	tool.Layout(RECT{ 0, 0, 480, 240 }, 96);
	const HWND view = tool.GetHwnd();
	ASSERT_NE(nullptr, view);
	EXPECT_EQ(0, ::GetWindowLongPtrW(view, GWL_STYLE) & WS_VSCROLL);

	FakeManagementService service;
	service.snapshot.state = senp::EManagementState::Ready;
	for (int index = 0; index < 8; ++index) {
		service.snapshot.extensions.push_back({
			.id = L"sakura.builtin-" + std::to_wstring(index),
			.displayName = L"Built-in Extension",
			.version = L"0.1.0",
			.publisher = L"sakura.builtin",
			.description = L"A compact built-in extension row.",
			.builtIn = true,
			.enabled = true,
		});
	}
	tool.SetManagementService(&service);

	const HWND scrollbar = ::FindWindowExW(
		view, nullptr, L"SakuraWorkbenchOverlayScrollbar", nullptr);
	ASSERT_NE(nullptr, scrollbar);
	EXPECT_NE(0L, ::GetWindowLongPtrW(scrollbar, GWL_STYLE) & WS_VISIBLE);
	RECT bounds{};
	ASSERT_TRUE(::GetWindowRect(scrollbar, &bounds));
	::MapWindowPoints(nullptr, view, reinterpret_cast<POINT*>(&bounds), 2);
	EXPECT_EQ(470, bounds.left);
	EXPECT_EQ(0, bounds.top);
	EXPECT_EQ(480, bounds.right);
	EXPECT_EQ(240, bounds.bottom);
}

} // namespace

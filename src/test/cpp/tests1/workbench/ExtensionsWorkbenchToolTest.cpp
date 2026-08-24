/*! @file */
/*
Copyright (C) 2026, Sakura Editor Organization

SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include <gtest/gtest.h>

#include <shellapi.h>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <thread>
#include <utility>
#include <vector>

#include "dlg/ModalDialogCloser.hpp"
#include "CSelectLang.h"
#include "sakura_rc.h"
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

int MeasureActionButtonWidth(HWND view, HWND button, std::wstring_view label)
{
	HDC dc = ::GetDC(view);
	if (dc == nullptr) return 0;
	SIZE textSize{};
	const HFONT font = reinterpret_cast<HFONT>(::SendMessageW(button, WM_GETFONT, 0, 0));
	const HGDIOBJ previousFont = font == nullptr ? nullptr : ::SelectObject(dc, font);
	const BOOL measured = ::GetTextExtentPoint32W(
		dc, label.data(), static_cast<int>(label.size()), &textSize);
	if (previousFont != nullptr) ::SelectObject(dc, previousFont);
	::ReleaseDC(view, dc);
	if (measured == FALSE) return 0;
	// 5-DIP horizontal padding + 1-DIP border on both sides at 96 DPI.
	return textSize.cx + 2 * (5 + 1);
}

std::wstring ExtensionActionLabel(bool installed)
{
	return std::wstring(CSelectLang::LoadStringW(installed
		? STR_WORKBENCH_EXTENSIONS_UNINSTALL : STR_WORKBENCH_EXTENSIONS_INSTALL));
}

class TemporaryPackageFile final {
public:
	explicit TemporaryPackageFile(std::wstring_view suffix)
	{
		wchar_t tempPath[MAX_PATH]{};
		wchar_t candidate[MAX_PATH]{};
		if (::GetTempPathW(static_cast<DWORD>(std::size(tempPath)), tempPath) == 0
			|| ::GetTempFileNameW(tempPath, L"sen", 0, candidate) == 0) {
			return;
		}
		(void)::DeleteFileW(candidate);
		m_path = std::filesystem::path(candidate);
		m_path += std::wstring(suffix);
		const HANDLE file = ::CreateFileW(m_path.c_str(), GENERIC_WRITE,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, CREATE_NEW,
			FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file == INVALID_HANDLE_VALUE) {
			m_path.clear();
			return;
		}
		::CloseHandle(file);
	}
	~TemporaryPackageFile()
	{
		if (!m_path.empty()) (void)::DeleteFileW(m_path.c_str());
	}
	TemporaryPackageFile(const TemporaryPackageFile&) = delete;
	TemporaryPackageFile& operator=(const TemporaryPackageFile&) = delete;
	TemporaryPackageFile(TemporaryPackageFile&& other) noexcept
		: m_path(std::move(other.m_path))
	{
		other.m_path.clear();
	}
	TemporaryPackageFile& operator=(TemporaryPackageFile&& other) noexcept
	{
		if (this == &other) return *this;
		if (!m_path.empty()) (void)::DeleteFileW(m_path.c_str());
		m_path = std::move(other.m_path);
		other.m_path.clear();
		return *this;
	}

	[[nodiscard]] bool IsValid() const noexcept { return !m_path.empty(); }
	[[nodiscard]] const std::filesystem::path& Path() const noexcept { return m_path; }

private:
	std::filesystem::path m_path;
};

class DropFilesPayload final {
public:
	explicit DropFilesPayload(const std::vector<std::wstring>& paths)
	{
		std::size_t characterCount = 1;
		for (const auto& path : paths) characterCount += path.size() + 1;
		const SIZE_T bytes = sizeof(DROPFILES) + characterCount * sizeof(wchar_t);
		m_global = ::GlobalAlloc(GHND, bytes);
		if (m_global == nullptr) return;
		auto* drop = static_cast<DROPFILES*>(::GlobalLock(m_global));
		if (drop == nullptr) {
			::GlobalFree(m_global);
			m_global = nullptr;
			return;
		}
		drop->pFiles = sizeof(DROPFILES);
		drop->fWide = TRUE;
		auto* target = reinterpret_cast<wchar_t*>(
			reinterpret_cast<std::byte*>(drop) + sizeof(DROPFILES));
		for (const auto& path : paths) {
			std::memcpy(target, path.c_str(), (path.size() + 1) * sizeof(wchar_t));
			target += path.size() + 1;
		}
		*target = L'\0';
		::GlobalUnlock(m_global);
	}
	~DropFilesPayload()
	{
		if (m_global != nullptr) (void)::GlobalFree(m_global);
	}
	DropFilesPayload(const DropFilesPayload&) = delete;
	DropFilesPayload& operator=(const DropFilesPayload&) = delete;

	[[nodiscard]] bool IsValid() const noexcept { return m_global != nullptr; }
	[[nodiscard]] HDROP Release() noexcept
	{
		const HDROP drop = reinterpret_cast<HDROP>(m_global);
		m_global = nullptr;
		return drop;
	}

private:
	HGLOBAL m_global = nullptr;
};

bool SendDrop(HWND view, const std::vector<std::wstring>& paths)
{
	DropFilesPayload payload(paths);
	if (!payload.IsValid()) return false;
	(void)::SendMessageW(view, WM_DROPFILES,
		reinterpret_cast<WPARAM>(payload.Release()), 0);
	return true;
}

class FakeManagementService final : public senp::ISenpManagementService {
public:
	[[nodiscard]] senp::ManagementOperationResult Start() override
	{
		return { senp::EManagementOperationStatus::Succeeded, snapshot };
	}
	[[nodiscard]] senp::ManagementOperationResult InstallDeveloperPackage(
		std::wstring_view packagePath, bool enable) override
	{
		developerInstallPaths.emplace_back(packagePath);
		developerInstallEnabled.push_back(enable);
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
	std::vector<std::wstring> developerInstallPaths;
	std::vector<bool> developerInstallEnabled;
};

class DropViewFixture final {
public:
	DropViewFixture() = default;
	~DropViewFixture()
	{
		tool.Close();
		if (parent != nullptr) ::DestroyWindow(parent);
	}
	DropViewFixture(const DropViewFixture&) = delete;
	DropViewFixture& operator=(const DropViewFixture&) = delete;

	bool Initialize()
	{
		parent = ::CreateWindowExW(0, L"STATIC", L"Extensions drop test parent", WS_POPUP,
			0, 0, 800, 600, nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
		if (parent == nullptr || !tool.Create(parent)) return false;
		tool.Layout(RECT{ 0, 0, 480, 320 }, 96);
		service.snapshot.state = senp::EManagementState::Ready;
		tool.SetManagementService(&service);
		tool.SetExtensionsChangedCallback([this] { ++extensionsChanged; });
		return true;
	}

	HWND parent = nullptr;
	workbench::extensions::CExtensionsWorkbenchTool tool;
	FakeManagementService service;
	int extensionsChanged = 0;
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
	const auto installLabel = ExtensionActionLabel(false);
	const auto uninstallLabel = ExtensionActionLabel(true);
	const HWND button = ::FindWindowExW(tool.GetHwnd(), nullptr, L"BUTTON", installLabel.c_str());
	ASSERT_NE(nullptr, button);
	EXPECT_NE(0L, ::GetWindowLongPtrW(button, GWL_STYLE) & WS_VISIBLE);
	EXPECT_NE(0L, ::GetWindowLongPtrW(button, GWL_STYLE) & WS_TABSTOP);
	EXPECT_EQ(BS_OWNERDRAW, ::GetWindowLongPtrW(button, GWL_STYLE) & BS_TYPEMASK);
	RECT bounds{};
	ASSERT_TRUE(::GetWindowRect(button, &bounds));
	::MapWindowPoints(nullptr, tool.GetHwnd(), reinterpret_cast<POINT*>(&bounds), 2);
	const int expectedInstallWidth = MeasureActionButtonWidth(tool.GetHwnd(), button, installLabel);
	EXPECT_GT(expectedInstallWidth, 0);
	EXPECT_EQ(480 - 10 - expectedInstallWidth, bounds.left);
	EXPECT_EQ(50, bounds.top);
	EXPECT_EQ(470, bounds.right);
	EXPECT_EQ(66, bounds.bottom);
	EXPECT_EQ(expectedInstallWidth, bounds.right - bounds.left);

	MemorySurface surface(expectedInstallWidth, 16);
	ASSERT_TRUE(surface.IsValid());
	ASSERT_EQ(1, ::SendMessageW(button, WM_ERASEBKGND,
		reinterpret_cast<WPARAM>(surface.Dc()), 0));
	EXPECT_EQ(palette.buttonBackground.ToColorRef(), surface.Pixel(2, 2));
	const DRAWITEMSTRUCT draw{ ODT_BUTTON, static_cast<UINT>(::GetDlgCtrlID(button)), 0,
		ODA_DRAWENTIRE, 0, button, surface.Dc(), RECT{ 0, 0, expectedInstallWidth, 16 }, 0 };
	ASSERT_EQ(TRUE, ::SendMessageW(tool.GetHwnd(), WM_DRAWITEM, draw.CtlID,
		reinterpret_cast<LPARAM>(&draw)));
	EXPECT_EQ(palette.buttonBackground.ToColorRef(), surface.Pixel(2, 2));

	(void)::SendMessageW(button, BM_CLICK, 0, 0);
	EXPECT_EQ(L"sakura-indent-rainbow", service.lastBuiltInInstall);
	EXPECT_EQ(1, extensionsChanged);
	EXPECT_EQ(nullptr, ::FindWindowExW(tool.GetHwnd(), nullptr, L"BUTTON", installLabel.c_str()));
	const HWND uninstall = ::FindWindowExW(tool.GetHwnd(), nullptr, L"BUTTON", uninstallLabel.c_str());
	ASSERT_NE(nullptr, uninstall);
	RECT uninstallBounds{};
	ASSERT_TRUE(::GetWindowRect(uninstall, &uninstallBounds));
	::MapWindowPoints(nullptr, tool.GetHwnd(), reinterpret_cast<POINT*>(&uninstallBounds), 2);
	const int expectedUninstallWidth =
		MeasureActionButtonWidth(tool.GetHwnd(), uninstall, uninstallLabel);
	EXPECT_GT(expectedUninstallWidth, 0);
	EXPECT_EQ(480 - 10 - expectedUninstallWidth, uninstallBounds.left);
	EXPECT_EQ(50, uninstallBounds.top);
	EXPECT_EQ(470, uninstallBounds.right);
	EXPECT_EQ(66, uninstallBounds.bottom);
	EXPECT_EQ(expectedUninstallWidth, uninstallBounds.right - uninstallBounds.left);
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

	const auto installLabel = ExtensionActionLabel(false);
	const auto uninstallLabel = ExtensionActionLabel(true);
	const HWND button = ::FindWindowExW(tool.GetHwnd(), nullptr, L"BUTTON", uninstallLabel.c_str());
	ASSERT_NE(nullptr, button);
	RECT bounds{};
	ASSERT_TRUE(::GetWindowRect(button, &bounds));
	::MapWindowPoints(nullptr, tool.GetHwnd(), reinterpret_cast<POINT*>(&bounds), 2);
	const int expectedUninstallWidth =
		MeasureActionButtonWidth(tool.GetHwnd(), button, uninstallLabel);
	EXPECT_GT(expectedUninstallWidth, 0);
	EXPECT_EQ(480 - 10 - expectedUninstallWidth, bounds.left);
	EXPECT_EQ(50, bounds.top);
	EXPECT_EQ(470, bounds.right);
	EXPECT_EQ(66, bounds.bottom);
	EXPECT_EQ(expectedUninstallWidth, bounds.right - bounds.left);
	(void)::SendMessageW(button, BM_CLICK, 0, 0);
	EXPECT_EQ(L"sakura-indent-rainbow", service.lastBuiltInUninstall);
	EXPECT_EQ(1, extensionsChanged);
	EXPECT_EQ(nullptr, ::FindWindowExW(tool.GetHwnd(), nullptr, L"BUTTON", uninstallLabel.c_str()));
	const HWND install = ::FindWindowExW(tool.GetHwnd(), nullptr, L"BUTTON", installLabel.c_str());
	ASSERT_NE(nullptr, install);
	RECT installBounds{};
	ASSERT_TRUE(::GetWindowRect(install, &installBounds));
	::MapWindowPoints(nullptr, tool.GetHwnd(), reinterpret_cast<POINT*>(&installBounds), 2);
	const int expectedInstallWidth =
		MeasureActionButtonWidth(tool.GetHwnd(), install, installLabel);
	EXPECT_EQ(480 - 10 - expectedInstallWidth, installBounds.left);
	EXPECT_EQ(50, installBounds.top);
	EXPECT_EQ(470, installBounds.right);
	EXPECT_EQ(66, installBounds.bottom);
	EXPECT_EQ(expectedInstallWidth, installBounds.right - installBounds.left);
}

TEST(ExtensionsWorkbenchTool, InstallsOneDroppedSenpPackageAfterDeveloperConfirmation)
{
	DropViewFixture fixture;
	ASSERT_TRUE(fixture.Initialize());
	TemporaryPackageFile package(L".SENP");
	ASSERT_TRUE(package.IsValid());

	const std::wstring path = package.Path().wstring();
	{
		dialog::ModalDialogCloser closer([](HWND dialog) {
			::SendMessageW(dialog, WM_COMMAND, MAKELONG(IDYES, BN_CLICKED), 0);
		});
		ASSERT_TRUE(SendDrop(fixture.tool.GetHwnd(), { path }));
	}

	ASSERT_EQ(1u, fixture.service.developerInstallPaths.size());
	EXPECT_EQ(path, fixture.service.developerInstallPaths.front());
	ASSERT_EQ(1u, fixture.service.developerInstallEnabled.size());
	EXPECT_TRUE(fixture.service.developerInstallEnabled.front());
	EXPECT_EQ(1, fixture.extensionsChanged);
}

TEST(ExtensionsWorkbenchTool, InstallsMultipleDroppedSenpPackagesInOrder)
{
	DropViewFixture fixture;
	ASSERT_TRUE(fixture.Initialize());
	TemporaryPackageFile first(L".senp");
	TemporaryPackageFile second(L".sEnP");
	ASSERT_TRUE(first.IsValid());
	ASSERT_TRUE(second.IsValid());
	const std::vector<std::wstring> paths{ first.Path().wstring(), second.Path().wstring() };

	{
		dialog::ModalDialogCloser closer([](HWND dialog) {
			::SendMessageW(dialog, WM_COMMAND, MAKELONG(IDYES, BN_CLICKED), 0);
		});
		ASSERT_TRUE(SendDrop(fixture.tool.GetHwnd(), paths));
	}

	EXPECT_EQ(paths, fixture.service.developerInstallPaths);
	ASSERT_EQ(2u, fixture.service.developerInstallEnabled.size());
	EXPECT_TRUE(fixture.service.developerInstallEnabled[0]);
	EXPECT_TRUE(fixture.service.developerInstallEnabled[1]);
	EXPECT_EQ(2, fixture.extensionsChanged);
}

TEST(ExtensionsWorkbenchTool, RejectsDroppedBatchAtomicallyWhenAnyPathIsNotSenp)
{
	DropViewFixture fixture;
	ASSERT_TRUE(fixture.Initialize());
	TemporaryPackageFile valid(L".senp");
	TemporaryPackageFile invalid(L".txt");
	ASSERT_TRUE(valid.IsValid());
	ASSERT_TRUE(invalid.IsValid());
	const std::vector<std::wstring> paths{ valid.Path().wstring(), invalid.Path().wstring() };

	{
		dialog::ModalDialogCloser closer([](HWND dialog) {
			::SendMessageW(dialog, WM_COMMAND, MAKELONG(IDOK, BN_CLICKED), 0);
		});
		ASSERT_TRUE(SendDrop(fixture.tool.GetHwnd(), paths));
	}

	EXPECT_TRUE(fixture.service.developerInstallPaths.empty());
	EXPECT_TRUE(fixture.service.developerInstallEnabled.empty());
	EXPECT_EQ(0, fixture.extensionsChanged);
}

TEST(ExtensionsWorkbenchTool, RejectsDroppedBatchAtomicallyWhenOverPackageLimit)
{
	DropViewFixture fixture;
	ASSERT_TRUE(fixture.Initialize());
	constexpr std::size_t packageLimit = 16;
	std::vector<TemporaryPackageFile> packages;
	packages.reserve(packageLimit + 1);
	for (std::size_t index = 0; index <= packageLimit; ++index) {
		packages.emplace_back(L".senp");
		ASSERT_TRUE(packages.back().IsValid());
	}
	std::vector<std::wstring> paths;
	paths.reserve(packages.size());
	for (const auto& package : packages) paths.push_back(package.Path().wstring());

	{
		dialog::ModalDialogCloser closer([](HWND dialog) {
			::SendMessageW(dialog, WM_COMMAND, MAKELONG(IDOK, BN_CLICKED), 0);
		});
		ASSERT_TRUE(SendDrop(fixture.tool.GetHwnd(), paths));
	}

	EXPECT_TRUE(fixture.service.developerInstallPaths.empty());
	EXPECT_TRUE(fixture.service.developerInstallEnabled.empty());
	EXPECT_EQ(0, fixture.extensionsChanged);
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

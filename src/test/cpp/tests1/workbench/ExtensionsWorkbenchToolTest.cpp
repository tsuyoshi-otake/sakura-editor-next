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
#include <memory>
#include <thread>
#include <utility>
#include <vector>

#include "dlg/ModalDialogCloser.hpp"
#include "CSelectLang.h"
#include "sakura_rc.h"
#include "senp/SenpManagementService.h"
#include "senp/SenpRuntimeService.h"
#include "workbench/SenpWindowExtensions.h"
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
		if (failUninstall) return Failure();
		for (auto& extension : snapshot.extensions) {
			if (extension.id != extensionId) continue;
			extension.installed = false;
			extension.enabled = false;
		}
		return { senp::EManagementOperationStatus::Succeeded, snapshot };
	}
	[[nodiscard]] senp::ManagementOperationResult SetExtensionEnabled(
		std::wstring_view extensionId, bool enabled) override
	{
		lastEnabledId = std::wstring(extensionId);
		lastEnabledValue = enabled;
		for (auto& extension : snapshot.extensions) {
			if (extension.id != extensionId) continue;
			extension.enabled = enabled;
		}
		return { senp::EManagementOperationStatus::Succeeded, snapshot };
	}
	[[nodiscard]] senp::ManagementOperationResult UninstallPackage(
		std::wstring_view stateName) override
	{
		lastPackageUninstall = std::wstring(stateName);
		if (failUninstall) return Failure();
		const auto stem = std::wstring_view(stateName).substr(
			0, stateName.size() > 5 ? stateName.size() - 5 : 0);
		std::erase_if(snapshot.extensions, [&](const auto& extension) {
			return extension.id == stem || extension.stateName == stateName;
		});
		return { senp::EManagementOperationStatus::Succeeded, snapshot };
	}
	[[nodiscard]] senp::ManagementOperationResult Refresh() override
	{
		return { senp::EManagementOperationStatus::Succeeded, snapshot };
	}
	void Stop() noexcept override {}
	[[nodiscard]] senp::ManagementSnapshot Snapshot() const override { return snapshot; }

	// A failed operation keeps the previous snapshot and carries the tool's
	// diagnostic, as the real service does.
	[[nodiscard]] senp::ManagementOperationResult Failure() const
	{
		auto failed = snapshot;
		failed.diagnostic = L"package store busy";
		return { senp::EManagementOperationStatus::Failed, std::move(failed) };
	}

	senp::ManagementSnapshot snapshot;
	bool failUninstall = false;
	std::wstring lastBuiltInInstall;
	std::wstring lastBuiltInUninstall;
	std::wstring lastEnabledId;
	std::wstring lastPackageUninstall;
	bool lastEnabledValue = false;
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

TEST(ExtensionsWorkbenchTool, AnInvalidListingRowGetsNoRowAction)
{
	const HWND parent = ::CreateWindowExW(0, L"STATIC", L"Extensions invalid row parent", WS_POPUP,
		0, 0, 800, 600, nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
	ASSERT_NE(nullptr, parent);
	WindowGuard parentGuard(parent);

	workbench::extensions::CExtensionsWorkbenchTool tool;
	ASSERT_TRUE(tool.Create(parent));
	tool.Layout(RECT{ 0, 0, 480, 320 }, 96);
	FakeManagementService service;
	service.snapshot.state = senp::EManagementState::Ready;
	service.snapshot.extensions.push_back({
		.id = L"pkg-broken",
		.displayName = L"pkg-broken",
		.installed = true,
		.invalidCode = L"checksumMismatch",
		.invalidMessage = L"module/extension.wasm",
	});
	tool.SetManagementService(&service);
	// A row the package tool could not read, and that names no state file to
	// remove, has no row action at all: never a button that cannot work.
	EXPECT_EQ(nullptr, ::FindWindowExW(tool.GetHwnd(), nullptr, L"BUTTON", nullptr));
}

TEST(ExtensionsWorkbenchTool, AnInvalidListingRowShowsOnlyUninstall)
{
	const HWND parent = ::CreateWindowExW(0, L"STATIC", L"Extensions invalid row parent", WS_POPUP,
		0, 0, 800, 600, nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
	ASSERT_NE(nullptr, parent);
	WindowGuard parentGuard(parent);

	workbench::extensions::CExtensionsWorkbenchTool tool;
	ASSERT_TRUE(tool.Create(parent));
	tool.Layout(RECT{ 0, 0, 480, 320 }, 96);
	FakeManagementService service;
	service.snapshot.state = senp::EManagementState::Ready;
	service.snapshot.extensions.push_back({
		.id = L"pkg-broken",
		.displayName = L"pkg-broken",
		.installed = true,
		.invalidCode = L"checksumMismatch",
		.invalidMessage = L"module/extension.wasm",
		.stateName = L"pkg-broken.json",
	});
	tool.SetManagementService(&service);
	// The only action on an unreadable state row is Uninstall: never Enable
	// or Install.
	const auto uninstall = std::wstring(CSelectLang::LoadStringW(STR_WORKBENCH_EXTENSIONS_UNINSTALL));
	EXPECT_NE(nullptr, ::FindWindowExW(tool.GetHwnd(), nullptr, L"BUTTON", uninstall.c_str()));
	const auto enable = std::wstring(CSelectLang::LoadStringW(STR_WORKBENCH_EXTENSIONS_ENABLE));
	const auto install = std::wstring(CSelectLang::LoadStringW(STR_WORKBENCH_EXTENSIONS_INSTALL));
	EXPECT_EQ(nullptr, ::FindWindowExW(tool.GetHwnd(), nullptr, L"BUTTON", enable.c_str()));
	EXPECT_EQ(nullptr, ::FindWindowExW(tool.GetHwnd(), nullptr, L"BUTTON", install.c_str()));
}

TEST(ExtensionsWorkbenchTool, UninstallsAnInvalidRowByItsStateFileName)
{
	const HWND parent = ::CreateWindowExW(0, L"STATIC", L"Extensions invalid uninstall parent", WS_POPUP,
		0, 0, 800, 600, nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
	ASSERT_NE(nullptr, parent);
	WindowGuard parentGuard(parent);

	workbench::extensions::CExtensionsWorkbenchTool tool;
	ASSERT_TRUE(tool.Create(parent));
	tool.Layout(RECT{ 0, 0, 480, 320 }, 96);
	FakeManagementService service;
	service.snapshot.state = senp::EManagementState::Ready;
	service.snapshot.extensions.push_back({
		.id = L"pkg-broken",
		.displayName = L"pkg-broken",
		.installed = true,
		.invalidCode = L"checksumMismatch",
		.stateName = L"pkg-broken.json",
	});
	tool.SetManagementService(&service);
	int modals = 0;
	tool.SetMessageBoxHook([&modals](HWND, const std::wstring&, const std::wstring&, UINT) {
		++modals;
		return IDOK;
	});

	const auto uninstall = std::wstring(CSelectLang::LoadStringW(STR_WORKBENCH_EXTENSIONS_UNINSTALL));
	const HWND button = ::FindWindowExW(tool.GetHwnd(), nullptr, L"BUTTON", uninstall.c_str());
	ASSERT_NE(nullptr, button);
	(void)::SendMessageW(button, BM_CLICK, 0, 0);
	// VS Code uninstalls on the click: no confirmation. The service is invoked
	// with the state file name, never the manifest id of a corrupt state row.
	EXPECT_EQ(0, modals);
	EXPECT_EQ(L"pkg-broken.json", service.lastPackageUninstall);
	EXPECT_EQ(nullptr, ::FindWindowExW(tool.GetHwnd(), nullptr, L"BUTTON", nullptr));
}

TEST(ExtensionsWorkbenchTool, AnInstalledDeveloperRowShowsDisableAndUninstall)
{
	const HWND parent = ::CreateWindowExW(0, L"STATIC", L"Extensions dev row parent", WS_POPUP,
		0, 0, 800, 600, nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
	ASSERT_NE(nullptr, parent);
	WindowGuard parentGuard(parent);

	workbench::extensions::CExtensionsWorkbenchTool tool;
	ASSERT_TRUE(tool.Create(parent));
	tool.Layout(RECT{ 0, 0, 480, 320 }, 96);
	FakeManagementService service;
	service.snapshot.state = senp::EManagementState::Ready;
	service.snapshot.extensions.push_back({
		.id = L"dev-pkg",
		.displayName = L"Dev Package",
		.installed = true,
		.enabled = true,
		.trust = L"developer",
	});
	tool.SetManagementService(&service);
	// VS Code parity: an installed extension offers Disable and Uninstall.
	const auto disable = std::wstring(CSelectLang::LoadStringW(STR_WORKBENCH_EXTENSIONS_DISABLE));
	const auto enable = std::wstring(CSelectLang::LoadStringW(STR_WORKBENCH_EXTENSIONS_ENABLE));
	const auto uninstall = std::wstring(CSelectLang::LoadStringW(STR_WORKBENCH_EXTENSIONS_UNINSTALL));
	EXPECT_NE(nullptr, ::FindWindowExW(tool.GetHwnd(), nullptr, L"BUTTON", disable.c_str()));
	EXPECT_NE(nullptr, ::FindWindowExW(tool.GetHwnd(), nullptr, L"BUTTON", uninstall.c_str()));
	EXPECT_EQ(nullptr, ::FindWindowExW(tool.GetHwnd(), nullptr, L"BUTTON", enable.c_str()));
}

TEST(ExtensionsWorkbenchTool, TogglesADeveloperPackageEnabledStateFromItsRowButton)
{
	const HWND parent = ::CreateWindowExW(0, L"STATIC", L"Extensions dev toggle parent", WS_POPUP,
		0, 0, 800, 600, nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
	ASSERT_NE(nullptr, parent);
	WindowGuard parentGuard(parent);

	workbench::extensions::CExtensionsWorkbenchTool tool;
	ASSERT_TRUE(tool.Create(parent));
	tool.Layout(RECT{ 0, 0, 480, 320 }, 96);
	FakeManagementService service;
	service.snapshot.state = senp::EManagementState::Ready;
	service.snapshot.extensions.push_back({
		.id = L"dev-pkg",
		.displayName = L"Dev Package",
		.installed = true,
		.enabled = true,
		.trust = L"developer",
	});
	tool.SetManagementService(&service);

	const auto disable = std::wstring(CSelectLang::LoadStringW(STR_WORKBENCH_EXTENSIONS_DISABLE));
	const HWND button = ::FindWindowExW(tool.GetHwnd(), nullptr, L"BUTTON", disable.c_str());
	ASSERT_NE(nullptr, button);
	(void)::SendMessageW(button, BM_CLICK, 0, 0);
	EXPECT_EQ(L"dev-pkg", service.lastEnabledId);
	EXPECT_FALSE(service.lastEnabledValue);
	// After disabling, the same slot flips to Enable.
	const auto enable = std::wstring(CSelectLang::LoadStringW(STR_WORKBENCH_EXTENSIONS_ENABLE));
	EXPECT_NE(nullptr, ::FindWindowExW(tool.GetHwnd(), nullptr, L"BUTTON", enable.c_str()));
}

TEST(ExtensionsWorkbenchTool, UninstallsADeveloperPackageWithTheStableIdStateName)
{
	const HWND parent = ::CreateWindowExW(0, L"STATIC", L"Extensions dev uninstall parent", WS_POPUP,
		0, 0, 800, 600, nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
	ASSERT_NE(nullptr, parent);
	WindowGuard parentGuard(parent);

	workbench::extensions::CExtensionsWorkbenchTool tool;
	ASSERT_TRUE(tool.Create(parent));
	tool.Layout(RECT{ 0, 0, 480, 320 }, 96);
	FakeManagementService service;
	service.snapshot.state = senp::EManagementState::Ready;
	service.snapshot.extensions.push_back({
		.id = L"dev-pkg",
		.displayName = L"Dev Package",
		.installed = true,
		.enabled = true,
		.trust = L"developer",
	});
	tool.SetManagementService(&service);
	int modals = 0;
	tool.SetMessageBoxHook([&modals](HWND, const std::wstring&, const std::wstring&, UINT) {
		++modals;
		return IDOK;
	});

	const auto uninstall = std::wstring(CSelectLang::LoadStringW(STR_WORKBENCH_EXTENSIONS_UNINSTALL));
	const HWND button = ::FindWindowExW(tool.GetHwnd(), nullptr, L"BUTTON", uninstall.c_str());
	ASSERT_NE(nullptr, button);
	(void)::SendMessageW(button, BM_CLICK, 0, 0);
	EXPECT_EQ(0, modals);
	// Uninstall for an installed package uses `<id>.json` as the state name.
	EXPECT_EQ(L"dev-pkg.json", service.lastPackageUninstall);
	EXPECT_EQ(nullptr, ::FindWindowExW(tool.GetHwnd(), nullptr, L"BUTTON", nullptr));
}

TEST(ExtensionsWorkbenchTool, AFailedUninstallKeepsTheRowAndReportsTheDiagnostic)
{
	const HWND parent = ::CreateWindowExW(0, L"STATIC", L"Extensions modal close parent", WS_POPUP,
		0, 0, 800, 600, nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
	ASSERT_NE(nullptr, parent);
	WindowGuard parentGuard(parent);

	auto tool = std::make_unique<workbench::extensions::CExtensionsWorkbenchTool>();
	ASSERT_TRUE(tool->Create(parent));
	tool->Layout(RECT{ 0, 0, 480, 320 }, 96);
	auto service = std::make_unique<FakeManagementService>();
	service->snapshot.state = senp::EManagementState::Ready;
	service->snapshot.extensions.push_back({
		.id = L"dev-pkg",
		.displayName = L"Dev Package",
		.installed = true,
		.enabled = true,
		.trust = L"developer",
	});
	service->failUninstall = true;
	auto* toolPtr = tool.get();
	tool->SetManagementService(service.get());
	int changed = 0;
	tool->SetExtensionsChangedCallback([&changed] { ++changed; });
	std::vector<std::pair<std::wstring, std::wstring>> modals;
	tool->SetMessageBoxHook([&modals](HWND, const std::wstring& message, const std::wstring& title, UINT) {
		modals.emplace_back(message, title);
		return IDOK;
	});

	const auto uninstall = std::wstring(CSelectLang::LoadStringW(STR_WORKBENCH_EXTENSIONS_UNINSTALL));
	const HWND button = ::FindWindowExW(toolPtr->GetHwnd(), nullptr, L"BUTTON", uninstall.c_str());
	ASSERT_NE(nullptr, button);
	(void)::SendMessageW(button, BM_CLICK, 0, 0);
	EXPECT_EQ(L"dev-pkg.json", service->lastPackageUninstall);
	ASSERT_EQ(1u, modals.size());
	EXPECT_EQ(L"package store busy", modals[0].first);
	EXPECT_EQ(std::wstring(CSelectLang::LoadStringW(STR_WORKBENCH_EXTENSIONS_UNINSTALL_FAILED)),
		modals[0].second);
	EXPECT_EQ(0, changed);
	// The previous snapshot stands: the row and its actions are still there.
	EXPECT_NE(nullptr, ::FindWindowExW(toolPtr->GetHwnd(), nullptr, L"BUTTON", uninstall.c_str()));
}

TEST(ExtensionsWorkbenchTool, AFailureModalThatClosesTheToolEndsTheUninstall)
{
	const HWND parent = ::CreateWindowExW(0, L"STATIC", L"Extensions modal close parent", WS_POPUP,
		0, 0, 800, 600, nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
	ASSERT_NE(nullptr, parent);
	WindowGuard parentGuard(parent);

	auto tool = std::make_unique<workbench::extensions::CExtensionsWorkbenchTool>();
	ASSERT_TRUE(tool->Create(parent));
	tool->Layout(RECT{ 0, 0, 480, 320 }, 96);
	auto service = std::make_unique<FakeManagementService>();
	service->snapshot.state = senp::EManagementState::Ready;
	service->snapshot.extensions.push_back({
		.id = L"dev-pkg",
		.displayName = L"Dev Package",
		.installed = true,
		.enabled = true,
		.trust = L"developer",
	});
	service->failUninstall = true;
	auto* toolPtr = tool.get();
	tool->SetManagementService(service.get());
	int changed = 0;
	tool->SetExtensionsChangedCallback([&changed] { ++changed; });
	// The failure report pumps messages: a Close() arriving there nulls the
	// service and destroys the window. Nothing after the modal may use either.
	int modals = 0;
	tool->SetMessageBoxHook([toolPtr, &modals](HWND, const std::wstring&, const std::wstring&, UINT) {
		++modals;
		toolPtr->Close();
		return IDOK;
	});

	const auto uninstall = std::wstring(CSelectLang::LoadStringW(STR_WORKBENCH_EXTENSIONS_UNINSTALL));
	const HWND button = ::FindWindowExW(toolPtr->GetHwnd(), nullptr, L"BUTTON", uninstall.c_str());
	ASSERT_NE(nullptr, button);
	(void)::SendMessageW(button, BM_CLICK, 0, 0);
	EXPECT_EQ(1, modals);
	EXPECT_EQ(L"dev-pkg.json", service->lastPackageUninstall);
	EXPECT_EQ(0, changed);
	EXPECT_EQ(nullptr, toolPtr->GetHwnd());
}

TEST(ExtensionsWorkbenchTool, AFailedBuiltInUninstallReportsThroughTheModalSeam)
{
	const HWND parent = ::CreateWindowExW(0, L"STATIC", L"Extensions built-in failure parent", WS_POPUP,
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
		.installed = true,
		.builtIn = true,
		.enabled = true,
	});
	service.failUninstall = true;
	tool.SetManagementService(&service);
	int changed = 0;
	tool.SetExtensionsChangedCallback([&changed] { ++changed; });
	// Built-in failures use the same seam as every other modal, so a Close()
	// during them falls under the same post-modal rule.
	std::vector<std::wstring> titles;
	tool.SetMessageBoxHook([&titles](HWND, const std::wstring&, const std::wstring& title, UINT) {
		titles.push_back(title);
		return IDOK;
	});

	const auto uninstall = std::wstring(CSelectLang::LoadStringW(STR_WORKBENCH_EXTENSIONS_UNINSTALL));
	const HWND button = ::FindWindowExW(tool.GetHwnd(), nullptr, L"BUTTON", uninstall.c_str());
	ASSERT_NE(nullptr, button);
	(void)::SendMessageW(button, BM_CLICK, 0, 0);
	EXPECT_EQ(L"sakura-indent-rainbow", service.lastBuiltInUninstall);
	ASSERT_EQ(1u, titles.size());
	EXPECT_EQ(std::wstring(CSelectLang::LoadStringW(STR_WORKBENCH_EXTENSIONS_UNINSTALL_FAILED)), titles[0]);
	EXPECT_EQ(0, changed);
}

TEST(ExtensionsWorkbenchTool, AModalDuringDeveloperInstallRunsNoServiceCall)
{
	DropViewFixture fixture;
	ASSERT_TRUE(fixture.Initialize());
	TemporaryPackageFile package(L".senp");
	ASSERT_TRUE(package.IsValid());
	auto* toolPtr = &fixture.tool;
	fixture.tool.SetMessageBoxHook([toolPtr](HWND, const std::wstring&, const std::wstring&, UINT) {
		// A Close() arriving while the confirmation modal pumps messages must
		// not let the handler reach the nulled service afterwards.
		toolPtr->Close();
		return IDYES;
	});
	// The drop path drives InstallDeveloperPackagePath's modal per package.
	const HWND view = toolPtr->GetHwnd();
	ASSERT_NE(nullptr, view);
	(void)SendDrop(view, { package.Path().wstring() });
	EXPECT_TRUE(fixture.service.developerInstallPaths.empty());
	EXPECT_TRUE(fixture.service.developerInstallEnabled.empty());
	EXPECT_EQ(nullptr, toolPtr->GetHwnd());
}

TEST(ExtensionsWorkbenchTool, RowStatusPaintsTheWindowExclusionBeforeThePackageState)
{
	using Tool = workbench::extensions::CExtensionsWorkbenchTool;
	using Exclusion = workbench::SenpWindowExtensionExclusion;
	using Kind = workbench::SenpWindowExtensionExclusionKind;
	const auto label = [](UINT id) { return std::wstring(CSelectLang::LoadStringW(id)); };

	senp::ExtensionDescriptor extension;
	extension.id = L"dev.extension";
	extension.installed = true;
	extension.enabled = true;
	extension.runtime.schemaVersion = 2;

	// No exclusion: the row reports the runtime compatibility a package listed
	// for another ABI carries, then the plain enable state.
	extension.runtime.compatible = false;
	EXPECT_EQ(label(STR_WORKBENCH_EXTENSIONS_STATUS_INCOMPATIBLE),
		Tool::ComposeExtensionRowStatus(extension, nullptr));
	extension.runtime.compatible = true;
	EXPECT_EQ(label(STR_WORKBENCH_EXTENSIONS_STATUS_ENABLED),
		Tool::ComposeExtensionRowStatus(extension, nullptr));
	extension.builtIn = true;
	EXPECT_EQ(label(STR_WORKBENCH_EXTENSIONS_STATUS_BUILT_IN),
		Tool::ComposeExtensionRowStatus(extension, nullptr));

	// An excluded package says why. An incompatible runtime and a declaration
	// conflict carry their own labels; other exclusions name a short reason.
	const Exclusion conflict{ Kind::Conflict, L"structure" };
	EXPECT_EQ(label(STR_WORKBENCH_EXTENSIONS_STATUS_CONFLICT),
		Tool::ComposeExtensionRowStatus(extension, &conflict));
	const Exclusion incompatible{ Kind::Incompatible, L"runtime" };
	EXPECT_EQ(label(STR_WORKBENCH_EXTENSIONS_STATUS_INCOMPATIBLE),
		Tool::ComposeExtensionRowStatus(extension, &incompatible));
	const Exclusion unsupported{ Kind::Unsupported, L"actions" };
	EXPECT_EQ(label(STR_WORKBENCH_EXTENSIONS_STATUS_UNSUPPORTED) + L": actions",
		Tool::ComposeExtensionRowStatus(extension, &unsupported));

	// An invalid listing row still wins over every other status.
	extension.invalidCode = L"checksumMismatch";
	EXPECT_TRUE(Tool::ComposeExtensionRowStatus(extension, &conflict)
		.starts_with(label(STR_WORKBENCH_EXTENSIONS_STATUS_INVALID)));

	// The painted row consumes the query the window hands it.
	const HWND parent = ::CreateWindowExW(0, L"STATIC", L"Extensions status test parent", WS_POPUP,
		0, 0, 800, 600, nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
	ASSERT_NE(nullptr, parent);
	WindowGuard parentGuard(parent);
	Tool tool;
	ASSERT_TRUE(tool.Create(parent));
	tool.Layout(RECT{ 0, 0, 480, 320 }, 96);
	FakeManagementService service;
	service.snapshot.state = senp::EManagementState::Ready;
	service.snapshot.extensions.push_back(extension);
	tool.SetManagementService(&service);
	int queries = 0;
	tool.SetExtensionExclusionQuery([&](std::wstring_view id) {
		++queries;
		EXPECT_EQ(L"dev.extension", id);
		return std::optional(unsupported);
	});
	MemorySurface surface(480, 320);
	ASSERT_TRUE(surface.IsValid());
	ASSERT_EQ(0, ::SendMessageW(tool.GetHwnd(), WM_PRINTCLIENT, reinterpret_cast<WPARAM>(surface.Dc()), 0));
	EXPECT_EQ(1, queries);
	tool.Close();
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

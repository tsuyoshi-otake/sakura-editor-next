/*! @file */
/*
Copyright (C) 2026, Sakura Editor Organization

SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"

#include "workbench/extensions/CExtensionsWorkbenchTool.h"

#include "CSelectLang.h"
#include "sakura_rc.h"
#include "senp/SenpManagementService.h"
#include "workbench/icons/CCodiconFont.h"
#include "workbench/icons/CodiconGlyphTable.h"
#include "workbench/icons/LabelRunPainter.h"

#include <algorithm>
#include <array>
#include <filesystem>

namespace workbench::extensions {
namespace {

constexpr wchar_t kWindowClass[] = L"SakuraSenpExtensionsView";
constexpr std::size_t kDeveloperPackagePathLimit = 32768;
constexpr UINT kMaximumDroppedDeveloperPackages = 16;
//! VS Code's extension list delegate publishes an exact 72px row height.
constexpr int kExtensionRowHeightDip = 72;
constexpr int kRowLeftDip = 16;
constexpr int kExtensionIconDip = 36;
constexpr int kExtensionIconGapDip = 16;
constexpr int kEmptyMessageHeightDip = 44;
constexpr int kDiagnosticHeightDip = 44;
constexpr int kEmptyMessageLeftDip = 20;
constexpr int kEmptyMessageRightDip = 12;
//! Keep row actions aligned with VS Code's compact `.extension-action` rules:
//! 14-DIP line height, 5-DIP horizontal padding, and a 1-DIP border.  Width is
//! derived from the rendered label rather than reserving a fixed rectangle.
constexpr int kActionButtonLineHeightDip = 14;
constexpr int kActionButtonHorizontalPaddingDip = 5;
constexpr int kActionButtonBorderDip = 1;
constexpr int kActionButtonRightDip = 10;
constexpr int kActionButtonBottomDip = 6;
constexpr int kActionButtonMetadataGapDip = 10;
constexpr int kActionButtonControlBase = 0x7200;
constexpr UINT_PTR kActionButtonSubclassId = 1;

int Scale(int value, unsigned int dpi) noexcept
{
	return ::MulDiv(value, static_cast<int>(dpi == 0 ? 96 : dpi), 96);
}

void Fill(HDC dc, const RECT& bounds, COLORREF color)
{
	const HBRUSH brush = ::CreateSolidBrush(color);
	if (brush == nullptr) return;
	::FillRect(dc, &bounds, brush);
	::DeleteObject(brush);
}

void Text(HDC dc, std::wstring_view value, RECT bounds, COLORREF color, HFONT font, UINT format)
{
	const HGDIOBJ oldFont = font == nullptr ? nullptr : ::SelectObject(dc, font);
	const int oldMode = ::SetBkMode(dc, TRANSPARENT);
	const COLORREF oldColor = ::SetTextColor(dc, color);
	::DrawTextW(dc, value.data(), static_cast<int>(value.size()), &bounds, format | DT_NOPREFIX);
	::SetTextColor(dc, oldColor);
	::SetBkMode(dc, oldMode);
	if (oldFont != nullptr) ::SelectObject(dc, oldFont);
}

std::wstring Localized(UINT resourceId)
{
	return std::wstring(CSelectLang::LoadStringW(resourceId));
}

std::wstring ActionLabel(bool installed)
{
	return Localized(installed
		? STR_WORKBENCH_EXTENSIONS_UNINSTALL : STR_WORKBENCH_EXTENSIONS_INSTALL);
}

std::wstring DeveloperPackageFilter()
{
	std::wstring filter = Localized(STR_WORKBENCH_EXTENSIONS_SENP_FILTER);
	filter.push_back(L'\0');
	filter.append(L"*.senp");
	filter.push_back(L'\0');
	filter.append(Localized(STR_DLGOPNFL_EXTNAME3));
	filter.append(L" (*.*)");
	filter.push_back(L'\0');
	filter.append(L"*.*");
	filter.push_back(L'\0');
	return filter;
}

void ShowLocalizedMessage(HWND owner, UINT messageId, UINT titleId, UINT flags) noexcept
{
	try {
		const auto message = Localized(messageId);
		const auto title = Localized(titleId);
		::MessageBoxW(owner, message.c_str(), title.c_str(), flags);
	}
	catch (...) {
		::OutputDebugStringW(L"Sakura Editor NEXT: localized Extensions message could not be shown.\n");
	}
}

int ActionButtonWidth(HDC dc, std::wstring_view label, HFONT font, unsigned int dpi) noexcept
{
	SIZE textSize{};
	if (dc != nullptr) {
		const HGDIOBJ previousFont = font == nullptr ? nullptr : ::SelectObject(dc, font);
		const BOOL measured = ::GetTextExtentPoint32W(
			dc, label.data(), static_cast<int>(label.size()), &textSize);
		if (previousFont != nullptr) ::SelectObject(dc, previousFont);
		if (measured == FALSE) textSize = {};
	}
	if (textSize.cx <= 0) {
		// The normal path always has the Chrome font, but keep the child usable if
		// font creation failed during a low-resource startup.
		textSize.cx = Scale(static_cast<int>(label.size()) * 8, dpi);
	}
	return textSize.cx + 2 * (Scale(kActionButtonHorizontalPaddingDip, dpi)
		+ Scale(kActionButtonBorderDip, dpi));
}

int ActionButtonHeight(unsigned int dpi) noexcept
{
	return Scale(kActionButtonLineHeightDip, dpi)
		+ 2 * Scale(kActionButtonBorderDip, dpi);
}

bool GetWindowBoundsInParent(HWND window, HWND parent, RECT& bounds) noexcept
{
	if (window == nullptr || parent == nullptr || ::GetWindowRect(window, &bounds) == FALSE) {
		return false;
	}
	::MapWindowPoints(nullptr, parent, reinterpret_cast<POINT*>(&bounds), 2);
	return true;
}

struct BuiltInLocalizationIds final {
	std::wstring_view extensionId;
	UINT displayName;
	UINT description;
};

constexpr std::array kBuiltInLocalizations{
	BuiltInLocalizationIds{ L"sakura-configuration-language-basics",
		STR_SENP_CONFIGURATION_LANGUAGE_BASICS_NAME,
		STR_SENP_CONFIGURATION_LANGUAGE_BASICS_DESCRIPTION },
	BuiltInLocalizationIds{ L"sakura-core-language-basics",
		STR_SENP_CORE_LANGUAGE_BASICS_NAME, STR_SENP_CORE_LANGUAGE_BASICS_DESCRIPTION },
	BuiltInLocalizationIds{ L"sakura-database-language-basics",
		STR_SENP_DATABASE_LANGUAGE_BASICS_NAME,
		STR_SENP_DATABASE_LANGUAGE_BASICS_DESCRIPTION },
	BuiltInLocalizationIds{ L"sakura-indent-rainbow",
		STR_SENP_INDENT_RAINBOW_NAME, STR_SENP_INDENT_RAINBOW_DESCRIPTION },
	BuiltInLocalizationIds{ L"sakura-infrastructure-language-basics",
		STR_SENP_INFRASTRUCTURE_LANGUAGE_BASICS_NAME,
		STR_SENP_INFRASTRUCTURE_LANGUAGE_BASICS_DESCRIPTION },
	BuiltInLocalizationIds{ L"sakura-legacy-language-basics",
		STR_SENP_LEGACY_LANGUAGE_BASICS_NAME, STR_SENP_LEGACY_LANGUAGE_BASICS_DESCRIPTION },
	BuiltInLocalizationIds{ L"sakura-shell-language-basics",
		STR_SENP_SHELL_LANGUAGE_BASICS_NAME, STR_SENP_SHELL_LANGUAGE_BASICS_DESCRIPTION },
};

std::wstring LocalizedBuiltInMetadata(const senp::ExtensionDescriptor& extension,
	std::wstring_view fallback, bool displayName)
{
	if (!extension.builtIn) return std::wstring(fallback);
	const auto found = std::ranges::find(kBuiltInLocalizations, extension.id,
		&BuiltInLocalizationIds::extensionId);
	if (found == kBuiltInLocalizations.end()) return std::wstring(fallback);
	const auto localized = Localized(displayName ? found->displayName : found->description);
	return localized.empty() ? std::wstring(fallback) : localized;
}

bool HasSenpExtension(std::wstring_view path)
{
	const auto separator = path.find_last_of(L"\\/");
	const auto dot = path.find_last_of(L'.');
	if (dot == std::wstring_view::npos
		|| (separator != std::wstring_view::npos && dot <= separator)
		|| path.size() - dot != 5) {
		return false;
	}
	constexpr std::wstring_view suffix = L".senp";
	for (std::size_t index = 0; index < suffix.size(); ++index) {
		wchar_t actual = path[dot + index];
		if (actual >= L'A' && actual <= L'Z') actual = static_cast<wchar_t>(actual + (L'a' - L'A'));
		if (actual != suffix[index]) return false;
	}
	return true;
}

bool IsRegularSenpFile(std::wstring_view path)
{
	if (!HasSenpExtension(path)) return false;
	std::error_code error;
	return std::filesystem::is_regular_file(std::filesystem::path(path), error) && !error;
}

class DropFilesGuard final {
public:
	explicit DropFilesGuard(HDROP drop) noexcept : m_drop(drop) {}
	~DropFilesGuard()
	{
		if (m_drop != nullptr) ::DragFinish(m_drop);
	}
	DropFilesGuard(const DropFilesGuard&) = delete;
	DropFilesGuard& operator=(const DropFilesGuard&) = delete;

	[[nodiscard]] HDROP Get() const noexcept { return m_drop; }

private:
	HDROP m_drop = nullptr;
};

} // namespace

CExtensionsWorkbenchTool::~CExtensionsWorkbenchTool()
{
	Close();
}

bool CExtensionsWorkbenchTool::Create(HWND parent)
{
	if (parent == nullptr || m_window != nullptr) return false;
	WNDCLASSEXW windowClass{ sizeof(windowClass) };
	windowClass.lpfnWndProc = WindowProc;
	windowClass.hInstance = G_AppInstance();
	windowClass.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
	windowClass.lpszClassName = kWindowClass;
	if (::RegisterClassExW(&windowClass) == 0 && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
	m_parent = parent;
	m_window = ::CreateWindowExW(0, kWindowClass, L"", WS_CHILD | WS_CLIPCHILDREN,
		0, 0, 0, 0, parent, nullptr, G_AppInstance(), this);
	if (m_window == nullptr) return false;
	::DragAcceptFiles(m_window, TRUE);
	if (!m_scrollbar.Create(m_window, m_window, [this](int pixelOffset) {
		ScrollTo(pixelOffset);
	}, controls::OverlayScrollbarSource::ExplicitModel)) {
		Close();
		::SetLastError(ERROR_NOT_ENOUGH_MEMORY);
		return false;
	}
	(void)m_font.RecreateForWindow(theme::ThemeFontKind::Chrome, m_window);
	RecreateDerivedFonts();
	ApplyAppearance();
	ApplyLayout();
	return true;
}

void CExtensionsWorkbenchTool::Layout(const RECT& contentRect, unsigned int dpi)
{
	m_bounds = contentRect;
	m_dpi = dpi == 0 ? 96 : dpi;
	if (m_font.Dpi() != m_dpi) {
		(void)m_font.Recreate(theme::ThemeFontKind::Chrome, m_dpi);
		RecreateDerivedFonts();
		ApplyAppearance();
	}
	ApplyLayout();
}

void CExtensionsWorkbenchTool::ApplyLayout()
{
	if (m_window == nullptr) return;
	const int width = std::max<LONG>(0, m_bounds.right - m_bounds.left);
	const int height = std::max<LONG>(0, m_bounds.bottom - m_bounds.top);
	::SetWindowPos(m_window, nullptr, m_bounds.left, m_bounds.top, width, height,
		SWP_NOACTIVATE | SWP_NOZORDER | (m_visible ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
	UpdateContentMetrics();
	SyncActionButtons();
	::InvalidateRect(m_window, nullptr, FALSE);
}

void CExtensionsWorkbenchTool::Activate()
{
	m_active = true;
	if (!m_actionButtons.empty()) ::SetFocus(m_actionButtons.front().window);
	else if (m_window != nullptr) ::SetFocus(m_window);
	Refresh();
}

void CExtensionsWorkbenchTool::Deactivate()
{
	m_active = false;
}

bool CExtensionsWorkbenchTool::PreTranslateMessage(MSG& message)
{
	return m_active && m_window != nullptr
		&& (message.hwnd == m_window || ::IsChild(m_window, message.hwnd))
		&& ::IsDialogMessageW(m_window, &message) != FALSE;
}

void CExtensionsWorkbenchTool::Close()
{
	m_service = nullptr;
	m_extensionsChanged = {};
	DestroyActionButtons();
	m_scrollbar.Destroy();
	if (m_window != nullptr) {
		::DragAcceptFiles(m_window, FALSE);
		::DestroyWindow(m_window);
	}
	m_window = nullptr;
	m_parent = nullptr;
	m_scrollOffset = 0;
	m_contentHeight = 0;
	m_active = false;
	ReleaseDerivedFonts();
}

void CExtensionsWorkbenchTool::SetVisible(bool visible) noexcept
{
	m_visible = visible;
	if (m_window != nullptr) ::ShowWindow(m_window, visible ? SW_SHOWNA : SW_HIDE);
}

bool CExtensionsWorkbenchTool::Reparent(HWND parent) noexcept
{
	if (m_window == nullptr || parent == nullptr) return false;
	if (::GetParent(m_window) != parent && ::SetParent(m_window, parent) == nullptr) return false;
	m_parent = parent;
	ApplyLayout();
	return true;
}

void CExtensionsWorkbenchTool::SetPalette(const theme::ThemePalette& palette)
{
	m_palette = palette;
	ApplyAppearance();
}

void CExtensionsWorkbenchTool::ApplyAppearance()
{
	for (const auto& button : m_actionButtons) {
		::SendMessageW(button.window, WM_SETFONT, reinterpret_cast<WPARAM>(m_font.Get()), TRUE);
		::InvalidateRect(button.window, nullptr, FALSE);
	}
	if (m_window != nullptr) ::InvalidateRect(m_window, nullptr, FALSE);
}

void CExtensionsWorkbenchTool::RecreateDerivedFonts()
{
	ReleaseDerivedFonts();
	const auto faceName = workbench::icons::CCodiconFont::Instance().FaceName();
	if (!faceName.empty()) {
		m_extensionIconFont = workbench::icons::CreateLabelRunGlyphFont(
			faceName, Scale(kExtensionIconDip, m_dpi));
	}
}

void CExtensionsWorkbenchTool::ReleaseDerivedFonts() noexcept
{
	if (m_extensionIconFont != nullptr) ::DeleteObject(m_extensionIconFont);
	m_extensionIconFont = nullptr;
}

void CExtensionsWorkbenchTool::SetManagementService(senp::ISenpManagementService* service) noexcept
{
	m_service = service;
	Refresh();
}

void CExtensionsWorkbenchTool::SetExtensionsChangedCallback(std::function<void()> callback)
{
	m_extensionsChanged = std::move(callback);
}

void CExtensionsWorkbenchTool::Refresh()
{
	if (m_window == nullptr) return;
	UpdateContentMetrics();
	SyncActionButtons();
	::InvalidateRect(m_window, nullptr, FALSE);
}

void CExtensionsWorkbenchTool::RefreshStrings()
{
	Refresh();
}

void CExtensionsWorkbenchTool::ScrollTo(int offset)
{
	if (m_window == nullptr) return;
	RECT client{};
	::GetClientRect(m_window, &client);
	const int viewportHeight = static_cast<int>(std::max<LONG>(0, client.bottom));
	const int maximum = std::max(0, m_contentHeight - viewportHeight);
	const int next = std::clamp(offset, 0, maximum);
	if (next == m_scrollOffset) return;
	m_scrollOffset = next;
	UpdateContentMetrics();
	SyncActionButtons();
	::InvalidateRect(m_window, nullptr, FALSE);
}

void CExtensionsWorkbenchTool::UpdateScrollBar(int viewportHeight)
{
	if (m_window == nullptr) return;
	const int maximum = std::max(0, m_contentHeight - std::max(0, viewportHeight));
	m_scrollOffset = std::clamp(m_scrollOffset, 0, maximum);
	RECT client{};
	::GetClientRect(m_window, &client);
	m_scrollbar.SetDpi(m_dpi);
	m_scrollbar.SetBounds(client);
	m_scrollbar.SetColors(controls::ResolveOverlayScrollbarColors(m_palette, m_palette.sideBar));
	m_scrollbar.SetScrollModel(controls::OverlayScrollbarModel{
		.contentExtent = m_contentHeight,
		.viewportExtent = std::max(0, viewportHeight),
		.offset = m_scrollOffset,
	});
	m_scrollbar.Update();
}

void CExtensionsWorkbenchTool::UpdateContentMetrics()
{
	if (m_window == nullptr) return;
	RECT client{};
	::GetClientRect(m_window, &client);
	const auto snapshot = m_service == nullptr ? senp::ManagementSnapshot{} : m_service->Snapshot();
	const int diagnosticHeight = snapshot.diagnostic.empty() ? 0 : Scale(kDiagnosticHeightDip, m_dpi);
	const int rowsHeight = snapshot.extensions.empty() ? Scale(kEmptyMessageHeightDip, m_dpi)
		: static_cast<int>(snapshot.extensions.size()) * Scale(kExtensionRowHeightDip, m_dpi);
	m_contentHeight = diagnosticHeight + rowsHeight;
	const int viewportHeight = static_cast<int>(std::max<LONG>(0, client.bottom));
	UpdateScrollBar(viewportHeight);
}

void CExtensionsWorkbenchTool::SyncActionButtons()
{
	if (m_window == nullptr) return;
	const auto snapshot = m_service == nullptr ? senp::ManagementSnapshot{} : m_service->Snapshot();
	RECT client{};
	::GetClientRect(m_window, &client);
	const int diagnosticHeight = snapshot.diagnostic.empty() ? 0 : Scale(kDiagnosticHeightDip, m_dpi);
	const int rowHeight = Scale(kExtensionRowHeightDip, m_dpi);
	const int buttonHeight = ActionButtonHeight(m_dpi);
	const int right = Scale(kActionButtonRightDip, m_dpi);
	const int bottom = Scale(kActionButtonBottomDip, m_dpi);
	struct PreviousBounds final {
		HWND window = nullptr;
		RECT bounds{};
	};
	std::vector<PreviousBounds> previousBounds;
	previousBounds.reserve(m_actionButtons.size());
	for (const auto& button : m_actionButtons) {
		RECT bounds{};
		if (GetWindowBoundsInParent(button.window, m_window, bounds)) {
			previousBounds.push_back({ button.window, bounds });
		}
	}
	std::vector<ActionButtonState> next;
	for (std::size_t index = 0; index < snapshot.extensions.size(); ++index) {
		const auto& extension = snapshot.extensions[index];
		if (!extension.builtIn) continue;
		const auto action = extension.installed ? ERowAction::Uninstall : ERowAction::Install;
		auto existing = std::ranges::find(m_actionButtons, extension.id,
			&ActionButtonState::extensionId);
		ActionButtonState state;
		if (existing != m_actionButtons.end()) {
			state = *existing;
		} else {
			state.extensionId = extension.id;
			state.window = ::CreateWindowExW(0, L"BUTTON", L"",
				WS_CHILD | WS_TABSTOP | BS_OWNERDRAW,
				0, 0, 0, 0, m_window, nullptr, G_AppInstance(), nullptr);
			if (state.window == nullptr
				|| ::SetWindowSubclass(state.window, ActionButtonSubclassProc,
					kActionButtonSubclassId, reinterpret_cast<DWORD_PTR>(this)) == FALSE) {
				if (state.window != nullptr) ::DestroyWindow(state.window);
				continue;
			}
		}
		state.rowIndex = index;
		state.action = action;
		::SetWindowTextW(state.window, ActionLabel(action == ERowAction::Uninstall).data());
		const int controlId = kActionButtonControlBase + static_cast<int>(next.size());
		::SetWindowLongPtrW(state.window, GWLP_ID, controlId);
		::SendMessageW(state.window, WM_SETFONT, reinterpret_cast<WPARAM>(m_font.Get()), TRUE);
		next.push_back(std::move(state));
	}
	for (auto& obsolete : m_actionButtons) {
		const auto retained = std::ranges::find(next, obsolete.window, &ActionButtonState::window);
		if (retained == next.end() && obsolete.window != nullptr) ::DestroyWindow(obsolete.window);
	}
	m_actionButtons = std::move(next);
	HDC measureDc = ::GetDC(m_window);
	for (const auto& button : m_actionButtons) {
		const int rowTop = diagnosticHeight + static_cast<int>(button.rowIndex) * rowHeight
			- m_scrollOffset;
		const int top = rowTop + rowHeight - bottom - buttonHeight;
		const int buttonWidth = ActionButtonWidth(
			measureDc, ActionLabel(button.action == ERowAction::Uninstall), m_font.Get(), m_dpi);
		const int left = std::max<LONG>(client.left, client.right - right - buttonWidth);
		const bool visible = top < client.bottom && top + buttonHeight > client.top;
		::SetWindowPos(button.window, HWND_TOP, left, top, buttonWidth, buttonHeight,
			SWP_NOACTIVATE | SWP_NOCOPYBITS | (visible ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
		if (visible) {
			::RedrawWindow(button.window, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
		}
	}
	if (measureDc != nullptr) ::ReleaseDC(m_window, measureDc);
	// A label transition changes the child width (and can also move its left
	// edge).  Explicitly invalidate both rectangles after the child has moved;
	// invalidating only the child leaves the part of the old rectangle exposed
	// to the parent with stale pixels on some Win32 paint paths.
	for (const auto& old : previousBounds) {
		::InvalidateRect(m_window, &old.bounds, FALSE);
	}
	for (const auto& button : m_actionButtons) {
		RECT bounds{};
		if (GetWindowBoundsInParent(button.window, m_window, bounds)) {
			::InvalidateRect(m_window, &bounds, FALSE);
		}
	}
}

void CExtensionsWorkbenchTool::DestroyActionButtons() noexcept
{
	for (auto& button : m_actionButtons) {
		if (button.window != nullptr && ::IsWindow(button.window)) ::DestroyWindow(button.window);
		button.window = nullptr;
	}
	m_actionButtons.clear();
}

CExtensionsWorkbenchTool::ActionButtonState* CExtensionsWorkbenchTool::FindActionButton(
	HWND window) noexcept
{
	const auto found = std::ranges::find(m_actionButtons, window, &ActionButtonState::window);
	return found == m_actionButtons.end() ? nullptr : &*found;
}

void CExtensionsWorkbenchTool::InstallBuiltIn(std::wstring_view extensionId)
{
	if (m_service == nullptr || extensionId.empty()) return;
	const auto result = m_service->InstallBuiltInPackage(extensionId);
	if (!result.Succeeded()) {
		const std::wstring message = result.snapshot.diagnostic.empty()
			? Localized(STR_WORKBENCH_EXTENSIONS_BUILTIN_INSTALL_FAILED)
			: result.snapshot.diagnostic;
		const auto title = Localized(STR_WORKBENCH_EXTENSIONS_INSTALL_FAILED);
		::MessageBoxW(m_window, message.c_str(), title.c_str(), MB_OK | MB_ICONERROR);
	}
	else if (m_extensionsChanged) {
		m_extensionsChanged();
	}
	Refresh();
}

void CExtensionsWorkbenchTool::UninstallBuiltIn(std::wstring_view extensionId)
{
	if (m_service == nullptr || extensionId.empty()) return;
	const auto result = m_service->UninstallBuiltInPackage(extensionId);
	if (!result.Succeeded()) {
		const std::wstring message = result.snapshot.diagnostic.empty()
			? Localized(STR_WORKBENCH_EXTENSIONS_BUILTIN_UNINSTALL_FAILED)
			: result.snapshot.diagnostic;
		const auto title = Localized(STR_WORKBENCH_EXTENSIONS_UNINSTALL_FAILED);
		::MessageBoxW(m_window, message.c_str(), title.c_str(), MB_OK | MB_ICONERROR);
	}
	else if (m_extensionsChanged) {
		m_extensionsChanged();
	}
	Refresh();
}

void CExtensionsWorkbenchTool::InstallDeveloperPackage()
{
	if (m_service == nullptr || m_window == nullptr) return;
	std::array<wchar_t, kDeveloperPackagePathLimit> path{};
	const auto filter = DeveloperPackageFilter();
	const auto title = Localized(STR_WORKBENCH_EXTENSIONS_INSTALL_FROM_SENP);
	OPENFILENAMEW picker{ sizeof(picker) };
	picker.hwndOwner = m_window;
	picker.lpstrFilter = filter.c_str();
	picker.lpstrTitle = title.c_str();
	picker.lpstrFile = path.data();
	picker.nMaxFile = static_cast<DWORD>(path.size());
	picker.lpstrDefExt = L"senp";
	picker.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
	if (!::GetOpenFileNameW(&picker)) return;
	(void)InstallDeveloperPackagePath(path.data());
}

CExtensionsWorkbenchTool::EDeveloperPackageInstallResult
CExtensionsWorkbenchTool::InstallDeveloperPackagePath(std::wstring_view packagePath)
{
	if (m_service == nullptr || m_window == nullptr || packagePath.empty()) {
		return EDeveloperPackageInstallResult::Failed;
	}
	const auto confirmation = Localized(STR_WORKBENCH_EXTENSIONS_DEVELOPER_WARNING);
	const auto confirmationTitle = Localized(STR_WORKBENCH_EXTENSIONS_DEVELOPER_WARNING_TITLE);
	const int choice = ::MessageBoxW(m_window,
		confirmation.c_str(), confirmationTitle.c_str(),
		MB_YESNOCANCEL | MB_ICONWARNING | MB_DEFBUTTON3);
	if (choice == IDCANCEL) return EDeveloperPackageInstallResult::Cancelled;
	const auto result = m_service->InstallDeveloperPackage(packagePath, choice == IDYES);
	if (!result.Succeeded()) {
		const std::wstring message = result.snapshot.diagnostic.empty()
			? Localized(STR_WORKBENCH_EXTENSIONS_PACKAGE_REJECTED) : result.snapshot.diagnostic;
		const auto failureTitle = Localized(STR_WORKBENCH_EXTENSIONS_INSTALL_FAILED);
		::MessageBoxW(m_window, message.c_str(), failureTitle.c_str(), MB_OK | MB_ICONERROR);
		Refresh();
		return EDeveloperPackageInstallResult::Failed;
	}
	if (m_extensionsChanged) {
		m_extensionsChanged();
	}
	Refresh();
	return EDeveloperPackageInstallResult::Succeeded;
}

void CExtensionsWorkbenchTool::HandleDroppedFiles(HDROP drop) noexcept
{
	if (m_service == nullptr || m_window == nullptr || drop == nullptr) return;
	try {
		const UINT count = ::DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
		if (count == 0) {
			ShowLocalizedMessage(m_window, STR_WORKBENCH_EXTENSIONS_DROP_UNSUPPORTED,
				STR_WORKBENCH_EXTENSIONS_DROP_UNSUPPORTED_TITLE, MB_OK | MB_ICONWARNING);
			return;
		}
		if (count > kMaximumDroppedDeveloperPackages) {
			ShowLocalizedMessage(m_window, STR_WORKBENCH_EXTENSIONS_DROP_TOO_MANY,
				STR_WORKBENCH_EXTENSIONS_DROP_TOO_MANY_TITLE, MB_OK | MB_ICONWARNING);
			return;
		}

		std::vector<std::wstring> paths;
		paths.reserve(count);
		for (UINT index = 0; index < count; ++index) {
			const UINT required = ::DragQueryFileW(drop, index, nullptr, 0);
			if (required == 0 || required >= kDeveloperPackagePathLimit) {
				ShowLocalizedMessage(m_window, STR_WORKBENCH_EXTENSIONS_DROP_UNSUPPORTED,
					STR_WORKBENCH_EXTENSIONS_DROP_UNSUPPORTED_TITLE,
					MB_OK | MB_ICONWARNING);
				return;
			}
			std::wstring path(required + 1, L'\0');
			const UINT copied = ::DragQueryFileW(drop, index, path.data(), required + 1);
			if (copied != required) {
				ShowLocalizedMessage(m_window, STR_WORKBENCH_EXTENSIONS_DROP_UNSUPPORTED,
					STR_WORKBENCH_EXTENSIONS_DROP_UNSUPPORTED_TITLE,
					MB_OK | MB_ICONWARNING);
				return;
			}
			path.resize(copied);
			if (!IsRegularSenpFile(path)) {
				ShowLocalizedMessage(m_window, STR_WORKBENCH_EXTENSIONS_DROP_UNSUPPORTED,
					STR_WORKBENCH_EXTENSIONS_DROP_UNSUPPORTED_TITLE,
					MB_OK | MB_ICONWARNING);
				return;
			}
			paths.push_back(std::move(path));
		}

		for (const auto& path : paths) {
			const auto result = InstallDeveloperPackagePath(path);
			if (result != EDeveloperPackageInstallResult::Succeeded) return;
		}
	} catch (...) {
		ShowLocalizedMessage(m_window, STR_WORKBENCH_EXTENSIONS_DROP_READ_FAILED,
			STR_WORKBENCH_EXTENSIONS_INSTALL_FAILED, MB_OK | MB_ICONWARNING);
	}
}

void CExtensionsWorkbenchTool::Paint(HDC dc)
{
	RECT client{};
	::GetClientRect(m_window, &client);
	Fill(dc, client, m_palette.sideBar.ToColorRef());
	UpdateContentMetrics();
	const int saved = ::SaveDC(dc);
	::IntersectClipRect(dc, client.left, client.top, client.right, client.bottom);
	int y = client.top - m_scrollOffset;
	const auto snapshot = m_service == nullptr ? senp::ManagementSnapshot{} : m_service->Snapshot();
	if (!snapshot.diagnostic.empty()) {
		RECT diagnostic{ client.left + Scale(kEmptyMessageLeftDip, m_dpi), y,
			client.right - Scale(kEmptyMessageRightDip, m_dpi), y + Scale(kDiagnosticHeightDip, m_dpi) };
		Text(dc, snapshot.diagnostic, diagnostic, m_palette.warning.ToColorRef(), m_font.Get(),
			DT_LEFT | DT_VCENTER | DT_WORDBREAK | DT_END_ELLIPSIS);
		y = diagnostic.bottom;
	}
	if (snapshot.extensions.empty()) {
		RECT empty{ client.left + Scale(kEmptyMessageLeftDip, m_dpi), y,
			client.right - Scale(kEmptyMessageRightDip, m_dpi),
			y + Scale(kEmptyMessageHeightDip, m_dpi) };
		const auto emptyText = Localized(STR_WORKBENCH_EXTENSIONS_EMPTY);
		Text(dc, emptyText, empty, m_palette.descriptionText.ToColorRef(), m_font.Get(),
			DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
	} else {
		for (const auto& extension : snapshot.extensions) {
			RECT row{ client.left, y, client.right, y + Scale(kExtensionRowHeightDip, m_dpi) };
			PaintExtensionRow(dc, row, extension);
			y = row.bottom;
		}
	}
	if (saved != 0) ::RestoreDC(dc, saved);
}

void CExtensionsWorkbenchTool::PaintExtensionRow(
	HDC dc, const RECT& bounds, const senp::ExtensionDescriptor& extension) const
{
	if (bounds.right <= bounds.left || bounds.bottom <= bounds.top) return;
	const bool deEmphasized = extension.installed && !extension.enabled;
	const COLORREF foreground = deEmphasized
		? m_palette.disabledText.ToColorRef() : m_palette.primaryText.ToColorRef();
	const COLORREF secondary = deEmphasized
		? m_palette.disabledText.ToColorRef() : m_palette.descriptionText.ToColorRef();
	const int rowLeft = Scale(kRowLeftDip, m_dpi);
	const int iconSide = Scale(kExtensionIconDip, m_dpi);
	RECT icon{ bounds.left + rowLeft,
		bounds.top + (bounds.bottom - bounds.top - iconSide) / 2,
		bounds.left + rowLeft + iconSide,
		bounds.top + (bounds.bottom - bounds.top + iconSide) / 2 };
	if (m_extensionIconFont != nullptr) {
		const auto glyph = workbench::icons::FindCodiconGlyph(L"extensions");
		if (glyph.has_value()) {
			const wchar_t iconText[] = { *glyph, L'\0' };
			Text(dc, iconText, icon, secondary, m_extensionIconFont,
				DT_CENTER | DT_VCENTER | DT_SINGLELINE);
		}
	}
	const LONG detailsLeft = icon.right + Scale(kExtensionIconGapDip, m_dpi);
	const LONG detailsRight = std::max(detailsLeft, bounds.right - Scale(10, m_dpi));
	RECT name{ detailsLeft, bounds.top + Scale(4, m_dpi), detailsRight,
		bounds.top + Scale(24, m_dpi) };
	const auto displayName = LocalizedBuiltInMetadata(extension, extension.displayName, true);
	// Keep extension names on the same Chrome font metrics and weight as the
	// Explorer and SCM rows. A derived semibold face made this one View read as
	// a larger type scale even though its nominal point size was unchanged.
	Text(dc, displayName, name, foreground, m_font.Get(),
		DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
	RECT description{ detailsLeft, bounds.top + Scale(23, m_dpi), detailsRight,
		bounds.top + Scale(45, m_dpi) };
	const auto descriptionText = LocalizedBuiltInMetadata(extension, extension.description, false);
	Text(dc, descriptionText, description, secondary, m_font.Get(),
		DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
	const std::wstring publisher = extension.publisher + L"  " + extension.version;
	const std::wstring status = !extension.installed ? L"" : extension.builtIn
		? Localized(STR_WORKBENCH_EXTENSIONS_STATUS_BUILT_IN)
		: Localized(extension.enabled
			? STR_WORKBENCH_EXTENSIONS_STATUS_ENABLED : STR_WORKBENCH_EXTENSIONS_STATUS_DISABLED);
	LONG metadataRight = detailsRight;
	if (extension.builtIn) {
		HDC measureDc = dc;
		const int actionWidth = ActionButtonWidth(
			measureDc, ActionLabel(extension.installed), m_font.Get(), m_dpi);
		metadataRight = std::max(detailsLeft, detailsRight
			- actionWidth - Scale(kActionButtonMetadataGapDip, m_dpi));
	}
	RECT statusBounds{ detailsLeft, bounds.top + Scale(45, m_dpi), metadataRight,
		bounds.bottom - Scale(3, m_dpi) };
	SIZE statusSize{};
	const HGDIOBJ previousFont = m_font.Get() == nullptr ? nullptr : ::SelectObject(dc, m_font.Get());
	(void)::GetTextExtentPoint32W(dc, status.c_str(), static_cast<int>(status.size()), &statusSize);
	if (previousFont != nullptr) ::SelectObject(dc, previousFont);
	statusBounds.left = std::max(statusBounds.left, statusBounds.right - statusSize.cx);
	if (!status.empty()) {
		Text(dc, status, statusBounds, secondary, m_font.Get(),
			DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
	}
	const LONG publisherRight = status.empty()
		? metadataRight : statusBounds.left - Scale(8, m_dpi);
	RECT publisherBounds{ detailsLeft, statusBounds.top,
		std::max(detailsLeft, publisherRight), statusBounds.bottom };
	Text(dc, publisher, publisherBounds, secondary, m_font.Get(),
		DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

void CExtensionsWorkbenchTool::PaintActionButton(
	HDC dc, HWND window, const RECT& bounds, UINT itemState) const
{
	if (dc == nullptr) return;
	const bool enabled = (itemState & ODS_DISABLED) == 0;
	const bool pressed = (itemState & ODS_SELECTED) != 0;
	const bool focused = (itemState & ODS_FOCUS) != 0
		&& (itemState & ODS_NOFOCUSRECT) == 0;
	bool hovered = false;
	ERowAction action = ERowAction::Install;
	for (const auto& button : m_actionButtons) {
		if (button.window == window) {
			hovered = button.hovered;
			action = button.action;
			break;
		}
	}
	const COLORREF background = !enabled ? m_palette.disabledText.ToColorRef()
		: (pressed || hovered ? m_palette.buttonHoverBackground : m_palette.buttonBackground).ToColorRef();
	Fill(dc, bounds, background);
	RECT label = bounds;
	if (pressed) ::OffsetRect(&label, 0, 1);
	const auto actionLabel = ActionLabel(action == ERowAction::Uninstall);
	Text(dc, actionLabel, label,
		m_palette.buttonForeground.ToColorRef(), m_font.Get(),
		DT_CENTER | DT_VCENTER | DT_SINGLELINE);
	if (!focused) return;
	RECT focus = bounds;
	::InflateRect(&focus, -Scale(2, m_dpi), -Scale(2, m_dpi));
	const HBRUSH border = ::CreateSolidBrush(m_palette.listFocusAndSelectionOutline.ToColorRef());
	if (border != nullptr) {
		::FrameRect(dc, &focus, border);
		::DeleteObject(border);
	}
}

LRESULT CALLBACK CExtensionsWorkbenchTool::WindowProc(
	HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
	auto* self = reinterpret_cast<CExtensionsWorkbenchTool*>(::GetWindowLongPtrW(window, GWLP_USERDATA));
	if (message == WM_NCCREATE) {
		const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
		self = static_cast<CExtensionsWorkbenchTool*>(create->lpCreateParams);
		::SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
	}
	return self == nullptr ? ::DefWindowProcW(window, message, wParam, lParam)
		: self->HandleMessage(window, message, wParam, lParam);
}

LRESULT CALLBACK CExtensionsWorkbenchTool::ActionButtonSubclassProc(
	HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR data)
{
	auto* self = reinterpret_cast<CExtensionsWorkbenchTool*>(data);
	if (message == WM_NCDESTROY) {
		(void)::RemoveWindowSubclass(window, ActionButtonSubclassProc, id);
		if (self != nullptr) {
			if (auto* state = self->FindActionButton(window); state != nullptr) state->window = nullptr;
		}
		return ::DefSubclassProc(window, message, wParam, lParam);
	}
	if (self == nullptr) return ::DefSubclassProc(window, message, wParam, lParam);
	auto paintButton = [self, window](HDC dc) {
		RECT bounds{};
		::GetClientRect(window, &bounds);
		UINT itemState = 0;
		if (::IsWindowEnabled(window) == FALSE) itemState |= ODS_DISABLED;
		if ((::SendMessageW(window, BM_GETSTATE, 0, 0) & BST_PUSHED) != 0) itemState |= ODS_SELECTED;
		if (::GetFocus() == window) itemState |= ODS_FOCUS;
		if ((::SendMessageW(window, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS) != 0) {
			itemState |= ODS_NOFOCUSRECT;
		}
		self->PaintActionButton(dc, window, bounds, itemState);
	};
	if (message == WM_ERASEBKGND) {
		paintButton(reinterpret_cast<HDC>(wParam));
		return 1;
	}
	if (message == WM_PAINT) {
		PAINTSTRUCT paint{};
		const HDC dc = ::BeginPaint(window, &paint);
		paintButton(dc);
		::EndPaint(window, &paint);
		return 0;
	}
	if (message == WM_PRINTCLIENT) {
		paintButton(reinterpret_cast<HDC>(wParam));
		return 0;
	}
	if (message == WM_MOUSEMOVE) {
		if (auto* state = self->FindActionButton(window); state != nullptr) {
			if (!state->hovered) {
				state->hovered = true;
				::InvalidateRect(window, nullptr, FALSE);
			}
			if (!state->trackingMouse) {
				TRACKMOUSEEVENT track{ sizeof(track), TME_LEAVE, window, 0 };
				state->trackingMouse = ::TrackMouseEvent(&track) != FALSE;
			}
		}
	} else if (message == WM_MOUSELEAVE) {
		if (auto* state = self->FindActionButton(window); state != nullptr) {
			state->hovered = false;
			state->trackingMouse = false;
			::InvalidateRect(window, nullptr, FALSE);
		}
	}
	const LRESULT result = ::DefSubclassProc(window, message, wParam, lParam);
	if (message == WM_SETFOCUS || message == WM_KILLFOCUS || message == WM_ENABLE) {
		::InvalidateRect(window, nullptr, FALSE);
	}
	return result;
}

LRESULT CExtensionsWorkbenchTool::HandleMessage(
	HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
	switch (message) {
	case WM_DRAWITEM: {
		const auto* draw = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
		if (draw != nullptr && FindActionButton(draw->hwndItem) != nullptr) {
			PaintActionButton(draw->hDC, draw->hwndItem, draw->rcItem, draw->itemState);
			return TRUE;
		}
		break;
	}
	case WM_COMMAND:
		if (HIWORD(wParam) == BN_CLICKED) {
			const HWND button = reinterpret_cast<HWND>(lParam);
			if (const auto* state = FindActionButton(button); state != nullptr) {
				const auto extensionId = state->extensionId;
				const auto action = state->action;
				if (action == ERowAction::Install) InstallBuiltIn(extensionId);
				else UninstallBuiltIn(extensionId);
				return 0;
			}
		}
		break;
	case WM_DROPFILES: {
		DropFilesGuard dropped(reinterpret_cast<HDROP>(wParam));
		HandleDroppedFiles(dropped.Get());
		return 0;
	}
	case WM_MOUSEWHEEL: {
		const int notches = GET_WHEEL_DELTA_WPARAM(wParam) / WHEEL_DELTA;
		ScrollTo(m_scrollOffset - notches * Scale(48, m_dpi));
		return 0;
	}
	case WM_ERASEBKGND:
		return 1;
	case WM_PAINT: {
		PAINTSTRUCT paint{};
		const HDC dc = ::BeginPaint(window, &paint);
		Paint(dc);
		::EndPaint(window, &paint);
		return 0;
	}
	case WM_PRINTCLIENT:
		Paint(reinterpret_cast<HDC>(wParam));
		return 0;
	case WM_NCDESTROY:
		::SetWindowLongPtrW(window, GWLP_USERDATA, 0);
		break;
	default:
		break;
	}
	return ::DefWindowProcW(window, message, wParam, lParam);
}

} // namespace workbench::extensions

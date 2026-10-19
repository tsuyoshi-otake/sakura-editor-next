/*! @file */
/*
Copyright (C) 2026, Sakura Editor Organization

SPDX-License-Identifier: Zlib
*/
#pragma once

#include "theme/CThemeService.h"
#include "workbench/IWorkbenchTool.h"
#include "workbench/controls/COverlayScrollbar.h"

#include <shellapi.h>

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace senp {
class ISenpManagementService;
struct ExtensionDescriptor;
}

namespace workbench {
struct SenpWindowExtensionExclusion;
}

namespace workbench::extensions {

//! Native projection of the first-party SENP extensions View. Package validation and
//! activation remain in SENP services; this class owns only HWND state.
class CExtensionsWorkbenchTool final : public IWorkbenchTool {
public:
	CExtensionsWorkbenchTool() = default;
	~CExtensionsWorkbenchTool() override;
	CExtensionsWorkbenchTool(const CExtensionsWorkbenchTool&) = delete;
	CExtensionsWorkbenchTool& operator=(const CExtensionsWorkbenchTool&) = delete;

	bool Create(HWND parent) override;
	void Layout(const RECT& contentRect, unsigned int dpi) override;
	void Activate() override;
	void Deactivate() override;
	bool PreTranslateMessage(MSG& message) override;
	void Close() override;
	void SetVisible(bool visible) noexcept;
	bool Reparent(HWND parent) noexcept;
	void SetPalette(const theme::ThemePalette& palette);
	void SetManagementService(senp::ISenpManagementService* service) noexcept;
	//! Test seam replacing MessageBoxW: hooks can answer programmatically and
	//! simulate a Close() mid-modal to prove post-modal re-validation. An empty
	//! hook keeps the real modal.
	void SetMessageBoxHook(
		std::function<int(HWND, const std::wstring&, const std::wstring&, UINT)> hook) noexcept
	{
		m_messageBox = std::move(hook);
	}
	//! Every modal call goes through this so the hook can replace MessageBoxW
	//! and so post-modal re-validation has one choke point.
	int ShowModalMessageBox(const std::wstring& message, const std::wstring& title, UINT flags);
	//! The window's package composition reports which snapshot packages it
	//! excluded and why; the row paints that reason instead of a bare state.
	void SetExtensionExclusionQuery(
		std::function<std::optional<workbench::SenpWindowExtensionExclusion>(std::wstring_view)> query);
	//! The window's extension-host runtime reports its own diagnostic (a
	//! repeatedly crashed, retired identity). It joins the management
	//! diagnostic on the same banner the View already paints.
	void SetRuntimeDiagnosticQuery(std::function<std::wstring()> query);
	//! The status a row shows: the invalid-listing marker, then this window's
	//! exclusion, then an incompatible runtime, then the install/enable state.
	[[nodiscard]] static std::wstring ComposeExtensionRowStatus(
		const senp::ExtensionDescriptor& extension,
		const workbench::SenpWindowExtensionExclusion* exclusion);
	//! Notifies the owning editor after a successful package-state transition so
	//! its asynchronous decoration cache is requested again on the next paint.
	void SetExtensionsChangedCallback(std::function<void()> callback);
	void Refresh();
	void RefreshStrings();
	//! Invokes the package-install command contributed to the Extensions
	//! ViewContainer title. The list surface never owns this action's placement.
	void InstallDeveloperPackage();
	[[nodiscard]] bool CanInstallDeveloperPackage() const noexcept { return m_service != nullptr; }

	[[nodiscard]] HWND GetHwnd() const noexcept { return m_window; }

	//! Row-action kinds: Install/Uninstall on built-ins; Enable/Disable plus
	//! Uninstall on installed non-built-ins; Uninstall only on invalid rows.
	enum class ERowAction {
		Install,
		Uninstall,
		Enable,
		Disable,
	};
	//! Which service operation a row action invokes: built-ins keep the
	//! tombstone flow, packages use the plain state removal, invalid rows are
	//! removed by their state file name.
	enum class ERowTarget {
		BuiltIn,
		Package,
		Invalid,
	};

private:
	struct ActionButtonState final {
		HWND window = nullptr;
		std::wstring extensionId;
		//! The `profiles/` state file name for Invalid targets (`<id>.json` is
		//! derived for Package targets).
		std::wstring stateName;
		std::size_t rowIndex = 0;
		ERowAction action = ERowAction::Install;
		ERowTarget target = ERowTarget::BuiltIn;
		bool hovered = false;
		bool trackingMouse = false;
	};
	enum class EDeveloperPackageInstallResult {
		Succeeded,
		Cancelled,
		Failed,
	};
	static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
	static LRESULT CALLBACK ActionButtonSubclassProc(HWND window, UINT message, WPARAM wParam,
		LPARAM lParam, UINT_PTR id, DWORD_PTR data);
	LRESULT HandleMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
	void ApplyLayout();
	void ApplyAppearance();
	void RecreateDerivedFonts();
	void ReleaseDerivedFonts() noexcept;
	void Paint(HDC dc);
	void PaintExtensionRow(HDC dc, const RECT& bounds,
		const senp::ExtensionDescriptor& extension) const;
	void PaintActionButton(HDC dc, HWND window, const RECT& bounds, UINT itemState) const;
	void SyncActionButtons();
	void DestroyActionButtons() noexcept;
	void InstallBuiltIn(std::wstring_view extensionId);
	void UninstallBuiltIn(std::wstring_view extensionId);
	void SetPackageEnabled(std::wstring_view extensionId, bool enabled);
	void UninstallPackage(const ActionButtonState& row);
	//! The service/window pair a modal may have torn down while pumping
	//! messages. Any path that continues after a ShowModalMessageBox return
	//! checks it again before touching the service.
	[[nodiscard]] bool IsUsable() const noexcept
	{
		return m_service != nullptr && m_window != nullptr && ::IsWindow(m_window) != FALSE;
	}
	//! The row actions a descriptor gets (VS Code parity): built-ins keep
	//! Install/Uninstall; installed non-built-ins get Enable/Disable plus
	//! Uninstall; invalid rows get only Uninstall by state name, and nothing
	//! when they name no state file.
	[[nodiscard]] static std::vector<std::pair<ERowAction, ERowTarget>> RowActions(
		const senp::ExtensionDescriptor& extension);
	[[nodiscard]] EDeveloperPackageInstallResult InstallDeveloperPackagePath(
		std::wstring_view packagePath);
	void HandleDroppedFiles(HDROP drop) noexcept;
	[[nodiscard]] ActionButtonState* FindActionButton(HWND window) noexcept;
	//! The banner's text: the management diagnostic first, then the runtime
	//! one; the single strip clips whatever will not fit.
	[[nodiscard]] std::wstring ComposeDiagnosticText(const std::wstring& management) const;
	void ScrollTo(int offset);
	void UpdateScrollBar(int viewportHeight);
	void UpdateContentMetrics();

	HWND m_parent = nullptr;
	HWND m_window = nullptr;
	RECT m_bounds{};
	unsigned int m_dpi = 96;
	theme::ThemePalette m_palette = theme::CThemeService::PaletteFor(theme::ThemeMode::Dark);
	theme::CThemeFont m_font;
	HFONT m_extensionIconFont = nullptr;
	controls::COverlayScrollbar m_scrollbar;
	std::vector<ActionButtonState> m_actionButtons;
	senp::ISenpManagementService* m_service = nullptr;
	std::function<std::optional<workbench::SenpWindowExtensionExclusion>(std::wstring_view)> m_exclusionQuery;
	std::function<std::wstring()> m_runtimeDiagnosticQuery;
	std::function<void()> m_extensionsChanged;
	std::function<int(HWND, const std::wstring&, const std::wstring&, UINT)> m_messageBox;
	int m_scrollOffset = 0;
	int m_contentHeight = 0;
	bool m_active = false;
	bool m_visible = true;
};

} // namespace workbench::extensions

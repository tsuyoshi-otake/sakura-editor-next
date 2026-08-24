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
#include <string>
#include <vector>

namespace senp {
class ISenpManagementService;
struct ExtensionDescriptor;
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

private:
	enum class ERowAction {
		Install,
		Uninstall,
	};
	struct ActionButtonState final {
		HWND window = nullptr;
		std::wstring extensionId;
		std::size_t rowIndex = 0;
		ERowAction action = ERowAction::Install;
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
	[[nodiscard]] EDeveloperPackageInstallResult InstallDeveloperPackagePath(
		std::wstring_view packagePath);
	void HandleDroppedFiles(HDROP drop) noexcept;
	[[nodiscard]] ActionButtonState* FindActionButton(HWND window) noexcept;
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
	std::function<void()> m_extensionsChanged;
	int m_scrollOffset = 0;
	int m_contentHeight = 0;
	bool m_active = false;
	bool m_visible = true;
};

} // namespace workbench::extensions

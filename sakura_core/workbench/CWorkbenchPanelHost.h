/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "theme/CThemeService.h"
#include "workbench/IWorkbenchTool.h"

#include <functional>
#include <memory>

namespace workbench {

//! Owns one CEditWnd-child host and the tool child hosted inside it.
class CWorkbenchPanelHost final {
public:
	using PersistExtentCallback = std::function<void(WorkbenchEdge edge, int extentDip)>;

	CWorkbenchPanelHost(WorkbenchEdge edge, int extentDip, PersistExtentCallback persistExtent = {});
	~CWorkbenchPanelHost();
	CWorkbenchPanelHost(const CWorkbenchPanelHost&) = delete;
	CWorkbenchPanelHost& operator=(const CWorkbenchPanelHost&) = delete;

	[[nodiscard]] bool Create(HWND parent, HINSTANCE instance, std::unique_ptr<IWorkbenchTool> tool);
	void Layout(const RECT& bounds, unsigned int dpi);
	void Show();
	void Hide();
	void ActivateTool();
	void SetPalette(const theme::ThemePalette& palette);
	//! Applies a shared extent without entering resize state or invoking persistence.
	void ApplyExtentDip(int extentDip);
	void BeginResize();
	void UpdateResize(int extentDip);
	void CommitResize();
	void CancelResize();
	[[nodiscard]] bool PreTranslateMessage(MSG& message);
	void Close();

	[[nodiscard]] HWND GetHwnd() const noexcept { return m_window; }
	[[nodiscard]] WorkbenchEdge GetEdge() const noexcept { return m_edge; }
	[[nodiscard]] WorkbenchPanelState GetState() const noexcept { return m_state; }
	[[nodiscard]] int GetExtentDip() const noexcept { return m_extentDip; }
	[[nodiscard]] int GetPendingExtentDip() const noexcept { return m_pendingExtentDip; }
	[[nodiscard]] unsigned int GetDpi() const noexcept { return m_dpi; }
	[[nodiscard]] int GetHeaderHeightPixels() const noexcept;
	static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

private:
	LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);
	void LayoutTool();
	void Paint();
	static int ClampExtent(int extentDip) noexcept;

	WorkbenchEdge m_edge;
	WorkbenchPanelState m_state = WorkbenchPanelState::Hidden;
	int m_extentDip = 0;
	int m_pendingExtentDip = 0;
	unsigned int m_dpi = 96;
	theme::ThemePalette m_palette = theme::CThemeService::PaletteFor(theme::ThemeMode::Dark);
	RECT m_bounds{};
	HWND m_window = nullptr;
	std::unique_ptr<IWorkbenchTool> m_tool;
	PersistExtentCallback m_persistExtent;
	bool m_closed = false;
};

} // namespace workbench

/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "theme/CThemeService.h"
#include "workbench/IWorkbenchTool.h"
#include "workbench/explorer/CExplorerTool.h"
#include "workbench/outline/COutlineWorkbenchTool.h"
#include "workbench/scm/CScmWorkbenchTool.h"

#include <functional>
#include <memory>

namespace workbench::explorer {

//! VS Code-like left sidebar containing Explorer and a collapsible Outline section.
class CExplorerOutlineTool final : public IWorkbenchTool {
public:
	using OutlineExpandedCallback = std::function<void(bool expanded)>;

	CExplorerOutlineTool(CDlgFuncList& dialog, OutlineExpandedCallback callback = {});
	~CExplorerOutlineTool() override;
	CExplorerOutlineTool(const CExplorerOutlineTool&) = delete;
	CExplorerOutlineTool& operator=(const CExplorerOutlineTool&) = delete;

	bool Create(HWND parent) override;
	void Layout(const RECT& contentRect, unsigned int dpi) override;
	void Activate() override;
	void Deactivate() override;
	bool PreTranslateMessage(MSG& message) override;
	void Close() override;

	void SetPalette(const theme::ThemePalette& palette);
	void SetOutlineExpanded(bool expanded, bool notify = false);
	void FocusOutline();
	void ShowSourceControl(bool show);
	[[nodiscard]] bool IsOutlineExpanded() const noexcept { return m_outlineExpanded; }
	[[nodiscard]] bool IsSourceControlVisible() const noexcept { return m_sourceControlVisible; }
	[[nodiscard]] CExplorerTool* Explorer() const noexcept { return m_explorer.get(); }
	[[nodiscard]] outline::COutlineWorkbenchTool* Outline() const noexcept { return m_outline.get(); }
	[[nodiscard]] scm::CScmWorkbenchTool* SourceControl() const noexcept { return m_scm.get(); }
	[[nodiscard]] static int OutlineHeaderHeightPixels(unsigned int dpi) noexcept;

	static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

private:
	LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);
	void LayoutChildren();
	void Paint();
	[[nodiscard]] bool IsOutlineHeaderPoint(POINT point) const noexcept;

	HWND m_window = nullptr;
	HINSTANCE m_instance = nullptr;
	std::unique_ptr<CExplorerTool> m_explorer;
	std::unique_ptr<outline::COutlineWorkbenchTool> m_outline;
	std::unique_ptr<scm::CScmWorkbenchTool> m_scm;
	OutlineExpandedCallback m_callback;
	theme::ThemePalette m_palette = theme::CThemeService::PaletteFor(theme::ThemeMode::Dark);
	theme::CThemeFont m_font;
	RECT m_bounds{};
	RECT m_outlineHeader{};
	unsigned int m_dpi = 96;
	bool m_outlineExpanded = true;
	bool m_sourceControlVisible = false;
	bool m_closed = false;
};

} // namespace workbench::explorer

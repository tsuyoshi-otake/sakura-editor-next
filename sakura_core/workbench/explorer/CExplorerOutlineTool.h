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
	//! Commits a user-originated expansion request to the owning model.
	//! Return false to veto the request. The callback deliberately carries only the
	//! requested value so it stays independent of HWND and layout/model types.
	using OutlineExpandedCallback = std::function<bool(bool expanded)>;

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
	//! Applies already-committed model state. This never calls m_callback.
	void SetOutlineExpanded(bool expanded);
	//! Sends a user request to the owner before changing the local/native state.
	//! A veto or callback failure leaves the prior state unchanged.
	[[nodiscard]] bool RequestOutlineExpanded(bool expanded) noexcept;
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

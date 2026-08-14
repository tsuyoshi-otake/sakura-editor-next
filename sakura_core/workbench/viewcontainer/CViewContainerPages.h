/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "theme/CThemeService.h"
#include "workbench/explorer/CExplorerTool.h"
#include "workbench/layout/WorkbenchIds.h"
#include "workbench/outline/COutlineWorkbenchTool.h"
#include "workbench/scm/CScmWorkbenchTool.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

class CDlgFuncList;

namespace workbench::viewcontainer {

namespace pageIds {
inline constexpr std::string_view Explorer = layout::ids::viewContainer::Explorer;
inline constexpr std::string_view SourceControl = layout::ids::viewContainer::SourceControl;
} // namespace pageIds

class CViewContainerPages final {
public:
	explicit CViewContainerPages(CDlgFuncList& dialog);
	~CViewContainerPages();
	CViewContainerPages(const CViewContainerPages&) = delete;
	CViewContainerPages& operator=(const CViewContainerPages&) = delete;

	[[nodiscard]] bool Create(HWND owner);
	void Close();
	[[nodiscard]] bool IsUsable() const noexcept { return m_created && !m_closed; }

	void Attach(std::string_view containerId, HWND host);
	[[nodiscard]] HWND AttachedHost(std::string_view containerId) const noexcept;
	void SetPageVisible(std::string_view containerId, bool visible);
	[[nodiscard]] bool Contains(std::string_view containerId) const noexcept;
	[[nodiscard]] std::vector<std::string> PageIds() const;
	void SetPalette(const theme::ThemePalette& palette);

	void SetOutlineExpanded(bool expanded) noexcept { m_outlineExpanded = expanded; }
	[[nodiscard]] bool IsOutlineExpanded() const noexcept { return m_outlineExpanded; }
	[[nodiscard]] explorer::CExplorerTool* Explorer() const noexcept { return m_explorer.get(); }
	[[nodiscard]] outline::COutlineWorkbenchTool* Outline() const noexcept { return m_outline.get(); }
	[[nodiscard]] scm::CScmWorkbenchTool* SourceControl() const noexcept { return m_scm.get(); }
	[[nodiscard]] bool IsWebviewOnly(std::string_view) const noexcept { return false; }
	[[nodiscard]] std::wstring PageTitle(std::string_view containerId) const;

private:
	struct Page {
		std::string id;
		std::wstring title;
		HWND attached = nullptr;
	};

	[[nodiscard]] Page* Find(std::string_view containerId) noexcept;
	[[nodiscard]] const Page* Find(std::string_view containerId) const noexcept;
	[[nodiscard]] HWND PageWindow(const Page& page) const noexcept;

	std::unique_ptr<explorer::CExplorerTool> m_explorer;
	std::unique_ptr<outline::COutlineWorkbenchTool> m_outline;
	std::unique_ptr<scm::CScmWorkbenchTool> m_scm;
	std::vector<Page> m_pages;
	HWND m_owner = nullptr;
	bool m_outlineExpanded = true;
	bool m_created = false;
	bool m_closed = false;
};

} // namespace workbench::viewcontainer

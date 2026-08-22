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
#include "workbench/search/CSearchWorkbenchTool.h"
#include "workbench/rendering/FrameSurfaceCommitState.h"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

class CDlgFuncList;

namespace workbench::viewcontainer {

namespace pageIds {
inline constexpr std::string_view Explorer = layout::ids::viewContainer::Explorer;
inline constexpr std::string_view SourceControl = layout::ids::viewContainer::SourceControl;
inline constexpr std::string_view Search = layout::ids::viewContainer::Search;
} // namespace pageIds

class CViewContainerPages final {
public:
	struct FrameSurfaceProjection final {
		rendering::FrameSurfaceAdapterSnapshot surface;
		std::uint32_t width = 1;
		std::uint32_t height = 1;
	};
	explicit CViewContainerPages(CDlgFuncList& dialog);
	~CViewContainerPages();
	CViewContainerPages(const CViewContainerPages&) = delete;
	CViewContainerPages& operator=(const CViewContainerPages&) = delete;

	[[nodiscard]] bool Create(HWND owner);
	void Close();
	[[nodiscard]] bool IsUsable() const noexcept { return m_created && !m_closed; }

	void Attach(std::string_view containerId, HWND host,
		std::string_view logicalHostId = "workbench.window.detached");
	[[nodiscard]] HWND AttachedHost(std::string_view containerId) const noexcept;
	void SetPageVisible(std::string_view containerId, bool visible);
	//! Records the completed native layout as the newest publishable frame.
	void NotifyPageLayout(std::string_view containerId);
	//! Commits adapter tickets only after the enclosing GDI frame reached its
	//! flush boundary, returning the surfaces that became publishable.
	[[nodiscard]] std::vector<FrameSurfaceProjection> CommitGdiFrame();
	[[nodiscard]] std::vector<FrameSurfaceProjection> FrameSurfaceProjections() const;
	[[nodiscard]] bool Contains(std::string_view containerId) const noexcept;
	[[nodiscard]] std::vector<std::string> PageIds() const;
	void SetPalette(const theme::ThemePalette& palette);
	//! Refreshes localized page chrome and child projections without reloading state.
	void RefreshStrings();

	void SetOutlineExpanded(bool expanded) noexcept { m_outlineExpanded = expanded; }
	[[nodiscard]] bool IsOutlineExpanded() const noexcept { return m_outlineExpanded; }
	[[nodiscard]] explorer::CExplorerTool* Explorer() const noexcept { return m_explorer.get(); }
	[[nodiscard]] outline::COutlineWorkbenchTool* Outline() const noexcept { return m_outline.get(); }
	[[nodiscard]] scm::CScmWorkbenchTool* SourceControl() const noexcept { return m_scm.get(); }
	[[nodiscard]] search::CSearchWorkbenchTool* Search() const noexcept { return m_search.get(); }
	[[nodiscard]] bool IsWebviewOnly(std::string_view) const noexcept { return false; }
	[[nodiscard]] std::wstring PageTitle(std::string_view containerId) const;

private:
	struct Page {
		std::string id;
		UINT titleResourceId = 0;
		HWND attached = nullptr;
		std::unique_ptr<rendering::FrameSurfaceCommitState> frameSurface;
	};

	[[nodiscard]] Page* Find(std::string_view containerId) noexcept;
	[[nodiscard]] const Page* Find(std::string_view containerId) const noexcept;
	[[nodiscard]] HWND PageWindow(const Page& page) const noexcept;
	void ApplySearchTexts();

	std::unique_ptr<explorer::CExplorerTool> m_explorer;
	std::unique_ptr<outline::COutlineWorkbenchTool> m_outline;
	std::unique_ptr<scm::CScmWorkbenchTool> m_scm;
	std::unique_ptr<search::CSearchWorkbenchTool> m_search;
	std::vector<Page> m_pages;
	HWND m_owner = nullptr;
	bool m_outlineExpanded = true;
	bool m_created = false;
	bool m_closed = false;
};

} // namespace workbench::viewcontainer

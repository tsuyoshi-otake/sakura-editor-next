/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "theme/CThemeService.h"
#include "workbench/explorer/CExplorerTool.h"
#include "workbench/extensions/CExtensionsWorkbenchTool.h"
#include "workbench/layout/WorkbenchIds.h"
#include "workbench/outline/COutlineWorkbenchTool.h"
#include "workbench/scm/CScmWorkbenchTool.h"
#include "workbench/search/CSearchWorkbenchTool.h"
#include "workbench/rendering/FrameSurfaceCommitState.h"
#include "workbench/viewcontainer/ViewContainerPagePool.h"

#include <cstdint>
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
inline constexpr std::string_view Extensions = layout::ids::viewContainer::Extensions;
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
	[[nodiscard]] HWND ParkingParent() const noexcept { return m_owner; }

	[[nodiscard]] ViewContainerPagePoolAttachResult Attach(std::string_view containerId,
		const ViewContainerPageHost& host) noexcept;
	[[nodiscard]] ViewContainerPagePoolDetachResult Detach(
		std::string_view containerId) noexcept;
	[[nodiscard]] HWND AttachedHost(std::string_view containerId) const noexcept;
	[[nodiscard]] bool IsMessageTarget(std::string_view containerId, HWND target) const noexcept;
	void LayoutPage(std::string_view containerId, const RECT& bounds,
		const RECT* excludedHostChrome = nullptr) noexcept;
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
	[[nodiscard]] explorer::CExplorerTool* Explorer() const noexcept;
	[[nodiscard]] outline::COutlineWorkbenchTool* Outline() const noexcept;
	[[nodiscard]] scm::CScmWorkbenchTool* SourceControl() const noexcept;
	[[nodiscard]] search::CSearchWorkbenchTool* Search() const noexcept;
	[[nodiscard]] extensions::CExtensionsWorkbenchTool* Extensions() const noexcept;
	[[nodiscard]] bool IsWebviewOnly(std::string_view) const noexcept { return false; }
	[[nodiscard]] std::wstring PageTitle(std::string_view containerId) const;

private:
	class PageAdapter;
	enum class PageKind : std::uint8_t {
		Explorer,
		SourceControl,
		Search,
		Extensions,
	};

	struct Page {
		std::string id;
		UINT titleResourceId = 0;
		PageKind kind{ PageKind::Explorer };
		PageAdapter* adapter = nullptr;
	};

	[[nodiscard]] Page* Find(std::string_view containerId) noexcept;
	[[nodiscard]] const Page* Find(std::string_view containerId) const noexcept;
	[[nodiscard]] HWND PageWindow(const Page& page) const noexcept;
	[[nodiscard]] std::unique_ptr<IViewContainerPage> CreatePage(PageKind kind) noexcept;
	void Bind(PageKind kind, PageAdapter* adapter) noexcept;
	void Unbind(PageKind kind, const PageAdapter* adapter) noexcept;
	void ApplySearchTexts();

	CDlgFuncList& m_dialog;
	ViewContainerPageRegistry m_registry;
	ViewContainerPagePool m_pool;
	std::vector<Page> m_pages;
	HWND m_owner = nullptr;
	bool m_outlineExpanded = true;
	bool m_created = false;
	bool m_closed = false;
};

} // namespace workbench::viewcontainer

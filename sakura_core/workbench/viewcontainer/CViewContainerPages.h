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
#include <span>
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

//! Required Win32 projection companion for contributed IViewContainerPage products.
//! The retained page/pool contract stays presentation-neutral. Native contribution
//! registration fails closed unless its product also implements this interface.
class IViewContainerPageProjection {
public:
	virtual ~IViewContainerPageProjection() = default;
	virtual void ActivateProjection() noexcept = 0;
	virtual void DeactivateProjection() noexcept = 0;
	[[nodiscard]] virtual bool PreTranslateProjection(MSG& message) noexcept = 0;
	//! Places the reparented native root in host coordinates, then lays out its
	//! children in root-local coordinates. Keeping both rectangles in one call
	//! prevents a physical host from applying only half of the geometry contract.
	virtual void LayoutProjection(const RECT& hostBounds, const RECT& contentBounds,
		unsigned int dpi) noexcept = 0;
	virtual void SetProjectionVisible(bool visible) noexcept = 0;
	//! Optional presentation updates for contributed native pages. Defaults keep
	//! existing projections source-compatible while allowing a contribution to
	//! follow the same theme and language lifecycle as built-in pages.
	virtual void SetProjectionPalette(const theme::ThemePalette&) noexcept {}
	virtual void RefreshProjectionStrings() noexcept {}
	//! Content invalidation is explicit so semantic workspace changes can update
	//! an active contributed page without polling or activating an inactive page.
	virtual void RefreshProjectionContent() noexcept {}
};

//! Independently testable production router for contributed native products.
//! It borrows the retained pool, validates every contribution before publication,
//! and owns the fail-closed shutdown when a factory omits the required companion.
class ContributedViewContainerPageHost final {
public:
	explicit ContributedViewContainerPageHost(ViewContainerPagePool& pool) noexcept;
	[[nodiscard]] bool Initialize(std::span<const std::string> containerIds) noexcept;
	void Close() noexcept;
	[[nodiscard]] bool IsUsable() const noexcept { return m_usable; }
	void Activate(std::string_view containerId) noexcept;
	void Deactivate(std::string_view containerId) noexcept;
	[[nodiscard]] bool PreTranslate(std::string_view containerId, MSG& message) noexcept;
	void Layout(std::string_view containerId, const RECT& hostBounds,
		const RECT& contentBounds, unsigned int dpi) noexcept;
	void SetVisible(std::string_view containerId, bool visible) noexcept;
	void SetPalette(std::string_view containerId,
		const theme::ThemePalette& palette) noexcept;
	void RefreshStrings(std::string_view containerId) noexcept;
	void RefreshContent(std::string_view containerId) noexcept;

private:
	[[nodiscard]] IViewContainerPageProjection* Projection(
		std::string_view containerId) noexcept;

	ViewContainerPagePool& m_pool;
	bool m_usable = false;
};

//! Host-facing retained-page capability used by all three physical Pane Composite
//! adapters. It is additive to the frozen page/pool contracts.
class IViewContainerPageHostService {
public:
	virtual ~IViewContainerPageHostService() = default;
	[[nodiscard]] virtual bool IsUsable() const noexcept = 0;
	[[nodiscard]] virtual ViewContainerPagePoolAttachResult Attach(std::string_view containerId,
		const ViewContainerPageHost& host) noexcept = 0;
	[[nodiscard]] virtual ViewContainerPagePoolDetachResult Detach(
		std::string_view containerId) noexcept = 0;
	[[nodiscard]] virtual HWND AttachedHost(std::string_view containerId) const noexcept = 0;
	[[nodiscard]] virtual bool SupportsLocation(std::string_view containerId,
		layout::EViewContainerLocation location) const noexcept = 0;
	[[nodiscard]] virtual std::vector<std::string> PageIds() const = 0;
	virtual void ActivatePage(std::string_view containerId) noexcept = 0;
	virtual void DeactivatePage(std::string_view containerId) noexcept = 0;
	[[nodiscard]] virtual bool PreTranslatePage(
		std::string_view containerId, MSG& message) noexcept = 0;
	virtual void LayoutPageProjection(std::string_view containerId,
		const RECT& hostBounds, const RECT& contentBounds, unsigned int dpi) noexcept = 0;
	virtual void SetPageVisible(std::string_view containerId, bool visible) = 0;
	virtual void NotifyPageLayout(std::string_view containerId) = 0;
};

class CViewContainerPages final : public IViewContainerPageHostService {
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

	//! Adds native companion page factories before Create. The complete pending
	//! batch is validated atomically and built-in IDs remain reserved.
	[[nodiscard]] ViewContainerPageRegistrationResult RegisterContributedPages(
		std::vector<ViewContainerPageDescriptor> descriptors) noexcept;
	[[nodiscard]] bool Create(HWND owner);
	void Close();
	[[nodiscard]] bool IsUsable() const noexcept override { return m_created && !m_closed; }
	[[nodiscard]] HWND ParkingParent() const noexcept { return m_owner; }

	[[nodiscard]] ViewContainerPagePoolAttachResult Attach(std::string_view containerId,
		const ViewContainerPageHost& host) noexcept override;
	[[nodiscard]] ViewContainerPagePoolDetachResult Detach(
		std::string_view containerId) noexcept override;
	[[nodiscard]] HWND AttachedHost(std::string_view containerId) const noexcept override;
	[[nodiscard]] bool IsMessageTarget(std::string_view containerId, HWND target) const noexcept;
	//! Routes host-independent page behavior through the retained page instance.
	//! Physical hosts keep their own chrome and only delegate page content here.
	void ActivatePage(std::string_view containerId) noexcept override;
	void DeactivatePage(std::string_view containerId) noexcept override;
	[[nodiscard]] bool PreTranslatePage(std::string_view containerId, MSG& message) noexcept override;
	void LayoutPageProjection(std::string_view containerId, const RECT& hostBounds,
		const RECT& contentBounds, unsigned int dpi) noexcept override;
	void LayoutPageContent(std::string_view containerId, const RECT& bounds,
		unsigned int dpi) noexcept;
	void LayoutPage(std::string_view containerId, const RECT& bounds,
		const RECT* excludedHostChrome = nullptr) noexcept;
	void SetPageVisible(std::string_view containerId, bool visible) override;
	//! Records the completed native layout as the newest publishable frame.
	void NotifyPageLayout(std::string_view containerId) override;
	//! Commits adapter tickets only after the enclosing GDI frame reached its
	//! flush boundary, returning the surfaces that became publishable.
	[[nodiscard]] std::vector<FrameSurfaceProjection> CommitGdiFrame();
	[[nodiscard]] std::vector<FrameSurfaceProjection> FrameSurfaceProjections() const;
	[[nodiscard]] bool Contains(std::string_view containerId) const noexcept;
	[[nodiscard]] bool SupportsLocation(std::string_view containerId,
		layout::EViewContainerLocation location) const noexcept override;
	[[nodiscard]] std::vector<std::string> PageIds() const override;
	void SetPalette(const theme::ThemePalette& palette);
	//! Refreshes localized page chrome and child projections without reloading state.
	void RefreshStrings();
	void RefreshPageContent(std::string_view containerId) noexcept;

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
	ContributedViewContainerPageHost m_contributedPages;
	std::vector<Page> m_pages;
	std::vector<ViewContainerPageDescriptor> m_pendingContributions;
	std::vector<std::string> m_contributedPageIds;
	std::vector<std::string> m_registeredPageIds;
	HWND m_owner = nullptr;
	bool m_outlineExpanded = true;
	bool m_created = false;
	bool m_closed = false;
};

} // namespace workbench::viewcontainer

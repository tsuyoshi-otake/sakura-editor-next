/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "theme/CThemeService.h"
#include "workbench/explorer/CExplorerTool.h"
#include "workbench/extension/CExtensionSidebarTool.h"
#include "workbench/outline/COutlineWorkbenchTool.h"
#include "workbench/scm/CScmWorkbenchTool.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

class CDlgFuncList;
class CExtensionPane;
class CExtensionViewRegistry;

namespace workbench::viewcontainer {

//! One built-in VS Code Activity Bar ViewContainer. This is never a physical Part.
enum class ViewContainerPage : std::uint8_t {
	//! `workbench.view.explorer`, including its nested Outline View.
	Explorer,
	//! `workbench.view.scm`.
	SourceControl,
	//! `workbench.view.extensions`.
	Extensions,
	Count,
};

inline constexpr std::size_t kViewContainerPageCount =
	static_cast<std::size_t>(ViewContainerPage::Count);

//! Bit for one page, so a host can declare the whole set it currently renders.
[[nodiscard]] constexpr std::uint32_t ViewContainerPageBit(ViewContainerPage page) noexcept
{
	return page == ViewContainerPage::Count
		? 0u : static_cast<std::uint32_t>(1u) << static_cast<std::uint32_t>(page);
}

/*!
	@brief The built-in ViewContainer page controls, owned independently of any host.

	VS Code lets an Activity Bar ViewContainer move between the Primary Side Bar and the
	Secondary Side Bar, and both bars can be visible at the same time. A page window must
	therefore outlive whichever host currently renders it, so this pool owns every page and
	only reparents one into the requesting host. The pool is not a layout authority: it
	never decides where a container belongs and never talks to the layout model.
*/
class CViewContainerPages final {
public:
	/*!
		@brief Creates the OpenVSX Marketplace control for the Extensions ViewContainer.

		VS Code's `workbench.view.extensions` *is* the Marketplace, so this pane is that
		container's own content. OpenVSX is profile-scoped, so a composition without a
		profile authority supplies no factory at all and the container reports the typed
		absence instead of guessing a profile. Returning null is that same absence, never
		a placeholder.
	*/
	using MarketplaceFactory = std::function<std::unique_ptr<CExtensionPane>(HWND owner)>;

	CViewContainerPages(CDlgFuncList& dialog, std::shared_ptr<CExtensionViewRegistry> extensionViews,
		MarketplaceFactory marketplaceFactory = {});
	~CViewContainerPages();
	CViewContainerPages(const CViewContainerPages&) = delete;
	CViewContainerPages& operator=(const CViewContainerPages&) = delete;

	//! Creates every page window under `owner`, detached and hidden.
	[[nodiscard]] bool Create(HWND owner);
	//! Destroys every page window. Call this before destroying any host that may hold one.
	void Close();
	[[nodiscard]] bool IsUsable() const noexcept { return m_created && !m_closed; }

	//! Moves one page under `host`, or back to the owner and hidden when `host` is null.
	//! The Explorer page carries its nested Outline View, exactly as in VS Code.
	void Attach(ViewContainerPage page, HWND host);
	[[nodiscard]] HWND AttachedHost(ViewContainerPage page) const noexcept;

	//! Shows or hides one page. Per-page visibility differs by control, so the knowledge
	//! stays here instead of being duplicated in every host.
	void SetPageVisible(ViewContainerPage page, bool visible);

	void SetPalette(const theme::ThemePalette& palette);

	//! Outline visibility is one model fact shared by every host, so it lives here rather
	//! than in a host that might not currently render the Explorer container.
	void SetOutlineExpanded(bool expanded) noexcept { m_outlineExpanded = expanded; }
	[[nodiscard]] bool IsOutlineExpanded() const noexcept { return m_outlineExpanded; }

	[[nodiscard]] explorer::CExplorerTool* Explorer() const noexcept { return m_explorer.get(); }
	[[nodiscard]] outline::COutlineWorkbenchTool* Outline() const noexcept { return m_outline.get(); }
	[[nodiscard]] scm::CScmWorkbenchTool* SourceControl() const noexcept { return m_scm.get(); }
	[[nodiscard]] extension::CExtensionSidebarTool* Extensions() const noexcept { return m_extensions.get(); }
	//! The OpenVSX Marketplace of the Extensions ViewContainer, or null when the
	//! composition supplied no profile-scoped factory.
	[[nodiscard]] CExtensionPane* Marketplace() const noexcept { return m_marketplace.get(); }
	//! True while an extension actually contributes a tree View. An empty contribution
	//! list reserves no space, exactly as VS Code renders no section for it.
	[[nodiscard]] bool HasContributedExtensionViews() const;

	//! Header caption of a page, matching the VS Code ViewContainer title.
	[[nodiscard]] static const wchar_t* PageTitle(ViewContainerPage page) noexcept;

private:
	[[nodiscard]] HWND PageWindow(ViewContainerPage page) const noexcept;

	std::unique_ptr<explorer::CExplorerTool> m_explorer;
	std::unique_ptr<outline::COutlineWorkbenchTool> m_outline;
	std::unique_ptr<scm::CScmWorkbenchTool> m_scm;
	std::unique_ptr<extension::CExtensionSidebarTool> m_extensions;
	//! Retained so the Extensions page can ask whether any contributed View exists; the
	//! sidebar tool owns the same registry.
	std::shared_ptr<CExtensionViewRegistry> m_extensionViews;
	MarketplaceFactory m_marketplaceFactory;
	//! Destroyed by `Close()`, which the destructor defined in the .cpp always runs, so an
	//! incomplete `CExtensionPane` is safe here.
	std::unique_ptr<CExtensionPane> m_marketplace;
	std::array<HWND, kViewContainerPageCount> m_attached{};
	HWND m_owner = nullptr;
	bool m_outlineExpanded = true;
	bool m_created = false;
	bool m_closed = false;
};

} // namespace workbench::viewcontainer

/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "theme/CThemeService.h"
#include "workbench/explorer/CExplorerTool.h"
#include "workbench/extension/CExtensionSidebarTool.h"
#include "workbench/layout/WorkbenchIds.h"
#include "workbench/outline/COutlineWorkbenchTool.h"
#include "workbench/scm/CScmWorkbenchTool.h"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

class CDlgFuncList;
class CExtensionPane;
class CExtensionViewRegistry;

namespace workbench::viewcontainer {

/*!
	@brief The built-in ViewContainers this pool renders.

	A page is addressed by container ID rather than by an enumerator, so an extension-contributed
	container is the same kind of thing as a built-in one, exactly as in VS Code. The IDs are the
	layout registry's, not a private copy: identity has one owner.
*/
namespace pageIds {
inline constexpr std::string_view Explorer = layout::ids::viewContainer::Explorer;
inline constexpr std::string_view SourceControl = layout::ids::viewContainer::SourceControl;
inline constexpr std::string_view Extensions = layout::ids::viewContainer::Extensions;
} // namespace pageIds

//! One ViewContainer an extension contributed to the Activity Bar.
struct ContributedViewContainer {
	std::string id;
	std::wstring title;
	/*!
		True when every View this container declares is `"type": "webview"`
		(`EExtensionViewKind::Webview` in `CExtensionContributionRegistry.h`). Neither this
		struct nor its `id`/`title` fields can answer that on their own: a manifest-declared
		View's kind lives in `CExtensionContributionRegistry`, and which container a View
		belongs to lives in `workbench::layout::WorkbenchContributionRegistry` — two registries
		this pool does not hold. The composition root cross-references both and hands down the
		one bit this pool actually needs. Defaulted to false so a caller that does not yet
		compute it keeps today's behavior (a contributed tree page, possibly empty).
	*/
	bool webviewOnly = false;
	[[nodiscard]] bool operator==(const ContributedViewContainer&) const = default;
};

/*!
	@brief The callbacks every contributed-view tree needs.

	A container that appears while the host is already running must be wired exactly like the
	ones that existed at startup. The pool therefore keeps the bundle and applies it to each
	tree it creates, instead of making the composition notice every new container.
*/
struct ExtensionViewCallbacks {
	extension::CExtensionSidebarTool::RequestChildrenCallback requestChildren;
	extension::CExtensionSidebarTool::SelectionChangedCallback selectionChanged;
	extension::CExtensionSidebarTool::CheckboxChangedCallback checkboxChanged;
	extension::CExtensionSidebarTool::CommandCallback command;
	extension::CExtensionSidebarTool::VisibilityChangedCallback visibilityChanged;
};

/*!
	@brief The ViewContainer page controls, owned independently of any host.

	VS Code lets an Activity Bar ViewContainer move between the Primary Side Bar and the
	Secondary Side Bar, and both bars can be visible at the same time. A page window must
	therefore outlive whichever host currently renders it, so this pool owns every page and
	only reparents one into the requesting host. The pool is not a layout authority: it never
	decides where a container belongs and never talks to the layout model. Which containers
	exist is likewise decided elsewhere and handed to `SyncContributedContainers`.
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

	//! Creates every built-in page window under `owner`, detached and hidden.
	[[nodiscard]] bool Create(HWND owner);
	//! Destroys every page window. Call this before destroying any host that may hold one.
	void Close();
	[[nodiscard]] bool IsUsable() const noexcept { return m_created && !m_closed; }

	/*!
		@brief Makes the pool render exactly `containers` in addition to the built-in pages.

		Creates a page for each newly contributed container and destroys the pages of the ones
		that vanished, preserving the pages that stayed so an unrelated extension update never
		resets a user's tree. Returns true when the set actually changed, so a caller can skip
		re-projecting the layout when an extension re-registers the same containers.
	*/
	bool SyncContributedContainers(std::vector<ContributedViewContainer> containers);

	//! Applies to every contributed tree, including ones created later.
	void SetExtensionViewCallbacks(ExtensionViewCallbacks callbacks);
	//! Rebuilds every contributed tree from the registry. Safe to call from an RPC worker.
	void RefreshExtensionViews();

	//! Moves one page under `host`, or back to the owner and hidden when `host` is null.
	//! The Explorer page carries its nested Outline View, exactly as in VS Code.
	void Attach(std::string_view containerId, HWND host);
	[[nodiscard]] HWND AttachedHost(std::string_view containerId) const noexcept;

	//! Shows or hides one page. Per-page visibility differs by control, so the knowledge
	//! stays here instead of being duplicated in every host.
	void SetPageVisible(std::string_view containerId, bool visible);

	//! True when this pool owns a page for `containerId`. A host must not claim a container
	//! that has no page, because nothing would ever be rendered in it.
	[[nodiscard]] bool Contains(std::string_view containerId) const noexcept;
	//! Every page this pool currently owns, built-ins first. Copies the IDs because a caller
	//! typically hides all pages, and hiding may not invalidate the pool's own storage.
	[[nodiscard]] std::vector<std::string> PageIds() const;

	void SetPalette(const theme::ThemePalette& palette);

	//! Outline visibility is one model fact shared by every host, so it lives here rather
	//! than in a host that might not currently render the Explorer container.
	void SetOutlineExpanded(bool expanded) noexcept { m_outlineExpanded = expanded; }
	[[nodiscard]] bool IsOutlineExpanded() const noexcept { return m_outlineExpanded; }

	[[nodiscard]] explorer::CExplorerTool* Explorer() const noexcept { return m_explorer.get(); }
	[[nodiscard]] outline::COutlineWorkbenchTool* Outline() const noexcept { return m_outline.get(); }
	[[nodiscard]] scm::CScmWorkbenchTool* SourceControl() const noexcept { return m_scm.get(); }
	[[nodiscard]] extension::CExtensionSidebarTool* Extensions() const noexcept { return m_extensions.get(); }
	//! The tree of one contributed container, or null when this pool renders no such page.
	[[nodiscard]] extension::CExtensionSidebarTool* ContributedViews(std::string_view containerId) const noexcept;
	//! The OpenVSX Marketplace of the Extensions ViewContainer, or null when the
	//! composition supplied no profile-scoped factory.
	[[nodiscard]] CExtensionPane* Marketplace() const noexcept { return m_marketplace.get(); }
	//! True while a view no contributed container claimed exists, so the Extensions page has
	//! a tree section to render. An empty contribution list reserves no space, exactly as
	//! VS Code renders no section for it.
	[[nodiscard]] bool HasContributedExtensionViews() const;

	//! True when `containerId` names a contributed page whose declared View(s) are all
	//! `"type": "webview"`, so this pool has no tree content for it and never will. False for
	//! a built-in page, an unknown container, and a container this pool has not been told is
	//! webview-only (see `ContributedViewContainer::webviewOnly`).
	[[nodiscard]] bool IsWebviewOnly(std::string_view containerId) const noexcept;

	//! Header caption of a page, matching the VS Code ViewContainer title. Empty for a
	//! container this pool does not render.
	[[nodiscard]] std::wstring PageTitle(std::string_view containerId) const;

private:
	//! One rendered ViewContainer. A built-in page leaves `views` null: its content is the
	//! dedicated control below, not a generic contributed tree.
	struct Page {
		std::string id;
		std::wstring title;
		std::unique_ptr<extension::CExtensionSidebarTool> views;
		HWND attached = nullptr;
		//! Mirrors `ContributedViewContainer::webviewOnly` for a contributed page. Always
		//! false for a built-in page.
		bool webviewOnly = false;
	};

	[[nodiscard]] Page* Find(std::string_view containerId) noexcept;
	[[nodiscard]] const Page* Find(std::string_view containerId) const noexcept;
	[[nodiscard]] HWND PageWindow(const Page& page) const noexcept;
	//! True when a contributed page claims `containerId`, so the Extensions page must not
	//! also render its views.
	[[nodiscard]] bool IsClaimedByContributedPage(std::wstring_view containerId) const;
	[[nodiscard]] std::unique_ptr<extension::CExtensionSidebarTool> CreateContributedViews(
		std::string_view containerId) const;
	void ApplyCallbacks(extension::CExtensionSidebarTool& tool) const;

	std::unique_ptr<explorer::CExplorerTool> m_explorer;
	std::unique_ptr<outline::COutlineWorkbenchTool> m_outline;
	std::unique_ptr<scm::CScmWorkbenchTool> m_scm;
	std::unique_ptr<extension::CExtensionSidebarTool> m_extensions;
	//! Retained so a page can ask which contributed Views exist; every sidebar tool the pool
	//! creates shares this same registry.
	std::shared_ptr<CExtensionViewRegistry> m_extensionViews;
	MarketplaceFactory m_marketplaceFactory;
	//! Destroyed by `Close()`, which the destructor defined in the .cpp always runs, so an
	//! incomplete `CExtensionPane` is safe here.
	std::unique_ptr<CExtensionPane> m_marketplace;
	ExtensionViewCallbacks m_callbacks;
	//! Built-in pages first, contributed pages after, both in Activity Bar order. Linear
	//! lookup is deliberate: the count is the number of Activity Bar icons, so a map would
	//! cost more in allocation and indirection than it saves in comparisons.
	std::vector<Page> m_pages;
	HWND m_owner = nullptr;
	bool m_outlineExpanded = true;
	bool m_created = false;
	bool m_closed = false;
};

} // namespace workbench::viewcontainer

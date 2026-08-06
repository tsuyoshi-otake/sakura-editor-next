/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "workbench/viewcontainer/CViewContainerPages.h"

#include "extension/CExtensionPane.h"
#include "extension/CExtensionViewRegistry.h"
#include "util/string_ex.h"

#include <algorithm>
#include <utility>

namespace workbench::viewcontainer {
namespace {

//! Number of built-in pages, which always occupy the front of the page list.
constexpr std::size_t kBuiltinPageCount = 3;

} // namespace

CViewContainerPages::CViewContainerPages(CDlgFuncList& dialog,
	std::shared_ptr<CExtensionViewRegistry> extensionViews,
	MarketplaceFactory marketplaceFactory)
	: m_explorer(std::make_unique<explorer::CExplorerTool>())
	, m_outline(std::make_unique<outline::COutlineWorkbenchTool>(dialog))
	, m_scm(std::make_unique<scm::CScmWorkbenchTool>())
	, m_extensions(std::make_unique<extension::CExtensionSidebarTool>(extensionViews,
		  // The Extensions container is the fallback bucket: it renders every contributed View
		  // that no dedicated container claimed, so a manifest that declares no container of
		  // its own still gets its tree somewhere visible.
		  [this](const SExtensionViewDescriptor& view) { return !IsClaimedByContributedPage(view.containerId); }))
	, m_extensionViews(std::move(extensionViews))
	, m_marketplaceFactory(std::move(marketplaceFactory))
{
	m_pages.reserve(kBuiltinPageCount);
	m_pages.push_back({ std::string(pageIds::Explorer), L"EXPLORER" });
	m_pages.push_back({ std::string(pageIds::SourceControl), L"SOURCE CONTROL" });
	m_pages.push_back({ std::string(pageIds::Extensions), L"EXTENSIONS" });
}

CViewContainerPages::~CViewContainerPages()
{
	Close();
}

bool CViewContainerPages::Create(HWND owner)
{
	if (m_closed || m_created || owner == nullptr) return false;
	m_owner = owner;
	if (!m_explorer->Create(owner) || !m_outline->Create(owner) || !m_scm->Create(owner)
		|| !m_extensions->Create(owner)) {
		Close();
		return false;
	}
	// The Marketplace is the Extensions ViewContainer's own content, exactly as VS Code's
	// `workbench.view.extensions` is the Marketplace. A composition with no profile
	// authority supplies no factory, and a factory that fails returns null: either way the
	// container renders whatever it really has instead of a placeholder.
	if (m_marketplaceFactory) {
		auto marketplace = m_marketplaceFactory(owner);
		if (marketplace && marketplace->GetHwnd() != nullptr) m_marketplace = std::move(marketplace);
	}

	// Every page starts detached and hidden. A host makes exactly the container it
	// currently renders visible, so no page may paint over the frame before then.
	m_outline->SetVisible(false);
	m_scm->SetVisible(false);
	if (const HWND explorerWindow = m_explorer->GetHwnd()) ::ShowWindow(explorerWindow, SW_HIDE);
	if (const HWND extensionsWindow = m_extensions->GetHwnd()) ::ShowWindow(extensionsWindow, SW_HIDE);
	if (m_marketplace) {
		if (const HWND window = m_marketplace->GetHwnd()) ::ShowWindow(window, SW_HIDE);
	}
	for (auto& page : m_pages) page.attached = nullptr;
	m_created = true;
	return true;
}

void CViewContainerPages::Close()
{
	if (m_closed) return;
	m_closed = true;
	// Contributed pages die first: their trees hold callbacks into the composition, which is
	// tearing down, and nothing below depends on them.
	for (auto& page : m_pages) {
		if (page.views) page.views->Close();
	}
	m_pages.erase(m_pages.begin() + std::min<std::size_t>(kBuiltinPageCount, m_pages.size()), m_pages.end());
	if (m_outline) m_outline->Close();
	if (m_scm) m_scm->Close();
	if (m_extensions) m_extensions->Close();
	// `CWnd` destroys its window, and the pane cancels any in-flight OpenVSX job first.
	m_marketplace.reset();
	if (m_explorer) m_explorer->Close();
	for (auto& page : m_pages) page.attached = nullptr;
	m_callbacks = {};
	m_owner = nullptr;
}

CViewContainerPages::Page* CViewContainerPages::Find(const std::string_view containerId) noexcept
{
	const auto found = std::find_if(m_pages.begin(), m_pages.end(),
		[containerId](const Page& page) { return page.id == containerId; });
	return found == m_pages.end() ? nullptr : &*found;
}

const CViewContainerPages::Page* CViewContainerPages::Find(const std::string_view containerId) const noexcept
{
	const auto found = std::find_if(m_pages.begin(), m_pages.end(),
		[containerId](const Page& page) { return page.id == containerId; });
	return found == m_pages.end() ? nullptr : &*found;
}

bool CViewContainerPages::Contains(const std::string_view containerId) const noexcept
{
	return Find(containerId) != nullptr;
}

std::vector<std::string> CViewContainerPages::PageIds() const
{
	std::vector<std::string> ids;
	ids.reserve(m_pages.size());
	for (const auto& page : m_pages) ids.push_back(page.id);
	return ids;
}

bool CViewContainerPages::IsClaimedByContributedPage(const std::wstring_view containerId) const
{
	if (containerId.empty() || m_pages.size() <= kBuiltinPageCount) return false;
	const auto narrow = wcstou8s(containerId);
	return std::any_of(m_pages.begin() + kBuiltinPageCount, m_pages.end(),
		[&narrow](const Page& page) { return page.id == narrow; });
}

std::unique_ptr<extension::CExtensionSidebarTool> CViewContainerPages::CreateContributedViews(
	const std::string_view containerId) const
{
	const auto wide = u8stowcs(containerId);
	return std::make_unique<extension::CExtensionSidebarTool>(m_extensionViews,
		[wide](const SExtensionViewDescriptor& view) { return view.containerId == wide; });
}

void CViewContainerPages::ApplyCallbacks(extension::CExtensionSidebarTool& tool) const
{
	tool.SetRequestChildrenCallback(m_callbacks.requestChildren);
	tool.SetSelectionChangedCallback(m_callbacks.selectionChanged);
	tool.SetCheckboxChangedCallback(m_callbacks.checkboxChanged);
	tool.SetCommandCallback(m_callbacks.command);
	tool.SetVisibilityChangedCallback(m_callbacks.visibilityChanged);
}

bool CViewContainerPages::SyncContributedContainers(std::vector<ContributedViewContainer> containers)
{
	if (!IsUsable()) return false;

	// A container an extension declares twice, or one colliding with a built-in page, would
	// give two pages the same identity. The first declaration wins, matching the layout
	// registry's own duplicate rejection.
	std::vector<ContributedViewContainer> wanted;
	wanted.reserve(containers.size());
	for (auto& container : containers) {
		if (container.id.empty()) continue;
		if (container.id == pageIds::Explorer || container.id == pageIds::SourceControl
			|| container.id == pageIds::Extensions) {
			continue;
		}
		if (std::any_of(wanted.begin(), wanted.end(),
				[&container](const ContributedViewContainer& kept) { return kept.id == container.id; })) {
			continue;
		}
		if (container.title.empty()) container.title = u8stowcs(container.id);
		wanted.push_back(std::move(container));
	}

	bool changed = false;
	// Drop the pages whose container vanished. Erasing destroys the tree control, so the
	// window goes away with the container instead of lingering under the host.
	for (std::size_t index = m_pages.size(); index > kBuiltinPageCount; --index) {
		auto& page = m_pages[index - 1];
		if (std::any_of(wanted.begin(), wanted.end(),
				[&page](const ContributedViewContainer& keep) { return keep.id == page.id; })) {
			continue;
		}
		if (page.views) page.views->Close();
		m_pages.erase(m_pages.begin() + static_cast<std::ptrdiff_t>(index - 1));
		changed = true;
	}

	// Add the new ones, and keep the surviving ones as they are: an unrelated extension
	// update must not reset a user's expanded tree.
	for (const auto& container : wanted) {
		if (Page* existing = Find(container.id)) {
			if (existing->title != container.title) {
				existing->title = container.title;
				changed = true;
			}
			if (existing->webviewOnly != container.webviewOnly) {
				existing->webviewOnly = container.webviewOnly;
				changed = true;
			}
			continue;
		}
		auto views = CreateContributedViews(container.id);
		if (!views || !views->Create(m_owner)) continue;
		ApplyCallbacks(*views);
		if (const HWND window = views->GetHwnd(); window != nullptr) ::ShowWindow(window, SW_HIDE);
		m_pages.push_back({ container.id, container.title, std::move(views), nullptr, container.webviewOnly });
		changed = true;
	}

	// The Extensions page is the fallback bucket, so the set of contributed containers changes
	// which views it owns. Rebuild it rather than waiting for the next registry change.
	if (changed && m_extensions) m_extensions->Refresh();
	return changed;
}

void CViewContainerPages::SetExtensionViewCallbacks(ExtensionViewCallbacks callbacks)
{
	m_callbacks = std::move(callbacks);
	if (m_extensions) ApplyCallbacks(*m_extensions);
	for (auto& page : m_pages) {
		if (page.views) ApplyCallbacks(*page.views);
	}
}

void CViewContainerPages::RefreshExtensionViews()
{
	if (m_extensions) m_extensions->Refresh();
	for (auto& page : m_pages) {
		if (page.views) page.views->Refresh();
	}
}

HWND CViewContainerPages::PageWindow(const Page& page) const noexcept
{
	if (page.views) return page.views->GetHwnd();
	if (page.id == pageIds::Explorer) return m_explorer ? m_explorer->GetHwnd() : nullptr;
	if (page.id == pageIds::SourceControl) return m_scm ? m_scm->GetHwnd() : nullptr;
	if (page.id == pageIds::Extensions) return m_extensions ? m_extensions->GetHwnd() : nullptr;
	return nullptr;
}

void CViewContainerPages::Attach(const std::string_view containerId, HWND host)
{
	if (!IsUsable()) return;
	Page* page = Find(containerId);
	if (page == nullptr) return;
	const HWND target = host != nullptr ? host : m_owner;
	if (target == nullptr) return;
	if (page->attached == host) return;

	SetPageVisible(containerId, false);
	const HWND window = PageWindow(*page);
	if (window != nullptr && ::IsWindow(window) && ::GetParent(window) != target) {
		if (::SetParent(window, target) == nullptr) return;
	}
	if (page->id == pageIds::Explorer && m_outline) {
		// Outline is a View nested inside the Explorer ViewContainer, so it always
		// follows that container to its new physical Part.
		(void)m_outline->Reparent(target);
	}
	if (page->id == pageIds::Extensions && m_marketplace) {
		// The Marketplace is part of the Extensions ViewContainer, so it moves with it
		// just like Outline moves with Explorer.
		if (const HWND marketplaceWindow = m_marketplace->GetHwnd();
			marketplaceWindow != nullptr && ::IsWindow(marketplaceWindow)
			&& ::GetParent(marketplaceWindow) != target) {
			(void)::SetParent(marketplaceWindow, target);
		}
	}
	page->attached = host;
}

void CViewContainerPages::SetPageVisible(const std::string_view containerId, const bool visible)
{
	if (!IsUsable()) return;
	const Page* page = Find(containerId);
	if (page == nullptr) return;

	if (page->views) {
		// A contributed container is nothing but its views, so the whole page shows or hides
		// with the container and the extension observes exactly that.
		if (const HWND window = page->views->GetHwnd(); window != nullptr && ::IsWindow(window)) {
			::ShowWindow(window, visible ? SW_SHOW : SW_HIDE);
		}
		page->views->SetSidebarVisible(visible);
		return;
	}
	if (page->id == pageIds::Explorer) {
		if (const HWND window = PageWindow(*page); window != nullptr && ::IsWindow(window)) {
			::ShowWindow(window, visible ? SW_SHOW : SW_HIDE);
		}
		// The nested Outline View is only visible when its container is, and only while
		// the user keeps that section expanded.
		if (m_outline) m_outline->SetVisible(visible && m_outlineExpanded);
		return;
	}
	if (page->id == pageIds::SourceControl) {
		if (m_scm) m_scm->SetVisible(visible);
		return;
	}
	if (page->id == pageIds::Extensions) {
		// The Marketplace is this container's own content and is shown whenever the
		// container is.
		if (m_marketplace) {
			if (const HWND window = m_marketplace->GetHwnd(); window != nullptr && ::IsWindow(window)) {
				::ShowWindow(window, visible ? SW_SHOW : SW_HIDE);
			}
		}
		// A contributed tree View is an additional section, so it appears only while an
		// extension really contributes one to this container.
		if (const HWND window = PageWindow(*page); window != nullptr && ::IsWindow(window)) {
			::ShowWindow(window, visible && HasContributedExtensionViews() ? SW_SHOW : SW_HIDE);
		}
		// Extensions still observe the ViewContainer's own visibility, not the visibility
		// of the section that happens to render their views.
		if (m_extensions) m_extensions->SetSidebarVisible(visible);
	}
}

bool CViewContainerPages::HasContributedExtensionViews() const
{
	if (m_extensionViews == nullptr) return false;
	const auto views = m_extensionViews->Views();
	return std::any_of(views.begin(), views.end(),
		[this](const SExtensionViewDescriptor& view) { return !IsClaimedByContributedPage(view.containerId); });
}

bool CViewContainerPages::IsWebviewOnly(const std::string_view containerId) const noexcept
{
	const Page* page = Find(containerId);
	return page != nullptr && page->webviewOnly;
}

HWND CViewContainerPages::AttachedHost(const std::string_view containerId) const noexcept
{
	const Page* page = Find(containerId);
	return page == nullptr ? nullptr : page->attached;
}

extension::CExtensionSidebarTool* CViewContainerPages::ContributedViews(
	const std::string_view containerId) const noexcept
{
	const Page* page = Find(containerId);
	return page == nullptr ? nullptr : page->views.get();
}

void CViewContainerPages::SetPalette(const theme::ThemePalette& palette)
{
	if (m_explorer) {
		// Explorer is a Side Bar ViewContainer; its injected background also backs
		// the TreeView's generated font-icon tiles.
		m_explorer->SetPalette({ palette.sideBar.ToColorRef(), palette.primaryText.ToColorRef(),
			palette.border.ToColorRef(), palette.accent.ToColorRef(), palette.border.ToColorRef(),
			palette.secondaryText.ToColorRef(), palette.raised.ToColorRef() });
	}
	if (m_outline) m_outline->SetPalette(palette);
	if (m_scm) m_scm->SetPalette(palette);
	if (m_extensions) m_extensions->SetPalette(palette);
	for (auto& page : m_pages) {
		if (page.views) page.views->SetPalette(palette);
	}
}

std::wstring CViewContainerPages::PageTitle(const std::string_view containerId) const
{
	const Page* page = Find(containerId);
	return page == nullptr ? std::wstring{} : page->title;
}

} // namespace workbench::viewcontainer

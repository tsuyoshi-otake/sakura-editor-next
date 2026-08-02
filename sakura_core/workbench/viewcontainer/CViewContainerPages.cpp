/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "workbench/viewcontainer/CViewContainerPages.h"

#include "extension/CExtensionPane.h"
#include "extension/CExtensionViewRegistry.h"

namespace workbench::viewcontainer {

CViewContainerPages::CViewContainerPages(CDlgFuncList& dialog,
	std::shared_ptr<CExtensionViewRegistry> extensionViews,
	MarketplaceFactory marketplaceFactory)
	: m_explorer(std::make_unique<explorer::CExplorerTool>())
	, m_outline(std::make_unique<outline::COutlineWorkbenchTool>(dialog))
	, m_scm(std::make_unique<scm::CScmWorkbenchTool>())
	, m_extensions(std::make_unique<extension::CExtensionSidebarTool>(extensionViews))
	, m_extensionViews(std::move(extensionViews))
	, m_marketplaceFactory(std::move(marketplaceFactory))
{
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
	m_attached.fill(nullptr);
	m_created = true;
	return true;
}

void CViewContainerPages::Close()
{
	if (m_closed) return;
	m_closed = true;
	if (m_outline) m_outline->Close();
	if (m_scm) m_scm->Close();
	if (m_extensions) m_extensions->Close();
	// `CWnd` destroys its window, and the pane cancels any in-flight OpenVSX job first.
	m_marketplace.reset();
	if (m_explorer) m_explorer->Close();
	m_attached.fill(nullptr);
	m_owner = nullptr;
}

HWND CViewContainerPages::PageWindow(ViewContainerPage page) const noexcept
{
	switch (page) {
	case ViewContainerPage::Explorer: return m_explorer ? m_explorer->GetHwnd() : nullptr;
	case ViewContainerPage::SourceControl: return m_scm ? m_scm->GetHwnd() : nullptr;
	case ViewContainerPage::Extensions: return m_extensions ? m_extensions->GetHwnd() : nullptr;
	case ViewContainerPage::Count: break;
	}
	return nullptr;
}

void CViewContainerPages::Attach(ViewContainerPage page, HWND host)
{
	if (!IsUsable() || page == ViewContainerPage::Count) return;
	const auto index = static_cast<std::size_t>(page);
	const HWND target = host != nullptr ? host : m_owner;
	if (target == nullptr) return;
	if (m_attached[index] == host) return;

	SetPageVisible(page, false);
	const HWND window = PageWindow(page);
	if (window != nullptr && ::IsWindow(window) && ::GetParent(window) != target) {
		if (::SetParent(window, target) == nullptr) return;
	}
	if (page == ViewContainerPage::Explorer && m_outline) {
		// Outline is a View nested inside the Explorer ViewContainer, so it always
		// follows that container to its new physical Part.
		(void)m_outline->Reparent(target);
	}
	if (page == ViewContainerPage::Extensions && m_marketplace) {
		// The Marketplace is part of the Extensions ViewContainer, so it moves with it
		// just like Outline moves with Explorer.
		if (const HWND marketplaceWindow = m_marketplace->GetHwnd();
			marketplaceWindow != nullptr && ::IsWindow(marketplaceWindow)
			&& ::GetParent(marketplaceWindow) != target) {
			(void)::SetParent(marketplaceWindow, target);
		}
	}
	m_attached[index] = host;
}

void CViewContainerPages::SetPageVisible(ViewContainerPage page, bool visible)
{
	if (!IsUsable()) return;
	switch (page) {
	case ViewContainerPage::Explorer:
		if (const HWND window = PageWindow(page); window != nullptr && ::IsWindow(window)) {
			::ShowWindow(window, visible ? SW_SHOW : SW_HIDE);
		}
		// The nested Outline View is only visible when its container is, and only while
		// the user keeps that section expanded.
		if (m_outline) m_outline->SetVisible(visible && m_outlineExpanded);
		return;
	case ViewContainerPage::SourceControl:
		if (m_scm) m_scm->SetVisible(visible);
		return;
	case ViewContainerPage::Extensions:
		// The Marketplace is this container's own content and is shown whenever the
		// container is.
		if (m_marketplace) {
			if (const HWND window = m_marketplace->GetHwnd(); window != nullptr && ::IsWindow(window)) {
				::ShowWindow(window, visible ? SW_SHOW : SW_HIDE);
			}
		}
		// A contributed tree View is an additional section, so it appears only while an
		// extension really contributes one.
		if (const HWND window = PageWindow(page); window != nullptr && ::IsWindow(window)) {
			::ShowWindow(window, visible && HasContributedExtensionViews() ? SW_SHOW : SW_HIDE);
		}
		// Extensions still observe the ViewContainer's own visibility, not the visibility
		// of the section that happens to render their views.
		if (m_extensions) m_extensions->SetSidebarVisible(visible);
		return;
	case ViewContainerPage::Count:
		return;
	}
}

bool CViewContainerPages::HasContributedExtensionViews() const
{
	return m_extensionViews != nullptr && !m_extensionViews->Views().empty();
}

HWND CViewContainerPages::AttachedHost(ViewContainerPage page) const noexcept
{
	if (page == ViewContainerPage::Count) return nullptr;
	return m_attached[static_cast<std::size_t>(page)];
}

void CViewContainerPages::SetPalette(const theme::ThemePalette& palette)
{
	if (m_explorer) {
		m_explorer->SetPalette({ palette.sideBar.ToColorRef(), palette.primaryText.ToColorRef(),
			palette.border.ToColorRef(), palette.accent.ToColorRef() });
	}
	if (m_outline) m_outline->SetPalette(palette);
	if (m_scm) m_scm->SetPalette(palette);
	if (m_extensions) m_extensions->SetPalette(palette);
}

const wchar_t* CViewContainerPages::PageTitle(ViewContainerPage page) noexcept
{
	switch (page) {
	case ViewContainerPage::SourceControl: return L"SOURCE CONTROL";
	case ViewContainerPage::Extensions: return L"EXTENSIONS";
	case ViewContainerPage::Explorer: return L"EXPLORER";
	case ViewContainerPage::Count: break;
	}
	return L"";
}

} // namespace workbench::viewcontainer

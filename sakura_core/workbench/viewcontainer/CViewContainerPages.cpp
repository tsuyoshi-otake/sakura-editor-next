/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "workbench/viewcontainer/CViewContainerPages.h"
#include "CSelectLang.h"

#include <algorithm>

namespace workbench::viewcontainer {

namespace {

void ShowPageWindow(HWND window, bool visible)
{
	if (window == nullptr || !::IsWindow(window)) return;
	if (::IsWindowVisible(window) == (visible != FALSE)) return;
	::ShowWindow(window, visible ? SW_SHOW : SW_HIDE);
	if (!visible) return;
	// A page can refresh while another ViewContainer is active.  Showing that
	// already-updated child must paint its whole subtree before the workbench
	// presents it, otherwise stale rows or a stale scrollbar can survive until a
	// later unrelated paint message.
	::RedrawWindow(window, nullptr, nullptr,
		RDW_INVALIDATE | RDW_FRAME | RDW_ALLCHILDREN | RDW_NOERASE);
}

void RedrawVisiblePage(HWND window)
{
	if (window == nullptr || !::IsWindow(window) || !::IsWindowVisible(window)) return;
	::RedrawWindow(window, nullptr, nullptr,
		RDW_INVALIDATE | RDW_FRAME | RDW_ALLCHILDREN | RDW_NOERASE);
}

} // namespace

CViewContainerPages::CViewContainerPages(CDlgFuncList& dialog)
	: m_explorer(std::make_unique<explorer::CExplorerTool>())
	, m_outline(std::make_unique<outline::COutlineWorkbenchTool>(dialog))
	, m_scm(std::make_unique<scm::CScmWorkbenchTool>())
	, m_search(std::make_unique<search::CSearchWorkbenchTool>())
{
	m_pages.push_back({ std::string(pageIds::Explorer), STR_WORKBENCH_EXPLORER_TITLE, nullptr,
		std::make_unique<rendering::FrameSurfaceCommitState>(1) });
	m_pages.push_back({ std::string(pageIds::SourceControl), STR_WORKBENCH_SOURCE_CONTROL_TITLE, nullptr,
		std::make_unique<rendering::FrameSurfaceCommitState>(2) });
	m_pages.push_back({ std::string(pageIds::Search), STR_WORKBENCH_SEARCH_TITLE, nullptr,
		std::make_unique<rendering::FrameSurfaceCommitState>(3) });
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
		|| !m_search->Create(owner)) {
		Close();
		return false;
	}
	m_outline->SetVisible(false);
	m_scm->SetVisible(false);
	m_search->SetVisible(false);
	ApplySearchTexts();
	if (const HWND window = m_explorer->GetHwnd()) ::ShowWindow(window, SW_HIDE);
	for (auto& page : m_pages) {
		if (page.frameSurface) {
			(void)page.frameSurface->Open("workbench.window.detached", false);
		}
	}
	m_created = true;
	return true;
}

void CViewContainerPages::Close()
{
	if (m_closed) return;
	m_closed = true;
	if (m_outline) m_outline->Close();
	if (m_search) m_search->Close();
	if (m_scm) m_scm->Close();
	if (m_explorer) m_explorer->Close();
	for (auto& page : m_pages) {
		if (page.frameSurface && page.frameSurface->IsOpen()) {
			(void)page.frameSurface->Close();
		}
		page.attached = nullptr;
	}
	m_owner = nullptr;
}

CViewContainerPages::Page* CViewContainerPages::Find(std::string_view containerId) noexcept
{
	const auto found = std::find_if(m_pages.begin(), m_pages.end(),
		[containerId](const Page& page) { return page.id == containerId; });
	return found == m_pages.end() ? nullptr : &*found;
}

const CViewContainerPages::Page* CViewContainerPages::Find(std::string_view containerId) const noexcept
{
	const auto found = std::find_if(m_pages.begin(), m_pages.end(),
		[containerId](const Page& page) { return page.id == containerId; });
	return found == m_pages.end() ? nullptr : &*found;
}

HWND CViewContainerPages::PageWindow(const Page& page) const noexcept
{
	if (page.id == pageIds::Explorer) return m_explorer ? m_explorer->GetHwnd() : nullptr;
	if (page.id == pageIds::SourceControl) return m_scm ? m_scm->GetHwnd() : nullptr;
	if (page.id == pageIds::Search) return m_search ? m_search->GetHwnd() : nullptr;
	return nullptr;
}

void CViewContainerPages::Attach(
	std::string_view containerId, HWND host, std::string_view logicalHostId)
{
	if (!IsUsable()) return;
	Page* page = Find(containerId);
	if (page == nullptr) return;
	const HWND target = host != nullptr ? host : m_owner;
	if (target == nullptr || page->attached == host) return;
	SetPageVisible(containerId, false);
	if (const HWND window = PageWindow(*page);
		window != nullptr && ::IsWindow(window) && ::GetParent(window) != target) {
		if (::SetParent(window, target) == nullptr) return;
	}
	if (page->id == pageIds::Explorer && m_outline) (void)m_outline->Reparent(target);
	page->attached = host;
	if (page->frameSurface) (void)page->frameSurface->SetHost(logicalHostId);
}

void CViewContainerPages::SetPageVisible(std::string_view containerId, bool visible)
{
	if (!IsUsable()) return;
	const Page* page = Find(containerId);
	if (page == nullptr) return;
	if (page->id == pageIds::Explorer) {
		ShowPageWindow(PageWindow(*page), visible);
		if (m_outline) m_outline->SetVisible(visible && m_outlineExpanded);
	} else if (page->id == pageIds::SourceControl && m_scm) {
		ShowPageWindow(PageWindow(*page), visible);
	} else if (page->id == pageIds::Search && m_search) {
		ShowPageWindow(PageWindow(*page), visible);
	}
	if (page->frameSurface) (void)page->frameSurface->SetVisible(visible);
}

void CViewContainerPages::NotifyPageLayout(std::string_view containerId)
{
	if (!IsUsable()) return;
	Page* page = Find(containerId);
	if (page == nullptr || !page->frameSurface || !page->frameSurface->IsOpen()) return;
	(void)page->frameSurface->NotifyLayout();
}

std::vector<CViewContainerPages::FrameSurfaceProjection>
CViewContainerPages::FrameSurfaceProjections() const
{
	std::vector<FrameSurfaceProjection> projections;
	projections.reserve(m_pages.size());
	for (const auto& page : m_pages) {
		if (!page.frameSurface || !page.frameSurface->IsOpen()) continue;
		RECT client{ 0, 0, 1, 1 };
		const HWND window = PageWindow(page);
		if (window != nullptr && ::IsWindow(window)) {
			RECT measured{};
			if (::GetClientRect(window, &measured)) client = measured;
		}
		projections.push_back(FrameSurfaceProjection{
			.surface = page.frameSurface->Snapshot(),
			.width = static_cast<std::uint32_t>(std::max<LONG>(1, client.right - client.left)),
			.height = static_cast<std::uint32_t>(std::max<LONG>(1, client.bottom - client.top)),
		});
	}
	return projections;
}

std::vector<CViewContainerPages::FrameSurfaceProjection>
CViewContainerPages::CommitGdiFrame()
{
	std::vector<FrameSurfaceProjection> committed;
	for (auto& page : m_pages) {
		if (!page.frameSurface) continue;
		const auto snapshot = page.frameSurface->CommitGdiFrame();
		if (!snapshot.has_value()) continue;
		RECT client{ 0, 0, 1, 1 };
		const HWND window = PageWindow(page);
		if (window != nullptr && ::IsWindow(window)) {
			RECT measured{};
			if (::GetClientRect(window, &measured)) client = measured;
		}
		committed.push_back(FrameSurfaceProjection{
			.surface = *snapshot,
			.width = static_cast<std::uint32_t>(std::max<LONG>(1, client.right - client.left)),
			.height = static_cast<std::uint32_t>(std::max<LONG>(1, client.bottom - client.top)),
		});
	}
	return committed;
}

bool CViewContainerPages::Contains(std::string_view containerId) const noexcept
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

HWND CViewContainerPages::AttachedHost(std::string_view containerId) const noexcept
{
	const Page* page = Find(containerId);
	return page == nullptr ? nullptr : page->attached;
}

void CViewContainerPages::SetPalette(const theme::ThemePalette& palette)
{
	if (m_explorer) {
		explorer::ExplorerPalette explorerPalette;
		explorerPalette.background = palette.sideBar.ToColorRef();
		explorerPalette.text = palette.primaryText.ToColorRef();
		explorerPalette.secondaryText = palette.secondaryText.ToColorRef();
		explorerPalette.border = palette.border.ToColorRef();
		explorerPalette.focus = palette.accent.ToColorRef();
		explorerPalette.inactiveSelection = palette.raised.ToColorRef();
		explorerPalette.hover = palette.raised.ToColorRef();
		explorerPalette.selectionText = palette.highlightText.ToColorRef();
		explorerPalette.button = palette.buttonBackground.ToColorRef();
		explorerPalette.buttonHover = palette.buttonHoverBackground.ToColorRef();
		explorerPalette.buttonText = palette.buttonForeground.ToColorRef();
		explorerPalette.scrollbarThumb = palette.border.ToColorRef();
		explorerPalette.scrollbarThumbHover = palette.secondaryText.ToColorRef();
		explorerPalette.scrollbarTrackHover = palette.raised.ToColorRef();
		// Indexed by `decorations::EFileDecorationColor`; index 0 is `None` and is
		// never drawn, so it is filled with the row's own text color rather than a
		// color that would silently become visible if it ever were.
		explorerPalette.decorationColors = {
			palette.primaryText.ToColorRef(),
			palette.gitAddedResourceForeground.ToColorRef(),
			palette.gitModifiedResourceForeground.ToColorRef(),
			palette.gitDeletedResourceForeground.ToColorRef(),
			palette.gitRenamedResourceForeground.ToColorRef(),
			palette.gitStageModifiedResourceForeground.ToColorRef(),
			palette.gitStageDeletedResourceForeground.ToColorRef(),
			palette.gitUntrackedResourceForeground.ToColorRef(),
			palette.gitIgnoredResourceForeground.ToColorRef(),
			palette.gitConflictingResourceForeground.ToColorRef(),
			palette.gitSubmoduleResourceForeground.ToColorRef(),
		};
		m_explorer->SetPalette(explorerPalette);
	}
	if (m_outline) m_outline->SetPalette(palette);
	if (m_scm) m_scm->SetPalette(palette);
	if (m_search) m_search->SetPalette(palette);
}

std::wstring CViewContainerPages::PageTitle(std::string_view containerId) const
{
	const Page* page = Find(containerId);
	return page == nullptr ? std::wstring{} : std::wstring(LS(page->titleResourceId));
}

void CViewContainerPages::RefreshStrings()
{
	if (m_explorer) m_explorer->RefreshStrings();
	if (m_scm) m_scm->RefreshStrings();
	ApplySearchTexts();
	for (const auto& page : m_pages) {
		if (page.attached != nullptr) RedrawVisiblePage(PageWindow(page));
	}
}

//! The view owns no resource ids, so the composition layer resolves every
//! localized string and hands it over as one value.
void CViewContainerPages::ApplySearchTexts()
{
	if (!m_search) return;
	search::SearchViewTexts texts;
	texts.searchPlaceholder = LS(STR_WORKBENCH_SEARCH_PLACEHOLDER);
	texts.replacePlaceholder = LS(STR_WORKBENCH_SEARCH_REPLACE_PLACEHOLDER);
	texts.toggleReplace = LS(STR_WORKBENCH_SEARCH_TOGGLE_REPLACE);
	texts.matchCase = LS(STR_WORKBENCH_SEARCH_MATCH_CASE);
	texts.wholeWord = LS(STR_WORKBENCH_SEARCH_WHOLE_WORD);
	texts.useRegex = LS(STR_WORKBENCH_SEARCH_USE_REGEX);
	texts.preserveCase = LS(STR_WORKBENCH_SEARCH_PRESERVE_CASE);
	texts.replaceAll = LS(STR_WORKBENCH_SEARCH_REPLACE_ALL);
	texts.replaceOne = LS(STR_WORKBENCH_SEARCH_REPLACE_ONE);
	texts.replaceInFile = LS(STR_WORKBENCH_SEARCH_REPLACE_IN_FILE);
	texts.dismiss = LS(STR_WORKBENCH_SEARCH_DISMISS);
	texts.noResults = LS(STR_WORKBENCH_SEARCH_NO_RESULTS);
	texts.resultSummary = LS(STR_WORKBENCH_SEARCH_RESULT_SUMMARY);
	texts.searching = LS(STR_WORKBENCH_SEARCH_SEARCHING);
	texts.limitHit = LS(STR_WORKBENCH_SEARCH_LIMIT_HIT);
	texts.regexUnavailable = LS(STR_WORKBENCH_SEARCH_REGEX_UNAVAILABLE);
	texts.invalidPattern = LS(STR_WORKBENCH_SEARCH_INVALID_PATTERN);
	texts.noWorkspace = LS(STR_WORKBENCH_SEARCH_NO_WORKSPACE);
	texts.replaceFailed = LS(STR_WORKBENCH_SEARCH_REPLACE_FAILED);
	m_search->SetTexts(std::move(texts));
}

} // namespace workbench::viewcontainer

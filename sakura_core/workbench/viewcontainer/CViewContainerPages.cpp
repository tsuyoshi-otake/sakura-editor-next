/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "workbench/viewcontainer/CViewContainerPages.h"
#include "workbench/controls/COverlayScrollbar.h"
#include "CSelectLang.h"

#include <algorithm>
#include <utility>

namespace workbench::viewcontainer {

namespace {

constexpr wchar_t kPageWindowClass[] = L"SakuraEditorNext.ViewContainerPage";

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

[[nodiscard]] bool ReparentWindow(HWND window, HWND parent) noexcept
{
	if (window == nullptr || !::IsWindow(window) || parent == nullptr || !::IsWindow(parent)) {
		return false;
	}
	if (::GetParent(window) == parent) return true;
	::SetLastError(ERROR_SUCCESS);
	const HWND previous = ::SetParent(window, parent);
	return (previous != nullptr || ::GetLastError() == ERROR_SUCCESS)
		&& ::GetParent(window) == parent;
}

LRESULT CALLBACK PageWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
	switch (message) {
	case WM_ERASEBKGND:
		return 1;
	case WM_PAINT: {
		PAINTSTRUCT paint{};
		(void)::BeginPaint(window, &paint);
		::EndPaint(window, &paint);
		return 0;
	}
	case WM_NCHITTEST:
		return HTTRANSPARENT;
	}
	return ::DefWindowProcW(window, message, wParam, lParam);
}

[[nodiscard]] bool EnsurePageWindowClass(HINSTANCE instance) noexcept
{
	static ATOM atom = 0;
	if (atom != 0) return true;
	WNDCLASSEXW windowClass{};
	windowClass.cbSize = sizeof(windowClass);
	windowClass.hInstance = instance;
	windowClass.lpfnWndProc = PageWindowProc;
	windowClass.lpszClassName = kPageWindowClass;
	atom = ::RegisterClassExW(&windowClass);
	if (atom != 0) return true;
	if (::GetLastError() == ERROR_CLASS_ALREADY_EXISTS) {
		atom = 1;
		return true;
	}
	return false;
}

} // namespace

class CViewContainerPages::PageAdapter final : public IViewContainerPage {
public:
	PageAdapter(CViewContainerPages& owner, const PageKind kind)
		: m_owner(owner)
		, m_kind(kind)
		, m_frameSurface(static_cast<rendering::FrameSurfaceId>(kind) + 1)
	{
	}

	~PageAdapter() override
	{
		Cleanup();
	}

	[[nodiscard]] bool CreateNative(HWND parkingParent) noexcept
	{
		if (m_created || m_closeInvoked || parkingParent == nullptr || !::IsWindow(parkingParent)) {
			return false;
		}
		m_parkingParent = parkingParent;
		HINSTANCE instance = reinterpret_cast<HINSTANCE>(
			::GetWindowLongPtrW(parkingParent, GWLP_HINSTANCE));
		if (instance == nullptr) instance = ::GetModuleHandleW(nullptr);
		if (!EnsurePageWindowClass(instance)) return false;
		// One page-owned wrapper is the only HWND the pool reparents. All native roots,
		// including Explorer's nested Outline View, remain children of this wrapper, so
		// a native reparent has no partially-moved multi-window failure state.
		m_window = ::CreateWindowExW(WS_EX_TRANSPARENT, kPageWindowClass, L"",
			WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, 0, 0, 0, 0,
			parkingParent, nullptr, instance, nullptr);
		if (m_window == nullptr) return false;
		try {
			switch (m_kind) {
			case PageKind::Explorer:
				m_explorer = std::make_unique<explorer::CExplorerTool>();
				m_outline = std::make_unique<outline::COutlineWorkbenchTool>(m_owner.m_dialog);
				if (!m_explorer->Create(m_window) || !m_outline->Create(m_window)) {
					Cleanup();
					return false;
				}
				break;
			case PageKind::SourceControl:
				m_scm = std::make_unique<scm::CScmWorkbenchTool>();
				if (!m_scm->Create(m_window)) {
					Cleanup();
					return false;
				}
				break;
			case PageKind::Search:
				m_search = std::make_unique<search::CSearchWorkbenchTool>();
				if (!m_search->Create(m_window)) {
					Cleanup();
					return false;
				}
				break;
			case PageKind::Extensions:
				m_extensions = std::make_unique<extensions::CExtensionsWorkbenchTool>();
				if (!m_extensions->Create(m_window)) {
					Cleanup();
					return false;
				}
				break;
			}
		} catch (...) {
			Cleanup();
			return false;
		}
		ApplyVisible(false);
		if (!m_frameSurface.Open("workbench.window.detached", false).Accepted()) {
			Cleanup();
			return false;
		}
		m_created = true;
		m_owner.Bind(m_kind, this);
		return true;
	}

	[[nodiscard]] std::string_view ContainerId() const noexcept override
	{
		switch (m_kind) {
		case PageKind::Explorer: return pageIds::Explorer;
		case PageKind::SourceControl: return pageIds::SourceControl;
		case PageKind::Search: return pageIds::Search;
		case PageKind::Extensions: return pageIds::Extensions;
		}
		return {};
	}

	[[nodiscard]] ViewContainerFocusCaptureResult CaptureFocusToken() noexcept override
	{
		if (!m_attachedHost) return { EViewContainerFocusCaptureStatus::NoFocus, std::nullopt };
		const HWND focused = ::GetFocus();
		if (focused == nullptr || !OwnsWindow(focused)) {
			return { EViewContainerFocusCaptureStatus::NoFocus, std::nullopt };
		}
		return { EViewContainerFocusCaptureStatus::Captured,
			ViewContainerFocusToken{ reinterpret_cast<ViewContainerNativeHandle>(focused) } };
	}

	[[nodiscard]] EViewContainerPageDetachStatus Detach(
		const ViewContainerPageHost& host) noexcept override
	{
		if (!m_created || !m_attachedHost || *m_attachedHost != host) {
			return EViewContainerPageDetachStatus::Failed;
		}
		ApplyVisible(false);
		const HWND previousHost = reinterpret_cast<HWND>(host.nativeParent);
		if (previousHost != nullptr && ::IsWindow(previousHost)) {
			::InvalidateRect(previousHost, nullptr, FALSE);
		}
		m_attachedHost.reset();
		return EViewContainerPageDetachStatus::Detached;
	}

	[[nodiscard]] EViewContainerPageReparentStatus Reparent(
		const ViewContainerNativeHandle nativeParent) noexcept override
	{
		if (!m_created || m_attachedHost) return EViewContainerPageReparentStatus::Failed;
		if (nativeParent == m_nativeParent) return EViewContainerPageReparentStatus::Reparented;
		HWND target = nativeParent == 0 ? m_parkingParent
			: reinterpret_cast<HWND>(nativeParent);
		if (target == nullptr || !::IsWindow(target)) return EViewContainerPageReparentStatus::Failed;
		if (!ReparentWindow(m_window, target)) return EViewContainerPageReparentStatus::Failed;
		m_nativeParent = nativeParent;
		return EViewContainerPageReparentStatus::Reparented;
	}

	[[nodiscard]] EViewContainerPageAttachStatus Attach(
		const ViewContainerPageHost& host) noexcept override
	{
		if (!m_created || m_attachedHost || host.nativeParent == 0
			|| host.nativeParent != m_nativeParent) {
			return EViewContainerPageAttachStatus::Failed;
		}
		std::optional<ViewContainerPageHost> staged;
		try {
			staged.emplace(host);
		} catch (...) {
			return EViewContainerPageAttachStatus::Failed;
		}
		m_attachedHost = std::move(staged);
		ApplyVisible(m_visible && m_layoutValid);
		return EViewContainerPageAttachStatus::Attached;
	}

	[[nodiscard]] EViewContainerFocusRestoreStatus RestoreFocusToken(
		const ViewContainerFocusToken token) noexcept override
	{
		const HWND target = reinterpret_cast<HWND>(token.value);
		if (!m_attachedHost || target == nullptr || !::IsWindow(target) || !OwnsWindow(target)) {
			return EViewContainerFocusRestoreStatus::Failed;
		}
		(void)::SetFocus(target);
		return ::GetFocus() == target ? EViewContainerFocusRestoreStatus::Restored
			: EViewContainerFocusRestoreStatus::Failed;
	}

	[[nodiscard]] EViewContainerPageCloseStatus Close() noexcept override
	{
		if (m_closeInvoked) return EViewContainerPageCloseStatus::Closed;
		m_closeInvoked = true;
		const bool frameClosed = !m_frameSurface.IsOpen() || m_frameSurface.Close().Accepted();
		Cleanup();
		return frameClosed ? EViewContainerPageCloseStatus::Closed
			: EViewContainerPageCloseStatus::Failed;
	}

	void SetVisible(const bool visible) noexcept
	{
		const bool effectiveVisible = visible && m_layoutValid;
		if (!m_frameSurface.SetVisible(
			m_attachedHost.has_value() && effectiveVisible).Accepted()) return;
		m_visible = visible;
		if (m_attachedHost) ApplyVisible(effectiveVisible);
	}

	void SynchronizeFrame(const std::optional<ViewContainerPageState>& state) noexcept
	{
		if (!state || state->state != EViewContainerPageStableState::Attached || !state->host) {
			(void)m_frameSurface.SetVisible(false);
			(void)m_frameSurface.SetHost("workbench.window.detached");
			return;
		}
		(void)m_frameSurface.SetHost(state->host->id);
		(void)m_frameSurface.SetVisible(m_visible && m_layoutValid);
	}

	void Layout(const RECT& bounds, const RECT* excludedHostChrome) noexcept
	{
		if (m_window == nullptr || !::IsWindow(m_window)) return;
		const LONG width = std::max(0L, bounds.right - bounds.left);
		const LONG height = std::max(0L, bounds.bottom - bounds.top);
		if (!::SetWindowPos(m_window, nullptr, bounds.left, bounds.top, width, height,
			SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOCOPYBITS | SWP_NOREDRAW)) {
			m_layoutValid = false;
			ApplyVisible(false);
			return;
		}
		if (excludedHostChrome == nullptr || ::IsRectEmpty(excludedHostChrome)) {
			m_layoutValid = ::SetWindowRgn(m_window, nullptr, TRUE) != 0;
			if (!m_layoutValid) ApplyVisible(false);
			return;
		}
		const LONG excludedTop = std::clamp(
			excludedHostChrome->top - bounds.top, 0L, height);
		const LONG excludedBottom = std::clamp(
			excludedHostChrome->bottom - bounds.top, excludedTop, height);
		HRGN contentRegion = ::CreateRectRgn(0, 0, width, excludedTop);
		HRGN lowerRegion = ::CreateRectRgn(0, excludedBottom, width, height);
		if (contentRegion == nullptr || lowerRegion == nullptr) {
			if (contentRegion != nullptr) ::DeleteObject(contentRegion);
			if (lowerRegion != nullptr) ::DeleteObject(lowerRegion);
			m_layoutValid = false;
			ApplyVisible(false);
			return;
		}
		(void)::CombineRgn(contentRegion, contentRegion, lowerRegion, RGN_OR);
		::DeleteObject(lowerRegion);
		// SetWindowRgn owns the region only on success. The hole is host-owned chrome,
		// so WS_CLIPCHILDREN excludes content roots but not the Outline header.
		if (::SetWindowRgn(m_window, contentRegion, TRUE) == 0) {
			::DeleteObject(contentRegion);
			m_layoutValid = false;
			ApplyVisible(false);
			return;
		}
		m_layoutValid = true;
	}

	[[nodiscard]] bool OwnsWindow(HWND target) const noexcept
	{
		if (target == nullptr || !::IsWindow(target)) return false;
		const auto owns = [target](HWND root) noexcept {
			return root != nullptr && ::IsWindow(root)
				&& (target == root || ::IsChild(root, target));
		};
		return owns(m_window);
	}

	[[nodiscard]] HWND PrimaryWindow() const noexcept
	{
		return m_window;
	}

	[[nodiscard]] HWND ContentWindow() const noexcept
	{
		switch (m_kind) {
		case PageKind::Explorer: return m_explorer ? m_explorer->GetHwnd() : nullptr;
		case PageKind::SourceControl: return m_scm ? m_scm->GetHwnd() : nullptr;
		case PageKind::Search: return m_search ? m_search->GetHwnd() : nullptr;
		case PageKind::Extensions: return m_extensions ? m_extensions->GetHwnd() : nullptr;
		}
		return nullptr;
	}

	[[nodiscard]] rendering::FrameSurfaceCommitState& FrameSurface() noexcept
	{
		return m_frameSurface;
	}
	[[nodiscard]] const rendering::FrameSurfaceCommitState& FrameSurface() const noexcept
	{
		return m_frameSurface;
	}

	[[nodiscard]] explorer::CExplorerTool* Explorer() const noexcept { return m_explorer.get(); }
	[[nodiscard]] outline::COutlineWorkbenchTool* Outline() const noexcept { return m_outline.get(); }
	[[nodiscard]] scm::CScmWorkbenchTool* SourceControl() const noexcept { return m_scm.get(); }
	[[nodiscard]] search::CSearchWorkbenchTool* Search() const noexcept { return m_search.get(); }
	[[nodiscard]] extensions::CExtensionsWorkbenchTool* Extensions() const noexcept
	{
		return m_extensions.get();
	}

private:
	void ApplyVisible(const bool visible) noexcept
	{
		if (!visible) ShowPageWindow(m_window, false);
		ShowPageWindow(ContentWindow(), visible);
		if (m_kind == PageKind::Explorer && m_outline) {
			m_outline->SetVisible(visible && m_owner.m_outlineExpanded);
		}
		if (visible) ShowPageWindow(m_window, true);
	}

	void Cleanup() noexcept
	{
		m_owner.Unbind(m_kind, this);
		m_attachedHost.reset();
		m_nativeParent = 0;
		if (m_outline) m_outline->Close();
		if (m_search) m_search->Close();
		if (m_extensions) m_extensions->Close();
		if (m_scm) m_scm->Close();
		if (m_explorer) m_explorer->Close();
		m_outline.reset();
		m_search.reset();
		m_extensions.reset();
		m_scm.reset();
		m_explorer.reset();
		if (m_window != nullptr && ::IsWindow(m_window)) ::DestroyWindow(m_window);
		m_window = nullptr;
		m_created = false;
	}

	CViewContainerPages& m_owner;
	PageKind m_kind;
	rendering::FrameSurfaceCommitState m_frameSurface;
	std::unique_ptr<explorer::CExplorerTool> m_explorer;
	std::unique_ptr<outline::COutlineWorkbenchTool> m_outline;
	std::unique_ptr<scm::CScmWorkbenchTool> m_scm;
	std::unique_ptr<search::CSearchWorkbenchTool> m_search;
	std::unique_ptr<extensions::CExtensionsWorkbenchTool> m_extensions;
	std::optional<ViewContainerPageHost> m_attachedHost;
	HWND m_window = nullptr;
	HWND m_parkingParent = nullptr;
	ViewContainerNativeHandle m_nativeParent{};
	bool m_visible = false;
	bool m_layoutValid = true;
	bool m_created = false;
	bool m_closeInvoked = false;
};

CViewContainerPages::CViewContainerPages(CDlgFuncList& dialog)
	: m_dialog(dialog)
	, m_pool(m_registry)
{
	m_pages.push_back({ std::string(pageIds::Explorer), STR_WORKBENCH_EXPLORER_TITLE,
		PageKind::Explorer, nullptr });
	m_pages.push_back({ std::string(pageIds::SourceControl), STR_WORKBENCH_SOURCE_CONTROL_TITLE,
		PageKind::SourceControl, nullptr });
	m_pages.push_back({ std::string(pageIds::Search), STR_WORKBENCH_SEARCH_TITLE,
		PageKind::Search, nullptr });
	m_pages.push_back({ std::string(pageIds::Extensions), STR_WORKBENCH_EXTENSIONS_TITLE,
		PageKind::Extensions, nullptr });
}

CViewContainerPages::~CViewContainerPages()
{
	Close();
}

bool CViewContainerPages::Create(HWND owner)
{
	if (m_closed || m_created || owner == nullptr) return false;
	m_owner = owner;
	const layout::SupportedViewContainerLocations sideBars{
		layout::EViewContainerLocation::Sidebar,
		layout::EViewContainerLocation::AuxiliaryBar,
	};
	std::vector<ViewContainerPageDescriptor> descriptors;
	descriptors.reserve(m_pages.size());
	for (const auto& page : m_pages) {
		descriptors.push_back({ page.id, sideBars,
			[this, kind = page.kind]() { return CreatePage(kind); } });
	}
	if (!m_registry.RegisterBatch(std::move(descriptors)).Succeeded()) {
		Close();
		return false;
	}
	for (const auto& page : m_pages) {
		if (!m_pool.Acquire(page.id).Succeeded()) {
			Close();
			return false;
		}
	}
	ApplySearchTexts();
	m_created = true;
	return true;
}

void CViewContainerPages::Close()
{
	if (m_closed) return;
	m_closed = true;
	(void)m_pool.Shutdown();
	m_created = false;
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
	return page.adapter ? page.adapter->PrimaryWindow() : nullptr;
}

std::unique_ptr<IViewContainerPage> CViewContainerPages::CreatePage(const PageKind kind) noexcept
{
	try {
		auto page = std::make_unique<PageAdapter>(*this, kind);
		if (!page->CreateNative(m_owner)) return {};
		return page;
	} catch (...) {
		return {};
	}
}

void CViewContainerPages::Bind(const PageKind kind, PageAdapter* adapter) noexcept
{
	const auto found = std::find_if(m_pages.begin(), m_pages.end(),
		[kind](const Page& page) { return page.kind == kind; });
	if (found != m_pages.end()) found->adapter = adapter;
}

void CViewContainerPages::Unbind(const PageKind kind, const PageAdapter* adapter) noexcept
{
	const auto found = std::find_if(m_pages.begin(), m_pages.end(),
		[kind](const Page& page) { return page.kind == kind; });
	if (found != m_pages.end() && found->adapter == adapter) found->adapter = nullptr;
}

ViewContainerPagePoolAttachResult CViewContainerPages::Attach(
	const std::string_view containerId, const ViewContainerPageHost& host) noexcept
{
	if (!IsUsable()) {
		return { EViewContainerPagePoolAttachStatus::PageUnavailable,
			EViewContainerPageTransitionStage::None, EViewContainerPageTransitionStage::None,
			std::nullopt, EViewContainerPageCleanupOwner::None };
	}
	auto result = m_pool.Attach(containerId, host);
	if (Page* page = Find(containerId); page != nullptr && page->adapter != nullptr
		&& result.finalState) {
		// Rendering metadata follows the pool's terminal state; it never participates in
		// the allocation-safe native compensation transaction above.
		page->adapter->SynchronizeFrame(result.finalState);
	}
	return result;
}

ViewContainerPagePoolDetachResult CViewContainerPages::Detach(
	const std::string_view containerId) noexcept
{
	if (!IsUsable()) {
		return { EViewContainerPagePoolDetachStatus::NotAcquired,
			EViewContainerPageTransitionStage::None, EViewContainerPageTransitionStage::None,
			std::nullopt, EViewContainerPageCleanupOwner::None };
	}
	auto result = m_pool.Detach(containerId);
	if (Page* page = Find(containerId); page != nullptr && page->adapter != nullptr
		&& result.finalState) {
		page->adapter->SynchronizeFrame(result.finalState);
	}
	return result;
}

void CViewContainerPages::SetPageVisible(std::string_view containerId, bool visible)
{
	if (!IsUsable()) return;
	Page* page = Find(containerId);
	if (page == nullptr || page->adapter == nullptr) return;
	page->adapter->SetVisible(visible);
}

void CViewContainerPages::NotifyPageLayout(std::string_view containerId)
{
	if (!IsUsable()) return;
	Page* page = Find(containerId);
	if (page == nullptr || page->adapter == nullptr
		|| !page->adapter->FrameSurface().IsOpen()) return;
	(void)page->adapter->FrameSurface().NotifyLayout();
}

std::vector<CViewContainerPages::FrameSurfaceProjection>
CViewContainerPages::FrameSurfaceProjections() const
{
	std::vector<FrameSurfaceProjection> projections;
	projections.reserve(m_pages.size());
	for (const auto& page : m_pages) {
		if (page.adapter == nullptr || !page.adapter->FrameSurface().IsOpen()) continue;
		RECT client{ 0, 0, 1, 1 };
		const HWND window = PageWindow(page);
		if (window != nullptr && ::IsWindow(window)) {
			RECT measured{};
			if (::GetClientRect(window, &measured)) client = measured;
		}
		projections.push_back(FrameSurfaceProjection{
			.surface = page.adapter->FrameSurface().Snapshot(),
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
		if (page.adapter == nullptr) continue;
		const auto snapshot = page.adapter->FrameSurface().CommitGdiFrame();
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
	const auto state = m_pool.State(containerId);
	if (state.status != EViewContainerPageStateStatus::Found || !state.state
		|| state.state->state != EViewContainerPageStableState::Attached
		|| !state.state->host) {
		return nullptr;
	}
	return reinterpret_cast<HWND>(state.state->host->nativeParent);
}

bool CViewContainerPages::IsMessageTarget(
	const std::string_view containerId, HWND target) const noexcept
{
	const Page* page = Find(containerId);
	return page != nullptr && page->adapter != nullptr && AttachedHost(containerId) != nullptr
		&& page->adapter->OwnsWindow(target);
}

void CViewContainerPages::LayoutPage(
	const std::string_view containerId, const RECT& bounds,
	const RECT* excludedHostChrome) noexcept
{
	Page* page = Find(containerId);
	if (page != nullptr && page->adapter != nullptr) {
		page->adapter->Layout(bounds, excludedHostChrome);
	}
}

void CViewContainerPages::SetPalette(const theme::ThemePalette& palette)
{
	if (auto* explorer = Explorer()) {
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
		const auto scrollbarColors = controls::ResolveOverlayScrollbarColors(palette, palette.sideBar);
		explorerPalette.scrollbarThumb = scrollbarColors.thumb;
		explorerPalette.scrollbarThumbHover = scrollbarColors.thumbHover;
		explorerPalette.scrollbarThumbActive = scrollbarColors.thumbActive;
		explorerPalette.scrollbarTrackHover = scrollbarColors.trackHover;
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
		explorer->SetPalette(explorerPalette);
	}
	if (auto* outline = Outline()) outline->SetPalette(palette);
	if (auto* scm = SourceControl()) scm->SetPalette(palette);
	if (auto* search = Search()) search->SetPalette(palette);
	if (auto* extensions = Extensions()) extensions->SetPalette(palette);
}

std::wstring CViewContainerPages::PageTitle(std::string_view containerId) const
{
	const Page* page = Find(containerId);
	if (page == nullptr) return {};
	return std::wstring(LS(page->titleResourceId));
}

void CViewContainerPages::RefreshStrings()
{
	if (auto* explorer = Explorer()) explorer->RefreshStrings();
	if (auto* scm = SourceControl()) scm->RefreshStrings();
	if (auto* extensions = Extensions()) extensions->RefreshStrings();
	ApplySearchTexts();
	for (const auto& page : m_pages) {
		if (AttachedHost(page.id) != nullptr) RedrawVisiblePage(PageWindow(page));
	}
}

//! The view owns no resource ids, so the composition layer resolves every
//! localized string and hands it over as one value.
void CViewContainerPages::ApplySearchTexts()
{
	auto* search = Search();
	if (!search) return;
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
	search->SetTexts(std::move(texts));
}

explorer::CExplorerTool* CViewContainerPages::Explorer() const noexcept
{
	const Page* page = Find(pageIds::Explorer);
	return page && page->adapter ? page->adapter->Explorer() : nullptr;
}

outline::COutlineWorkbenchTool* CViewContainerPages::Outline() const noexcept
{
	const Page* page = Find(pageIds::Explorer);
	return page && page->adapter ? page->adapter->Outline() : nullptr;
}

scm::CScmWorkbenchTool* CViewContainerPages::SourceControl() const noexcept
{
	const Page* page = Find(pageIds::SourceControl);
	return page && page->adapter ? page->adapter->SourceControl() : nullptr;
}

search::CSearchWorkbenchTool* CViewContainerPages::Search() const noexcept
{
	const Page* page = Find(pageIds::Search);
	return page && page->adapter ? page->adapter->Search() : nullptr;
}

extensions::CExtensionsWorkbenchTool* CViewContainerPages::Extensions() const noexcept
{
	const Page* page = Find(pageIds::Extensions);
	return page && page->adapter ? page->adapter->Extensions() : nullptr;
}

} // namespace workbench::viewcontainer

/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#include "StdAfx.h"
#include "workbench/SenpDeclaredTreeViews.h"
#include "workbench/SenpExtensionActivation.h"
#include "workbench/commands/CommandArgumentsJson.h"
#include "workbench/tree/SenpTreeView.h"
#include "theme/CThemeService.h"
#include <CommCtrl.h>
#include <algorithm>
#include <map>
#include <utility>

namespace workbench {
namespace {
constexpr wchar_t kClass[] = L"SakuraSenpDeclaredTreeView";
constexpr UINT kActivate = WM_APP + 0x5eb;
constexpr UINT_PTR kRetrySubclass = 296;
using Activation = SenpExtensionActivationState;
int Dip(int value, unsigned int dpi) noexcept { return ::MulDiv(value, dpi, 96); }
const wchar_t* StatusText(Activation state) noexcept
{
	switch (state) {
	case Activation::Dormant: return L"View is not active.";
	case Activation::Queued: return L"Waiting to activate extension...";
	case Activation::Preparing: return L"Activating extension...";
	case Activation::Active: return L"No data provider is registered for this view.";
	case Activation::Failed: return L"The extension could not be activated.";
	case Activation::Busy: return L"The extension could not start because runtime capacity is in use.";
	case Activation::Unsupported: return L"This extension cannot provide this view in this environment.";
	case Activation::Disabled: return L"This extension is disabled.";
	case Activation::Stopped: return L"This extension has stopped.";
	}
	return L"This view is unavailable.";
}
bool CanRetry(Activation state) noexcept { return state == Activation::Failed || state == Activation::Busy; }
void Fill(HDC dc, const RECT& bounds, COLORREF color) noexcept
{
	const auto old = ::SetDCBrushColor(dc, color);
	::FillRect(dc, &bounds, static_cast<HBRUSH>(::GetStockObject(DC_BRUSH)));
	::SetDCBrushColor(dc, old);
}
}

struct CSenpDeclaredTreeViews::Impl final : std::enable_shared_from_this<Impl> {
	struct Slot final {
		std::weak_ptr<Impl> cohort;
		std::wstring id, title;
		viewcontainer::SenpViewBodyHost host;
		HWND window{}, message{}, retry{};
		std::unique_ptr<tree::CSenpTreeView> current, pending;
		theme::CThemeFont font;
		theme::ThemePalette palette{ theme::CThemeService::PaletteFor(theme::ThemeMode::Dark) };
		layout::EViewContainerLocation location{ layout::EViewContainerLocation::Sidebar };
		Activation rendered{ Activation::Stopped };
		unsigned int dpi{ 96 };
		bool mounted{}, closed{}, visible{}, posted{}, explicitRetry{}, hover{}, showingTree{};
		~Slot() { Close(); }
		COLORREF Surface() const noexcept
		{ return (location == layout::EViewContainerLocation::Panel ? palette.bottomPanel : palette.sideBar).ToColorRef(); }
		bool Usable() const noexcept { return !closed && window && message && retry; }
		void Fault() noexcept
		{
			if (auto owner = cohort.lock()) owner->failed = true;
			if (auto report = std::exchange(host.projectionFailed, {})) report();
		}
		void Close() noexcept
		{
			if (closed) return;
			closed = true; posted = false; host = {};
			if (pending) pending->Close();
			if (current) current->Close();
			pending.reset(); current.reset();
			if (window) ::DestroyWindow(window);
			window = message = retry = nullptr; font.Reset();
		}
		bool Initialize(viewcontainer::SenpViewBodyHost value)
		{
			if (mounted || closed || !::IsWindow(value.parent)) return false;
			mounted = true; host = std::move(value);
			WNDCLASSEXW cls{ sizeof(cls) }; cls.lpfnWndProc = Procedure; cls.hInstance = ::GetModuleHandleW(nullptr);
			cls.hCursor = ::LoadCursorW(nullptr, IDC_ARROW); cls.lpszClassName = kClass;
			if (!::RegisterClassExW(&cls) && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
			window = ::CreateWindowExW(WS_EX_CONTROLPARENT, kClass, title.c_str(),
				WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, 0, 0, 0, 0, host.parent, nullptr, cls.hInstance, this);
			if (!window) return false;
			message = ::CreateWindowExW(0, L"STATIC", StatusText(Activation::Dormant),
				WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX, 0, 0, 0, 0, window, reinterpret_cast<HMENU>(1), cls.hInstance, nullptr);
			retry = ::CreateWindowExW(0, L"BUTTON", L"Retry", WS_CHILD | WS_TABSTOP | BS_OWNERDRAW,
				0, 0, 0, 0, window, reinterpret_cast<HMENU>(2), cls.hInstance, nullptr);
			return message && retry
				&& ::SetWindowSubclass(message, RetryProcedure, kRetrySubclass, reinterpret_cast<DWORD_PTR>(this))
				&& ::SetWindowSubclass(retry, RetryProcedure, kRetrySubclass, reinterpret_cast<DWORD_PTR>(this)) && Metrics();
		}
		bool Metrics() noexcept
		{
			if (!font.Recreate(theme::ThemeFontKind::Chrome, dpi)) return false;
			::SendMessageW(message, WM_SETFONT, reinterpret_cast<WPARAM>(font.Get()), FALSE);
			::SendMessageW(retry, WM_SETFONT, reinterpret_cast<WPARAM>(font.Get()), FALSE);
			return true;
		}
		void LayoutChildren() noexcept
		{
			if (!Usable()) return;
			RECT bounds{}; ::GetClientRect(window, &bounds);
			if (current) current->Layout(bounds, dpi);
			const int inset = Dip(12, dpi), width = std::max(0L, bounds.right - inset * 2);
			RECT text{ 0, 0, width, 0 };
			if (const HDC dc = ::GetDC(window)) {
				const auto old = ::SelectObject(dc, font.Get());
				::DrawTextW(dc, StatusText(rendered), -1, &text, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
				::SelectObject(dc, old); ::ReleaseDC(window, dc);
			}
			const int height = std::max<long>(Dip(22, dpi), text.bottom);
			if (!::SetWindowPos(message, nullptr, inset, inset, width, height, SWP_NOACTIVATE | SWP_NOZORDER)
				|| !::SetWindowPos(retry, nullptr, inset, inset + height + Dip(8, dpi),
					std::min(width, Dip(100, dpi)), Dip(26, dpi), SWP_NOACTIVATE | SWP_NOZORDER)) Fault();
		}
		void Request(bool isRetry) noexcept
		{
			const auto owner = cohort.lock();
			if (!Usable() || !owner || owner->closed || owner->failed || posted || owner->bindingOwner
				|| (isRetry ? !CanRetry(owner->activation) : owner->activation != Activation::Dormant)) return;
			explicitRetry = isRetry;
			posted = ::PostMessageW(window, kActivate, 0, 0) != FALSE;
			if (!posted) Fault();
			else ::EnableWindow(retry, FALSE);
		}
		bool Project(Activation state, bool bound) noexcept
		{
			if (!Usable()) return false;
			if (!bound) {
				if (pending) pending->Close();
				if (current) current->Close();
				pending.reset(); current.reset();
			} else if (pending) {
				if (current) current->Close();
				current = std::move(pending);
				current->SetPalette(palette, location);
				RECT bounds{}; ::GetClientRect(window, &bounds); current->Layout(bounds, dpi);
				current->SetVisible(visible);
			}
			const bool treeVisible = current != nullptr;
			const bool changed = rendered != state || showingTree != treeVisible;
			rendered = state; showingTree = treeVisible;
			if (changed) {
				::SetWindowTextW(message, StatusText(state));
				LayoutChildren();
				::ShowWindow(message, treeVisible ? SW_HIDE : SW_SHOWNA);
				::ShowWindow(retry, !treeVisible && CanRetry(state) ? SW_SHOWNA : SW_HIDE);
				::RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
			}
			const bool enabled = !posted && CanRetry(state);
			if ((::IsWindowEnabled(retry) != FALSE) != enabled) ::EnableWindow(retry, enabled);
			return !current || current->IsUsable();
		}
		void DrawRetry(const DRAWITEMSTRUCT& draw) noexcept
		{
			const bool enabled = !(draw.itemState & ODS_DISABLED), pressed = (draw.itemState & ODS_SELECTED) != 0;
			Fill(draw.hDC, draw.rcItem, (enabled ? (hover || pressed ? palette.buttonHoverBackground
				: palette.buttonBackground) : palette.sideBar).ToColorRef());
			const auto old = ::SelectObject(draw.hDC, font.Get());
			::SetBkMode(draw.hDC, TRANSPARENT); ::SetTextColor(draw.hDC, (enabled ? palette.buttonForeground : palette.secondaryText).ToColorRef());
			auto text = draw.rcItem; if (pressed) ::OffsetRect(&text, 0, 1);
			::DrawTextW(draw.hDC, L"Retry", -1, &text, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
			if (draw.itemState & ODS_FOCUS) { auto focus = draw.rcItem; ::InflateRect(&focus, -2, -2); ::DrawFocusRect(draw.hDC, &focus); }
			::SelectObject(draw.hDC, old);
		}
		static LRESULT CALLBACK RetryProcedure(HWND window, UINT message, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR data) noexcept
		{
			auto& self = *reinterpret_cast<Slot*>(data);
			// DrawRetry fills the whole item; the show-time erase would paint btnface first.
			if (window == self.retry && message == WM_ERASEBKGND) return 1;
			try {
				if (message == WM_MOUSEMOVE) {
					TRACKMOUSEEVENT track{ sizeof(track), TME_LEAVE, window, 0 }; ::TrackMouseEvent(&track);
					if (window == self.retry && !self.hover) { self.hover = true; ::InvalidateRect(window, nullptr, FALSE); }
				} else if (window == self.retry && message == WM_MOUSELEAVE) { self.hover = false; ::InvalidateRect(window, nullptr, FALSE); }
				if (message == WM_SETFOCUS || message == WM_KILLFOCUS || message == WM_MOUSEMOVE || message == WM_MOUSELEAVE)
					if (self.host.interactionChanged) self.host.interactionChanged();
				if (window == self.retry && message == WM_KEYDOWN && w == VK_RETURN) { self.Request(true); return 0; }
			} catch (...) { self.Fault(); }
			return ::DefSubclassProc(window, message, w, l);
		}
		static LRESULT CALLBACK Procedure(HWND window, UINT message, WPARAM w, LPARAM l) noexcept
		{
			auto* self = reinterpret_cast<Slot*>(::GetWindowLongPtrW(window, GWLP_USERDATA));
			if (message == WM_NCCREATE) {
				self = static_cast<Slot*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
				::SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self)); self->window = window;
			}
			if (!self) return ::DefWindowProcW(window, message, w, l);
			try {
				switch (message) {
				case kActivate: {
					if (!std::exchange(self->posted, false)) return 0;
					const auto owner = self->cohort.lock();
					if (!owner || owner->closed || owner->failed) return 0;
					if (!self->visible || owner->bindingOwner) { (void)owner->Project(); return 0; }
					owner->activation = owner->request(self->id, std::exchange(self->explicitRetry, false));
					(void)owner->Project(); return 0;
				}
				case WM_COMMAND: if (LOWORD(w) == 2 && HIWORD(w) == BN_CLICKED) { self->Request(true); return 0; } break;
				case WM_DRAWITEM: if (w == 2) { self->DrawRetry(*reinterpret_cast<DRAWITEMSTRUCT*>(l)); return TRUE; } break;
				case WM_SIZE: self->LayoutChildren(); return 0;
				case WM_MOUSEMOVE: {
					TRACKMOUSEEVENT track{ sizeof(track), TME_LEAVE, window, 0 }; ::TrackMouseEvent(&track);
					if (self->host.interactionChanged) self->host.interactionChanged(); break;
				}
				case WM_MOUSELEAVE: case WM_SETFOCUS: case WM_KILLFOCUS:
					if (self->host.interactionChanged) self->host.interactionChanged(); break;
				case WM_CTLCOLORSTATIC: {
					const auto dc = reinterpret_cast<HDC>(w); ::SetTextColor(dc, self->palette.primaryText.ToColorRef());
					::SetBkColor(dc, self->Surface()); ::SetDCBrushColor(dc, self->Surface());
					return reinterpret_cast<LRESULT>(::GetStockObject(DC_BRUSH));
				}
				case WM_ERASEBKGND: return 1;
				case WM_PAINT: {
					PAINTSTRUCT paint{}; const auto dc = ::BeginPaint(window, &paint); Fill(dc, paint.rcPaint, self->Surface()); ::EndPaint(window, &paint); return 0;
				}
				case WM_PRINTCLIENT: { RECT rect{}; ::GetClientRect(window, &rect); Fill(reinterpret_cast<HDC>(w), rect, self->Surface()); return 0; }
				case WM_NCDESTROY: self->window = nullptr; ::SetWindowLongPtrW(window, GWLP_USERDATA, 0); if (!self->closed) self->Fault(); break;
				}
			} catch (...) { self->Fault(); }
			return ::DefWindowProcW(window, message, w, l);
		}
	};

	std::wstring extensionId;
	std::map<std::wstring, std::shared_ptr<Slot>, std::less<>> slots;
	SenpDeclaredViewActivation request;
	std::optional<senp::ContributionOwnerIdentity> bindingOwner;
	Activation activation{ Activation::Dormant };
	bool closed{}, failed{}, projecting{};
	bool Project() noexcept
	{
		if (closed || failed || projecting) return false;
		projecting = true;
		for (const auto& [id, slot] : slots) {
			if (slot->mounted && !slot->Project(activation, bindingOwner.has_value())) { failed = true; break; }
		}
		projecting = false; return !failed;
	}
	void Close() noexcept
	{
		if (closed) return;
		closed = true; request = {}; bindingOwner.reset(); providers.clear();
		for (const auto& [id, slot] : slots) slot->Close();
	}
	//! Swaps in the bound generation's providers (for title actions), releasing
	//! whatever the previous generation held.
	void SwapProviders(std::map<std::wstring, std::shared_ptr<tree::SenpTreeProvider>, std::less<>>& other) noexcept
	{ providers.swap(other); }
	void ClearProviders() noexcept { providers.clear(); }
	bool ExecuteProviderCommand(const std::wstring& viewId, const std::wstring& commandId) const
	{
		const auto provider = providers.find(viewId);
		return provider != providers.end() && provider->second->ExecuteViewCommand(commandId);
	}

private:
	//! The bound generation's providers, for title actions. Empty while unbound.
	std::map<std::wstring, std::shared_ptr<tree::SenpTreeProvider>, std::less<>> providers;
};

class CSenpDeclaredTreeViews::Body final : public viewcontainer::ISenpViewBody {
public:
	explicit Body(std::shared_ptr<Impl::Slot> slot) noexcept : m_slot(std::move(slot)) {}
	~Body() override { Close(); }
	HWND Window() const noexcept override { return m_slot->window; }
	void Layout(const RECT& bounds, unsigned int dpi) noexcept override
	{
		if (!m_slot->Usable()) return;
		if (m_slot->dpi != dpi) { m_slot->dpi = dpi; if (!m_slot->Metrics()) { m_slot->Fault(); return; } }
		if (!::SetWindowPos(Window(), nullptr, bounds.left, bounds.top, std::max(0L, bounds.right - bounds.left),
			std::max(0L, bounds.bottom - bounds.top), SWP_NOACTIVATE | SWP_NOZORDER)) m_slot->Fault();
		m_slot->LayoutChildren();
	}
	void SetVisible(bool visible) noexcept override
	{
		if (!m_slot->Usable()) return;
		m_slot->visible = visible;
		if (m_slot->current) m_slot->current->SetVisible(visible);
		::ShowWindow(Window(), visible ? SW_SHOWNA : SW_HIDE);
		if (visible) m_slot->Request(false);
	}
	void SetPalette(const theme::ThemePalette& palette, layout::EViewContainerLocation location) noexcept override
	{
		if (!m_slot->Usable()) return;
		m_slot->palette = palette; m_slot->location = location;
		if (m_slot->current) m_slot->current->SetPalette(palette, location);
		::RedrawWindow(Window(), nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
	}
	bool Focus() noexcept override
	{
		if (!m_slot->Usable() || !m_slot->visible) return false;
		if (m_slot->current) return m_slot->current->Focus();
		const auto target = CanRetry(m_slot->rendered) ? m_slot->retry : Window();
		::SetFocus(target); return ::GetFocus() == target;
	}
	bool PreTranslate(MSG& message) noexcept override
	{ return m_slot->current && m_slot->current->PreTranslate(message); }
	void Close() noexcept override { m_slot->Close(); }
private:
	std::shared_ptr<Impl::Slot> m_slot;
};

class CSenpDeclaredTreeViews::Publication final : public ISenpDeclaredTreePublication {
public:
	Publication(std::shared_ptr<Impl> state, senp::ContributionOwnerIdentity owner)
		: m_state(state), m_owner(std::move(owner)), m_expected(state->bindingOwner), m_commitOwner(m_owner) {}
	~Publication() override { Close(); }
	bool Prepare(std::vector<SenpOwnerBoundTree> trees)
	{
		const auto state = m_state.lock();
		if (!state || trees.size() != state->slots.size()) return false;
		for (const auto& tree : trees) {
			const auto slot = state->slots.find(tree.ViewId());
			if (slot == state->slots.end() || !slot->second->Usable() || !tree.Provider()
				|| tree.Provider()->ViewId() != tree.ViewId() || m_bodies.contains(slot->first)) return false;
			const std::weak_ptr<Impl::Slot> weak = slot->second;
			auto body = tree::CSenpTreeView::Create({ { slot->second->window,
				[weak] { if (const auto value = weak.lock(); value && value->host.interactionChanged) value->host.interactionChanged(); },
				[weak] { if (const auto value = weak.lock()) value->Fault(); } }, tree.Provider(), slot->second->title });
			if (!body) return false;
			m_bodies.emplace(slot->first, std::move(body));
			m_providers.emplace(slot->first, tree.Provider());
		}
		return CanCommit();
	}
	bool CanCommit() const noexcept override
	{
		const auto state = m_state.lock();
		return !m_closed && !m_committed && state && !state->closed && !state->failed
			&& state->bindingOwner == m_expected && m_bodies.size() == state->slots.size()
			&& std::ranges::all_of(state->slots, [](const auto& entry) { return entry.second->Usable() && !entry.second->pending; });
	}
	bool Commit() noexcept override
	{
		if (!CanCommit()) return false;
		const auto state = m_state.lock();
		state->bindingOwner = std::move(m_commitOwner);
		for (auto& [id, body] : m_bodies) state->slots.find(id)->second->pending = std::move(body);
		state->SwapProviders(m_providers);
		m_committed = true; return true;
	}
	bool Pump() noexcept override
	{
		const auto state = m_state.lock();
		return m_committed && !m_closed && state && state->bindingOwner == m_owner && state->Project();
	}
	void Close() noexcept override
	{
		if (m_closed) return;
		m_closed = true;
		const auto state = m_state.lock();
		if (m_committed && state && state->bindingOwner == m_owner) { state->bindingOwner.reset(); state->ClearProviders(); }
		m_bodies.clear(); m_providers.clear();
	}
private:
	std::weak_ptr<Impl> m_state;
	senp::ContributionOwnerIdentity m_owner;
	std::optional<senp::ContributionOwnerIdentity> m_expected, m_commitOwner;
	std::map<std::wstring, std::unique_ptr<tree::CSenpTreeView>, std::less<>> m_bodies;
	// After Commit this holds the predecessor's providers, released with it.
	std::map<std::wstring, std::shared_ptr<tree::SenpTreeProvider>, std::less<>> m_providers;
	bool m_committed{}, m_closed{};
};

CSenpDeclaredTreeViews::CSenpDeclaredTreeViews(std::shared_ptr<Impl> impl) noexcept : m_impl(std::move(impl)) {}
CSenpDeclaredTreeViews::~CSenpDeclaredTreeViews() { Close(); }
std::shared_ptr<CSenpDeclaredTreeViews> CSenpDeclaredTreeViews::Create(std::wstring extensionId,
	std::vector<layout::WorkbenchViewDescriptor> views, SenpDeclaredViewActivation requestActivation) noexcept
{
	try {
		if (extensionId.empty() || extensionId.size() > 256 || views.empty() || views.size() > 64 || !requestActivation) return {};
		auto state = std::make_shared<Impl>(); state->extensionId = std::move(extensionId); state->request = std::move(requestActivation);
		for (const auto& view : views) {
			const auto id = commands::json::ToWideStrict(view.id), title = commands::json::ToWideStrict(view.title);
			if (!id || id->empty() || id->size() > 256 || !title || title->empty() || title->size() > 1024 || view.provider != "senp.tree") return {};
			auto slot = std::make_shared<Impl::Slot>(); slot->cohort = state; slot->id = *id; slot->title = *title;
			if (!state->slots.emplace(*id, std::move(slot)).second) return {};
		}
		return std::shared_ptr<CSenpDeclaredTreeViews>(new CSenpDeclaredTreeViews(std::move(state)));
	} catch (const std::exception&) {
		// Only std container/allocation work above can throw; no callback runs here.
		return {};
	}
}
std::unique_ptr<viewcontainer::ISenpViewBody> CSenpDeclaredTreeViews::CreateBody(
	std::wstring_view viewId, viewcontainer::SenpViewBodyHost host) noexcept
{
	try {
		if (!IsUsable()) return {};
		const auto slot = m_impl->slots.find(viewId);
		if (slot == m_impl->slots.end() || slot->second->mounted) return {};
		if (!slot->second->Initialize(std::move(host))) { slot->second->Close(); m_impl->failed = true; return {}; }
		return std::make_unique<Body>(slot->second);
	} catch (...) { m_impl->failed = true; return {}; }
}
std::unique_ptr<ISenpDeclaredTreePublication> CSenpDeclaredTreeViews::PrepareBinding(
	const senp::ContributionOwnerIdentity& owner, std::vector<SenpOwnerBoundTree> trees) noexcept
{
	try {
		if (!IsUsable() || owner.extensionId != m_impl->extensionId || owner.generation <= 0
			|| owner.workspaceRevision <= 0 || owner.accountGeneration <= 0
			|| (m_impl->bindingOwner && owner.generation <= m_impl->bindingOwner->generation)) return {};
		auto publication = std::make_unique<Publication>(m_impl, owner);
		return publication->Prepare(std::move(trees)) ? std::unique_ptr<ISenpDeclaredTreePublication>(std::move(publication)) : nullptr;
	} catch (...) { return {}; }
}
bool CSenpDeclaredTreeViews::Pump(SenpExtensionActivationState state) noexcept
{ m_impl->activation = state; return m_impl->Project(); }
bool CSenpDeclaredTreeViews::ExecuteTitleCommand(std::string_view viewId, std::string_view commandId) noexcept
{
	try {
		if (!IsUsable() || !m_impl->bindingOwner) return false;
		const auto view = commands::json::ToWideStrict(std::string(viewId));
		const auto command = commands::json::ToWideStrict(std::string(commandId));
		if (!view || !command) return false;
		return m_impl->ExecuteProviderCommand(*view, *command);
	} catch (const std::exception&) {
		// commands::json::ToWideStrict and the map/string lookups above only ever
		// throw std exceptions; ExecuteViewCommand is a same-process provider call.
		return false;
	}
}
bool CSenpDeclaredTreeViews::IsUsable() const noexcept { return !m_impl->closed && !m_impl->failed; }
void CSenpDeclaredTreeViews::Close() noexcept { m_impl->Close(); }

} // namespace workbench

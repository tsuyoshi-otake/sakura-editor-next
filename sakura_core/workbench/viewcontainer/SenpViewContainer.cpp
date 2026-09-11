/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#include "StdAfx.h"
#include "workbench/viewcontainer/SenpViewContainer.h"
#include "workbench/viewcontainer/ViewPaneChrome.h"
#include "workbench/controls/COverlayScrollbar.h"
#include "workbench/IconMetrics.h"
#include "accessibility/CustomUiAutomationProvider.h"
#include <CommCtrl.h>
#include <windowsx.h>
#include <algorithm>
#include <array>
#include <limits>
#include <unordered_set>

namespace workbench::viewcontainer {
namespace {
constexpr wchar_t kClass[] = L"SakuraSenpViewSurface";
constexpr UINT_PTR kControlSubclass = 0x5e70;
constexpr UINT kRefreshInteraction = WM_APP + 0x5e7;
constexpr std::size_t kMaximumActions = 8;
constexpr std::size_t kMaximumContainers = 16;
enum class NodeKind { Container, Pane, Sash };

int Dip(int value, unsigned int dpi) noexcept { return icons::ScaleDip(value, dpi); }
bool Contains(HWND ancestor, HWND target) noexcept
{
	return ancestor && target && (ancestor == target || ::IsChild(ancestor, target));
}
void Fill(HDC dc, const RECT& rect, COLORREF color) noexcept
{
	const auto previous = ::SetDCBrushColor(dc, color);
	::FillRect(dc, &rect, static_cast<HBRUSH>(::GetStockObject(DC_BRUSH)));
	::SetDCBrushColor(dc, previous);
}
bool Parent(HWND window, HWND target) noexcept
{
	if (!window || !target || !::IsWindow(window) || !::IsWindow(target)) return false;
	if (::GetParent(window) == target) return true;
	::SetLastError(0);
	const bool changed = ::SetParent(window, target) != nullptr || ::GetLastError() == 0;
	return changed && ::GetParent(window) == target;
}
bool ValidActions(std::span<const SenpViewTitleAction> actions) noexcept
{
	if (actions.size() > kMaximumActions) return false;
	for (std::size_t i = 0; i < actions.size(); ++i) {
		const auto& action = actions[i];
		if (!layout::WorkbenchContributionRegistry::IsValidStableId(action.commandId)
			|| action.title.empty() || action.title.size() > 1024 || action.icon.empty() || action.icon.size() > 160
			|| action.title.find(L'\0') != std::wstring::npos
			|| std::ranges::any_of(action.icon, [](wchar_t c) { return !((c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9') || c == L'-'); })) return false;
		for (std::size_t j = 0; j < i; ++j) if (actions[j].commandId == action.commandId) return false;
	}
	return true;
}
std::wstring Wide(std::string_view text)
{
	if (text.empty()) return {};
	const int size = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
	if (size <= 0) throw std::invalid_argument("invalid View title UTF-8");
	std::wstring result(size, L'\0');
	::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), size);
	return result;
}
} // namespace

struct CSenpViewContainers::Impl {
	struct Pane;
	struct Container;
	struct Node {
		Impl* owner{};
		NodeKind kind{};
		HWND window{};
		Pane* pane{};
		Container* container{};
	};
	struct Control final : accessibility::ICustomUiAutomationHost {
		Pane* pane{};
		HWND window{};
		std::size_t index{}; // 0 is the header, 1..8 are title actions.
		bool hovered{};
		std::shared_ptr<accessibility::CustomUiAutomationLifetime> lifetime = std::make_shared<accessibility::CustomUiAutomationLifetime>();
		HWND AccessibilityWindow() const noexcept override { return window; }
		std::shared_ptr<accessibility::CustomUiAutomationLifetime> AccessibilityLifetime() const noexcept override { return lifetime; }
		std::wstring AccessibilityName() const override;
		std::wstring AccessibilityAutomationId() const override;
		CONTROLTYPEID AccessibilityControlType() const noexcept override { return UIA_ButtonControlTypeId; }
		int AccessibilityChildCount(int) const noexcept override { return 0; }
		int AccessibilityChildAt(int, int) const noexcept override { return -2; }
		int AccessibilityParent(int) const noexcept override { return -2; }
		accessibility::CustomUiAutomationNode AccessibilityNode(int) const override { return AccessibilityRootNode(); }
		accessibility::CustomUiAutomationNode AccessibilityRootNode() const override;
		int AccessibilityFocusedNode() const noexcept override { return -1; }
		bool AccessibilityInvoke(int) noexcept override;
		void AccessibilitySetFocus(int) noexcept override;
		bool AccessibilityExpandCollapse(int, bool expanded) noexcept override;
	};
	struct Container {
		Node root;
		layout::WorkbenchViewContainerDescriptor descriptor;
		std::optional<ViewContainerPageHost> host;
		ViewContainerNativeHandle nativeParent{};
		controls::COverlayScrollbar scrollbar;
		unsigned int dpi{ 96 };
		RECT content{};
		int scroll{};
		bool pageIssued{}, closed{}, active{}, visible{};
	};
	struct Pane {
		Impl* owner{};
		Node root, sash;
		SenpNativeViewDefinition definition;
		std::wstring title;
		std::unique_ptr<ISenpViewBody> body;
		std::array<Control, kMaximumActions + 1> controls;
		HWND tooltip{};
		Container* container{};
		Pane* nextResizable{};
		ViewPaneBounds bounds{};
		std::int32_t order{};
		bool visible{ true }, collapsed{}, bodyVisible{}, mountedVisible{}, separator{};
		theme::CThemeFont font;
		int dragOriginY{}, dragFirst{}, dragSecond{};
		bool dragging{}, interactionPosted{};
		layout::EViewContainerLocation bodyLocation{ layout::EViewContainerLocation::Sidebar };
	};
	SenpViewContainerOptions options;
	std::vector<std::unique_ptr<Container>> containers;
	std::vector<std::unique_ptr<Pane>> panes;
	theme::ThemePalette palette{ theme::CThemeService::PaletteFor(theme::ThemeMode::Dark) };
	std::uint64_t layoutGeneration{}, layoutRevision{};
	bool closed{}, failed{}, applying{}, focusDispatch{};

	explicit Impl(SenpViewContainerOptions value) : options(std::move(value)) {}
	~Impl() { Close(); }
	bool Usable() const noexcept { return !closed && !failed; }
	layout::EViewContainerLocation Location(const Container& container) const noexcept
	{ return container.host ? container.host->location : container.descriptor.location; }
	const theme::ThemeColor& Surface(const Container& container) const noexcept
	{ return Location(container) == layout::EViewContainerLocation::Panel ? palette.bottomPanel : palette.sideBar; }
	Container* FindContainer(std::string_view id) const noexcept
	{
		for (const auto& container : containers) if (container->descriptor.id == id) return container.get();
		return nullptr;
	}
	Pane* FindPane(std::string_view id) const noexcept
	{
		for (const auto& pane : panes) if (pane->definition.descriptor.id == id) return pane.get();
		return nullptr;
	}
	void Fault() noexcept
	{
		failed = true;
		// The cohort retains cleanup ownership until explicit Close/destruction.
		// IsUsable exposes this terminal projection failure to its publication owner.
		for (const auto& container : containers) if (container->root.window) ::ShowWindow(container->root.window, SW_HIDE);
	}
	bool CreateNode(Node& node, NodeKind kind, HWND parent, Pane* pane, Container* container)
	{
		node.owner = this; node.kind = kind; node.pane = pane; node.container = container;
		node.window = ::CreateWindowExW(WS_EX_CONTROLPARENT, kClass, L"", WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
			0, 0, 0, 0, parent, nullptr, ::GetModuleHandleW(nullptr), &node);
		return node.window != nullptr;
	}
	bool Initialize()
	{
		if (!::IsWindow(options.parkingParent) || options.owner.generation == 0
			|| options.owner.generation > INT64_MAX || !layout::WorkbenchContributionRegistry::IsValidStableId(options.owner.ownerId)
			|| options.containers.empty() || options.containers.size() > kMaximumContainers
			|| options.views.empty() || options.views.size() > kMaximumViewPanes || !options.requestFocus) return false;
		std::unordered_set<std::string_view> containerIds, viewIds;
		for (const auto& descriptor : options.containers) {
			if (!layout::WorkbenchContributionRegistry::IsValidViewContainerDescriptor(descriptor)
				|| !containerIds.emplace(descriptor.id).second) return false;
		}
		for (const auto& view : options.views) {
			if (!view.createBody || !layout::WorkbenchContributionRegistry::IsValidStableId(view.descriptor.id)
				|| !containerIds.contains(view.descriptor.containerId) || !viewIds.emplace(view.descriptor.id).second
				|| view.descriptor.title.empty() || view.descriptor.title.size() > 1024
				|| view.descriptor.title.find('\0') != std::string::npos || !ValidActions(view.actions)
				|| (!view.actions.empty() && !options.execute) || view.minimumBodyDip < 1 || view.minimumBodyDip > 65535
				|| view.preferredBodyDip < view.minimumBodyDip || view.preferredBodyDip > 65535) return false;
			(void)Wide(view.descriptor.title);
		}
		WNDCLASSEXW cls{ sizeof(cls) };
		cls.lpfnWndProc = Procedure; cls.hInstance = ::GetModuleHandleW(nullptr);
		cls.hCursor = ::LoadCursorW(nullptr, IDC_ARROW); cls.lpszClassName = kClass;
		if (!::RegisterClassExW(&cls) && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
		containers.reserve(options.containers.size()); panes.reserve(options.views.size());
		for (const auto& descriptor : options.containers) {
			auto value = std::make_unique<Container>();
			value->descriptor = descriptor;
			auto& container = *value;
			containers.push_back(std::move(value));
			if (!CreateNode(container.root, NodeKind::Container, options.parkingParent, nullptr, &container)) return false;
			if (!container.scrollbar.Create(container.root.window, container.root.window,
				[this, &container](int offset) { if (Usable()) { container.scroll = offset; Layout(container); } },
				controls::OverlayScrollbarSource::ExplicitModel)) return false;
		}
		for (auto& definition : options.views) {
			auto value = std::make_unique<Pane>();
			auto& pane = *value;
			pane.owner = this; pane.title = Wide(definition.descriptor.title);
			pane.order = definition.descriptor.order; pane.collapsed = definition.initiallyCollapsed;
			pane.container = FindContainer(definition.descriptor.containerId);
			pane.definition = std::move(definition);
			panes.push_back(std::move(value));
			if (!CreateNode(pane.root, NodeKind::Pane, pane.container->root.window, &pane, nullptr)
				|| !CreateNode(pane.sash, NodeKind::Sash, pane.container->root.window, &pane, nullptr)) return false;
			for (std::size_t i = 0; i < pane.controls.size(); ++i) {
				auto& control = pane.controls[i]; control.pane = &pane; control.index = i;
				control.window = ::CreateWindowExW(0, L"BUTTON", i == 0 ? pane.title.c_str() : L"",
					WS_CHILD | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 0, 0, pane.root.window,
					reinterpret_cast<HMENU>(static_cast<UINT_PTR>(i + 1)), ::GetModuleHandleW(nullptr), nullptr);
				if (!control.window || !::SetWindowSubclass(control.window, ControlProcedure, kControlSubclass,
					reinterpret_cast<DWORD_PTR>(&control))) return false;
			}
			pane.tooltip = ::CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP,
				CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, pane.root.window, nullptr, ::GetModuleHandleW(nullptr), nullptr);
			if (!pane.tooltip) return false;
			for (std::size_t i = 1; i < pane.controls.size(); ++i) {
				TOOLINFOW info{ sizeof(info) }; info.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
				info.hwnd = pane.root.window; info.uId = reinterpret_cast<UINT_PTR>(pane.controls[i].window);
				info.lpszText = const_cast<LPWSTR>(L"");
				if (!::SendMessageW(pane.tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&info))) return false;
			}
			pane.body = pane.definition.createBody({ pane.root.window, [this, &pane] { ScheduleInteraction(pane); }, [this]() noexcept { Fault(); } });
			if (!pane.body || !::IsWindow(pane.body->Window()) || ::GetParent(pane.body->Window()) != pane.root.window) return false;
			pane.body->SetVisible(false); pane.bodyLocation = Location(*pane.container);
			pane.body->SetPalette(palette, pane.bodyLocation);
			if (!pane.font.Recreate(theme::ThemeFontKind::Chrome, 96)) return false;
			UpdateActions(pane);
		}
		options.views.clear(); // Native panes are the sole body/factory owners now.
		return true;
	}
	void UpdateActions(Pane& pane)
	{
		for (std::size_t i = 1; i < pane.controls.size(); ++i) {
			auto& control = pane.controls[i];
			const bool present = i <= pane.definition.actions.size();
			const wchar_t* title = present ? pane.definition.actions[i - 1].title.c_str() : L"";
			::SetWindowTextW(control.window, title);
			::EnableWindow(control.window, present && pane.definition.actions[i - 1].enabled);
			TOOLINFOW info{ sizeof(info) }; info.hwnd = pane.root.window;
			info.uId = reinterpret_cast<UINT_PTR>(control.window); info.lpszText = const_cast<LPWSTR>(title);
			::SendMessageW(pane.tooltip, TTM_UPDATETIPTEXTW, 0, reinterpret_cast<LPARAM>(&info));
		}
	}
	void Close() noexcept
	{
		if (closed) return;
		closed = true;
		options.requestFocus = {}; options.execute = {};
		for (auto& pane : panes) {
			for (auto& control : pane->controls) control.lifetime->Invalidate();
			if (pane->sash.window && ::GetCapture() == pane->sash.window) ::ReleaseCapture();
			if (pane->body) { pane->body->SetVisible(false); pane->body->Close(); pane->body.reset(); }
			if (pane->tooltip) { ::DestroyWindow(pane->tooltip); pane->tooltip = nullptr; }
			if (pane->sash.window) ::DestroyWindow(pane->sash.window);
			if (pane->root.window) ::DestroyWindow(pane->root.window);
			pane->font.Reset();
		}
		for (auto& container : containers) {
			container->closed = true; container->scrollbar.Destroy();
			if (container->root.window) ::DestroyWindow(container->root.window);
		}
	}
	// `publish` is false for a layout the workbench drives (attach, projected
	// bounds, visibility, palette, committed placement): those run several times
	// for one page switch and the frame commit publishes the finished frame.
	// A gesture inside the container owns its own frame and publishes it here.
	void Layout(Container& container, bool publish = true);
	void LayoutPane(Pane& pane, int width, int height);
	void ScheduleInteraction(Pane& pane) noexcept
	{
		if (!Usable() || pane.interactionPosted || !pane.root.window) return;
		pane.interactionPosted = ::PostMessageW(pane.root.window, kRefreshInteraction, 0, 0) != FALSE;
		if (!pane.interactionPosted) Fault();
	}
	bool RequestFocus(Pane& pane) noexcept;
	bool Focus(Pane& pane) noexcept;
	bool Collapse(Pane& pane, bool value) noexcept;
	void MoveHeaderFocus(Pane& pane, int direction) noexcept;
	void BeginSash(Pane& pane) noexcept;
	void DragSash(Pane& pane) noexcept;
	void DrawControl(const DRAWITEMSTRUCT& draw, Pane& pane) noexcept;
	bool PreTranslate(Container& container, MSG& message) noexcept;
	LRESULT Message(Node& node, UINT message, WPARAM wParam, LPARAM lParam);
	static LRESULT CALLBACK Procedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) noexcept;
	static LRESULT CALLBACK ControlProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR data) noexcept;
};

std::wstring CSenpViewContainers::Impl::Control::AccessibilityName() const { return pane->title; }
std::wstring CSenpViewContainers::Impl::Control::AccessibilityAutomationId() const { return Wide(pane->definition.descriptor.id); }
accessibility::CustomUiAutomationNode CSenpViewContainers::Impl::Control::AccessibilityRootNode() const
{
	RECT bounds{}; ::GetClientRect(window, &bounds);
	return { -1, pane->title, AccessibilityAutomationId(), UIA_ButtonControlTypeId, bounds,
		pane->owner->Usable() && pane->mountedVisible, ::GetFocus() == window, true, !pane->collapsed };
}
bool CSenpViewContainers::Impl::Control::AccessibilityInvoke(int) noexcept
{
	return pane->owner->RequestFocus(*pane) && pane->owner->Collapse(*pane, !pane->collapsed);
}
void CSenpViewContainers::Impl::Control::AccessibilitySetFocus(int) noexcept
{
	if (pane->owner->RequestFocus(*pane)) ::SetFocus(window);
}
bool CSenpViewContainers::Impl::Control::AccessibilityExpandCollapse(int, bool expanded) noexcept
{
	return pane->owner->Usable() && pane->owner->Collapse(*pane, !expanded);
}

void CSenpViewContainers::Impl::Layout(Container& container, bool publish)
{
	if (!Usable() || container.closed || !container.root.window) return;
	const int width = std::max(0L, container.content.right - container.content.left);
	const int height = std::max(0L, container.content.bottom - container.content.top);
	std::array<Pane*, kMaximumViewPanes> ordered{};
	std::array<ViewPaneSize, kMaximumViewPanes> sizes{};
	std::size_t count{};
	for (auto& pane : panes) if (pane->container == &container) {
		pane->nextResizable = nullptr;
		if (pane->visible) ordered[count++] = pane.get();
		else {
			pane->mountedVisible = false;
			::ShowWindow(pane->root.window, SW_HIDE); ::ShowWindow(pane->sash.window, SW_HIDE);
			if (pane->bodyVisible) { pane->bodyVisible = false; pane->body->SetVisible(false); }
		}
	}
	std::sort(ordered.begin(), ordered.begin() + count, [](const auto* left, const auto* right) {
		return left->order != right->order ? left->order < right->order : left->definition.descriptor.id < right->definition.descriptor.id;
	});
	for (std::size_t i = 0; i < count; ++i) sizes[i] = { ordered[i]->collapsed,
		Dip(ordered[i]->definition.preferredBodyDip, container.dpi), Dip(ordered[i]->definition.minimumBodyDip, container.dpi) };
	const auto stack = BuildViewPaneStackLayout({ sizes.data(), count }, height, Dip(kViewPaneHeaderDip, container.dpi));
	if (!stack.valid) { Fault(); return; }
	container.scroll = std::clamp(container.scroll, 0, std::max(0, stack.contentHeight - height));
	const bool visible = container.visible && container.host.has_value();
	for (std::size_t i = 0; i < count; ++i) {
		auto& pane = *ordered[i]; pane.bounds = stack.panes[i]; pane.separator = i != 0;
		if (pane.font.Dpi() != container.dpi && !pane.font.Recreate(theme::ThemeFontKind::Chrome, container.dpi)) { Fault(); return; }
		if (!::SetWindowPos(pane.root.window, nullptr, container.content.left,
			container.content.top + pane.bounds.top - container.scroll, width, pane.bounds.bottom - pane.bounds.top,
			SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS)) { Fault(); return; }
		pane.mountedVisible = visible;
		LayoutPane(pane, width, pane.bounds.bottom - pane.bounds.top);
		::ShowWindow(pane.root.window, visible ? SW_SHOWNOACTIVATE : SW_HIDE);
		// A sash spans the next header boundary without consuming layout space.
		// Decide its placement once. Hiding it and showing it again within the
		// same pass publishes an intermediate frame on every layout.
		bool sashPlaced = false;
		if (!pane.collapsed) {
			for (std::size_t next = i + 1; next < count; ++next) if (!ordered[next]->collapsed) {
				pane.nextResizable = ordered[next];
				break;
			}
			if (pane.nextResizable && visible) {
				const int sash = Dip(kViewPaneSashDip, container.dpi);
				const int y = container.content.top + pane.bounds.bottom - container.scroll - sash / 2;
				if (y >= container.content.top && y + sash <= container.content.bottom) {
					::SetWindowPos(pane.sash.window, HWND_TOP, container.content.left, y, width, sash,
						SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOCOPYBITS);
					sashPlaced = true;
				}
			}
		}
		if (!sashPlaced) ::ShowWindow(pane.sash.window, SW_HIDE);
	}
	container.scrollbar.SetDpi(container.dpi);
	container.scrollbar.SetBounds(container.content);
	container.scrollbar.SetColors(controls::ResolveOverlayScrollbarColors(palette, Surface(container)));
	container.scrollbar.SetScrollModel({ stack.contentHeight, height, container.scroll });
	container.scrollbar.Update();
	::RedrawWindow(container.root.window, nullptr, nullptr,
		RDW_INVALIDATE | RDW_ALLCHILDREN | (publish ? RDW_UPDATENOW : 0u));
}

void CSenpViewContainers::Impl::LayoutPane(Pane& pane, int width, int height)
{
	if (!Usable() || !pane.root.window || !pane.body) return;
	if (!::IsWindow(pane.body->Window()) || ::GetParent(pane.body->Window()) != pane.root.window) { Fault(); return; }
	if (pane.bodyLocation != Location(*pane.container)) {
		pane.bodyLocation = Location(*pane.container); pane.body->SetPalette(palette, pane.bodyLocation);
	}
	const auto dpi = pane.container->dpi;
	const int header = Dip(kViewPaneHeaderDip, dpi);
	const int actionSide = Dip(kViewPaneIconDip + 2 * kViewPaneActionPaddingDip, dpi);
	const int gap = Dip(kViewPaneActionGapDip, dpi);
	POINT cursor{}; ::GetCursorPos(&cursor);
	const bool hovered = Contains(pane.root.window, ::WindowFromPoint(cursor));
	const bool actionsVisible = !pane.collapsed && (hovered || Contains(pane.root.window, ::GetFocus()));
	int actionLeft = width - Dip(kViewPaneActionTrailingDip, dpi);
	for (std::size_t i = pane.controls.size() - 1; i > 0; --i) {
		auto& control = pane.controls[i];
		const bool show = actionsVisible && i <= pane.definition.actions.size()
			&& actionLeft >= actionSide + Dip(kViewPaneIconDip + 2 * kViewPaneHeaderIconInsetDip, dpi);
		if (show) {
			actionLeft -= actionSide;
			::SetWindowPos(control.window, nullptr, actionLeft, (header - actionSide) / 2, actionSide, actionSide,
				SWP_NOACTIVATE | SWP_NOZORDER | SWP_SHOWWINDOW);
			actionLeft -= gap;
		} else ::ShowWindow(control.window, SW_HIDE);
	}
	const int headerWidth = actionsVisible && !pane.definition.actions.empty() ? std::max(0, actionLeft) : width;
	::SetWindowPos(pane.controls[0].window, nullptr, 0, 0, headerWidth, header, SWP_NOACTIVATE | SWP_NOZORDER | SWP_SHOWWINDOW);
	const bool bodyVisible = pane.mountedVisible && !pane.collapsed && height > header;
	if (pane.bodyVisible != bodyVisible) { pane.bodyVisible = bodyVisible; pane.body->SetVisible(bodyVisible); }
	pane.body->Layout({ 0, header, width, std::max(header, height) }, dpi);
}

bool CSenpViewContainers::Impl::RequestFocus(Pane& pane) noexcept
{
	if (!Usable() || !pane.mountedVisible) return false;
	if (applying) return true; // SetParent preserves focus without reentering model publication.
	if (focusDispatch) return true;
	focusDispatch = true;
	bool accepted{};
	try { accepted = options.requestFocus(pane.definition.descriptor.id); }
	catch (...) { Fault(); }
	focusDispatch = false;
	return accepted && Usable();
}

bool CSenpViewContainers::Impl::Focus(Pane& pane) noexcept
{
	if (!Usable() || !pane.mountedVisible || !pane.visible || !pane.container->active) return false;
	if (pane.container->content.right <= pane.container->content.left || pane.container->content.bottom <= pane.container->content.top) return false;
	if (pane.collapsed) {
		::SetFocus(pane.controls[0].window);
		return ::GetFocus() == pane.controls[0].window;
	}
	return pane.body && pane.body->Focus();
}

bool CSenpViewContainers::Impl::Collapse(Pane& pane, bool value) noexcept
{
	if (!Usable() || pane.container->closed) return false;
	if (pane.collapsed == value) return true;
	if (value && pane.body && Contains(pane.body->Window(), ::GetFocus())) {
		::SetFocus(pane.controls[0].window);
		if (::GetFocus() != pane.controls[0].window) return false;
	}
	pane.collapsed = value;
	Layout(*pane.container);
	accessibility::RaiseExpandedChanged(pane.controls[0], -1, value, !value);
	return Usable();
}

void CSenpViewContainers::Impl::MoveHeaderFocus(Pane& pane, int direction) noexcept
{
	std::array<Pane*, kMaximumViewPanes> ordered{};
	std::size_t count{};
	for (const auto& candidate : panes) if (candidate->container == pane.container && candidate->visible) ordered[count++] = candidate.get();
	std::sort(ordered.begin(), ordered.begin() + count, [](const auto* a, const auto* b) { return a->bounds.top < b->bounds.top; });
	for (std::size_t i = 0; i < count; ++i) if (ordered[i] == &pane) {
		const auto next = static_cast<std::ptrdiff_t>(i) + direction;
		if (next < 0 || next >= static_cast<std::ptrdiff_t>(count)) return;
		auto& target = *ordered[static_cast<std::size_t>(next)];
		if (!RequestFocus(target)) return;
		const int height = pane.container->content.bottom - pane.container->content.top;
		pane.container->scroll = std::clamp(pane.container->scroll,
			std::min(target.bounds.top, std::max(0, target.bounds.headerBottom - height)), target.bounds.top);
		Layout(*pane.container);
		::SetFocus(target.controls[0].window);
		return;
	}
}

void CSenpViewContainers::Impl::BeginSash(Pane& pane) noexcept
{
	if (!Usable() || !pane.nextResizable || pane.collapsed) return;
	POINT cursor{}; ::GetCursorPos(&cursor);
	// Freeze the actual distributed sizes before resizing one neighboring pair.
	for (auto& other : panes) if (other->container == pane.container && !other->collapsed && other->visible)
		other->definition.preferredBodyDip = std::max(other->definition.minimumBodyDip,
			::MulDiv(other->bounds.bottom - other->bounds.headerBottom, 96, pane.container->dpi));
	pane.dragOriginY = cursor.y;
	pane.dragFirst = pane.definition.preferredBodyDip;
	pane.dragSecond = pane.nextResizable->definition.preferredBodyDip;
	::SetCapture(pane.sash.window); pane.dragging = ::GetCapture() == pane.sash.window;
}

void CSenpViewContainers::Impl::DragSash(Pane& pane) noexcept
{
	if (!Usable() || !pane.dragging || !pane.nextResizable) return;
	POINT cursor{}; ::GetCursorPos(&cursor);
	auto& next = *pane.nextResizable;
	const int total = pane.dragFirst + pane.dragSecond;
	const int delta = ::MulDiv(cursor.y - pane.dragOriginY, 96, pane.container->dpi);
	pane.definition.preferredBodyDip = std::clamp(pane.dragFirst + delta,
		pane.definition.minimumBodyDip, total - next.definition.minimumBodyDip);
	next.definition.preferredBodyDip = total - pane.definition.preferredBodyDip;
	Layout(*pane.container);
}

void CSenpViewContainers::Impl::DrawControl(const DRAWITEMSTRUCT& draw, Pane& pane) noexcept
{
	const auto index = draw.CtlID - 1;
	if (index >= pane.controls.size()) return;
	const auto& control = pane.controls[index];
	const bool pressed = (draw.itemState & ODS_SELECTED) != 0;
	Fill(draw.hDC, draw.rcItem, (pressed || (index != 0 && control.hovered) ? palette.raised : Surface(*pane.container)).ToColorRef());
	const auto previous = ::SelectObject(draw.hDC, pane.font.Get());
	if (index == 0) {
		PaintViewPaneHeader(draw.hDC, draw.rcItem, pane.title, pane.collapsed, pane.separator, pane.container->dpi, palette);
	} else if (index <= pane.definition.actions.size()) {
		const auto& action = pane.definition.actions[index - 1];
		const int padding = Dip(kViewPaneActionPaddingDip, pane.container->dpi);
		RECT icon = draw.rcItem; ::InflateRect(&icon, -padding, -padding);
		PaintViewPaneIcon(draw.hDC, icon, action.icon, (action.enabled ? palette.primaryText : palette.disabledText).ToColorRef());
	}
	::SelectObject(draw.hDC, previous);
	if (draw.itemState & ODS_FOCUS) {
		const auto color = ::SetDCBrushColor(draw.hDC, palette.accent.ToColorRef());
		::FrameRect(draw.hDC, &draw.rcItem, static_cast<HBRUSH>(::GetStockObject(DC_BRUSH)));
		::SetDCBrushColor(draw.hDC, color);
	}
}

bool CSenpViewContainers::Impl::PreTranslate(Container& container, MSG& message) noexcept
{
	if (!Usable() || !container.active || !Contains(container.root.window, message.hwnd)) return false;
	for (const auto& pane : panes) if (pane->container == &container && pane->bodyVisible && Contains(pane->body->Window(), message.hwnd)) {
		if (message.message == WM_KEYDOWN && !RequestFocus(*pane)) return true;
		if (pane->body->PreTranslate(message)) return true;
	}
	return message.message == WM_KEYDOWN && message.wParam == VK_TAB && ::IsDialogMessageW(container.root.window, &message);
}

LRESULT CSenpViewContainers::Impl::Message(Node& node, UINT message, WPARAM wParam, LPARAM lParam)
{
	if (message == WM_PAINT || message == WM_PRINTCLIENT) {
		PAINTSTRUCT paint{};
		const HDC dc = message == WM_PAINT ? ::BeginPaint(node.window, &paint) : reinterpret_cast<HDC>(wParam);
		RECT client{}; ::GetClientRect(node.window, &client);
		const auto* container = node.kind == NodeKind::Container ? node.container : node.pane->container;
		Fill(dc, client, node.kind == NodeKind::Sash && node.pane->dragging ? palette.accent.ToColorRef() : Surface(*container).ToColorRef());
		if (message == WM_PAINT) ::EndPaint(node.window, &paint);
		return 0;
	}
	if (message == WM_ERASEBKGND) return 1;
	if (!Usable()) return ::DefWindowProcW(node.window, message, wParam, lParam);
	if (node.kind == NodeKind::Sash) {
		auto& pane = *node.pane;
		switch (message) {
		case WM_SETCURSOR: ::SetCursor(::LoadCursorW(nullptr, IDC_SIZENS)); return TRUE;
		case WM_LBUTTONDOWN: BeginSash(pane); return 0;
		case WM_MOUSEMOVE: DragSash(pane); return 0;
		case WM_LBUTTONUP: pane.dragging = false; if (::GetCapture() == node.window) ::ReleaseCapture(); return 0;
		case WM_CAPTURECHANGED: pane.dragging = false; return 0;
		}
	}
	if (node.kind == NodeKind::Pane) {
		auto& pane = *node.pane;
		if (message == kRefreshInteraction) {
			pane.interactionPosted = false;
			if (pane.body && Contains(pane.body->Window(), ::GetFocus())) (void)RequestFocus(pane);
			LayoutPane(pane, pane.container->content.right - pane.container->content.left, pane.bounds.bottom - pane.bounds.top);
			return 0;
		}
		if (message == WM_DRAWITEM) { DrawControl(*reinterpret_cast<const DRAWITEMSTRUCT*>(lParam), pane); return TRUE; }
		if (message == WM_COMMAND && HIWORD(wParam) == BN_CLICKED) {
			const auto index = LOWORD(wParam) - 1;
			if (!RequestFocus(pane)) return 0;
			if (index == 0) (void)Collapse(pane, !pane.collapsed);
			else if (index <= pane.definition.actions.size() && pane.definition.actions[index - 1].enabled && options.execute) {
				// Copy the operand before a command callback can update title actions.
				const auto command = pane.definition.actions[index - 1].commandId;
				(void)options.execute(pane.definition.descriptor.id, command);
			}
			return 0;
		}
		if (message == WM_PARENTNOTIFY && LOWORD(wParam) == WM_LBUTTONDOWN) (void)RequestFocus(pane);
	}
	if (message == WM_MOUSEWHEEL) {
		auto* container = node.kind == NodeKind::Container ? node.container : node.pane->container;
		container->scroll = std::max(0, container->scroll - GET_WHEEL_DELTA_WPARAM(wParam) * Dip(kViewPaneRowDip * 3, container->dpi) / WHEEL_DELTA);
		Layout(*container); return 0;
	}
	return ::DefWindowProcW(node.window, message, wParam, lParam);
}

LRESULT CALLBACK CSenpViewContainers::Impl::Procedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) noexcept
{
	auto* node = reinterpret_cast<Node*>(::GetWindowLongPtrW(window, GWLP_USERDATA));
	if (message == WM_NCCREATE) {
		node = static_cast<Node*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
		node->window = window; ::SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(node));
	}
	if (!node) return ::DefWindowProcW(window, message, wParam, lParam);
	if (message == WM_NCDESTROY) {
		node->window = nullptr; ::SetWindowLongPtrW(window, GWLP_USERDATA, 0);
		if (!node->owner->closed && !(node->kind == NodeKind::Container && node->container->closed)) node->owner->Fault();
		return ::DefWindowProcW(window, message, wParam, lParam);
	}
	try { return node->owner->Message(*node, message, wParam, lParam); }
	catch (...) { node->owner->Fault(); return 0; }
}

LRESULT CALLBACK CSenpViewContainers::Impl::ControlProcedure(HWND window, UINT message, WPARAM wParam,
	LPARAM lParam, UINT_PTR, DWORD_PTR data) noexcept
{
	auto& control = *reinterpret_cast<Control*>(data);
	auto& pane = *control.pane;
	auto& owner = *pane.owner;
	if (message == WM_DESTROY && control.index == 0) {
		control.lifetime->Invalidate();
		// Release UIA's HWND/event map before this native handle can be recycled.
		(void)::UiaReturnRawElementProvider(window, 0, 0, nullptr);
	}
	if (message == WM_NCDESTROY) {
		control.lifetime->Invalidate();
		control.window = nullptr; ::RemoveWindowSubclass(window, ControlProcedure, kControlSubclass);
		if (!owner.closed) owner.Fault();
		return ::DefSubclassProc(window, message, wParam, lParam);
	}
	if (message == WM_GETOBJECT && control.index == 0)
		return owner.Usable() && control.lifetime->IsAlive() ? accessibility::HandleGetObject(control, wParam, lParam) : 0;
	// DrawControl fills the whole item. A separate erase lands a frame before
	// WM_DRAWITEM when the page is shown, so the header blinks out and back.
	if (message == WM_ERASEBKGND) return 1;
	if (!owner.Usable()) return ::DefSubclassProc(window, message, wParam, lParam);
	if (message == WM_SETFOCUS) {
		if (!owner.RequestFocus(pane) && ::IsWindow(reinterpret_cast<HWND>(wParam))) ::SetFocus(reinterpret_cast<HWND>(wParam));
		owner.ScheduleInteraction(pane);
		if (control.index == 0) accessibility::RaiseFocusChanged(control, -1);
	}
	if (message == WM_KILLFOCUS || message == WM_MOUSEMOVE || message == WM_MOUSELEAVE) {
		const bool hovered = message == WM_MOUSEMOVE;
		if (message != WM_KILLFOCUS && control.hovered != hovered) {
			control.hovered = hovered;
			if (hovered) { TRACKMOUSEEVENT track{ sizeof(track), TME_LEAVE, window, 0 }; ::TrackMouseEvent(&track); }
			::InvalidateRect(window, nullptr, FALSE);
		}
		owner.ScheduleInteraction(pane);
	}
	if (message == WM_GETDLGCODE && control.index == 0) {
		const auto* key = reinterpret_cast<const MSG*>(lParam);
		return DLGC_WANTARROWS | ((key && (key->wParam == VK_RETURN || key->wParam == VK_SPACE)) ? DLGC_WANTMESSAGE : 0);
	}
	if (message == WM_KEYDOWN && control.index == 0) {
		if ((wParam == VK_RETURN || wParam == VK_SPACE || wParam == VK_LEFT || wParam == VK_RIGHT) && !owner.RequestFocus(pane)) return 0;
		switch (wParam) {
		case VK_RETURN: case VK_SPACE: (void)owner.Collapse(pane, !pane.collapsed); return 0;
		case VK_LEFT: (void)owner.Collapse(pane, true); return 0;
		case VK_RIGHT: (void)owner.Collapse(pane, false); return 0;
		case VK_UP: owner.MoveHeaderFocus(pane, -1); return 0;
		case VK_DOWN: owner.MoveHeaderFocus(pane, 1); return 0;
		}
	}
	// The default BUTTON's Space-up handler must not toggle a second time.
	if (message == WM_KEYUP && control.index == 0 && (wParam == VK_SPACE || wParam == VK_RETURN)) return 0;
	return ::DefSubclassProc(window, message, wParam, lParam);
}

class CSenpViewContainers::Page final : public IViewContainerPage, public IViewContainerPageProjection {
public:
	Page(std::shared_ptr<CSenpViewContainers> owner, Impl::Container& container) : m_owner(std::move(owner)), m_container(container) {}
	~Page() override { if (Close() == EViewContainerPageCloseStatus::Failed) m_owner->Close(); }
	std::string_view ContainerId() const noexcept override { return m_container.descriptor.id; }
	ViewContainerFocusCaptureResult CaptureFocusToken() noexcept override
	{
		if (!m_container.host || !Contains(m_container.root.window, ::GetFocus()))
			return { EViewContainerFocusCaptureStatus::NoFocus, {} };
		return { EViewContainerFocusCaptureStatus::Captured, ViewContainerFocusToken{ reinterpret_cast<ViewContainerNativeHandle>(::GetFocus()) } };
	}
	EViewContainerPageDetachStatus Detach(const ViewContainerPageHost& host) noexcept override
	{
		if (m_container.closed || m_container.host != host) return EViewContainerPageDetachStatus::Failed;
		::ShowWindow(m_container.root.window, SW_HIDE);
		m_container.host.reset();
		for (auto& pane : m_owner->m_impl->panes) if (pane->container == &m_container) {
			pane->mountedVisible = false;
			if (pane->bodyVisible) { pane->bodyVisible = false; pane->body->SetVisible(false); }
		}
		return EViewContainerPageDetachStatus::Detached;
	}
	EViewContainerPageReparentStatus Reparent(ViewContainerNativeHandle parent) noexcept override
	{
		if (m_container.closed || m_container.host) return EViewContainerPageReparentStatus::Failed;
		if (!Parent(m_container.root.window, parent ? reinterpret_cast<HWND>(parent) : m_owner->m_impl->options.parkingParent))
			return EViewContainerPageReparentStatus::Failed;
		m_container.nativeParent = parent;
		return EViewContainerPageReparentStatus::Reparented;
	}
	EViewContainerPageAttachStatus Attach(const ViewContainerPageHost& host) noexcept override
	{
		if (!m_owner->IsUsable() || m_container.closed || m_container.host || host.nativeParent == 0
			|| host.nativeParent != m_container.nativeParent || !m_container.descriptor.supportedLocations.Contains(host.location))
			return EViewContainerPageAttachStatus::Failed;
		try { m_container.host = host; }
		catch (...) { return EViewContainerPageAttachStatus::Failed; }
		// Lay the panes out while the container is still hidden, then show it, so a
		// retained page never appears at its previous geometry first.
		m_owner->m_impl->Layout(m_container, false);
		::ShowWindow(m_container.root.window, m_container.visible ? SW_SHOWNOACTIVATE : SW_HIDE);
		if (!m_owner->IsUsable()) { m_container.host.reset(); return EViewContainerPageAttachStatus::Failed; }
		return EViewContainerPageAttachStatus::Attached;
	}
	EViewContainerFocusRestoreStatus RestoreFocusToken(ViewContainerFocusToken token) noexcept override
	{
		const auto target = reinterpret_cast<HWND>(token.value);
		if (!m_owner->IsUsable() || !m_container.host || !Contains(m_container.root.window, target) || !::IsWindowVisible(target))
			return EViewContainerFocusRestoreStatus::Failed;
		::SetFocus(target);
		return ::GetFocus() == target ? EViewContainerFocusRestoreStatus::Restored : EViewContainerFocusRestoreStatus::Failed;
	}
	EViewContainerPageCloseStatus Close() noexcept override
	{
		if (m_container.closed) return EViewContainerPageCloseStatus::Closed;
		auto& owner = *m_owner->m_impl;
		for (auto& pane : owner.panes) if (pane->container == &m_container) {
			if (pane->bodyVisible) { pane->bodyVisible = false; pane->body->SetVisible(false); }
			pane->mountedVisible = false;
			::ShowWindow(pane->root.window, SW_HIDE); ::ShowWindow(pane->sash.window, SW_HIDE);
			if (!Parent(pane->root.window, owner.options.parkingParent) || !Parent(pane->sash.window, owner.options.parkingParent)) {
				owner.Fault(); return EViewContainerPageCloseStatus::Failed;
			}
		}
		m_container.closed = true; m_container.host.reset();
		m_container.scrollbar.Destroy();
		if (m_container.root.window) ::DestroyWindow(m_container.root.window);
		return EViewContainerPageCloseStatus::Closed;
	}
	void ActivateProjection() noexcept override { m_container.active = true; }
	void DeactivateProjection() noexcept override { m_container.active = false; }
	bool PreTranslateProjection(MSG& message) noexcept override { return m_owner->m_impl->PreTranslate(m_container, message); }
	void LayoutProjection(const RECT& bounds, const RECT& content, unsigned int dpi) noexcept override
	{
		if (!m_owner->IsUsable() || m_container.closed) return;
		if (bounds.right < bounds.left || bounds.bottom < bounds.top || content.right < content.left || content.bottom < content.top
			|| bounds.right - static_cast<std::int64_t>(bounds.left) > 1000000 || bounds.bottom - static_cast<std::int64_t>(bounds.top) > 1000000
			|| content.left < 0 || content.top < 0 || content.right > bounds.right - bounds.left || content.bottom > bounds.bottom - bounds.top
			|| dpi > 768) { m_owner->m_impl->Fault(); return; }
		m_container.content = content; m_container.dpi = dpi == 0 ? 96 : dpi;
		if (!::SetWindowPos(m_container.root.window, nullptr, bounds.left, bounds.top, bounds.right - bounds.left,
			bounds.bottom - bounds.top, SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOCOPYBITS)) { m_owner->m_impl->Fault(); return; }
		m_owner->m_impl->Layout(m_container, false);
	}
	void SetProjectionVisible(bool visible) noexcept override
	{
		if (!m_owner->IsUsable() || m_container.closed) return;
		m_container.visible = visible;
		const bool show = visible && m_container.host.has_value();
		if (!show) ::ShowWindow(m_container.root.window, SW_HIDE);
		m_owner->m_impl->Layout(m_container, false);
		if (show) ::ShowWindow(m_container.root.window, SW_SHOWNOACTIVATE);
	}
	void SetProjectionPalette(const theme::ThemePalette& palette) noexcept override
	{
		auto& owner = *m_owner->m_impl;
		if (!owner.Usable()) return;
		owner.palette = palette;
		for (auto& pane : owner.panes) pane->body->SetPalette(palette, owner.Location(*pane->container));
		for (auto& container : owner.containers) owner.Layout(*container, false);
	}
private:
	std::shared_ptr<CSenpViewContainers> m_owner;
	Impl::Container& m_container;
};

CSenpViewContainers::CSenpViewContainers(SenpViewContainerOptions options) : m_impl(std::make_unique<Impl>(std::move(options))) {}
CSenpViewContainers::~CSenpViewContainers() = default;

std::shared_ptr<CSenpViewContainers> CSenpViewContainers::Create(SenpViewContainerOptions options) noexcept
{
	try {
		auto result = std::shared_ptr<CSenpViewContainers>(new CSenpViewContainers(std::move(options)));
		return result->m_impl->Initialize() ? result : nullptr;
	} catch (...) { return nullptr; } // The provisional cohort owns all native rollback.
}

std::vector<ViewContainerPageDescriptor> CSenpViewContainers::PageDescriptors()
{
	std::vector<ViewContainerPageDescriptor> result;
	if (!IsUsable()) return result;
	for (const auto& container : m_impl->containers) {
		result.push_back({ container->descriptor.id, container->descriptor.supportedLocations,
			[weak = weak_from_this(), id = container->descriptor.id]() -> std::unique_ptr<IViewContainerPage> {
				auto owner = weak.lock();
				if (!owner || !owner->IsUsable()) return {};
				auto* found = owner->m_impl->FindContainer(id);
				if (!found || found->pageIssued || found->closed) return {};
				auto page = std::make_unique<Page>(owner, *found);
				found->pageIssued = true;
				return page;
			} });
	}
	return result;
}

bool CSenpViewContainers::IsUsable() const noexcept { return m_impl->Usable(); }
void CSenpViewContainers::Close() noexcept { m_impl->Close(); }

ESenpViewProjectionStatus CSenpViewContainers::ApplyLayout(const layout::WorkbenchLayoutStateSnapshot& snapshot) noexcept
{
	using Status = ESenpViewProjectionStatus;
	auto& state = *m_impl;
	if (!state.Usable()) return Status::Stopped;
	if (state.applying || (state.layoutGeneration && (snapshot.generation != state.layoutGeneration || snapshot.revision < state.layoutRevision)))
		return Status::Conflict;
	if (snapshot.schemaVersion != layout::kWorkbenchLayoutStateSchemaVersion || snapshot.generation == 0) return Status::Invalid;
	struct Placement { Impl::Pane* pane{}; Impl::Container* target{}; const layout::WorkbenchViewState* model{}; HWND oldParent{}, oldSashParent{}; };
	std::array<Placement, kMaximumViewPanes> changes{};
	std::size_t count{};
	for (const auto& pane : state.panes) {
		const auto found = std::ranges::find(snapshot.views, pane->definition.descriptor.id, &layout::WorkbenchViewState::viewId);
		if (found == snapshot.views.end() || std::ranges::count(snapshot.views, found->viewId, &layout::WorkbenchViewState::viewId) != 1) return Status::Invalid;
		auto* target = state.FindContainer(found->containerId);
		if (!target || target->closed || !::IsWindow(target->root.window)) return Status::Unsupported;
		if (std::ranges::count(snapshot.containers, found->containerId, &layout::WorkbenchViewContainerState::containerId) != 1) return Status::Invalid;
		if (state.layoutGeneration && snapshot.revision == state.layoutRevision
			&& (pane->container != target || pane->visible != found->visible || pane->order != found->order)) return Status::Conflict;
		changes[count++] = { pane.get(), target, &*found, ::GetParent(pane->root.window), ::GetParent(pane->sash.window) };
	}
	state.applying = true;
	const auto focus = ::GetFocus();
	std::size_t touched{};
	for (; touched < count; ++touched) {
		auto& change = changes[touched];
		if (!Parent(change.pane->root.window, change.target->root.window) || !Parent(change.pane->sash.window, change.target->root.window)) break;
	}
	if (touched != count) {
		bool restored = true;
		for (std::size_t i = touched + 1; i > 0; --i) {
			auto& change = changes[i - 1];
			const bool pane = Parent(change.pane->root.window, change.oldParent);
			const bool sash = Parent(change.pane->sash.window, change.oldSashParent);
			restored = pane && sash && restored;
		}
		state.applying = false;
		if (!restored) state.Close();
		return Status::Failed;
	}
	for (std::size_t i = 0; i < count; ++i) {
		auto& change = changes[i];
		change.pane->container = change.target; change.pane->visible = change.model->visible; change.pane->order = change.model->order;
	}
	state.layoutGeneration = snapshot.generation; state.layoutRevision = snapshot.revision;
	for (auto& container : state.containers) state.Layout(*container, false);
	state.applying = false;
	// Movement preserves the same actual focused control, never focuses a new View.
	if (focus && ::IsWindow(focus) && ::IsWindowVisible(focus) && ::GetFocus() != focus) ::SetFocus(focus);
	return state.Usable() ? Status::Applied : Status::Failed;
}

bool CSenpViewContainers::FocusView(std::string_view id) noexcept
{
	const auto pane = m_impl->FindPane(id);
	return pane && m_impl->Focus(*pane);
}
bool CSenpViewContainers::SetCollapsed(std::string_view id, bool collapsed) noexcept
{
	const auto pane = m_impl->FindPane(id);
	return pane && m_impl->Collapse(*pane, collapsed);
}
ESenpViewProjectionStatus CSenpViewContainers::SetTitleActions(std::string_view id, std::vector<SenpViewTitleAction> actions) noexcept
{
	if (!IsUsable()) return ESenpViewProjectionStatus::Stopped;
	auto* pane = m_impl->FindPane(id);
	if (!pane || !ValidActions(actions) || (!actions.empty() && !m_impl->options.execute)) return ESenpViewProjectionStatus::Invalid;
	for (std::size_t i = 1; i < pane->controls.size(); ++i)
		if (::GetFocus() == pane->controls[i].window && (i > actions.size() || !actions[i - 1].enabled)) ::SetFocus(pane->controls[0].window);
	pane->definition.actions = std::move(actions);
	m_impl->UpdateActions(*pane);
	m_impl->Layout(*pane->container);
	return IsUsable() ? ESenpViewProjectionStatus::Applied : ESenpViewProjectionStatus::Failed;
}
std::optional<SenpViewPaneSnapshot> CSenpViewContainers::Snapshot(std::string_view id) const
{
	const auto* pane = m_impl->FindPane(id);
	if (!IsUsable() || !pane) return {};
	return SenpViewPaneSnapshot{ pane->definition.descriptor.id, pane->container->descriptor.id,
		pane->root.window, pane->controls[0].window, pane->body->Window(), pane->collapsed,
		pane->mountedVisible, pane->definition.preferredBodyDip };
}

} // namespace workbench::viewcontainer

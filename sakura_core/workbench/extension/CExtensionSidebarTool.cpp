/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"

#include "workbench/extension/CExtensionSidebarTool.h"
#include "extension/CExtensionViewRegistry.h"

#include <CommCtrl.h>

#include <algorithm>
#include <atomic>
#include <optional>
#include <unordered_set>
#include <utility>

namespace workbench::extension {
namespace {

constexpr wchar_t kWindowClass[] = L"SakuraExtensionSidebarTool";
constexpr UINT kRefreshMessage = WM_APP + 0x5c1;
constexpr UINT kDispatchRequestsMessage = WM_APP + 0x5c2;
constexpr UINT_PTR kTreeControlId = 1;
constexpr unsigned int kDefaultDpi = 96;
constexpr int kMaximumTreeDepth = 64;

#ifndef TVS_EX_MULTISELECT
#define TVS_EX_MULTISELECT 0x0002
#endif

bool EnsureClass(HINSTANCE instance)
{
	WNDCLASSEXW windowClass{};
	windowClass.cbSize = sizeof(windowClass);
	windowClass.hInstance = instance;
	windowClass.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
	windowClass.lpfnWndProc = CExtensionSidebarTool::WindowProc;
	windowClass.lpszClassName = kWindowClass;
	return ::RegisterClassExW(&windowClass) != 0 || ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

std::wstring NodeKey(std::wstring_view viewHandle, std::wstring_view itemHandle)
{
	std::wstring key(viewHandle);
	key.push_back(L'\0');
	key.append(itemHandle);
	return key;
}

} // namespace

struct CExtensionSidebarTool::Impl {
	enum class NodeKind { View, Item, Placeholder };
	struct Node {
		NodeKind kind = NodeKind::Placeholder;
		std::wstring viewHandle;
		std::wstring itemHandle;
		std::wstring command;
		std::string commandArgumentsJson = "[]";
		std::wstring tooltip;
		std::optional<bool> checked;
	};
	struct PendingRequest {
		std::wstring viewHandle;
		std::wstring parentHandle;
	};

	std::shared_ptr<CExtensionViewRegistry> registry;
	HWND window = nullptr;
	HWND tree = nullptr;
	unsigned int dpi = kDefaultDpi;
	theme::ThemePalette palette = theme::CThemeService::PaletteFor(theme::ThemeMode::Dark);
	theme::CThemeFont font;
	RequestChildrenCallback requestChildren;
	SelectionChangedCallback selectionChanged;
	CheckboxChangedCallback checkboxChanged;
	CommandCallback command;
	VisibilityChangedCallback visibilityChanged;
	ViewFilter viewFilter;
	std::vector<std::unique_ptr<Node>> nodes;
	std::vector<PendingRequest> pendingRequests;
	std::unordered_set<std::wstring> inFlight;
	std::unordered_set<std::wstring> expanded;
	std::atomic_bool refreshPosted = false;
	bool active = false;
	bool sidebarVisible = false;
	bool rebuilding = false;
	bool closed = false;

	explicit Impl(std::shared_ptr<CExtensionViewRegistry> value, ViewFilter filter)
		: registry(std::move(value)), viewFilter(std::move(filter)) {}

	//! Every view this container renders. The registry is shared by all containers, so the
	//! filter — not the registry — decides which of them are this tool's.
	[[nodiscard]] std::vector<SExtensionViewDescriptor> OwnedViews() const
	{
		auto views = registry->Views();
		if (viewFilter) std::erase_if(views, [this](const auto& view) { return !viewFilter(view); });
		return views;
	}

	Node* NewNode(Node value)
	{
		auto node = std::make_unique<Node>(std::move(value));
		Node* raw = node.get();
		nodes.push_back(std::move(node));
		return raw;
	}

	HTREEITEM Insert(HTREEITEM parent, std::wstring text, Node node, bool hasChildren, bool expandedState)
	{
		Node* payload = NewNode(std::move(node));
		TVINSERTSTRUCTW insert{};
		insert.hParent = parent;
		insert.hInsertAfter = TVI_LAST;
		insert.item.mask = TVIF_TEXT | TVIF_PARAM | TVIF_CHILDREN | TVIF_STATE;
		insert.item.pszText = text.data();
		insert.item.lParam = reinterpret_cast<LPARAM>(payload);
		insert.item.cChildren = hasChildren ? 1 : 0;
		insert.item.stateMask = TVIS_EXPANDED;
		insert.item.state = expandedState ? TVIS_EXPANDED : 0;
		const auto item = TreeView_InsertItem(tree, &insert);
		if (item != nullptr) {
			TVITEMW state{};
			state.mask = TVIF_HANDLE | TVIF_STATE;
			state.hItem = item;
			state.stateMask = TVIS_STATEIMAGEMASK;
			state.state = payload->checked.has_value()
				? INDEXTOSTATEIMAGEMASK(*payload->checked ? 2 : 1) : 0;
			TreeView_SetItem(tree, &state);
		}
		return item;
	}

	void QueueRequest(std::wstring_view viewHandle, std::wstring_view parentHandle)
	{
		if (!requestChildren || registry->HasChildrenSnapshot(viewHandle, parentHandle)) return;
		const auto key = NodeKey(viewHandle, parentHandle);
		if (!inFlight.emplace(key).second) return;
		pendingRequests.push_back({ std::wstring(viewHandle), std::wstring(parentHandle) });
	}

	void InsertLoading(HTREEITEM parent, std::wstring_view viewHandle, std::wstring text)
	{
		Insert(parent, std::move(text), { NodeKind::Placeholder, std::wstring(viewHandle) }, false, false);
	}

	void InsertChildren(HTREEITEM parent, std::wstring_view viewHandle, std::wstring_view parentHandle, int depth)
	{
		if (depth >= kMaximumTreeDepth) {
			InsertLoading(parent, viewHandle, L"Tree depth limit reached");
			return;
		}
		const auto children = registry->Children(viewHandle, parentHandle);
		if (children.empty()) {
			if (!registry->HasChildrenSnapshot(viewHandle, parentHandle)) {
				QueueRequest(viewHandle, parentHandle);
				InsertLoading(parent, viewHandle, L"Loading…");
			}
			return;
		}
		for (const auto& child : children) {
			std::wstring label = child.label;
			if (!child.description.empty()) label += L"  " + child.description;
			const bool mayHaveChildren = child.collapsibleState != EExtensionTreeItemCollapsibleState::None;
			const auto key = NodeKey(viewHandle, child.handle);
			const bool isExpanded = child.collapsibleState == EExtensionTreeItemCollapsibleState::Expanded
				|| expanded.contains(key);
			const auto treeItem = Insert(parent, std::move(label), {
				NodeKind::Item, child.viewHandle, child.handle, child.command, child.commandArgumentsJson, child.tooltip,
				child.checkboxState,
			}, mayHaveChildren, isExpanded);
			if (treeItem != nullptr && mayHaveChildren && isExpanded) InsertChildren(treeItem, viewHandle, child.handle, depth + 1);
		}
	}

	void Rebuild()
	{
		refreshPosted.store(false, std::memory_order_release);
		if (closed || !tree || rebuilding) return;
		rebuilding = true;
		::SendMessageW(tree, WM_SETREDRAW, FALSE, 0);
		TreeView_DeleteAllItems(tree);
		nodes.clear();
		const auto views = OwnedViews();
		std::unordered_set<std::wstring> liveViews;
		for (const auto& view : views) liveViews.emplace(view.handle);
		std::erase_if(inFlight, [&](const auto& key) {
			const auto separator = key.find(L'\0');
			if (separator == std::wstring::npos) return true;
			const std::wstring_view viewHandle(key.data(), separator);
			const std::wstring_view parentHandle(key.data() + separator + 1, key.size() - separator - 1);
			return !liveViews.contains(std::wstring(viewHandle))
				|| registry->HasChildrenSnapshot(viewHandle, parentHandle);
		});
		if (views.empty()) {
			Insert(TVI_ROOT, L"No contributed views", { NodeKind::Placeholder }, false, false);
		} else {
			for (const auto& view : views) {
				std::wstring title = view.title.empty() ? view.viewId : view.title;
				if (view.badgeValue != 0) title += L"  (" + std::to_wstring(view.badgeValue) + L")";
				const auto key = NodeKey(view.handle, {});
				const bool isExpanded = !expanded.contains(L"-" + key);
				const auto root = Insert(TVI_ROOT, std::move(title), {
					NodeKind::View, view.handle, {}, {}, "[]", view.description,
				}, true, isExpanded);
				if (root != nullptr && isExpanded) {
					const auto children = registry->Children(view.handle);
					if (children.empty() && registry->HasChildrenSnapshot(view.handle)) {
						InsertLoading(root, view.handle, view.message.empty() ? L"No data" : view.message);
					} else {
						InsertChildren(root, view.handle, {}, 0);
					}
				}
			}
		}
		::SendMessageW(tree, WM_SETREDRAW, TRUE, 0);
		::InvalidateRect(tree, nullptr, TRUE);
		rebuilding = false;
		if (!pendingRequests.empty()) ::PostMessageW(window, kDispatchRequestsMessage, 0, 0);
	}

	Node* Payload(HTREEITEM item) const
	{
		if (!tree || !item) return nullptr;
		TVITEMW value{};
		value.mask = TVIF_HANDLE | TVIF_PARAM;
		value.hItem = item;
		return TreeView_GetItem(tree, &value) ? reinterpret_cast<Node*>(value.lParam) : nullptr;
	}

	void RequestExpanded(Node& node)
	{
		if (node.kind == NodeKind::Placeholder) return;
		QueueRequest(node.viewHandle, node.itemHandle);
		if (!pendingRequests.empty()) ::PostMessageW(window, kDispatchRequestsMessage, 0, 0);
	}

	void DispatchRequests()
	{
		auto requests = std::move(pendingRequests);
		pendingRequests.clear();
		for (const auto& request : requests) {
			try {
				if (requestChildren) requestChildren(request.viewHandle, request.parentHandle);
			} catch (...) {
				inFlight.erase(NodeKey(request.viewHandle, request.parentHandle));
			}
		}
	}

	void NotifySelection(HTREEITEM selected)
	{
		Node* selectedNode = Payload(selected);
		if (!selectedNode || selectedNode->kind != NodeKind::Item || !selectionChanged) return;
		std::vector<std::wstring> handles{ selectedNode->itemHandle };
		(void)registry->SetSelection(selectedNode->viewHandle, handles);
		try { selectionChanged(selectedNode->viewHandle, handles); } catch (...) {}
	}

	void InvokeSelected()
	{
		Node* node = Payload(TreeView_GetSelection(tree));
		if (!node || node->kind != NodeKind::Item || node->command.empty() || !command) return;
		try { command(node->command, node->commandArgumentsJson, node->viewHandle, node->itemHandle); } catch (...) {}
	}

	void NotifyCheckbox(HTREEITEM item, UINT state)
	{
		Node* node = Payload(item);
		if (!node || node->kind != NodeKind::Item || !node->checked.has_value()) return;
		const UINT image = (state & TVIS_STATEIMAGEMASK) >> 12;
		if (image != 1 && image != 2) return;
		const bool checked = image == 2;
		if (*node->checked == checked) return;
		node->checked = checked;
		if (checkboxChanged) {
			try { checkboxChanged(node->viewHandle, node->itemHandle, checked); } catch (...) {}
		}
	}

	void NotifyVisibility(bool visible)
	{
		if (sidebarVisible == visible) return;
		sidebarVisible = visible;
		if (!visibilityChanged) return;
		try { visibilityChanged(visible); } catch (...) {}
	}
};

CExtensionSidebarTool::CExtensionSidebarTool(
	std::shared_ptr<CExtensionViewRegistry> registry, ViewFilter viewFilter)
	: m_impl(std::make_unique<Impl>(std::move(registry), std::move(viewFilter)))
{
}

CExtensionSidebarTool::~CExtensionSidebarTool()
{
	Close();
}

bool CExtensionSidebarTool::Create(HWND parent)
{
	if (m_impl->closed || m_impl->window || !parent || !m_impl->registry) return false;
	auto instance = reinterpret_cast<HINSTANCE>(::GetWindowLongPtrW(parent, GWLP_HINSTANCE));
	if (!instance) instance = ::GetModuleHandleW(nullptr);
	if (!EnsureClass(instance)) return false;
	m_impl->window = ::CreateWindowExW(0, kWindowClass, L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
		0, 0, 0, 0, parent, nullptr, instance, this);
	if (!m_impl->window) return false;
	m_impl->tree = ::CreateWindowExW(WS_EX_CLIENTEDGE, WC_TREEVIEWW, L"",
		WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | TVS_HASBUTTONS | TVS_HASLINES |
		TVS_LINESATROOT | TVS_SHOWSELALWAYS | TVS_INFOTIP | TVS_CHECKBOXES,
		0, 0, 0, 0, m_impl->window, reinterpret_cast<HMENU>(kTreeControlId), instance, nullptr);
	if (!m_impl->tree) { Close(); return false; }
	::SendMessageW(m_impl->tree, TVM_SETEXTENDEDSTYLE,
		TVS_EX_DOUBLEBUFFER | TVS_EX_MULTISELECT, TVS_EX_DOUBLEBUFFER | TVS_EX_MULTISELECT);
	SetPalette(m_impl->palette);
	m_impl->Rebuild();
	return true;
}

void CExtensionSidebarTool::Layout(const RECT& contentRect, unsigned int dpi)
{
	if (m_impl->closed || !m_impl->window) return;
	m_impl->dpi = dpi == 0 ? kDefaultDpi : dpi;
	if (m_impl->font.Dpi() != m_impl->dpi) (void)m_impl->font.Recreate(theme::ThemeFontKind::Chrome, m_impl->dpi);
	if (m_impl->tree) ::SendMessageW(m_impl->tree, WM_SETFONT, reinterpret_cast<WPARAM>(m_impl->font.Get()), TRUE);
	const int width = std::max(0L, contentRect.right - contentRect.left);
	const int height = std::max(0L, contentRect.bottom - contentRect.top);
	::SetWindowPos(m_impl->window, nullptr, contentRect.left, contentRect.top, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
	if (m_impl->tree) ::MoveWindow(m_impl->tree, 0, 0, width, height, TRUE);
}

void CExtensionSidebarTool::Activate()
{
	m_impl->active = true;
	if (m_impl->tree) ::SetFocus(m_impl->tree);
}

void CExtensionSidebarTool::Deactivate()
{
	m_impl->active = false;
}

bool CExtensionSidebarTool::PreTranslateMessage(MSG& message)
{
	if (!m_impl->active || message.hwnd != m_impl->tree || message.message != WM_KEYDOWN) return false;
	if (message.wParam == VK_RETURN) { m_impl->InvokeSelected(); return true; }
	return false;
}

void CExtensionSidebarTool::Close()
{
	if (!m_impl || m_impl->closed) return;
	m_impl->closed = true;
	m_impl->NotifyVisibility(false);
	m_impl->refreshPosted.store(false, std::memory_order_release);
	if (m_impl->window && ::IsWindow(m_impl->window)) ::DestroyWindow(m_impl->window);
	m_impl->window = nullptr;
	m_impl->tree = nullptr;
	m_impl->nodes.clear();
	m_impl->pendingRequests.clear();
	m_impl->inFlight.clear();
}

void CExtensionSidebarTool::SetPalette(const theme::ThemePalette& palette)
{
	m_impl->palette = palette;
	if (m_impl->tree) {
		TreeView_SetBkColor(m_impl->tree, palette.sideBar.ToColorRef());
		TreeView_SetTextColor(m_impl->tree, palette.primaryText.ToColorRef());
		TreeView_SetLineColor(m_impl->tree, palette.border.ToColorRef());
		::InvalidateRect(m_impl->tree, nullptr, TRUE);
	}
}

void CExtensionSidebarTool::SetRequestChildrenCallback(RequestChildrenCallback callback) { m_impl->requestChildren = std::move(callback); }
void CExtensionSidebarTool::SetSelectionChangedCallback(SelectionChangedCallback callback) { m_impl->selectionChanged = std::move(callback); }
void CExtensionSidebarTool::SetCheckboxChangedCallback(CheckboxChangedCallback callback) { m_impl->checkboxChanged = std::move(callback); }
void CExtensionSidebarTool::SetCommandCallback(CommandCallback callback) { m_impl->command = std::move(callback); }
void CExtensionSidebarTool::SetVisibilityChangedCallback(VisibilityChangedCallback callback) { m_impl->visibilityChanged = std::move(callback); }

void CExtensionSidebarTool::Refresh()
{
	if (m_impl->closed || !m_impl->window) return;
	if (!m_impl->refreshPosted.exchange(true, std::memory_order_acq_rel)) {
		if (!::PostMessageW(m_impl->window, kRefreshMessage, 0, 0)) m_impl->refreshPosted.store(false, std::memory_order_release);
	}
}

void CExtensionSidebarTool::SetSidebarVisible(bool visible)
{
	m_impl->NotifyVisibility(visible);
	if (visible) Refresh();
}

HWND CExtensionSidebarTool::GetHwnd() const noexcept { return m_impl->window; }

LRESULT CALLBACK CExtensionSidebarTool::WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
	if (message == WM_NCCREATE) {
		auto* self = static_cast<CExtensionSidebarTool*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
		::SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
	}
	auto* self = reinterpret_cast<CExtensionSidebarTool*>(::GetWindowLongPtrW(window, GWLP_USERDATA));
	if (!self || !self->m_impl) return ::DefWindowProcW(window, message, wParam, lParam);
	auto& impl = *self->m_impl;
	switch (message) {
	case WM_SIZE:
		if (impl.tree) ::MoveWindow(impl.tree, 0, 0, LOWORD(lParam), HIWORD(lParam), TRUE);
		return 0;
	case WM_ERASEBKGND:
		return 1;
	case kRefreshMessage:
		impl.Rebuild();
		return 0;
	case kDispatchRequestsMessage:
		impl.DispatchRequests();
		return 0;
	case WM_NOTIFY: {
		const auto* header = reinterpret_cast<const NMHDR*>(lParam);
		if (!header || header->hwndFrom != impl.tree) break;
		if (header->code == TVN_ITEMEXPANDINGW) {
			const auto* event = reinterpret_cast<const NMTREEVIEWW*>(lParam);
			if (auto* node = impl.Payload(event->itemNew.hItem); node != nullptr) {
				const auto key = NodeKey(node->viewHandle, node->itemHandle);
				if (event->action == TVE_EXPAND) {
					if (node->kind == Impl::NodeKind::View) impl.expanded.erase(L"-" + key);
					else impl.expanded.insert(key);
					impl.RequestExpanded(*node);
				} else if (event->action == TVE_COLLAPSE) {
					if (node->kind == Impl::NodeKind::View) impl.expanded.insert(L"-" + key);
					else impl.expanded.erase(key);
				}
			}
			return 0;
		}
		if (header->code == TVN_SELCHANGEDW) {
			impl.NotifySelection(reinterpret_cast<const NMTREEVIEWW*>(lParam)->itemNew.hItem);
			return 0;
		}
		if (header->code == TVN_ITEMCHANGEDW && !impl.rebuilding) {
			const auto* event = reinterpret_cast<const NMTVITEMCHANGE*>(lParam);
			if ((event->uChanged & TVIF_STATE) != 0 &&
				(event->uStateNew & TVIS_STATEIMAGEMASK) != (event->uStateOld & TVIS_STATEIMAGEMASK)) {
				impl.NotifyCheckbox(event->hItem, event->uStateNew);
			}
			return 0;
		}
		if (header->code == NM_DBLCLK || header->code == NM_RETURN) {
			impl.InvokeSelected();
			return 0;
		}
		if (header->code == TVN_GETINFOTIPW) {
			auto* tip = reinterpret_cast<NMTVGETINFOTIPW*>(lParam);
			if (const auto* node = impl.Payload(tip->hItem); node != nullptr && !node->tooltip.empty()) {
				::wcsncpy_s(tip->pszText, static_cast<std::size_t>(tip->cchTextMax), node->tooltip.c_str(), _TRUNCATE);
			}
			return 0;
		}
		break;
	}
	}
	return ::DefWindowProcW(window, message, wParam, lParam);
}

} // namespace workbench::extension

/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#include "StdAfx.h"
#include "workbench/tree/SenpTreeView.h"
#include "workbench/controls/COverlayScrollbar.h"
#include "workbench/viewcontainer/ViewPaneChrome.h"
#include "theme/CThemeService.h"
#include <CommCtrl.h>
#include <windowsx.h>
#include <algorithm>
#include <map>
#include <set>
#include <unordered_map>
#include <utility>

namespace workbench::tree {
namespace {
constexpr wchar_t kClass[] = L"SakuraSenpTreeView";
constexpr UINT kSync = WM_APP + 0x5e8, kInvoke = WM_APP + 0x5e9;
constexpr UINT_PTR kDeadlineTimer = 1, kTreeSubclass = 296;
int Dip(int value, unsigned int dpi) noexcept { return ::MulDiv(value, dpi, 96); }
SenpTreeProvider::Time Now() noexcept { return std::chrono::steady_clock::now(); }
void Fill(HDC dc, RECT rect, COLORREF color) noexcept
{
	const auto old = ::SetDCBrushColor(dc, color); ::FillRect(dc, &rect, static_cast<HBRUSH>(::GetStockObject(DC_BRUSH))); ::SetDCBrushColor(dc, old);
}
}

struct CSenpTreeView::Impl final : ISenpTreeObserver {
	enum class RowKind { Item, Loading, Empty, Retry, More, Unrequested, Stopped };
	struct Row final {
		std::wstring id, parent, label;
		HTREEITEM handle{};
		std::uint64_t token{};
		RowKind kind{ RowKind::Item };
	};
	SenpTreeViewOptions options;
	HWND window{}, tree{};
	std::unordered_map<HTREEITEM, std::unique_ptr<Row>> rows;
	std::map<std::wstring, HTREEITEM, std::less<>> items;
	std::set<std::wstring, std::less<>> dirty;
	controls::COverlayScrollbar scrollbar;
	theme::CThemeFont font;
	theme::ThemePalette palette{ theme::CThemeService::PaletteFor(theme::ThemeMode::Dark) };
	layout::EViewContainerLocation location{ layout::EViewContainerLocation::Sidebar };
	unsigned int dpi{ 96 };
	std::uint64_t nextToken{ 1 }, armed{}, invoking{};
	HTREEITEM hover{};
	bool closed{}, failed{}, visible{}, synchronizing{}, posted{}, invokePosted{}, observed{}, invokeExpand{};
	explicit Impl(SenpTreeViewOptions value) : options(std::move(value)) {}
	~Impl() { Close(); }
	bool Usable() const noexcept { return !closed && !failed && window && tree; }
	theme::ThemeColor Surface() const noexcept { return location == layout::EViewContainerLocation::Panel ? palette.bottomPanel : palette.sideBar; }
	Row* Find(HTREEITEM item) const noexcept { const auto it = rows.find(item); return it == rows.end() ? nullptr : it->second.get(); }
	HTREEITEM Handle(std::wstring_view id) const noexcept { const auto it = items.find(id); return it == items.end() ? nullptr : it->second; }
	void Interaction() { if (options.host.interactionChanged) options.host.interactionChanged(); }
	void Fault() noexcept
	{
		if (failed || closed) return;
		failed = true; armed = invoking = 0;
		if (observed) { options.provider->Unobserve(*this); observed = false; options.provider->Close(); }
		if (window) ::KillTimer(window, kDeadlineTimer);
		scrollbar.Destroy();
		if (window) ::DestroyWindow(window);
		if (auto report = std::exchange(options.host.projectionFailed, {})) report();
	}
	void Close() noexcept
	{
		if (closed) return;
		closed = true;
		options.host.interactionChanged = {};
		options.host.projectionFailed = {};
		if (observed) { options.provider->Unobserve(*this); observed = false; options.provider->Close(); }
		if (window) ::KillTimer(window, kDeadlineTimer);
		scrollbar.Destroy();
		if (window) ::DestroyWindow(window);
		window = tree = nullptr; rows.clear(); items.clear(); dirty.clear(); font.Reset();
	}
	bool Initialize()
	{
		if (!options.provider || options.provider->Model().IsClosed() || !::IsWindow(options.host.parent)
			|| options.title.empty() || options.title.size() > 1024) return false;
		WNDCLASSEXW cls{ sizeof(cls) }; cls.lpfnWndProc = Procedure; cls.hInstance = ::GetModuleHandleW(nullptr);
		cls.hCursor = ::LoadCursorW(nullptr, IDC_ARROW); cls.lpszClassName = kClass;
		if (!::RegisterClassExW(&cls) && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
		INITCOMMONCONTROLSEX controls{ sizeof(controls), ICC_TREEVIEW_CLASSES };
		if (!::InitCommonControlsEx(&controls)) return false;
		window = ::CreateWindowExW(WS_EX_CONTROLPARENT, kClass, L"", WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
			0, 0, 0, 0, options.host.parent, nullptr, cls.hInstance, this);
		if (!window) return false;
		tree = ::CreateWindowExW(0, WC_TREEVIEWW, options.title.c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP
			| TVS_HASBUTTONS | TVS_LINESATROOT | TVS_FULLROWSELECT | TVS_SHOWSELALWAYS | TVS_NOHSCROLL | TVS_INFOTIP | TVS_DISABLEDRAGDROP | TVS_NONEVENHEIGHT,
			0, 0, 0, 0, window, reinterpret_cast<HMENU>(1), cls.hInstance, nullptr);
		if (!tree || !::SetWindowSubclass(tree, TreeProcedure, kTreeSubclass, reinterpret_cast<DWORD_PTR>(this))) return false;
		::SendMessageW(tree, TVM_SETEXTENDEDSTYLE, TVS_EX_DOUBLEBUFFER, TVS_EX_DOUBLEBUFFER);
		if (!scrollbar.Create(window, tree, [this](int position) {
			if (!Usable()) return;
			auto item = TreeView_GetRoot(tree);
			for (int row = 0; item && row < position; ++row) item = TreeView_GetNextVisible(tree, item);
			if (item) TreeView_SelectSetFirstVisible(tree, item);
			scrollbar.Update();
		})) return false;
		observed = options.provider->Observe(*this);
		if (!observed || !Metrics()) return false;
		TreeChanged(L""); return true;
	}
	bool Metrics()
	{
		if (!font.Recreate(theme::ThemeFontKind::Chrome, dpi)) return false;
		::SendMessageW(tree, WM_SETFONT, reinterpret_cast<WPARAM>(font.Get()), FALSE);
		TreeView_SetItemHeight(tree, Dip(viewcontainer::kViewPaneRowDip, dpi)); TreeView_SetIndent(tree, Dip(12, dpi));
		TreeView_SetBkColor(tree, Surface().ToColorRef()); TreeView_SetTextColor(tree, palette.primaryText.ToColorRef());
		scrollbar.SetDpi(dpi); scrollbar.SetColors(controls::ResolveOverlayScrollbarColors(palette, Surface()));
		return true;
	}
	void TreeChanged(std::wstring_view parentId) noexcept override
	{
		if (!Usable()) return;
		try {
			if (parentId.empty() || dirty.size() >= kMaximumTreeLoads) { dirty.clear(); dirty.emplace(); }
			else if (!dirty.contains(L"")) dirty.emplace(parentId);
			if (!posted) { posted = ::PostMessageW(window, kSync, 0, 0) != FALSE; if (!posted) Fault(); }
		} catch (...) { Fault(); }
	}
	std::wstring Label(const TreeNodeSnapshot& node) const
	{ return node.item.description.empty() ? node.item.label : node.item.label + L" - " + node.item.description; }
	void Insert(std::unique_ptr<Row> row, HTREEITEM parent, bool hasChildren)
	{
		row->token = nextToken++;
		TVINSERTSTRUCTW insert{}; insert.hParent = parent; insert.hInsertAfter = TVI_LAST;
		insert.item.mask = TVIF_TEXT | TVIF_CHILDREN;
		insert.item.pszText = row->label.data(); insert.item.cChildren = hasChildren ? 1 : 0;
		row->handle = TreeView_InsertItem(tree, &insert);
		if (!row->handle) { Fault(); return; }
		if (row->kind == RowKind::Item) items[row->id] = row->handle;
		rows.emplace(row->handle, std::move(row));
	}
	std::vector<std::wstring> CurrentChildren(HTREEITEM parent) const
	{
		std::vector<std::wstring> result;
		for (auto item = parent == TVI_ROOT ? TreeView_GetRoot(tree) : TreeView_GetChild(tree, parent); item; item = TreeView_GetNextSibling(tree, item))
			if (const auto* row = Find(item); row && row->kind == RowKind::Item) result.push_back(row->id);
		return result;
	}
	void DeleteChildren(HTREEITEM parent)
	{
		while (const auto item = parent == TVI_ROOT ? TreeView_GetRoot(tree) : TreeView_GetChild(tree, parent))
			if (!TreeView_DeleteItem(tree, item)) { Fault(); return; }
	}
	void Status(const TreeNodeSnapshot& node, HTREEITEM parent)
	{
		for (auto item = parent == TVI_ROOT ? TreeView_GetRoot(tree) : TreeView_GetChild(tree, parent); item;) {
			const auto next = TreeView_GetNextSibling(tree, item);
			if (const auto* row = Find(item); row && row->kind != RowKind::Item) TreeView_DeleteItem(tree, item);
			item = next;
		}
		auto row = std::make_unique<Row>(); row->parent = node.item.id;
		switch (node.state) {
		case TreeChildrenState::Loading: row->kind = RowKind::Loading; row->label = L"Loading..."; break;
		case TreeChildrenState::Partial: row->kind = RowKind::More; row->label = L"Load more..."; break;
		case TreeChildrenState::Failed: row->kind = RowKind::Retry; row->label = L"Retry"; if (!node.message.empty()) row->label += L" - " + node.message; break;
		case TreeChildrenState::Empty: row->kind = RowKind::Empty; row->label = L"No items"; break;
		case TreeChildrenState::Unrequested: case TreeChildrenState::Stale: row->kind = RowKind::Unrequested; row->label = L"Not loaded"; break;
		case TreeChildrenState::Stopped: row->kind = RowKind::Stopped; row->label = L"Provider unavailable"; break;
		default: return;
		}
		if (node.state == TreeChildrenState::Empty && parent != TVI_ROOT) return;
		Insert(std::move(row), parent, false);
	}
	void SyncBranch(std::wstring_view id)
	{
		const auto* parent = options.provider->Model().Inspect(id);
		if (!parent) return;
		const auto parentHandle = id.empty() ? TVI_ROOT : Handle(id);
		if (!parentHandle) return;
		if (CurrentChildren(parentHandle) != parent->children) {
			DeleteChildren(parentHandle);
			for (const auto& child : parent->children) {
				const auto* node = options.provider->Model().Inspect(child); if (!node) { Fault(); return; }
				auto row = std::make_unique<Row>(); row->id = child; row->parent = std::wstring(id); row->label = Label(*node);
				Insert(std::move(row), parentHandle, node->item.collapsibleState != TreeItemCollapsibleState::None);
				if (!Usable()) return;
			}
		}
		for (const auto& child : parent->children) {
			const auto* node = options.provider->Model().Inspect(child);
			const auto item = Handle(child); auto* row = Find(item);
			if (!node || !row) { Fault(); return; }
			row->label = Label(*node);
			TVITEMW value{}; value.mask = TVIF_TEXT | TVIF_CHILDREN; value.hItem = item; value.pszText = row->label.data();
			value.cChildren = node->item.collapsibleState != TreeItemCollapsibleState::None ? 1 : 0;
			if (!TreeView_SetItem(tree, &value)) { Fault(); return; }
			if (node->item.collapsibleState == TreeItemCollapsibleState::None) DeleteChildren(item);
			else if (node->expanded) SyncBranch(child);
			TreeView_Expand(tree, item, node->expanded ? TVE_EXPAND : TVE_COLLAPSE);
		}
		Status(*parent, parentHandle);
	}
	void Deadline()
	{
		::KillTimer(window, kDeadlineTimer);
		if (const auto deadline = options.provider->NextDeadline()) {
			const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(*deadline - Now()).count();
			if (!::SetTimer(window, kDeadlineTimer, static_cast<UINT>(std::clamp<std::int64_t>(milliseconds + 1, 1, 30001)), nullptr)) Fault();
		}
	}
	void Synchronize()
	{
		posted = false;
		const auto first = TreeView_GetFirstVisible(tree); const auto* firstRow = Find(first);
		const std::wstring anchor = firstRow ? firstRow->id : L"";
		synchronizing = true;
		struct Guard final { bool& flag; ~Guard() { flag = false; } } guard{ synchronizing };
		if (dirty.contains(L"")) SyncBranch(L"");
		else for (const auto& id : dirty) {
			if (!Handle(id)) { SyncBranch(L""); break; }
			SyncBranch(id);
			if (const auto* node = options.provider->Model().Inspect(id)) TreeView_Expand(tree, Handle(id), node->expanded ? TVE_EXPAND : TVE_COLLAPSE);
		}
		dirty.clear();
		if (!Usable()) return;
		const auto selection = options.provider->Model().Selection();
		const auto selected = Handle(selection);
		if (selected != TreeView_GetSelection(tree)) TreeView_SelectItem(tree, selected);
		if (!anchor.empty()) if (auto item = Handle(anchor)) {
			// Restoring a now-hidden child with TVGN_FIRSTVISIBLE silently
			// expands its ancestors. Anchor to the collapsed parent instead.
			for (auto parent = TreeView_GetParent(tree, item); parent; parent = TreeView_GetParent(tree, parent))
				if (!(TreeView_GetItemState(tree, parent, TVIS_EXPANDED) & TVIS_EXPANDED)) item = parent;
			TreeView_SelectSetFirstVisible(tree, item);
		}
			scrollbar.Update(); ::InvalidateRect(tree, nullptr, FALSE); Deadline();
	}
	Row* Hit(POINT point, bool activationOnly) const noexcept
	{
		TVHITTESTINFO hit{}; hit.pt = point; const auto item = TreeView_HitTest(tree, &hit);
		if (activationOnly && !(hit.flags & (TVHT_ONITEMLABEL | TVHT_ONITEMICON | TVHT_ONITEMRIGHT))) return nullptr;
		return Find(item);
	}
	void QueueInvoke(Row* row, bool explicitExpand = false)
	{
		if (!row || (row->kind != RowKind::Item && row->kind != RowKind::More && row->kind != RowKind::Retry)) return;
		invoking = row->token;
		invokeExpand = explicitExpand;
		if (!invokePosted) { invokePosted = ::PostMessageW(window, kInvoke, 0, 0) != FALSE; if (!invokePosted) Fault(); }
	}
	void Invoke()
	{
		invokePosted = false; const auto token = std::exchange(invoking, 0);
		for (const auto& [handle, row] : rows) if (row->token == token) {
			const auto id = row->id, parent = row->parent; const auto kind = row->kind;
			if (kind == RowKind::More) (void)options.provider->LoadNext(parent, Now());
			else if (kind == RowKind::Retry) (void)options.provider->Retry(parent, Now());
			else {
				const auto* node = options.provider->Model().Inspect(id);
				if (node && node->item.commandId.empty() && (invokeExpand || options.expandOnSingleClick) && node->item.collapsibleState != TreeItemCollapsibleState::None)
					(void)options.provider->SetExpanded(id, !node->expanded, Now());
				else (void)options.provider->Execute(id);
			}
			return;
		}
	}
	void Draw(NMTVCUSTOMDRAW& draw)
	{
		const auto item = reinterpret_cast<HTREEITEM>(draw.nmcd.dwItemSpec); const auto* row = Find(item); if (!row) return;
		RECT line{}; if (!TreeView_GetItemRect(tree, item, &line, FALSE)) return;
		RECT text{}; if (!TreeView_GetItemRect(tree, item, &text, TRUE)) return;
		RECT client{}; ::GetClientRect(tree, &client); line.left = client.left; line.right = client.right;
		const bool selected = (draw.nmcd.uItemState & CDIS_SELECTED) != 0, focused = ::GetFocus() == tree;
		Fill(draw.nmcd.hdc, line, (selected ? focused ? palette.accent : palette.raised : hover == item ? palette.raised : Surface()).ToColorRef());
		const auto oldFont = ::SelectObject(draw.nmcd.hdc, font.Get()); const auto oldMode = ::SetBkMode(draw.nmcd.hdc, TRANSPARENT);
		const auto main = selected && focused ? palette.highlightText : palette.primaryText;
		const auto secondary = selected && focused ? palette.highlightText : palette.descriptionText;
		const int side = Dip(viewcontainer::kViewPaneIconDip, dpi), top = line.top + (line.bottom - line.top - side) / 2;
		std::wstring_view label = row->label, description;
		if (row->kind == RowKind::Item) {
			if (const auto* node = options.provider->Model().Inspect(row->id)) {
				label = node->item.label; description = node->item.description;
				if (node->item.collapsibleState != TreeItemCollapsibleState::None)
					viewcontainer::PaintViewPaneIcon(draw.nmcd.hdc, { text.left - side, top, text.left, top + side }, node->expanded ? L"chevron-down" : L"chevron-right", secondary.ToColorRef());
				if (!node->item.icon.empty()) {
					viewcontainer::PaintViewPaneIcon(draw.nmcd.hdc, { text.left, top, text.left + side, top + side }, node->item.icon, main.ToColorRef());
					text.left += side + Dip(6, dpi);
				}
			}
		}
		text.top = line.top; text.bottom = line.bottom; text.right = std::max(text.left, client.right - Dip(10, dpi));
		const auto oldColor = ::SetTextColor(draw.nmcd.hdc, main.ToColorRef());
		if (!description.empty() && text.right > text.left) {
			SIZE labelSize{}, descriptionSize{};
			::GetTextExtentPoint32W(draw.nmcd.hdc, label.data(), static_cast<int>(label.size()), &labelSize);
			::GetTextExtentPoint32W(draw.nmcd.hdc, description.data(), static_cast<int>(description.size()), &descriptionSize);
			const int available = text.right - text.left, gap = Dip(6, dpi);
			const int labelWidth = std::max(0, std::min(static_cast<int>(labelSize.cx), available - std::min(static_cast<int>(descriptionSize.cx) + gap, available / 3)));
			RECT primary = text; primary.right = primary.left + labelWidth;
			::DrawTextW(draw.nmcd.hdc, label.data(), static_cast<int>(label.size()), &primary, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
			text.left = std::min(text.right, primary.right + gap); label = description; ::SetTextColor(draw.nmcd.hdc, secondary.ToColorRef());
		}
		::DrawTextW(draw.nmcd.hdc, label.data(), static_cast<int>(label.size()), &text, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
		::SetTextColor(draw.nmcd.hdc, oldColor); ::SetBkMode(draw.nmcd.hdc, oldMode); ::SelectObject(draw.nmcd.hdc, oldFont);
	}
	LRESULT Message(UINT message, WPARAM wParam, LPARAM lParam)
	{
		if (message == WM_PAINT || message == WM_PRINTCLIENT) {
			PAINTSTRUCT paint{}; const auto dc = message == WM_PAINT ? ::BeginPaint(window, &paint) : reinterpret_cast<HDC>(wParam);
			RECT client{}; ::GetClientRect(window, &client); Fill(dc, client, Surface().ToColorRef());
			if (message == WM_PAINT) ::EndPaint(window, &paint); return 0;
		}
		if (!Usable()) return ::DefWindowProcW(window, message, wParam, lParam);
		if (message == kSync) { Synchronize(); return 0; }
		if (message == kInvoke) { Invoke(); return 0; }
		if (message == WM_TIMER && wParam == kDeadlineTimer) { ::KillTimer(window, kDeadlineTimer); options.provider->Pump(Now()); Deadline(); return 0; }
		if (message == WM_NOTIFY) {
			const auto* notice = reinterpret_cast<const NMHDR*>(lParam); if (!notice || notice->hwndFrom != tree) return 0;
			if (notice->code == TVN_DELETEITEMW) {
				const auto handle = reinterpret_cast<const NMTREEVIEWW*>(notice)->itemOld.hItem;
				if (const auto* row = Find(handle); row && row->kind == RowKind::Item) items.erase(row->id);
				if (hover == handle) hover = nullptr;
				rows.erase(handle); return 0;
			}
			if (notice->code == NM_CUSTOMDRAW) {
				auto* draw = reinterpret_cast<NMTVCUSTOMDRAW*>(lParam);
				if (draw->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
				if (draw->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) { Draw(*draw); return CDRF_SKIPDEFAULT; }
			}
			if (notice->code == TVN_GETINFOTIPW) {
				auto* tip = reinterpret_cast<NMTVGETINFOTIPW*>(lParam);
				const auto* row = Find(tip->hItem);
				if (row && tip->cchTextMax > 0) {
					const auto* node = row->kind == RowKind::Item ? options.provider->Model().Inspect(row->id) : nullptr;
					const auto& text = node && !node->item.tooltip.empty() ? node->item.tooltip : row->label;
					::wcsncpy_s(tip->pszText, static_cast<std::size_t>(tip->cchTextMax), text.c_str(), _TRUNCATE);
				}
				return 0;
			}
			if (synchronizing) return 0;
			if (notice->code == TVN_ITEMEXPANDINGW) {
				const auto* change = reinterpret_cast<const NMTREEVIEWW*>(notice);
				const auto* row = Find(change->itemNew.hItem);
				if (row && row->kind == RowKind::Item) (void)options.provider->SetExpanded(row->id, (change->action & TVE_EXPAND) != 0, Now());
				return TRUE; // The coalesced projection applies the admitted model state.
			}
			if (notice->code == TVN_SELCHANGEDW) {
				const auto* row = Find(reinterpret_cast<const NMTREEVIEWW*>(notice)->itemNew.hItem);
				if (row && row->kind == RowKind::Item) (void)options.provider->Select(row->id);
				Interaction(); return 0;
			}
		}
		return ::DefWindowProcW(window, message, wParam, lParam);
	}
	static LRESULT CALLBACK Procedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) noexcept
	{
		auto* state = reinterpret_cast<Impl*>(::GetWindowLongPtrW(window, GWLP_USERDATA));
		if (message == WM_NCCREATE) { state = static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams); state->window = window; ::SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state)); }
		if (!state) return ::DefWindowProcW(window, message, wParam, lParam);
		if (message == WM_NCDESTROY) { state->window = nullptr; ::SetWindowLongPtrW(window, GWLP_USERDATA, 0); if (!state->closed) state->Fault(); return ::DefWindowProcW(window, message, wParam, lParam); }
		try { return state->Message(message, wParam, lParam); } catch (...) { state->Fault(); return 0; }
	}
	static LRESULT CALLBACK TreeProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR data) noexcept
	{
		auto& state = *reinterpret_cast<Impl*>(data);
		if (message == WM_NCDESTROY) { ::RemoveWindowSubclass(window, TreeProcedure, kTreeSubclass); state.tree = nullptr; if (!state.closed) state.Fault(); return ::DefSubclassProc(window, message, wParam, lParam); }
		if (!state.Usable()) return ::DefSubclassProc(window, message, wParam, lParam);
		try {
			// TVM_EXPAND stops notifying after TVIS_EXPANDEDONCE. Intercept
			// external/native-accessibility requests before that silent path so
			// every repetition still updates the authoritative model.
			if (message == TVM_EXPAND && !state.synchronizing) {
				const auto* row = state.Find(reinterpret_cast<HTREEITEM>(lParam));
				if (!row || row->kind != RowKind::Item || (wParam != TVE_EXPAND && wParam != TVE_COLLAPSE && wParam != TVE_TOGGLE)) return FALSE;
				const auto* node = state.options.provider->Model().Inspect(row->id); if (!node) return FALSE;
				const bool expand = wParam == TVE_TOGGLE ? !node->expanded : wParam == TVE_EXPAND;
				const auto result = state.options.provider->SetExpanded(row->id, expand, Now());
				return result == TreeResult::Applied || result == TreeResult::Unchanged;
			}
			if (message == WM_LBUTTONDOWN) { const auto* row = state.Hit({ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) }, true); state.armed = row ? row->token : 0; }
			if (message == WM_LBUTTONUP) {
				auto* row = state.Hit({ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) }, true);
				if (row && row->token == state.armed) state.QueueInvoke(row);
				state.armed = 0;
			}
			if (message == WM_CANCELMODE || message == WM_CAPTURECHANGED) state.armed = 0;
			if (message == WM_LBUTTONDBLCLK) {
				if (!state.options.expandOnSingleClick) state.QueueInvoke(state.Hit({ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) }, true), true);
				return 0; // Native double-click expansion must not bypass the command/twistie rule.
			}
			if (message == WM_KEYDOWN && wParam == VK_RETURN) { state.QueueInvoke(state.Find(TreeView_GetSelection(window)), true); return 0; }
			if (message == WM_GETDLGCODE) {
				const auto* key = reinterpret_cast<const MSG*>(lParam);
				if (key && key->message == WM_KEYDOWN && key->wParam == VK_RETURN) return DLGC_WANTMESSAGE;
			}
			if (message == WM_MOUSEMOVE || message == WM_MOUSELEAVE) {
				const auto* row = message == WM_MOUSEMOVE ? state.Hit({ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) }, false) : nullptr;
				const auto hovered = row ? row->handle : nullptr;
				if (hovered != state.hover) { state.hover = hovered; ::InvalidateRect(window, nullptr, FALSE); }
				if (message == WM_MOUSEMOVE) { TRACKMOUSEEVENT tracking{ sizeof(tracking), TME_LEAVE, window, 0 }; ::TrackMouseEvent(&tracking); }
				state.Interaction();
			}
			if (message == WM_SETFOCUS || message == WM_KILLFOCUS) { ::InvalidateRect(window, nullptr, FALSE); state.Interaction(); }
			const auto result = ::DefSubclassProc(window, message, wParam, lParam);
			if (message == WM_MOUSEWHEEL || message == WM_VSCROLL || message == WM_KEYDOWN || message == WM_SIZE) state.scrollbar.Update();
			return result;
		} catch (...) { state.Fault(); return 0; }
	}
};

CSenpTreeView::CSenpTreeView(SenpTreeViewOptions options) : m_impl(std::make_unique<Impl>(std::move(options))) {}
CSenpTreeView::~CSenpTreeView() = default;
std::unique_ptr<CSenpTreeView> CSenpTreeView::Create(SenpTreeViewOptions options) noexcept
{
	try { auto result = std::unique_ptr<CSenpTreeView>(new CSenpTreeView(std::move(options))); return result->m_impl->Initialize() ? std::move(result) : nullptr; }
	catch (...) { return {}; }
}
HWND CSenpTreeView::Window() const noexcept { return m_impl->window; }
HWND CSenpTreeView::TreeWindow() const noexcept { return m_impl->tree; }
bool CSenpTreeView::IsUsable() const noexcept { return m_impl->Usable(); }
void CSenpTreeView::Layout(const RECT& bounds, unsigned int dpi) noexcept
{
	auto& state = *m_impl; if (!state.Usable()) return;
	if (dpi > 768 || bounds.right < bounds.left || bounds.bottom < bounds.top
		|| bounds.right - static_cast<std::int64_t>(bounds.left) > 1000000 || bounds.bottom - static_cast<std::int64_t>(bounds.top) > 1000000) { state.Fault(); return; }
	try {
		const auto next = dpi ? dpi : 96;
		if (state.dpi != next) { state.dpi = next; if (!state.Metrics()) { state.Fault(); return; } }
		const int width = bounds.right - bounds.left, height = bounds.bottom - bounds.top;
		if (!::SetWindowPos(state.window, nullptr, bounds.left, bounds.top, width, height, SWP_NOZORDER | SWP_NOACTIVATE)
			|| !::SetWindowPos(state.tree, nullptr, 0, 0, width, height, SWP_NOZORDER | SWP_NOACTIVATE)) { state.Fault(); return; }
		state.scrollbar.Update(); ::InvalidateRect(state.tree, nullptr, FALSE);
	} catch (...) { state.Fault(); }
}
void CSenpTreeView::SetVisible(bool visible) noexcept
{
	auto& state = *m_impl; if (!state.Usable()) return;
	try { state.visible = visible; state.options.provider->SetVisible(visible, Now()); ::ShowWindow(state.window, visible ? SW_SHOWNOACTIVATE : SW_HIDE); state.Deadline(); }
	catch (...) { state.Fault(); }
}
void CSenpTreeView::SetPalette(const theme::ThemePalette& palette, layout::EViewContainerLocation location) noexcept
{
	auto& state = *m_impl; if (!state.Usable()) return;
	state.palette = palette; state.location = location;
	try { if (!state.Metrics()) { state.Fault(); return; } state.scrollbar.Update(); ::InvalidateRect(state.tree, nullptr, FALSE); ::InvalidateRect(state.window, nullptr, FALSE); }
	catch (...) { state.Fault(); }
}
bool CSenpTreeView::Focus() noexcept
{
	if (!m_impl->Usable() || !m_impl->visible || !::IsWindowVisible(m_impl->tree)) return false;
	::SetFocus(m_impl->tree); return ::GetFocus() == m_impl->tree;
}
bool CSenpTreeView::PreTranslate(MSG&) noexcept { return false; }
void CSenpTreeView::Close() noexcept { m_impl->Close(); }

} // namespace workbench::tree

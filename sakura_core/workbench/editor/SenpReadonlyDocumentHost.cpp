/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#include "StdAfx.h"
#include "workbench/editor/SenpReadonlyDocumentHost.h"
#include "markdown/CMarkdownPreviewWnd.h"
#include "theme/CThemeService.h"
#include <CommCtrl.h>
#include <algorithm>
#include <stdexcept>

namespace workbench::editor {
namespace {
constexpr UINT kSectionControl = 1;
bool SameOwner(const SenpReadonlyScope& document, const senp::TextResourceScope& resource)
{
	return resource.extensionId == document.extensionId && resource.ownerGeneration == document.ownerGeneration
		&& resource.workspaceRevision == document.workspaceRevision && resource.accountGeneration == document.accountGeneration;
}
const wchar_t* Notice(SenpDocumentHostState state, SenpDocumentState model)
{
	if (state == SenpDocumentHostState::Denied) return L"This document is no longer available.";
	if (state == SenpDocumentHostState::Unsupported) return L"Text output is unavailable in this environment.";
	if (state == SenpDocumentHostState::Failed) return L"The document could not be displayed. Refresh to try again.";
	switch (model) {
	case SenpDocumentState::Loading: return L"Loading...";
	case SenpDocumentState::Failed: return L"The document could not be loaded. Refresh to try again.";
	case SenpDocumentState::Expired: return L"This document is no longer available.";
	case SenpDocumentState::Closed: return L"The document is closed.";
	default: return L"No content has been requested.";
	}
}
}
class SenpReadonlyDocumentHost::Impl {
	friend class SenpReadonlyDocumentHost;
	class TextBody {
		friend class SenpReadonlyDocumentHost::Impl;
		friend class SenpReadonlyDocumentHost;
		senp::TextResourceScope scope;
		std::wstring handle;
		std::unique_ptr<SenpTextResourceView> view;
		bool readReady{ true };
	public:
		TextBody(senp::TextResourceScope value, std::wstring identity) : scope(std::move(value)), handle(std::move(identity)) {}
	};
	class Page {
		friend class SenpReadonlyDocumentHost::Impl;
		friend class SenpReadonlyDocumentHost;
		SenpDocumentPageKind kind{ SenpDocumentPageKind::Structured };
		std::wstring label;
		SenpStructuredSectionRange sections;
		std::unique_ptr<SenpReadonlyDocumentView> structured;
		std::shared_ptr<TextBody> text;
	public:
		Page() = default;
		HWND Window() const noexcept { return structured ? structured->Window() : text && text->view ? text->view->Window() : nullptr; }
		HWND Focus() const noexcept { return structured ? structured->FocusWindow() : text && text->view ? text->view->FocusWindow() : nullptr; }
	};
	SenpReadonlyDocument& model;
	rendering::FrameSurfaceId firstSurfaceId;
	const ISenpReadonlyTextResources* resources;
	SenpTextResourceView::CopySink copy;
	HWND root{}, selector{}, label{}, notice{};
	SenpDocumentHostState state{ SenpDocumentHostState::Unavailable };
	std::vector<Page> pages;
	std::optional<std::size_t> active;
	std::optional<std::uint64_t> generation;
	std::optional<SenpDocumentTextRead> inFlight;
	std::uint64_t nextTicket{};
	theme::ThemePalette palette{ theme::CThemeService::PaletteFor(theme::ThemeMode::Dark) };
	theme::CThemeFont chromeFont;
	LOGFONT editorFont{};
	unsigned int dpi{ 96 };

	public:
	Impl(SenpReadonlyDocument& value, rendering::FrameSurfaceId id, const ISenpReadonlyTextResources* authority,
		SenpTextResourceView::CopySink sink) : model(value), firstSurfaceId(id), resources(authority), copy(std::move(sink)) {
		if (!id || id > UINT64_MAX - 31) throw std::invalid_argument("Reserve 32 SENP document frame identities.");
		wcscpy_s(editorFont.lfFaceName, L"Consolas");
	}
	private:
	Page* Current() noexcept { return active && *active < pages.size() ? &pages[*active] : nullptr; }
	void ClearPages() noexcept {
		active.reset(); inFlight.reset();
		for (auto& page : pages) {
			if (page.structured) page.structured->Close();
			if (page.text && page.text->view) page.text->view->Close();
		}
		pages.clear();
		if (selector) ::SendMessageW(selector, CB_RESETCONTENT, 0, 0);
	}
	void Close() noexcept {
		state = SenpDocumentHostState::Closed; ClearPages();
		if (root) { auto window = root; root = nullptr; ::DestroyWindow(window); }
		selector = label = notice = nullptr; copy = {};
	}
	void LayoutChildren() {
		if (!root) return;
		RECT bounds{}; ::GetClientRect(root, &bounds);
		const bool navigation = pages.size() > 1;
		const int header = navigation ? std::min<int>(bounds.bottom, ::MulDiv(36, dpi, 96)) : 0;
		const int inset = ::MulDiv(6, dpi, 96), labelWidth = ::MulDiv(54, dpi, 96);
		constexpr auto flags = SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOREDRAW | SWP_NOCOPYBITS;
		if (!::SetWindowPos(label, nullptr, inset, 0, labelWidth, header, flags)
			|| !::SetWindowPos(selector, nullptr, inset + labelWidth, ::MulDiv(3, dpi, 96),
				std::max<int>(0, bounds.right - labelWidth - 2 * inset), ::MulDiv(220, dpi, 96), flags)
			|| !::SetWindowPos(notice, nullptr, inset, header + inset, std::max<int>(0, bounds.right - 2 * inset),
				std::max<int>(0, bounds.bottom - header - 2 * inset), flags)) { Close(); return; }
		::ShowWindow(label, navigation ? SW_SHOWNA : SW_HIDE);
		::ShowWindow(selector, navigation ? SW_SHOWNA : SW_HIDE);
		::ShowWindow(notice, state == SenpDocumentHostState::Ready ? SW_HIDE : SW_SHOWNA);
		bounds.top = header;
		// Hidden retained pages receive geometry when selected, not on every resize.
		if (auto* page = Current()) {
			if (page->structured) page->structured->Layout(bounds, dpi);
			else if (page->text && page->text->view) page->text->view->Layout(bounds, dpi);
		}
		::RedrawWindow(root, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN | RDW_UPDATENOW);
	}
	SenpDocumentHostState Terminal(SenpDocumentHostState result) {
		ClearPages(); state = result;
		if (notice) ::SetWindowTextW(notice, Notice(state, model.State()));
		LayoutChildren(); return state;
	}
	bool Authorized() const {
		for (const auto& page : pages)
			if (page.text && (!resources || !resources->IsCurrent(page.text->scope, page.text->handle))) return false;
		return true;
	}
	bool Select(std::size_t index, bool focus) {
		if (state != SenpDocumentHostState::Ready || generation != model.Generation() || model.State() != SenpDocumentState::Ready
			|| index >= pages.size()) return false;
		if (!Authorized()) { Terminal(SenpDocumentHostState::Denied); return false; }
		auto& page = pages[index];
		if (!page.Window()) {
			if (page.text) {
				page.text->view = std::make_unique<SenpTextResourceView>(page.text->scope, page.text->handle, copy);
				if (!page.text->view->Create(root)) { Terminal(SenpDocumentHostState::Failed); return false; }
				page.text->view->SetStyle(palette, dpi);
			} else {
				page.structured = std::make_unique<SenpReadonlyDocumentView>(model, firstSurfaceId + index, page.sections);
				page.structured->SetCopySink(copy);
				if (!page.structured->Create(root)) { Terminal(SenpDocumentHostState::Failed); return false; }
				page.structured->SetStyle(palette, editorFont, dpi);
			}
			if (!page.Window()) { Terminal(SenpDocumentHostState::Failed); return false; }
		}
		if (auto* previous = Current(); previous && previous->Window() != page.Window()) ::ShowWindow(previous->Window(), SW_HIDE);
		active = index; ::SendMessageW(selector, CB_SETCURSEL, index, 0);
		LayoutChildren();
		if (state != SenpDocumentHostState::Ready) return false;
		::ShowWindow(page.Window(), SW_SHOWNA);
		::RedrawWindow(root, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN | RDW_UPDATENOW);
		if (focus) ::SetFocus(page.Focus());
		return true;
	}
	SenpDocumentHostState Sync() {
		if (!root || state == SenpDocumentHostState::Closed) return state;
		if (generation == model.Generation()) {
			if (state == SenpDocumentHostState::Ready && !Authorized()) return Terminal(SenpDocumentHostState::Denied);
			return state;
		}
		ClearPages(); generation = model.Generation();
		if (model.State() != SenpDocumentState::Ready) return Terminal(SenpDocumentHostState::Unavailable);
		const auto document = model.Content();
		if (!document || !senp::effect::ValidateDocument(*document)) return Terminal(SenpDocumentHostState::Failed);
		// Resolve the complete bounded cohort before creating or revealing a body.
		for (std::size_t first = 0; first < document->sections.size();) {
			Page page;
			const auto rangeFirst = first;
			if (const auto* section = std::get_if<senp::effect::TextResourceSection>(&document->sections[first])) {
				if (!resources) return Terminal(SenpDocumentHostState::Unsupported);
				const auto scope = resources->Resolve(model.Input().scope, *section);
				if (!scope || !SameOwner(model.Input().scope, *scope) || !resources->IsCurrent(*scope, section->handle))
					return Terminal(SenpDocumentHostState::Denied);
				page.kind = SenpDocumentPageKind::TextResource;
				page.label = L"Text output"; page.sections = { rangeFirst, 1 };
				for (const auto& existing : pages)
					if (existing.text && existing.text->scope == *scope && existing.text->handle == section->handle) page.text = existing.text;
				if (!page.text) page.text = std::make_shared<TextBody>(*scope, section->handle);
				++first;
			} else {
				page.label = L"Details";
				do { ++first; } while (first < document->sections.size() && !std::holds_alternative<senp::effect::TextResourceSection>(document->sections[first]));
				page.sections = { rangeFirst, first - rangeFirst };
			}
			pages.push_back(std::move(page));
		}
		if (pages.empty()) { Page page; page.label = L"Details"; pages.push_back(std::move(page)); }
		for (std::size_t index = 0; index < pages.size(); ++index) {
			auto& page = pages[index]; std::size_t count = 0, ordinal = 0;
			for (std::size_t other = 0; other < pages.size(); ++other) if (pages[other].kind == page.kind) { ++count; if (other <= index) ++ordinal; }
			if (count > 1) page.label += L" " + std::to_wstring(ordinal);
			if (::SendMessageW(selector, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(page.label.c_str())) < 0)
				return Terminal(SenpDocumentHostState::Failed);
		}
		state = SenpDocumentHostState::Ready;
		(void)Select(0, false); return state;
	}
	Page* CommandPage() {
		if (Sync() != SenpDocumentHostState::Ready) return nullptr;
		return Current();
	}
	void Style() {
		if (!chromeFont.Recreate(theme::ThemeFontKind::Chrome, dpi)) { Close(); return; }
		for (auto window : { selector, label, notice }) if (window) ::SendMessageW(window, WM_SETFONT, reinterpret_cast<WPARAM>(chromeFont.Get()), FALSE);
		if (selector) {
			::SendMessageW(selector, CB_SETITEMHEIGHT, 0, ::MulDiv(24, dpi, 96));
			::SendMessageW(selector, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), ::MulDiv(24, dpi, 96));
		}
		for (auto& page : pages) {
			if (page.structured) page.structured->SetStyle(palette, editorFont, dpi);
			if (page.text && page.text->view) page.text->view->SetStyle(palette, dpi);
		}
		LayoutChildren();
	}
	void Draw(const DRAWITEMSTRUCT& item) const {
		if (item.CtlID != kSectionControl) return;
		const bool selected = (item.itemState & ODS_SELECTED) != 0;
		::SetDCBrushColor(item.hDC, (selected ? palette.accent : palette.inputBackground).ToColorRef());
		::FillRect(item.hDC, &item.rcItem, static_cast<HBRUSH>(::GetStockObject(DC_BRUSH)));
		::SetTextColor(item.hDC, (selected ? palette.highlightText : palette.primaryText).ToColorRef());
		::SetBkMode(item.hDC, TRANSPARENT);
		const auto previous = ::SelectObject(item.hDC, chromeFont.Get());
		RECT text = item.rcItem; text.left += ::MulDiv(6, dpi, 96);
		if (item.itemID < pages.size()) ::DrawTextW(item.hDC, pages[item.itemID].label.c_str(), -1, &text,
			DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
		if ((item.itemState & (ODS_FOCUS | ODS_NOFOCUSRECT)) == ODS_FOCUS) ::DrawFocusRect(item.hDC, &item.rcItem);
		::SelectObject(item.hDC, previous);
	}
	static LRESULT CALLBACK Procedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR context) {
		auto& self = *reinterpret_cast<Impl*>(context);
		try {
			switch (message) {
			case WM_SIZE: self.LayoutChildren(); return 0;
			case WM_SETFOCUS: if (auto* page = self.CommandPage()) ::SetFocus(page->Focus()); return 0;
			case WM_COMMAND:
				if (reinterpret_cast<HWND>(lParam) == self.selector && HIWORD(wParam) == CBN_SELCHANGE) {
					const auto index = ::SendMessageW(self.selector, CB_GETCURSEL, 0, 0);
					if (index >= 0) (void)self.Select(static_cast<std::size_t>(index), false);
					return 0;
				}
				break;
			case WM_MEASUREITEM:
				if (wParam == kSectionControl) { reinterpret_cast<MEASUREITEMSTRUCT*>(lParam)->itemHeight = ::MulDiv(24, self.dpi, 96); return TRUE; }
				break;
			case WM_DRAWITEM: self.Draw(*reinterpret_cast<const DRAWITEMSTRUCT*>(lParam)); return TRUE;
			case WM_CTLCOLORLISTBOX: case WM_CTLCOLORSTATIC: {
				const auto background = (message == WM_CTLCOLORLISTBOX ? self.palette.inputBackground : self.palette.canvas).ToColorRef();
				::SetTextColor(reinterpret_cast<HDC>(wParam), self.palette.primaryText.ToColorRef());
				::SetBkColor(reinterpret_cast<HDC>(wParam), background); ::SetDCBrushColor(reinterpret_cast<HDC>(wParam), background);
				return reinterpret_cast<LRESULT>(::GetStockObject(DC_BRUSH));
			}
			case WM_ERASEBKGND: {
				RECT bounds{}; ::GetClientRect(window, &bounds); ::SetDCBrushColor(reinterpret_cast<HDC>(wParam), self.palette.canvas.ToColorRef());
				::FillRect(reinterpret_cast<HDC>(wParam), &bounds, static_cast<HBRUSH>(::GetStockObject(DC_BRUSH))); return 1;
			}
			case WM_PAINT: {
				PAINTSTRUCT paint{}; const auto dc = ::BeginPaint(window, &paint); ::SetDCBrushColor(dc, self.palette.canvas.ToColorRef());
				::FillRect(dc, &paint.rcPaint, static_cast<HBRUSH>(::GetStockObject(DC_BRUSH))); ::EndPaint(window, &paint); return 0;
			}
			case WM_NCDESTROY:
				self.root = self.selector = self.label = self.notice = nullptr; self.state = SenpDocumentHostState::Closed;
				self.ClearPages(); self.copy = {}; ::RemoveWindowSubclass(window, Procedure, 1); break;
			default: break;
			}
		} catch (const std::exception&) { self.Close(); return 0; }
		return ::DefSubclassProc(window, message, wParam, lParam);
	}
};

SenpReadonlyDocumentHost::SenpReadonlyDocumentHost(SenpReadonlyDocument& model, rendering::FrameSurfaceId id,
	const ISenpReadonlyTextResources* resources, SenpTextResourceView::CopySink copy)
	: m_impl(std::make_unique<Impl>(model, id, resources, std::move(copy))) {}
SenpReadonlyDocumentHost::~SenpReadonlyDocumentHost() { m_impl->Close(); }
bool SenpReadonlyDocumentHost::Create(HWND parent) {
	auto& self = *m_impl;
	if (self.root || self.state == SenpDocumentHostState::Closed || !::IsWindow(parent)) return false;
	try {
		self.root = ::CreateWindowExW(WS_EX_CONTROLPARENT, L"STATIC", L"", WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
			0, 0, 1, 1, parent, nullptr, ::GetModuleHandleW(nullptr), nullptr);
		if (!self.root || !::SetWindowSubclass(self.root, Impl::Procedure, 1, reinterpret_cast<DWORD_PTR>(&self))) { self.Close(); return false; }
		self.label = ::CreateWindowExW(0, L"STATIC", L"&Section", WS_CHILD | SS_CENTERIMAGE, 0, 0, 1, 1, self.root, nullptr, nullptr, nullptr);
		self.selector = ::CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS,
			0, 0, 1, 1, self.root, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kSectionControl)), nullptr, nullptr);
		self.notice = ::CreateWindowExW(0, L"STATIC", L"", WS_CHILD, 0, 0, 1, 1, self.root, nullptr, nullptr, nullptr);
		if (!self.selector || !self.label || !self.notice) { self.Close(); return false; }
		self.Style(); (void)self.Sync(); return self.state != SenpDocumentHostState::Closed;
	} catch (const std::exception&) { self.Close(); return false; }
}
SenpDocumentHostState SenpReadonlyDocumentHost::Sync() { try { return m_impl->Sync(); } catch (const std::exception&) { m_impl->Close(); return SenpDocumentHostState::Closed; } }
void SenpReadonlyDocumentHost::SetStyle(const theme::ThemePalette& palette, const LOGFONT& font, unsigned int dpi) {
	if (m_impl->state == SenpDocumentHostState::Closed) return;
	try { m_impl->palette = palette; m_impl->editorFont = font; m_impl->dpi = dpi ? dpi : 96; m_impl->Style(); } catch (const std::exception&) { m_impl->Close(); }
}
void SenpReadonlyDocumentHost::Layout(const RECT& bounds, unsigned int dpi) {
	if (!m_impl->root || bounds.right < bounds.left || bounds.bottom < bounds.top) return;
	try {
		if (dpi && dpi != m_impl->dpi) { m_impl->dpi = dpi; m_impl->Style(); }
		if (!::SetWindowPos(m_impl->root, nullptr, bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top,
			SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS)) { m_impl->Close(); return; }
		m_impl->LayoutChildren();
	} catch (const std::exception&) { m_impl->Close(); }
}
void SenpReadonlyDocumentHost::Show(bool visible) noexcept { if (m_impl->root) ::ShowWindow(m_impl->root, visible ? SW_SHOWNA : SW_HIDE); }
void SenpReadonlyDocumentHost::Close() noexcept { m_impl->Close(); }
bool SenpReadonlyDocumentHost::SelectPage(std::size_t index, bool focus) {
	try { (void)m_impl->Sync(); return m_impl->Select(index, focus); } catch (const std::exception&) { m_impl->Close(); return false; }
}
std::vector<SenpDocumentPage> SenpReadonlyDocumentHost::Pages() const {
	std::vector<SenpDocumentPage> result;
	for (const auto& page : m_impl->pages) result.emplace_back(page.kind, page.label, page.sections);
	return result;
}
std::optional<std::size_t> SenpReadonlyDocumentHost::ActivePage() const noexcept { return m_impl->active; }
SenpDocumentHostState SenpReadonlyDocumentHost::State() const noexcept { return m_impl->state; }
std::optional<SenpDocumentTextRead> SenpReadonlyDocumentHost::TakeTextRead() {
	try {
		auto& self = *m_impl; auto* page = self.CommandPage();
		if (!page || !page->text || !page->text->view || !page->text->readReady || self.inFlight || !::IsWindowVisible(self.root)) return {};
		const auto viewport = page->text->view->Viewport();
		if (viewport.state != senp::TextResourceState::Loading) return {};
		if (self.nextTicket == UINT64_MAX) { self.Close(); return {}; }
		self.inFlight = SenpDocumentTextRead{ *self.generation, ++self.nextTicket, page->text->scope, page->text->handle,
			viewport.byteOffset, senp::SenpTextResourceStore::kChunkBytes };
		page->text->readReady = false; return self.inFlight;
	} catch (const std::exception&) { m_impl->Close(); return {}; }
}
SenpTextViewResult SenpReadonlyDocumentHost::ApplyText(const SenpDocumentTextRead& request, const senp::TextResourceChunk& chunk) {
	try {
		auto& self = *m_impl;
		if (self.Sync() == SenpDocumentHostState::Closed) return SenpTextViewResult::Closed;
		if (!self.inFlight || *self.inFlight != request || self.state != SenpDocumentHostState::Ready) return SenpTextViewResult::Stale;
		self.inFlight.reset();
		for (auto& page : self.pages) if (page.text && page.text->view && page.text->scope == request.Scope() && page.text->handle == request.Handle()) {
			if (chunk.offset != request.Offset() || chunk.bytes.size() > request.Count()) { page.text->view->Fail(); return SenpTextViewResult::Invalid; }
			const auto result = page.text->view->Apply(request.Scope(), chunk);
			if (result == SenpTextViewResult::Applied) page.text->readReady = page.text->readReady || !chunk.bytes.empty();
			else if (result != SenpTextViewResult::Closed) page.text->view->Fail();
			return result;
		}
		return SenpTextViewResult::Stale;
	} catch (const std::exception&) { m_impl->Close(); return SenpTextViewResult::Closed; }
}
void SenpReadonlyDocumentHost::FailText(const SenpDocumentTextRead& request, senp::TextResourceEnd reason) noexcept {
	auto& self = *m_impl;
	if (!self.inFlight || *self.inFlight != request) return;
	self.inFlight.reset();
		for (auto& page : self.pages) if (page.text && page.text->view && page.text->scope == request.Scope() && page.text->handle == request.Handle()) {
		if (reason == senp::TextResourceEnd::Revoked) page.text->view->Expire(); else page.text->view->Fail(reason);
		page.text->readReady = false; return;
	}
}
void SenpReadonlyDocumentHost::NotifyTextChanged(const senp::TextResourceScope& scope, std::wstring_view handle) {
	try {
		if (m_impl->Sync() != SenpDocumentHostState::Ready) return;
		for (auto& page : m_impl->pages) if (page.text && page.text->scope == scope && page.text->handle == handle) page.text->readReady = true;
	} catch (const std::exception&) { m_impl->Close(); }
}
void SenpReadonlyDocumentHost::SelectAll() {
	try { if (auto* page = m_impl->CommandPage()) { if (page->structured) page->structured->SelectAll(); else page->text->view->SelectAll(); } }
	catch (const std::exception&) { m_impl->Close(); }
}
std::wstring SenpReadonlyDocumentHost::SelectedText() {
	try { if (auto* page = m_impl->CommandPage()) return page->structured ? page->structured->SelectedText() : page->text->view->SelectedText(); }
	catch (const std::exception&) { m_impl->Close(); } return {};
}
bool SenpReadonlyDocumentHost::Copy() {
	try { if (auto* page = m_impl->CommandPage()) return page->structured ? page->structured->Copy() : page->text->view->Copy(); }
	catch (const std::exception&) { m_impl->Close(); } return false;
}
void SenpReadonlyDocumentHost::ShowFind(bool visible) {
	try { if (auto* page = m_impl->CommandPage()) { if (page->structured) page->structured->ShowFind(visible); else page->text->view->ShowFind(visible); } }
	catch (const std::exception&) { m_impl->Close(); }
}
markdown::PreviewFindResult SenpReadonlyDocumentHost::Find(std::wstring_view query, bool previous, bool matchCase) {
	try {
		if (auto* page = m_impl->CommandPage()) {
			if (page->structured) return page->structured->Find(query, previous, matchCase);
			switch (page->text->view->Find(query, previous, matchCase)) {
			case SenpTextFindResult::Found: return markdown::PreviewFindResult::Found;
			case SenpTextFindResult::Wrapped: return markdown::PreviewFindResult::Wrapped;
			case SenpTextFindResult::NotFound: return markdown::PreviewFindResult::NotFound;
			case SenpTextFindResult::Invalid: return markdown::PreviewFindResult::Invalid;
			default: break;
			}
		}
	} catch (const std::exception&) { m_impl->Close(); } return markdown::PreviewFindResult::Unavailable;
}
HWND SenpReadonlyDocumentHost::Window() const noexcept { return m_impl->root; }
HWND SenpReadonlyDocumentHost::FocusWindow() const noexcept { return m_impl->root; }

} // namespace workbench::editor

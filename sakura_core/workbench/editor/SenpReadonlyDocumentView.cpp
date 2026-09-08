/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#include "StdAfx.h"
#include "workbench/editor/SenpReadonlyDocumentView.h"
#include "markdown/CMarkdownPreviewWnd.h"
#include "workbench/controls/CInputBoxGeometry.h"
#include "theme/CThemeService.h"
#include <CommCtrl.h>
#include <algorithm>
#include <stdexcept>

namespace workbench::editor {
namespace {
markdown::Document Notice(std::wstring text)
{
	markdown::Document result;
	markdown::Block block; block.text = std::move(text);
	result.blocks.push_back(std::move(block));
	return result;
}
std::wstring StatusText(SenpDocumentState state)
{
	switch (state) {
	case SenpDocumentState::Dormant: return L"No content has been requested.";
	case SenpDocumentState::Loading: return L"Loading...";
	case SenpDocumentState::Failed: return L"The document could not be loaded. Refresh to try again.";
	case SenpDocumentState::Expired: return L"This document is no longer available.";
	case SenpDocumentState::Closed: return L"The document is closed.";
	default: return L"The document is unavailable.";
	}
}
}
class SenpReadonlyDocumentView::Impl {
	std::optional<SenpStructuredSectionRange> sectionRange;
public:
	SenpReadonlyDocument& model;
	markdown::CMarkdownPreviewWnd preview;
	HWND root{}, query{}, status{};
	bool findVisible{}, statusVisible{};
	theme::ThemePalette palette{ theme::CThemeService::PaletteFor(theme::ThemeMode::Dark) };
	theme::CThemeFont chromeFont;
	RECT inputFrame{};
	int inputLineHeight{};
	unsigned int dpi{ 96 };
	SenpDocumentViewState state{ SenpDocumentViewState::Unavailable };
	std::optional<std::uint64_t> queued;
	std::uint64_t prepared{};
	Impl(SenpReadonlyDocument& value, rendering::FrameSurfaceId surfaceId, std::optional<SenpStructuredSectionRange> range)
		: model(value), preview({}, surfaceId), sectionRange(range)
	{
		if (!surfaceId) throw std::invalid_argument("A SENP document needs a distinct surface identity.");
	}
	void LayoutChild() {
		if (!root || !preview.IsCreated()) return;
		RECT bounds{}; ::GetClientRect(root, &bounds);
		const int findHeight = findVisible ? std::min<int>(bounds.bottom, ::MulDiv(30, dpi, 96)) : 0;
		const int statusHeight = statusVisible ? std::min<int>(bounds.bottom - findHeight, ::MulDiv(22, dpi, 96)) : 0;
		const int inset = ::MulDiv(6, dpi, 96), padding = ::MulDiv(3, dpi, 96);
		inputFrame = { inset, padding, std::max<int>(inset, bounds.right - inset), std::max(padding, findHeight - padding) };
		const auto editor = controls::CenterSingleLineEditor(inputFrame, inputLineHeight, ::MulDiv(4, dpi, 96), ::MulDiv(14, dpi, 96));
		constexpr auto flags = SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS | SWP_NOREDRAW;
		if (!::SetWindowPos(query, nullptr, editor.left, editor.top, editor.right - editor.left, editor.bottom - editor.top, flags)
			|| !::SetWindowPos(status, nullptr, inset, bounds.bottom - statusHeight,
				std::max<int>(0, bounds.right - 2 * inset), statusHeight, flags)) { Close(); return; }
		bounds.top += findHeight; bounds.bottom -= statusHeight; preview.Layout(bounds, dpi);
		// Commit moved chrome and the preview together. Pending child paints can
		// otherwise expose the old heading/query position for a composited frame.
		::RedrawWindow(root, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN | RDW_UPDATENOW);
	}
	void Status(const wchar_t* text) {
		if (!status) return;
		statusVisible = text && *text; ::SetWindowTextW(status, text ? text : L"");
		::ShowWindow(status, statusVisible ? SW_SHOWNA : SW_HIDE); LayoutChild();
	}
	void ShowFind(bool visible, bool focus = true) {
		if (!query || state == SenpDocumentViewState::Closed) return;
		findVisible = visible; ::ShowWindow(query, visible ? SW_SHOWNA : SW_HIDE); LayoutChild();
		if (visible) { if (focus) ::SetFocus(query); ::SendMessageW(query, EM_SETSEL, 0, -1); }
		else { Status(nullptr); if (focus) ::SetFocus(preview.GetHwnd()); }
	}
	markdown::PreviewFindResult Find(std::wstring_view value, bool previous, bool matchCase) {
		if (model.State() != SenpDocumentState::Ready || state != SenpDocumentViewState::Prepared) return markdown::PreviewFindResult::Unavailable;
		const auto result = preview.FindText(value, previous, matchCase);
		if (value.size() <= 1024 && value.find(L'\0') == std::wstring_view::npos) ::SetWindowTextW(query, std::wstring(value).c_str());
		switch (result) {
		case markdown::PreviewFindResult::Found: Status(L"Match in the document."); break;
		case markdown::PreviewFindResult::Wrapped: Status(L"Search wrapped in the document."); break;
		case markdown::PreviewFindResult::NotFound: Status(L"No results in the document."); break;
		case markdown::PreviewFindResult::Invalid: Status(L"Enter a valid search query."); break;
		default: Status(L"The document is not ready to search."); break;
		}
		return result;
	}
	bool Copy() {
		if (model.State() != SenpDocumentState::Ready || state != SenpDocumentViewState::Prepared) return false;
		const bool copied = preview.CopySelection();
		Status(copied ? L"Selection copied." : L"The selection could not be copied. Try again.");
		return copied;
	}
	void FindCurrent(bool previous) {
		wchar_t value[1025]{}; ::GetWindowTextW(query, value, 1025);
		if (!*value) ShowFind(true); else (void)Find(value, previous, false);
	}
	static LRESULT CALLBACK QueryProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR context) {
		auto& self = *reinterpret_cast<Impl*>(context);
		try {
			if (message == WM_NCDESTROY) ::RemoveWindowSubclass(window, QueryProcedure, 1);
			if (message == WM_SETFOCUS || message == WM_KILLFOCUS) {
				const auto result = ::DefSubclassProc(window, message, wParam, lParam);
				if (self.root) ::RedrawWindow(self.root, &self.inputFrame, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
				return result;
			}
			if (message == WM_GETDLGCODE) return DLGC_WANTALLKEYS | DLGC_WANTCHARS;
			if (message == WM_KEYDOWN && (wParam == VK_RETURN || wParam == VK_F3)) { self.FindCurrent((::GetKeyState(VK_SHIFT) & 0x8000) != 0); return 0; }
			if (message == WM_KEYDOWN && wParam == VK_ESCAPE) { self.ShowFind(false); return 0; }
			if (message == WM_CHAR && (wParam == VK_RETURN || wParam == VK_ESCAPE)) return 0;
		} catch (...) { self.Close(); return 0; }
		return ::DefSubclassProc(window, message, wParam, lParam);
	}
	static LRESULT CALLBACK Procedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR context) {
		auto& self = *reinterpret_cast<Impl*>(context);
		try {
			switch (message) {
			case WM_SIZE: self.LayoutChild(); return 0;
			case WM_SETFOCUS: if (self.preview.IsCreated()) ::SetFocus(self.preview.GetHwnd()); return 0;
			case WM_CTLCOLOREDIT: case WM_CTLCOLORSTATIC: {
				const auto background = message == WM_CTLCOLOREDIT ? self.palette.inputBackground.ToColorRef() : self.palette.canvas.ToColorRef();
				::SetBkColor(reinterpret_cast<HDC>(wParam), background);
				::SetTextColor(reinterpret_cast<HDC>(wParam), self.palette.primaryText.ToColorRef());
				::SetDCBrushColor(reinterpret_cast<HDC>(wParam), background);
				return reinterpret_cast<LRESULT>(::GetStockObject(DC_BRUSH));
			}
			case WM_ERASEBKGND: {
				RECT bounds{}; ::GetClientRect(window, &bounds);
				::SetDCBrushColor(reinterpret_cast<HDC>(wParam), self.palette.canvas.ToColorRef());
				::FillRect(reinterpret_cast<HDC>(wParam), &bounds, static_cast<HBRUSH>(::GetStockObject(DC_BRUSH))); return 1;
			}
			case WM_PAINT: {
				PAINTSTRUCT paint{}; const auto dc = ::BeginPaint(window, &paint);
				::SetDCBrushColor(dc, self.palette.canvas.ToColorRef());
				::FillRect(dc, &paint.rcPaint, static_cast<HBRUSH>(::GetStockObject(DC_BRUSH)));
				if (self.findVisible) {
					const auto brush = static_cast<HBRUSH>(::GetStockObject(DC_BRUSH));
					::SetDCBrushColor(dc, self.palette.inputBackground.ToColorRef());
					::FillRect(dc, &self.inputFrame, brush);
					::SetDCBrushColor(dc, (::GetFocus() == self.query ? self.palette.accent : self.palette.inputBorder).ToColorRef());
					::FrameRect(dc, &self.inputFrame, brush);
				}
				::EndPaint(window, &paint); return 0;
			}
			case WM_NCDESTROY:
				self.preview.SetPreparationCallback({}); self.preview.SetFindCallback({}); self.preview.SetCopyCommand({}); self.root = self.query = self.status = nullptr;
				self.state = SenpDocumentViewState::Closed;
				::RemoveWindowSubclass(window, Procedure, 1); break;
			default: break;
			}
		} catch (...) { self.Close(); return 0; }
		return ::DefSubclassProc(window, message, wParam, lParam);
	}
	void Close() noexcept {
		state = SenpDocumentViewState::Closed; preview.SetPreparationCallback({}); preview.Close();
		if (root) { const auto window = root; root = nullptr; ::DestroyWindow(window); }
		query = status = nullptr;
	}
	bool Sync() {
		if (!root || !preview.IsCreated() || state == SenpDocumentViewState::Closed) return false;
		const auto generation = model.Generation();
		if (queued == generation) return state != SenpDocumentViewState::Failed;
		const auto content = model.State() == SenpDocumentState::Ready ? model.Content() : nullptr;
		const auto statusText = StatusText(model.State());
		std::function<markdown::Document()> prepare;
		if (content) prepare = [content, range = sectionRange] {
			auto result = PrepareSenpReadonlyDocument(*content, range);
			if (result.result != SenpDocumentResult::Accepted) throw std::runtime_error("SENP document preparation failed.");
			return std::move(result.document);
		};
		else prepare = [statusText] { return Notice(statusText); };
		// Generation is a separate key field; never narrow an int64 wire revision.
		if (!preview.QueuePreparedDocument(std::move(prepare), { generation, 0 })) { Close(); return false; }
		queued = generation; state = SenpDocumentViewState::Preparing;
		if (!content) { ::SetWindowTextW(query, L""); ShowFind(false, false); }
		::SetWindowTextW(preview.GetHwnd(), content ? content->title.c_str() : statusText.c_str());
		return true;
	}
};

SenpReadonlyDocumentView::SenpReadonlyDocumentView(SenpReadonlyDocument& model, rendering::FrameSurfaceId id,
	SenpStructuredSectionRange range) : m_impl(std::make_unique<Impl>(model, id, range)) {}
SenpReadonlyDocumentView::SenpReadonlyDocumentView(SenpReadonlyDocument& model, rendering::FrameSurfaceId id)
	: m_impl(std::make_unique<Impl>(model, id, std::nullopt)) {}
SenpReadonlyDocumentView::~SenpReadonlyDocumentView() { m_impl->Close(); }
bool SenpReadonlyDocumentView::Create(HWND parent)
{
	auto& self = *m_impl;
	if (self.root || self.state == SenpDocumentViewState::Closed || !::IsWindow(parent)) return false;
	try {
		self.root = ::CreateWindowExW(WS_EX_CONTROLPARENT, L"STATIC", L"", WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
			0, 0, 1, 1, parent, nullptr, ::GetModuleHandleW(nullptr), nullptr);
		if (!self.root || !::SetWindowSubclass(self.root, Impl::Procedure, 1, reinterpret_cast<DWORD_PTR>(&self))
			|| !self.preview.Create(self.root)) { self.Close(); return false; }
		self.query = ::CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL,
			0, 0, 1, 1, self.root, nullptr, nullptr, nullptr);
		self.status = ::CreateWindowExW(0, L"STATIC", L"", WS_CHILD | SS_LEFTNOWORDWRAP,
			0, 0, 1, 1, self.root, nullptr, nullptr, nullptr);
		if (!self.query || !self.status || !::SetWindowSubclass(self.query, Impl::QueryProcedure, 1, reinterpret_cast<DWORD_PTR>(&self))) { self.Close(); return false; }
		::SendMessageW(self.query, EM_LIMITTEXT, 1024, 0);
		::SendMessageW(self.query, EM_SETCUEBANNER, FALSE, reinterpret_cast<LPARAM>(L"Find in document (Enter / Shift+Enter)"));
		self.preview.SetCopyCommand([this] { (void)Copy(); });
		self.preview.SetFindCallback([this](markdown::PreviewFindAction action) {
			if (action == markdown::PreviewFindAction::Show) { ShowFind(true); return; }
			wchar_t value[1025]{}; ::GetWindowTextW(m_impl->query, value, 1025);
			if (!*value) ShowFind(true); else (void)Find(value, action == markdown::PreviewFindAction::Previous, false);
		});
		self.preview.SetPreparationCallback([&self](markdown::PreviewRenderKey key, bool succeeded) noexcept {
			try {
			if (self.state == SenpDocumentViewState::Closed || self.queued != key.generation) return;
			self.prepared = key.generation;
			self.state = succeeded ? SenpDocumentViewState::Prepared : SenpDocumentViewState::Failed;
			if (!succeeded) {
				// The failed worker generation is terminal. Replace its last-good
				// projection with an explicit failure; no automatic retry is queued.
				self.preview.SetDocument(Notice(L"The document could not be rendered. Refresh to try again."));
				::SetWindowTextW(self.preview.GetHwnd(), L"The document could not be rendered.");
			}
			} catch (...) { self.Close(); }
		});
		self.preview.Show(true); self.LayoutChild(); return self.Sync();
	} catch (...) { self.Close(); return false; }
}
bool SenpReadonlyDocumentView::Sync() { try { return m_impl->Sync(); } catch (...) { m_impl->Close(); return false; } }
void SenpReadonlyDocumentView::SetStyle(const theme::ThemePalette& palette, const LOGFONT& font, unsigned int dpi)
{
	if (m_impl->state == SenpDocumentViewState::Closed) return;
	m_impl->dpi = dpi ? dpi : 96;
	m_impl->palette = palette;
	if (!m_impl->chromeFont.Recreate(theme::ThemeFontKind::Chrome, m_impl->dpi)) { m_impl->Close(); return; }
	for (auto child : { m_impl->query, m_impl->status }) if (child) ::SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(m_impl->chromeFont.Get()), FALSE);
	m_impl->inputLineHeight = controls::MeasureTextLineHeight(m_impl->root, m_impl->chromeFont.Get());
	m_impl->preview.SetPalette(palette); m_impl->preview.SetEditorFont(font, m_impl->dpi); m_impl->LayoutChild();
}
void SenpReadonlyDocumentView::Layout(const RECT& bounds, unsigned int dpi)
{
	if (!m_impl->root || bounds.right < bounds.left || bounds.bottom < bounds.top) return;
	m_impl->dpi = dpi ? dpi : 96;
	if (!::SetWindowPos(m_impl->root, nullptr, bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top,
		SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS)) { m_impl->Close(); return; }
	m_impl->LayoutChild();
}
void SenpReadonlyDocumentView::Show(bool visible) noexcept { if (m_impl->root) ::ShowWindow(m_impl->root, visible ? SW_SHOWNA : SW_HIDE); }
void SenpReadonlyDocumentView::Close() noexcept { m_impl->Close(); }
void SenpReadonlyDocumentView::SelectAll() noexcept { if (m_impl->model.State() == SenpDocumentState::Ready) m_impl->preview.SelectAllText(); }
std::wstring SenpReadonlyDocumentView::SelectedText() const { return m_impl->model.State() == SenpDocumentState::Ready ? m_impl->preview.SelectedText() : std::wstring{}; }
bool SenpReadonlyDocumentView::Copy() {
	try {
		return m_impl->Copy();
	} catch (...) { m_impl->Close(); return false; }
}
void SenpReadonlyDocumentView::SetCopySink(std::function<bool(std::wstring_view)> sink) { m_impl->preview.SetCopySink(std::move(sink)); }
void SenpReadonlyDocumentView::ShowFind(bool visible) { try { m_impl->ShowFind(visible); } catch (...) { m_impl->Close(); } }
markdown::PreviewFindResult SenpReadonlyDocumentView::Find(std::wstring_view query, bool previous, bool matchCase) {
	try { return m_impl->Find(query, previous, matchCase); } catch (...) { m_impl->Close(); return markdown::PreviewFindResult::Unavailable; }
}
HWND SenpReadonlyDocumentView::Window() const noexcept { return m_impl->root; }
HWND SenpReadonlyDocumentView::FocusWindow() const noexcept { return m_impl->preview.GetHwnd(); }
SenpDocumentViewState SenpReadonlyDocumentView::State() const noexcept { return m_impl->state; }
std::uint64_t SenpReadonlyDocumentView::PreparedGeneration() const noexcept { return m_impl->prepared; }
markdown::PreviewViewportSnapshot SenpReadonlyDocumentView::ViewportSnapshot() const noexcept { return m_impl->preview.ViewportSnapshot(); }

} // namespace workbench::editor

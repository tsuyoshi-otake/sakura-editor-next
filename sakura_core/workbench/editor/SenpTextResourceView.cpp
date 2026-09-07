/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#include "StdAfx.h"
#include "workbench/editor/SenpTextResourceView.h"
#include "theme/CThemeService.h"
#include <CommCtrl.h>
#include <Richedit.h>
#include <algorithm>
#include <cwchar>
#include <optional>
#include <stdexcept>

namespace workbench::editor {
namespace {
using namespace senp;
bool ValidEnd(const TextResourceChunk& chunk) noexcept
{
	const bool failure = chunk.end == TextResourceEnd::Failed || chunk.end == TextResourceEnd::Cancelled || chunk.end == TextResourceEnd::LimitExceeded;
	switch (chunk.state) {
	case TextResourceState::Loading: return chunk.end == TextResourceEnd::None;
	case TextResourceState::Complete: return chunk.end == TextResourceEnd::Complete;
	case TextResourceState::Partial: return failure && chunk.length > 0;
	case TextResourceState::Failed: return failure && chunk.length == 0;
	default: return false;
	}
}
bool CopyUnicode(HWND owner, std::wstring_view text)
{
	const auto memory = ::GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1) * sizeof(wchar_t));
	if (!memory) return false;
	auto* target = static_cast<wchar_t*>(::GlobalLock(memory));
	if (!target) { ::GlobalFree(memory); return false; }
	std::copy(text.begin(), text.end(), target); target[text.size()] = 0; ::GlobalUnlock(memory);
	if (!::OpenClipboard(owner)) { ::GlobalFree(memory); return false; }
	const bool success = ::EmptyClipboard() && ::SetClipboardData(CF_UNICODETEXT, memory);
	::CloseClipboard(); if (!success) ::GlobalFree(memory);
	return success;
}
}
struct SenpTextResourceView::Impl {
	TextResourceScope scope;
	std::wstring handle;
	CopySink copy;
	SenpTextResourceDecoder decoder;
	HMODULE richEdit{};
	HWND root{}, text{}, status{}, query{};
	bool closed{}, findVisible{}, nativeFailure{};
	unsigned int dpi{ 96 };
	std::size_t characters{}, knownLength{};
	TextResourceState state{ TextResourceState::Loading };
	TextResourceEnd end{ TextResourceEnd::None };
	struct SourceEnd final { TextResourceState state; TextResourceEnd end; std::size_t length; };
	std::optional<SourceEnd> sourceEnd;
	theme::ThemePalette palette{ theme::CThemeService::PaletteFor(theme::ThemeMode::Dark) };
	theme::CThemeFont editorFont, chromeFont;
	Impl(TextResourceScope scopeValue, std::wstring handleValue, CopySink copyValue)
		: scope(std::move(scopeValue)), handle(std::move(handleValue)), copy(std::move(copyValue)) {
		if (handle.empty() || handle.size() > 512) throw std::invalid_argument("A text view requires a bounded resource handle.");
	}
	~Impl() { Close(); if (richEdit) ::FreeLibrary(richEdit); }
	int Scale(int value) const noexcept { return ::MulDiv(value, dpi, 96); }
	CHARRANGE Selection() const noexcept { CHARRANGE range{}; if (text) ::SendMessageW(text, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&range)); return range; }
	void Status(const wchar_t* message = nullptr) noexcept {
		if (!status) return;
		const wchar_t* sourceStatus{};
		switch (state) {
		case TextResourceState::Loading: sourceStatus = L"Loading log..."; break;
		case TextResourceState::Complete: sourceStatus = characters ? L"Read-only log" : L"The log is empty."; break;
		case TextResourceState::Partial: sourceStatus = end == TextResourceEnd::LimitExceeded ? L"Partial log: the size limit was reached."
			: end == TextResourceEnd::Cancelled ? L"Partial log: loading was cancelled." : L"Partial log: loading failed."; break;
		case TextResourceState::Failed: sourceStatus = L"The log could not be loaded."; break;
		case TextResourceState::Expired: sourceStatus = L"This log is no longer available."; break;
		default: sourceStatus = L"The log is closed."; break;
		}
		// Search/copy feedback must never hide loading or partial-source warnings.
		wchar_t combined[512]{};
		if (message && ::swprintf_s(combined, L"%ls  %ls", sourceStatus, message) > 0) sourceStatus = combined;
		::SetWindowTextW(status, sourceStatus);
	}
	void LayoutChildren() noexcept {
		if (!root || !text) return;
		RECT bounds{}; ::GetClientRect(root, &bounds);
		const int width = bounds.right, height = bounds.bottom;
		const int findHeight = findVisible ? (std::min)(height, Scale(30)) : 0;
		const int statusHeight = (std::min)(height - findHeight, Scale(22));
		constexpr UINT flags = SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS | SWP_NOREDRAW;
		if (!::SetWindowPos(query, nullptr, Scale(6), Scale(3), (std::max)(0, width - Scale(12)), (std::max)(0, findHeight - Scale(6)), flags)
			|| !::SetWindowPos(text, nullptr, 0, findHeight, width, (std::max)(0, height - statusHeight - findHeight), flags)
			|| !::SetWindowPos(status, nullptr, Scale(6), height - statusHeight, (std::max)(0, width - Scale(12)), statusHeight, flags)) { Close(); return; }
		::RedrawWindow(root, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
	}
	void Close() noexcept {
		closed = true; state = TextResourceState::Closed; decoder.Close(); characters = 0;
		if (root) { auto window = root; root = nullptr; ::DestroyWindow(window); }
		text = status = query = nullptr;
	}
	void Fail(TextResourceEnd reason) noexcept {
		if (closed || state != TextResourceState::Loading) return;
		state = characters ? TextResourceState::Partial : TextResourceState::Failed;
		end = reason == TextResourceEnd::Cancelled || reason == TextResourceEnd::LimitExceeded ? reason : TextResourceEnd::Failed;
		decoder.Close(); Status();
	}
	void Expire() noexcept {
		if (closed) return;
		decoder.Close(); state = TextResourceState::Expired; end = TextResourceEnd::Revoked; characters = 0;
		if (text) ::SetWindowTextW(text, L"");
		if (query) ::SetWindowTextW(query, L"");
		Status();
	}
	std::wstring SelectedText(bool currentLine) const {
		if (!text || closed || state == TextResourceState::Expired) return {};
		auto range = Selection(); bool lineCopy = false;
		if (range.cpMin == range.cpMax && currentLine) {
			const auto line = ::SendMessageW(text, EM_EXLINEFROMCHAR, 0, range.cpMin);
			range.cpMin = static_cast<LONG>(::SendMessageW(text, EM_LINEINDEX, line, 0));
			const auto next = ::SendMessageW(text, EM_LINEINDEX, line + 1, 0);
			range.cpMax = next >= 0 ? static_cast<LONG>(next) : static_cast<LONG>(characters); lineCopy = true;
		}
		if (range.cpMin < 0 || range.cpMax < range.cpMin || static_cast<std::size_t>(range.cpMax) > characters) return {};
		std::wstring result(static_cast<std::size_t>(range.cpMax - range.cpMin) + 1, L'\0');
		TEXTRANGEW request{ range, result.data() };
		const auto copied = ::SendMessageW(text, EM_GETTEXTRANGE, 0, reinterpret_cast<LPARAM>(&request));
		if (copied < 0 || static_cast<std::size_t>(copied) >= result.size()) return {};
		result.resize(static_cast<std::size_t>(copied));
		// Native text uses CR internally. Expose LF like the decoder and provider.
		std::replace(result.begin(), result.end(), L'\r', L'\n');
		if (lineCopy && (result.empty() || result.back() != L'\n')) result += L'\n';
		return result;
	}
	bool Copy() {
		if (!text || closed || state == TextResourceState::Expired) return false;
		const auto value = SelectedText(true);
		const bool success = copy ? copy(value) : CopyUnicode(text, value);
		if (!success) Status(L"The selection could not be copied. Try again.");
		return success;
	}
	void SelectAll() noexcept { if (text) { CHARRANGE range{ 0, -1 }; ::SendMessageW(text, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&range)); } }
	void ShowFind(bool visible) {
		if (!root || closed) return;
		findVisible = visible; ::ShowWindow(query, visible ? SW_SHOWNA : SW_HIDE); LayoutChildren();
		if (visible) { ::SetFocus(query); ::SendMessageW(query, EM_SETSEL, 0, -1); }
		else if (text) ::SetFocus(text);
	}
	SenpTextFindResult Find(std::wstring_view value, bool previous, bool matchCase) {
		if (!text || closed || state == TextResourceState::Expired) return SenpTextFindResult::Closed;
		if (value.empty() || value.size() > 1024 || value.find_first_of(L"\r\n") != std::wstring_view::npos
			|| value.find(L'\0') != std::wstring_view::npos) return SenpTextFindResult::Invalid;
		const std::wstring queryValue(value); const auto selected = Selection();
		wchar_t currentQuery[1025]{}; ::GetWindowTextW(query, currentQuery, 1025);
		if (value != currentQuery) ::SetWindowTextW(query, queryValue.c_str());
		FINDTEXTEXW request{}; request.lpstrText = queryValue.c_str();
		request.chrg = previous ? CHARRANGE{ selected.cpMin, 0 } : CHARRANGE{ selected.cpMax, -1 };
		const auto flags = (previous ? 0 : FR_DOWN) | (matchCase ? FR_MATCHCASE : 0);
		bool wrapped = false;
		if (::SendMessageW(text, EM_FINDTEXTEXW, flags, reinterpret_cast<LPARAM>(&request)) < 0) {
			request.chrg = previous ? CHARRANGE{ static_cast<LONG>(characters), selected.cpMin } : CHARRANGE{ 0, selected.cpMax };
			if (::SendMessageW(text, EM_FINDTEXTEXW, flags, reinterpret_cast<LPARAM>(&request)) < 0) {
				Status(L"No results in the loaded text."); return SenpTextFindResult::NotFound;
			}
			wrapped = true;
		}
		::SendMessageW(text, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&request.chrgText)); ::SendMessageW(text, EM_SCROLLCARET, 0, 0);
		Status(wrapped ? L"Search wrapped in the loaded text." : L"Match in the loaded text.");
		return wrapped ? SenpTextFindResult::Wrapped : SenpTextFindResult::Found;
	}
	void FindCurrent(bool previous) {
		wchar_t value[1025]{}; ::GetWindowTextW(query, value, 1025); (void)Find(value, previous, false);
	}
	static LRESULT CALLBACK ChildProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR context) {
		auto& self = *reinterpret_cast<Impl*>(context);
		try {
			const bool body = window == self.text;
			if (message == WM_NCDESTROY) ::RemoveWindowSubclass(window, ChildProcedure, 1);
			if (body && (message == WM_PASTE || message == WM_CUT || message == WM_CLEAR || message == WM_UNDO || message == EM_REDO)) return 0;
			if (body && message == WM_COPY) return self.Copy() ? 1 : 0;
			if (message == WM_KEYDOWN) {
				const bool control = (::GetKeyState(VK_CONTROL) & 0x8000) != 0, shift = (::GetKeyState(VK_SHIFT) & 0x8000) != 0;
				if (body && control && wParam == 'A') { self.SelectAll(); return 0; }
				if (body && control && wParam == 'C') { (void)self.Copy(); return 0; }
				if (control && wParam == 'F') { self.ShowFind(true); return 0; }
				if (wParam == VK_F3 || (!body && wParam == VK_RETURN)) { self.FindCurrent(shift); return 0; }
				if (wParam == VK_ESCAPE && self.findVisible) { self.ShowFind(false); return 0; }
			}
			if (message == WM_CHAR && (body || wParam == VK_RETURN || wParam == VK_ESCAPE)) return 0;
		} catch (...) { self.Close(); return 0; }
		return ::DefSubclassProc(window, message, wParam, lParam);
	}
	static LRESULT CALLBACK RootProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR context) {
		auto& self = *reinterpret_cast<Impl*>(context);
		switch (message) {
		case WM_SIZE: self.LayoutChildren(); return 0;
		case WM_SETFOCUS: if (self.text) ::SetFocus(self.text); return 0;
		case WM_COMMAND:
			if (reinterpret_cast<HWND>(lParam) == self.text && (HIWORD(wParam) == EN_ERRSPACE || HIWORD(wParam) == EN_MAXTEXT)) self.nativeFailure = true;
			break;
		case WM_CTLCOLOREDIT: case WM_CTLCOLORSTATIC:
			::SetBkColor(reinterpret_cast<HDC>(wParam), self.palette.canvas.ToColorRef());
			::SetTextColor(reinterpret_cast<HDC>(wParam), self.palette.primaryText.ToColorRef());
			::SetDCBrushColor(reinterpret_cast<HDC>(wParam), self.palette.canvas.ToColorRef());
			return reinterpret_cast<LRESULT>(::GetStockObject(DC_BRUSH));
		case WM_ERASEBKGND: {
			RECT bounds{}; ::GetClientRect(window, &bounds); ::SetDCBrushColor(reinterpret_cast<HDC>(wParam), self.palette.canvas.ToColorRef());
			::FillRect(reinterpret_cast<HDC>(wParam), &bounds, static_cast<HBRUSH>(::GetStockObject(DC_BRUSH))); return 1;
		}
		case WM_PAINT: {
			PAINTSTRUCT paint{}; const auto dc = ::BeginPaint(window, &paint);
			::SetDCBrushColor(dc, self.palette.canvas.ToColorRef());
			::FillRect(dc, &paint.rcPaint, static_cast<HBRUSH>(::GetStockObject(DC_BRUSH)));
			::EndPaint(window, &paint); return 0;
		}
		case WM_NCDESTROY:
			self.root = self.text = self.status = self.query = nullptr; self.closed = true;
			self.state = TextResourceState::Closed; self.decoder.Close(); self.characters = 0;
			::RemoveWindowSubclass(window, RootProcedure, 1); break;
		default: break;
		}
		return ::DefSubclassProc(window, message, wParam, lParam);
	}
	SenpTextViewResult Apply(const TextResourceScope& current, const TextResourceChunk& chunk) {
		if (closed || !text) return SenpTextViewResult::Closed;
		if (current != scope || chunk.handle != handle || chunk.revision != scope.revision) return SenpTextViewResult::Stale;
		if (chunk.result == TextResourceResult::Expired && chunk.state == TextResourceState::Expired
			&& chunk.end == TextResourceEnd::Revoked && chunk.bytes.empty() && chunk.length == 0) { Expire(); return SenpTextViewResult::Applied; }
		if (state == TextResourceState::Expired || decoder.IsClosed()) return SenpTextViewResult::Stale;
		if (chunk.result != TextResourceResult::Accepted || !ValidEnd(chunk) || chunk.length > SenpTextResourceStore::kResourceBytes
			|| chunk.bytes.size() > SenpTextResourceStore::kChunkBytes || chunk.length < knownLength || chunk.offset > chunk.length
			|| chunk.bytes.size() > chunk.length - chunk.offset || (chunk.bytes.empty() && chunk.offset < chunk.length)) return SenpTextViewResult::Invalid;
		if (chunk.offset != decoder.Offset()) return SenpTextViewResult::Stale;
		if (sourceEnd && (sourceEnd->state != chunk.state || sourceEnd->end != chunk.end || sourceEnd->length != chunk.length)) return SenpTextViewResult::Stale;
		const bool terminal = chunk.state != TextResourceState::Loading;
		const bool final = terminal && chunk.offset + chunk.bytes.size() == chunk.length;
		auto decoded = decoder.Decode(chunk.offset, chunk.bytes, final);
		if (decoded.result != TextDecodeResult::Accepted) { Fail(TextResourceEnd::Failed); Status(L"The log contains invalid or incomplete UTF-8."); return SenpTextViewResult::DecodeFailed; }
		if (terminal) sourceEnd = SourceEnd{ chunk.state, chunk.end, chunk.length };
		knownLength = chunk.length;
		if (!decoded.text.empty()) {
			const auto selection = Selection(); POINT scroll{}; ::SendMessageW(text, EM_GETSCROLLPOS, 0, reinterpret_cast<LPARAM>(&scroll));
			// WM_SETREDRAW changes the child visibility bit; preserve that bit
			// independently of whether the retained parent is currently hidden.
			const bool visible = (::GetWindowLongPtrW(text, GWL_STYLE) & WS_VISIBLE) != 0;
			if (visible) ::SendMessageW(text, WM_SETREDRAW, FALSE, 0);
			CHARRANGE append{ static_cast<LONG>(characters), static_cast<LONG>(characters) };
			::SendMessageW(text, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&append));
			nativeFailure = false; ::SendMessageW(text, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(decoded.text.c_str()));
			::SendMessageW(text, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&selection));
			::SendMessageW(text, EM_SETSCROLLPOS, 0, reinterpret_cast<LPARAM>(&scroll));
			::SendMessageW(text, EM_SETMODIFY, FALSE, 0);
			if (visible) ::SendMessageW(text, WM_SETREDRAW, TRUE, 0);
			// Resuming redraw must include the native scrollbars/nonclient area.
			::RedrawWindow(text, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
			// Native allocation failure may have inserted only part of this chunk.
			// No trustworthy character/index checkpoint remains: erase the surface
			// by closing it and let composition present the explicit failed result.
			if (closed || !text) return SenpTextViewResult::Closed;
			if (nativeFailure) { Close(); return SenpTextViewResult::Failed; }
			characters += decoded.text.size();
		}
		state = final ? chunk.state : TextResourceState::Loading; end = final ? chunk.end : TextResourceEnd::None; Status();
		return SenpTextViewResult::Applied;
	}
};

SenpTextResourceView::SenpTextResourceView(TextResourceScope scope, std::wstring handle, CopySink copy)
	: m_impl(std::make_unique<Impl>(std::move(scope), std::move(handle), std::move(copy))) {}
SenpTextResourceView::~SenpTextResourceView() = default;
bool SenpTextResourceView::Create(HWND parent)
{
	auto& self = *m_impl;
	if (self.closed || self.root || !::IsWindow(parent)) return false;
	self.richEdit = ::LoadLibraryExW(L"Msftedit.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
	if (!self.richEdit) { self.Close(); return false; }
	self.root = ::CreateWindowExW(WS_EX_CONTROLPARENT, L"STATIC", L"Read-only log", WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
		0, 0, 1, 1, parent, nullptr, ::GetModuleHandleW(nullptr), nullptr);
	if (!self.root || !::SetWindowSubclass(self.root, Impl::RootProcedure, 1, reinterpret_cast<DWORD_PTR>(&self))) { self.Close(); return false; }
	self.text = ::CreateWindowExW(0, MSFTEDIT_CLASS, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | WS_HSCROLL
		| ES_MULTILINE | ES_AUTOVSCROLL | ES_AUTOHSCROLL | ES_READONLY | ES_NOHIDESEL, 0, 0, 1, 1, self.root, nullptr, self.richEdit, nullptr);
	self.status = ::CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP, 0, 0, 1, 1, self.root, nullptr, nullptr, nullptr);
	self.query = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 1, 1, self.root, nullptr, nullptr, nullptr);
	if (!self.text || !self.status || !self.query || !::SetWindowSubclass(self.text, Impl::ChildProcedure, 1, reinterpret_cast<DWORD_PTR>(&self))
		|| !::SetWindowSubclass(self.query, Impl::ChildProcedure, 1, reinterpret_cast<DWORD_PTR>(&self))
		|| ::SendMessageW(self.text, EM_SETTEXTMODE, TM_PLAINTEXT | TM_MULTICODEPAGE, 0) != 0) { self.Close(); return false; }
	::SendMessageW(self.text, EM_EXLIMITTEXT, 0, static_cast<LPARAM>(SenpTextResourceStore::kResourceBytes));
	::SendMessageW(self.text, EM_SETUNDOLIMIT, 0, 0); ::SendMessageW(self.text, EM_AUTOURLDETECT, 0, 0);
	::SendMessageW(self.text, EM_SETEVENTMASK, 0, ENM_NONE);
	::SendMessageW(self.query, EM_LIMITTEXT, 1024, 0);
	::SendMessageW(self.query, EM_SETCUEBANNER, FALSE, reinterpret_cast<LPARAM>(L"Find in log (Enter / Shift+Enter)"));
	SetStyle(self.palette, self.dpi); self.Status(); self.LayoutChildren(); return !self.closed;
}
SenpTextViewResult SenpTextResourceView::Apply(const TextResourceScope& scope, const TextResourceChunk& chunk)
{
	try { return m_impl->Apply(scope, chunk); } catch (...) { m_impl->Close(); return SenpTextViewResult::Failed; }
}
void SenpTextResourceView::Fail(TextResourceEnd reason) noexcept { m_impl->Fail(reason); }
void SenpTextResourceView::Expire() noexcept { m_impl->Expire(); }
void SenpTextResourceView::Close() noexcept { m_impl->Close(); }
void SenpTextResourceView::Layout(const RECT& bounds, unsigned int dpi)
{
	auto& self = *m_impl; if (!self.root || bounds.right < bounds.left || bounds.bottom < bounds.top) return;
	self.dpi = dpi ? dpi : 96;
	if (!::SetWindowPos(self.root, nullptr, bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top,
		SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS)) self.Close();
	else self.LayoutChildren();
}
void SenpTextResourceView::SetStyle(const theme::ThemePalette& palette, unsigned int dpi)
{
	auto& self = *m_impl; if (self.closed) return; self.palette = palette; self.dpi = dpi ? dpi : 96;
	if (!self.editorFont.Recreate(theme::ThemeFontKind::Editor, self.dpi) || !self.chromeFont.Recreate(theme::ThemeFontKind::Chrome, self.dpi)) { self.Close(); return; }
	if (self.text) {
		::SendMessageW(self.text, WM_SETFONT, reinterpret_cast<WPARAM>(self.editorFont.Get()), FALSE);
		::SendMessageW(self.status, WM_SETFONT, reinterpret_cast<WPARAM>(self.chromeFont.Get()), FALSE);
		::SendMessageW(self.query, WM_SETFONT, reinterpret_cast<WPARAM>(self.chromeFont.Get()), FALSE);
		::SendMessageW(self.text, EM_SETBKGNDCOLOR, 0, palette.canvas.ToColorRef());
		CHARFORMAT2W format{}; format.cbSize = sizeof(format); format.dwMask = CFM_COLOR; format.crTextColor = palette.primaryText.ToColorRef();
		::SendMessageW(self.text, EM_SETCHARFORMAT, SCF_ALL, reinterpret_cast<LPARAM>(&format));
		::SendMessageW(self.text, EM_SETMODIFY, FALSE, 0); self.LayoutChildren();
	}
}
void SenpTextResourceView::ShowFind(bool visible) { m_impl->ShowFind(visible); }
SenpTextFindResult SenpTextResourceView::Find(std::wstring_view query, bool previous, bool matchCase)
{
	try { return m_impl->Find(query, previous, matchCase); } catch (...) { m_impl->Close(); return SenpTextFindResult::Closed; }
}
void SenpTextResourceView::SelectAll() noexcept { m_impl->SelectAll(); }
bool SenpTextResourceView::Copy() { try { return m_impl->Copy(); } catch (...) { m_impl->Close(); return false; } }
std::wstring SenpTextResourceView::SelectedText(bool currentLineIfEmpty) const { return m_impl->SelectedText(currentLineIfEmpty); }
HWND SenpTextResourceView::Window() const noexcept { return m_impl->root; }
HWND SenpTextResourceView::FocusWindow() const noexcept { return m_impl->text; }
SenpTextViewport SenpTextResourceView::Viewport() const noexcept
{
	const auto& self = *m_impl; const auto range = self.Selection(); POINT scroll{};
	if (self.text) ::SendMessageW(self.text, EM_GETSCROLLPOS, 0, reinterpret_cast<LPARAM>(&scroll));
	return { self.state, self.end, self.decoder.Offset(), self.characters, range.cpMin, range.cpMax, scroll.x, scroll.y, self.findVisible };
}

} // namespace workbench::editor

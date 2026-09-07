/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#include "StdAfx.h"
#include "workbench/editor/SenpReadonlyDocumentView.h"
#include "markdown/CMarkdownPreviewWnd.h"
#include <CommCtrl.h>
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
struct SenpReadonlyDocumentView::Impl {
	SenpReadonlyDocument& model;
	markdown::CMarkdownPreviewWnd preview;
	HWND root{};
	unsigned int dpi{ 96 };
	SenpDocumentViewState state{ SenpDocumentViewState::Unavailable };
	std::optional<std::uint64_t> queued;
	std::uint64_t prepared{};
	Impl(SenpReadonlyDocument& value, rendering::FrameSurfaceId surfaceId) : model(value), preview({}, surfaceId)
	{
		if (!surfaceId) throw std::invalid_argument("A SENP document needs a distinct surface identity.");
	}
	void LayoutChild() {
		if (!root || !preview.IsCreated()) return;
		RECT bounds{}; ::GetClientRect(root, &bounds); preview.Layout(bounds, dpi);
	}
	static LRESULT CALLBACK Procedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR context) {
		auto& self = *reinterpret_cast<Impl*>(context);
		try {
			switch (message) {
			case WM_SIZE: self.LayoutChild(); return 0;
			case WM_SETFOCUS: if (self.preview.IsCreated()) ::SetFocus(self.preview.GetHwnd()); return 0;
			case WM_NCDESTROY:
				self.preview.SetPreparationCallback({}); self.root = nullptr;
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
	}
	bool Sync() {
		if (!root || !preview.IsCreated() || state == SenpDocumentViewState::Closed) return false;
		const auto generation = model.Generation();
		if (queued == generation) return state != SenpDocumentViewState::Failed;
		const auto content = model.State() == SenpDocumentState::Ready ? model.Content() : nullptr;
		const auto status = StatusText(model.State());
		std::function<markdown::Document()> prepare;
		if (content) prepare = [content] {
			auto result = PrepareSenpReadonlyDocument(*content);
			if (result.result != SenpDocumentResult::Accepted) throw std::runtime_error("SENP document preparation failed.");
			return std::move(result.document);
		};
		else prepare = [status] { return Notice(status); };
		// Generation is a separate key field; never narrow an int64 wire revision.
		if (!preview.QueuePreparedDocument(std::move(prepare), { generation, 0 })) { Close(); return false; }
		queued = generation; state = SenpDocumentViewState::Preparing;
		::SetWindowTextW(preview.GetHwnd(), content ? content->title.c_str() : status.c_str());
		return true;
	}
};

SenpReadonlyDocumentView::SenpReadonlyDocumentView(SenpReadonlyDocument& model, rendering::FrameSurfaceId id)
	: m_impl(std::make_unique<Impl>(model, id)) {}
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
HWND SenpReadonlyDocumentView::Window() const noexcept { return m_impl->root; }
HWND SenpReadonlyDocumentView::FocusWindow() const noexcept { return m_impl->preview.GetHwnd(); }
SenpDocumentViewState SenpReadonlyDocumentView::State() const noexcept { return m_impl->state; }
std::uint64_t SenpReadonlyDocumentView::PreparedGeneration() const noexcept { return m_impl->prepared; }
markdown::PreviewViewportSnapshot SenpReadonlyDocumentView::ViewportSnapshot() const noexcept { return m_impl->preview.ViewportSnapshot(); }

} // namespace workbench::editor

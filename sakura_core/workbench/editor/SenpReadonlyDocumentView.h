/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#pragma once

#include <windows.h>
#include "workbench/editor/SenpReadonlyDocument.h"
#include "workbench/rendering/FrameSurfaceAdapter.h"
#include "markdown/MarkdownPreviewScrollMap.h"

namespace theme { struct ThemePalette; }
namespace workbench::editor {

enum class SenpDocumentViewState : std::uint8_t { Unavailable, Preparing, Prepared, Failed, Closed };
//! Native projection of one retained input. Owns its root, preview and preview
//! worker. Composition calls Sync after model transitions and owns cancellation.
//! Bind the root (not the preview child) to SenpEditorSurfaceSwitcher so the
//! preview's sibling overlay shares the same visibility/lifetime boundary.
class SenpReadonlyDocumentView final {
public:
	SenpReadonlyDocumentView(SenpReadonlyDocument& model, rendering::FrameSurfaceId surfaceId);
	~SenpReadonlyDocumentView();
	SenpReadonlyDocumentView(const SenpReadonlyDocumentView&) = delete;
	SenpReadonlyDocumentView& operator=(const SenpReadonlyDocumentView&) = delete;
	[[nodiscard]] bool Create(HWND parent);
	[[nodiscard]] bool Sync();
	void SetStyle(const theme::ThemePalette& palette, const LOGFONT& font, unsigned int dpi);
	void Layout(const RECT& bounds, unsigned int dpi);
	void Show(bool visible) noexcept;
	void Close() noexcept;
	[[nodiscard]] HWND Window() const noexcept;
	[[nodiscard]] HWND FocusWindow() const noexcept;
	[[nodiscard]] SenpDocumentViewState State() const noexcept;
	[[nodiscard]] std::uint64_t PreparedGeneration() const noexcept;
	[[nodiscard]] markdown::PreviewViewportSnapshot ViewportSnapshot() const noexcept;
private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};

} // namespace workbench::editor

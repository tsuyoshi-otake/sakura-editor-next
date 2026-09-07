/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#pragma once

#include <windows.h>
#include <functional>
#include "senp/SenpTextResource.h"

namespace theme { struct ThemePalette; }
namespace workbench::editor {

enum class SenpTextViewResult : std::uint8_t { Applied, Stale, Invalid, DecodeFailed, Failed, Closed };
enum class SenpTextFindResult : std::uint8_t { Found, Wrapped, NotFound, Invalid, Closed };
struct SenpTextViewport final {
	senp::TextResourceState state{ senp::TextResourceState::Loading };
	senp::TextResourceEnd end{ senp::TextResourceEnd::None };
	std::size_t byteOffset{}, characters{};
	long selectionStart{}, selectionEnd{}, scrollX{}, scrollY{};
	bool findVisible{};
};

//! Native readonly, plain-Unicode text projection. Rich Edit owns incremental
//! text storage/indexing, character selection and accessibility. No RTF, URLs,
//! OLE content, source paths, network work or extension callbacks enter it.
//! Composition authorizes and reads one bounded chunk outside paint, calls
//! Apply, then stops polling on a terminal/error. Expire erases private text.
//! Bind the root to the retained Editor surface switcher; unbind before Close.
class SenpTextResourceView final {
public:
	//! Optional clipboard sink for deterministic tests; defaults to the OS clipboard.
	using CopySink = std::function<bool(std::wstring_view)>;
	SenpTextResourceView(senp::TextResourceScope scope, std::wstring handle, CopySink copy = {});
	~SenpTextResourceView();
	SenpTextResourceView(const SenpTextResourceView&) = delete;
	SenpTextResourceView& operator=(const SenpTextResourceView&) = delete;
	[[nodiscard]] bool Create(HWND parent);
	[[nodiscard]] SenpTextViewResult Apply(const senp::TextResourceScope& scope, const senp::TextResourceChunk& chunk);
	void Fail(senp::TextResourceEnd reason = senp::TextResourceEnd::Failed) noexcept;
	void Expire() noexcept;
	void Close() noexcept;
	void Layout(const RECT& bounds, unsigned int dpi);
	void SetStyle(const theme::ThemePalette& palette, unsigned int dpi);
	void ShowFind(bool visible);
	[[nodiscard]] SenpTextFindResult Find(std::wstring_view query, bool previous = false, bool matchCase = false);
	void SelectAll() noexcept;
	[[nodiscard]] bool Copy();
	[[nodiscard]] std::wstring SelectedText(bool currentLineIfEmpty = false) const;
	[[nodiscard]] HWND Window() const noexcept;
	[[nodiscard]] HWND FocusWindow() const noexcept;
	[[nodiscard]] SenpTextViewport Viewport() const noexcept;
private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};

} // namespace workbench::editor

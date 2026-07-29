/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "terminal/session/TerminalSession.h"
#include "theme/CThemeService.h"

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <vector>

namespace terminal {

class SakuraTerminalInputAdapter;
class TerminalModel;

//! Native GDI terminal viewport. It owns no session or parser.
class CTerminalWnd final {
public:
	using InputSink = std::function<void(std::span<const std::uint8_t> bytes)>;
	using ResizeSink = std::function<void(TerminalSize size)>;

	CTerminalWnd();
	~CTerminalWnd();
	CTerminalWnd( const CTerminalWnd& ) = delete;
	CTerminalWnd& operator=( const CTerminalWnd& ) = delete;

	[[nodiscard]] bool Create( HWND parent, HINSTANCE instance );
	void Layout( const RECT& bounds, unsigned int dpi );
	void SetModel( TerminalModel* model );
	void SetInputAdapter( SakuraTerminalInputAdapter* inputAdapter );
	void SetInputSink( InputSink sink );
	void SetResizeSink( ResizeSink sink );
	void SetPalette( const theme::ThemePalette& palette );
	void InvalidateDirtyRows( const std::vector<std::size_t>& dirtyScreenRows );
	void InvalidateAll();
	[[nodiscard]] bool PreTranslateMessage( MSG& message );
	void Focus();
	void Close() noexcept;

	[[nodiscard]] HWND GetHwnd() const noexcept;
	[[nodiscard]] TerminalSize GetTerminalSize() const noexcept;
	[[nodiscard]] bool HasSelection() const noexcept;
	[[nodiscard]] bool CopySelectionToClipboard();
	[[nodiscard]] bool PasteFromClipboard();

	static LRESULT CALLBACK WindowProc( HWND window, UINT message, WPARAM wParam, LPARAM lParam );

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};

} // namespace terminal

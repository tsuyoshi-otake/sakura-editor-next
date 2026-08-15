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
#include <string>
#include <vector>

namespace terminal {

class SakuraTerminalInputAdapter;
class TerminalModel;

//! Native GDI terminal viewport. It owns no session or parser.
class CTerminalWnd final {
public:
	// The renderer retains interactive input when the bounded session queue is
	// full. Returning the result makes that pressure visible instead of silently
	// losing a key, paste, mouse report, or IME commit.
	using InputSink = std::function<TerminalQueueInputResult(std::span<const std::uint8_t> bytes)>;
	using ResizeSink = std::function<void(TerminalSize size)>;
	//! Called after this native viewport becomes the focused terminal pane.
	//! The workbench owns session selection; the renderer only reports focus.
	using FocusSink = std::function<void()>;
	using ImeResultReader = std::function<bool(HWND window, std::wstring& result)>;

	CTerminalWnd();
	explicit CTerminalWnd( ImeResultReader imeResultReader );
	~CTerminalWnd();
	CTerminalWnd( const CTerminalWnd& ) = delete;
	CTerminalWnd& operator=( const CTerminalWnd& ) = delete;

	[[nodiscard]] bool Create( HWND parent, HINSTANCE instance );
	void Layout( const RECT& bounds, unsigned int dpi );
	void SetModel( TerminalModel* model );
	void SetInputAdapter( SakuraTerminalInputAdapter* inputAdapter );
	void SetInputSink( InputSink sink );
	void SetResizeSink( ResizeSink sink );
	void SetFocusSink( FocusSink sink );
	//! Drops input and IME state owned by the previously bound session. This must
	//! be called before a renderer is rebound across a workspace/session boundary.
	void ResetSessionInputState() noexcept;
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

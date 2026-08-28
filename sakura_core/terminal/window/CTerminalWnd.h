/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "terminal/session/TerminalSession.h"
#include "terminal/window/TerminalNativeFrameBridge.h"
#include "terminal/window/TerminalSurfaceAdapter.h"
#include "theme/CThemeService.h"

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace terminal {

class SakuraTerminalInputAdapter;
class TerminalModel;
struct TerminalScrollbackChange;

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
	using FrameSurfaceId = TerminalSurfaceAdapter::SurfaceId;
	using FrameSurfaceResult = TerminalSurfaceAdapter::Result;
	using FrameSurfaceSnapshotType = TerminalSurfaceAdapter::SnapshotType;
	using NativeFrame = workbench::rendering::FrameNativeSurfaceFrame;

	CTerminalWnd();
	explicit CTerminalWnd( ImeResultReader imeResultReader );
	explicit CTerminalWnd( FrameSurfaceId surfaceId );
	CTerminalWnd( FrameSurfaceId surfaceId, ImeResultReader imeResultReader );
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
	//! Applies one bounded-history coordinate mutation before repaint damage is
	//! mapped. A live-bottom viewport follows output; a scrolled viewport remains
	//! anchored until its retained content is actually discarded.
	void ApplyScrollbackChange( const TerminalScrollbackChange& change, bool invalidate = true );
	void InvalidateDirtyRows( const std::vector<std::size_t>& dirtyScreenRows );
	void InvalidateAll();
	//! Changes the logical frame host without tying publication to an HWND.
	[[nodiscard]] FrameSurfaceResult SetFrameHost( std::string_view hostId ) noexcept;
	//! Fences a visible/hidden transition while retaining the terminal model.
	[[nodiscard]] FrameSurfaceResult SetFrameVisible( bool visible ) noexcept;
	//! Announces one bounded model-drain publication. Bytes never pass through
	//! this surface; the terminal model remains the lossless source of truth.
	[[nodiscard]] FrameSurfaceResult NotifyFrameContent() noexcept;
	//! Announces a geometry/theme/layout boundary.
	[[nodiscard]] FrameSurfaceResult NotifyFrameLayout() noexcept;
	//! Fences a lost/recreated rendering device domain.
	[[nodiscard]] FrameSurfaceResult NotifyFrameDeviceEpoch(std::uint64_t epoch) noexcept;
	//! Commits only after the enclosing owner has reached its post-paint GDI
	//! boundary. This call does not flush GDI or wait for UI work.
	[[nodiscard]] std::optional<FrameSurfaceSnapshotType> CommitGdiFrame() noexcept;
	[[nodiscard]] FrameSurfaceSnapshotType FrameSurfaceSnapshot() const noexcept;
	[[nodiscard]] FrameSurfaceSnapshotType GetFrameSurfaceSnapshot() const noexcept;
	[[nodiscard]] FrameSurfaceId GetFrameSurfaceId() const noexcept;
	[[nodiscard]] FrameSurfaceId StableSurfaceId() const noexcept;
	[[nodiscard]] bool HasPendingFrame() const noexcept;
	//! Binds the optional frame-runtime bridge. Registration and update callbacks
	//! are UI-thread mailbox operations; native payload conversion stays on the
	//! bounded terminal worker.
	void SetNativeFrameBridge(TerminalNativeFrameBridgePtr bridge) noexcept;
	void SetNativeFrameReadySink(std::function<void()> sink) noexcept;
	[[nodiscard]] std::shared_ptr<const NativeFrame> TakeNativeFrame() noexcept;
	[[nodiscard]] bool PreTranslateMessage( MSG& message );
	void Focus();
	void Close() noexcept;

	[[nodiscard]] HWND GetHwnd() const noexcept;
	[[nodiscard]] TerminalSize GetTerminalSize() const noexcept;
	[[nodiscard]] TerminalViewportDiagnosticSnapshot GetViewportDiagnostic() const noexcept;
	[[nodiscard]] bool HasSelection() const noexcept;
	[[nodiscard]] bool CopySelectionToClipboard();
	[[nodiscard]] bool PasteFromClipboard();

	static LRESULT CALLBACK WindowProc( HWND window, UINT message, WPARAM wParam, LPARAM lParam );

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};

} // namespace terminal

/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "terminal/window/TerminalRenderPlan.h"

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace terminal {

//! A non-owning, logical-top-left view of a 32-bit BGRX pixel surface.
//!
//! `pixels` addresses logical row zero.  A positive stride advances through a
//! top-down DIB; a negative stride is also accepted for callers whose logical
//! rows descend in memory.  DrawBatch clips every raw store to `clip` and to
//! the declared [0, width) x [0, height) surface bounds.
struct TerminalBuiltinGlyphPixelSurface {
	std::uint32_t* pixels{};
	std::ptrdiff_t stridePixels{};
	LONG width{};
	LONG height{};
	RECT clip{};
};

//! Rasterizes terminal box/block/shade glyphs from integer cell geometry.  It
//! never selects a font or asks GDI/DirectWrite for an outline.
class TerminalBuiltinGlyphRenderer final {
public:
	TerminalBuiltinGlyphRenderer() noexcept;
	~TerminalBuiltinGlyphRenderer() noexcept;

	TerminalBuiltinGlyphRenderer(const TerminalBuiltinGlyphRenderer&) = delete;
	TerminalBuiltinGlyphRenderer& operator=(const TerminalBuiltinGlyphRenderer&) = delete;

	[[nodiscard]] static bool Handles(char32_t glyph) noexcept;
	static void Draw(HDC dc, const TerminalBuiltinGlyphCommand& command) noexcept;
	static void Draw(HDC dc, char32_t glyph, const RECT& cellRect, COLORREF foreground) noexcept;
	//! Writes deterministic glyph pixels directly into a checked BGRX surface.
	//! The batch is completely preflighted before its first store, so false
	//! means the caller can safely use the HDC fallback without a partial frame.
	[[nodiscard]] static bool DrawBatch(const TerminalBuiltinGlyphPixelSurface& surface,
		std::span<const TerminalBuiltinGlyphCommand> commands) noexcept;

	//! Batches adjacent, identical-style deterministic glyphs without changing
	//! their geometry or the surrounding HDC state.  Scratch is allocated only
	//! after a built-in glyph is actually painted and has a fixed upper bound.
	void DrawBatch(HDC dc, std::span<const TerminalBuiltinGlyphCommand> commands) noexcept;

private:
	struct BatchState;
	std::unique_ptr<BatchState> m_batchState;
};

} // namespace terminal

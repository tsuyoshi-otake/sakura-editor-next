/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <Windows.h>

namespace workbench::rendering {

//! Persistent GDI composition target for one UI-owned surface.
//!
//! The buffer grows on demand and is reused across paints. It owns no HWND,
//! timer, worker, or lock; the surface's UI thread owns all access.
class CGdiBackBuffer final {
public:
	CGdiBackBuffer() = default;
	~CGdiBackBuffer() noexcept;

	CGdiBackBuffer(const CGdiBackBuffer&) = delete;
	CGdiBackBuffer& operator=(const CGdiBackBuffer&) = delete;
	CGdiBackBuffer(CGdiBackBuffer&&) = delete;
	CGdiBackBuffer& operator=(CGdiBackBuffer&&) = delete;

	[[nodiscard]] bool Ensure(HDC reference, int width, int height) noexcept;
	void Reset() noexcept;

	[[nodiscard]] HDC Dc() const noexcept { return m_dc; }
	[[nodiscard]] int Width() const noexcept { return m_width; }
	[[nodiscard]] int Height() const noexcept { return m_height; }
	[[nodiscard]] bool Present(HDC target, const RECT& region) const noexcept;

private:
	HDC m_dc{};
	HBITMAP m_bitmap{};
	HGDIOBJ m_originalBitmap{};
	int m_width{};
	int m_height{};
};

} // namespace workbench::rendering

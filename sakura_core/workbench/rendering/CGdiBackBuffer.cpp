/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "workbench/rendering/CGdiBackBuffer.h"

#include <algorithm>

namespace workbench::rendering {

CGdiBackBuffer::~CGdiBackBuffer() noexcept
{
	Reset();
}

bool CGdiBackBuffer::Ensure(HDC reference, int width, int height) noexcept
{
	if (reference == nullptr || width <= 0 || height <= 0) return false;
	if (m_dc != nullptr && m_bitmap != nullptr && width <= m_width && height <= m_height) return true;

	HDC dc = m_dc;
	if (dc == nullptr) {
		dc = ::CreateCompatibleDC(reference);
		if (dc == nullptr) return false;
	}
	const int nextWidth = std::max(width, m_width);
	const int nextHeight = std::max(height, m_height);
	const HBITMAP replacement = ::CreateCompatibleBitmap(reference, nextWidth, nextHeight);
	if (replacement == nullptr) {
		if (m_dc == nullptr) ::DeleteDC(dc);
		return false;
	}

	const HGDIOBJ previous = ::SelectObject(dc, replacement);
	if (previous == nullptr || previous == HGDI_ERROR) {
		::DeleteObject(replacement);
		if (m_dc == nullptr) ::DeleteDC(dc);
		return false;
	}
	if (m_dc == nullptr) {
		m_dc = dc;
		m_originalBitmap = previous;
	} else if (m_bitmap != nullptr) {
		::DeleteObject(m_bitmap);
	}
	m_bitmap = replacement;
	m_width = nextWidth;
	m_height = nextHeight;
	return true;
}

void CGdiBackBuffer::Reset() noexcept
{
	if (m_dc != nullptr && m_originalBitmap != nullptr) {
		(void)::SelectObject(m_dc, m_originalBitmap);
	}
	if (m_bitmap != nullptr) ::DeleteObject(m_bitmap);
	if (m_dc != nullptr) ::DeleteDC(m_dc);
	m_dc = nullptr;
	m_bitmap = nullptr;
	m_originalBitmap = nullptr;
	m_width = 0;
	m_height = 0;
}

bool CGdiBackBuffer::Present(HDC target, const RECT& region) const noexcept
{
	if (target == nullptr || m_dc == nullptr || m_bitmap == nullptr) return false;
	const int width = std::max(0L, region.right - region.left);
	const int height = std::max(0L, region.bottom - region.top);
	if (width == 0 || height == 0) return true;
	return ::BitBlt(target, region.left, region.top, width, height,
		m_dc, region.left, region.top, SRCCOPY) != FALSE;
}

} // namespace workbench::rendering

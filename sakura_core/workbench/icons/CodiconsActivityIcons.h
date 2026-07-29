/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib AND CC-BY-4.0
*/
#pragma once

// The Sakura GDI wrapper is Zlib; imported Codicons geometry is CC-BY-4.0.
// Imported from microsoft/vscode-codicons at c20fbe9efb8ff7cc77182c5b43c44025544ff843.
// See CODICONS-ATTRIBUTION.md beside this file for the source path data, license,
// attribution, and the local coordinate transformation.

#include "workbench/IconMetrics.h"

#include <Windows.h>

namespace workbench::icons::codicons {
namespace detail {

constexpr int kSvgUnits = 24;
constexpr int kCoordinateScale = 2000;
constexpr int kLogicalExtent = kSvgUnits * kCoordinateScale;

[[nodiscard]] inline bool BeginIconPath(HDC dc, const IconRect& box) noexcept
{
	if (dc == nullptr || box.Width() <= 0 || box.Height() <= 0) return false;
	if (::SetGraphicsMode(dc, GM_ADVANCED) == 0) return false;
	::SetPolyFillMode(dc, WINDING); // SVG defaults to the nonzero fill rule.
	const XFORM transform{
		static_cast<float>(box.Width()) / kLogicalExtent, 0.0F,
		0.0F, static_cast<float>(box.Height()) / kLogicalExtent,
		static_cast<float>(box.left), static_cast<float>(box.top),
	};
	return ::SetWorldTransform(dc, &transform) != FALSE && ::BeginPath(dc) != FALSE;
}

inline void MoveTo(HDC dc, int x, int y) noexcept { ::MoveToEx(dc, x, y, nullptr); }
inline void PathLineTo(HDC dc, int x, int y) noexcept { ::LineTo(dc, x, y); }
inline void CubicTo(HDC dc, int x1, int y1, int x2, int y2, int x3, int y3) noexcept
{
	const POINT points[] = { { x1, y1 }, { x2, y2 }, { x3, y3 } };
	::PolyBezierTo(dc, points, 3);
}

} // namespace detail

//! Exact geometry adapted from vscode-codicons `files` (24x24 SVG path).
inline void DrawFiles(HDC dc, const IconRect& box, COLORREF color) noexcept
{
	const int saved = ::SaveDC(dc);
	if (saved == 0) return;
	if (!detail::BeginIconPath(dc, box)) {
		::RestoreDC(dc, saved);
		return;
	}
	using namespace detail;
	MoveTo(dc, 15000, 45000);
	PathLineTo(dc, 35190, 45000);
	CubicTo(dc, 34140, 46800, 32220, 48000, 30000, 48000);
	PathLineTo(dc, 15000, 48000);
	CubicTo(dc, 8370, 48000, 3000, 42630, 3000, 36000);
	PathLineTo(dc, 3000, 12000);
	CubicTo(dc, 3000, 9780, 4200, 7860, 6000, 6810);
	PathLineTo(dc, 6000, 36000);
	CubicTo(dc, 6000, 40950, 10050, 45000, 15000, 45000);
	::CloseFigure(dc);
	MoveTo(dc, 42000, 16242);
	PathLineTo(dc, 42000, 36000);
	CubicTo(dc, 42000, 39309, 39309, 42000, 36000, 42000);
	PathLineTo(dc, 15000, 42000);
	CubicTo(dc, 11691, 42000, 9000, 39309, 9000, 36000);
	PathLineTo(dc, 9000, 6000);
	CubicTo(dc, 9000, 2691, 11691, 0, 15000, 0);
	PathLineTo(dc, 25758, 0);
	CubicTo(dc, 26943, 0, 28101, 480, 28941, 1317);
	PathLineTo(dc, 40683, 13059);
	CubicTo(dc, 41532, 13908, 42000, 15039, 42000, 16242);
	::CloseFigure(dc);
	MoveTo(dc, 27000, 13500);
	CubicTo(dc, 27000, 14328, 27675, 15000, 28500, 15000);
	PathLineTo(dc, 38379, 15000);
	PathLineTo(dc, 27000, 3621);
	PathLineTo(dc, 27000, 13500);
	::CloseFigure(dc);
	MoveTo(dc, 39000, 36000);
	PathLineTo(dc, 39000, 18000);
	PathLineTo(dc, 28500, 18000);
	CubicTo(dc, 26019, 18000, 24000, 15981, 24000, 13500);
	PathLineTo(dc, 24000, 3000);
	PathLineTo(dc, 15000, 3000);
	CubicTo(dc, 13344, 3000, 12000, 4347, 12000, 6000);
	PathLineTo(dc, 12000, 36000);
	CubicTo(dc, 12000, 37653, 13344, 39000, 15000, 39000);
	PathLineTo(dc, 36000, 39000);
	CubicTo(dc, 37656, 39000, 39000, 37653, 39000, 36000);
	::CloseFigure(dc);
	::EndPath(dc);
	const HBRUSH brush = ::CreateSolidBrush(color);
	if (brush != nullptr) {
		const HGDIOBJ oldBrush = ::SelectObject(dc, brush);
		::FillPath(dc);
		::SelectObject(dc, oldBrush);
		::DeleteObject(brush);
	}
	::RestoreDC(dc, saved);
}

//! Exact geometry adapted from vscode-codicons `source-control` (24x24 SVG path).
inline void DrawSourceControl(HDC dc, const IconRect& box, COLORREF color) noexcept
{
	const int saved = ::SaveDC(dc);
	if (saved == 0) return;
	if (!detail::BeginIconPath(dc, box)) {
		::RestoreDC(dc, saved);
		return;
	}
	using namespace detail;
	MoveTo(dc, 42000, 16500);
	CubicTo(dc, 42000, 12363, 38637, 9000, 34500, 9000);
	CubicTo(dc, 30363, 9000, 27000, 12363, 27000, 16500);
	CubicTo(dc, 27000, 20046, 29478, 23007, 32790, 23784);
	CubicTo(dc, 32232, 25638, 30531, 27000, 28500, 27000);
	PathLineTo(dc, 19500, 27000);
	CubicTo(dc, 17805, 27000, 16257, 27585, 15000, 28536);
	PathLineTo(dc, 15000, 14847);
	CubicTo(dc, 18420, 14150, 21000, 11121, 21000, 7500);
	CubicTo(dc, 21000, 3363, 17637, 0, 13500, 0);
	CubicTo(dc, 9363, 0, 6000, 3363, 6000, 7500);
	CubicTo(dc, 6000, 11124, 8580, 14150, 12000, 14847);
	PathLineTo(dc, 12000, 33150);
	CubicTo(dc, 8580, 33846, 6000, 36876, 6000, 40497);
	CubicTo(dc, 6000, 44634, 9363, 47997, 13500, 47997);
	CubicTo(dc, 17637, 47997, 21000, 44634, 21000, 40497);
	CubicTo(dc, 21000, 36951, 18522, 33991, 15210, 33213);
	CubicTo(dc, 15768, 31358, 17469, 29997, 19500, 29997);
	PathLineTo(dc, 28500, 29997);
	CubicTo(dc, 32169, 29997, 35220, 27345, 35862, 23858);
	CubicTo(dc, 39348, 23214, 42000, 20168, 42000, 16500);
	::CloseFigure(dc);
	MoveTo(dc, 9000, 7500);
	CubicTo(dc, 9000, 5019, 11019, 3000, 13500, 3000);
	CubicTo(dc, 15981, 3000, 18000, 5019, 18000, 7500);
	CubicTo(dc, 18000, 9981, 15981, 12000, 13500, 12000);
	CubicTo(dc, 11019, 12000, 9000, 9981, 9000, 7500);
	::CloseFigure(dc);
	MoveTo(dc, 18000, 40500);
	CubicTo(dc, 18000, 42981, 15981, 45000, 13500, 45000);
	CubicTo(dc, 11019, 45000, 9000, 42981, 9000, 40500);
	CubicTo(dc, 9000, 38019, 11019, 36000, 13500, 36000);
	CubicTo(dc, 15981, 36000, 18000, 38019, 18000, 40500);
	::CloseFigure(dc);
	MoveTo(dc, 34500, 21000);
	CubicTo(dc, 32019, 21000, 30000, 18981, 30000, 16500);
	CubicTo(dc, 30000, 14019, 32019, 12000, 34500, 12000);
	CubicTo(dc, 36981, 12000, 39000, 14019, 39000, 16500);
	CubicTo(dc, 39000, 18981, 36981, 21000, 34500, 21000);
	::CloseFigure(dc);
	::EndPath(dc);
	const HBRUSH brush = ::CreateSolidBrush(color);
	if (brush != nullptr) {
		const HGDIOBJ oldBrush = ::SelectObject(dc, brush);
		::FillPath(dc);
		::SelectObject(dc, oldBrush);
		::DeleteObject(brush);
	}
	::RestoreDC(dc, saved);
}

} // namespace workbench::icons::codicons

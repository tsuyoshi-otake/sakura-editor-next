/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "terminal/window/TerminalBuiltinGlyphRenderer.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <limits>
#include <memory>
#include <new>

namespace terminal {
namespace {

enum class Stroke : std::uint8_t {
	None,
	Light,
	Heavy,
	Double,
};

struct BoxSpec {
	Stroke left{};
	Stroke right{};
	Stroke up{};
	Stroke down{};
	unsigned int dashCount{};
	bool diagonalForward{};
	bool diagonalBackward{};
};

constexpr std::size_t kMaximumBatchRectangles = 256U;
constexpr std::size_t kMaximumRectanglesPerGlyph = kMaximumBatchRectangles;
// Direct rasterization deliberately has a fixed per-command work bound.  This
// keeps malformed cell geometry on the exact HDC fallback without allocating
// pixel-area scratch storage on the paint path.
constexpr std::size_t kMaximumDirectPixelWorkPerGlyph = 64U * 1024U;
constexpr std::size_t kMaximumDirectPixelWorkMultiplier = 9U;

[[nodiscard]] bool IsValidRect(const RECT& rect) noexcept
{
	return rect.right > rect.left && rect.bottom > rect.top;
}

[[nodiscard]] bool HasSafeRasterExtent(const RECT& rect) noexcept
{
	if( !IsValidRect(rect) ) return false;
	const auto width = static_cast<long long>(rect.right) - static_cast<long long>(rect.left);
	const auto height = static_cast<long long>(rect.bottom) - static_cast<long long>(rect.top);
	return width > 0 && height > 0 &&
		width <= std::numeric_limits<int>::max() && height <= std::numeric_limits<int>::max();
}

[[nodiscard]] bool ExceedsDirectPixelWorkLimit(const RECT& rect) noexcept
{
	if( !HasSafeRasterExtent(rect) ) return true;
	const auto width = static_cast<std::size_t>(static_cast<long long>(rect.right) - static_cast<long long>(rect.left));
	const auto height = static_cast<std::size_t>(static_cast<long long>(rect.bottom) - static_cast<long long>(rect.top));
	if( width > kMaximumDirectPixelWorkPerGlyph / kMaximumDirectPixelWorkMultiplier ) return true;
	const auto maximumHeight = kMaximumDirectPixelWorkPerGlyph /
		(width * kMaximumDirectPixelWorkMultiplier);
	return height > maximumHeight;
}

[[nodiscard]] std::uint32_t ColorRefToBgrx(COLORREF color) noexcept
{
	// COLORREF is 0x00bbggrr.  A 32-bit BI_RGB DIB stores little-endian BGRX,
	// so the numeric value written through a uint32_t pointer is 0x00rrggbb.
	return (static_cast<std::uint32_t>(GetRValue(color)) << 16U) |
		(static_cast<std::uint32_t>(GetGValue(color)) << 8U) |
		static_cast<std::uint32_t>(GetBValue(color));
}

struct PixelRasterSink final {
	std::uint32_t* pixels{};
	std::ptrdiff_t stridePixels{};
	RECT clip{};
	std::uint32_t color{};

	void Fill(RECT rect) const noexcept
	{
		rect.left = std::max(rect.left, clip.left);
		rect.top = std::max(rect.top, clip.top);
		rect.right = std::min(rect.right, clip.right);
		rect.bottom = std::min(rect.bottom, clip.bottom);
		if( !IsValidRect(rect) ) return;
		const auto width = static_cast<std::size_t>(rect.right - rect.left);
		for( LONG y = rect.top; y < rect.bottom; ++y ) {
			auto* const row = pixels + static_cast<std::ptrdiff_t>(y) * stridePixels;
			std::fill_n(row + rect.left, width, color);
		}
	}

	void Pixel(LONG x, LONG y) const noexcept
	{
		if( x < clip.left || x >= clip.right || y < clip.top || y >= clip.bottom ) return;
		auto* const row = pixels + static_cast<std::ptrdiff_t>(y) * stridePixels;
		row[x] = color;
	}
};

[[nodiscard]] bool InitializePixelRasterSink(const TerminalBuiltinGlyphPixelSurface& surface,
	PixelRasterSink& sink) noexcept
{
	if( surface.pixels == nullptr || surface.width <= 0 || surface.height <= 0 ||
		surface.stridePixels == 0 ||
		surface.stridePixels == std::numeric_limits<std::ptrdiff_t>::min() ) {
		return false;
	}
	const auto width = static_cast<std::size_t>(surface.width);
	const auto height = static_cast<std::size_t>(surface.height);
	const auto maximumOffset = static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max());
	if( width > maximumOffset ) return false;
	const auto strideMagnitude = static_cast<std::size_t>(surface.stridePixels < 0
		? -surface.stridePixels : surface.stridePixels);
	if( strideMagnitude < width ) return false;
	const auto finalColumnOffset = width - 1U;
	if( height > 1U && strideMagnitude > (maximumOffset - finalColumnOffset) / (height - 1U) ) return false;

	sink = {
		surface.pixels,
		surface.stridePixels,
		{
			std::max<LONG>(0, surface.clip.left),
			std::max<LONG>(0, surface.clip.top),
			std::min(surface.width, surface.clip.right),
			std::min(surface.height, surface.clip.bottom),
		},
	};
	return true;
}

struct ImmediateRasterSink final {
	HDC dc{};
	COLORREF color{};

	void Fill(RECT rect) const noexcept
	{
		if( dc == nullptr || !IsValidRect(rect) ) return;
		const auto brush = static_cast<HBRUSH>(::GetStockObject(DC_BRUSH));
		::SetDCBrushColor(dc, color);
		::FillRect(dc, &rect, brush);
	}

	void Pixel(LONG x, LONG y) const noexcept
	{
		if( dc != nullptr ) ::SetPixelV(dc, x, y, color);
	}
};

[[nodiscard]] int StrokeThickness(const RECT& rect, Stroke stroke) noexcept
{
	const int extent = static_cast<int>(std::max<LONG>(1, std::min(rect.right - rect.left, rect.bottom - rect.top)));
	const int light = std::max(1, extent / 12);
	return stroke == Stroke::Heavy ? std::max(light + 1, extent / 6) : light;
}

template<typename RasterSink>
void DrawHorizontal(RasterSink& sink, const RECT& rect, int centerY, int thickness, bool left, bool right) noexcept
{
	const LONG top = std::clamp<LONG>(centerY - thickness / 2, rect.top, rect.bottom);
	const LONG bottom = std::clamp<LONG>(top + thickness, rect.top, rect.bottom);
	const int centerX = rect.left + (rect.right - rect.left) / 2;
	sink.Fill({ left ? rect.left : centerX, top, right ? rect.right : centerX + 1, bottom });
}

template<typename RasterSink>
void DrawVertical(RasterSink& sink, const RECT& rect, int centerX, int thickness, bool up, bool down) noexcept
{
	const LONG left = std::clamp<LONG>(centerX - thickness / 2, rect.left, rect.right);
	const LONG right = std::clamp<LONG>(left + thickness, rect.left, rect.right);
	const int centerY = rect.top + (rect.bottom - rect.top) / 2;
	sink.Fill({ left, up ? rect.top : centerY, right, down ? rect.bottom : centerY + 1 });
}

template<typename RasterSink>
void DrawDirectionalStroke(RasterSink& sink, const RECT& rect, Stroke stroke, bool horizontal, bool negative) noexcept
{
	if( stroke == Stroke::None ) return;
	const int width = rect.right - rect.left;
	const int height = rect.bottom - rect.top;
	const int centerX = rect.left + width / 2;
	const int centerY = rect.top + height / 2;
	if( stroke == Stroke::Double ) {
		const int offset = std::max(1, std::min(width, height) / 4);
		const int thickness = StrokeThickness(rect, Stroke::Light);
		if( horizontal ) {
			DrawHorizontal(sink, rect, centerY - offset, thickness, negative, !negative);
			DrawHorizontal(sink, rect, centerY + offset, thickness, negative, !negative);
		} else {
			DrawVertical(sink, rect, centerX - offset, thickness, negative, !negative);
			DrawVertical(sink, rect, centerX + offset, thickness, negative, !negative);
		}
		return;
	}
	if( horizontal ) DrawHorizontal(sink, rect, centerY, StrokeThickness(rect, stroke), negative, !negative);
	else DrawVertical(sink, rect, centerX, StrokeThickness(rect, stroke), negative, !negative);
}

template<typename RasterSink>
void DrawDashed(RasterSink& sink, const RECT& rect, bool horizontal, Stroke stroke, unsigned int dashCount) noexcept
{
	const int length = horizontal ? rect.right - rect.left : rect.bottom - rect.top;
	if( length <= 0 ) return;
	const int thickness = StrokeThickness(rect, stroke);
	const int center = horizontal ? rect.top + (rect.bottom - rect.top) / 2 : rect.left + (rect.right - rect.left) / 2;
	const unsigned int count = std::max(2U, dashCount);
	for( unsigned int dash = 0; dash < count; ++dash ) {
		const int begin = static_cast<int>((static_cast<long long>(dash) * length) / count);
		const int end = static_cast<int>((static_cast<long long>(dash + 1) * length) / count);
		const int used = std::max(1, (end - begin + 1) / 2);
		if( horizontal ) {
			const LONG top = std::clamp<LONG>(center - thickness / 2, rect.top, rect.bottom);
			sink.Fill({ rect.left + begin, top, std::min<LONG>(rect.right, rect.left + begin + used),
				std::min<LONG>(rect.bottom, top + thickness) });
		} else {
			const LONG left = std::clamp<LONG>(center - thickness / 2, rect.left, rect.right);
			sink.Fill({ left, rect.top + begin, std::min<LONG>(rect.right, left + thickness),
				std::min<LONG>(rect.bottom, rect.top + begin + used) });
		}
	}
}

[[nodiscard]] BoxSpec BoxSpecFor(char32_t glyph) noexcept
{
	BoxSpec result{};
	const auto heavy = [](BoxSpec& spec, bool left, bool right, bool up, bool down) {
		if( left ) spec.left = Stroke::Heavy;
		if( right ) spec.right = Stroke::Heavy;
		if( up ) spec.up = Stroke::Heavy;
		if( down ) spec.down = Stroke::Heavy;
	};
	switch( glyph ) {
	case 0x2500: return { Stroke::Light, Stroke::Light };
	case 0x2501: return { Stroke::Heavy, Stroke::Heavy };
	case 0x2502: return { {}, {}, Stroke::Light, Stroke::Light };
	case 0x2503: return { {}, {}, Stroke::Heavy, Stroke::Heavy };
	case 0x2504: return { Stroke::Light, Stroke::Light, {}, {}, 3 };
	case 0x2505: return { Stroke::Heavy, Stroke::Heavy, {}, {}, 3 };
	case 0x2506: return { {}, {}, Stroke::Light, Stroke::Light, 3 };
	case 0x2507: return { {}, {}, Stroke::Heavy, Stroke::Heavy, 3 };
	case 0x2508: return { Stroke::Light, Stroke::Light, {}, {}, 4 };
	case 0x2509: return { Stroke::Heavy, Stroke::Heavy, {}, {}, 4 };
	case 0x250a: return { {}, {}, Stroke::Light, Stroke::Light, 4 };
	case 0x250b: return { {}, {}, Stroke::Heavy, Stroke::Heavy, 4 };
	case 0x254c: return { Stroke::Light, Stroke::Light, {}, {}, 2 };
	case 0x254d: return { Stroke::Heavy, Stroke::Heavy, {}, {}, 2 };
	case 0x254e: return { {}, {}, Stroke::Light, Stroke::Light, 2 };
	case 0x254f: return { {}, {}, Stroke::Heavy, Stroke::Heavy, 2 };
	case 0x2550: return { Stroke::Double, Stroke::Double };
	case 0x2551: return { {}, {}, Stroke::Double, Stroke::Double };
	case 0x2552: return { {}, Stroke::Double, {}, Stroke::Light };
	case 0x2553: return { {}, Stroke::Light, {}, Stroke::Double };
	case 0x2554: return { {}, Stroke::Double, {}, Stroke::Double };
	case 0x2555: return { Stroke::Double, {}, {}, Stroke::Light };
	case 0x2556: return { Stroke::Light, {}, {}, Stroke::Double };
	case 0x2557: return { Stroke::Double, {}, {}, Stroke::Double };
	case 0x2558: return { {}, Stroke::Double, Stroke::Light, {} };
	case 0x2559: return { {}, Stroke::Light, Stroke::Double, {} };
	case 0x255a: return { {}, Stroke::Double, Stroke::Double, {} };
	case 0x255b: return { Stroke::Double, {}, Stroke::Light, {} };
	case 0x255c: return { Stroke::Light, {}, Stroke::Double, {} };
	case 0x255d: return { Stroke::Double, {}, Stroke::Double, {} };
	case 0x255e: return { {}, Stroke::Double, Stroke::Light, Stroke::Light };
	case 0x255f: return { {}, Stroke::Light, Stroke::Double, Stroke::Double };
	case 0x2560: return { {}, Stroke::Double, Stroke::Double, Stroke::Double };
	case 0x2561: return { Stroke::Double, {}, Stroke::Light, Stroke::Light };
	case 0x2562: return { Stroke::Light, {}, Stroke::Double, Stroke::Double };
	case 0x2563: return { Stroke::Double, {}, Stroke::Double, Stroke::Double };
	case 0x2564: return { Stroke::Double, Stroke::Double, {}, Stroke::Light };
	case 0x2565: return { Stroke::Light, Stroke::Light, {}, Stroke::Double };
	case 0x2566: return { Stroke::Double, Stroke::Double, {}, Stroke::Double };
	case 0x2567: return { Stroke::Double, Stroke::Double, Stroke::Light, {} };
	case 0x2568: return { Stroke::Light, Stroke::Light, Stroke::Double, {} };
	case 0x2569: return { Stroke::Double, Stroke::Double, Stroke::Double, {} };
	case 0x256a: return { Stroke::Double, Stroke::Double, Stroke::Light, Stroke::Light };
	case 0x256b: return { Stroke::Light, Stroke::Light, Stroke::Double, Stroke::Double };
	case 0x256c: return { Stroke::Double, Stroke::Double, Stroke::Double, Stroke::Double };
	case 0x256d: return { {}, Stroke::Light, {}, Stroke::Light };
	case 0x256e: return { Stroke::Light, {}, {}, Stroke::Light };
	case 0x256f: return { Stroke::Light, {}, Stroke::Light, {} };
	case 0x2570: return { {}, Stroke::Light, Stroke::Light, {} };
	// U+2571 is upper-right to lower-left; U+2572 is upper-left to
	// lower-right.  DrawDiagonal(true) follows the latter direction.
	case 0x2571: result.diagonalBackward = true; return result;
	case 0x2572: result.diagonalForward = true; return result;
	case 0x2573: result.diagonalForward = result.diagonalBackward = true; return result;
	case 0x2574: return { Stroke::Light };
	case 0x2575: return { {}, {}, Stroke::Light };
	case 0x2576: return { {}, Stroke::Light };
	case 0x2577: return { {}, {}, {}, Stroke::Light };
	case 0x2578: return { Stroke::Heavy };
	case 0x2579: return { {}, {}, Stroke::Heavy };
	case 0x257a: return { {}, Stroke::Heavy };
	case 0x257b: return { {}, {}, {}, Stroke::Heavy };
	case 0x257c: return { Stroke::Light, Stroke::Heavy };
	case 0x257d: return { {}, {}, Stroke::Light, Stroke::Heavy };
	case 0x257e: return { Stroke::Heavy, Stroke::Light };
	case 0x257f: return { {}, {}, Stroke::Heavy, Stroke::Light };
	default:
		break;
	}

	if( glyph >= 0x250c && glyph <= 0x251b ) {
		const auto group = static_cast<unsigned int>((glyph - 0x250c) / 4);
		const auto variant = static_cast<unsigned int>((glyph - 0x250c) % 4);
		Stroke first = (variant == 2 || variant == 3) ? Stroke::Heavy : Stroke::Light;
		Stroke second = (variant == 1 || variant == 3) ? Stroke::Heavy : Stroke::Light;
		switch( group ) {
		case 0: result.down = first; result.right = second; break;
		case 1: result.down = first; result.left = second; break;
		case 2: result.up = first; result.right = second; break;
		default: result.up = first; result.left = second; break;
		}
		return result;
	}
	if( glyph >= 0x251c && glyph <= 0x2523 ) {
		result = { {}, Stroke::Light, Stroke::Light, Stroke::Light };
		switch( glyph ) {
		case 0x251d: heavy(result, false, true, false, false); break;
		case 0x251e: heavy(result, false, false, true, false); break;
		case 0x251f: heavy(result, false, false, false, true); break;
		case 0x2520: heavy(result, false, false, true, true); break;
		case 0x2521: heavy(result, false, true, true, false); break;
		case 0x2522: heavy(result, false, true, false, true); break;
		case 0x2523: heavy(result, false, true, true, true); break;
		default: break;
		}
		return result;
	}
	if( glyph >= 0x2524 && glyph <= 0x252b ) {
		result = { Stroke::Light, {}, Stroke::Light, Stroke::Light };
		switch( glyph ) {
		case 0x2525: heavy(result, true, false, false, false); break;
		case 0x2526: heavy(result, false, false, true, false); break;
		case 0x2527: heavy(result, false, false, false, true); break;
		case 0x2528: heavy(result, false, false, true, true); break;
		case 0x2529: heavy(result, true, false, true, false); break;
		case 0x252a: heavy(result, true, false, false, true); break;
		case 0x252b: heavy(result, true, false, true, true); break;
		default: break;
		}
		return result;
	}
	if( glyph >= 0x252c && glyph <= 0x2533 ) {
		result = { Stroke::Light, Stroke::Light, {}, Stroke::Light };
		switch( glyph ) {
		case 0x252d: heavy(result, true, false, false, false); break;
		case 0x252e: heavy(result, false, true, false, false); break;
		case 0x252f: heavy(result, true, true, false, false); break;
		case 0x2530: heavy(result, false, false, false, true); break;
		case 0x2531: heavy(result, true, false, false, true); break;
		case 0x2532: heavy(result, false, true, false, true); break;
		case 0x2533: heavy(result, true, true, false, true); break;
		default: break;
		}
		return result;
	}
	if( glyph >= 0x2534 && glyph <= 0x253b ) {
		result = { Stroke::Light, Stroke::Light, Stroke::Light, {} };
		switch( glyph ) {
		case 0x2535: heavy(result, true, false, false, false); break;
		case 0x2536: heavy(result, false, true, false, false); break;
		case 0x2537: heavy(result, true, true, false, false); break;
		case 0x2538: heavy(result, false, false, true, false); break;
		case 0x2539: heavy(result, true, false, true, false); break;
		case 0x253a: heavy(result, false, true, true, false); break;
		case 0x253b: heavy(result, true, true, true, false); break;
		default: break;
		}
		return result;
	}
	if( glyph >= 0x253c && glyph <= 0x254b ) {
		result = { Stroke::Light, Stroke::Light, Stroke::Light, Stroke::Light };
		switch( glyph ) {
		case 0x253d: heavy(result, true, false, false, false); break;
		case 0x253e: heavy(result, false, true, false, false); break;
		case 0x253f: heavy(result, true, true, false, false); break;
		case 0x2540: heavy(result, false, false, true, false); break;
		case 0x2541: heavy(result, false, false, false, true); break;
		case 0x2542: heavy(result, false, false, true, true); break;
		case 0x2543: heavy(result, true, false, true, false); break;
		case 0x2544: heavy(result, false, true, true, false); break;
		case 0x2545: heavy(result, true, false, false, true); break;
		case 0x2546: heavy(result, false, true, false, true); break;
		case 0x2547: heavy(result, true, true, true, false); break;
		case 0x2548: heavy(result, true, true, false, true); break;
		case 0x2549: heavy(result, true, false, true, true); break;
		case 0x254a: heavy(result, false, true, true, true); break;
		case 0x254b: heavy(result, true, true, true, true); break;
		default: break;
		}
		return result;
	}
	return { Stroke::Light, Stroke::Light };
}

template<typename RasterSink>
void DrawDiagonal(RasterSink& sink, const RECT& rect, bool forward) noexcept
{
	const int width = rect.right - rect.left;
	const int height = rect.bottom - rect.top;
	const int steps = std::max(width, height);
	const int thickness = StrokeThickness(rect, Stroke::Light);
	if( steps <= 0 ) return;
	for( int step = 0; step < steps; ++step ) {
		const int x = rect.left + (step * std::max(0, width - 1)) / std::max(1, steps - 1);
		const int relativeY = (step * std::max(0, height - 1)) / std::max(1, steps - 1);
		const int y = forward ? rect.top + relativeY : rect.bottom - 1 - relativeY;
		const RECT pixel{
			std::max<LONG>(rect.left, x - thickness / 2),
			std::max<LONG>(rect.top, y - thickness / 2),
			std::min<LONG>(rect.right, x - thickness / 2 + thickness),
			std::min<LONG>(rect.bottom, y - thickness / 2 + thickness),
		};
		sink.Fill(pixel);
	}
}

template<typename RasterSink>
void DrawBox(RasterSink& sink, char32_t glyph, const RECT& rect) noexcept
{
	const auto spec = BoxSpecFor(glyph);
	if( spec.diagonalForward ) DrawDiagonal(sink, rect, true);
	if( spec.diagonalBackward ) DrawDiagonal(sink, rect, false);
	if( spec.dashCount != 0 ) {
		if( spec.left != Stroke::None || spec.right != Stroke::None ) DrawDashed(sink, rect, true, spec.left, spec.dashCount);
		if( spec.up != Stroke::None || spec.down != Stroke::None ) DrawDashed(sink, rect, false, spec.up, spec.dashCount);
		return;
	}
	DrawDirectionalStroke(sink, rect, spec.left, true, true);
	DrawDirectionalStroke(sink, rect, spec.right, true, false);
	DrawDirectionalStroke(sink, rect, spec.up, false, true);
	DrawDirectionalStroke(sink, rect, spec.down, false, false);
}

template<typename RasterSink>
void DrawShade(RasterSink& sink, const RECT& rect, unsigned int threshold) noexcept
{
	static constexpr std::array<std::array<unsigned int, 4>, 4> bayer{{
		{{ 0, 8, 2, 10 }},
		{{ 12, 4, 14, 6 }},
		{{ 3, 11, 1, 9 }},
		{{ 15, 7, 13, 5 }},
	}};
	for( LONG y = rect.top; y < rect.bottom; ++y ) {
		for( LONG x = rect.left; x < rect.right; ++x ) {
			if( bayer[static_cast<std::size_t>(y - rect.top) % 4][static_cast<std::size_t>(x - rect.left) % 4] < threshold ) {
				sink.Pixel(x, y);
			}
		}
	}
}

template<typename RasterSink>
void DrawBlock(RasterSink& sink, char32_t glyph, const RECT& rect) noexcept
{
	const int width = rect.right - rect.left;
	const int height = rect.bottom - rect.top;
	const auto horizontal = [&](int eighths, bool fromLeft) {
		const int covered = std::clamp((width * eighths + 7) / 8, 0, width);
		sink.Fill({ fromLeft ? rect.left : rect.right - covered, rect.top,
			fromLeft ? rect.left + covered : rect.right, rect.bottom });
	};
	const auto vertical = [&](int eighths, bool fromTop) {
		const int covered = std::clamp((height * eighths + 7) / 8, 0, height);
		sink.Fill({ rect.left, fromTop ? rect.top : rect.bottom - covered, rect.right,
			fromTop ? rect.top + covered : rect.bottom });
	};
	const int middleX = rect.left + (width + 1) / 2;
	const int middleY = rect.top + (height + 1) / 2;
	const auto quadrant = [&](bool upper, bool right) {
		sink.Fill({ right ? middleX : rect.left, upper ? rect.top : middleY,
			right ? rect.right : middleX, upper ? middleY : rect.bottom });
	};

	switch( glyph ) {
	case 0x2580: vertical(4, true); break;
	case 0x2581: vertical(1, false); break;
	case 0x2582: vertical(2, false); break;
	case 0x2583: vertical(3, false); break;
	case 0x2584: vertical(4, false); break;
	case 0x2585: vertical(5, false); break;
	case 0x2586: vertical(6, false); break;
	case 0x2587: vertical(7, false); break;
	case 0x2588: sink.Fill(rect); break;
	case 0x2589: horizontal(7, true); break;
	case 0x258a: horizontal(6, true); break;
	case 0x258b: horizontal(5, true); break;
	case 0x258c: horizontal(4, true); break;
	case 0x258d: horizontal(3, true); break;
	case 0x258e: horizontal(2, true); break;
	case 0x258f: horizontal(1, true); break;
	case 0x2590: horizontal(4, false); break;
	case 0x2591: DrawShade(sink, rect, 4); break;
	case 0x2592: DrawShade(sink, rect, 8); break;
	case 0x2593: DrawShade(sink, rect, 12); break;
	case 0x2594: vertical(1, true); break;
	case 0x2595: horizontal(1, false); break;
	case 0x2596: quadrant(false, false); break;
	case 0x2597: quadrant(false, true); break;
	case 0x2598: quadrant(true, false); break;
	case 0x2599: quadrant(true, false); quadrant(false, false); quadrant(false, true); break;
	case 0x259a: quadrant(true, false); quadrant(false, true); break;
	case 0x259b: quadrant(true, false); quadrant(true, true); quadrant(false, false); break;
	case 0x259c: quadrant(true, false); quadrant(true, true); quadrant(false, true); break;
	case 0x259d: quadrant(true, true); break;
	case 0x259e: quadrant(true, true); quadrant(false, false); break;
	case 0x259f: quadrant(true, true); quadrant(false, false); quadrant(false, true); break;
	default: break;
	}
}

[[nodiscard]] std::size_t AddRectangleCount(std::size_t total, std::size_t added) noexcept
{
	if( added == 0 ) return total;
	if( total > kMaximumRectanglesPerGlyph || added > kMaximumRectanglesPerGlyph - total ) {
		return kMaximumRectanglesPerGlyph + 1;
	}
	return total + added;
}

[[nodiscard]] std::size_t RectangularExtent(LONG lower, LONG upper) noexcept
{
	if( upper <= lower ) return 0;
	const auto extent = static_cast<unsigned long long>(static_cast<long long>(upper) - static_cast<long long>(lower));
	return extent > kMaximumRectanglesPerGlyph ? kMaximumRectanglesPerGlyph + 1 : static_cast<std::size_t>(extent);
}

[[nodiscard]] std::size_t DirectionalStrokeRectangleCount(Stroke stroke) noexcept
{
	if( stroke == Stroke::None ) return 0;
	return stroke == Stroke::Double ? 2U : 1U;
}

[[nodiscard]] std::size_t EstimateBoxRectangles(char32_t glyph, const RECT& rect) noexcept
{
	const auto spec = BoxSpecFor(glyph);
	std::size_t count{};
	const auto steps = std::max(RectangularExtent(rect.left, rect.right), RectangularExtent(rect.top, rect.bottom));
	if( spec.diagonalForward ) count = AddRectangleCount(count, steps);
	if( spec.diagonalBackward ) count = AddRectangleCount(count, steps);
	if( spec.dashCount != 0 ) {
		const auto dashCount = std::max(2U, spec.dashCount);
		if( spec.left != Stroke::None || spec.right != Stroke::None ) count = AddRectangleCount(count, dashCount);
		if( spec.up != Stroke::None || spec.down != Stroke::None ) count = AddRectangleCount(count, dashCount);
		return count;
	}
	count = AddRectangleCount(count, DirectionalStrokeRectangleCount(spec.left));
	count = AddRectangleCount(count, DirectionalStrokeRectangleCount(spec.right));
	count = AddRectangleCount(count, DirectionalStrokeRectangleCount(spec.up));
	return AddRectangleCount(count, DirectionalStrokeRectangleCount(spec.down));
}

[[nodiscard]] std::size_t EstimateShadeRectangles(const RECT& rect) noexcept
{
	const auto width = RectangularExtent(rect.left, rect.right);
	const auto height = RectangularExtent(rect.top, rect.bottom);
	if( width > kMaximumRectanglesPerGlyph || height > kMaximumRectanglesPerGlyph ||
		width != 0 && height > kMaximumRectanglesPerGlyph / width ) {
		return kMaximumRectanglesPerGlyph + 1;
	}
	// A conservative bound deliberately avoids carrying a pixel-area scratch
	// buffer.  Large shade cells take the ordered scalar fallback instead.
	return width * height;
}

[[nodiscard]] std::size_t EstimateBlockRectangles(char32_t glyph, const RECT& rect) noexcept
{
	switch( glyph ) {
	case 0x2591:
	case 0x2592:
	case 0x2593:
		return EstimateShadeRectangles(rect);
	case 0x2599:
	case 0x259b:
	case 0x259c:
	case 0x259f:
		return 3;
	case 0x259a:
	case 0x259e:
		return 2;
	default:
		return 1;
	}
}

[[nodiscard]] std::size_t EstimateGlyphRectangles(const TerminalBuiltinGlyphCommand& command) noexcept
{
	std::size_t count = command.glyph <= 0x257f
		? EstimateBoxRectangles(command.glyph, command.rect)
		: EstimateBlockRectangles(command.glyph, command.rect);
	if( command.style.underline ) count = AddRectangleCount(count, 1);
	return count;
}

[[nodiscard]] RECT UnderlineRect(const RECT& rect) noexcept
{
	return { rect.left, std::max(rect.top, rect.bottom - 2), rect.right, rect.bottom };
}

template<typename RasterSink>
void DrawGlyph(RasterSink& sink, char32_t glyph, const RECT& rect) noexcept
{
	if( glyph <= 0x257f ) DrawBox(sink, glyph, rect);
	else DrawBlock(sink, glyph, rect);
}

template<typename AppendRectangle>
struct BatchRasterSink final {
	AppendRectangle& appendRectangle;
	bool overflow{};

	void Fill(RECT rect) noexcept
	{
		if( overflow || !IsValidRect(rect) ) return;
		if( !appendRectangle(rect) ) overflow = true;
	}

	void Pixel(LONG x, LONG y) noexcept
	{
		if( x == std::numeric_limits<LONG>::max() || y == std::numeric_limits<LONG>::max() ) {
			overflow = true;
			return;
		}
		Fill({ x, y, x + 1, y + 1 });
	}
};

void DrawGlyphImmediately(HDC dc, char32_t glyph, const RECT& cellRect, COLORREF foreground) noexcept
{
	if( dc == nullptr || !TerminalBuiltinGlyphRenderer::Handles(glyph) || !IsValidRect(cellRect) ) return;
	ImmediateRasterSink sink{ dc, foreground };
	DrawGlyph(sink, glyph, cellRect);
}

void DrawCommandImmediately(HDC dc, const TerminalBuiltinGlyphCommand& command) noexcept
{
	DrawGlyphImmediately(dc, command.glyph, command.rect, command.style.foreground);
	if( command.style.underline ) {
		ImmediateRasterSink sink{ dc, command.style.foreground };
		sink.Fill(UnderlineRect(command.rect));
	}
}

} // namespace

struct TerminalBuiltinGlyphRenderer::BatchState final {
	std::array<RECT, kMaximumBatchRectangles> rectangles{};
	std::array<POINT, kMaximumBatchRectangles * 4U> points{};
	std::array<INT, kMaximumBatchRectangles> polygonVertexCounts{};
	TerminalRenderStyle style{};
	std::size_t rectangleCount{};
	bool hasStyle{};
};

bool TerminalBuiltinGlyphRenderer::Handles(char32_t glyph) noexcept
{
	return glyph >= 0x2500 && glyph <= 0x259f;
}

TerminalBuiltinGlyphRenderer::TerminalBuiltinGlyphRenderer() noexcept = default;

TerminalBuiltinGlyphRenderer::~TerminalBuiltinGlyphRenderer() noexcept = default;

void TerminalBuiltinGlyphRenderer::Draw(HDC dc, const TerminalBuiltinGlyphCommand& command) noexcept
{
	DrawGlyphImmediately(dc, command.glyph, command.rect, command.style.foreground);
}

void TerminalBuiltinGlyphRenderer::Draw(HDC dc, char32_t glyph, const RECT& cellRect, COLORREF foreground) noexcept
{
	DrawGlyphImmediately(dc, glyph, cellRect, foreground);
}

bool TerminalBuiltinGlyphRenderer::DrawBatch(const TerminalBuiltinGlyphPixelSurface& surface,
	std::span<const TerminalBuiltinGlyphCommand> commands) noexcept
{
	if( commands.empty() ) return true;
	PixelRasterSink sink{};
	if( !InitializePixelRasterSink(surface, sink) ) return false;
	for( const auto& command : commands ) {
		if( !Handles(command.glyph) || !HasSafeRasterExtent(command.rect) ||
			ExceedsDirectPixelWorkLimit(command.rect) ) {
			return false;
		}
	}

	for( const auto& command : commands ) {
		sink.color = ColorRefToBgrx(command.style.foreground);
		DrawGlyph(sink, command.glyph, command.rect);
		if( command.style.underline ) sink.Fill(UnderlineRect(command.rect));
	}
	return true;
}

void TerminalBuiltinGlyphRenderer::DrawBatch(HDC dc,
	std::span<const TerminalBuiltinGlyphCommand> commands) noexcept
{
	if( dc == nullptr || commands.empty() ) return;
	if( !m_batchState ) {
		try {
			m_batchState = std::make_unique<BatchState>();
		} catch( const std::bad_alloc& ) {
			for( const auto& command : commands ) DrawCommandImmediately(dc, command);
			return;
		}
	}

	auto& state = *m_batchState;
	state.rectangleCount = 0;
	state.hasStyle = false;
	const int savedDc = ::SaveDC(dc);
	if( savedDc == 0 ) {
		for( const auto& command : commands ) DrawCommandImmediately(dc, command);
		return;
	}
	const auto brush = static_cast<HBRUSH>(::GetStockObject(DC_BRUSH));
	const auto pen = static_cast<HPEN>(::GetStockObject(NULL_PEN));
	const auto previousBrush = brush ? ::SelectObject(dc, brush) : nullptr;
	const auto previousPen = pen ? ::SelectObject(dc, pen) : nullptr;
	if( previousBrush == nullptr || previousBrush == HGDI_ERROR || previousPen == nullptr || previousPen == HGDI_ERROR ||
		::SetPolyFillMode(dc, WINDING) == 0 ) {
		::RestoreDC(dc, savedDc);
		for( const auto& command : commands ) DrawCommandImmediately(dc, command);
		return;
	}

	const auto flush = [&]() noexcept {
		if( state.rectangleCount == 0 ) return;
		::SetDCBrushColor(dc, state.style.foreground);
		for( std::size_t index = 0; index < state.rectangleCount; ++index ) {
			const auto pointIndex = index * 4U;
			const auto& rect = state.rectangles[index];
			state.points[pointIndex] = { rect.left, rect.top };
			state.points[pointIndex + 1U] = { rect.right, rect.top };
			state.points[pointIndex + 2U] = { rect.right, rect.bottom };
			state.points[pointIndex + 3U] = { rect.left, rect.bottom };
			state.polygonVertexCounts[index] = 4;
		}
		if( !::PolyPolygon(dc, state.points.data(), state.polygonVertexCounts.data(),
			static_cast<int>(state.rectangleCount)) ) {
			::SetDCBrushColor(dc, state.style.foreground);
			for( std::size_t index = 0; index < state.rectangleCount; ++index ) {
				::FillRect(dc, &state.rectangles[index], brush);
			}
		}
		state.rectangleCount = 0;
	};

	for( const auto& command : commands ) {
		if( !Handles(command.glyph) || !IsValidRect(command.rect) ) {
			flush();
			DrawCommandImmediately(dc, command);
			continue;
		}
		const auto rectangleCount = EstimateGlyphRectangles(command);
		if( rectangleCount > kMaximumRectanglesPerGlyph ) {
			// The exact scalar path has no temporary geometry.  This protects the
			// reusable batch from malformed, pixel-area-sized cell dimensions.
			flush();
			DrawCommandImmediately(dc, command);
			continue;
		}
		if( !state.hasStyle || !(state.style == command.style) ) {
			flush();
			state.style = command.style;
			state.hasStyle = true;
		}
		if( state.rectangleCount + rectangleCount > kMaximumBatchRectangles ) flush();

		const auto appendRectangle = [&state](const RECT& rect) noexcept {
			if( state.rectangleCount == state.rectangles.size() ) return false;
			state.rectangles[state.rectangleCount++] = rect;
			return true;
		};
		BatchRasterSink<decltype(appendRectangle)> sink{ appendRectangle };
		DrawGlyph(sink, command.glyph, command.rect);
		if( command.style.underline ) sink.Fill(UnderlineRect(command.rect));
		if( sink.overflow ) {
			// A conservative estimator should make this unreachable.  The partial
			// same-color batch is safe to replay before the exact scalar fallback.
			flush();
			DrawCommandImmediately(dc, command);
		}
	}
	flush();
	::RestoreDC(dc, savedDc);
}

} // namespace terminal

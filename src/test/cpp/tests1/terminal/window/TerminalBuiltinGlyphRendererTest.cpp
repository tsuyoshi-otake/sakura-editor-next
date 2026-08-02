/*! @file */
#include "pch.h"

#include "terminal/window/TerminalBuiltinGlyphRenderer.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace terminal {
namespace {

class TestDib final {
public:
	TestDib() noexcept = default;
	~TestDib() noexcept { Close(); }
	TestDib(const TestDib&) = delete;
	TestDib& operator=(const TestDib&) = delete;

	bool Create(int width, int height) noexcept
	{
		Close();
		m_width = width;
		m_height = height;
		m_dc = ::CreateCompatibleDC(nullptr);
		if( m_dc == nullptr ) return false;
		BITMAPINFO info{};
		info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
		info.bmiHeader.biWidth = width;
		info.bmiHeader.biHeight = -height;
		info.bmiHeader.biPlanes = 1;
		info.bmiHeader.biBitCount = 32;
		info.bmiHeader.biCompression = BI_RGB;
		void* bits = nullptr;
		m_bitmap = ::CreateDIBSection(m_dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
		if( m_bitmap == nullptr || bits == nullptr ) {
			Close();
			return false;
		}
		m_bits = static_cast<std::uint32_t*>(bits);
		m_oldBitmap = ::SelectObject(m_dc, m_bitmap);
		if( m_oldBitmap == nullptr || m_oldBitmap == HGDI_ERROR ) {
			Close();
			return false;
		}
		return true;
	}

	void Fill(COLORREF color) noexcept
	{
		const auto brush = ::CreateSolidBrush(color);
		if( brush == nullptr ) return;
		const RECT rect{ 0, 0, m_width, m_height };
		::FillRect(m_dc, &rect, brush);
		::DeleteObject(brush);
	}

	[[nodiscard]] COLORREF Pixel(int x, int y) const noexcept
	{
		if( m_dc == nullptr || x < 0 || y < 0 || x >= m_width || y >= m_height ) return CLR_INVALID;
		return ::GetPixel(m_dc, x, y);
	}
	[[nodiscard]] HDC Dc() const noexcept { return m_dc; }
	[[nodiscard]] std::uint32_t* Bits() noexcept { return m_bits; }
	[[nodiscard]] const std::uint32_t* Bits() const noexcept { return m_bits; }
	[[nodiscard]] int Width() const noexcept { return m_width; }
	[[nodiscard]] int Height() const noexcept { return m_height; }

private:
	void Close() noexcept
	{
		if( m_dc != nullptr && m_oldBitmap != nullptr && m_oldBitmap != HGDI_ERROR ) {
			::SelectObject(m_dc, m_oldBitmap);
		}
		if( m_bitmap != nullptr ) ::DeleteObject(m_bitmap);
		if( m_dc != nullptr ) ::DeleteDC(m_dc);
		m_dc = nullptr;
		m_bitmap = nullptr;
		m_oldBitmap = nullptr;
		m_bits = nullptr;
	}

	HDC m_dc{};
	HBITMAP m_bitmap{};
	HGDIOBJ m_oldBitmap{};
	std::uint32_t* m_bits{};
	int m_width{};
	int m_height{};
};

std::size_t CountColor(const TestDib& dib, COLORREF color, const RECT& rect)
{
	std::size_t count{};
	for( int y = std::max(0L, rect.top); y < std::min(dib.Height(), static_cast<int>(rect.bottom)); ++y ) {
		for( int x = std::max(0L, rect.left); x < std::min(dib.Width(), static_cast<int>(rect.right)); ++x ) {
			if( dib.Pixel(x, y) == color ) ++count;
		}
	}
	return count;
}

std::uint32_t ColorRefToBgrx(COLORREF color) noexcept
{
	return (static_cast<std::uint32_t>(GetRValue(color)) << 16U) |
		(static_cast<std::uint32_t>(GetGValue(color)) << 8U) |
		static_cast<std::uint32_t>(GetBValue(color));
}

std::uint32_t RawPixel(const TestDib& dib, int x, int y) noexcept
{
	if( x < 0 || y < 0 || x >= dib.Width() || y >= dib.Height() ) return 0;
	return dib.Bits()[static_cast<std::size_t>(y) * static_cast<std::size_t>(dib.Width()) +
		static_cast<std::size_t>(x)];
}

TerminalBuiltinGlyphPixelSurface SurfaceFor(TestDib& dib, const RECT& clip) noexcept
{
	return {
		dib.Bits(),
		static_cast<std::ptrdiff_t>(dib.Width()),
		static_cast<LONG>(dib.Width()),
		static_cast<LONG>(dib.Height()),
		clip,
	};
}

struct DcState final {
	HGDIOBJ brush{};
	HGDIOBJ pen{};
	int mapMode{};
	int backgroundMode{};
	COLORREF textColor{};
	COLORREF backgroundColor{};
	COLORREF dcBrushColor{};
	POINT windowOrigin{};
	POINT viewportOrigin{};
	int clipType{};
	RECT clip{};
};

DcState CaptureDcState(HDC dc) noexcept
{
	DcState state{};
	state.brush = ::GetCurrentObject(dc, OBJ_BRUSH);
	state.pen = ::GetCurrentObject(dc, OBJ_PEN);
	state.mapMode = ::GetMapMode(dc);
	state.backgroundMode = ::GetBkMode(dc);
	state.textColor = ::GetTextColor(dc);
	state.backgroundColor = ::GetBkColor(dc);
	state.dcBrushColor = ::GetDCBrushColor(dc);
	::GetWindowOrgEx(dc, &state.windowOrigin);
	::GetViewportOrgEx(dc, &state.viewportOrigin);
	state.clipType = ::GetClipBox(dc, &state.clip);
	return state;
}

bool SelectNonzeroClip(HDC dc, const RECT& clip) noexcept
{
	const auto region = ::CreateRectRgn(clip.left, clip.top, clip.right, clip.bottom);
	if( region == nullptr ) return false;
	const auto result = ::SelectClipRgn(dc, region);
	::DeleteObject(region);
	return result != ERROR;
}

void DrawScalarCommand(HDC dc, const TerminalBuiltinGlyphCommand& command) noexcept
{
	TerminalBuiltinGlyphRenderer::Draw(dc, command);
	if( !command.style.underline ) return;
	const RECT underline{ command.rect.left, std::max(command.rect.top, command.rect.bottom - 2),
		command.rect.right, command.rect.bottom };
	if( underline.right <= underline.left || underline.bottom <= underline.top ) return;
	const auto brush = static_cast<HBRUSH>(::GetStockObject(DC_BRUSH));
	::SetDCBrushColor(dc, command.style.foreground);
	::FillRect(dc, &underline, brush);
}

std::size_t CountPixelMismatches(const TestDib& left, const TestDib& right,
	int& firstX, int& firstY) noexcept
{
	std::size_t mismatches{};
	firstX = -1;
	firstY = -1;
	for( int y = 0; y < left.Height(); ++y ) {
		for( int x = 0; x < left.Width(); ++x ) {
			if( left.Pixel(x, y) == right.Pixel(x, y) ) continue;
			if( mismatches == 0 ) {
				firstX = x;
				firstY = y;
			}
			++mismatches;
		}
	}
	return mismatches;
}

bool ConfigureComparisonDc(HDC dc, const RECT& clip) noexcept
{
	::SetMapMode(dc, MM_TEXT);
	::SetBkMode(dc, OPAQUE);
	::SetTextColor(dc, RGB(0x11, 0x22, 0x33));
	::SetBkColor(dc, RGB(0x44, 0x55, 0x66));
	::SetDCBrushColor(dc, RGB(0x77, 0x88, 0x99));
	return SelectNonzeroClip(dc, clip);
}

void ExpectSameDcState(const DcState& expected, const DcState& actual)
{
	EXPECT_EQ(expected.brush, actual.brush);
	EXPECT_EQ(expected.pen, actual.pen);
	EXPECT_EQ(expected.mapMode, actual.mapMode);
	EXPECT_EQ(expected.backgroundMode, actual.backgroundMode);
	EXPECT_EQ(expected.textColor, actual.textColor);
	EXPECT_EQ(expected.backgroundColor, actual.backgroundColor);
	EXPECT_EQ(expected.dcBrushColor, actual.dcBrushColor);
	EXPECT_EQ(expected.windowOrigin.x, actual.windowOrigin.x);
	EXPECT_EQ(expected.windowOrigin.y, actual.windowOrigin.y);
	EXPECT_EQ(expected.viewportOrigin.x, actual.viewportOrigin.x);
	EXPECT_EQ(expected.viewportOrigin.y, actual.viewportOrigin.y);
	EXPECT_EQ(expected.clipType, actual.clipType);
	EXPECT_EQ(expected.clip.left, actual.clip.left);
	EXPECT_EQ(expected.clip.top, actual.clip.top);
	EXPECT_EQ(expected.clip.right, actual.clip.right);
	EXPECT_EQ(expected.clip.bottom, actual.clip.bottom);
}

TEST(TerminalBuiltinGlyphRenderer, HandlesEveryBoxBlockShadeScalar)
{
	for( char32_t glyph = 0x2500; glyph <= 0x259F; ++glyph ) {
		EXPECT_TRUE(TerminalBuiltinGlyphRenderer::Handles(glyph)) << std::hex << static_cast<unsigned int>(glyph);
	}
	EXPECT_FALSE(TerminalBuiltinGlyphRenderer::Handles(0x24FF));
	EXPECT_FALSE(TerminalBuiltinGlyphRenderer::Handles(0x25A0));
}

TEST(TerminalBuiltinGlyphRenderer, RasterizesCompleteRangeWithinAbsoluteCellAtSupportedDpis)
{
	constexpr COLORREF background = RGB(0x28, 0x2C, 0x34);
	constexpr COLORREF foreground = RGB(0xDC, 0xDF, 0xE4);
	for( const int dpi : { 96, 120, 144, 192 } ) {
		const int width = std::max(8, ::MulDiv(16, dpi, 96));
		const int height = std::max(8, ::MulDiv(16, dpi, 96));
		TestDib dib;
		ASSERT_TRUE(dib.Create(width + 32, height + 32));
		const RECT cell{ 11, 7, 11 + width, 7 + height };
		for( char32_t glyph = 0x2500; glyph <= 0x259F; ++glyph ) {
			dib.Fill(background);
			TerminalBuiltinGlyphRenderer::Draw(dib.Dc(), glyph, cell, foreground);
			EXPECT_GT(CountColor(dib, foreground, cell), 0u) << "dpi=" << dpi << " glyph=" << std::hex
				<< static_cast<unsigned int>(glyph);
			// The renderer receives absolute cell coordinates and must never paint
			// outside that cell, even when the DIB has a nonzero origin margin.
			EXPECT_EQ(0u, CountColor(dib, foreground, { 0, 0, cell.left, dib.Height() }))
				<< "dpi=" << dpi << " glyph=" << std::hex << static_cast<unsigned int>(glyph);
			EXPECT_EQ(0u, CountColor(dib, foreground, { cell.right, 0, dib.Width(), dib.Height() }))
				<< "dpi=" << dpi << " glyph=" << std::hex << static_cast<unsigned int>(glyph);
			EXPECT_EQ(0u, CountColor(dib, foreground, { 0, 0, dib.Width(), cell.top }))
				<< "dpi=" << dpi << " glyph=" << std::hex << static_cast<unsigned int>(glyph);
			EXPECT_EQ(0u, CountColor(dib, foreground, { 0, cell.bottom, dib.Width(), dib.Height() }))
				<< "dpi=" << dpi << " glyph=" << std::hex << static_cast<unsigned int>(glyph);
		}
	}
}

TEST(TerminalBuiltinGlyphRenderer, AdjacentCellsShareBoxStrokeEdgeAtSupportedDpis)
{
	constexpr COLORREF background = RGB(0x28, 0x2C, 0x34);
	constexpr COLORREF foreground = RGB(0xDC, 0xDF, 0xE4);
	for( const int dpi : { 96, 120, 144, 192 } ) {
		const int width = std::max(8, ::MulDiv(16, dpi, 96));
		const int height = std::max(8, ::MulDiv(16, dpi, 96));
		TestDib dib;
		ASSERT_TRUE(dib.Create(width * 2 + 32, height + 32));
		const RECT leftCell{ 11, 7, 11 + width, 7 + height };
		const RECT rightCell{ leftCell.right, leftCell.top, leftCell.right + width, leftCell.bottom };
		const int centerY = leftCell.top + height / 2;
		dib.Fill(background);
		TerminalBuiltinGlyphRenderer::Draw(dib.Dc(), 0x2500, leftCell, foreground);
		TerminalBuiltinGlyphRenderer::Draw(dib.Dc(), 0x2500, rightCell, foreground);
		// Both matching box strokes reach the shared edge.  A missing pixel at
		// either side would be a one-pixel background seam between cells.
		for( int x = leftCell.left; x < rightCell.right; ++x ) {
			EXPECT_EQ(foreground, dib.Pixel(x, centerY)) << "dpi=" << dpi << " x=" << x;
		}
	}
}

TEST(TerminalBuiltinGlyphRenderer, PreservesRepresentativeBoundaryGeometry)
{
	constexpr COLORREF background = RGB(0x28, 0x2C, 0x34);
	constexpr COLORREF foreground = RGB(0xDC, 0xDF, 0xE4);
	TestDib dib;
	ASSERT_TRUE(dib.Create(64, 64));
	const RECT cell{ 9, 11, 41, 43 };
	const int centerX = cell.left + (cell.right - cell.left) / 2;
	const int centerY = cell.top + (cell.bottom - cell.top) / 2;

	dib.Fill(background);
	TerminalBuiltinGlyphRenderer::Draw(dib.Dc(), 0x2500, cell, foreground);
	EXPECT_EQ(foreground, dib.Pixel(cell.left, centerY)); // light horizontal boundary

	dib.Fill(background);
	TerminalBuiltinGlyphRenderer::Draw(dib.Dc(), 0x2501, cell, foreground);
	EXPECT_EQ(foreground, dib.Pixel(centerX, centerY)); // heavy horizontal core

	dib.Fill(background);
	TerminalBuiltinGlyphRenderer::Draw(dib.Dc(), 0x2550, cell, foreground);
	EXPECT_EQ(foreground, dib.Pixel(centerX, centerY - 8)); // double-line pair

	// Unicode defines U+2571 as upper-right to lower-left.  Assert both
	// endpoints and both opposite corners so a mirrored diagonal cannot pass.
	dib.Fill(background);
	TerminalBuiltinGlyphRenderer::Draw(dib.Dc(), 0x2571, cell, foreground);
	EXPECT_EQ(foreground, dib.Pixel(cell.right - 1, cell.top));
	EXPECT_EQ(foreground, dib.Pixel(cell.left, cell.bottom - 1));
	EXPECT_EQ(background, dib.Pixel(cell.left, cell.top));
	EXPECT_EQ(background, dib.Pixel(cell.right - 1, cell.bottom - 1));

	// U+2572 is the opposite orientation: upper-left to lower-right.
	dib.Fill(background);
	TerminalBuiltinGlyphRenderer::Draw(dib.Dc(), 0x2572, cell, foreground);
	EXPECT_EQ(foreground, dib.Pixel(cell.left, cell.top));
	EXPECT_EQ(foreground, dib.Pixel(cell.right - 1, cell.bottom - 1));
	EXPECT_EQ(background, dib.Pixel(cell.right - 1, cell.top));
	EXPECT_EQ(background, dib.Pixel(cell.left, cell.bottom - 1));

	dib.Fill(background);
	TerminalBuiltinGlyphRenderer::Draw(dib.Dc(), 0x2588, cell, foreground);
	EXPECT_EQ((cell.right - cell.left) * (cell.bottom - cell.top),
		static_cast<int>(CountColor(dib, foreground, cell))); // full block

	dib.Fill(background);
	TerminalBuiltinGlyphRenderer::Draw(dib.Dc(), 0x2596, cell, foreground);
	EXPECT_EQ(foreground, dib.Pixel(cell.left, cell.top + (cell.bottom - cell.top) * 3 / 4)); // lower-left quadrant
}

TEST(TerminalBuiltinGlyphRenderer, ShadeCoverageTracksQuarterHalfAndThreeQuarterRatios)
{
	constexpr COLORREF background = RGB(0x28, 0x2C, 0x34);
	constexpr COLORREF foreground = RGB(0xDC, 0xDF, 0xE4);
	TestDib dib;
	ASSERT_TRUE(dib.Create(64, 64));
	const RECT cell{ 8, 8, 56, 56 };
	const double area = static_cast<double>((cell.right - cell.left) * (cell.bottom - cell.top));
	for( const auto [glyph, expected] : { std::pair<char32_t, double>{ 0x2591, .25 },
		std::pair<char32_t, double>{ 0x2592, .50 }, std::pair<char32_t, double>{ 0x2593, .75 } } ) {
		dib.Fill(background);
		TerminalBuiltinGlyphRenderer::Draw(dib.Dc(), glyph, cell, foreground);
		const auto ratio = static_cast<double>(CountColor(dib, foreground, cell)) / area;
		EXPECT_NEAR(expected, ratio, 0.04) << "glyph=" << std::hex << static_cast<unsigned int>(glyph);
	}
}

TEST(TerminalBuiltinGlyphRenderer, BatchMatchesScalarForCompleteRangeStylesAndClippedGeometry)
{
	constexpr COLORREF background = RGB(0x28, 0x2C, 0x34);
	constexpr std::array foregrounds{
		RGB(0xDC, 0xDF, 0xE4), RGB(0xFF, 0xA0, 0x40), RGB(0x80, 0xD0, 0xFF), RGB(0xA0, 0xFF, 0x80),
	};
	constexpr int columns = 20;
	constexpr int rows = 8;
	constexpr int margin = 11;
	for( const int dpi : { 96, 120, 144, 192 } ) {
		const int cellWidth = std::max(8, ::MulDiv(16, dpi, 96));
		const int cellHeight = std::max(8, ::MulDiv(16, dpi, 96));
		const int gridWidth = columns * cellWidth;
		const int gridHeight = rows * cellHeight;
		TestDib scalar;
		TestDib batch;
		ASSERT_TRUE(scalar.Create(gridWidth + margin * 2, gridHeight + margin * 2));
		ASSERT_TRUE(batch.Create(gridWidth + margin * 2, gridHeight + margin * 2));
		scalar.Fill(background);
		batch.Fill(background);
		const RECT clip{ margin, margin, margin + gridWidth, margin + gridHeight };
		ASSERT_TRUE(ConfigureComparisonDc(scalar.Dc(), clip));
		ASSERT_TRUE(ConfigureComparisonDc(batch.Dc(), clip));

		std::vector<TerminalBuiltinGlyphCommand> commands;
		commands.reserve(0xA0);
		for( std::size_t index = 0; index < 0xA0; ++index ) {
			const auto styleIndex = (index / 3U) % foregrounds.size();
			const RECT rect{
				margin + static_cast<int>(index % columns) * cellWidth,
				margin + static_cast<int>(index / columns) * cellHeight,
				margin + static_cast<int>(index % columns + 1U) * cellWidth,
				margin + static_cast<int>(index / columns + 1U) * cellHeight,
			};
			TerminalRenderStyle style{ foregrounds[styleIndex], background, false,
				styleIndex == 1U || styleIndex == 3U, false, false };
			commands.push_back({ rect, static_cast<char32_t>(0x2500U + index), style });
		}
		for( const auto& command : commands ) DrawScalarCommand(scalar.Dc(), command);

		const auto stateBefore = CaptureDcState(batch.Dc());
		const auto gdiObjectsBefore = ::GetGuiResources(::GetCurrentProcess(), GR_GDIOBJECTS);
		TerminalBuiltinGlyphRenderer renderer;
		renderer.DrawBatch(batch.Dc(), commands);
		const auto gdiObjectsAfter = ::GetGuiResources(::GetCurrentProcess(), GR_GDIOBJECTS);
		const auto stateAfter = CaptureDcState(batch.Dc());
		int firstX{};
		int firstY{};
		const auto mismatches = CountPixelMismatches(scalar, batch, firstX, firstY);
		EXPECT_EQ(0u, mismatches) << "dpi=" << dpi << " first mismatch=" << firstX << "," << firstY;
		EXPECT_EQ(gdiObjectsBefore, gdiObjectsAfter) << "dpi=" << dpi;
		ExpectSameDcState(stateBefore, stateAfter);
	}
}

TEST(TerminalBuiltinGlyphRenderer, BatchMatchesScalarAtOversizedDiagonalAndShadeFallbackBoundary)
{
	constexpr COLORREF background = RGB(0x28, 0x2C, 0x34);
	constexpr COLORREF foreground = RGB(0xDC, 0xDF, 0xE4);
	TestDib scalar;
	TestDib batch;
	ASSERT_TRUE(scalar.Create(380, 120));
	ASSERT_TRUE(batch.Create(380, 120));
	scalar.Fill(background);
	batch.Fill(background);
	const TerminalRenderStyle style{ foreground, background, false, true, false, false };
	// U+2571 has 257 diagonal steps (>256); U+2591 is a 32x32 shade cell
	// (1024 scalar pixels), so both must take the exact scalar fallback path.
	const std::array commands{
		TerminalBuiltinGlyphCommand{ { 17, 13, 274, 45 }, 0x2571, style },
		TerminalBuiltinGlyphCommand{ { 17, 61, 49, 93 }, 0x2591, style },
	};
	for( const auto& command : commands ) DrawScalarCommand(scalar.Dc(), command);
	const auto stateBefore = CaptureDcState(batch.Dc());
	const auto gdiObjectsBefore = ::GetGuiResources(::GetCurrentProcess(), GR_GDIOBJECTS);
	TerminalBuiltinGlyphRenderer renderer;
	renderer.DrawBatch(batch.Dc(), commands);
	const auto gdiObjectsAfter = ::GetGuiResources(::GetCurrentProcess(), GR_GDIOBJECTS);
	const auto stateAfter = CaptureDcState(batch.Dc());
	int firstX{};
	int firstY{};
	const auto mismatches = CountPixelMismatches(scalar, batch, firstX, firstY);
	EXPECT_EQ(0u, mismatches) << "first mismatch=" << firstX << "," << firstY;
	EXPECT_EQ(gdiObjectsBefore, gdiObjectsAfter);
	ExpectSameDcState(stateBefore, stateAfter);
}

TEST(TerminalBuiltinGlyphRenderer, DirectPixelBatchMatchesScalarForCompleteRangeStylesAtSupportedDpis)
{
	constexpr COLORREF background = RGB(0x28, 0x2C, 0x34);
	constexpr std::array foregrounds{
		RGB(0xDC, 0xDF, 0xE4), RGB(0xFF, 0xA0, 0x40), RGB(0x80, 0xD0, 0xFF), RGB(0xA0, 0xFF, 0x80),
	};
	constexpr int columns = 20;
	constexpr int rows = 8;
	constexpr int margin = 13;
	for( const int dpi : { 96, 120, 144, 192 } ) {
		const int cellWidth = std::max(8, ::MulDiv(16, dpi, 96));
		const int cellHeight = std::max(8, ::MulDiv(16, dpi, 96));
		const int gridWidth = columns * cellWidth;
		const int gridHeight = rows * cellHeight;
		TestDib scalar;
		TestDib direct;
		ASSERT_TRUE(scalar.Create(gridWidth + margin * 2, gridHeight + margin * 2));
		ASSERT_TRUE(direct.Create(gridWidth + margin * 2, gridHeight + margin * 2));
		scalar.Fill(background);
		direct.Fill(background);
		const RECT clip{ margin, margin, margin + gridWidth, margin + gridHeight };
		ASSERT_TRUE(ConfigureComparisonDc(scalar.Dc(), clip));

		std::vector<TerminalBuiltinGlyphCommand> commands;
		commands.reserve(0xA0);
		for( std::size_t index = 0; index < 0xA0; ++index ) {
			const auto styleIndex = (index / 3U) % foregrounds.size();
			const RECT rect{
				margin + static_cast<int>(index % columns) * cellWidth,
				margin + static_cast<int>(index / columns) * cellHeight,
				margin + static_cast<int>(index % columns + 1U) * cellWidth,
				margin + static_cast<int>(index / columns + 1U) * cellHeight,
			};
			const TerminalRenderStyle style{ foregrounds[styleIndex], background, false,
				styleIndex == 1U || styleIndex == 3U, false, false };
			commands.push_back({ rect, static_cast<char32_t>(0x2500U + index), style });
		}
		for( const auto& command : commands ) DrawScalarCommand(scalar.Dc(), command);
		::GdiFlush();
		const auto surface = SurfaceFor(direct, clip);
		ASSERT_TRUE(TerminalBuiltinGlyphRenderer::DrawBatch(surface, commands));

		for( int y = 0; y < scalar.Height(); ++y ) {
			for( int x = 0; x < scalar.Width(); ++x ) {
				EXPECT_EQ(RawPixel(scalar, x, y), RawPixel(direct, x, y))
					<< "dpi=" << dpi << " x=" << x << " y=" << y;
			}
		}
		EXPECT_EQ(ColorRefToBgrx(background), RawPixel(direct, 0, 0)) << "dpi=" << dpi;
	}
}

TEST(TerminalBuiltinGlyphRenderer, DirectPixelBatchPreservesNarrowClipAndPositiveNegativeStride)
{
	constexpr int width = 23;
	constexpr int height = 19;
	constexpr std::uint32_t sentinel = 0xA1B2C3D4U;
	constexpr COLORREF foreground = RGB(0x12, 0x34, 0x56);
	const RECT clip{ 5, 7, 17, 14 };
	const TerminalBuiltinGlyphCommand command{
		{ 0, 0, width, height }, 0x2588, { foreground, RGB(0, 0, 0), false, false, false, false },
	};
	std::vector<std::uint32_t> positive(static_cast<std::size_t>(width * height), sentinel);
	std::vector<std::uint32_t> negative(static_cast<std::size_t>(width * height), sentinel);
	const TerminalBuiltinGlyphPixelSurface positiveSurface{
		positive.data(), width, width, height, clip,
	};
	const TerminalBuiltinGlyphPixelSurface negativeSurface{
		negative.data() + static_cast<std::size_t>(height - 1) * width,
		-width, width, height, clip,
	};
	ASSERT_TRUE(TerminalBuiltinGlyphRenderer::DrawBatch(positiveSurface, { &command, 1 }));
	ASSERT_TRUE(TerminalBuiltinGlyphRenderer::DrawBatch(negativeSurface, { &command, 1 }));
	const auto expected = ColorRefToBgrx(foreground);
	for( int y = 0; y < height; ++y ) {
		for( int x = 0; x < width; ++x ) {
			const auto positivePixel = positive[static_cast<std::size_t>(y) * width + x];
			const auto negativePixel = negative[static_cast<std::size_t>(height - 1 - y) * width + x];
			EXPECT_EQ(positivePixel, negativePixel) << "x=" << x << " y=" << y;
			EXPECT_EQ((x >= clip.left && x < clip.right && y >= clip.top && y < clip.bottom)
				? expected : sentinel, positivePixel) << "x=" << x << " y=" << y;
		}
	}
}

TEST(TerminalBuiltinGlyphRenderer, DirectPixelBatchStoresExactBgrxColor)
{
	constexpr COLORREF background = RGB(0xA0, 0xB0, 0xC0);
	constexpr COLORREF foreground = RGB(0x12, 0x34, 0x56);
	TestDib dib;
	ASSERT_TRUE(dib.Create(12, 10));
	dib.Fill(background);
	::GdiFlush();
	const RECT clip{ 0, 0, dib.Width(), dib.Height() };
	const TerminalBuiltinGlyphCommand command{
		{ 2, 1, 10, 8 }, 0x2588, { foreground, background, false, false, false, false },
	};
	ASSERT_TRUE(TerminalBuiltinGlyphRenderer::DrawBatch(SurfaceFor(dib, clip), { &command, 1 }));
	const auto backgroundBgrx = ColorRefToBgrx(background);
	const auto foregroundBgrx = ColorRefToBgrx(foreground);
	for( int y = 0; y < dib.Height(); ++y ) {
		for( int x = 0; x < dib.Width(); ++x ) {
			const auto expected = x >= command.rect.left && x < command.rect.right &&
				y >= command.rect.top && y < command.rect.bottom ? foregroundBgrx : backgroundBgrx;
			EXPECT_EQ(expected, RawPixel(dib, x, y)) << "x=" << x << " y=" << y;
		}
	}
}

TEST(TerminalBuiltinGlyphRenderer, DirectPixelBatchPreflightsInvalidSurfaceAndFullBatch)
{
	constexpr std::uint32_t sentinel = 0x10203040U;
	constexpr COLORREF foreground = RGB(0xDC, 0xDF, 0xE4);
	const TerminalBuiltinGlyphCommand valid{
		{ 0, 0, 2, 2 }, 0x2588, { foreground, RGB(0, 0, 0), false, false, false, false },
	};
	const TerminalBuiltinGlyphCommand invalidGlyph{
		{ 0, 0, 2, 2 }, 0x25A0, valid.style,
	};
	const TerminalBuiltinGlyphCommand invalidRect{
		{ 2, 2, 2, 4 }, 0x2588, valid.style,
	};
	const TerminalBuiltinGlyphCommand oversized{
		{ 0, 0, 300, 300 }, 0x2591, valid.style,
	};

	auto expectRejectedWithoutWrites = [&](TerminalBuiltinGlyphPixelSurface surface,
		std::span<const TerminalBuiltinGlyphCommand> commands, std::vector<std::uint32_t>& storage) {
		std::fill(storage.begin(), storage.end(), sentinel);
		const auto before = storage;
		EXPECT_FALSE(TerminalBuiltinGlyphRenderer::DrawBatch(surface, commands));
		EXPECT_EQ(before, storage);
	};

	std::vector<std::uint32_t> storage(16, sentinel);
	expectRejectedWithoutWrites({ nullptr, 4, 4, 4, { 0, 0, 4, 4 } }, { &valid, 1 }, storage);
	expectRejectedWithoutWrites({ storage.data(), 0, 4, 4, { 0, 0, 4, 4 } }, { &valid, 1 }, storage);
	expectRejectedWithoutWrites({ storage.data(), 3, 4, 4, { 0, 0, 4, 4 } }, { &valid, 1 }, storage);
	expectRejectedWithoutWrites({ storage.data(), std::numeric_limits<std::ptrdiff_t>::min(), 4, 4,
		{ 0, 0, 4, 4 } }, { &valid, 1 }, storage);

	std::vector<TerminalBuiltinGlyphCommand> malformed{ valid, invalidGlyph };
	expectRejectedWithoutWrites({ storage.data(), 4, 4, 4, { 0, 0, 4, 4 } }, malformed, storage);
	malformed = { valid, invalidRect };
	expectRejectedWithoutWrites({ storage.data(), 4, 4, 4, { 0, 0, 4, 4 } }, malformed, storage);
	std::vector<std::uint32_t> largeStorage(300U * 300U, sentinel);
	malformed = { valid, oversized };
	expectRejectedWithoutWrites({ largeStorage.data(), 300, 300, 300, { 0, 0, 300, 300 } }, malformed, largeStorage);
}

} // namespace
} // namespace terminal

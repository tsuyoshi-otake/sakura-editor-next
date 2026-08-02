/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include "terminal/window/TerminalLegacyRendererBaseline.h"
#include "terminal/model/TerminalModel.h"
#include "terminal/window/TerminalBuiltinGlyphRenderer.h"
#include "terminal/window/TerminalDWriteRenderer.h"
#include "terminal/window/TerminalRenderPlan.h"

#include <Windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <dwrite_2.h>
#include <gdiplus.h>
#include <psapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cmath>
#include <cstdint>
#include <chrono>
#include <iomanip>
#include <limits>
#include <locale>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "psapi.lib")

namespace terminal {
namespace {

using Microsoft::WRL::ComPtr;

constexpr std::array<std::uint32_t, 4> kSupportedDpis{ 96, 120, 144, 192 };
constexpr std::uint32_t kMinimumDimension = 1;
constexpr std::uint32_t kDefaultVisibleColumns = 120;
constexpr std::uint32_t kDefaultVisibleRows = 8;

struct Geometry final {
	std::uint32_t fontPixelHeight{};
	std::uint32_t cellWidth{};
	std::uint32_t cellHeight{};
};

Geometry GeometryForDpi(std::uint32_t dpi) noexcept
{
	switch( dpi ) {
	case 96: return { 12, 7, 14 };
	case 120: return { 15, 9, 18 };
	case 144: return { 18, 11, 22 };
	case 192: return { 24, 14, 29 };
	default:
		if( dpi == 0 ) dpi = 96;
		return {
			std::max(1u, static_cast<unsigned int>(::MulDiv(9, static_cast<int>(dpi), 72))),
			std::max(1u, static_cast<unsigned int>(::MulDiv(7, static_cast<int>(dpi), 96))),
			std::max(1u, static_cast<unsigned int>(std::lround(14.0 * static_cast<double>(dpi) / 96.0)))
		};
	}
}

bool IsSupportedDpi(std::uint32_t dpi) noexcept
{
	return std::find(kSupportedDpis.begin(), kSupportedDpis.end(), dpi) != kSupportedDpis.end();
}

struct ProcessStats final {
	std::int64_t privateBytes{};
	std::int64_t gdiObjects{};
};

ProcessStats ReadProcessStats() noexcept
{
	ProcessStats stats;
	PROCESS_MEMORY_COUNTERS_EX counters{};
	if( ::GetProcessMemoryInfo(::GetCurrentProcess(),
		reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters)) != FALSE ) {
		stats.privateBytes = static_cast<std::int64_t>(counters.PrivateUsage);
	}
	// The first query lazily initializes the per-process GDI accounting on a
	// console test process.  Read twice so before/after snapshots share the
	// same accounting baseline and the delta describes renderer-owned objects.
	static_cast<void>(::GetGuiResources(::GetCurrentProcess(), GR_GDIOBJECTS));
	stats.gdiObjects = static_cast<std::int64_t>(::GetGuiResources(::GetCurrentProcess(), GR_GDIOBJECTS));
	return stats;
}

class ScopedCom final {
public:
	ScopedCom() noexcept
		: m_result(::CoInitializeEx(nullptr, COINIT_MULTITHREADED))
	{
		m_uninitialize = SUCCEEDED(m_result);
	}
	~ScopedCom() noexcept
	{
		if( m_uninitialize ) ::CoUninitialize();
	}
	ScopedCom(const ScopedCom&) = delete;
	ScopedCom& operator=(const ScopedCom&) = delete;
	[[nodiscard]] HRESULT Result() const noexcept { return m_result; }

private:
	HRESULT m_result{};
	bool m_uninitialize{};
};

class OffscreenDib final {
public:
	OffscreenDib() = default;
	~OffscreenDib() noexcept { Close(); }
	OffscreenDib(const OffscreenDib&) = delete;
	OffscreenDib& operator=(const OffscreenDib&) = delete;

	bool Create(std::uint32_t width, std::uint32_t height) noexcept
	{
		Close();
		m_width = std::max(kMinimumDimension, width);
		m_height = std::max(kMinimumDimension, height);
		m_dc = ::CreateCompatibleDC(nullptr);
		if( m_dc == nullptr ) return false;
		BITMAPINFO info{};
		info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
		info.bmiHeader.biWidth = static_cast<LONG>(m_width);
		info.bmiHeader.biHeight = -static_cast<LONG>(m_height); // top-down DIB
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
		m_oldBitmap = static_cast<HBITMAP>(::SelectObject(m_dc, m_bitmap));
		if( m_oldBitmap == nullptr || m_oldBitmap == HGDI_ERROR ) {
			Close();
			return false;
		}
		return true;
	}

	void Close() noexcept
	{
		if( m_dc != nullptr && m_oldBitmap != nullptr && m_oldBitmap != HGDI_ERROR ) {
			::SelectObject(m_dc, m_oldBitmap);
		}
		if( m_bitmap != nullptr ) {
			::DeleteObject(m_bitmap);
			m_bitmap = nullptr;
		}
		m_bits = nullptr;
		if( m_dc != nullptr ) {
			::DeleteDC(m_dc);
			m_dc = nullptr;
		}
		m_oldBitmap = nullptr;
	}

	void Fill(COLORREF color) noexcept
	{
		if( m_dc == nullptr ) return;
		RECT rect{ 0, 0, static_cast<LONG>(m_width), static_cast<LONG>(m_height) };
		const auto brush = ::CreateSolidBrush(color);
		if( brush != nullptr ) {
			::FillRect(m_dc, &rect, brush);
			::DeleteObject(brush);
		}
	}

	[[nodiscard]] HDC Dc() const noexcept { return m_dc; }
	[[nodiscard]] std::uint32_t* Bits() const noexcept { return m_bits; }
	[[nodiscard]] std::uint32_t Width() const noexcept { return m_width; }
	[[nodiscard]] std::uint32_t Height() const noexcept { return m_height; }

private:
	HDC m_dc{};
	HBITMAP m_bitmap{};
	HBITMAP m_oldBitmap{};
	std::uint32_t* m_bits{};
	std::uint32_t m_width{};
	std::uint32_t m_height{};
};

void WarmGdiAccounting() noexcept
{
	// A console-hosted test process lazily creates one process-owned GDI
	// accounting object on its first DIB/DC use. Establish that setup baseline
	// before taking the renderer snapshot so the reported delta covers only
	// objects retained by the renderer itself.
	OffscreenDib warmup;
	if( warmup.Create(1, 1) ) {
		warmup.Fill(RGB(0, 0, 0));
		warmup.Close();
	}
	::GdiFlush();
}

bool HasFontFamily(HDC dc, std::wstring_view family) noexcept
{
	if( dc == nullptr || family.empty() ) return false;
	const auto font = ::CreateFontW(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
		DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_NATURAL_QUALITY,
		FIXED_PITCH | FF_MODERN, std::wstring(family).c_str());
	if( font == nullptr ) return false;
	const auto old = static_cast<HFONT>(::SelectObject(dc, font));
	wchar_t actual[LF_FACESIZE]{};
	const auto length = ::GetTextFaceW(dc, LF_FACESIZE, actual);
	if( old != nullptr && old != HGDI_ERROR ) ::SelectObject(dc, old);
	const auto deleted = ::DeleteObject(font);
	if( deleted == FALSE || length == 0 ) return false;
	return ::_wcsicmp(actual, std::wstring(family).c_str()) == 0;
}

std::wstring SelectFontFamily(HDC dc, const TerminalLegacyRendererConditions& conditions) noexcept
{
	if( HasFontFamily(dc, conditions.preferredFont) ) return conditions.preferredFont;
	if( HasFontFamily(dc, conditions.fallbackFont) ) return conditions.fallbackFont;
	return {};
}

std::wstring Utf16(std::uint32_t scalar)
{
	if( scalar <= 0xFFFF ) return std::wstring(1, static_cast<wchar_t>(scalar));
	scalar -= 0x10000;
	return {
		static_cast<wchar_t>(0xD800 + (scalar >> 10)),
		static_cast<wchar_t>(0xDC00 + (scalar & 0x3FF))
	};
}

void AppendAscii(std::vector<TerminalLegacyRendererCell>& result, std::wstring_view text)
{
	for( const auto character : text ) result.push_back({ std::wstring(1, character), 1, false });
}

void AppendTui(std::vector<TerminalLegacyRendererCell>& result)
{
	for( std::uint32_t scalar = 0x2500; scalar <= 0x257F; ++scalar ) {
		result.push_back({ Utf16(scalar), 1, false });
	}
	for( std::uint32_t scalar = 0x2580; scalar <= 0x259F; ++scalar ) {
		result.push_back({ Utf16(scalar), 1, false });
	}
}

void AppendMixedPattern(std::vector<TerminalLegacyRendererCell>& result)
{
	// Keep each grapheme as one cell while retaining its independent UTF-16
	// length.  The first two scalars are the exact prompt text required by the
	// baseline corpus contract: U+23F5 U+23F5 followed by ASCII words.
	result.push_back({ L"\u23F5", 1, false });
	result.push_back({ L"\u23F5", 1, false });
	AppendAscii(result, L" bypass permissions on ");
	result.push_back({ L"\u65E5", 2, false });
	result.push_back({ L"\u672C", 2, false });
	result.push_back({ L"\u8A9E", 2, false });
	AppendAscii(result, L" ");
	result.push_back({ L"e\u0301", 1, false });
	AppendAscii(result, L" ");
	result.push_back({ L"\u2764\uFE0F", 2, false });
	AppendAscii(result, L" ");
	result.push_back({ L"\U0001F469\u200D\U0001F4BB", 2, false });
	AppendAscii(result, L" ");
	result.push_back({ L"\U0001F1EF\U0001F1F5", 2, false });
	AppendAscii(result, L" ");
	result.push_back({ L"\U0001F44D\U0001F3FD", 2, false });
	AppendAscii(result, L" ");
}

struct PlacedCell final {
	std::wstring_view text;
	LONG x{};
	LONG y{};
	LONG width{};
	LONG height{};
	std::uint8_t occupiedColumns{};
};

std::vector<PlacedCell> PlaceCells(const std::vector<TerminalLegacyRendererCell>& cells,
	const TerminalLegacyRendererConditions& conditions, const Geometry& geometry)
{
	std::vector<PlacedCell> placed;
	placed.reserve(static_cast<std::size_t>(conditions.columns) * conditions.rows);
	std::uint32_t column = 0;
	std::uint32_t row = 0;
	for( const auto& cell : cells ) {
		if( cell.continuation ) continue;
		const auto occupiedColumns = std::max<std::uint8_t>(1, cell.occupiedColumns);
		if( occupiedColumns > conditions.columns ) continue;
		if( column + occupiedColumns > conditions.columns ) {
			column = 0;
			++row;
		}
		if( row >= conditions.rows ) break;
		placed.push_back({
			cell.text,
			static_cast<LONG>(column * geometry.cellWidth),
			static_cast<LONG>(row * geometry.cellHeight),
			static_cast<LONG>(occupiedColumns * geometry.cellWidth),
			static_cast<LONG>(geometry.cellHeight),
			occupiedColumns,
		});
		column += occupiedColumns;
	}
	return placed;
}

struct DirectWriteRun final {
	std::wstring text;
	LONG x{};
	LONG y{};
	LONG width{};
	LONG height{};
};

class DirectWriteTextAnalysisSource final : public IDWriteTextAnalysisSource {
public:
	explicit DirectWriteTextAnalysisSource(std::wstring_view text) noexcept
		: m_text(text)
	{
	}
	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) noexcept override
	{
		if( object == nullptr ) return E_POINTER;
		*object = nullptr;
		if( iid == __uuidof(IUnknown) || iid == __uuidof(IDWriteTextAnalysisSource) ) {
			*object = static_cast<IDWriteTextAnalysisSource*>(this);
			AddRef();
			return S_OK;
		}
		return E_NOINTERFACE;
	}
	ULONG STDMETHODCALLTYPE AddRef() noexcept override { return static_cast<ULONG>(::InterlockedIncrement(&m_references)); }
	ULONG STDMETHODCALLTYPE Release() noexcept override
	{
		const auto references = static_cast<ULONG>(::InterlockedDecrement(&m_references));
		if( references == 0 ) delete this;
		return references;
	}
	HRESULT STDMETHODCALLTYPE GetTextAtPosition(UINT32 position, WCHAR const** text, UINT32* length) noexcept override
	{
		if( text == nullptr || length == nullptr ) return E_POINTER;
		if( position >= m_text.size() ) {
			*text = nullptr;
			*length = 0;
		} else {
			*text = m_text.data() + position;
			*length = static_cast<UINT32>(m_text.size() - position);
		}
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE GetTextBeforePosition(UINT32 position, WCHAR const** text, UINT32* length) noexcept override
	{
		if( text == nullptr || length == nullptr ) return E_POINTER;
		const auto bounded = std::min<std::size_t>(position, m_text.size());
		if( bounded == 0 ) {
			*text = nullptr;
			*length = 0;
		} else {
			*text = m_text.data();
			*length = static_cast<UINT32>(bounded);
		}
		return S_OK;
	}
	DWRITE_READING_DIRECTION STDMETHODCALLTYPE GetParagraphReadingDirection() noexcept override
	{
		return DWRITE_READING_DIRECTION_LEFT_TO_RIGHT;
	}
	HRESULT STDMETHODCALLTYPE GetLocaleName(UINT32, UINT32* length, WCHAR const** localeName) noexcept override
	{
		if( length == nullptr || localeName == nullptr ) return E_POINTER;
		static constexpr wchar_t locale[] = L"en-us";
		*length = static_cast<UINT32>(m_text.size());
		*localeName = locale;
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE GetNumberSubstitution(UINT32, UINT32* length,
		IDWriteNumberSubstitution** substitution) noexcept override
	{
		if( length == nullptr || substitution == nullptr ) return E_POINTER;
		*length = static_cast<UINT32>(m_text.size());
		*substitution = nullptr;
		return S_OK;
	}

	~DirectWriteTextAnalysisSource() = default;

	private:
	std::wstring_view m_text;
	volatile LONG m_references{ 1 };
};

class DirectWriteTextAnalysisSink final : public IDWriteTextAnalysisSink {
public:
	DirectWriteTextAnalysisSink() noexcept = default;
	~DirectWriteTextAnalysisSink() = default;
	DirectWriteTextAnalysisSink(const DirectWriteTextAnalysisSink&) = delete;
	DirectWriteTextAnalysisSink& operator=(const DirectWriteTextAnalysisSink&) = delete;

	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) noexcept override
	{
		if( object == nullptr ) return E_POINTER;
		*object = nullptr;
		if( iid == __uuidof(IUnknown) || iid == __uuidof(IDWriteTextAnalysisSink) ) {
			*object = static_cast<IDWriteTextAnalysisSink*>(this);
			AddRef();
			return S_OK;
		}
		return E_NOINTERFACE;
	}
	ULONG STDMETHODCALLTYPE AddRef() noexcept override
	{
		return static_cast<ULONG>(::InterlockedIncrement(&m_references));
	}
	ULONG STDMETHODCALLTYPE Release() noexcept override
	{
		const auto references = static_cast<ULONG>(::InterlockedDecrement(&m_references));
		if( references == 0 ) delete this;
		return references;
	}
	HRESULT STDMETHODCALLTYPE SetScriptAnalysis(UINT32, UINT32,
		DWRITE_SCRIPT_ANALYSIS const* scriptAnalysis) noexcept override
	{
		if( scriptAnalysis == nullptr ) return E_POINTER;
		m_scriptAnalysis = *scriptAnalysis;
		m_hasScriptAnalysis = true;
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE SetLineBreakpoints(UINT32, UINT32,
		DWRITE_LINE_BREAKPOINT const*) noexcept override
	{
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE SetBidiLevel(UINT32, UINT32, UINT8,
		UINT8 resolvedLevel) noexcept override
	{
		m_resolvedBidiLevel = resolvedLevel;
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE SetNumberSubstitution(UINT32, UINT32,
		IDWriteNumberSubstitution*) noexcept override
	{
		return S_OK;
	}

	[[nodiscard]] bool HasScriptAnalysis() const noexcept { return m_hasScriptAnalysis; }
	[[nodiscard]] const DWRITE_SCRIPT_ANALYSIS& ScriptAnalysis() const noexcept { return m_scriptAnalysis; }
	[[nodiscard]] UINT8 ResolvedBidiLevel() const noexcept { return m_resolvedBidiLevel; }

private:
	volatile LONG m_references{ 1 };
	DWRITE_SCRIPT_ANALYSIS m_scriptAnalysis{};
	UINT8 m_resolvedBidiLevel{};
	bool m_hasScriptAnalysis{};
};

std::vector<DirectWriteRun> BuildDirectWriteRuns(const std::vector<PlacedCell>& placed)
{
	std::vector<DirectWriteRun> runs;
	runs.reserve(placed.size());
	for( const auto& cell : placed ) {
		const bool simpleCell = cell.occupiedColumns == 1 && cell.text.size() == 1;
		const bool canJoin = simpleCell && !runs.empty() &&
			runs.back().y == cell.y && runs.back().x + runs.back().width == cell.x;
		if( canJoin ) {
			runs.back().text.append(cell.text);
			runs.back().width += cell.width;
			continue;
		}
		runs.push_back({ std::wstring(cell.text), cell.x, cell.y, cell.width, cell.height });
	}
	return runs;
}

class RenderBackend {
public:
	virtual ~RenderBackend() = default;
	virtual bool RenderFrame(OffscreenDib& surface, const std::vector<PlacedCell>& placed,
		const TerminalLegacyRendererConditions& conditions, const Geometry& geometry,
		TerminalLegacyRendererCounters& counters, std::string& error) = 0;
};

class LegacyGdiBackend final : public RenderBackend {
public:
	LegacyGdiBackend(HDC dc, std::wstring family, const Geometry& geometry) noexcept
		: m_dc(dc)
	{
		m_font = ::CreateFontW(-static_cast<int>(geometry.fontPixelHeight), 0, 0, 0,
			FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
			CLIP_DEFAULT_PRECIS, CLEARTYPE_NATURAL_QUALITY, FIXED_PITCH | FF_MODERN,
			family.c_str());
		if( m_font != nullptr ) m_oldFont = static_cast<HFONT>(::SelectObject(m_dc, m_font));
		::SetBkMode(m_dc, TRANSPARENT);
	}
	~LegacyGdiBackend() noexcept override
	{
		if( m_dc != nullptr && m_oldFont != nullptr && m_oldFont != HGDI_ERROR ) {
			::SelectObject(m_dc, m_oldFont);
		}
		if( m_font != nullptr ) {
			::DeleteObject(m_font);
		}
	}
	[[nodiscard]] bool IsReady() const noexcept { return m_font != nullptr && m_oldFont != nullptr && m_oldFont != HGDI_ERROR; }
	bool RenderFrame(OffscreenDib& surface, const std::vector<PlacedCell>& placed,
		const TerminalLegacyRendererConditions& conditions, const Geometry&,
		TerminalLegacyRendererCounters& counters, std::string& error) override
	{
		if( !IsReady() ) {
			error = "CreateFontW/SelectObject failed";
			return false;
		}
		surface.Fill(conditions.background);
		::SetTextColor(m_dc, conditions.foreground);
		// Reuse one pair of contiguous scratch buffers for all simple cells.  The
		// grid supplies an explicit advance for each one-UTF-16-unit cell, while
		// complex clusters use one natural-advance call of their own.
		std::wstring batchText;
		std::vector<int> batchAdvances;
		batchText.reserve(placed.size());
		batchAdvances.reserve(placed.size());
		LONG batchX = 0;
		LONG batchY = 0;
		LONG batchRight = 0;
		LONG batchBottom = 0;
		bool hasBatch = false;
		bool batchAllowsJoin = false;
		const auto flushBatch = [&]() {
			if( !hasBatch || batchText.empty() ) {
				batchText.clear();
				batchAdvances.clear();
				hasBatch = false;
				batchAllowsJoin = false;
				return;
			}
			const RECT rect{ batchX, batchY, batchRight, batchBottom };
			if( ::ExtTextOutW(m_dc, batchX, batchY, ETO_CLIPPED, &rect, batchText.data(),
				static_cast<UINT>(batchText.size()), batchAdvances.data()) == FALSE ) {
				error = "ExtTextOutW failed";
			}
			++counters.extTextOutCalls;
			++counters.gdiBatchCount;
			batchText.clear();
			batchAdvances.clear();
			hasBatch = false;
			batchAllowsJoin = false;
		};
		const auto drawNaturalCluster = [&](const PlacedCell& cell) {
			if( cell.text.empty() ) return;
			const RECT rect{ cell.x, cell.y, cell.x + cell.width, cell.y + cell.height };
			if( ::ExtTextOutW(m_dc, cell.x, cell.y, ETO_CLIPPED, &rect, cell.text.data(),
				static_cast<UINT>(cell.text.size()), nullptr) == FALSE ) {
				error = "ExtTextOutW failed";
			}
			++counters.extTextOutCalls;
			++counters.gdiBatchCount;
		};
		for( const auto& cell : placed ) {
			const bool simpleCell = !cell.text.empty() && cell.text.size() == 1;
			if( !simpleCell ) {
				flushBatch();
				drawNaturalCluster(cell);
				continue;
			}
			const bool canJoin = hasBatch && batchAllowsJoin && cell.occupiedColumns == 1 &&
				cell.y == batchY && cell.x == batchRight;
			if( !canJoin ) {
				flushBatch();
				batchX = cell.x;
				batchY = cell.y;
				batchRight = cell.x;
				batchBottom = cell.y + cell.height;
				hasBatch = true;
				batchAllowsJoin = cell.occupiedColumns == 1;
			}
			batchText.append(cell.text);
			batchAdvances.push_back(cell.width);
			batchRight = cell.x + cell.width;
			batchBottom = cell.y + cell.height;
			// A wide one-code-unit cell has an explicit grid advance but cannot
			// share a run with the neighboring one-cell fast path.
			if( cell.occupiedColumns != 1 ) flushBatch();
		}
		flushBatch();
		return error.empty();
	}

private:
	HDC m_dc{};
	HFONT m_font{};
	HFONT m_oldFont{};
};

class GdiPlusRuntime final {
public:
	GdiPlusRuntime() = default;
	~GdiPlusRuntime() noexcept
	{
		if( m_token != 0 ) Gdiplus::GdiplusShutdown(m_token);
	}
	GdiPlusRuntime(const GdiPlusRuntime&) = delete;
	GdiPlusRuntime& operator=(const GdiPlusRuntime&) = delete;

	[[nodiscard]] bool EnsureStarted() noexcept
	{
		if( m_token != 0 ) return true;
		Gdiplus::GdiplusStartupInput input;
		input.GdiplusVersion = 1;
		return Gdiplus::GdiplusStartup(&m_token, &input, nullptr) == Gdiplus::Ok;
	}

private:
	ULONG_PTR m_token{};
};

GdiPlusRuntime& GetGdiPlusRuntime() noexcept
{
	static GdiPlusRuntime runtime;
	return runtime;
}

class GdiPlusBackend final : public RenderBackend {
public:
	GdiPlusBackend(HDC dc, std::wstring family, const Geometry& geometry) noexcept
		: m_dc(dc), m_family(std::move(family)), m_geometry(geometry)
	{
		if( !GetGdiPlusRuntime().EnsureStarted() ) return;
		m_gdiPlusReady = true;
		m_graphics = std::make_unique<Gdiplus::Graphics>(m_dc);
		if( !m_graphics ) return;
		m_graphics->SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
		m_graphics->SetSmoothingMode(Gdiplus::SmoothingModeNone);
		m_graphics->SetTextRenderingHint(Gdiplus::TextRenderingHintSingleBitPerPixelGridFit);
		m_familyObject = std::make_unique<Gdiplus::FontFamily>(m_family.c_str());
		m_font = std::make_unique<Gdiplus::Font>(m_familyObject.get(),
			static_cast<Gdiplus::REAL>(m_geometry.fontPixelHeight), Gdiplus::FontStyleRegular,
			Gdiplus::UnitPixel);
		m_brush = std::make_unique<Gdiplus::SolidBrush>(Gdiplus::Color(255, 0xDC, 0xDF, 0xE4));
		m_format = std::make_unique<Gdiplus::StringFormat>();
		if( m_format ) m_format->SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
	}
	~GdiPlusBackend() noexcept override
	{
		m_format.reset();
		m_brush.reset();
		m_font.reset();
		m_familyObject.reset();
		m_graphics.reset();
	}
	[[nodiscard]] bool IsReady() const noexcept
	{
		return m_gdiPlusReady && m_graphics != nullptr && m_familyObject != nullptr &&
			m_font != nullptr && m_brush != nullptr && m_format != nullptr && m_font->GetLastStatus() == Gdiplus::Ok;
	}

	bool RenderFrame(OffscreenDib& surface, const std::vector<PlacedCell>& placed,
		const TerminalLegacyRendererConditions& conditions, const Geometry&,
		TerminalLegacyRendererCounters& counters, std::string& error) override
	{
		if( !IsReady() ) {
			error = "GDI+ initialization failed";
			return false;
		}
		surface.Fill(conditions.background);
		m_brush->SetColor(Gdiplus::Color(255, GetRValue(conditions.foreground),
			GetGValue(conditions.foreground), GetBValue(conditions.foreground)));
		for( const auto& cell : placed ) {
			const Gdiplus::RectF rect(static_cast<Gdiplus::REAL>(cell.x), static_cast<Gdiplus::REAL>(cell.y),
				static_cast<Gdiplus::REAL>(cell.width), static_cast<Gdiplus::REAL>(cell.height));
			const auto status = m_graphics->DrawString(cell.text.data(), static_cast<INT>(cell.text.size()),
				m_font.get(), rect, m_format.get(), m_brush.get());
			++counters.drawStringCalls;
			if( status != Gdiplus::Ok ) {
				error = "GDI+ DrawString failed";
				return false;
			}
		}
		return true;
	}

private:
	HDC m_dc{};
	std::wstring m_family;
	Geometry m_geometry;
	bool m_gdiPlusReady{};
	std::unique_ptr<Gdiplus::Graphics> m_graphics;
	std::unique_ptr<Gdiplus::FontFamily> m_familyObject;
	std::unique_ptr<Gdiplus::Font> m_font;
	std::unique_ptr<Gdiplus::SolidBrush> m_brush;
	std::unique_ptr<Gdiplus::StringFormat> m_format;
};

class DirectWriteBackend final : public RenderBackend {
public:
	DirectWriteBackend(std::wstring family, const Geometry& geometry,
		TerminalLegacyRendererCounters& counters, bool explicitShapingStages) noexcept
		: m_family(std::move(family)), m_geometry(geometry), m_counters(counters),
		m_explicitShapingStages(explicitShapingStages)
	{
		if( FAILED(::D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, m_d2dFactory.GetAddressOf())) ) return;
		if( FAILED(::DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
			reinterpret_cast<IUnknown**>(m_dwriteFactory.GetAddressOf()))) ) return;
		ComPtr<IDWriteFactory2> factory2;
		if( SUCCEEDED(m_dwriteFactory.As(&factory2)) ) {
			static_cast<void>(factory2->GetSystemFontFallback(m_fontFallback.GetAddressOf()));
		}
		if( m_explicitShapingStages ) {
			if( FAILED(m_dwriteFactory->CreateTextAnalyzer(m_textAnalyzer.GetAddressOf())) ) return;
			if( FAILED(m_dwriteFactory->GetSystemFontCollection(m_fontCollection.GetAddressOf(), FALSE)) ) return;
			UINT32 familyIndex = UINT_MAX;
			BOOL familyExists = FALSE;
			if( FAILED(m_fontCollection->FindFamilyName(m_family.c_str(), &familyIndex, &familyExists)) ||
				!familyExists || familyIndex == UINT_MAX ) return;
			ComPtr<IDWriteFontFamily> fontFamily;
			if( FAILED(m_fontCollection->GetFontFamily(familyIndex, fontFamily.GetAddressOf())) ) return;
			ComPtr<IDWriteFont> font;
			if( FAILED(fontFamily->GetFirstMatchingFont(DWRITE_FONT_WEIGHT_NORMAL,
				DWRITE_FONT_STRETCH_NORMAL, DWRITE_FONT_STYLE_NORMAL, font.GetAddressOf())) ) return;
			if( FAILED(font->CreateFontFace(m_fontFace.GetAddressOf())) ) return;
		}
		if( FAILED(m_dwriteFactory->CreateTextFormat(m_family.c_str(), nullptr, DWRITE_FONT_WEIGHT_NORMAL,
			DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
			static_cast<FLOAT>(m_geometry.fontPixelHeight), L"", m_textFormat.GetAddressOf())) ) return;
		m_textFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
		m_textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
		m_textFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
		const auto properties = D2D1::RenderTargetProperties(
			D2D1_RENDER_TARGET_TYPE_DEFAULT,
			D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
			96.0f, 96.0f, D2D1_RENDER_TARGET_USAGE_NONE, D2D1_FEATURE_LEVEL_DEFAULT);
		if( FAILED(m_d2dFactory->CreateDCRenderTarget(&properties, m_target.GetAddressOf())) ) return;
		++m_counters.d2dTargetCreates;
		if( FAILED(m_target->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), m_brush.GetAddressOf())) ) return;
		m_ready = true;
	}
	~DirectWriteBackend() override = default;
	[[nodiscard]] bool IsReady() const noexcept
	{
		return m_ready && m_target != nullptr && m_brush != nullptr && m_textFormat != nullptr;
	}

	bool RenderFrame(OffscreenDib& surface, const std::vector<PlacedCell>& placed,
		const TerminalLegacyRendererConditions& conditions, const Geometry& geometry,
		TerminalLegacyRendererCounters& counters, std::string& error) override
	{
		if( !IsReady() ) {
			error = "DirectWrite/Direct2D initialization failed";
			return false;
		}
		surface.Fill(conditions.background);
		RECT clip{ 0, 0, static_cast<LONG>(surface.Width()), static_cast<LONG>(surface.Height()) };
		if( FAILED(m_target->BindDC(surface.Dc(), &clip)) ) {
			error = "ID2D1DCRenderTarget::BindDC failed";
			return false;
		}
		++counters.d2dTargetBinds;
		m_target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
		m_target->BeginDraw();
		bool ok = true;
		const auto runs = BuildDirectWriteRuns(placed);
		for( const auto& run : runs ) {
			ComPtr<IDWriteTextLayout> layout;
			const auto hr = m_dwriteFactory->CreateTextLayout(run.text.data(),
				static_cast<UINT32>(run.text.size()), m_textFormat.Get(),
				static_cast<FLOAT>(run.width), static_cast<FLOAT>(geometry.cellHeight), layout.GetAddressOf());
			++counters.textLayoutCreates;
			if( FAILED(hr) || layout == nullptr ) {
				error = "IDWriteFactory::CreateTextLayout failed";
				ok = false;
				break;
			}
			if( m_explicitShapingStages ) {
				ComPtr<DirectWriteTextAnalysisSource> source;
				source.Attach(new DirectWriteTextAnalysisSource(run.text));
				ComPtr<IDWriteFont> mappedFont;
				UINT32 mappedLength{};
				FLOAT scaleFactor = 1.0f;
				HRESULT mapResult = E_NOINTERFACE;
				if( m_fontFallback != nullptr ) {
					mapResult = m_fontFallback->MapCharacters(source.Get(), 0,
						static_cast<UINT32>(run.text.size()), m_fontCollection.Get(), m_family.c_str(),
						DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
						DWRITE_FONT_STRETCH_NORMAL, &mappedLength, mappedFont.GetAddressOf(), &scaleFactor);
				}
				if( SUCCEEDED(mapResult) ) ++counters.mapCharactersCalls;

				ComPtr<IDWriteFontFace> mappedFontFace;
				if( mappedFont != nullptr ) {
					static_cast<void>(mappedFont->CreateFontFace(mappedFontFace.GetAddressOf()));
				}
				IDWriteFontFace* shapingFontFace = mappedFontFace != nullptr ? mappedFontFace.Get() : m_fontFace.Get();
				ComPtr<DirectWriteTextAnalysisSink> sink;
				sink.Attach(new DirectWriteTextAnalysisSink());
				const auto analysisResult = m_textAnalyzer->AnalyzeScript(source.Get(), 0,
					static_cast<UINT32>(run.text.size()), sink.Get());
				if( SUCCEEDED(analysisResult) ) ++counters.analysisCalls;
				if( SUCCEEDED(analysisResult) && sink->HasScriptAnalysis() && shapingFontFace != nullptr ) {
					const auto textLength = static_cast<UINT32>(run.text.size());
					const auto maxGlyphCount = std::max<UINT32>(16u, textLength + (textLength / 2u) + 16u);
					std::vector<UINT16> clusterMap(textLength);
					std::vector<DWRITE_SHAPING_TEXT_PROPERTIES> textProperties(textLength);
					std::vector<UINT16> glyphIndices(maxGlyphCount);
					std::vector<DWRITE_SHAPING_GLYPH_PROPERTIES> glyphProperties(maxGlyphCount);
					UINT32 glyphCount{};
					const auto glyphResult = m_textAnalyzer->GetGlyphs(run.text.data(), textLength,
						shapingFontFace, FALSE, sink->ResolvedBidiLevel() % 2 != 0,
						&sink->ScriptAnalysis(), L"en-us", nullptr, nullptr, nullptr, 0,
						maxGlyphCount, clusterMap.data(), textProperties.data(), glyphIndices.data(),
						glyphProperties.data(), &glyphCount);
					if( SUCCEEDED(glyphResult) ) {
						++counters.glyphsCalls;
						std::vector<FLOAT> glyphAdvances(glyphCount);
						std::vector<DWRITE_GLYPH_OFFSET> glyphOffsets(glyphCount);
						const auto placementResult = m_textAnalyzer->GetGlyphPlacements(run.text.data(),
							clusterMap.data(), textProperties.data(), textLength, glyphIndices.data(),
							glyphProperties.data(), glyphCount, shapingFontFace,
							static_cast<FLOAT>(m_geometry.fontPixelHeight) * scaleFactor, FALSE,
							sink->ResolvedBidiLevel() % 2 != 0, &sink->ScriptAnalysis(), L"en-us",
							nullptr, nullptr, 0, glyphAdvances.data(), glyphOffsets.data());
						if( SUCCEEDED(placementResult) ) ++counters.placementCalls;
					}
				}
			}
			m_brush->SetColor(D2D1::ColorF(
				static_cast<FLOAT>(GetRValue(conditions.foreground)) / 255.0f,
				static_cast<FLOAT>(GetGValue(conditions.foreground)) / 255.0f,
				static_cast<FLOAT>(GetBValue(conditions.foreground)) / 255.0f));
			m_target->DrawTextLayout(D2D1::Point2F(static_cast<FLOAT>(run.x), static_cast<FLOAT>(run.y)),
				layout.Get(), m_brush.Get(), D2D1_DRAW_TEXT_OPTIONS_NO_SNAP | D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
			++counters.d2dDrawCalls;
		}
		const auto endResult = m_target->EndDraw();
		++counters.d2dEndDrawCalls;
		if( endResult == D2DERR_RECREATE_TARGET ) {
			++counters.d2dTargetLosses;
			error = "D2DERR_RECREATE_TARGET";
			ok = false;
		} else if( FAILED(endResult) ) {
			error = "ID2D1RenderTarget::EndDraw failed";
			ok = false;
		}
		return ok;
	}

private:
	std::wstring m_family;
	Geometry m_geometry;
	TerminalLegacyRendererCounters& m_counters;
	const bool m_explicitShapingStages{};
	ComPtr<ID2D1Factory> m_d2dFactory;
	ComPtr<IDWriteFactory> m_dwriteFactory;
	ComPtr<IDWriteFontFallback> m_fontFallback;
	ComPtr<IDWriteFontCollection> m_fontCollection;
	ComPtr<IDWriteFontFace> m_fontFace;
	ComPtr<IDWriteTextAnalyzer> m_textAnalyzer;
	ComPtr<ID2D1DCRenderTarget> m_target;
	ComPtr<ID2D1SolidColorBrush> m_brush;
	ComPtr<IDWriteTextFormat> m_textFormat;
	bool m_ready{};
};

class CandidateClassifier final : public ITerminalRenderClassifier {
public:
	TerminalRenderClassification Classify(std::wstring_view text, bool) noexcept override
	{
		// The production classifier binds primary-font coverage to a window DC.
		// The candidate harness intentionally uses a deterministic equivalent:
		// ordinary ASCII remains the unconditional GDI fast path and every other
		// scalar is sent through the production DWrite fallback.  Built-in
		// U+2500-U+259F glyphs are intercepted by TerminalRenderPlan before this
		// seam is consulted.
		return text.size() == 1 && text.front() < 0x80
			? TerminalRenderClassification::GdiSimple
			: TerminalRenderClassification::ShapedFallback;
	}
	std::uint64_t Generation() const noexcept override { return 1; }
};

struct CandidateStyleContext final {
	COLORREF foreground{};
	COLORREF background{};
};

TerminalRenderStyle ResolveCandidateStyle(void* context, const TerminalAttributes& attributes,
	bool selected) noexcept
{
	const auto& colors = *static_cast<const CandidateStyleContext*>(context);
	const bool usesSurfaceDefaultBackground = !selected && !attributes.inverse &&
		attributes.background.kind == TerminalColorKind::Default;
	return { colors.foreground, selected ? RGB(0x40, 0x50, 0x70) : colors.background,
		attributes.bold, attributes.underline, attributes.inverse, selected, usesSurfaceDefaultBackground };
}

void AppendCandidateCodepoints(TerminalModel& model, std::wstring_view text)
{
	for( std::size_t index = 0; index < text.size(); ) {
		const auto first = static_cast<std::uint32_t>(text[index++]);
		if( first >= 0xD800 && first <= 0xDBFF && index < text.size() ) {
			const auto second = static_cast<std::uint32_t>(text[index]);
			if( second >= 0xDC00 && second <= 0xDFFF ) {
				++index;
				model.Print(static_cast<char32_t>(0x10000 + ((first - 0xD800) << 10) + second - 0xDC00));
				continue;
			}
		}
		model.Print(static_cast<char32_t>(first));
	}
}

std::unique_ptr<TerminalModel> BuildCandidateModel(const TerminalLegacyRendererConditions& conditions,
	const std::vector<TerminalLegacyRendererCell>& cells)
{
	auto model = std::make_unique<TerminalModel>(conditions.columns, conditions.rows, 0);
	const auto targetColumns = static_cast<std::uint64_t>(conditions.columns) * conditions.rows;
	std::uint64_t occupiedColumns{};
	for( const auto& cell : cells ) {
		if( occupiedColumns >= targetColumns ) break;
		AppendCandidateCodepoints(*model, cell.text);
		occupiedColumns += std::max<std::uint8_t>(1, cell.occupiedColumns);
	}
	return model;
}

class CandidateNativeBackend final : public RenderBackend {
public:
	CandidateNativeBackend(HDC dc, std::wstring family, const Geometry& geometry,
		const TerminalLegacyRendererConditions& conditions,
		const std::vector<TerminalLegacyRendererCell>& cells)
		: m_dc(dc)
		, m_family(std::move(family))
		, m_geometry(geometry)
		, m_model(BuildCandidateModel(conditions, cells))
	{
		m_font = ::CreateFontW(-static_cast<int>(geometry.fontPixelHeight), 0, 0, 0,
			FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
			CLIP_DEFAULT_PRECIS, CLEARTYPE_NATURAL_QUALITY, FIXED_PITCH | FF_MODERN,
			m_family.c_str());
		if( m_font != nullptr ) m_oldFont = static_cast<HFONT>(::SelectObject(m_dc, m_font));
		::SetBkMode(m_dc, TRANSPARENT);
	}

	~CandidateNativeBackend() noexcept override
	{
		m_dwrite.Close();
		if( m_dc != nullptr && m_oldFont != nullptr && m_oldFont != HGDI_ERROR ) ::SelectObject(m_dc, m_oldFont);
		if( m_font != nullptr ) ::DeleteObject(m_font);
	}

	[[nodiscard]] bool IsReady() const noexcept
	{
		return m_font != nullptr && m_oldFont != nullptr && m_oldFont != HGDI_ERROR && m_model != nullptr;
	}

	bool RenderFrame(OffscreenDib& surface, const std::vector<PlacedCell>&,
		const TerminalLegacyRendererConditions& conditions, const Geometry&,
		TerminalLegacyRendererCounters& counters, std::string& error) override
	{
		if( !IsReady() ) {
			error = "Candidate native initialization failed";
			return false;
		}
		surface.Fill(conditions.background);
		m_style.foreground = conditions.foreground;
		m_style.background = conditions.background;
		const RECT paintRect{ 0, 0, static_cast<LONG>(surface.Width()), static_cast<LONG>(surface.Height()) };
		const TerminalRenderPlanBuildInput input{
			m_model.get(),
			{ conditions.rows, conditions.rows, 0 },
			paintRect,
			static_cast<int>(m_geometry.cellWidth),
			static_cast<int>(m_geometry.cellHeight),
			false,
			{},
			{},
			&m_classifier,
			&ResolveCandidateStyle,
			&m_style,
			true,
		};
		if( !m_plan.Build(input) ) {
			error = "TerminalRenderPlan::Build rejected candidate viewport";
			return false;
		}
		const auto brush = static_cast<HBRUSH>(::GetStockObject(DC_BRUSH));
		for( const auto& span : m_plan.BackgroundSpans() ) {
			::SetDCBrushColor(m_dc, span.style.background);
			::FillRect(m_dc, &span.rect, brush);
		}
		::GdiFlush();
		const TerminalBuiltinGlyphPixelSurface pixelSurface{
			surface.Bits(),
			static_cast<std::ptrdiff_t>(surface.Width()),
			static_cast<LONG>(surface.Width()),
			static_cast<LONG>(surface.Height()),
			paintRect,
		};
		if( !TerminalBuiltinGlyphRenderer::DrawBatch(pixelSurface, m_plan.BuiltinGlyphs()) ) {
			error = "TerminalBuiltinGlyphRenderer::DrawBatch rejected candidate surface";
			return false;
		}
		::SetBkMode(m_dc, TRANSPARENT);
		::SetTextColor(m_dc, conditions.foreground);
		for( const auto& run : m_plan.GdiRuns() ) {
			const auto text = m_plan.Text(run.textOffset, run.textLength);
			const auto advances = m_plan.Advances(run.advanceOffset, run.advanceCount);
			if( text.empty() || advances.size() != text.size() ) continue;
			if( ::ExtTextOutW(m_dc, run.rect.left, run.rect.top, ETO_CLIPPED, &run.rect,
				text.data(), static_cast<UINT>(text.size()), advances.data()) == FALSE ) {
				error = "Candidate ExtTextOutW failed";
				return false;
			}
			++counters.extTextOutCalls;
			++counters.gdiBatchCount;
		}
		if( !m_plan.ShapedClusters().empty() ) {
			if( !m_dwriteConfigured ) {
				m_dwrite.Configure({ m_family, L"en-us", static_cast<int>(m_geometry.fontPixelHeight),
					FW_NORMAL, conditions.dpi, 1 });
				m_dwriteConfigured = true;
			}
			TerminalDWriteFrame frame;
			if( m_dwrite.BeginFrame(m_dc, paintRect, frame) != TerminalDWriteFrameOutcome::Rendered ) {
				error = "Candidate DirectWrite BeginFrame unavailable";
				return false;
			}
			for( const auto& cluster : m_plan.ShapedClusters() ) {
				const auto text = m_plan.Text(cluster.textOffset, cluster.textLength);
				if( text.empty() || !m_dwrite.DrawCluster(cluster, text) ) {
					m_dwrite.AbortFrame(frame);
					error = "Candidate DirectWrite DrawCluster failed";
					MapDWriteCounters(counters);
					return false;
				}
				++counters.fallbackRuns;
			}
			if( m_dwrite.FinalizeFrame(frame) != TerminalDWriteFrameOutcome::Rendered ) {
				error = "Candidate DirectWrite FinalizeFrame unavailable";
				MapDWriteCounters(counters);
				return false;
			}
		}
		MapDWriteCounters(counters);
		return true;
	}

private:
	void MapDWriteCounters(TerminalLegacyRendererCounters& counters) const noexcept
	{
		const auto& source = m_dwrite.Counters();
		counters.fallbackCacheHits = source.cache.hits;
		counters.fallbackCacheMisses = source.cache.misses;
		counters.mapCharactersCalls = source.mapCharactersCalls;
		counters.analysisCalls = source.scriptAnalysisCalls + source.bidiAnalysisCalls;
		counters.glyphsCalls = source.glyphCalls;
		counters.placementCalls = source.placementCalls;
		counters.d2dTargetCreates = source.targetCreations;
		counters.d2dTargetBinds = source.targetBinds;
		counters.d2dDrawCalls = source.glyphRunDraws;
		counters.d2dEndDrawCalls = source.endDrawCalls;
		counters.d2dTargetLosses = source.targetLosses;
	}

	HDC m_dc{};
	std::wstring m_family;
	Geometry m_geometry;
	std::unique_ptr<TerminalModel> m_model;
	TerminalRenderPlan m_plan;
	CandidateClassifier m_classifier;
	CandidateStyleContext m_style;
	TerminalDWriteRenderer m_dwrite;
	HFONT m_font{};
	HFONT m_oldFont{};
	bool m_dwriteConfigured{};
};

std::unique_ptr<RenderBackend> CreateBackend(const TerminalLegacyRendererConditions& conditions,
	const std::wstring& family, const Geometry& geometry, HDC dc,
	TerminalLegacyRendererCounters& counters, std::string& error)
{
	switch( conditions.backend ) {
	case TerminalLegacyRendererBackend::LegacyGdi: {
		auto backend = std::make_unique<LegacyGdiBackend>(dc, family, geometry);
		if( !backend->IsReady() ) error = "GDI font initialization failed";
		return backend;
	}
	case TerminalLegacyRendererBackend::GdiPlus: {
		auto backend = std::make_unique<GdiPlusBackend>(dc, family, geometry);
		if( !backend->IsReady() ) error = "GDI+ initialization failed";
		return backend;
	}
	case TerminalLegacyRendererBackend::DirectWrite:
	case TerminalLegacyRendererBackend::DirectWriteDirect2D: {
		auto backend = std::make_unique<DirectWriteBackend>(family, geometry, counters,
			conditions.backend == TerminalLegacyRendererBackend::DirectWrite);
		if( !backend->IsReady() ) error = "DirectWrite/Direct2D initialization failed";
		return backend;
	}
	case TerminalLegacyRendererBackend::CandidateNative: {
		const auto cells = BuildTerminalLegacyRendererCorpus(conditions.corpus);
		auto backend = std::make_unique<CandidateNativeBackend>(dc, family, geometry, conditions, cells);
		if( !backend->IsReady() ) error = "Candidate native initialization failed";
		return backend;
	}
	}
	error = "Unknown backend";
	return {};
}

std::string NarrowAscii(std::wstring_view value)
{
	std::string result;
	result.reserve(value.size());
	for( const auto character : value ) result.push_back(character <= 0x7F ? static_cast<char>(character) : '?');
	return result;
}

std::string EscapeJson(std::string_view value)
{
	std::string result;
	result.reserve(value.size() + 8);
	for( const auto character : value ) {
		switch( character ) {
		case '"': result += "\\\""; break;
		case '\\': result += "\\\\"; break;
		case '\n': result += "\\n"; break;
		case '\r': result += "\\r"; break;
		case '\t': result += "\\t"; break;
		default:
			if( static_cast<unsigned char>(character) < 0x20 ) result += '?';
			else result.push_back(character);
			break;
		}
	}
	return result;
}

void AppendJsonNumber(std::ostringstream& output, double value)
{
	if( !std::isfinite(value) ) {
		output << "null";
		return;
	}
	output << std::fixed << std::setprecision(6) << value;
}

} // namespace

const char* TerminalLegacyRendererBackendName(TerminalLegacyRendererBackend backend) noexcept
{
	switch( backend ) {
	case TerminalLegacyRendererBackend::LegacyGdi: return "legacy-gdi";
	case TerminalLegacyRendererBackend::GdiPlus: return "gdi-plus";
	case TerminalLegacyRendererBackend::DirectWrite: return "directwrite";
	case TerminalLegacyRendererBackend::DirectWriteDirect2D: return "directwrite-d2d";
	case TerminalLegacyRendererBackend::CandidateNative: return "candidate-native";
	}
	return "unknown";
}

const char* TerminalLegacyRendererCorpusName(TerminalLegacyRendererCorpus corpus) noexcept
{
	switch( corpus ) {
	case TerminalLegacyRendererCorpus::AsciiShell: return "ascii-shell";
	case TerminalLegacyRendererCorpus::TuiBoxBlockShade: return "tui-box-block-shade";
	case TerminalLegacyRendererCorpus::MixedUnicode: return "mixed-unicode";
	}
	return "unknown";
}

TerminalLegacyRendererConditions DefaultTerminalLegacyRendererConditions() noexcept
{
	return {};
}

std::vector<TerminalLegacyRendererCell> BuildTerminalLegacyRendererCorpus(TerminalLegacyRendererCorpus corpus)
{
	std::vector<TerminalLegacyRendererCell> result;
	result.reserve(static_cast<std::size_t>(kDefaultVisibleColumns) * kDefaultVisibleRows * 2);
	constexpr std::uint64_t targetColumns = static_cast<std::uint64_t>(kDefaultVisibleColumns) * kDefaultVisibleRows;
	std::uint64_t occupiedColumns = 0;
	switch( corpus ) {
	case TerminalLegacyRendererCorpus::AsciiShell: {
		constexpr std::wstring_view line = L"$ echo \"sakura\" && pwd | sed -n '1,3p' ; printf ready";
		while( occupiedColumns < targetColumns ) {
			AppendAscii(result, line);
			occupiedColumns += line.size();
		}
		break;
	}
	case TerminalLegacyRendererCorpus::TuiBoxBlockShade:
		while( occupiedColumns < targetColumns ) {
			AppendTui(result);
			occupiedColumns += 0xA0;
		}
		break;
	case TerminalLegacyRendererCorpus::MixedUnicode:
		while( occupiedColumns < targetColumns ) {
			const auto before = result.size();
			AppendMixedPattern(result);
			for( std::size_t i = before; i < result.size(); ++i ) occupiedColumns += result[i].occupiedColumns;
		}
		break;
	}
	return result;
}

TerminalLegacyRendererResult RunTerminalLegacyRenderer(const TerminalLegacyRendererConditions& input)
{
	TerminalLegacyRendererConditions conditions = input;
	if( conditions.dpi == 0 ) conditions.dpi = 96;
	conditions.columns = std::max(kMinimumDimension, conditions.columns);
	conditions.rows = std::max(kMinimumDimension, conditions.rows);
	conditions.frameCount = std::max(kMinimumDimension, conditions.frameCount);
	const auto geometry = GeometryForDpi(conditions.dpi);
	TerminalLegacyRendererResult result;
	result.backend = conditions.backend;
	result.corpus = conditions.corpus;
	result.dpi = conditions.dpi;
	result.columns = conditions.columns;
	result.rows = conditions.rows;
	result.cellWidth = geometry.cellWidth;
	result.cellHeight = geometry.cellHeight;
	result.fontPixelHeight = geometry.fontPixelHeight;
	result.warmupFrames = conditions.warmupFrames;
	result.frameCount = conditions.frameCount;
	const auto cells = BuildTerminalLegacyRendererCorpus(conditions.corpus);
	for( const auto& cell : cells ) {
		++result.corpusCellCount;
		result.corpusUtf16CodeUnits += cell.text.size();
		result.corpusOccupiedColumns += std::max<std::uint8_t>(1, cell.occupiedColumns);
	}
	WarmGdiAccounting();
	const auto placed = PlaceCells(cells, conditions, geometry);
	ScopedCom com;
	if( FAILED(com.Result()) && com.Result() != RPC_E_CHANGED_MODE ) {
		result.unavailableReason = "CoInitializeEx failed";
		return result;
	}

	// The first instance measures genuine process-cold initialization.  Its
	// teardown is deliberately excluded from renderer-owned resource deltas:
	// GDI+, DirectWrite, and font services may retain process-global caches.
	const auto processGlobalBefore = ReadProcessStats();
	result.processGlobalPrivateBytesBefore = processGlobalBefore.privateBytes;
	result.processGlobalGdiObjectsBefore = processGlobalBefore.gdiObjects;
	std::wstring family;
	std::string backendError;
	{
		OffscreenDib coldSurface;
		const auto width = conditions.columns * geometry.cellWidth;
		const auto height = conditions.rows * geometry.cellHeight;
		if( !coldSurface.Create(width, height) ) {
			result.unavailableReason = "CreateDIBSection/CreateCompatibleDC failed";
		} else {
			family = SelectFontFamily(coldSurface.Dc(), conditions);
			if( family.empty() ) {
				result.unavailableReason = "Cascadia Mono and Consolas are unavailable";
			} else {
				result.fontFamily = NarrowAscii(family);
				TerminalLegacyRendererCounters coldCounters;
				const auto coldStart = std::chrono::steady_clock::now();
				auto coldBackend = CreateBackend(conditions, family, geometry, coldSurface.Dc(), coldCounters, backendError);
				const auto coldEnd = std::chrono::steady_clock::now();
				result.coldInitializationMs = std::chrono::duration<double, std::milli>(coldEnd - coldStart).count();
				if( coldBackend == nullptr || !backendError.empty() ) {
					result.unavailableReason = backendError.empty() ? "Backend initialization failed" : backendError;
				} else {
					std::string coldRenderError;
					if( !coldBackend->RenderFrame(coldSurface, placed, conditions, geometry,
						coldCounters, coldRenderError) ) {
						result.unavailableReason = coldRenderError.empty() ? "Cold setup frame failed" : coldRenderError;
					}
				}
				if( coldBackend != nullptr ) {
					coldBackend.reset();
				}
				coldSurface.Close();
			}
		}
	}
	// A second, untimed instance stabilizes backend-local lazy caches.  It is
	// fully destroyed before the external resource baseline is captured, so
	// the measured instance starts from a closed renderer with no live DIB/DC,
	// font, or Graphics objects.
	if( result.unavailableReason.empty() && !family.empty() ) {
		OffscreenDib stabilizationSurface;
		const auto width = conditions.columns * geometry.cellWidth;
		const auto height = conditions.rows * geometry.cellHeight;
		if( !stabilizationSurface.Create(width, height) ) {
			result.unavailableReason = "CreateDIBSection/CreateCompatibleDC failed for stabilization probe";
		} else {
			TerminalLegacyRendererCounters stabilizationCounters;
			backendError.clear();
			auto stabilizationBackend = CreateBackend(conditions, family, geometry,
				stabilizationSurface.Dc(), stabilizationCounters, backendError);
			if( stabilizationBackend == nullptr || !backendError.empty() ) {
				result.unavailableReason = backendError.empty() ?
					"Stabilization backend initialization failed" : backendError;
			} else {
				std::string stabilizationError;
				// GDI+ may defer one-time text state until its second frame. Two
				// setup frames cover that transition while keeping this probe bounded.
				for( std::uint32_t frame = 0; frame < 2 && result.unavailableReason.empty(); ++frame ) {
					if( !stabilizationBackend->RenderFrame(stabilizationSurface, placed, conditions,
						geometry, stabilizationCounters, stabilizationError) ) {
						result.unavailableReason = stabilizationError.empty() ?
							"Stabilization setup frame failed" : stabilizationError;
					}
				}
			}
			if( stabilizationBackend != nullptr ) stabilizationBackend.reset();
			stabilizationSurface.Close();
		}
	}
	::GdiFlush();
	::Sleep(10);
	const auto processGlobalAfter = ReadProcessStats();
	result.processGlobalPrivateBytesAfter = processGlobalAfter.privateBytes;
	result.processGlobalPrivateBytesDelta = processGlobalAfter.privateBytes - processGlobalBefore.privateBytes;
	result.processGlobalGdiObjectsAfter = processGlobalAfter.gdiObjects;
	result.processGlobalGdiObjectsDelta = processGlobalAfter.gdiObjects - processGlobalBefore.gdiObjects;
	if( !result.unavailableReason.empty() || family.empty() ) return result;

	// The measured-instance resource baseline is captured outside the live
	// instance, after the stabilization probe has been destroyed.
	const auto statsBefore = processGlobalAfter;
	{
		OffscreenDib surface;
		const auto width = conditions.columns * geometry.cellWidth;
		const auto height = conditions.rows * geometry.cellHeight;
		if( !surface.Create(width, height) ) {
			result.unavailableReason = "CreateDIBSection/CreateCompatibleDC failed";
		} else {
			backendError.clear();
			const auto measurementStart = std::chrono::steady_clock::now();
			auto backend = CreateBackend(conditions, family, geometry, surface.Dc(), result.counters, backendError);
			const auto measurementEnd = std::chrono::steady_clock::now();
			result.measurementInitializationMs = std::chrono::duration<double, std::milli>(measurementEnd - measurementStart).count();
			if( backend == nullptr || !backendError.empty() ) {
				result.unavailableReason = backendError.empty() ? "Backend initialization failed" : backendError;
			} else {
				bool ok = true;
				for( std::uint32_t frame = 0; frame < conditions.warmupFrames && ok; ++frame ) {
					ok = backend->RenderFrame(surface, placed, conditions, geometry, result.counters, backendError);
				}
				result.frameDurationMs.reserve(conditions.frameCount);
				for( std::uint32_t frame = 0; frame < conditions.frameCount && ok; ++frame ) {
					const auto start = std::chrono::steady_clock::now();
					ok = backend->RenderFrame(surface, placed, conditions, geometry, result.counters, backendError);
					const auto finish = std::chrono::steady_clock::now();
					result.frameDurationMs.push_back(std::chrono::duration<double, std::milli>(finish - start).count());
					if( ok ) ++result.counters.frames;
				}
				if( !ok ) result.unavailableReason = backendError.empty() ? "Frame rendering failed" : backendError;
				else result.available = true;
			}
			if( backend != nullptr ) backend.reset();
			surface.Close();
		}
	}
	::GdiFlush();
	::Sleep(10);
	const auto statsAfter = ReadProcessStats();
	result.privateBytesBefore = statsBefore.privateBytes;
	result.privateBytesAfter = statsAfter.privateBytes;
	result.privateBytesDelta = statsAfter.privateBytes - statsBefore.privateBytes;
	result.gdiObjectsBefore = statsBefore.gdiObjects;
	result.gdiObjectsAfter = statsAfter.gdiObjects;
	result.gdiObjectsDelta = statsAfter.gdiObjects - statsBefore.gdiObjects;
	return result;
}

std::string SerializeTerminalLegacyRendererResult(const TerminalLegacyRendererResult& result)
{
	std::ostringstream output;
	output.imbue(std::locale::classic());
	output << "{\"schemaVersion\":" << result.schemaVersion
		<< ",\"available\":" << (result.available ? "true" : "false")
		<< ",\"unavailableReason\":\"" << EscapeJson(result.unavailableReason) << "\""
		<< ",\"backend\":\"" << TerminalLegacyRendererBackendName(result.backend) << "\""
		<< ",\"corpus\":\"" << TerminalLegacyRendererCorpusName(result.corpus) << "\""
		<< ",\"conditions\":{";
	output << "\"dpi\":" << result.dpi << ",\"columns\":" << result.columns
		<< ",\"rows\":" << result.rows << ",\"cellWidth\":" << result.cellWidth
		<< ",\"cellHeight\":" << result.cellHeight << ",\"fontPixelHeight\":" << result.fontPixelHeight
		<< ",\"warmupFrames\":" << result.warmupFrames << ",\"frameCount\":" << result.frameCount
		<< ",\"fontFamily\":\"" << EscapeJson(result.fontFamily) << "\"}";
	output << ",\"corpusStats\":{";
	output << "\"cellCount\":" << result.corpusCellCount << ",\"utf16CodeUnits\":"
		<< result.corpusUtf16CodeUnits << ",\"occupiedColumns\":" << result.corpusOccupiedColumns << "}";
	output << ",\"timings\":{\"coldInitializationMs\":";
	AppendJsonNumber(output, result.coldInitializationMs);
	output << ",\"measurementInitializationMs\":";
	AppendJsonNumber(output, result.measurementInitializationMs);
	output << ",\"frameDurationMs\":[";
	for( std::size_t i = 0; i < result.frameDurationMs.size(); ++i ) {
		if( i != 0 ) output << ',';
		AppendJsonNumber(output, result.frameDurationMs[i]);
	}
	output << "]}";
	output << ",\"resources\":{\"processGlobalPrivateBytesBefore\":" << result.processGlobalPrivateBytesBefore
		<< ",\"processGlobalPrivateBytesAfter\":" << result.processGlobalPrivateBytesAfter
		<< ",\"processGlobalPrivateBytesDelta\":" << result.processGlobalPrivateBytesDelta
		<< ",\"processGlobalGdiObjectsBefore\":" << result.processGlobalGdiObjectsBefore
		<< ",\"processGlobalGdiObjectsAfter\":" << result.processGlobalGdiObjectsAfter
		<< ",\"processGlobalGdiObjectsDelta\":" << result.processGlobalGdiObjectsDelta
		<< ",\"privateBytesBefore\":" << result.privateBytesBefore
		<< ",\"privateBytesAfter\":" << result.privateBytesAfter << ",\"privateBytesDelta\":"
		<< result.privateBytesDelta << ",\"gdiObjectsBefore\":" << result.gdiObjectsBefore
		<< ",\"gdiObjectsAfter\":" << result.gdiObjectsAfter << ",\"gdiObjectsDelta\":"
		<< result.gdiObjectsDelta << "}";
	const auto& counters = result.counters;
	output << ",\"counters\":{\"frames\":" << counters.frames
		<< ",\"extTextOutCalls\":" << counters.extTextOutCalls
		<< ",\"gdiBatchCount\":" << counters.gdiBatchCount
		<< ",\"drawStringCalls\":" << counters.drawStringCalls
		<< ",\"textLayoutCreates\":" << counters.textLayoutCreates
		<< ",\"fallbackRuns\":" << counters.fallbackRuns
		<< ",\"fallbackCacheHits\":" << counters.fallbackCacheHits
		<< ",\"fallbackCacheMisses\":" << counters.fallbackCacheMisses
		<< ",\"mapCharactersCalls\":" << counters.mapCharactersCalls
		<< ",\"analysisCalls\":" << counters.analysisCalls
		<< ",\"glyphsCalls\":" << counters.glyphsCalls
		<< ",\"placementCalls\":" << counters.placementCalls
		<< ",\"d2dTargetCreates\":" << counters.d2dTargetCreates
		<< ",\"d2dTargetBinds\":" << counters.d2dTargetBinds
		<< ",\"d2dDrawCalls\":" << counters.d2dDrawCalls
		<< ",\"d2dEndDrawCalls\":" << counters.d2dEndDrawCalls
		<< ",\"d2dTargetLosses\":" << counters.d2dTargetLosses << "}";
	output << "}";
	return output.str();
}

} // namespace terminal

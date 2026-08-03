/*! @file */
#include "pch.h"

#include "terminal/window/TerminalDWriteRenderer.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>

namespace terminal {
namespace {

class ScopedCom final {
public:
	ScopedCom() noexcept
		: m_result(::CoInitializeEx(nullptr, COINIT_MULTITHREADED))
		, m_uninitialize(SUCCEEDED(m_result))
	{
	}
	~ScopedCom() noexcept
	{
		if( m_uninitialize ) ::CoUninitialize();
	}
	[[nodiscard]] HRESULT Result() const noexcept { return m_result; }

private:
	HRESULT m_result{};
	bool m_uninitialize{};
};

class TestDib final {
public:
	TestDib() noexcept = default;
	~TestDib() noexcept { Close(); }
	TestDib(const TestDib&) = delete;
	TestDib& operator=(const TestDib&) = delete;

	bool Create(int width, int height) noexcept
	{
		Close();
		m_dc = ::CreateCompatibleDC(nullptr);
		if( m_dc == nullptr ) return false;
		BITMAPINFO info{};
		info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
		info.bmiHeader.biWidth = width;
		info.bmiHeader.biHeight = -height;
		info.bmiHeader.biPlanes = 1;
		info.bmiHeader.biBitCount = 32;
		info.bmiHeader.biCompression = BI_RGB;
		m_bitmap = ::CreateDIBSection(m_dc, &info, DIB_RGB_COLORS, &m_bits, nullptr, 0);
		if( m_bitmap == nullptr || m_bits == nullptr ) {
			Close();
			return false;
		}
		m_oldBitmap = ::SelectObject(m_dc, m_bitmap);
		if( m_oldBitmap == nullptr || m_oldBitmap == HGDI_ERROR ) {
			Close();
			return false;
		}
		m_width = width;
		m_height = height;
		return true;
	}

	void Fill(COLORREF color) noexcept
	{
		if( m_dc == nullptr ) return;
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
	[[nodiscard]] int Width() const noexcept { return m_width; }
	[[nodiscard]] int Height() const noexcept { return m_height; }
	[[nodiscard]] HDC Dc() const noexcept { return m_dc; }

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
	void* m_bits{};
	int m_width{};
	int m_height{};
};

std::size_t CountNotColor(const TestDib& dib, COLORREF color, const RECT& rect)
{
	std::size_t count{};
	const int left = std::max(0L, rect.left);
	const int top = std::max(0L, rect.top);
	const int right = std::min(dib.Width(), static_cast<int>(rect.right));
	const int bottom = std::min(dib.Height(), static_cast<int>(rect.bottom));
	for( int y = top; y < bottom; ++y ) {
		for( int x = left; x < right; ++x ) {
			if( dib.Pixel(x, y) != color ) ++count;
		}
	}
	return count;
}

std::size_t CountNotColorOutside(const TestDib& dib, COLORREF color, const RECT& inside)
{
	std::size_t count{};
	for( int y = 0; y < dib.Height(); ++y ) {
		for( int x = 0; x < dib.Width(); ++x ) {
			if( x >= inside.left && x < inside.right && y >= inside.top && y < inside.bottom ) continue;
			if( dib.Pixel(x, y) != color ) ++count;
		}
	}
	return count;
}

TerminalRenderStyle Style() noexcept
{
	return { RGB(0xDC, 0xDF, 0xE4), RGB(0x28, 0x2C, 0x34), false, false, false, false };
}

TerminalDWriteConfiguration Configuration(std::uint64_t generation = 1)
{
	return { L"Segoe UI Symbol", L"en-us", 16, FW_NORMAL, 96, generation };
}

bool DrawCluster(TerminalDWriteRenderer& renderer, HDC dc, std::wstring_view text,
	std::uint64_t column = 0)
{
	TerminalDWriteFrame frame;
	const RECT clip{ 0, 0, 512, 64 };
	if( renderer.BeginFrame(dc, clip, frame) != TerminalDWriteFrameOutcome::Rendered ) return false;
	const RECT rect{ static_cast<LONG>(column * 16), 0, static_cast<LONG>(column * 16 + 16), 32 };
	const TerminalShapedClusterCommand command{ rect, Style(), 0, text.size() };
	if( !renderer.DrawCluster(command, text) ) {
		renderer.AbortFrame(frame);
		return false;
	}
	return renderer.FinalizeFrame(frame) == TerminalDWriteFrameOutcome::Rendered;
}

TEST(TerminalDWriteRenderer, ConfigureIsDormantAndASCIIIsLazy)
{
	ScopedCom com;
	if( FAILED(com.Result()) && com.Result() != RPC_E_CHANGED_MODE ) GTEST_SKIP() << "COM unavailable";
	TerminalDWriteRenderer renderer;
	renderer.Configure(Configuration());
	EXPECT_EQ(TerminalDWriteLifecycle::Dormant, renderer.Lifecycle());
	EXPECT_EQ(0u, renderer.CacheInformation().entries);
	EXPECT_EQ(0u, renderer.Counters().factoryCreationAttempts);
	EXPECT_EQ(0u, renderer.Counters().frameBegins);
	EXPECT_EQ(0u, renderer.Counters().endDrawCalls);
}

TEST(TerminalDWriteRenderer, ShapesU23F5AndCachesRealGlyphIndex)
{
	ScopedCom com;
	if( FAILED(com.Result()) && com.Result() != RPC_E_CHANGED_MODE ) GTEST_SKIP() << "COM unavailable";
	TestDib dib;
	ASSERT_TRUE(dib.Create(512, 64));
	TerminalDWriteRenderer renderer;
	renderer.Configure(Configuration());
	ASSERT_TRUE(DrawCluster(renderer, dib.Dc(), L"\u23F5"));
	EXPECT_EQ(TerminalDWriteLifecycle::Ready, renderer.Lifecycle());
	EXPECT_NE(0u, renderer.CachedFirstGlyphIndex(L"\u23F5", Style()));
	EXPECT_GT(renderer.Counters().factoryCreations, 0u);
	EXPECT_GT(renderer.Counters().mapCharactersCalls, 0u);
	EXPECT_GT(renderer.Counters().scriptAnalysisCalls, 0u);
	EXPECT_GT(renderer.Counters().bidiAnalysisCalls, 0u);
	EXPECT_GT(renderer.Counters().glyphCalls, 0u);
	EXPECT_GT(renderer.Counters().placementCalls, 0u);
	EXPECT_GT(renderer.Counters().glyphRunDraws, 0u);
	EXPECT_EQ(1u, renderer.Counters().endDrawCalls);
	EXPECT_EQ(1u, renderer.Counters().renderedFrames);
}

TEST(TerminalDWriteRenderer, MapCharactersRangeRetainsFontFaceAcrossScriptSubruns)
{
	ScopedCom com;
	if( FAILED(com.Result()) && com.Result() != RPC_E_CHANGED_MODE ) GTEST_SKIP() << "COM unavailable";
	TestDib dib;
	ASSERT_TRUE(dib.Create(512, 64));
	// Segoe UI covers both Latin and Greek here, so DirectWrite returns one
	// fallback range while script analysis splits the two code units into
	// separate runs.  The renderer must retain a usable face for every run.
	const TerminalDWriteConfiguration configuration{ L"Segoe UI", L"en-us", 16, FW_NORMAL, 96, 1 };
	const std::wstring text = L"A\u03A9";
	TerminalDWriteRenderer renderer;
	renderer.Configure(configuration);
	ASSERT_TRUE(DrawCluster(renderer, dib.Dc(), text));
	const auto& counters = renderer.Counters();
	EXPECT_EQ(1u, counters.mapCharactersCalls);
	EXPECT_GE(counters.glyphCalls, 2u);
	EXPECT_GE(counters.glyphRunDraws, 2u);
	EXPECT_EQ(1u, counters.endDrawCalls);
	EXPECT_EQ(1u, counters.renderedFrames);
}

TEST(TerminalDWriteRenderer, AbsoluteCommandRectHonorsNonzeroBindClip)
{
	ScopedCom com;
	if( FAILED(com.Result()) && com.Result() != RPC_E_CHANGED_MODE ) GTEST_SKIP() << "COM unavailable";
	constexpr COLORREF background = RGB(0x28, 0x2C, 0x34);
	TestDib dib;
	ASSERT_TRUE(dib.Create(160, 112));
	dib.Fill(background);
	TerminalDWriteRenderer renderer;
	renderer.Configure(Configuration());
	TerminalDWriteFrame frame;
	const RECT clip{ 17, 13, 143, 99 };
	ASSERT_EQ(TerminalDWriteFrameOutcome::Rendered, renderer.BeginFrame(dib.Dc(), clip, frame));
	const RECT modelRect{ 43, 29, 75, 77 };
	ASSERT_TRUE(renderer.DrawCluster({ modelRect, Style(), 0, 1 }, L"\u23F5"));
	ASSERT_EQ(TerminalDWriteFrameOutcome::Rendered, renderer.FinalizeFrame(frame));
	EXPECT_GT(CountNotColor(dib, background, modelRect), 0u);
	EXPECT_EQ(0u, CountNotColorOutside(dib, background, modelRect));
}

TEST(TerminalDWriteRenderer, FullSurfaceBindKeepsJapaneseVisibleToPartialGdiCopy)
{
	ScopedCom com;
	if( FAILED(com.Result()) && com.Result() != RPC_E_CHANGED_MODE ) GTEST_SKIP() << "COM unavailable";
	constexpr COLORREF background = RGB(0x28, 0x2C, 0x34);
	TestDib source;
	TestDib destination;
	ASSERT_TRUE(source.Create(160, 112));
	ASSERT_TRUE(destination.Create(160, 112));
	source.Fill(background);
	destination.Fill(background);
	TerminalDWriteRenderer renderer;
	renderer.Configure(Configuration());
	TerminalDWriteFrame frame;
	const RECT surface{ 0, 0, source.Width(), source.Height() };
	const RECT damage{ 0, 32, 160, 64 };
	ASSERT_EQ(TerminalDWriteFrameOutcome::Rendered, renderer.BeginFrame(source.Dc(), surface, frame));
	const RECT modelRect{ 8, 32, 40, 64 };
	ASSERT_TRUE(renderer.DrawCluster({ modelRect, Style(), 0, 1 }, L"\u65e5"));
	ASSERT_EQ(TerminalDWriteFrameOutcome::Rendered, renderer.FinalizeFrame(frame));
	ASSERT_GT(CountNotColor(source, background, modelRect), 0u);
	ASSERT_TRUE(::BitBlt(destination.Dc(), damage.left, damage.top, damage.right - damage.left,
		damage.bottom - damage.top, source.Dc(), damage.left, damage.top, SRCCOPY));
	EXPECT_GT(CountNotColor(destination, background, modelRect), 0u);
	EXPECT_EQ(0u, CountNotColorOutside(destination, background, modelRect));
}

TEST(TerminalDWriteRenderer, CacheEntriesAndBytesStayWithinBound)
{
	ScopedCom com;
	if( FAILED(com.Result()) && com.Result() != RPC_E_CHANGED_MODE ) GTEST_SKIP() << "COM unavailable";
	TestDib dib;
	ASSERT_TRUE(dib.Create(512, 64));
	TerminalDWriteRenderer renderer;
	renderer.Configure(Configuration());
	TerminalDWriteFrame frame;
	ASSERT_EQ(TerminalDWriteFrameOutcome::Rendered,
		renderer.BeginFrame(dib.Dc(), { 0, 0, 512, 64 }, frame));
	for( std::uint32_t index = 0; index < 300; ++index ) {
		const std::wstring text(1, static_cast<wchar_t>(0x4E00 + index));
		const RECT rect{ 0, 0, 16, 32 };
		const TerminalShapedClusterCommand command{ rect, Style(), 0, text.size() };
		ASSERT_TRUE(renderer.DrawCluster(command, text)) << index;
	}
	ASSERT_EQ(TerminalDWriteFrameOutcome::Rendered, renderer.FinalizeFrame(frame));
	const auto cache = renderer.CacheInformation();
	const auto counters = renderer.Counters();
	ASSERT_GT(counters.cache.insertions, 0u);
	ASSERT_EQ(TerminalDWriteRenderer::kMaximumCacheEntries, cache.entries);
	ASSERT_GT(counters.cache.evictions, 0u);
	EXPECT_LE(cache.entries, TerminalDWriteRenderer::kMaximumCacheEntries);
	EXPECT_LE(cache.bytes, TerminalDWriteRenderer::kMaximumCacheBytes);
	EXPECT_EQ(TerminalDWriteRenderer::kMaximumCacheEntries, cache.entryCapacity);
	EXPECT_EQ(TerminalDWriteRenderer::kMaximumCacheBytes, cache.byteCapacity);

	const auto beforeDistinctInsertion = renderer.Counters().cache;
	ASSERT_TRUE(DrawCluster(renderer, dib.Dc(), L"\u4F2C"));
	const auto afterDistinctInsertion = renderer.Counters().cache;
	EXPECT_GT(afterDistinctInsertion.insertions, beforeDistinctInsertion.insertions);
	EXPECT_GT(afterDistinctInsertion.evictions, beforeDistinctInsertion.evictions);
	EXPECT_EQ(TerminalDWriteRenderer::kMaximumCacheEntries, renderer.CacheInformation().entries);
	EXPECT_LE(renderer.CacheInformation().bytes, TerminalDWriteRenderer::kMaximumCacheBytes);
}

TEST(TerminalDWriteRenderer, ConfigureAndInvalidateReturnDormantAndClearCache)
{
	ScopedCom com;
	if( FAILED(com.Result()) && com.Result() != RPC_E_CHANGED_MODE ) GTEST_SKIP() << "COM unavailable";
	TestDib dib;
	ASSERT_TRUE(dib.Create(512, 64));
	TerminalDWriteRenderer renderer;
	renderer.Configure(Configuration(1));
	ASSERT_TRUE(DrawCluster(renderer, dib.Dc(), L"\u23F5"));
	ASSERT_GT(renderer.CacheInformation().entries, 0u);
	renderer.Configure(Configuration(2));
	EXPECT_EQ(TerminalDWriteLifecycle::Dormant, renderer.Lifecycle());
	EXPECT_EQ(0u, renderer.CacheInformation().entries);
	EXPECT_EQ(0u, renderer.CacheInformation().bytes);
	ASSERT_TRUE(DrawCluster(renderer, dib.Dc(), L"\u23F5"));
	renderer.Invalidate();
	EXPECT_EQ(TerminalDWriteLifecycle::Dormant, renderer.Lifecycle());
	EXPECT_EQ(0u, renderer.CacheInformation().entries);
	EXPECT_EQ(0u, renderer.CacheInformation().bytes);
}

TEST(TerminalDWriteRenderer, InjectedTargetLossRecoversWithOneRecreation)
{
	ScopedCom com;
	if( FAILED(com.Result()) && com.Result() != RPC_E_CHANGED_MODE ) GTEST_SKIP() << "COM unavailable";
	TestDib dib;
	ASSERT_TRUE(dib.Create(512, 64));
	TerminalDWriteRenderer renderer;
	renderer.Configure(Configuration());
	TerminalDWriteFrame frame;
	ASSERT_EQ(TerminalDWriteFrameOutcome::Rendered,
		renderer.BeginFrame(dib.Dc(), { 0, 0, 512, 64 }, frame));
	ASSERT_TRUE(renderer.DrawCluster({ { 0, 0, 16, 32 }, Style(), 0, 1 }, L"\u23F5"));
	renderer.SetFaultInjection({ 1 });
	EXPECT_EQ(TerminalDWriteFrameOutcome::UnavailableForFrame, renderer.FinalizeFrame(frame));
	EXPECT_EQ(TerminalDWriteLifecycle::TargetLost, renderer.Lifecycle());
	const auto afterLoss = renderer.Counters();
	EXPECT_EQ(1u, afterLoss.endDrawCalls);
	EXPECT_EQ(1u, afterLoss.targetLosses);
	EXPECT_EQ(1u, afterLoss.unavailableFrames);

	TerminalDWriteFrame recovered;
	EXPECT_EQ(TerminalDWriteFrameOutcome::Rendered,
		renderer.BeginFrame(dib.Dc(), { 0, 0, 512, 64 }, recovered));
	ASSERT_TRUE(renderer.DrawCluster({ { 0, 0, 16, 32 }, Style(), 0, 1 }, L"\u23F5"));
	EXPECT_EQ(TerminalDWriteFrameOutcome::Rendered, renderer.FinalizeFrame(recovered));
	EXPECT_EQ(TerminalDWriteLifecycle::Ready, renderer.Lifecycle());
	EXPECT_EQ(afterLoss.targetCreationAttempts + 1, renderer.Counters().targetCreationAttempts);
	EXPECT_EQ(afterLoss.endDrawCalls + 1, renderer.Counters().endDrawCalls);
}

TEST(TerminalDWriteRenderer, AbortAndCloseAreIdempotentAndFinalizeExactlyOnce)
{
	ScopedCom com;
	if( FAILED(com.Result()) && com.Result() != RPC_E_CHANGED_MODE ) GTEST_SKIP() << "COM unavailable";
	TestDib dib;
	ASSERT_TRUE(dib.Create(512, 64));
	TerminalDWriteRenderer renderer;
	renderer.Configure(Configuration());
	TerminalDWriteFrame frame;
	ASSERT_EQ(TerminalDWriteFrameOutcome::Rendered,
		renderer.BeginFrame(dib.Dc(), { 0, 0, 512, 64 }, frame));
	ASSERT_TRUE(renderer.DrawCluster({ { 0, 0, 16, 32 }, Style(), 0, 1 }, L"\u23F5"));
	renderer.AbortFrame(frame);
	const auto afterAbort = renderer.Counters();
	EXPECT_EQ(1u, afterAbort.endDrawCalls);
	EXPECT_EQ(1u, afterAbort.abortedFrames);
	renderer.AbortFrame(frame);
	EXPECT_EQ(afterAbort.endDrawCalls, renderer.Counters().endDrawCalls);
	EXPECT_EQ(TerminalDWriteFrameOutcome::UnavailableForFrame, renderer.FinalizeFrame(frame));
	EXPECT_EQ(afterAbort.endDrawCalls, renderer.Counters().endDrawCalls);

	renderer.Close();
	renderer.Close();
	EXPECT_EQ(TerminalDWriteLifecycle::Dormant, renderer.Lifecycle());
	EXPECT_EQ(0u, renderer.CacheInformation().entries);
	EXPECT_EQ(0u, renderer.CacheInformation().bytes);
}

} // namespace
} // namespace terminal

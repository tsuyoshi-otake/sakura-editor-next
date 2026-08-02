/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "terminal/window/TerminalDWriteRenderer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <new>

#include <d2d1helper.h>

namespace terminal {
namespace {

constexpr float kTargetDpi = 96.0f;
constexpr std::size_t kMaximumGlyphsPerCluster = TerminalCell::kMaxCodeUnits * 3 + 16;
constexpr std::size_t kMaximumAnalysisRuns = TerminalCell::kMaxCodeUnits + 1;

[[nodiscard]] float ColorChannel(BYTE value) noexcept
{
	return static_cast<float>(value) / 255.0f;
}

class TextAnalysisSource final : public IDWriteTextAnalysisSource {
public:
	TextAnalysisSource(std::wstring_view text, const wchar_t* locale) noexcept
		: m_text(text)
		, m_locale(locale == nullptr ? L"" : locale)
	{
	}

	IFACEMETHOD(QueryInterface)(REFIID iid, void** object) override
	{
		if( object == nullptr ) return E_INVALIDARG;
		*object = nullptr;
		if( iid == __uuidof(IUnknown) || iid == __uuidof(IDWriteTextAnalysisSource) ) {
			*object = static_cast<IDWriteTextAnalysisSource*>(this);
			return S_OK;
		}
		return E_NOINTERFACE;
	}
	IFACEMETHOD_(ULONG, AddRef)() override { return 2; }
	IFACEMETHOD_(ULONG, Release)() override { return 1; }

	IFACEMETHOD(GetTextAtPosition)(UINT32 position, WCHAR const** text, UINT32* length) override
	{
		if( text == nullptr || length == nullptr ) return E_INVALIDARG;
		if( position >= m_text.size() ) {
			*text = nullptr;
			*length = 0;
			return S_OK;
		}
		*text = m_text.data() + position;
		*length = static_cast<UINT32>(m_text.size() - position);
		return S_OK;
	}
	IFACEMETHOD(GetTextBeforePosition)(UINT32 position, WCHAR const** text, UINT32* length) override
	{
		if( text == nullptr || length == nullptr ) return E_INVALIDARG;
		const auto available = std::min<std::size_t>(position, m_text.size());
		*text = available == 0 ? nullptr : m_text.data();
		*length = static_cast<UINT32>(available);
		return S_OK;
	}
	IFACEMETHOD_(DWRITE_READING_DIRECTION, GetParagraphReadingDirection)() override
	{
		return DWRITE_READING_DIRECTION_LEFT_TO_RIGHT;
	}
	IFACEMETHOD(GetLocaleName)(UINT32 position, UINT32* length, WCHAR const** locale) override
	{
		if( length == nullptr || locale == nullptr ) return E_INVALIDARG;
		*locale = m_locale;
		*length = position >= m_text.size() ? 0 : static_cast<UINT32>(m_text.size() - position);
		return S_OK;
	}
	IFACEMETHOD(GetNumberSubstitution)(UINT32 position, UINT32* length,
		IDWriteNumberSubstitution** substitution) override
	{
		if( length == nullptr || substitution == nullptr ) return E_INVALIDARG;
		*length = position >= m_text.size() ? 0 : static_cast<UINT32>(m_text.size() - position);
		*substitution = nullptr;
		return S_OK;
	}

private:
	std::wstring_view m_text;
	const wchar_t* m_locale{};
};

struct ScriptRun {
	UINT32 start{};
	UINT32 length{};
	DWRITE_SCRIPT_ANALYSIS analysis{};
};

struct BidiRun {
	UINT32 start{};
	UINT32 length{};
	UINT8 level{};
};

class TextAnalysisSink final : public IDWriteTextAnalysisSink {
public:
	explicit TextAnalysisSink(UINT32 length) noexcept
		: m_length(length)
	{
	}

	IFACEMETHOD(QueryInterface)(REFIID iid, void** object) override
	{
		if( object == nullptr ) return E_INVALIDARG;
		*object = nullptr;
		if( iid == __uuidof(IUnknown) || iid == __uuidof(IDWriteTextAnalysisSink) ) {
			*object = static_cast<IDWriteTextAnalysisSink*>(this);
			return S_OK;
		}
		return E_NOINTERFACE;
	}
	IFACEMETHOD_(ULONG, AddRef)() override { return 2; }
	IFACEMETHOD_(ULONG, Release)() override { return 1; }

	IFACEMETHOD(SetScriptAnalysis)(UINT32 position, UINT32 length,
		DWRITE_SCRIPT_ANALYSIS const* analysis) override
	{
		if( analysis == nullptr ) return E_INVALIDARG;
		if( m_scriptCount < m_scripts.size() ) m_scripts[m_scriptCount++] = { position, length, *analysis };
		return S_OK;
	}
	IFACEMETHOD(SetLineBreakpoints)(UINT32, UINT32, DWRITE_LINE_BREAKPOINT const*) override { return S_OK; }
	IFACEMETHOD(SetBidiLevel)(UINT32 position, UINT32 length, UINT8, UINT8 resolvedLevel) override
	{
		if( m_bidiCount < m_bidi.size() ) m_bidi[m_bidiCount++] = { position, length, resolvedLevel };
		return S_OK;
	}
	IFACEMETHOD(SetNumberSubstitution)(UINT32, UINT32, IDWriteNumberSubstitution*) override { return S_OK; }

	[[nodiscard]] DWRITE_SCRIPT_ANALYSIS ScriptAt(UINT32 position, UINT32& end) const noexcept
	{
		for( std::size_t index = 0; index < m_scriptCount; ++index ) {
			const auto& run = m_scripts[index];
			if( position >= run.start && position < run.start + run.length ) {
				end = run.start + run.length;
				return run.analysis;
			}
		}
		end = m_length;
		return {};
	}

	[[nodiscard]] UINT8 BidiAt(UINT32 position, UINT32& end) const noexcept
	{
		for( std::size_t index = 0; index < m_bidiCount; ++index ) {
			const auto& run = m_bidi[index];
			if( position >= run.start && position < run.start + run.length ) {
				end = run.start + run.length;
				return run.level;
			}
		}
		end = m_length;
		return 0;
	}

private:
	UINT32 m_length{};
	std::array<ScriptRun, kMaximumAnalysisRuns> m_scripts{};
	std::array<BidiRun, kMaximumAnalysisRuns> m_bidi{};
	std::size_t m_scriptCount{};
	std::size_t m_bidiCount{};
};

} // namespace

void TerminalDWriteRenderer::ResetCacheEntry(CacheEntry& entry) noexcept
{
	entry.text.clear();
	entry.glyphIndices.clear();
	entry.advances.clear();
	entry.offsets.clear();
	entry.runs.clear();
	entry.style = {};
	entry.generation = 0;
	entry.hash = 0;
	entry.lastUse = 0;
	entry.byteSize = 0;
}

std::size_t TerminalDWriteRenderer::CacheEntryBytes(const CacheEntry& entry) noexcept
{
	return sizeof(entry) + entry.text.capacity() * sizeof(wchar_t) +
		entry.glyphIndices.capacity() * sizeof(std::uint16_t) + entry.advances.capacity() * sizeof(FLOAT) +
		entry.offsets.capacity() * sizeof(DWRITE_GLYPH_OFFSET) + entry.runs.capacity() * sizeof(CachedGlyphRun);
}

float TerminalDWriteRenderer::BaselineFor(const CachedGlyphRun& run, const RECT& rect) noexcept
{
	DWRITE_FONT_METRICS metrics{};
	run.fontFace->GetMetrics(&metrics);
	const float height = static_cast<float>(std::max<LONG>(1, rect.bottom - rect.top));
	if( metrics.designUnitsPerEm == 0 ) return static_cast<float>(rect.top) + height * 0.8f;
	const float scale = run.fontEmSize / static_cast<float>(metrics.designUnitsPerEm);
	const float ascent = static_cast<float>(metrics.ascent) * scale;
	const float descent = static_cast<float>(metrics.descent) * scale;
	const float baseline = static_cast<float>(rect.top) + std::max(0.0f, (height - ascent - descent) * 0.5f) + ascent;
	return std::clamp(baseline, static_cast<float>(rect.top), static_cast<float>(rect.bottom));
}

TerminalDWriteRenderer::TerminalDWriteRenderer()
{
	UpdateCacheCounters();
}

TerminalDWriteRenderer::~TerminalDWriteRenderer()
{
	Close();
}

void TerminalDWriteRenderer::Configure(const TerminalDWriteConfiguration& configuration)
{
	if( m_drawing && m_d2dTarget ) {
		static_cast<void>(m_d2dTarget->EndDraw());
		++m_counters.endDrawCalls;
		++m_counters.abortedFrames;
	}
	m_drawing = false;
	ReleaseTarget();
	ClearCache();
	m_primaryFamily.assign(configuration.primaryFamily);
	m_locale.assign(configuration.locale.empty() ? L"" : configuration.locale);
	m_fontPixelHeight = std::max(1, configuration.fontPixelHeight);
	m_fontWeight = std::clamp(configuration.fontWeight, static_cast<int>(DWRITE_FONT_WEIGHT_THIN),
		static_cast<int>(DWRITE_FONT_WEIGHT_BLACK));
	m_dpi = configuration.dpi == 0 ? 96U : configuration.dpi;
	m_generation = configuration.generation;
	m_configured = !m_primaryFamily.empty();
	// Factories are intentionally retained only after a prior shaped frame.  The
	// persistent state nevertheless returns to Dormant and retryable font/fallback
	// resources are rebuilt on the next eligible frame.
	m_textAnalyzer.Reset();
	m_fontFallback.Reset();
	m_fontCollection.Reset();
	m_brush.Reset();
	m_brushColor = CLR_INVALID;
	m_lifecycle = TerminalDWriteLifecycle::Dormant;
}

void TerminalDWriteRenderer::Invalidate() noexcept
{
	if( m_drawing && m_d2dTarget ) {
		static_cast<void>(m_d2dTarget->EndDraw());
		++m_counters.endDrawCalls;
		++m_counters.abortedFrames;
	}
	m_drawing = false;
	ReleaseTarget();
	ClearCache();
	m_textAnalyzer.Reset();
	m_fontFallback.Reset();
	m_fontCollection.Reset();
	m_lifecycle = TerminalDWriteLifecycle::Dormant;
}

void TerminalDWriteRenderer::Close() noexcept
{
	Invalidate();
	m_d2dFactory.Reset();
	m_dwriteFactory.Reset();
	m_brush.Reset();
	m_primaryFamily.clear();
	m_locale.clear();
	m_clusterMap = {};
	m_textProperties = {};
	m_glyphIndices = {};
	m_glyphProperties = {};
	m_glyphAdvances = {};
	m_glyphOffsets = {};
	m_drawAdvances = {};
	m_drawOffsets = {};
	m_cache = {};
	m_cacheIndex = {};
	m_transientEntry = {};
	m_cacheBytes = 0;
	m_cacheClock = 0;
	m_generation = 0;
	m_configured = false;
	m_lifecycle = TerminalDWriteLifecycle::Dormant;
	UpdateCacheCounters();
}

TerminalDWriteLifecycle TerminalDWriteRenderer::Lifecycle() const noexcept
{
	return m_lifecycle;
}

const TerminalDWriteRendererCounters& TerminalDWriteRenderer::Counters() const noexcept
{
	return m_counters;
}

TerminalDWriteCacheInformation TerminalDWriteRenderer::CacheInformation() const noexcept
{
	return m_counters.cache;
}

std::uint16_t TerminalDWriteRenderer::CachedFirstGlyphIndex(std::wstring_view text,
	const TerminalRenderStyle& style) const noexcept
{
	const auto hash = CacheHash(text, style);
	const auto range = m_cacheIndex.equal_range(hash);
	for( auto index = range.first; index != range.second; ++index ) {
		if( index->second >= m_cache.size() ) continue;
		const auto& entry = m_cache[index->second];
		if( entry.hash == hash && entry.generation == m_generation && entry.style == style && entry.text == text &&
			!entry.glyphIndices.empty() ) return entry.glyphIndices.front();
	}
	return 0;
}

void TerminalDWriteRenderer::SetFaultInjection(TerminalDWriteFaultInjection fault) noexcept
{
	m_faultInjection = fault;
}

TerminalDWriteFrameOutcome TerminalDWriteRenderer::BeginFrame(HDC target, const RECT& clip,
	TerminalDWriteFrame& frame) noexcept
{
	frame = {};
	if( target == nullptr || clip.right <= clip.left || clip.bottom <= clip.top || m_drawing || !m_configured ) {
		++m_counters.unavailableFrames;
		return TerminalDWriteFrameOutcome::UnavailableForFrame;
	}
	frame.m_identifier = ++m_nextFrameIdentifier;
	if( !EnsureTarget(frame) ) {
		++m_counters.unavailableFrames;
		return TerminalDWriteFrameOutcome::UnavailableForFrame;
	}
	if( FAILED(m_d2dTarget->BindDC(target, &clip)) ) {
		RecordTargetLoss();
		++m_counters.unavailableFrames;
		return TerminalDWriteFrameOutcome::UnavailableForFrame;
	}
	++m_counters.targetBinds;
	m_d2dTarget->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
	m_d2dTarget->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
	m_d2dTarget->BeginDraw();
	m_drawing = true;
	frame.m_begun = true;
	++m_counters.frameBegins;
	return TerminalDWriteFrameOutcome::Rendered;
}

bool TerminalDWriteRenderer::DrawCluster(const TerminalShapedClusterCommand& command,
	std::wstring_view text) noexcept
{
	if( !m_drawing || m_d2dTarget == nullptr || text.empty() || command.rect.right <= command.rect.left ||
		command.rect.bottom <= command.rect.top ) return false;
	const auto* entry = FindOrShape(text, command.style);
	return entry != nullptr && DrawCachedCluster(command, *entry);
}

TerminalDWriteFrameOutcome TerminalDWriteRenderer::FinalizeFrame(TerminalDWriteFrame& frame) noexcept
{
	if( !frame.m_begun || frame.m_finalized ) {
		++m_counters.unavailableFrames;
		return TerminalDWriteFrameOutcome::UnavailableForFrame;
	}
	return EndFrame(frame, false);
}

void TerminalDWriteRenderer::AbortFrame(TerminalDWriteFrame& frame) noexcept
{
	if( !frame.m_begun || frame.m_finalized ) return;
	static_cast<void>(EndFrame(frame, true));
}

bool TerminalDWriteRenderer::EnsureInfrastructure() noexcept
{
	if( !m_configured ) return false;
	if( m_d2dFactory && m_dwriteFactory && m_textAnalyzer && m_fontFallback && m_fontCollection ) return true;
	++m_counters.factoryCreationAttempts;
	try {
		if( !m_d2dFactory && FAILED(::D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, m_d2dFactory.GetAddressOf())) ) {
			m_d2dFactory.Reset();
			return false;
		}
		if( !m_dwriteFactory && FAILED(::DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory2),
			reinterpret_cast<IUnknown**>(m_dwriteFactory.GetAddressOf()))) ) {
			m_dwriteFactory.Reset();
			m_d2dFactory.Reset();
			return false;
		}
		if( FAILED(m_dwriteFactory->CreateTextAnalyzer(m_textAnalyzer.GetAddressOf())) ||
			FAILED(m_dwriteFactory->GetSystemFontFallback(m_fontFallback.GetAddressOf())) ||
			FAILED(m_dwriteFactory->GetSystemFontCollection(m_fontCollection.GetAddressOf())) ) {
			m_textAnalyzer.Reset();
			m_fontFallback.Reset();
			m_fontCollection.Reset();
			return false;
		}
		m_clusterMap.reserve(TerminalCell::kMaxCodeUnits);
		m_textProperties.reserve(TerminalCell::kMaxCodeUnits);
		m_glyphIndices.reserve(kMaximumGlyphsPerCluster);
		m_glyphProperties.reserve(kMaximumGlyphsPerCluster);
		m_glyphAdvances.reserve(kMaximumGlyphsPerCluster);
		m_glyphOffsets.reserve(kMaximumGlyphsPerCluster);
		m_drawAdvances.reserve(kMaximumGlyphsPerCluster);
		m_drawOffsets.reserve(kMaximumGlyphsPerCluster);
		++m_counters.factoryCreations;
		return true;
	} catch( const std::bad_alloc& ) {
		m_textAnalyzer.Reset();
		m_fontFallback.Reset();
		m_fontCollection.Reset();
		return false;
	}
}

bool TerminalDWriteRenderer::EnsureTarget(TerminalDWriteFrame& frame) noexcept
{
	if( frame.m_creationAttempted ) return false;
	frame.m_creationAttempted = true;
	if( !EnsureInfrastructure() ) return false;
	if( m_d2dTarget ) {
		m_lifecycle = TerminalDWriteLifecycle::Ready;
		return true;
	}
	if( !CreateTarget() ) {
		m_lifecycle = TerminalDWriteLifecycle::TargetLost;
		return false;
	}
	m_lifecycle = TerminalDWriteLifecycle::Ready;
	return true;
}

bool TerminalDWriteRenderer::CreateTarget() noexcept
{
	if( !m_d2dFactory ) return false;
	++m_counters.targetCreationAttempts;
	m_d2dTarget.Reset();
	m_brush.Reset();
	m_brushColor = CLR_INVALID;
	const auto properties = D2D1::RenderTargetProperties(
		D2D1_RENDER_TARGET_TYPE_DEFAULT,
		D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
		kTargetDpi, kTargetDpi, D2D1_RENDER_TARGET_USAGE_NONE, D2D1_FEATURE_LEVEL_DEFAULT);
	if( FAILED(m_d2dFactory->CreateDCRenderTarget(&properties, m_d2dTarget.GetAddressOf())) ) {
		m_d2dTarget.Reset();
		return false;
	}
	++m_counters.targetCreations;
	return true;
}

bool TerminalDWriteRenderer::CreateBrush(COLORREF color) noexcept
{
	if( m_brush && m_brushColor == color ) return true;
	if( !m_d2dTarget ) return false;
	const auto d2dColor = D2D1::ColorF(ColorChannel(GetRValue(color)), ColorChannel(GetGValue(color)),
		ColorChannel(GetBValue(color)), 1.0f);
	if( m_brush ) m_brush->SetColor(d2dColor);
	else if( FAILED(m_d2dTarget->CreateSolidColorBrush(d2dColor, m_brush.GetAddressOf())) ) return false;
	m_brushColor = color;
	return true;
}

TerminalDWriteRenderer::CacheEntry* TerminalDWriteRenderer::FindCache(std::wstring_view text,
	const TerminalRenderStyle& style, std::uint64_t hash) noexcept
{
	const auto range = m_cacheIndex.equal_range(hash);
	for( auto index = range.first; index != range.second; ++index ) {
		if( index->second >= m_cache.size() ) continue;
		auto& entry = m_cache[index->second];
		if( entry.hash == hash && entry.generation == m_generation && entry.style == style && entry.text == text ) {
			entry.lastUse = ++m_cacheClock;
			++m_counters.cache.hits;
			return &entry;
		}
	}
	return nullptr;
}

bool TerminalDWriteRenderer::ShapeCluster(std::wstring_view text, const TerminalRenderStyle& style,
	CacheEntry& entry) noexcept
{
	if( text.empty() || text.size() > TerminalCell::kMaxCodeUnits || !m_textAnalyzer || !m_fontFallback ||
		!m_fontCollection ) return false;
	try {
		ResetCacheEntry(entry);
		entry.text.assign(text);
		entry.style = style;
		entry.generation = m_generation;
		entry.hash = CacheHash(text, style);
		entry.glyphIndices.reserve(kMaximumGlyphsPerCluster);
		entry.advances.reserve(kMaximumGlyphsPerCluster);
		entry.offsets.reserve(kMaximumGlyphsPerCluster);
		entry.runs.reserve(TerminalCell::kMaxCodeUnits);

		TextAnalysisSource source(text, m_locale.c_str());
		TextAnalysisSink analysis(static_cast<UINT32>(text.size()));
		++m_counters.scriptAnalysisCalls;
		if( FAILED(m_textAnalyzer->AnalyzeScript(&source, 0, static_cast<UINT32>(text.size()), &analysis)) ) {
			ResetCacheEntry(entry);
			return false;
		}
		++m_counters.bidiAnalysisCalls;
		if( FAILED(m_textAnalyzer->AnalyzeBidi(&source, 0, static_cast<UINT32>(text.size()), &analysis)) ) {
			ResetCacheEntry(entry);
			return false;
		}

		UINT32 position = 0;
		const UINT32 totalLength = static_cast<UINT32>(text.size());
		const int requestedWeight = style.bold
			? std::max(m_fontWeight, static_cast<int>(DWRITE_FONT_WEIGHT_BOLD))
			: m_fontWeight;
		const auto effectiveWeight = static_cast<DWRITE_FONT_WEIGHT>(requestedWeight);
		while( position < totalLength ) {
			UINT32 mappedLength{};
			FLOAT scale{};
			Microsoft::WRL::ComPtr<IDWriteFont> mappedFont;
			++m_counters.mapCharactersCalls;
			if( FAILED(m_fontFallback->MapCharacters(&source, position, totalLength - position,
				m_fontCollection.Get(), m_primaryFamily.c_str(), effectiveWeight,
				DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, &mappedLength,
				mappedFont.GetAddressOf(), &scale)) || mappedLength == 0 || !mappedFont ) {
				ResetCacheEntry(entry);
				return false;
			}
			Microsoft::WRL::ComPtr<IDWriteFontFace> fontFace;
			if( FAILED(mappedFont->CreateFontFace(fontFace.GetAddressOf())) || !fontFace ) {
				ResetCacheEntry(entry);
				return false;
			}
			UINT32 mappedEnd = std::min(totalLength, position + mappedLength);
			while( position < mappedEnd ) {
				UINT32 scriptEnd{};
				UINT32 bidiEnd{};
				const auto script = analysis.ScriptAt(position, scriptEnd);
				const auto bidiLevel = analysis.BidiAt(position, bidiEnd);
				const auto end = std::min({ mappedEnd, scriptEnd, bidiEnd });
				const auto length = end > position ? end - position : 1U;
				const auto glyphCapacity = std::min<std::size_t>(kMaximumGlyphsPerCluster,
					static_cast<std::size_t>(length) * 3 + 16);
				m_clusterMap.resize(length);
				m_textProperties.resize(length);
				m_glyphIndices.resize(glyphCapacity);
				m_glyphProperties.resize(glyphCapacity);
				m_glyphAdvances.resize(glyphCapacity);
				m_glyphOffsets.resize(glyphCapacity);
				UINT32 actualGlyphCount{};
				++m_counters.glyphCalls;
				if( FAILED(m_textAnalyzer->GetGlyphs(text.data() + position, length, fontFace.Get(), FALSE,
					(bidiLevel & 1U) != 0, &script, m_locale.c_str(), nullptr, nullptr, nullptr, 0,
					static_cast<UINT32>(glyphCapacity), m_clusterMap.data(), m_textProperties.data(),
					m_glyphIndices.data(), m_glyphProperties.data(), &actualGlyphCount)) || actualGlyphCount == 0 ||
					actualGlyphCount > glyphCapacity ) {
					ResetCacheEntry(entry);
					return false;
				}
				++m_counters.placementCalls;
				const float fontEmSize = static_cast<float>(m_fontPixelHeight) * (scale > 0.0f ? scale : 1.0f);
				if( FAILED(m_textAnalyzer->GetGlyphPlacements(text.data() + position, m_clusterMap.data(),
					m_textProperties.data(), length, m_glyphIndices.data(), m_glyphProperties.data(), actualGlyphCount,
					fontFace.Get(), fontEmSize, FALSE, (bidiLevel & 1U) != 0, &script, m_locale.c_str(), nullptr, nullptr, 0,
					m_glyphAdvances.data(), m_glyphOffsets.data())) ) {
					ResetCacheEntry(entry);
					return false;
				}
				const auto glyphOffset = entry.glyphIndices.size();
				entry.glyphIndices.insert(entry.glyphIndices.end(), m_glyphIndices.begin(),
					m_glyphIndices.begin() + actualGlyphCount);
				entry.advances.insert(entry.advances.end(), m_glyphAdvances.begin(),
					m_glyphAdvances.begin() + actualGlyphCount);
				entry.offsets.insert(entry.offsets.end(), m_glyphOffsets.begin(),
					m_glyphOffsets.begin() + actualGlyphCount);
				// A single MapCharacters range may contain several script/bidi runs.
				// Each cached run owns a ComPtr reference to the mapped face; moving it
				// here would leave later iterations of this inner loop with no face.
				entry.runs.push_back({ fontFace, glyphOffset, actualGlyphCount, fontEmSize, bidiLevel });
				position += length;
			}
		}
		entry.byteSize = CacheEntryBytes(entry);
		return !entry.glyphIndices.empty();
	} catch( const std::bad_alloc& ) {
		ResetCacheEntry(entry);
		return false;
	}
}

TerminalDWriteRenderer::CacheEntry* TerminalDWriteRenderer::FindOrShape(std::wstring_view text,
	const TerminalRenderStyle& style) noexcept
{
	const auto hash = CacheHash(text, style);
	if( auto* cached = FindCache(text, style, hash) ) return cached;
	++m_counters.cache.misses;
	if( !ShapeCluster(text, style, m_transientEntry) ) return nullptr;
	m_transientEntry.lastUse = ++m_cacheClock;
	try {
		// Copying the bounded transient entry can still allocate.  Keep that
		// copy inside this noexcept boundary so allocation pressure degrades to
		// the per-frame transient fallback rather than terminating the paint.
		CacheEntry cacheEntry(m_transientEntry);
		if( InsertCacheEntry(std::move(cacheEntry)) ) return &m_cache.back();
	} catch( const std::bad_alloc& ) {
		++m_counters.cache.rejectedInsertions;
		UpdateCacheCounters();
	}
	return &m_transientEntry;
}

bool TerminalDWriteRenderer::DrawCachedCluster(const TerminalShapedClusterCommand& command,
	const CacheEntry& entry) noexcept
{
	if( !CreateBrush(command.style.foreground) || entry.glyphIndices.empty() ) return false;
	float naturalAdvance{};
	for( const auto advance : entry.advances ) naturalAdvance += std::max(0.0f, advance);
	const float targetAdvance = static_cast<float>(std::max<LONG>(1, command.rect.right - command.rect.left));
	const float normalization = naturalAdvance > 0.0f ? targetAdvance / naturalAdvance : 0.0f;
	std::size_t completedGlyphs{};
	const auto totalGlyphs = entry.glyphIndices.size();
	float cursor = static_cast<float>(command.rect.left);
	for( const auto& run : entry.runs ) {
		if( !run.fontFace || run.glyphCount == 0 || run.glyphOffset + run.glyphCount > entry.glyphIndices.size() ) return false;
		m_drawAdvances.resize(run.glyphCount);
		m_drawOffsets.resize(run.glyphCount);
		float runAdvance{};
		for( std::size_t glyph = 0; glyph < run.glyphCount; ++glyph ) {
			const auto sourceIndex = run.glyphOffset + glyph;
			float advance = std::max(0.0f, entry.advances[sourceIndex]) * normalization;
			if( completedGlyphs + glyph + 1 == totalGlyphs ) {
				advance = std::max(0.0f, static_cast<float>(command.rect.right) - cursor - runAdvance);
			}
			m_drawAdvances[glyph] = advance;
			m_drawOffsets[glyph] = entry.offsets[sourceIndex];
			m_drawOffsets[glyph].advanceOffset *= normalization;
			runAdvance += advance;
		}
		DWRITE_GLYPH_RUN glyphRun{};
		glyphRun.fontFace = run.fontFace.Get();
		glyphRun.fontEmSize = run.fontEmSize;
		glyphRun.glyphCount = static_cast<UINT32>(run.glyphCount);
		glyphRun.glyphIndices = entry.glyphIndices.data() + run.glyphOffset;
		glyphRun.glyphAdvances = m_drawAdvances.data();
		glyphRun.glyphOffsets = m_drawOffsets.data();
		glyphRun.isSideways = FALSE;
		glyphRun.bidiLevel = run.bidiLevel;
		m_d2dTarget->PushAxisAlignedClip(D2D1::RectF(static_cast<float>(command.rect.left),
			static_cast<float>(command.rect.top), static_cast<float>(command.rect.right),
			static_cast<float>(command.rect.bottom)), D2D1_ANTIALIAS_MODE_ALIASED);
		m_d2dTarget->DrawGlyphRun(D2D1::Point2F(cursor, BaselineFor(run, command.rect)), &glyphRun,
			m_brush.Get(), DWRITE_MEASURING_MODE_NATURAL);
		m_d2dTarget->PopAxisAlignedClip();
		++m_counters.glyphRunDraws;
		cursor += runAdvance;
		completedGlyphs += run.glyphCount;
	}
	if( command.style.underline ) {
		const float underlineY = static_cast<float>(std::max<LONG>(command.rect.top, command.rect.bottom - 2));
		m_d2dTarget->PushAxisAlignedClip(D2D1::RectF(static_cast<float>(command.rect.left),
			static_cast<float>(command.rect.top), static_cast<float>(command.rect.right),
			static_cast<float>(command.rect.bottom)), D2D1_ANTIALIAS_MODE_ALIASED);
		m_d2dTarget->DrawLine(D2D1::Point2F(static_cast<float>(command.rect.left), underlineY),
			D2D1::Point2F(static_cast<float>(command.rect.right), underlineY), m_brush.Get(), 1.0f);
		m_d2dTarget->PopAxisAlignedClip();
	}
	return completedGlyphs == totalGlyphs;
}

TerminalDWriteFrameOutcome TerminalDWriteRenderer::EndFrame(TerminalDWriteFrame& frame, bool aborted) noexcept
{
	frame.m_finalized = true;
	if( !m_drawing || !m_d2dTarget ) {
		m_drawing = false;
		++m_counters.unavailableFrames;
		return TerminalDWriteFrameOutcome::UnavailableForFrame;
	}
	m_drawing = false;
	const auto result = m_d2dTarget->EndDraw();
	++m_counters.endDrawCalls;
	if( aborted ) ++m_counters.abortedFrames;
	const bool injectedLoss = m_faultInjection.targetLossesOnEndDraw != 0;
	if( injectedLoss ) --m_faultInjection.targetLossesOnEndDraw;
	if( result == D2DERR_RECREATE_TARGET || injectedLoss || FAILED(result) ) {
		RecordTargetLoss();
		++m_counters.unavailableFrames;
		return TerminalDWriteFrameOutcome::UnavailableForFrame;
	}
	if( aborted ) {
		++m_counters.unavailableFrames;
		return TerminalDWriteFrameOutcome::UnavailableForFrame;
	}
	m_lifecycle = TerminalDWriteLifecycle::Ready;
	++m_counters.renderedFrames;
	return TerminalDWriteFrameOutcome::Rendered;
}

void TerminalDWriteRenderer::ReleaseTarget() noexcept
{
	m_brush.Reset();
	m_brushColor = CLR_INVALID;
	m_d2dTarget.Reset();
}

void TerminalDWriteRenderer::RecordTargetLoss() noexcept
{
	ReleaseTarget();
	m_lifecycle = TerminalDWriteLifecycle::TargetLost;
	++m_counters.targetLosses;
}

void TerminalDWriteRenderer::ClearCache() noexcept
{
	m_cache.clear();
	m_cacheIndex.clear();
	m_cacheBytes = 0;
	ResetCacheEntry(m_transientEntry);
	UpdateCacheCounters();
}

void TerminalDWriteRenderer::RebuildCacheIndex() noexcept
{
	try {
		m_cacheIndex.clear();
		for( std::size_t index = 0; index < m_cache.size(); ++index ) {
			m_cacheIndex.emplace(m_cache[index].hash, index);
		}
	} catch( const std::bad_alloc& ) {
		m_cacheIndex.clear();
	}
}

void TerminalDWriteRenderer::UpdateCacheCounters() noexcept
{
	m_counters.cache.entries = m_cache.size();
	m_counters.cache.bytes = m_cacheBytes;
	m_counters.cache.entryCapacity = kMaximumCacheEntries;
	m_counters.cache.byteCapacity = kMaximumCacheBytes;
}

bool TerminalDWriteRenderer::InsertCacheEntry(CacheEntry&& entry) noexcept
{
	if( entry.byteSize == 0 || entry.byteSize > kMaximumCacheBytes ) {
		++m_counters.cache.rejectedInsertions;
		return false;
	}
	try {
		if( m_cache.capacity() == 0 ) {
			m_cache.reserve(kMaximumCacheEntries);
			m_cacheIndex.reserve(kMaximumCacheEntries * 2);
		}
		while( !m_cache.empty() && (m_cache.size() >= kMaximumCacheEntries ||
			m_cacheBytes > kMaximumCacheBytes - entry.byteSize) ) {
			auto leastRecent = std::min_element(m_cache.begin(), m_cache.end(), [](const auto& left, const auto& right) {
				return left.lastUse < right.lastUse;
			});
			m_cacheBytes -= leastRecent->byteSize;
			m_cache.erase(leastRecent);
			++m_counters.cache.evictions;
		}
		if( m_cache.size() >= kMaximumCacheEntries || m_cacheBytes > kMaximumCacheBytes - entry.byteSize ) {
			++m_counters.cache.rejectedInsertions;
			UpdateCacheCounters();
			return false;
		}
		m_cache.push_back(std::move(entry));
		m_cacheBytes += m_cache.back().byteSize;
		RebuildCacheIndex();
		++m_counters.cache.insertions;
		UpdateCacheCounters();
		return !m_cacheIndex.empty();
	} catch( const std::bad_alloc& ) {
		++m_counters.cache.rejectedInsertions;
		UpdateCacheCounters();
		return false;
	}
}

std::uint64_t TerminalDWriteRenderer::CacheHash(std::wstring_view text, const TerminalRenderStyle& style) const noexcept
{
	std::uint64_t value = 1469598103934665603ULL;
	const auto append = [&value](std::uint64_t part) {
		value ^= part;
		value *= 1099511628211ULL;
	};
	for( const auto codeUnit : text ) append(static_cast<std::uint16_t>(codeUnit));
	append(style.foreground);
	append(style.background);
	append(style.bold ? 1U : 0U);
	append(style.underline ? 1U : 0U);
	append(style.inverse ? 1U : 0U);
	append(style.selected ? 1U : 0U);
	append(m_generation);
	return value;
}

} // namespace terminal

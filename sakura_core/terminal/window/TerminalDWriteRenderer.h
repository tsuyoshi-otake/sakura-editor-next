/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "terminal/window/TerminalRenderPlan.h"

#include <d2d1.h>
#include <dwrite_2.h>
#include <wrl/client.h>

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace terminal {

enum class TerminalDWriteLifecycle : std::uint8_t {
	Dormant,
	Ready,
	TargetLost,
};

enum class TerminalDWriteFrameOutcome : std::uint8_t {
	Rendered,
	UnavailableForFrame,
};

struct TerminalDWriteConfiguration {
	std::wstring_view primaryFamily;
	std::wstring_view locale;
	int fontPixelHeight{ 1 };
	int fontWeight{ FW_NORMAL };
	unsigned int dpi{ 96 };
	std::uint64_t generation{};
};

struct TerminalDWriteFrame {
	[[nodiscard]] bool Begun() const noexcept { return m_begun; }
	[[nodiscard]] bool Finalized() const noexcept { return m_finalized; }

private:
	friend class TerminalDWriteRenderer;
	std::uint64_t m_identifier{};
	bool m_begun{};
	bool m_finalized{};
	bool m_creationAttempted{};
};

//! Explicit deterministic test seam.  It is inactive by default and never
//! performs retries; a caller must opt in to every injected target loss.
struct TerminalDWriteFaultInjection {
	std::uint32_t targetLossesOnEndDraw{};
};

struct TerminalDWriteCacheInformation {
	std::size_t entries{};
	std::size_t bytes{};
	std::size_t entryCapacity{};
	std::size_t byteCapacity{};
	std::uint64_t hits{};
	std::uint64_t misses{};
	std::uint64_t insertions{};
	std::uint64_t evictions{};
	std::uint64_t rejectedInsertions{};
};

struct TerminalDWriteRendererCounters {
	std::uint64_t factoryCreationAttempts{};
	std::uint64_t factoryCreations{};
	std::uint64_t targetCreationAttempts{};
	std::uint64_t targetCreations{};
	std::uint64_t targetBinds{};
	std::uint64_t frameBegins{};
	std::uint64_t endDrawCalls{};
	std::uint64_t abortedFrames{};
	std::uint64_t renderedFrames{};
	std::uint64_t unavailableFrames{};
	std::uint64_t targetLosses{};
	std::uint64_t mapCharactersCalls{};
	std::uint64_t scriptAnalysisCalls{};
	std::uint64_t bidiAnalysisCalls{};
	std::uint64_t glyphCalls{};
	std::uint64_t placementCalls{};
	std::uint64_t glyphRunDraws{};
	TerminalDWriteCacheInformation cache{};
};

//! Lazy DirectWrite/Direct2D fallback renderer.  It accepts only model-owned
//! cell rectangles; no DirectWrite measurement is exposed as grid geometry.
class TerminalDWriteRenderer final {
public:
	static constexpr std::size_t kMaximumCacheEntries = 256;
	static constexpr std::size_t kMaximumCacheBytes = 1024U * 1024U;

	TerminalDWriteRenderer();
	~TerminalDWriteRenderer();
	TerminalDWriteRenderer(const TerminalDWriteRenderer&) = delete;
	TerminalDWriteRenderer& operator=(const TerminalDWriteRenderer&) = delete;

	//! Copies generation-bound font/DPI/locale inputs but does not initialize a
	//! factory or target.  Every change invalidates cache and returns Dormant.
	void Configure(const TerminalDWriteConfiguration& configuration);
	void Invalidate() noexcept;
	void Close() noexcept;

	[[nodiscard]] TerminalDWriteLifecycle Lifecycle() const noexcept;
	[[nodiscard]] const TerminalDWriteRendererCounters& Counters() const noexcept;
	[[nodiscard]] TerminalDWriteCacheInformation CacheInformation() const noexcept;
	[[nodiscard]] std::uint16_t CachedFirstGlyphIndex(std::wstring_view text,
		const TerminalRenderStyle& style) const noexcept;

	void SetFaultInjection(TerminalDWriteFaultInjection fault) noexcept;

	//! A frame makes at most one infrastructure/target creation attempt.  A
	//! Rendered outcome here means BeginDraw succeeded and this caller owns the
	//! matching FinalizeFrame or AbortFrame operation.
	[[nodiscard]] TerminalDWriteFrameOutcome BeginFrame(HDC target, const RECT& clip,
		TerminalDWriteFrame& frame) noexcept;
	[[nodiscard]] bool DrawCluster(const TerminalShapedClusterCommand& command,
		std::wstring_view text) noexcept;
	[[nodiscard]] TerminalDWriteFrameOutcome FinalizeFrame(TerminalDWriteFrame& frame) noexcept;
	void AbortFrame(TerminalDWriteFrame& frame) noexcept;

private:
	struct CachedGlyphRun {
		Microsoft::WRL::ComPtr<IDWriteFontFace> fontFace;
		std::size_t glyphOffset{};
		std::size_t glyphCount{};
		FLOAT fontEmSize{ 1.0f };
		UINT8 bidiLevel{};
	};

	struct CacheEntry {
		std::wstring text;
		TerminalRenderStyle style{};
		std::vector<std::uint16_t> glyphIndices;
		std::vector<FLOAT> advances;
		std::vector<DWRITE_GLYPH_OFFSET> offsets;
		std::vector<CachedGlyphRun> runs;
		std::uint64_t generation{};
		std::uint64_t hash{};
		std::uint64_t lastUse{};
		std::size_t byteSize{};
	};

	static void ResetCacheEntry(CacheEntry& entry) noexcept;
	[[nodiscard]] static std::size_t CacheEntryBytes(const CacheEntry& entry) noexcept;
	[[nodiscard]] static float BaselineFor(const CachedGlyphRun& run, const RECT& rect) noexcept;

	[[nodiscard]] bool EnsureInfrastructure() noexcept;
	[[nodiscard]] bool EnsureTarget(TerminalDWriteFrame& frame) noexcept;
	[[nodiscard]] bool CreateTarget() noexcept;
	[[nodiscard]] bool CreateBrush(COLORREF color) noexcept;
	[[nodiscard]] CacheEntry* FindCache(std::wstring_view text, const TerminalRenderStyle& style,
		std::uint64_t hash) noexcept;
	[[nodiscard]] bool ShapeCluster(std::wstring_view text, const TerminalRenderStyle& style,
		CacheEntry& entry) noexcept;
	[[nodiscard]] CacheEntry* FindOrShape(std::wstring_view text, const TerminalRenderStyle& style) noexcept;
	[[nodiscard]] bool DrawCachedCluster(const TerminalShapedClusterCommand& command,
		const CacheEntry& entry) noexcept;
	[[nodiscard]] TerminalDWriteFrameOutcome EndFrame(TerminalDWriteFrame& frame, bool aborted) noexcept;
	void ReleaseTarget() noexcept;
	void RecordTargetLoss() noexcept;
	void ClearCache() noexcept;
	void RebuildCacheIndex() noexcept;
	void UpdateCacheCounters() noexcept;
	[[nodiscard]] bool InsertCacheEntry(CacheEntry&& entry) noexcept;
	[[nodiscard]] std::uint64_t CacheHash(std::wstring_view text, const TerminalRenderStyle& style) const noexcept;

	Microsoft::WRL::ComPtr<ID2D1Factory> m_d2dFactory;
	Microsoft::WRL::ComPtr<IDWriteFactory2> m_dwriteFactory;
	Microsoft::WRL::ComPtr<IDWriteTextAnalyzer> m_textAnalyzer;
	Microsoft::WRL::ComPtr<IDWriteFontFallback> m_fontFallback;
	Microsoft::WRL::ComPtr<IDWriteFontCollection> m_fontCollection;
	Microsoft::WRL::ComPtr<ID2D1DCRenderTarget> m_d2dTarget;
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_brush;
	std::wstring m_primaryFamily;
	std::wstring m_locale;
	std::vector<CacheEntry> m_cache;
	std::unordered_multimap<std::uint64_t, std::size_t> m_cacheIndex;
	CacheEntry m_transientEntry;
	std::vector<std::uint16_t> m_clusterMap;
	std::vector<DWRITE_SHAPING_TEXT_PROPERTIES> m_textProperties;
	std::vector<std::uint16_t> m_glyphIndices;
	std::vector<DWRITE_SHAPING_GLYPH_PROPERTIES> m_glyphProperties;
	std::vector<FLOAT> m_glyphAdvances;
	std::vector<DWRITE_GLYPH_OFFSET> m_glyphOffsets;
	std::vector<FLOAT> m_drawAdvances;
	std::vector<DWRITE_GLYPH_OFFSET> m_drawOffsets;
	std::uint64_t m_generation{};
	std::uint64_t m_cacheClock{};
	std::uint64_t m_nextFrameIdentifier{};
	std::size_t m_cacheBytes{};
	int m_fontPixelHeight{ 1 };
	int m_fontWeight{ FW_NORMAL };
	unsigned int m_dpi{ 96 };
	COLORREF m_brushColor{ CLR_INVALID };
	TerminalDWriteLifecycle m_lifecycle{ TerminalDWriteLifecycle::Dormant };
	TerminalDWriteFaultInjection m_faultInjection{};
	TerminalDWriteRendererCounters m_counters{};
	bool m_configured{};
	bool m_drawing{};
};

} // namespace terminal

/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <Windows.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace terminal {

//! Versioned, test-only rendering baselines.  This API intentionally has no
//! dependency on the production terminal renderer so that the measurement
//! commitment remains stable while a candidate renderer changes.
enum class TerminalLegacyRendererBackend : std::uint8_t {
	LegacyGdi,
	GdiPlus,
	DirectWrite,
	DirectWriteDirect2D,
	CandidateNative,
};

enum class TerminalLegacyRendererCorpus : std::uint8_t {
	AsciiShell,
	TuiBoxBlockShade,
	MixedUnicode,
};

constexpr std::uint32_t kTerminalLegacyRendererSchemaVersion = 1;

struct TerminalLegacyRendererCell final {
	std::wstring text;
	std::uint8_t occupiedColumns{ 1 };
	bool continuation{};
};

struct TerminalLegacyRendererConditions final {
	TerminalLegacyRendererBackend backend{ TerminalLegacyRendererBackend::LegacyGdi };
	TerminalLegacyRendererCorpus corpus{ TerminalLegacyRendererCorpus::AsciiShell };
	std::uint32_t dpi{ 96 };
	std::uint32_t warmupFrames{ 2 };
	std::uint32_t frameCount{ 20 };
	std::uint32_t columns{ 120 };
	std::uint32_t rows{ 8 };
	COLORREF foreground{ RGB(0xDC, 0xDF, 0xE4) };
	COLORREF background{ RGB(0x28, 0x2C, 0x34) };
	std::wstring preferredFont{ L"Cascadia Mono" };
	std::wstring fallbackFont{ L"Consolas" };
};

struct TerminalLegacyRendererCounters final {
	std::uint64_t frames{};
	std::uint64_t extTextOutCalls{};
	std::uint64_t gdiBatchCount{};
	std::uint64_t drawStringCalls{};
	std::uint64_t textLayoutCreates{};
	std::uint64_t fallbackRuns{};
	std::uint64_t fallbackCacheHits{};
	std::uint64_t fallbackCacheMisses{};
	std::uint64_t mapCharactersCalls{};
	std::uint64_t analysisCalls{};
	std::uint64_t glyphsCalls{};
	std::uint64_t placementCalls{};
	std::uint64_t d2dTargetCreates{};
	std::uint64_t d2dTargetBinds{};
	std::uint64_t d2dDrawCalls{};
	std::uint64_t d2dEndDrawCalls{};
	std::uint64_t d2dTargetLosses{};
};

struct TerminalLegacyRendererResult final {
	std::uint32_t schemaVersion{ kTerminalLegacyRendererSchemaVersion };
	bool available{};
	std::string unavailableReason;
	TerminalLegacyRendererBackend backend{ TerminalLegacyRendererBackend::LegacyGdi };
	TerminalLegacyRendererCorpus corpus{ TerminalLegacyRendererCorpus::AsciiShell };
	std::uint32_t dpi{};
	std::uint32_t columns{};
	std::uint32_t rows{};
	std::uint32_t cellWidth{};
	std::uint32_t cellHeight{};
	std::uint32_t fontPixelHeight{};
	std::uint32_t warmupFrames{};
	std::uint32_t frameCount{};
	std::string fontFamily;
	std::uint64_t corpusCellCount{};
	std::uint64_t corpusUtf16CodeUnits{};
	std::uint64_t corpusOccupiedColumns{};
	double coldInitializationMs{};
	double measurementInitializationMs{};
	std::int64_t processGlobalPrivateBytesBefore{};
	std::int64_t processGlobalPrivateBytesAfter{};
	std::int64_t processGlobalPrivateBytesDelta{};
	std::int64_t processGlobalGdiObjectsBefore{};
	std::int64_t processGlobalGdiObjectsAfter{};
	std::int64_t processGlobalGdiObjectsDelta{};
	std::int64_t privateBytesBefore{};
	std::int64_t privateBytesAfter{};
	std::int64_t privateBytesDelta{};
	std::int64_t gdiObjectsBefore{};
	std::int64_t gdiObjectsAfter{};
	std::int64_t gdiObjectsDelta{};
	std::vector<double> frameDurationMs;
	TerminalLegacyRendererCounters counters;
};

[[nodiscard]] const char* TerminalLegacyRendererBackendName(TerminalLegacyRendererBackend backend) noexcept;
[[nodiscard]] const char* TerminalLegacyRendererCorpusName(TerminalLegacyRendererCorpus corpus) noexcept;
[[nodiscard]] TerminalLegacyRendererConditions DefaultTerminalLegacyRendererConditions() noexcept;

//! Builds one deterministic corpus.  Cluster text and occupied columns are
//! deliberately separate so UTF-16 length never becomes grid geometry.
[[nodiscard]] std::vector<TerminalLegacyRendererCell> BuildTerminalLegacyRendererCorpus(
	TerminalLegacyRendererCorpus corpus);

//! Executes one frozen backend entirely against a top-down 32-bpp DIB.
[[nodiscard]] TerminalLegacyRendererResult RunTerminalLegacyRenderer(
	const TerminalLegacyRendererConditions& conditions);

//! Stable machine-readable schema used by the PowerShell measurement driver
//! and by a future candidate adapter.
[[nodiscard]] std::string SerializeTerminalLegacyRendererResult(
	const TerminalLegacyRendererResult& result);

} // namespace terminal

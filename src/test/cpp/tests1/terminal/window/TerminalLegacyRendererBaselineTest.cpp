/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include "terminal/window/TerminalLegacyRendererBaseline.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>

namespace terminal {
namespace {

constexpr std::array<TerminalLegacyRendererBackend, 5> kBackends{
	TerminalLegacyRendererBackend::LegacyGdi,
	TerminalLegacyRendererBackend::GdiPlus,
	TerminalLegacyRendererBackend::DirectWrite,
	TerminalLegacyRendererBackend::DirectWriteDirect2D,
	TerminalLegacyRendererBackend::CandidateNative,
};
constexpr std::array<TerminalLegacyRendererCorpus, 3> kCorpora{
	TerminalLegacyRendererCorpus::AsciiShell,
	TerminalLegacyRendererCorpus::TuiBoxBlockShade,
	TerminalLegacyRendererCorpus::MixedUnicode,
};
constexpr std::array<std::uint32_t, 4> kDpis{ 96, 120, 144, 192 };

std::wstring Environment(std::wstring_view name)
{
	std::wstring key(name);
	const auto length = ::GetEnvironmentVariableW(key.c_str(), nullptr, 0);
	if( length == 0 ) return {};
	std::wstring value(length, L'\0');
	const auto written = ::GetEnvironmentVariableW(key.c_str(), value.data(), length);
	value.resize(written);
	return value;
}

std::uint32_t EnvironmentUnsigned(std::wstring_view name, std::uint32_t fallback)
{
	const auto value = Environment(name);
	if( value.empty() ) return fallback;
	wchar_t* end = nullptr;
	const auto parsed = std::wcstoul(value.c_str(), &end, 10);
	if( end == value.c_str() || *end != L'\0' ) return fallback;
	return parsed > std::numeric_limits<std::uint32_t>::max() ? fallback : static_cast<std::uint32_t>(parsed);
}

TerminalLegacyRendererBackend BackendFromEnvironment(TerminalLegacyRendererBackend fallback)
{
	const auto value = Environment(L"SAKURA_TERMINAL_BASELINE_BACKEND");
	if( value == L"legacy-gdi" ) return TerminalLegacyRendererBackend::LegacyGdi;
	if( value == L"gdi-plus" ) return TerminalLegacyRendererBackend::GdiPlus;
	if( value == L"directwrite" ) return TerminalLegacyRendererBackend::DirectWrite;
	if( value == L"directwrite-d2d" ) return TerminalLegacyRendererBackend::DirectWriteDirect2D;
	if( value == L"candidate-native" ) return TerminalLegacyRendererBackend::CandidateNative;
	return fallback;
}

TerminalLegacyRendererCorpus CorpusFromEnvironment(TerminalLegacyRendererCorpus fallback)
{
	const auto value = Environment(L"SAKURA_TERMINAL_BASELINE_CORPUS");
	if( value == L"ascii-shell" ) return TerminalLegacyRendererCorpus::AsciiShell;
	if( value == L"tui-box-block-shade" ) return TerminalLegacyRendererCorpus::TuiBoxBlockShade;
	if( value == L"mixed-unicode" ) return TerminalLegacyRendererCorpus::MixedUnicode;
	return fallback;
}

std::string NarrowPath(const std::wstring& value)
{
	if( value.empty() ) return {};
	const auto length = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.c_str(),
		static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
	if( length <= 0 ) return {};
	std::string result(static_cast<std::size_t>(length), '\0');
	::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.c_str(),
		static_cast<int>(value.size()), result.data(), length, nullptr, nullptr);
	return result;
}

TerminalLegacyRendererConditions ConditionsFromEnvironment()
{
	auto conditions = DefaultTerminalLegacyRendererConditions();
	conditions.backend = BackendFromEnvironment(conditions.backend);
	conditions.corpus = CorpusFromEnvironment(conditions.corpus);
	conditions.dpi = EnvironmentUnsigned(L"SAKURA_TERMINAL_BASELINE_DPI", conditions.dpi);
	conditions.warmupFrames = EnvironmentUnsigned(L"SAKURA_TERMINAL_BASELINE_WARMUP_FRAMES", conditions.warmupFrames);
	conditions.frameCount = EnvironmentUnsigned(L"SAKURA_TERMINAL_BASELINE_FRAMES", conditions.frameCount);
	conditions.columns = EnvironmentUnsigned(L"SAKURA_TERMINAL_BASELINE_COLUMNS", conditions.columns);
	conditions.rows = EnvironmentUnsigned(L"SAKURA_TERMINAL_BASELINE_ROWS", conditions.rows);
	return conditions;
}

TEST(TerminalLegacyRendererBaseline, CorpusKeepsUtf16LengthSeparateFromOccupiedColumns)
{
	const auto mixed = BuildTerminalLegacyRendererCorpus(TerminalLegacyRendererCorpus::MixedUnicode);
	ASSERT_FALSE(mixed.empty());
	bool sawTwoColumnCluster = false;
	bool sawComplexUtf16 = false;
	for( const auto& cell : mixed ) {
		if( cell.occupiedColumns == 2 ) sawTwoColumnCluster = true;
		if( cell.text.size() != cell.occupiedColumns ) sawComplexUtf16 = true;
	}
	EXPECT_TRUE(sawTwoColumnCluster);
	EXPECT_TRUE(sawComplexUtf16);

	const auto tui = BuildTerminalLegacyRendererCorpus(TerminalLegacyRendererCorpus::TuiBoxBlockShade);
	ASSERT_FALSE(tui.empty());
	EXPECT_EQ(L"\u2500", tui.front().text);
	EXPECT_EQ(L"\u259F", tui[0x9F].text);
}

TEST(TerminalLegacyRendererBaseline, FrozenBackendsRenderEveryCorpusAtEverySupportedDpi)
{
	for( const auto dpi : kDpis ) {
		for( const auto backend : kBackends ) {
			for( const auto corpus : kCorpora ) {
				auto conditions = DefaultTerminalLegacyRendererConditions();
				conditions.backend = backend;
				conditions.corpus = corpus;
				conditions.dpi = dpi;
				conditions.columns = 32;
				conditions.rows = 2;
				conditions.warmupFrames = 1;
				conditions.frameCount = 1;
				const auto result = RunTerminalLegacyRenderer(conditions);
				ASSERT_TRUE(result.available)
					<< "backend=" << TerminalLegacyRendererBackendName(backend)
					<< " corpus=" << TerminalLegacyRendererCorpusName(corpus)
					<< " dpi=" << dpi << " reason=" << result.unavailableReason;
				EXPECT_EQ(kTerminalLegacyRendererSchemaVersion, result.schemaVersion);
				EXPECT_EQ(1u, result.frameDurationMs.size());
				EXPECT_EQ(1u, result.counters.frames);
				// The runner destroys its cold and stabilization probes before taking
				// the measured-instance snapshots; any non-zero delta indicates a
				// renderer leak.
				EXPECT_EQ(0, result.gdiObjectsDelta)
					<< "backend=" << TerminalLegacyRendererBackendName(backend);
				switch( backend ) {
				case TerminalLegacyRendererBackend::LegacyGdi:
					EXPECT_GT(result.counters.extTextOutCalls, 0u);
					EXPECT_EQ(result.counters.extTextOutCalls, result.counters.gdiBatchCount);
					break;
				case TerminalLegacyRendererBackend::GdiPlus:
					EXPECT_GT(result.counters.drawStringCalls, 0u);
					break;
				case TerminalLegacyRendererBackend::DirectWrite:
				case TerminalLegacyRendererBackend::DirectWriteDirect2D:
					EXPECT_GT(result.counters.textLayoutCreates, 0u);
					EXPECT_GT(result.counters.d2dTargetCreates, 0u);
					EXPECT_GT(result.counters.d2dTargetBinds, 0u);
					EXPECT_GT(result.counters.d2dDrawCalls, 0u);
					EXPECT_GT(result.counters.d2dEndDrawCalls, 0u);
					EXPECT_EQ(0u, result.counters.d2dTargetLosses);
					break;
				case TerminalLegacyRendererBackend::CandidateNative:
					EXPECT_EQ(0u, result.counters.textLayoutCreates);
					if( corpus == TerminalLegacyRendererCorpus::MixedUnicode ) {
						EXPECT_GT(result.counters.d2dTargetCreates, 0u);
						EXPECT_GT(result.counters.d2dTargetBinds, 0u);
						EXPECT_GT(result.counters.d2dDrawCalls, 0u);
						EXPECT_GT(result.counters.d2dEndDrawCalls, 0u);
					} else {
						EXPECT_EQ(0u, result.counters.d2dTargetCreates);
						EXPECT_EQ(0u, result.counters.d2dTargetBinds);
						EXPECT_EQ(0u, result.counters.d2dDrawCalls);
						EXPECT_EQ(0u, result.counters.d2dEndDrawCalls);
					}
					EXPECT_EQ(0u, result.counters.d2dTargetLosses);
					break;
				}
			}
		}
	}
}

TEST(TerminalLegacyRendererBaseline, DirectWriteGroupsSimpleAsciiCellsIntoRowRuns)
{
	for( const auto backend : { TerminalLegacyRendererBackend::DirectWrite,
		TerminalLegacyRendererBackend::DirectWriteDirect2D } ) {
		auto conditions = DefaultTerminalLegacyRendererConditions();
		conditions.backend = backend;
		conditions.corpus = TerminalLegacyRendererCorpus::AsciiShell;
		conditions.columns = 32;
		conditions.rows = 2;
		conditions.warmupFrames = 0;
		conditions.frameCount = 1;
		const auto result = RunTerminalLegacyRenderer(conditions);
		ASSERT_TRUE(result.available) << result.unavailableReason;
		// One contiguous layout/draw run per row mirrors PaintDirectWrite's
		// width-1, one-UTF-16-unit grouping instead of one layout per cell.
		EXPECT_EQ(2u, result.counters.textLayoutCreates);
		EXPECT_EQ(2u, result.counters.d2dDrawCalls);
		if( backend == TerminalLegacyRendererBackend::DirectWrite ) {
			EXPECT_GT(result.counters.mapCharactersCalls, 0u);
			EXPECT_GT(result.counters.analysisCalls, 0u);
			EXPECT_GT(result.counters.glyphsCalls, 0u);
			EXPECT_GT(result.counters.placementCalls, 0u);
		} else {
			// The direct2D baseline intentionally mirrors the grouped
			// CreateTextLayout/DrawTextLayout path without synthetic shaping
			// telemetry.
			EXPECT_EQ(0u, result.counters.mapCharactersCalls);
			EXPECT_EQ(0u, result.counters.analysisCalls);
			EXPECT_EQ(0u, result.counters.glyphsCalls);
			EXPECT_EQ(0u, result.counters.placementCalls);
			EXPECT_EQ(0u, result.counters.fallbackCacheHits);
			EXPECT_EQ(0u, result.counters.fallbackCacheMisses);
		}
	}
}

TEST(TerminalLegacyRendererBaseline, CandidateAsciiBypassesDWriteInitialization)
{
	auto conditions = DefaultTerminalLegacyRendererConditions();
	conditions.backend = TerminalLegacyRendererBackend::CandidateNative;
	conditions.corpus = TerminalLegacyRendererCorpus::AsciiShell;
	conditions.columns = 32;
	conditions.rows = 2;
	conditions.warmupFrames = 1;
	conditions.frameCount = 2;
	const auto result = RunTerminalLegacyRenderer(conditions);
	ASSERT_TRUE(result.available) << result.unavailableReason;
	EXPECT_GT(result.counters.extTextOutCalls, 0u);
	EXPECT_GT(result.counters.gdiBatchCount, 0u);
	EXPECT_EQ(0u, result.counters.d2dTargetCreates);
	EXPECT_EQ(0u, result.counters.d2dTargetBinds);
	EXPECT_EQ(0u, result.counters.d2dDrawCalls);
	EXPECT_EQ(0u, result.counters.d2dEndDrawCalls);
	EXPECT_EQ(0u, result.counters.mapCharactersCalls);
	EXPECT_EQ(0u, result.counters.analysisCalls);
	EXPECT_EQ(0u, result.counters.glyphsCalls);
	EXPECT_EQ(0u, result.counters.placementCalls);
}

TEST(TerminalLegacyRendererBaseline, TuiCorpusRendersCompleteBoxBlockShadeRange)
{
	for( const auto backend : kBackends ) {
		auto conditions = DefaultTerminalLegacyRendererConditions();
		conditions.backend = backend;
		conditions.corpus = TerminalLegacyRendererCorpus::TuiBoxBlockShade;
		conditions.columns = 160;
		conditions.rows = 1;
		conditions.warmupFrames = 0;
		conditions.frameCount = 1;
		const auto result = RunTerminalLegacyRenderer(conditions);
		ASSERT_TRUE(result.available)
			<< "backend=" << TerminalLegacyRendererBackendName(backend)
			<< " reason=" << result.unavailableReason;
		EXPECT_GE(result.corpusCellCount, 160u);
		switch( backend ) {
		case TerminalLegacyRendererBackend::LegacyGdi:
			EXPECT_GT(result.counters.extTextOutCalls, 0u);
			break;
		case TerminalLegacyRendererBackend::GdiPlus:
			EXPECT_GT(result.counters.drawStringCalls, 0u);
			break;
		case TerminalLegacyRendererBackend::DirectWrite:
		case TerminalLegacyRendererBackend::DirectWriteDirect2D:
			EXPECT_GT(result.counters.textLayoutCreates, 0u);
			EXPECT_GT(result.counters.d2dDrawCalls, 0u);
			break;
		case TerminalLegacyRendererBackend::CandidateNative:
			EXPECT_EQ(0u, result.counters.d2dTargetCreates);
			EXPECT_EQ(0u, result.counters.d2dDrawCalls);
			break;
		}
	}
}

TEST(TerminalLegacyRendererBaseline, EmitsConfiguredBenchmarkJson)
{
	const auto outputPath = NarrowPath(Environment(L"SAKURA_TERMINAL_BASELINE_OUTPUT"));
	if( outputPath.empty() ) GTEST_SKIP() << "SAKURA_TERMINAL_BASELINE_OUTPUT is not set";
	const auto result = RunTerminalLegacyRenderer(ConditionsFromEnvironment());
	ASSERT_TRUE(result.available) << result.unavailableReason;
	std::ofstream output(outputPath, std::ios::binary | std::ios::trunc);
	ASSERT_TRUE(output.good()) << outputPath;
	output << SerializeTerminalLegacyRendererResult(result) << '\n';
	ASSERT_TRUE(output.good());
}

} // namespace
} // namespace terminal

/*! @file
	@brief Opt-in startup timeline trace.

	The trace intentionally contains only fixed event names and numeric values.
	It must never contain document, profile, command-line, title, or path data.
*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <cstdint>

class CStartupTrace final
{
public:
	enum class Role : std::uint8_t {
		Unknown,
		Editor,
		Control,
	};

	enum class Event : std::uint8_t {
		ProcessEntry,
		IsaDispatch,
		FactoryBegin,
		FactoryEnd,
		ControlSpawnBegin,
		ControlSpawnEnd,
		ControlWaitBegin,
		ControlWaitEnd,
		ControlWaitResult,
		ControlInitializeBegin,
		ControlSharedDataReady,
		ControlTrayCreated,
		ControlReadyEventBegin,
		ControlReadyEventEnd,
		EditorSpawnBegin,
		EditorSpawnEnd,
		EditorWaitBegin,
		EditorWaitEnd,
		EditorWaitResult,
		EditorReadyEventBegin,
		EditorReadyEventEnd,
		UipiCheckBegin,
		UipiCheckEnd,
		ReadBegin,
		ReadEnd,
		LayoutBegin,
		LayoutDecision,
		LayoutComplete,
		StartupDocumentArmed,
		StartupDocumentComplete,
		StartupDocumentAborted,
		StartupDrawCommitBegin,
		StartupDrawCommitEnd,
		StartupDrawLayoutBegin,
		StartupDrawLayoutEnd,
		StartupDrawScrollBegin,
		StartupDrawScrollEnd,
		StartupDrawShowBegin,
		StartupDrawShowEnd,
		StartupDrawRedrawBegin,
		StartupDrawRedrawEnd,
		FirstContentPaintBegin,
		FirstContentPaintEnd,
		FirstContentPaintPrepareBegin,
		FirstContentPaintPrepareEnd,
		FirstContentPaintLinesBegin,
		FirstContentPaintLinesEnd,
		FirstContentPaintFinishBegin,
		FirstContentPaintFinishEnd,
		FirstContentAdvanceWidthSummary,
		FirstContentDrawWidthSummary,
		FirstContentTextOutputSummary,
		FirstContentTextVolumeSummary,
		FirstContentTextBlockSummary,
		FirstContentTextBlockFontSummary,
		FirstContentTextBoundarySummary,
		FirstContentTextScanSummary,
		FirstContentNonBlockTextRangeSummary,
		FirstContentNonBlockTextRiskSummary,
		FirstContentNonBlockTextOtherSummary,
		StartupDrawMiniMapPaintSummary,
		StartupDrawMiniMapUpdateSummary,
		FirstContentPainted,
	};

	enum class LayoutReason : std::uint8_t {
		None,
		BelowMinimumLines,
		RangeBasedColor,
	};

	enum class NonBlockTextCategory : std::uint8_t {
		CjkSymbolsAndPunctuation,
		GeneralPunctuation,
		LatinExtended,
		CombiningOrVariation,
		SurrogatePair,
		OtherBmp,
	};

	// Call once from wWinMain before any startup work.  Tracing is enabled only
	// when SAKURA_STARTUP_TRACE_DIR names an existing directory.
	static void Initialize();
	static bool IsEnabled();
	static void SetRole(Role role);
	static void Mark(Event event, std::int64_t value1 = 0, std::int64_t value2 = 0);
	static void MarkLayoutDecision(std::int64_t lines, std::int64_t workers, LayoutReason reason);

	// The editor startup owner arms/completes this transaction around the initial
	// document load.  FirstContentPainted is emitted only after completion.
	static void ArmStartupDocument();
	static void CompleteStartupDocument();
	static void AbortStartupDocument();
	static bool IsStartupDocumentPending();
	static bool IsAwaitingFirstContentPaint();

	// Hot-path paint measurements are accumulated in memory and written only
	// after the first primary content paint. Durations use raw QPC ticks so the
	// trace record's frequency remains the single conversion source.
	static void BeginFirstContentPaintMetrics() noexcept;
	static void EndFirstContentPaintMetrics() noexcept;
	static bool IsCollectingFirstContentPaintMetrics() noexcept;
	static void AccumulateFirstContentAdvanceWidth(
		std::int64_t qpcTicks, std::int64_t utf16Units) noexcept;
	static void AccumulateFirstContentDrawWidth(
		std::int64_t qpcTicks, std::int64_t utf16Units) noexcept;
	static void AccumulateFirstContentTextOutput(
		std::int64_t qpcTicks, std::int64_t utf16Units) noexcept;
	static void AccumulateFirstContentTextBlock(
		std::int64_t utf16Units, bool alternateFont) noexcept;
	static void AccumulateFirstContentTextBoundary(
		bool renderType, bool lengthLimit, bool colorChange, bool tail) noexcept;
	static void AccumulateFirstContentTextScan(bool textFigure, bool colorChanged) noexcept;
	static void AccumulateFirstContentNonBlockText(NonBlockTextCategory category) noexcept;
	static void FlushFirstContentPaintMetrics();
	static void MarkFirstContentPainted();
};

/*! @file @brief Opt-in startup timeline trace. */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "debug/StartupTrace.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <strsafe.h>

namespace
{
constexpr wchar_t kTraceDirectoryEnvironmentVariable[] = L"SAKURA_STARTUP_TRACE_DIR";

struct TraceState {
	std::atomic<bool> enabled{false};
	std::atomic<CStartupTrace::Role> role{CStartupTrace::Role::Unknown};
	std::atomic<bool> startupDocumentArmed{false};
	std::atomic<bool> startupDocumentPending{false};
	std::atomic<bool> startupDocumentCompleted{false};
	std::atomic<bool> firstContentPainted{false};
	LARGE_INTEGER frequency{};
	HANDLE file{INVALID_HANDLE_VALUE};
	std::mutex writeMutex;

	~TraceState()
	{
		if (file != INVALID_HANDLE_VALUE) {
			::CloseHandle(file);
		}
	}
};

struct FirstContentPaintMetrics {
	bool collecting{};
	bool pending{};
	std::int64_t advanceWidthTicks{};
	std::int64_t advanceWidthCalls{};
	std::int64_t advanceUtf16Units{};
	std::int64_t drawWidthTicks{};
	std::int64_t drawWidthCalls{};
	std::int64_t drawUtf16Units{};
	std::int64_t textOutputTicks{};
	std::int64_t textOutputCalls{};
	std::int64_t textOutputUtf16Units{};
	std::int64_t textBlockCount{};
	std::int64_t textBlockUtf16Units{};
	std::int64_t alternateFontBlockCount{};
	std::int64_t maximumTextBlockUtf16Units{};
	std::int64_t renderTypeBoundaryCount{};
	std::int64_t lengthBoundaryCount{};
	std::int64_t colorBoundaryCount{};
	std::int64_t tailBoundaryCount{};
	std::int64_t figureLookupCount{};
	std::int64_t nonTextFigureCount{};
	std::int64_t colorChangeCount{};
	std::int64_t cjkSymbolsAndPunctuationCount{};
	std::int64_t generalPunctuationCount{};
	std::int64_t latinExtendedCount{};
	std::int64_t combiningOrVariationCount{};
	std::int64_t surrogatePairCount{};
	std::int64_t otherBmpCount{};
};

thread_local FirstContentPaintMetrics g_firstContentPaintMetrics;

constexpr std::size_t kStartupDocumentSubphaseCount = static_cast<std::size_t>(CStartupTrace::StartupDocumentSubphase::Count);
constexpr std::size_t kMakeOneLineCostCount = static_cast<std::size_t>(CStartupTrace::MakeOneLineCost::Count);

struct StartupDocumentMetrics {
	std::array<std::atomic<std::int64_t>, kStartupDocumentSubphaseCount> subphaseTicks{};
	std::array<std::atomic<std::int64_t>, kStartupDocumentSubphaseCount> subphaseOperations{};
	std::atomic<std::int64_t> readInputBytes{};
	std::atomic<std::int64_t> readActivePartitions{};
	std::atomic<std::int64_t> readLogicalLines{};
	std::atomic<std::int64_t> readResult{-1};
	std::atomic<std::int64_t> readWorkerTicks{};
	std::atomic<std::int64_t> readWorkerOperations{};
	std::atomic<std::int64_t> readWorkersStarted{};
	std::atomic<std::int64_t> readWorkersCollected{};
	std::atomic<std::int64_t> readCopyOperations{};
	std::atomic<std::int64_t> readMoveOperations{};
	std::atomic<std::int64_t> miniMapCacheHits{};
	std::atomic<std::int64_t> miniMapCacheMisses{};
	std::atomic<std::int64_t> miniMapBuildTicks{};
	std::atomic<std::int64_t> miniMapGeneratedRows{};
	std::atomic<std::int64_t> makeOneLineTicks{};
	std::atomic<std::int64_t> makeOneLineOperations{};
	std::atomic<std::int64_t> makeOneLineUtf16Units{};
	std::array<std::atomic<std::int64_t>, kMakeOneLineCostCount> makeOneLineCostTicks{};
	std::array<std::atomic<std::int64_t>, kMakeOneLineCostCount> makeOneLineCostOperations{};
	std::atomic<bool> emitted{false};
};

StartupDocumentMetrics g_startupDocumentMetrics;

TraceState& GetTraceState()
{
	static TraceState state;
	return state;
}

void WriteRecord(CStartupTrace::Event event, std::int64_t value1, std::int64_t value2, const char* detail);

const char* RoleName(CStartupTrace::Role role)
{
	switch (role) {
	case CStartupTrace::Role::Editor: return "editor";
	case CStartupTrace::Role::Control: return "control";
	default: return "unknown";
	}
}

const char* EventName(CStartupTrace::Event event)
{
	switch (event) {
	case CStartupTrace::Event::ProcessEntry: return "process_entry";
	case CStartupTrace::Event::IsaDispatch: return "isa_dispatch";
	case CStartupTrace::Event::FactoryBegin: return "factory_begin";
	case CStartupTrace::Event::FactoryEnd: return "factory_end";
	case CStartupTrace::Event::ControlSpawnBegin: return "control_spawn_begin";
	case CStartupTrace::Event::ControlSpawnEnd: return "control_spawn_end";
	case CStartupTrace::Event::ControlWaitBegin: return "control_wait_begin";
	case CStartupTrace::Event::ControlWaitEnd: return "control_wait_end";
	case CStartupTrace::Event::ControlWaitResult: return "control_wait_result";
	case CStartupTrace::Event::ControlInitializeBegin: return "control_initialize_begin";
	case CStartupTrace::Event::ControlSharedDataReady: return "control_shared_data_ready";
	case CStartupTrace::Event::ControlTrayCreated: return "control_tray_created";
	case CStartupTrace::Event::ControlReadyEventBegin: return "control_ready_event_begin";
	case CStartupTrace::Event::ControlReadyEventEnd: return "control_ready_event_end";
	case CStartupTrace::Event::EditorSpawnBegin: return "editor_spawn_begin";
	case CStartupTrace::Event::EditorSpawnEnd: return "editor_spawn_end";
	case CStartupTrace::Event::EditorWaitBegin: return "editor_wait_begin";
	case CStartupTrace::Event::EditorWaitEnd: return "editor_wait_end";
	case CStartupTrace::Event::EditorWaitResult: return "editor_wait_result";
	case CStartupTrace::Event::EditorReadyEventBegin: return "editor_ready_event_begin";
	case CStartupTrace::Event::EditorReadyEventEnd: return "editor_ready_event_end";
	case CStartupTrace::Event::UipiCheckBegin: return "uipi_check_begin";
	case CStartupTrace::Event::UipiCheckEnd: return "uipi_check_end";
	case CStartupTrace::Event::ReadBegin: return "read_begin";
	case CStartupTrace::Event::ReadEnd: return "read_end";
	case CStartupTrace::Event::LayoutBegin: return "layout_begin";
	case CStartupTrace::Event::LayoutDecision: return "layout_decision";
	case CStartupTrace::Event::StartupLayoutInputSummary: return "startup_layout_input_summary";
	case CStartupTrace::Event::LayoutComplete: return "layout_complete";
	case CStartupTrace::Event::StartupDocumentArmed: return "startup_document_armed";
	case CStartupTrace::Event::StartupDocumentComplete: return "startup_document_complete";
	case CStartupTrace::Event::StartupDocumentAborted: return "startup_document_aborted";
	case CStartupTrace::Event::StartupDrawCommitBegin: return "startup_draw_commit_begin";
	case CStartupTrace::Event::StartupDrawCommitEnd: return "startup_draw_commit_end";
	case CStartupTrace::Event::StartupDrawLayoutBegin: return "startup_draw_layout_begin";
	case CStartupTrace::Event::StartupDrawLayoutEnd: return "startup_draw_layout_end";
	case CStartupTrace::Event::StartupDrawScrollBegin: return "startup_draw_scroll_begin";
	case CStartupTrace::Event::StartupDrawScrollEnd: return "startup_draw_scroll_end";
	case CStartupTrace::Event::StartupDrawShowBegin: return "startup_draw_show_begin";
	case CStartupTrace::Event::StartupDrawShowEnd: return "startup_draw_show_end";
	case CStartupTrace::Event::StartupDrawRedrawBegin: return "startup_draw_redraw_begin";
	case CStartupTrace::Event::StartupDrawRedrawEnd: return "startup_draw_redraw_end";
	case CStartupTrace::Event::FirstContentPaintBegin: return "first_content_paint_begin";
	case CStartupTrace::Event::FirstContentPaintEnd: return "first_content_paint_end";
	case CStartupTrace::Event::FirstContentPaintPrepareBegin: return "first_content_paint_prepare_begin";
	case CStartupTrace::Event::FirstContentPaintPrepareEnd: return "first_content_paint_prepare_end";
	case CStartupTrace::Event::FirstContentPaintLinesBegin: return "first_content_paint_lines_begin";
	case CStartupTrace::Event::FirstContentPaintLinesEnd: return "first_content_paint_lines_end";
	case CStartupTrace::Event::FirstContentPaintFinishBegin: return "first_content_paint_finish_begin";
	case CStartupTrace::Event::FirstContentPaintFinishEnd: return "first_content_paint_finish_end";
	case CStartupTrace::Event::FirstContentAdvanceWidthSummary: return "first_content_advance_width_summary";
	case CStartupTrace::Event::FirstContentDrawWidthSummary: return "first_content_draw_width_summary";
	case CStartupTrace::Event::FirstContentTextOutputSummary: return "first_content_text_output_summary";
	case CStartupTrace::Event::FirstContentTextVolumeSummary: return "first_content_text_volume_summary";
	case CStartupTrace::Event::FirstContentTextBlockSummary: return "first_content_text_block_summary";
	case CStartupTrace::Event::FirstContentTextBlockFontSummary: return "first_content_text_block_font_summary";
	case CStartupTrace::Event::FirstContentTextBoundarySummary: return "first_content_text_boundary_summary";
	case CStartupTrace::Event::FirstContentTextScanSummary: return "first_content_text_scan_summary";
	case CStartupTrace::Event::FirstContentNonBlockTextRangeSummary: return "first_content_nonblock_text_range_summary";
	case CStartupTrace::Event::FirstContentNonBlockTextRiskSummary: return "first_content_nonblock_text_risk_summary";
	case CStartupTrace::Event::FirstContentNonBlockTextOtherSummary: return "first_content_nonblock_text_other_summary";
	case CStartupTrace::Event::StartupDrawMiniMapPaintSummary: return "startup_draw_minimap_paint_summary";
	case CStartupTrace::Event::StartupDrawMiniMapUpdateSummary: return "startup_draw_minimap_update_summary";
	case CStartupTrace::Event::StartupDocumentSubphaseSummary: return "startup_document_subphase_summary";
	case CStartupTrace::Event::StartupReadDecisionSummary: return "startup_read_decision_summary";
	case CStartupTrace::Event::StartupReadResultSummary: return "startup_read_result_summary";
	case CStartupTrace::Event::StartupReadWorkerSummary: return "startup_read_worker_summary";
	case CStartupTrace::Event::StartupReadWorkerLifecycleSummary: return "startup_read_worker_lifecycle_summary";
	case CStartupTrace::Event::StartupReadTransferSummary: return "startup_read_transfer_summary";
	case CStartupTrace::Event::StartupMiniMapCacheSummary: return "startup_minimap_cache_summary";
	case CStartupTrace::Event::StartupMiniMapBuildSummary: return "startup_minimap_build_summary";
	case CStartupTrace::Event::StartupMakeOneLineSummary: return "startup_make_one_line_summary";
	case CStartupTrace::Event::StartupMakeOneLineWorkSummary: return "startup_make_one_line_work_summary";
	case CStartupTrace::Event::StartupMakeOneLineCostSummary: return "startup_make_one_line_cost_summary";
	case CStartupTrace::Event::FirstContentPainted: return "first_content_painted";
	default: return "unknown";
	}
}

const char* LayoutReasonName(CStartupTrace::LayoutReason reason)
{
	switch (reason) {
	case CStartupTrace::LayoutReason::BelowMinimumLines: return "below_minimum_lines";
	case CStartupTrace::LayoutReason::RangeBasedColor: return "range_based_color";
	default: return "none";
	}
}

const char* StartupDocumentSubphaseName(CStartupTrace::StartupDocumentSubphase subphase)
{
	switch (subphase) {
	case CStartupTrace::StartupDocumentSubphase::PreReadSettings: return "pre_read_settings";
	case CStartupTrace::StartupDocumentSubphase::Read: return "read";
	case CStartupTrace::StartupDocumentSubphase::Decode: return "decode";
	case CStartupTrace::StartupDocumentSubphase::LineBuild: return "line_build";
	case CStartupTrace::StartupDocumentSubphase::Layout: return "layout";
	case CStartupTrace::StartupDocumentSubphase::PostLoadFinalize: return "post_load_finalize";
	case CStartupTrace::StartupDocumentSubphase::WorkbenchUi: return "workbench_ui";
	case CStartupTrace::StartupDocumentSubphase::DrawCommit: return "draw_commit";
	default: return "unknown";
	}
}

const char* MakeOneLineCostName(CStartupTrace::MakeOneLineCost cost)
{
	switch (cost) {
	case CStartupTrace::MakeOneLineCost::KinsokuAndWord: return "kinsoku_and_word_inclusive";
	case CStartupTrace::MakeOneLineCost::ColorBoundary: return "color_boundary";
	case CStartupTrace::MakeOneLineCost::CharacterWidth: return "character_width";
	case CStartupTrace::MakeOneLineCost::LayoutAllocation: return "layout_allocation";
	default: return "unknown";
	}
}

void ResetStartupDocumentMetrics() noexcept
{
	for (auto& value : g_startupDocumentMetrics.subphaseTicks) {
		value.store(0, std::memory_order_relaxed);
	}
	for (auto& value : g_startupDocumentMetrics.subphaseOperations) {
		value.store(0, std::memory_order_relaxed);
	}
	g_startupDocumentMetrics.readInputBytes.store(0, std::memory_order_relaxed);
	g_startupDocumentMetrics.readActivePartitions.store(0, std::memory_order_relaxed);
	g_startupDocumentMetrics.readLogicalLines.store(0, std::memory_order_relaxed);
	g_startupDocumentMetrics.readResult.store(-1, std::memory_order_relaxed);
	g_startupDocumentMetrics.readWorkerTicks.store(0, std::memory_order_relaxed);
	g_startupDocumentMetrics.readWorkerOperations.store(0, std::memory_order_relaxed);
	g_startupDocumentMetrics.readWorkersStarted.store(0, std::memory_order_relaxed);
	g_startupDocumentMetrics.readWorkersCollected.store(0, std::memory_order_relaxed);
	g_startupDocumentMetrics.readCopyOperations.store(0, std::memory_order_relaxed);
	g_startupDocumentMetrics.readMoveOperations.store(0, std::memory_order_relaxed);
	g_startupDocumentMetrics.miniMapCacheHits.store(0, std::memory_order_relaxed);
	g_startupDocumentMetrics.miniMapCacheMisses.store(0, std::memory_order_relaxed);
	g_startupDocumentMetrics.miniMapBuildTicks.store(0, std::memory_order_relaxed);
	g_startupDocumentMetrics.miniMapGeneratedRows.store(0, std::memory_order_relaxed);
	g_startupDocumentMetrics.makeOneLineTicks.store(0, std::memory_order_relaxed);
	g_startupDocumentMetrics.makeOneLineOperations.store(0, std::memory_order_relaxed);
	g_startupDocumentMetrics.makeOneLineUtf16Units.store(0, std::memory_order_relaxed);
	for (auto& value : g_startupDocumentMetrics.makeOneLineCostTicks) {
		value.store(0, std::memory_order_relaxed);
	}
	for (auto& value : g_startupDocumentMetrics.makeOneLineCostOperations) {
		value.store(0, std::memory_order_relaxed);
	}
	g_startupDocumentMetrics.emitted.store(false, std::memory_order_relaxed);
}

void FlushStartupDocumentMetricRecords()
{
	if (g_startupDocumentMetrics.emitted.exchange(true, std::memory_order_relaxed)) {
		return;
	}
	for (std::size_t i = 0; i < kStartupDocumentSubphaseCount; ++i) {
		const auto subphase = static_cast<CStartupTrace::StartupDocumentSubphase>(i);
		WriteRecord(CStartupTrace::Event::StartupDocumentSubphaseSummary,
			g_startupDocumentMetrics.subphaseTicks[i].load(std::memory_order_relaxed),
			g_startupDocumentMetrics.subphaseOperations[i].load(std::memory_order_relaxed),
			StartupDocumentSubphaseName(subphase));
	}
	WriteRecord(CStartupTrace::Event::StartupReadDecisionSummary,
		g_startupDocumentMetrics.readInputBytes.load(std::memory_order_relaxed),
		g_startupDocumentMetrics.readActivePartitions.load(std::memory_order_relaxed), "");
	WriteRecord(CStartupTrace::Event::StartupReadResultSummary,
		g_startupDocumentMetrics.readLogicalLines.load(std::memory_order_relaxed),
		g_startupDocumentMetrics.readResult.load(std::memory_order_relaxed), "");
	WriteRecord(CStartupTrace::Event::StartupReadWorkerSummary,
		g_startupDocumentMetrics.readWorkerTicks.load(std::memory_order_relaxed),
		g_startupDocumentMetrics.readWorkerOperations.load(std::memory_order_relaxed), "");
	WriteRecord(CStartupTrace::Event::StartupReadWorkerLifecycleSummary,
		g_startupDocumentMetrics.readWorkersStarted.load(std::memory_order_relaxed),
		g_startupDocumentMetrics.readWorkersCollected.load(std::memory_order_relaxed), "");
	WriteRecord(CStartupTrace::Event::StartupReadTransferSummary,
		g_startupDocumentMetrics.readCopyOperations.load(std::memory_order_relaxed),
		g_startupDocumentMetrics.readMoveOperations.load(std::memory_order_relaxed), "");
	WriteRecord(CStartupTrace::Event::StartupMiniMapCacheSummary,
		g_startupDocumentMetrics.miniMapCacheHits.load(std::memory_order_relaxed),
		g_startupDocumentMetrics.miniMapCacheMisses.load(std::memory_order_relaxed), "");
	WriteRecord(CStartupTrace::Event::StartupMiniMapBuildSummary,
		g_startupDocumentMetrics.miniMapBuildTicks.load(std::memory_order_relaxed),
		g_startupDocumentMetrics.miniMapGeneratedRows.load(std::memory_order_relaxed), "");
	WriteRecord(CStartupTrace::Event::StartupMakeOneLineSummary,
		g_startupDocumentMetrics.makeOneLineTicks.load(std::memory_order_relaxed),
		g_startupDocumentMetrics.makeOneLineOperations.load(std::memory_order_relaxed), "");
	WriteRecord(CStartupTrace::Event::StartupMakeOneLineWorkSummary,
		g_startupDocumentMetrics.makeOneLineOperations.load(std::memory_order_relaxed),
		g_startupDocumentMetrics.makeOneLineUtf16Units.load(std::memory_order_relaxed), "");
	for (std::size_t i = 0; i < kMakeOneLineCostCount; ++i) {
		const auto cost = static_cast<CStartupTrace::MakeOneLineCost>(i);
		WriteRecord(CStartupTrace::Event::StartupMakeOneLineCostSummary,
			g_startupDocumentMetrics.makeOneLineCostTicks[i].load(std::memory_order_relaxed),
			g_startupDocumentMetrics.makeOneLineCostOperations[i].load(std::memory_order_relaxed),
			MakeOneLineCostName(cost));
	}
}

void WriteRecord(CStartupTrace::Event event, std::int64_t value1, std::int64_t value2, const char* detail)
{
	auto& state = GetTraceState();
	if (!state.enabled.load(std::memory_order_relaxed)) {
		return;
	}

	LARGE_INTEGER counter{};
	::QueryPerformanceCounter(&counter);
	std::array<char, 512> buffer{};
	const int length = sprintf_s(
		buffer.data(), buffer.size(),
		"{\"schemaVersion\":1,\"qpc\":%lld,\"frequency\":%lld,\"pid\":%lu,\"tid\":%lu,\"role\":\"%s\",\"event\":\"%s\",\"value1\":%lld,\"value2\":%lld,\"detail\":\"%s\"}\n",
		static_cast<long long>(counter.QuadPart),
		static_cast<long long>(state.frequency.QuadPart),
		static_cast<unsigned long>(::GetCurrentProcessId()),
		static_cast<unsigned long>(::GetCurrentThreadId()),
		RoleName(state.role.load(std::memory_order_relaxed)),
		EventName(event),
		static_cast<long long>(value1),
		static_cast<long long>(value2),
		detail);
	if (length <= 0) {
		return;
	}

	std::lock_guard lock(state.writeMutex);
	DWORD written{};
	(void)::WriteFile(state.file, buffer.data(), static_cast<DWORD>(length), &written, nullptr);
}
}

void CStartupTrace::Initialize()
{
	auto& state = GetTraceState();
	if (state.enabled.load(std::memory_order_relaxed)) {
		return;
	}

	std::array<wchar_t, 32768> directory{};
	const DWORD length = ::GetEnvironmentVariableW(kTraceDirectoryEnvironmentVariable, directory.data(), static_cast<DWORD>(directory.size()));
	if (length == 0 || length >= static_cast<DWORD>(directory.size())) {
		return;
	}
	const DWORD attributes = ::GetFileAttributesW(directory.data());
	if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
		return;
	}

	std::array<wchar_t, 32768> fileName{};
	const wchar_t* separator = directory[length - 1] == L'\\' || directory[length - 1] == L'/' ? L"" : L"\\";
	if (FAILED(::StringCchPrintfW(fileName.data(), fileName.size(), L"%s%sstartup-trace-%lu.jsonl", directory.data(), separator, static_cast<unsigned long>(::GetCurrentProcessId())))) {
		return;
	}

	HANDLE file = ::CreateFileW(fileName.data(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE) {
		return;
	}
	LARGE_INTEGER frequency{};
	if (!::QueryPerformanceFrequency(&frequency)) {
		::CloseHandle(file);
		return;
	}
	state.file = file;
	state.frequency = frequency;
	state.enabled.store(true, std::memory_order_release);
}

bool CStartupTrace::IsEnabled()
{
	return GetTraceState().enabled.load(std::memory_order_relaxed);
}

void CStartupTrace::SetRole(Role role)
{
	auto& state = GetTraceState();
	if (!state.enabled.load(std::memory_order_relaxed)) {
		return;
	}
	state.role.store(role, std::memory_order_relaxed);
}

void CStartupTrace::Mark(Event event, std::int64_t value1, std::int64_t value2)
{
	WriteRecord(event, value1, value2, "");
}

void CStartupTrace::MarkLayoutDecision(std::int64_t lines, std::int64_t workers, LayoutReason reason)
{
	WriteRecord(Event::LayoutDecision, lines, workers, LayoutReasonName(reason));
}

void CStartupTrace::ArmStartupDocument()
{
	auto& state = GetTraceState();
	if (!state.enabled.load(std::memory_order_relaxed)) {
		return;
	}
	state.startupDocumentArmed.store(true, std::memory_order_relaxed);
	state.startupDocumentPending.store(true, std::memory_order_relaxed);
	state.startupDocumentCompleted.store(false, std::memory_order_relaxed);
	state.firstContentPainted.store(false, std::memory_order_relaxed);
	ResetStartupDocumentMetrics();
	Mark(Event::StartupDocumentArmed);
}

void CStartupTrace::CompleteStartupDocument()
{
	auto& state = GetTraceState();
	if (!state.enabled.load(std::memory_order_relaxed)
		|| !state.startupDocumentArmed.load(std::memory_order_relaxed)
		|| !state.startupDocumentPending.exchange(false, std::memory_order_relaxed)) {
		return;
	}
	state.startupDocumentCompleted.store(true, std::memory_order_relaxed);
	Mark(Event::StartupDocumentComplete);
}

void CStartupTrace::AbortStartupDocument()
{
	auto& state = GetTraceState();
	if (!state.enabled.load(std::memory_order_relaxed)
		|| !state.startupDocumentArmed.load(std::memory_order_relaxed)
		|| !state.startupDocumentPending.exchange(false, std::memory_order_relaxed)) {
		return;
	}
	state.startupDocumentCompleted.store(false, std::memory_order_relaxed);
	FlushStartupDocumentMetricRecords();
	Mark(Event::StartupDocumentAborted);
}

bool CStartupTrace::IsStartupDocumentPending()
{
	auto& state = GetTraceState();
	return state.enabled.load(std::memory_order_relaxed) && state.startupDocumentPending.load(std::memory_order_relaxed);
}

bool CStartupTrace::IsAwaitingFirstContentPaint()
{
	auto& state = GetTraceState();
	return state.enabled.load(std::memory_order_relaxed)
		&& state.startupDocumentArmed.load(std::memory_order_relaxed)
		&& state.startupDocumentCompleted.load(std::memory_order_relaxed)
		&& !state.firstContentPainted.load(std::memory_order_relaxed);
}

bool CStartupTrace::IsCollectingStartupDocumentMetrics() noexcept
{
	auto& state = GetTraceState();
	return state.enabled.load(std::memory_order_relaxed)
		&& state.startupDocumentArmed.load(std::memory_order_relaxed)
		&& !g_startupDocumentMetrics.emitted.load(std::memory_order_relaxed);
}

void CStartupTrace::AccumulateStartupDocumentSubphase(
	StartupDocumentSubphase subphase, std::int64_t qpcTicks, std::int64_t operations) noexcept
{
	if (!IsCollectingStartupDocumentMetrics() || qpcTicks < 0 || operations < 0) {
		return;
	}
	const auto index = static_cast<std::size_t>(subphase);
	if (index >= kStartupDocumentSubphaseCount) {
		return;
	}
	g_startupDocumentMetrics.subphaseTicks[index].fetch_add(qpcTicks, std::memory_order_relaxed);
	g_startupDocumentMetrics.subphaseOperations[index].fetch_add(operations, std::memory_order_relaxed);
}

void CStartupTrace::SetStartupReadDecision(
	std::int64_t inputBytes, std::int64_t activePartitions, std::int64_t launchedWorkers) noexcept
{
	if (!IsCollectingStartupDocumentMetrics()
		|| inputBytes < 0 || activePartitions < 0 || launchedWorkers < 0) {
		return;
	}
	g_startupDocumentMetrics.readInputBytes.store(inputBytes, std::memory_order_relaxed);
	g_startupDocumentMetrics.readActivePartitions.store(activePartitions, std::memory_order_relaxed);
	g_startupDocumentMetrics.readWorkersStarted.store(launchedWorkers, std::memory_order_relaxed);
}

void CStartupTrace::SetStartupReadResult(std::int64_t logicalLines, std::int64_t result) noexcept
{
	if (!IsCollectingStartupDocumentMetrics() || logicalLines < 0) {
		return;
	}
	g_startupDocumentMetrics.readLogicalLines.store(logicalLines, std::memory_order_relaxed);
	g_startupDocumentMetrics.readResult.store(result, std::memory_order_relaxed);
}

void CStartupTrace::AccumulateStartupReadWorker(std::int64_t qpcTicks) noexcept
{
	if (!IsCollectingStartupDocumentMetrics() || qpcTicks < 0) {
		return;
	}
	g_startupDocumentMetrics.readWorkerTicks.fetch_add(qpcTicks, std::memory_order_relaxed);
	g_startupDocumentMetrics.readWorkerOperations.fetch_add(1, std::memory_order_relaxed);
}

void CStartupTrace::SetStartupReadWorkerLifecycle(
	std::int64_t startedWorkers, std::int64_t collectedWorkers) noexcept
{
	if (!IsCollectingStartupDocumentMetrics() || startedWorkers < 0 || collectedWorkers < 0) {
		return;
	}
	g_startupDocumentMetrics.readWorkersStarted.store(startedWorkers, std::memory_order_relaxed);
	g_startupDocumentMetrics.readWorkersCollected.store(collectedWorkers, std::memory_order_relaxed);
}

void CStartupTrace::AccumulateStartupReadTransfer(std::int64_t copyOperations, std::int64_t moveOperations) noexcept
{
	if (!IsCollectingStartupDocumentMetrics() || copyOperations < 0 || moveOperations < 0) {
		return;
	}
	g_startupDocumentMetrics.readCopyOperations.fetch_add(copyOperations, std::memory_order_relaxed);
	g_startupDocumentMetrics.readMoveOperations.fetch_add(moveOperations, std::memory_order_relaxed);
}

void CStartupTrace::AccumulateStartupMiniMapCacheLookup(
	bool hit, std::int64_t buildQpcTicks, std::int64_t generatedRows) noexcept
{
	if (!IsCollectingStartupDocumentMetrics() || buildQpcTicks < 0 || generatedRows < 0) {
		return;
	}
	(hit ? g_startupDocumentMetrics.miniMapCacheHits : g_startupDocumentMetrics.miniMapCacheMisses)
		.fetch_add(1, std::memory_order_relaxed);
	g_startupDocumentMetrics.miniMapBuildTicks.fetch_add(buildQpcTicks, std::memory_order_relaxed);
	g_startupDocumentMetrics.miniMapGeneratedRows.fetch_add(generatedRows, std::memory_order_relaxed);
}

void CStartupTrace::AccumulateStartupMakeOneLine(std::int64_t qpcTicks, std::int64_t utf16Units) noexcept
{
	if (!IsCollectingStartupDocumentMetrics() || qpcTicks < 0 || utf16Units < 0) {
		return;
	}
	g_startupDocumentMetrics.makeOneLineTicks.fetch_add(qpcTicks, std::memory_order_relaxed);
	g_startupDocumentMetrics.makeOneLineOperations.fetch_add(1, std::memory_order_relaxed);
	g_startupDocumentMetrics.makeOneLineUtf16Units.fetch_add(utf16Units, std::memory_order_relaxed);
}

void CStartupTrace::AccumulateStartupMakeOneLineCost(
	MakeOneLineCost cost, std::int64_t qpcTicks, std::int64_t operations) noexcept
{
	if (!IsCollectingStartupDocumentMetrics() || qpcTicks < 0 || operations < 0) {
		return;
	}
	const auto index = static_cast<std::size_t>(cost);
	if (index >= kMakeOneLineCostCount) {
		return;
	}
	g_startupDocumentMetrics.makeOneLineCostTicks[index].fetch_add(qpcTicks, std::memory_order_relaxed);
	g_startupDocumentMetrics.makeOneLineCostOperations[index].fetch_add(operations, std::memory_order_relaxed);
}

void CStartupTrace::FlushStartupDocumentMetrics()
{
	FlushStartupDocumentMetricRecords();
}

void CStartupTrace::BeginFirstContentPaintMetrics() noexcept
{
	auto& metrics = g_firstContentPaintMetrics;
	metrics = {};
	metrics.collecting = IsAwaitingFirstContentPaint();
}

void CStartupTrace::EndFirstContentPaintMetrics() noexcept
{
	auto& metrics = g_firstContentPaintMetrics;
	if (!metrics.collecting) {
		return;
	}
	metrics.collecting = false;
	metrics.pending = true;
}

bool CStartupTrace::IsCollectingFirstContentPaintMetrics() noexcept
{
	return g_firstContentPaintMetrics.collecting;
}

void CStartupTrace::AccumulateFirstContentAdvanceWidth(
	std::int64_t qpcTicks, std::int64_t utf16Units) noexcept
{
	auto& metrics = g_firstContentPaintMetrics;
	if (!metrics.collecting || qpcTicks < 0 || utf16Units < 0) {
		return;
	}
	metrics.advanceWidthTicks += qpcTicks;
	++metrics.advanceWidthCalls;
	metrics.advanceUtf16Units += utf16Units;
}

void CStartupTrace::AccumulateFirstContentDrawWidth(
	std::int64_t qpcTicks, std::int64_t utf16Units) noexcept
{
	auto& metrics = g_firstContentPaintMetrics;
	if (!metrics.collecting || qpcTicks < 0 || utf16Units < 0) {
		return;
	}
	metrics.drawWidthTicks += qpcTicks;
	++metrics.drawWidthCalls;
	metrics.drawUtf16Units += utf16Units;
}

void CStartupTrace::AccumulateFirstContentTextOutput(
	std::int64_t qpcTicks, std::int64_t utf16Units) noexcept
{
	auto& metrics = g_firstContentPaintMetrics;
	if (!metrics.collecting || qpcTicks < 0 || utf16Units < 0) {
		return;
	}
	metrics.textOutputTicks += qpcTicks;
	++metrics.textOutputCalls;
	metrics.textOutputUtf16Units += utf16Units;
}

void CStartupTrace::AccumulateFirstContentTextBlock(
	std::int64_t utf16Units, bool alternateFont) noexcept
{
	auto& metrics = g_firstContentPaintMetrics;
	if (!metrics.collecting || utf16Units < 0) {
		return;
	}
	++metrics.textBlockCount;
	metrics.textBlockUtf16Units += utf16Units;
	metrics.maximumTextBlockUtf16Units = (std::max)(metrics.maximumTextBlockUtf16Units, utf16Units);
	if (alternateFont) {
		++metrics.alternateFontBlockCount;
	}
}

void CStartupTrace::AccumulateFirstContentTextBoundary(
	bool renderType, bool lengthLimit, bool colorChange, bool tail) noexcept
{
	auto& metrics = g_firstContentPaintMetrics;
	if (!metrics.collecting) {
		return;
	}
	metrics.renderTypeBoundaryCount += renderType ? 1 : 0;
	metrics.lengthBoundaryCount += lengthLimit ? 1 : 0;
	metrics.colorBoundaryCount += colorChange ? 1 : 0;
	metrics.tailBoundaryCount += tail ? 1 : 0;
}

void CStartupTrace::AccumulateFirstContentTextScan(bool textFigure, bool colorChanged) noexcept
{
	auto& metrics = g_firstContentPaintMetrics;
	if (!metrics.collecting) {
		return;
	}
	++metrics.figureLookupCount;
	metrics.nonTextFigureCount += textFigure ? 0 : 1;
	metrics.colorChangeCount += colorChanged ? 1 : 0;
}

void CStartupTrace::AccumulateFirstContentNonBlockText(NonBlockTextCategory category) noexcept
{
	auto& metrics = g_firstContentPaintMetrics;
	if (!metrics.collecting) {
		return;
	}
	switch (category) {
	case NonBlockTextCategory::CjkSymbolsAndPunctuation:
		++metrics.cjkSymbolsAndPunctuationCount;
		break;
	case NonBlockTextCategory::GeneralPunctuation:
		++metrics.generalPunctuationCount;
		break;
	case NonBlockTextCategory::LatinExtended:
		++metrics.latinExtendedCount;
		break;
	case NonBlockTextCategory::CombiningOrVariation:
		++metrics.combiningOrVariationCount;
		break;
	case NonBlockTextCategory::SurrogatePair:
		++metrics.surrogatePairCount;
		break;
	case NonBlockTextCategory::OtherBmp:
		++metrics.otherBmpCount;
		break;
	}
}

void CStartupTrace::FlushFirstContentPaintMetrics()
{
	auto& metrics = g_firstContentPaintMetrics;
	if (!metrics.pending) {
		return;
	}
	metrics.pending = false;
	Mark(Event::FirstContentAdvanceWidthSummary,
		metrics.advanceWidthTicks, metrics.advanceWidthCalls);
	Mark(Event::FirstContentDrawWidthSummary,
		metrics.drawWidthTicks, metrics.drawWidthCalls);
	Mark(Event::FirstContentTextOutputSummary,
		metrics.textOutputTicks, metrics.textOutputCalls);
	Mark(Event::FirstContentTextVolumeSummary,
		metrics.advanceUtf16Units,
		metrics.textOutputUtf16Units > 0 ? metrics.textOutputUtf16Units : metrics.drawUtf16Units);
	Mark(Event::FirstContentTextBlockSummary,
		metrics.textBlockCount, metrics.textBlockUtf16Units);
	Mark(Event::FirstContentTextBlockFontSummary,
		metrics.alternateFontBlockCount, metrics.maximumTextBlockUtf16Units);
	Mark(Event::FirstContentTextBoundarySummary,
		(metrics.renderTypeBoundaryCount << 32) | (metrics.lengthBoundaryCount & 0xffffffffLL),
		(metrics.colorBoundaryCount << 32) | (metrics.tailBoundaryCount & 0xffffffffLL));
	Mark(Event::FirstContentTextScanSummary,
		metrics.figureLookupCount,
		(metrics.nonTextFigureCount << 32) | (metrics.colorChangeCount & 0xffffffffLL));
	Mark(Event::FirstContentNonBlockTextRangeSummary,
		metrics.cjkSymbolsAndPunctuationCount, metrics.generalPunctuationCount);
	Mark(Event::FirstContentNonBlockTextRiskSummary,
		metrics.combiningOrVariationCount, metrics.surrogatePairCount);
	Mark(Event::FirstContentNonBlockTextOtherSummary,
		metrics.latinExtendedCount, metrics.otherBmpCount);
}

void CStartupTrace::MarkFirstContentPainted()
{
	auto& state = GetTraceState();
	if (!state.enabled.load(std::memory_order_relaxed)
		|| !state.startupDocumentArmed.load(std::memory_order_relaxed)
		|| !state.startupDocumentCompleted.load(std::memory_order_relaxed)
		|| state.firstContentPainted.exchange(true, std::memory_order_relaxed)) {
		return;
	}
	Mark(Event::FirstContentPainted);
}

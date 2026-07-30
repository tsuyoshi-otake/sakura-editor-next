/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"
#include "extension/CExtensionDocumentBridge.h"
#include "extension/CExtensionRpcProtocol.h"
#include "extension/CExtensionService.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <vector>

namespace {

SExtensionDocumentSnapshot Snapshot(std::uint64_t version, std::wstring text = L"one\r\ntwo\r\n")
{
	return {
		.id = { 100, 7 }, .uri = L"file:///C:/work/sample.txt", .languageId = L"plaintext",
		.text = std::move(text), .version = version, .dirty = version > 1,
	};
}

struct PerformanceSummary {
	std::size_t samples = 0;
	double p50Milliseconds = 0;
	double p95Milliseconds = 0;
	double maximumMilliseconds = 0;
};

PerformanceSummary SummarizePerformance(std::vector<double> milliseconds)
{
	std::sort(milliseconds.begin(), milliseconds.end());
	const auto percentile = [&](double value) {
		const auto rank = static_cast<std::size_t>(std::ceil(value * milliseconds.size()));
		return milliseconds[std::clamp<std::size_t>(rank, 1, milliseconds.size()) - 1];
	};
	return {
		.samples = milliseconds.size(),
		.p50Milliseconds = percentile(0.50),
		.p95Milliseconds = percentile(0.95),
		.maximumMilliseconds = milliseconds.back(),
	};
}

template<typename Action>
double MeasureMilliseconds(Action&& action)
{
	const auto begin = std::chrono::steady_clock::now();
	std::forward<Action>(action)();
	return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
}

std::string PerformanceJson(const PerformanceSummary& summary)
{
	std::ostringstream stream;
	stream << std::fixed << std::setprecision(6)
		<< R"({"samples":)" << summary.samples
		<< R"(,"p50Ms":)" << summary.p50Milliseconds
		<< R"(,"p95Ms":)" << summary.p95Milliseconds
		<< R"(,"maxMs":)" << summary.maximumMilliseconds << '}';
	return stream.str();
}

TEST(CExtensionDocumentSync, UsesProcessWideIdsAndRequiresContiguousVersions)
{
	CExtensionDocumentSync documents;
	EXPECT_EQ(EExtensionDocumentUpdateResult::Applied, documents.Open(Snapshot(1)));
	EXPECT_EQ(EExtensionDocumentUpdateResult::VersionGap, documents.Change(Snapshot(3, L"gap")));
	ASSERT_TRUE(documents.Snapshot({ 100, 7 }).has_value());
	EXPECT_EQ(1u, documents.Snapshot({ 100, 7 })->version);
	EXPECT_EQ(EExtensionDocumentUpdateResult::Applied, documents.Change(Snapshot(2, L"changed")));
	EXPECT_EQ(EExtensionDocumentUpdateResult::StaleVersion, documents.Change(Snapshot(2, L"stale")));
	EXPECT_FALSE(documents.Close({ 200, 7 }));
	EXPECT_TRUE(documents.Close({ 100, 7 }));
}

TEST(CExtensionDocumentUri, EncodesWindowsPathsLikeTheHost)
{
	const auto uri = ExtensionFileUriFromPath(L"C:\\work folder\\日本語 #1.txt");
	EXPECT_EQ(L"file:///C%3A/work%20folder/%E6%97%A5%E6%9C%AC%E8%AA%9E%20%231.txt", uri);
}

TEST(CExtensionDocumentUri, DecodesHostFileUris)
{
	const auto decoded = ExtensionFilePathFromUri(
		L"file:///C%3A/work%20folder/%E6%97%A5%E6%9C%AC%E8%AA%9E%20%231.txt");
	ASSERT_TRUE(decoded.has_value());
	EXPECT_EQ(L"C:\\work folder\\日本語 #1.txt", decoded->wstring());
}

TEST(CExtensionDocumentUri, RejectsNonFileAndMalformedUris)
{
	EXPECT_FALSE(ExtensionFilePathFromUri(L"https://example.invalid/file.txt").has_value());
	EXPECT_FALSE(ExtensionFilePathFromUri(L"file:///C%ZZ/file.txt").has_value());
}

TEST(CExtensionEventAggregator, BoundsLatencyAndFallsBackToLatestSnapshotOnOverload)
{
	using namespace std::chrono_literals;
	CExtensionEventAggregator aggregator(2, 64, 8ms, 16ms);
	const auto start = CExtensionEventAggregator::Clock::time_point{};
	aggregator.Enqueue(Snapshot(2, L"a"), start);
	ASSERT_TRUE(aggregator.NextReadyTime().has_value());
	EXPECT_EQ(start + 8ms, *aggregator.NextReadyTime());
	aggregator.Enqueue(Snapshot(3, L"bb"), start + 4ms);
	aggregator.Enqueue(Snapshot(4, L"ccc"), start + 9ms);
	EXPECT_TRUE(aggregator.DrainReady(start + 15ms).empty());
	const auto ready = aggregator.DrainReady(start + 16ms);
	ASSERT_EQ(1u, ready.size());
	EXPECT_EQ(4u, ready[0].snapshot.version);
	EXPECT_EQ(L"ccc", ready[0].snapshot.text);
	EXPECT_EQ(3u, ready[0].coalescedChanges);
	EXPECT_TRUE(ready[0].snapshotOnly);
	EXPECT_FALSE(aggregator.NextReadyTime().has_value());
}

TEST(CExtensionUiDispatcher, IsBoundedAndStopsWithExplicitTerminalResults)
{
	CExtensionUiDispatcher dispatcher(2);
	int value = 0;
	EXPECT_EQ(CExtensionUiDispatcher::PostResult::Queued, dispatcher.Post([&]() { value += 1; }));
	EXPECT_EQ(CExtensionUiDispatcher::PostResult::Queued, dispatcher.Post([&]() { value += 2; }));
	EXPECT_EQ(CExtensionUiDispatcher::PostResult::Overloaded, dispatcher.Post([&]() { value += 4; }));
	EXPECT_EQ(1u, dispatcher.Drain(1));
	EXPECT_EQ(1, value);
	dispatcher.Stop();
	EXPECT_EQ(CExtensionUiDispatcher::PostResult::Stopped, dispatcher.Post([&]() { value += 8; }));
	EXPECT_EQ(0u, dispatcher.Size());
}

TEST(CExtensionApplyEdit, AppliesMultipleEditsAtomicallyAsOneUndoUnit)
{
	CExtensionDocumentSync documents;
	ASSERT_EQ(EExtensionDocumentUpdateResult::Applied, documents.Open(Snapshot(1)));
	CExtensionApplyEdit apply(documents);
	const auto result = apply.Apply({ {
		.documentId = { 100, 7 }, .expectedVersion = 1,
		.edits = {
			{ .range = { { 0, 0 }, { 0, 3 } }, .newText = L"ONE" },
			{ .range = { { 1, 0 }, { 1, 3 } }, .newText = L"TWO" },
		},
	} });
	ASSERT_TRUE(result.Applied());
	EXPECT_EQ(1u, result.undoUnit);
	EXPECT_EQ(1u, apply.UndoUnitCount());
	const auto changed = documents.Snapshot({ 100, 7 });
	ASSERT_TRUE(changed.has_value());
	EXPECT_EQ(2u, changed->version);
	EXPECT_EQ(L"ONE\r\nTWO\r\n", changed->text);
}

TEST(CExtensionApplyEdit, AppliesRequestedDocumentEolInSameUndoUnit)
{
	CExtensionDocumentSync documents;
	ASSERT_EQ(EExtensionDocumentUpdateResult::Applied, documents.Open(Snapshot(1)));
	CExtensionApplyEdit apply(documents);
	const auto result = apply.Apply({ {
		.documentId = { 100, 7 }, .expectedVersion = 1, .edits = {}, .crlf = false,
	} });
	ASSERT_TRUE(result.Applied());
	EXPECT_EQ(1u, result.undoUnit);
	EXPECT_EQ(1u, apply.UndoUnitCount());
	const auto changed = documents.Snapshot({ 100, 7 });
	ASSERT_TRUE(changed.has_value());
	EXPECT_EQ(2u, changed->version);
	EXPECT_EQ(L"one\ntwo\n", changed->text);
}

TEST(CExtensionApplyEdit, RejectsVersionMismatchOverlapAndCommandReentryWithoutMutation)
{
	CExtensionDocumentSync documents;
	ASSERT_EQ(EExtensionDocumentUpdateResult::Applied, documents.Open(Snapshot(5)));
	CExtensionApplyEdit apply(documents);
	auto request = SExtensionDocumentEdit{
		.documentId = { 100, 7 }, .expectedVersion = 4,
		.edits = { { .range = { { 0, 0 }, { 0, 1 } }, .newText = L"x" } },
	};
	EXPECT_EQ(EExtensionApplyEditStatus::VersionMismatch, apply.Apply({ request }).status);
	request.expectedVersion = 5;
	request.edits.push_back({ .range = { { 0, 0 }, { 0, 2 } }, .newText = L"y" });
	EXPECT_EQ(EExtensionApplyEditStatus::OverlappingEdits, apply.Apply({ request }).status);
	request.edits.resize(1);
	EXPECT_EQ(EExtensionApplyEditStatus::CommandReentry, apply.Apply({ request }, true).status);
	const auto unchanged = documents.Snapshot({ 100, 7 });
	ASSERT_TRUE(unchanged.has_value());
	EXPECT_EQ(5u, unchanged->version);
	EXPECT_EQ(L"one\r\ntwo\r\n", unchanged->text);
	EXPECT_EQ(0u, apply.UndoUnitCount());
}

TEST(CExtensionPerformance, MeasuresNativeStartupEnqueueApplyEditAndHostLossBudgets)
{
	const char* enabled = std::getenv("SAKURA_RUN_EXTENSION_PERFORMANCE");
	if (!enabled || std::string_view(enabled) != "1") {
		GTEST_SKIP() << "Run through Measure-ExtensionHostPerformance.ps1";
	}
	const char* configuredSamples = std::getenv("SAKURA_EXTENSION_PERFORMANCE_SAMPLES");
	const auto parsedSamples = configuredSamples ? std::strtoul(configuredSamples, nullptr, 10) : 50;
	const std::size_t samples = std::clamp<std::size_t>(parsedSamples, 20, 500);

	std::vector<double> startup;
	std::vector<double> enqueue;
	std::vector<double> applyEdit;
	std::vector<double> hostLoss;
	startup.reserve(samples);
	enqueue.reserve(samples);
	applyEdit.reserve(samples);
	hostLoss.reserve(samples);

	const auto profile = std::filesystem::temp_directory_path() / L"sakura-extension-performance-profile";
	const auto views = std::make_shared<CExtensionViewRegistry>();
	const auto eventSnapshot = Snapshot(2, std::wstring(4096, L'x'));
	for (std::size_t index = 0; index < samples; ++index) {
		startup.push_back(MeasureMilliseconds([&] {
			CExtensionService service(nullptr, nullptr, profile, views);
		}));

		CExtensionEventAggregator aggregator;
		enqueue.push_back(MeasureMilliseconds([&] {
			aggregator.Enqueue(eventSnapshot);
		}));

		CExtensionDocumentSync documents;
		ASSERT_EQ(EExtensionDocumentUpdateResult::Applied, documents.Open(Snapshot(1)));
		CExtensionApplyEdit engine(documents);
		const SExtensionDocumentEdit edit{
			.documentId = { 100, 7 },
			.expectedVersion = 1,
			.edits = { { .range = { { 0, 0 }, { 0, 3 } }, .newText = L"ONE" } },
		};
		applyEdit.push_back(MeasureMilliseconds([&] {
			EXPECT_TRUE(engine.Apply({ edit }).Applied());
		}));

		CExtensionRpcProtocol protocol;
		for (std::size_t pending = 0; pending < 32; ++pending) {
			SExtensionRpcOutbound outbound;
			std::string error;
			ASSERT_TRUE(protocol.CreateRequest("performance/pending", R"({"value":1})", outbound, error));
		}
		hostLoss.push_back(MeasureMilliseconds([&] {
			const auto result = protocol.CloseHostLost("performance probe");
			EXPECT_EQ(32u, result.failedRequests.size());
		}));
	}

	const auto startupSummary = SummarizePerformance(std::move(startup));
	const auto enqueueSummary = SummarizePerformance(std::move(enqueue));
	const auto applySummary = SummarizePerformance(std::move(applyEdit));
	const auto hostLossSummary = SummarizePerformance(std::move(hostLoss));
	std::cout << "EXTENSION_NATIVE_PERF_JSON={"
		<< R"("editorStartupAdded":)" << PerformanceJson(startupSummary) << ','
		<< R"("uiThreadWaitMs":0,)"
		<< R"("inputEnqueue":)" << PerformanceJson(enqueueSummary) << ','
		<< R"("applyEdit":)" << PerformanceJson(applySummary) << ','
		<< R"("protocolHostLossReject":)" << PerformanceJson(hostLossSummary)
		<< "}" << std::endl;

	EXPECT_LE(startupSummary.p95Milliseconds, 5.0);
	EXPECT_LE(enqueueSummary.p95Milliseconds, 0.5);
	EXPECT_LE(applySummary.p95Milliseconds, 16.0);
	EXPECT_LE(hostLossSummary.p95Milliseconds, 500.0);
}

} // namespace

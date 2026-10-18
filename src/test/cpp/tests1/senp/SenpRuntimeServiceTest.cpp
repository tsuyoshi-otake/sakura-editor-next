/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#include "pch.h"
#include <gtest/gtest.h>
#include "senp/SenpRuntimeService.h"
#include "senp/SenpManagementService.h"
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

// The exact bytes the v1 host sends; the producer side is pinned byte for byte
// by `hello_reply_is_the_exact_v1_handshake_bytes` in `sakura_senp_host/src/main.rs`.
constexpr std::string_view kHello =
	R"({"type":"hello","protocol":1,"abi":"sakura:senp/extension@1.0.0"})";
constexpr std::string_view kDecorations =
	R"({"type":"decorations","slots":[{"line":0,"visualStart":0,"visualLength":1,"depth":0}]})";

TEST(SenpRuntimeService, HelloReplyRequiresExactlyTheV1Members)
{
	using senp::detail::AcceptExtensionHello;
	EXPECT_TRUE(AcceptExtensionHello(kHello));
	// Strict JSON only: comments and trailing commas are not the protocol.
	EXPECT_FALSE(AcceptExtensionHello(
		R"({"type":"hello",/*c*/"protocol":1,"abi":"sakura:senp/extension@1.0.0"})"));
	EXPECT_FALSE(AcceptExtensionHello(
		R"({"type":"hello","protocol":1,"abi":"sakura:senp/extension@1.0.0",})"));
	// The marker text inside another member's value is not a hello reply.
	EXPECT_FALSE(AcceptExtensionHello(
		R"({"type":"error","detail":{"type":"hello","protocol":1,"abi":"sakura:senp/extension@1.0.0"}})"));
	// Exactly the three members: wrong values and extra/missing members fail.
	EXPECT_FALSE(AcceptExtensionHello(
		R"({"type":"hello","protocol":2,"abi":"sakura:senp/extension@1.0.0"})"));
	EXPECT_FALSE(AcceptExtensionHello(
		R"({"type":"hello","protocol":1,"abi":"sakura:senp/extension@1.0.0","extra":1})"));
	EXPECT_FALSE(AcceptExtensionHello(
		R"({"type":"hello","protocol":1})"));
	EXPECT_FALSE(AcceptExtensionHello(R"({"type":"hello","protocol":"1","abi":"sakura:senp/extension@1.0.0"})"));
	EXPECT_FALSE(AcceptExtensionHello(R"(["type","hello","protocol",1])"));
	EXPECT_FALSE(AcceptExtensionHello(""));
}

TEST(SenpRuntimeService, DecorationsReplyUsesStrictJson)
{
	using senp::detail::ParseDecorationSlots;
	std::vector<std::pair<std::uint32_t, senp::IndentDecoration>> slots;
	ASSERT_TRUE(ParseDecorationSlots(kDecorations, 1, slots));
	ASSERT_EQ(slots.size(), 1U);
	EXPECT_EQ(slots[0].first, 0U);
	EXPECT_EQ(slots[0].second.visualStart, 0U);
	EXPECT_EQ(slots[0].second.visualLength, 1U);
	EXPECT_EQ(slots[0].second.depth, 0U);
	// Strict JSON only: a trailing comma or a comment is not the protocol.
	EXPECT_FALSE(ParseDecorationSlots(
		R"({"type":"decorations","slots":[{"line":0,"visualStart":0,"visualLength":1,"depth":0},]})", 1, slots));
	EXPECT_FALSE(ParseDecorationSlots(
		R"({"type":"decorations",/*c*/"slots":[]})", 1, slots));
	// Shape checks that already held keep holding.
	EXPECT_FALSE(ParseDecorationSlots(R"({"type":"other","slots":[]})", 1, slots));
	EXPECT_FALSE(ParseDecorationSlots(
		R"({"type":"decorations","slots":[{"line":0,"visualStart":-1,"visualLength":1,"depth":0}]})", 1, slots));
	EXPECT_FALSE(ParseDecorationSlots("", 1, slots));
}

TEST(SenpRuntimeService, DecorationsReplyMapsSlotsToBatchLines)
{
	using senp::detail::ParseDecorationSlots;
	std::vector<std::pair<std::uint32_t, senp::IndentDecoration>> slots;
	ASSERT_TRUE(ParseDecorationSlots(
		R"({"type":"decorations","slots":[{"line":0,"visualStart":0,"visualLength":1,"depth":0},{"line":1,"visualStart":4,"visualLength":2,"depth":1}]})",
		2, slots));
	ASSERT_EQ(slots.size(), 2U);
	EXPECT_EQ(slots[0].first, 0U);
	EXPECT_EQ(slots[1].first, 1U);
	EXPECT_EQ(slots[1].second.depth, 1U);
	// A slot index outside the request's lines array is rejected.
	EXPECT_FALSE(ParseDecorationSlots(
		R"({"type":"decorations","slots":[{"line":2,"visualStart":0,"visualLength":1,"depth":0}]})",
		2, slots));
	EXPECT_FALSE(ParseDecorationSlots(
		R"({"type":"decorations","slots":[{"line":-1,"visualStart":0,"visualLength":1,"depth":0}]})",
		2, slots));
}

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

template <typename Predicate>
bool WaitFor(Predicate&& condition, std::chrono::milliseconds timeout = 15s)
{
	const auto deadline = Clock::now() + timeout;
	while (Clock::now() < deadline) {
		if (condition()) return true;
		std::this_thread::sleep_for(1ms);
	}
	return condition();
}

// Test-side management authority: a scriptable extension catalog behind a
// mutex, with call counters the worker's access pattern is asserted against.
class FakeManagement final : public senp::ISenpManagementService {
public:
	void SetExtensions(std::vector<senp::ExtensionDescriptor> extensions)
	{
		std::lock_guard lock(m_mutex);
		m_snapshot.state = senp::EManagementState::Ready;
		m_snapshot.extensions = std::move(extensions);
		++m_snapshot.revision;
	}

	// Arms a one-shot block inside Revision() so pending lines accumulate
	// while the worker is stuck; ReleaseRevision() lets it through.
	void ArmRevisionGate() { m_revisionGate.store(1); }
	void ReleaseRevision() { m_revisionGate.store(2); }

	[[nodiscard]] senp::ManagementOperationResult Start() override { return Unavailable(); }
	[[nodiscard]] senp::ManagementOperationResult InstallDeveloperPackage(
		std::wstring_view, bool) override { return Unavailable(); }
	[[nodiscard]] senp::ManagementOperationResult InstallBuiltInPackage(
		std::wstring_view) override { return Unavailable(); }
	[[nodiscard]] senp::ManagementOperationResult UninstallBuiltInPackage(
		std::wstring_view) override { return Unavailable(); }
	[[nodiscard]] senp::ManagementOperationResult SetExtensionEnabled(
		std::wstring_view, bool) override { return Unavailable(); }
	[[nodiscard]] senp::ManagementOperationResult UninstallPackage(
		std::wstring_view) override { return Unavailable(); }
	[[nodiscard]] senp::ManagementOperationResult Refresh() override { return Unavailable(); }
	void Stop() noexcept override {}
	[[nodiscard]] senp::ManagementSnapshot Snapshot() const override
	{
		++snapshotCalls;
		std::lock_guard lock(m_mutex);
		return m_snapshot;
	}
	[[nodiscard]] std::uint64_t Revision() const override
	{
		++revisionCalls;
		if (m_revisionGate.load() == 1) {
			(void)WaitFor([this] { return m_revisionGate.load() != 1; }, 30s);
		}
		std::lock_guard lock(m_mutex);
		return m_snapshot.revision;
	}

	mutable std::atomic<int> snapshotCalls{ 0 };
	mutable std::atomic<int> revisionCalls{ 0 };

private:
	[[nodiscard]] senp::ManagementOperationResult Unavailable() const
	{
		return { senp::EManagementOperationStatus::Unavailable, {} };
	}

	mutable std::mutex m_mutex;
	senp::ManagementSnapshot m_snapshot;
	std::atomic<int> m_revisionGate{ 0 };
};

// One scripted host instance. Counters are atomic because the worker thread
// owns the calls.
struct HostScript final {
	std::atomic<int> openCalls{ 0 };
	std::atomic<int> decorateCalls{ 0 };
	std::atomic<int> closeCalls{ 0 };
	std::atomic<int> maxFrameLines{ 0 };
	std::wstring openedModulePath;
	std::wstring openedSha256;
	bool openSucceeds = true;
	std::function<bool(const std::vector<senp::detail::DecorateLine>&,
		std::vector<std::pair<std::uint32_t, senp::IndentDecoration>>&)> onDecorate;
};

class FakeHost final : public senp::detail::ISenpExtensionHost {
public:
	explicit FakeHost(std::shared_ptr<HostScript> script) : m_script(std::move(script)) {}

	[[nodiscard]] bool Open(std::wstring_view modulePath, std::wstring_view moduleSha256) override
	{
		++m_script->openCalls;
		m_script->openedModulePath = modulePath;
		m_script->openedSha256 = moduleSha256;
		return m_script->openSucceeds;
	}
	[[nodiscard]] bool Decorate(std::uint32_t,
		const std::vector<senp::detail::DecorateLine>& lines,
		std::vector<std::pair<std::uint32_t, senp::IndentDecoration>>& slots) override
	{
		++m_script->decorateCalls;
		int expected = m_script->maxFrameLines.load();
		while (static_cast<int>(lines.size()) > expected
			&& !m_script->maxFrameLines.compare_exchange_weak(expected,
				static_cast<int>(lines.size()))) {
		}
		if (m_script->onDecorate) return m_script->onDecorate(lines, slots);
		for (const auto& line : lines) slots.push_back({ line.line, { 0, 1, 0 } });
		return true;
	}
	void Close() noexcept override { ++m_script->closeCalls; }

private:
	std::shared_ptr<HostScript> m_script;
};

// Runtime fixture: scriptable hosts in creation order, an atomic repaint
// counter, and a management authority with Ready extensions.
struct RuntimeFixture final {
	RuntimeFixture()
	{
		hooks.hostFactory = [this]() -> std::unique_ptr<senp::detail::ISenpExtensionHost> {
			std::shared_ptr<HostScript> script;
			if (nextScript < scripts.size()) script = scripts[nextScript++];
			else script = std::make_shared<HostScript>();
			return std::make_unique<FakeHost>(std::move(script));
		};
		hooks.repaint = [this](std::uintptr_t) { ++repaints; };
	}

	senp::ExtensionDescriptor Extension(std::wstring id, std::wstring_view sha = {})
	{
		senp::ExtensionDescriptor extension;
		extension.id = std::move(id);
		extension.enabled = true;
		extension.contributesIndentDecorations = true;
		extension.modulePath = L"C:/fake/" + extension.id + L".wasm";
		extension.moduleSha256 = sha.empty() ? std::wstring(64, L'a') : std::wstring(sha);
		return extension;
	}

	std::optional<std::vector<senp::IndentDecoration>> Request(
		std::wstring_view line, std::uint32_t tabSize = 4, std::uintptr_t repaintWindow = 0)
	{
		return service->RequestIndentDecorations(line, tabSize, repaintWindow);
	}
	std::optional<std::vector<senp::IndentDecoration>> WaitCached(
		std::wstring_view line, std::uint32_t tabSize = 4)
	{
		std::optional<std::vector<senp::IndentDecoration>> result;
		if (!WaitFor([&] {
				result = Request(line, tabSize);
				return result.has_value();
			})) return std::nullopt;
		return result;
	}

	FakeManagement management;
	senp::detail::SenpRuntimeHooks hooks;
	std::vector<std::shared_ptr<HostScript>> scripts;
	std::size_t nextScript = 0;
	std::atomic<int> repaints{ 0 };
	std::unique_ptr<senp::ISenpRuntimeService> service;

	void Start()
	{
		service = senp::CreateWin32SenpRuntimeService(management, hooks);
		ASSERT_TRUE(service->Start());
	}
};

TEST(SenpRuntimeService, FailedHostIsRetiredAndLineCompletesFromSurvivors)
{
	RuntimeFixture fixture;
	const auto hostA = std::make_shared<HostScript>();
	hostA->onDecorate = [](const std::vector<senp::detail::DecorateLine>&,
		std::vector<std::pair<std::uint32_t, senp::IndentDecoration>>&) { return false; };
	const auto hostB = std::make_shared<HostScript>();
	fixture.scripts = { hostA, hostB };
	fixture.management.SetExtensions({ fixture.Extension(L"ext.a"), fixture.Extension(L"ext.b") });
	fixture.Start();

	// The same line requested ten times meets one failed exchange, not ten:
	// the line completes from B, is cached empty-for-A's-part, and repaints
	// once. A dead host is retired, never reopened at the same identity.
	int lastCalls = 0;
	for (int i = 0; i < 10; ++i) {
		const auto result = fixture.Request(L"\tvalue", 4, 7);
		if (i == 9) {
			EXPECT_TRUE(result.has_value());
			// The surviving host B contributes its decoration.
			if (result) EXPECT_EQ(result->size(), 1U);
		}
		(void)WaitFor([&] {
			return hostA->decorateCalls.load() > lastCalls || result.has_value();
		}, 2s);
		lastCalls = hostA->decorateCalls.load();
	}
	EXPECT_EQ(hostA->decorateCalls.load(), 1);
	EXPECT_EQ(hostA->closeCalls.load(), 1);
	EXPECT_EQ(hostA->openCalls.load(), 1);
	EXPECT_EQ(fixture.repaints.load(), 1);
}

TEST(SenpRuntimeService, FailureProducesDiagnosticNamingExtension)
{
	RuntimeFixture fixture;
	const auto hostA = std::make_shared<HostScript>();
	hostA->onDecorate = [](const std::vector<senp::detail::DecorateLine>&,
		std::vector<std::pair<std::uint32_t, senp::IndentDecoration>>&) { return false; };
	fixture.scripts = { hostA };
	fixture.management.SetExtensions({ fixture.Extension(L"ext.a") });
	fixture.Start();

	fixture.Request(L"\tvalue");
	ASSERT_TRUE(WaitFor([&] {
		return fixture.service->Snapshot().state == senp::ERuntimeState::ReadyWithDiagnostics;
	}));
	EXPECT_NE(fixture.service->Snapshot().diagnostic.find(L"ext.a"), std::wstring::npos);
	EXPECT_EQ(fixture.service->Snapshot().activeHosts, 0U);
	// The retired identity is never reopened, even across a revision bump
	// that still advertises the same identity. `other` resolves from the
	// remaining (zero) hosts.
	fixture.management.SetExtensions({ fixture.Extension(L"ext.a") });
	ASSERT_TRUE(fixture.WaitCached(L"other"));
	EXPECT_EQ(hostA->openCalls.load(), 1);
}

TEST(SenpRuntimeService, CacheEvictsLeastRecentlyUsedByBytes)
{
	RuntimeFixture fixture;
	fixture.scripts = { std::make_shared<HostScript>() };
	fixture.management.SetExtensions({ fixture.Extension(L"ext.a") });
	fixture.Start();

	// ~64 KiB per key (the largest admitted line): 300 entries account for
	// well over the 16 MiB cache cap while staying far below the retired
	// 4096-entry count bound.
	const std::wstring body(65000, L'x');
	std::vector<std::wstring> lines;
	for (int i = 0; i < 300; ++i) {
		lines.push_back(body);
		lines.back().replace(0, 6, std::to_wstring(i));
	}
	// lines[0] is touched after every insertion, so it is never the least
	// recently used entry; insertion-order eviction would drop it as soon as
	// the cap is first exceeded, and the hit below would then fail.
	ASSERT_TRUE(fixture.WaitCached(lines[0]));
	for (std::size_t i = 1; i < lines.size(); ++i) {
		ASSERT_TRUE(fixture.WaitCached(lines[i])) << i;
		ASSERT_TRUE(fixture.Request(lines[0]).has_value()) << i;
	}
	EXPECT_EQ(fixture.scripts[0]->decorateCalls.load(), 300);
	// Each entry is charged its ~65000-byte key twice (map and LRU list)
	// plus its decorations and entry size, so 16 MiB holds 129 of them:
	// lines[0] and the 128 newest. Charging the key once would hold twice
	// as many. Counting stops at the first miss, so it evicts nothing it
	// still has to count.
	std::size_t newest = 0;
	for (auto i = lines.size() - 1; i > 0 && fixture.Request(lines[i]).has_value(); --i) ++newest;
	EXPECT_GE(newest, 120U);
	EXPECT_LE(newest, 135U);
	// The never-touched lines[1] was the least recently used and is gone:
	// asking for it again re-queues the work.
	EXPECT_FALSE(fixture.Request(lines[1]).has_value());
}

TEST(SenpRuntimeService, CacheKeepsTheNewestEntryEvenAboveTheCap)
{
	RuntimeFixture fixture;
	const auto host = std::make_shared<HostScript>();
	// One line whose merged decorations alone exceed the 16 MiB cache cap,
	// as many hosts each contributing a full reply would.
	host->onDecorate = [](const std::vector<senp::detail::DecorateLine>& lines,
		std::vector<std::pair<std::uint32_t, senp::IndentDecoration>>& slots) {
		for (const auto& line : lines) {
			for (std::uint32_t i = 0; i < 1'500'000; ++i) slots.push_back({ line.line, { i, 1, 0 } });
		}
		return true;
	};
	fixture.scripts = { host };
	fixture.management.SetExtensions({ fixture.Extension(L"ext.a") });
	fixture.Start();

	const auto result = fixture.WaitCached(L"\tvalue");
	ASSERT_TRUE(result);
	EXPECT_EQ(result->size(), 1'500'000U);
	// Stored once, never re-queued by its own eviction.
	EXPECT_EQ(host->decorateCalls.load(), 1);
}

TEST(SenpRuntimeService, BatchFramesStopAtTheLineBound)
{
	RuntimeFixture fixture;
	const auto host = std::make_shared<HostScript>();
	fixture.scripts = { host };
	fixture.management.SetExtensions({ fixture.Extension(L"ext.a") });
	fixture.management.ArmRevisionGate();
	fixture.Start();

	std::vector<std::wstring> lines;
	for (int i = 0; i < 70; ++i) {
		lines.push_back(L"line " + std::to_wstring(i));
		fixture.Request(lines.back());
	}
	fixture.management.ReleaseRevision();
	for (const auto& line : lines) ASSERT_TRUE(fixture.WaitCached(line));
	EXPECT_EQ(host->decorateCalls.load(), 2);
	EXPECT_EQ(host->maxFrameLines.load(), 64);
}

TEST(SenpRuntimeService, BatchFramesStopAtTheUnitBound)
{
	RuntimeFixture fixture;
	const auto host = std::make_shared<HostScript>();
	fixture.scripts = { host };
	fixture.management.SetExtensions({ fixture.Extension(L"ext.a") });
	fixture.management.ArmRevisionGate();
	fixture.Start();

	// A 20000-unit first line travels alone; the 500-unit lines behind it
	// group twenty to a frame (10000 units), and the twenty-first starts the
	// next frame. Without the unit bound all 22 lines share one frame.
	std::vector<std::wstring> lines{ std::wstring(20000, L'\t') };
	for (int i = 0; i < 21; ++i) {
		lines.push_back(std::wstring(500, L' '));
		lines.back().replace(0, 3, std::to_wstring(100 + i));
	}
	for (const auto& line : lines) fixture.Request(line);
	fixture.management.ReleaseRevision();
	for (const auto& line : lines) ASSERT_TRUE(fixture.WaitCached(line));
	EXPECT_EQ(host->decorateCalls.load(), 3);
	EXPECT_EQ(host->maxFrameLines.load(), 20);
}

TEST(SenpRuntimeService, RetirementDiagnosticKeepsItsReasonUntilReleased)
{
	RuntimeFixture fixture;
	const auto hostA = std::make_shared<HostScript>();
	hostA->openSucceeds = false;
	const auto hostB = std::make_shared<HostScript>();
	fixture.scripts = { hostA, hostB };
	const auto extA = fixture.Extension(L"ext.a");
	const auto extB = fixture.Extension(L"ext.b");
	fixture.management.SetExtensions({ extA });
	fixture.Start();

	ASSERT_TRUE(fixture.WaitCached(L"first"));
	auto snapshot = fixture.service->Snapshot();
	ASSERT_EQ(snapshot.state, senp::ERuntimeState::ReadyWithDiagnostics);
	EXPECT_NE(snapshot.diagnostic.find(L"could not be started: ext.a"), std::wstring::npos);

	// Adding B changes the live set; A's retirement keeps its own reason.
	fixture.management.SetExtensions({ extA, extB });
	ASSERT_TRUE(fixture.WaitCached(L"second"));
	snapshot = fixture.service->Snapshot();
	EXPECT_EQ(snapshot.activeHosts, 1U);
	EXPECT_NE(snapshot.diagnostic.find(L"could not be started: ext.a"), std::wstring::npos);

	// Dropping the retired A changes no live host, yet its diagnostic clears.
	fixture.management.SetExtensions({ extB });
	ASSERT_TRUE(fixture.WaitCached(L"third"));
	snapshot = fixture.service->Snapshot();
	EXPECT_EQ(snapshot.state, senp::ERuntimeState::Ready);
	EXPECT_TRUE(snapshot.diagnostic.empty());
	EXPECT_EQ(hostA->openCalls.load(), 1);
	EXPECT_EQ(hostB->closeCalls.load(), 0);
}

TEST(SenpRuntimeService, PendingQueueIsByteBounded)
{
	RuntimeFixture fixture;
	fixture.scripts = { std::make_shared<HostScript>() };
	fixture.management.SetExtensions({ fixture.Extension(L"ext.a") });
	// Hold the worker in the first Revision() poll until every request has
	// been offered, so admission (not drain speed) decides the queue depth.
	fixture.management.ArmRevisionGate();
	fixture.Start();

	// ~65000-byte keys, each charged twice (queue and dedupe set) plus the
	// entry size: the 4 MiB pending cap admits exactly 32 of the 80 offers.
	// Charging the key once admits 64; a count-only bound admits all 80.
	const std::wstring body(65000, L'y');
	std::vector<std::wstring> lines;
	for (int i = 0; i < 80; ++i) {
		lines.push_back(body);
		lines.back().replace(0, 6, std::to_wstring(i));
	}
	for (const auto& line : lines) fixture.Request(line);
	fixture.management.ReleaseRevision();
	// Settle barrier: the last line resolves only after every admitted line
	// was processed, so the counting pass below is deterministic.
	// lines[79] itself was rejected and is completed by the barrier's own
	// re-requests, so the count covers lines[0..78] only.
	ASSERT_TRUE(fixture.WaitCached(lines[79]));
	int firstPass = 0;
	for (std::size_t i = 0; i + 1 < lines.size(); ++i) {
		if (fixture.Request(lines[i]).has_value()) ++firstPass;
	}
	EXPECT_EQ(firstPass, 32);
	// Rejected lines are still completable on a later request.
	for (const auto& line : lines) ASSERT_TRUE(fixture.WaitCached(line));
}

TEST(SenpRuntimeService, BatchedDecorateSendsOneFramePerHost)
{
	RuntimeFixture fixture;
	const auto hostA = std::make_shared<HostScript>();
	// A answers each line with its own text length as the depth, so a slot
	// mapped to the wrong line shows up in that line's result.
	hostA->onDecorate = [](const std::vector<senp::detail::DecorateLine>& lines,
		std::vector<std::pair<std::uint32_t, senp::IndentDecoration>>& slots) {
		for (const auto& line : lines) {
			slots.push_back({ line.line, { 4, 1, static_cast<std::uint32_t>(line.text.size()) } });
		}
		return true;
	};
	const auto hostB = std::make_shared<HostScript>();
	fixture.scripts = { hostA, hostB };
	fixture.management.SetExtensions({ fixture.Extension(L"ext.a"), fixture.Extension(L"ext.b") });
	// Hold the worker in the first Revision() poll until all 50 lines are
	// queued, so the drain takes them as one batch.
	fixture.management.ArmRevisionGate();
	fixture.Start();

	std::vector<std::wstring> lines;
	for (int i = 0; i < 50; ++i) {
		lines.push_back(L"line " + std::to_wstring(i));
		fixture.Request(lines.back());
	}
	fixture.management.ReleaseRevision();
	for (const auto& line : lines) {
		const auto result = fixture.WaitCached(line);
		ASSERT_TRUE(result);
		// B's {0, 1, 0} sorts before A's {4, 1, len}.
		ASSERT_EQ(result->size(), 2U);
		EXPECT_EQ((*result)[0].visualStart, 0U);
		EXPECT_EQ((*result)[1].visualStart, 4U);
		EXPECT_EQ((*result)[1].depth, line.size());
	}
	EXPECT_EQ(hostA->decorateCalls.load(), 1);
	EXPECT_EQ(hostB->decorateCalls.load(), 1);
	EXPECT_EQ(hostA->maxFrameLines.load(), 50);
	EXPECT_EQ(hostB->maxFrameLines.load(), 50);
}

TEST(SenpRuntimeService, SnapshotOnlyWhenRevisionChanges)
{
	RuntimeFixture fixture;
	fixture.scripts = { std::make_shared<HostScript>() };
	fixture.management.SetExtensions({ fixture.Extension(L"ext.a") });
	fixture.Start();

	// Each line is awaited before the next is offered, so every line is its
	// own worker pass: twenty Revision() polls, one Snapshot().
	for (int i = 0; i < 20; ++i) {
		ASSERT_TRUE(fixture.WaitCached(L"line " + std::to_wstring(i)));
	}
	EXPECT_GE(fixture.management.revisionCalls.load(), 20);
	EXPECT_EQ(fixture.management.snapshotCalls.load(), 1);
}

TEST(SenpRuntimeService, RevisionChangeRestartsOnlyChangedHosts)
{
	RuntimeFixture fixture;
	const auto hostA = std::make_shared<HostScript>();
	const auto hostB = std::make_shared<HostScript>();
	// The reopened A is a third host instance.
	const auto hostA2 = std::make_shared<HostScript>();
	fixture.scripts = { hostA, hostB, hostA2 };
	const auto extA = fixture.Extension(L"ext.a");
	const auto extB = fixture.Extension(L"ext.b");
	fixture.management.SetExtensions({ extA, extB });
	fixture.Start();
	ASSERT_TRUE(fixture.WaitCached(L"first"));

	// Removing A closes only A; B keeps the same live instance.
	fixture.management.SetExtensions({ extB });
	ASSERT_TRUE(fixture.WaitCached(L"second"));
	EXPECT_EQ(hostA->openCalls.load(), 1);
	EXPECT_EQ(hostA->closeCalls.load(), 1);
	EXPECT_EQ(hostB->openCalls.load(), 1);
	EXPECT_EQ(hostB->closeCalls.load(), 0);

	// Reintroducing A with a new module digest reopens only A.
	auto upgradedA = extA;
	upgradedA.moduleSha256 = std::wstring(64, L'b');
	fixture.management.SetExtensions({ extB, upgradedA });
	ASSERT_TRUE(fixture.WaitCached(L"third"));
	EXPECT_EQ(hostA2->openCalls.load(), 1);
	EXPECT_EQ(hostA2->openedSha256, std::wstring(64, L'b'));
	EXPECT_EQ(hostB->openCalls.load(), 1);
	EXPECT_EQ(hostB->closeCalls.load(), 0);

	// A revision bump with an identical set must not invalidate the cache.
	// `fourth` forces a reconcile pass so the comparison is settled.
	fixture.management.SetExtensions({ extB, upgradedA });
	ASSERT_TRUE(fixture.WaitCached(L"fourth"));
	EXPECT_TRUE(fixture.Request(L"third").has_value());
}

TEST(SenpRuntimeService, RetiredHostInvalidatesTheCache)
{
	RuntimeFixture fixture;
	const auto hostA = std::make_shared<HostScript>();
	hostA->onDecorate = [](const std::vector<senp::detail::DecorateLine>& lines,
		std::vector<std::pair<std::uint32_t, senp::IndentDecoration>>& slots) {
		for (const auto& line : lines) {
			// Fail on the marked line, otherwise contribute a depth-9 marker.
			if (line.text.find(L"fail-me") != std::wstring::npos) return false;
			slots.push_back({ line.line, { 0, 1, 9 } });
		}
		return true;
	};
	const auto hostB = std::make_shared<HostScript>();
	fixture.scripts = { hostA, hostB };
	fixture.management.SetExtensions({ fixture.Extension(L"ext.a"), fixture.Extension(L"ext.b") });
	fixture.Start();

	// L is computed under the two-host set.
	const auto first = fixture.WaitCached(L"keep");
	ASSERT_TRUE(first);
	ASSERT_EQ(first->size(), 2U);

	// A dies on M; that retires it and invalidates every result computed
	// with it. L's next request is recomputed from B alone.
	fixture.Request(L"fail-me");
	ASSERT_TRUE(WaitFor([&] { return hostA->closeCalls.load() >= 1; }));
	const auto second = fixture.WaitCached(L"keep");
	ASSERT_TRUE(second);
	ASSERT_EQ(second->size(), 1U);
	EXPECT_EQ(second->front().depth, 0U);
	EXPECT_EQ(hostA->decorateCalls.load(), 2);
}

TEST(SenpRuntimeService, LineWithUnpairedSurrogateGetsEmptyResult)
{
	RuntimeFixture fixture;
	const auto host = std::make_shared<HostScript>();
	fixture.scripts = { host };
	fixture.management.SetExtensions({ fixture.Extension(L"ext.a") });
	fixture.Start();

	const std::wstring line = std::wstring(L"a") + static_cast<wchar_t>(0xD800) + L"z";
	const auto result = fixture.Request(line);
	ASSERT_TRUE(result.has_value());
	EXPECT_TRUE(result->empty());
	std::this_thread::sleep_for(50ms);
	EXPECT_EQ(host->decorateCalls.load(), 0);
}

TEST(SenpRuntimeService, SecondTabSizeIsNotStarved)
{
	RuntimeFixture fixture;
	fixture.scripts = { std::make_shared<HostScript>() };
	fixture.management.SetExtensions({ fixture.Extension(L"ext.a") });
	fixture.Start();

	for (int i = 0; i < 8; ++i) fixture.Request(L"tab4-" + std::to_wstring(i), 4);
	fixture.Request(L"tab8", 8);
	for (int i = 0; i < 8; ++i) ASSERT_TRUE(fixture.WaitCached(L"tab4-" + std::to_wstring(i), 4));
	EXPECT_TRUE(fixture.WaitCached(L"tab8", 8));
}

TEST(SenpRuntimeService, StopJoinsWithQueuedBatch)
{
	RuntimeFixture fixture;
	const auto host = std::make_shared<HostScript>();
	std::atomic<bool> release{ false };
	host->onDecorate = [&release](const std::vector<senp::detail::DecorateLine>& lines,
		std::vector<std::pair<std::uint32_t, senp::IndentDecoration>>& slots) {
		(void)WaitFor([&] { return release.load(); });
		for (const auto& line : lines) slots.push_back({ line.line, { 0, 1, 0 } });
		return true;
	};
	fixture.scripts = { host };
	fixture.management.SetExtensions({ fixture.Extension(L"ext.a") });
	fixture.Start();
	for (int i = 0; i < 10; ++i) fixture.Request(L"queued-" + std::to_wstring(i));
	ASSERT_TRUE(WaitFor([&] { return host->decorateCalls.load() >= 1; }));
	release.store(true);
	const auto started = Clock::now();
	fixture.service->Stop();
	EXPECT_LT(Clock::now() - started, 5s);
	EXPECT_EQ(fixture.service->Snapshot().state, senp::ERuntimeState::Stopped);
}

} // namespace

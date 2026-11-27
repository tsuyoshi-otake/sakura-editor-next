/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include "terminal/runtime/TerminalRuntimeService.h"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace {

using namespace std::chrono_literals;

struct FakeBackendState final {
	std::mutex mutex;
	std::condition_variable condition;
	terminal::TerminalStartResult startResult = terminal::TerminalStartResult::Success();
	terminal::TerminalLaunchOptions launch;
	bool closed{};
	std::size_t startCalls{};
};

class FakeTerminalBackend final : public terminal::ITerminalBackend {
public:
	explicit FakeTerminalBackend(std::shared_ptr<FakeBackendState> state)
		: m_state(std::move(state)) {}

	terminal::TerminalStartResult Start(const terminal::TerminalLaunchOptions& options) override
	{
		const std::lock_guard lock(m_state->mutex);
		m_state->launch = options;
		++m_state->startCalls;
		return m_state->startResult;
	}
	terminal::TerminalBackendReadResult ReadOutput(
		std::span<std::uint8_t> destination, std::chrono::milliseconds timeout) override
	{
		std::unique_lock lock(m_state->mutex);
		m_state->condition.wait_for(lock, timeout, [&] { return m_state->closed; });
		if (m_state->closed) return { terminal::TerminalBackendReadStatus::EndOfFile, 0, 0 };
		static_cast<void>(destination);
		return {};
	}
	terminal::TerminalBackendWriteResult WriteInput(std::span<const std::uint8_t> source) override
	{
		const std::lock_guard lock(m_state->mutex);
		if (m_state->closed) return { terminal::TerminalBackendWriteStatus::Closed, 0, ERROR_OPERATION_ABORTED };
		return { terminal::TerminalBackendWriteStatus::Completed, source.size(), 0 };
	}
	terminal::TerminalBackendOperationResult Resize(terminal::TerminalSize) override
	{
		return { true, 0 };
	}
	void RequestGracefulClose() noexcept override { m_state->condition.notify_all(); }
	terminal::TerminalBackendExitResult WaitForExit(std::chrono::milliseconds) noexcept override
	{
		return { terminal::TerminalBackendExitStatus::Exited, 0, 0 };
	}
	void ForceTerminate() noexcept override { m_state->condition.notify_all(); }
	void Close() noexcept override
	{
		{
			const std::lock_guard lock(m_state->mutex);
			m_state->closed = true;
		}
		m_state->condition.notify_all();
	}
	std::optional<terminal::TerminalBackendProcessIdentity> GetProcessIdentity() const noexcept override
	{
		return std::nullopt;
	}
	bool OwnsProcess(const std::uint32_t, const std::uint64_t) const noexcept override { return false; }
private:
	std::shared_ptr<FakeBackendState> m_state;
};

terminal::HarnessOperationId Operation(const std::uint8_t value)
{
	terminal::HarnessOperationId operation;
	operation.value[0] = value;
	return operation;
}

terminal::TerminalRuntimeSessionFactory Factory(const std::shared_ptr<FakeBackendState>& state)
{
	return [state](terminal::TerminalSessionCallbacks callbacks) {
		return std::make_unique<terminal::CTerminalSession>(
			std::make_unique<FakeTerminalBackend>(state), std::move(callbacks));
	};
}

terminal::TerminalCreateRequest CreateRequest(const std::uint8_t operation)
{
	terminal::TerminalCreateRequest request;
	request.operationId = Operation(operation);
	request.sessionId = terminal::TerminalSessionId{ 1 };
	request.launch.executablePath = L"fake-terminal.exe";
	request.launch.initialSize = { 80, 24 };
	return request;
}

void Print(terminal::TerminalModel& model, std::u32string_view text)
{
	for (const auto ch : text) {
		if (ch < 32) model.ExecuteControl(static_cast<wchar_t>(ch));
		else model.Print(ch);
	}
}

terminal::TerminalCaptureRequest CaptureRequest(
	const terminal::TerminalTargetCoordinate& target, std::uint8_t operation,
	std::optional<terminal::TerminalCaptureCursor> since = std::nullopt)
{
	terminal::TerminalCaptureRequest request;
	request.operationId = Operation(operation);
	request.target = target;
	request.since = since;
	return request;
}

struct CaptureFixture final {
	std::shared_ptr<FakeBackendState> backend = std::make_shared<FakeBackendState>();
	terminal::CTerminalRuntimeService service{ Factory(backend) };
	terminal::TerminalInstanceId id;
	terminal::TerminalTargetCoordinate target;

	~CaptureFixture()
	{
		if (id.IsValid()) {
			service.BeginCloseInstance(id);
			static_cast<void>(service.WaitForInstanceClose(
				id, std::chrono::steady_clock::now() + 2s));
		}
	}
	bool Create()
	{
		const auto created = service.CreateInstance(CreateRequest(7));
		if (!created.Succeeded()) return false;
		id = created.instanceId;
		const auto* instance = service.Instance(id);
		if (instance == nullptr) return false;
		target = instance->Snapshot().coordinate;
		return true;
	}
	terminal::TerminalModel* Model() { return service.Model(id); }
};

// H1: the panel consumes the resize's change itself (the pane sink), so
// the capture index must still keep ordinal bookkeeping straight: after a
// rows-only shrink, a panel-style consume, and a drain, a fresh cursor
// followed by output must yield exactly the new rows (issue #445 step 2).
TEST(TerminalReflowCapture, PanelConsumeKeepsIncrementalCaptureExact)
{
	CaptureFixture fixture;
	ASSERT_TRUE(fixture.Create());
	auto* model = fixture.Model();
	ASSERT_NE(nullptr, model);
	for (std::size_t line = 0; line < 30; ++line) {
		std::u32string text = U"row" + std::u32string(1, U'A' + static_cast<char32_t>(line % 26));
		text += std::u32string(1, U'0' + static_cast<char32_t>(line % 10));
		Print(*model, text + U"\r\n");
	}
	fixture.service.DrainOutput(fixture.id);

	auto* instance = fixture.service.Instance(fixture.id);
	ASSERT_NE(nullptr, instance);
	ASSERT_TRUE(instance->Resize({ 80, 10 }).succeeded);
	// The panel's resize sink consumes the model's change itself.
	static_cast<void>(model->ConsumeScrollbackChange());
	fixture.service.DrainOutput(fixture.id);
	// A fresh cursor taken after the resize drain addresses the shrunken
	// screen's retained set.
	const auto fresh = fixture.service.Capture(CaptureRequest(fixture.target, 11));
	ASSERT_EQ(terminal::TerminalCaptureResultCode::Succeeded, fresh.code);

	Print(*model, U"new-one\r\nnew-two\r\n");
	fixture.service.DrainOutput(fixture.id);
	const auto delta = fixture.service.Capture(CaptureRequest(fixture.target, 12, fresh.nextCursor));
	ASSERT_EQ(terminal::TerminalCaptureResultCode::Succeeded, delta.code);
	// The exact new history ordinals come back: the two rows scrolled off
	// the top by the output, and nothing stale from before the resize.
	// Screen rows moved by the scroll are reported dirty, which is correct.
	std::vector<std::u16string> historyTexts;
	std::vector<std::u16string> screenTexts;
	for (const auto& line : delta.lines) {
		if (line.text.empty()) continue;
		if (line.firstRow < 0) historyTexts.push_back(line.text);
		else screenTexts.push_back(line.text);
	}
	ASSERT_EQ(2u, historyTexts.size());
	EXPECT_EQ(u"rowV1", historyTexts[0]);
	EXPECT_EQ(u"rowW2", historyTexts[1]);
	EXPECT_NE(screenTexts.end(),
		std::find(screenTexts.begin(), screenTexts.end(), u"new-one"));
	EXPECT_NE(screenTexts.end(),
		std::find(screenTexts.begin(), screenTexts.end(), u"new-two"));
}

// H1 (epoch): a column reflow renumbers retained rows, so every cursor
// taken before it is stale.  A capture between the resize and the next
// drain must already refuse the old cursor.
TEST(TerminalReflowCapture, ColumnResizeRejectsCursorsFromBeforeTheReflow)
{
	CaptureFixture fixture;
	ASSERT_TRUE(fixture.Create());
	auto* model = fixture.Model();
	ASSERT_NE(nullptr, model);
	Print(*model, U"0123456789abcdef\r\n");
	fixture.service.DrainOutput(fixture.id);
	const auto first = fixture.service.Capture(CaptureRequest(fixture.target, 10));
	ASSERT_EQ(terminal::TerminalCaptureResultCode::Succeeded, first.code);
	const auto cursor = first.nextCursor;

	auto* instance = fixture.service.Instance(fixture.id);
	ASSERT_NE(nullptr, instance);
	ASSERT_TRUE(instance->Resize({ 40, 24 }).succeeded);
	// No drain yet: the old cursor names a layout that no longer exists.
	const auto stale = fixture.service.Capture(CaptureRequest(fixture.target, 11, cursor));
	EXPECT_EQ(terminal::TerminalCaptureResultCode::StaleCursor, stale.code);
	EXPECT_TRUE(stale.resyncSnapshot);

	// A capture without a cursor still addresses the new layout correctly.
	const auto fresh = fixture.service.Capture(CaptureRequest(fixture.target, 12));
	ASSERT_EQ(terminal::TerminalCaptureResultCode::Succeeded, fresh.code);
}

// H2: lowering the scrollback limit evicts history rows outside the drain
// path.  The base ordinal must advance anyway; the old cursor is stale.
TEST(TerminalReflowCapture, LimitChangeAdvancesTheHistoryBase)
{
	CaptureFixture fixture;
	ASSERT_TRUE(fixture.Create());
	auto* model = fixture.Model();
	ASSERT_NE(nullptr, model);
	for (std::size_t line = 0; line < 40; ++line) {
		std::u32string text = U"line" + std::u32string(1, U'A' + static_cast<char32_t>(line % 26))
			+ std::u32string(1, U'0' + static_cast<char32_t>(line % 10));
		Print(*model, text + U"\r\n");
	}
	fixture.service.DrainOutput(fixture.id);
	const auto first = fixture.service.Capture(CaptureRequest(fixture.target, 10));
	ASSERT_EQ(terminal::TerminalCaptureResultCode::Succeeded, first.code);
	const auto cursor = first.nextCursor;
	const auto evicted = model->ScrollbackSize() - 5;

	// The tab manager consumes the model's change itself; the drain never
	// sees it.  The capture index must still learn the eviction.
	model->SetScrollbackLimit(5);
	static_cast<void>(model->ConsumeScrollbackChange());
	Print(*model, U"post\r\n");
	fixture.service.DrainOutput(fixture.id);

	const auto rejected = fixture.service.Capture(CaptureRequest(fixture.target, 11, cursor));
	EXPECT_EQ(terminal::TerminalCaptureResultCode::StaleCursor, rejected.code);
	EXPECT_TRUE(rejected.resyncSnapshot);

	const auto fresh = fixture.service.Capture(CaptureRequest(fixture.target, 12));
	ASSERT_EQ(terminal::TerminalCaptureResultCode::Succeeded, fresh.code);
	EXPECT_GT(fresh.coordinates.scrollbackBaseOrdinal, cursor.scrollbackBaseOrdinal);
	// The base advanced past every eviction (the limit shrink plus the one
	// row `post` pushed out of the bounded scrollback), whether or not a
	// drain record carried them.
	EXPECT_EQ(model->HistoryEvictedTotal() + 1, fresh.coordinates.scrollbackBaseOrdinal);
	EXPECT_EQ(cursor.scrollbackBaseOrdinal + evicted + 1, fresh.coordinates.scrollbackBaseOrdinal);
}

} // namespace

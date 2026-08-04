/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include "outline/CDlgFuncList.h"
#include "outline/CFuncInfo.h"
#include "outline/CFuncInfoArr.h"
#include "sakura_rc.h"
#include "workbench/outline/COutlineWorkbenchTool.h"
#include "workbench/outline/OutlineParserWorker.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace workbench::outline {
namespace {

template <class T>
size_t AppendTemplateValue( std::vector<std::byte>& data, T value )
{
	const size_t offset = data.size();
	const auto* bytes = reinterpret_cast<const std::byte*>(&value);
	data.insert( data.end(), bytes, bytes + sizeof(value) );
	return offset;
}

void AppendTemplateString( std::vector<std::byte>& data, const wchar_t* value )
{
	do {
		AppendTemplateValue<WORD>( data, static_cast<WORD>(*value) );
	} while( *value++ != L'\0' );
}

void AlignTemplateDword( std::vector<std::byte>& data )
{
	while( (reinterpret_cast<std::uintptr_t>(data.data()) + data.size()) % 4u != 0u ){
		data.push_back(std::byte{});
	}
}

struct TemplateItemOffsets {
	size_t style;
	size_t exStyle;
};

TemplateItemOffsets AppendStandardTemplateItem(
	std::vector<std::byte>& data, WORD id, DWORD style, DWORD exStyle )
{
	AlignTemplateDword(data);
	const size_t styleOffset = AppendTemplateValue<DWORD>(data, style);
	const size_t exStyleOffset = AppendTemplateValue<DWORD>(data, exStyle);
	for( int i = 0; i < 4; ++i ) AppendTemplateValue<short>(data, 0);
	AppendTemplateValue<WORD>(data, id);
	AppendTemplateValue<WORD>(data, 0xffff);
	AppendTemplateValue<WORD>(data, 0x0080);
	AppendTemplateString(data, L"");
	AppendTemplateValue<WORD>(data, 0);
	return { styleOffset, exStyleOffset };
}

template <class T>
T ReadTemplateValue( const std::vector<std::byte>& data, size_t offset )
{
	T value{};
	std::memcpy( &value, data.data() + offset, sizeof(value) );
	return value;
}

std::shared_ptr<const OutlineDocumentSnapshot> MakeWorkerSnapshot(
	std::uint64_t identity, std::uint64_t version )
{
	auto snapshot = std::make_shared<OutlineDocumentSnapshot>();
	snapshot->documentVersion = { identity, version };
	snapshot->filePath = L"worker-fixture.cpp";
	EXPECT_TRUE(snapshot->AppendLine(L"int value;\r\n"));
	return snapshot;
}

class BlockingOutlineParser final {
public:
	explicit BlockingOutlineParser( bool cancellationResponsive = true ) noexcept
		: m_cancellationResponsive(cancellationResponsive)
	{
	}

	OutlineParserWorker::ParseFunction Function()
	{
		return [this](
			const OutlineDocumentSnapshot& snapshot,
			int outlineType,
			int listType,
			const OutlineParserWorker::CancelToken& cancelToken ) {
				{
					std::lock_guard lock(m_mutex);
					++m_callCount;
					m_callsChanged.notify_all();
				}
				std::unique_lock lock(m_mutex);
				while( !m_release && (!m_cancellationResponsive || !cancelToken->load(std::memory_order_acquire)) ) {
					m_callsChanged.wait_for(lock, std::chrono::milliseconds(5));
				}
				OutlineParseResult result;
				result.documentVersion = snapshot.documentVersion;
				result.outlineType = outlineType;
				result.listType = listType;
				OutlineSymbolDto symbol;
				symbol.logicalLine = static_cast<int>(snapshot.documentVersion.version + 1);
				symbol.logicalColumn = 1;
				symbol.name = snapshot.documentVersion.version == 3 ? L"latest" : L"stale";
				symbol.info = FL_OBJ_FUNCTION;
				result.symbols.emplace_back(std::move(symbol));
				return result;
			};
	}

	bool WaitForCalls( int expected )
	{
		std::unique_lock lock(m_mutex);
		return m_callsChanged.wait_for(lock, std::chrono::seconds(2), [this, expected] {
			return m_callCount >= expected;
		});
	}

	void Release()
	{
		{
			std::lock_guard lock(m_mutex);
			m_release = true;
		}
		m_callsChanged.notify_all();
	}

private:
	std::mutex m_mutex;
	std::condition_variable m_callsChanged;
	int m_callCount = 0;
	bool m_release = false;
	bool m_cancellationResponsive = true;
};

class MessageOnlyWindow final {
public:
	MessageOnlyWindow()
	{
		m_window = ::CreateWindowExW(
			0, L"STATIC", L"", WS_POPUP, 0, 0, 1, 1, HWND_MESSAGE, nullptr,
			::GetModuleHandleW(nullptr), nullptr);
	}

	~MessageOnlyWindow()
	{
		Drain();
		if( m_window != nullptr ) ::DestroyWindow(m_window);
	}

	[[nodiscard]] HWND Get() const noexcept { return m_window; }

	void Drain() const noexcept
	{
		if( m_window == nullptr ) return;
		MSG message{};
		while( ::PeekMessageW(&message, m_window, 0, 0, PM_REMOVE) != FALSE ) {}
	}

private:
	HWND m_window = nullptr;
};

bool WaitForWorkerIdle( OutlineParserWorker& worker )
{
	for( int attempt = 0; attempt < 400; ++attempt ) {
		const auto state = worker.GetStateSnapshot();
		if( !state.active && !state.pending ) return true;
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	return false;
}

bool WaitForPostedResult( OutlineParserWorker& worker )
{
	for( int attempt = 0; attempt < 400; ++attempt ) {
		const auto state = worker.GetStateSnapshot();
		if( state.resultPending && state.resultMessagePosted ) return true;
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	return false;
}

TEST(OutlineWorkbenchTool, LifecycleHasExplicitTerminalState)
{
	auto state = OutlineToolLifecycle::Idle;
	EXPECT_EQ( OutlineToolLifecycle::Idle,
		AdvanceOutlineToolLifecycle(state, OutlineToolEvent::Activate) );
	state = AdvanceOutlineToolLifecycle( state, OutlineToolEvent::Create );
	EXPECT_EQ( OutlineToolLifecycle::Inactive, state );
	EXPECT_EQ( OutlineToolLifecycle::Inactive,
		AdvanceOutlineToolLifecycle(state, OutlineToolEvent::Create) );
	state = AdvanceOutlineToolLifecycle( state, OutlineToolEvent::Activate );
	EXPECT_EQ( OutlineToolLifecycle::Active, state );
	state = AdvanceOutlineToolLifecycle( state, OutlineToolEvent::Deactivate );
	EXPECT_EQ( OutlineToolLifecycle::Inactive, state );
	state = AdvanceOutlineToolLifecycle( state, OutlineToolEvent::Close );
	EXPECT_EQ( OutlineToolLifecycle::Closed, state );
	for( const auto event : { OutlineToolEvent::Create, OutlineToolEvent::Activate,
		OutlineToolEvent::Deactivate, OutlineToolEvent::Close } ) {
		EXPECT_EQ( OutlineToolLifecycle::Closed, AdvanceOutlineToolLifecycle(state, event) );
	}
}

TEST(OutlineWorkbenchTool, GeometryClampsInvalidBoundsAndDefaultsDpi)
{
	const auto layout = NormalizeOutlineToolLayout( RECT{ -20, -10, -40, -30 }, 0 );
	EXPECT_EQ( (OutlineToolLayout{ RECT{ 0, 0, 0, 0 }, 96 }), layout );
}

TEST(OutlineWorkbenchTool, GeometryPreservesHostCoordinatesAtPerMonitorDpi)
{
	const RECT expected{ 3, 31, 403, 731 };
	const auto layout = NormalizeOutlineToolLayout( expected, 192 );
	EXPECT_EQ( (OutlineToolLayout{ expected, 192 }), layout );
}

TEST(OutlineWorkbenchTool, CreatedInactiveDialogIsVisibleWithoutActivation)
{
	// A SHOW_RELOAD can create the child after the visible host's previous layout.
	// Visibility is independent from keyboard activation so editor focus is retained.
	EXPECT_FALSE( ShouldShowOutlineDialog(OutlineToolLifecycle::Idle) );
	EXPECT_TRUE( ShouldShowOutlineDialog(OutlineToolLifecycle::Inactive) );
	EXPECT_TRUE( ShouldShowOutlineDialog(OutlineToolLifecycle::Active) );
	EXPECT_FALSE( ShouldShowOutlineDialog(OutlineToolLifecycle::Closed) );
}

TEST(OutlineWorkbenchTool, ReloadRelayoutRequiresSuccessfulVisibleChildCreation)
{
	EXPECT_FALSE( ShouldRelayoutOutlineAfterReload(false, true, true) );
	EXPECT_FALSE( ShouldRelayoutOutlineAfterReload(true, false, true) );
	EXPECT_FALSE( ShouldRelayoutOutlineAfterReload(true, true, false) );
	EXPECT_TRUE( ShouldRelayoutOutlineAfterReload(true, true, true) );
}

TEST(OutlineRefreshScheduler, BurstEditsCaptureAndSubmitOnlyLatestVersion)
{
	OutlineRefreshScheduler scheduler;
	int armCount = 0;
	int killCount = 0;
	int refreshCount = 0;
	int closedCount = 0;
	auto arm = [&](std::uint64_t) { ++armCount; return true; };
	auto kill = [&](std::uint64_t) { ++killCount; };
	auto refresh = [&] { ++refreshCount; };
	auto closed = [&] { ++closedCount; };

	EXPECT_EQ(OutlineRefreshScheduleResult::Armed,
		scheduler.NotifyChange(true, true, {3, 17}, arm, kill, refresh, closed));
	EXPECT_EQ(OutlineRefreshScheduleResult::Armed,
		scheduler.NotifyChange(true, true, {3, 18}, arm, kill, refresh, closed));
	EXPECT_EQ(OutlineRefreshScheduleResult::Armed,
		scheduler.NotifyChange(true, true, {3, 19}, arm, kill, refresh, closed));
	EXPECT_EQ(3, armCount);
	EXPECT_EQ(2, killCount);
	EXPECT_EQ(0, refreshCount);
	EXPECT_TRUE(scheduler.CanReuse(false, OUTLINE_CPP, OUTLINE_CPP,
		{}, 0, {3, 19}));
	EXPECT_FALSE(scheduler.CanReuse(false, OUTLINE_CPP, OUTLINE_CPP,
		{}, 0, {3, 18}));

	const auto timerToken = scheduler.TimerToken();
	EXPECT_NE(0u, timerToken);
	EXPECT_TRUE(scheduler.ConsumeTimer(timerToken, true, true, {3, 19}, kill, refresh));
	EXPECT_EQ(3, killCount);
	EXPECT_EQ(1, refreshCount);
	EXPECT_EQ(0, closedCount);
	EXPECT_FALSE(scheduler.IsTimerArmed());
}

TEST(OutlineRefreshScheduler, StopAndDocumentSwitchRejectOldCallback)
{
	OutlineRefreshScheduler scheduler;
	int killCount = 0;
	int refreshCount = 0;
	auto kill = [&](std::uint64_t) { ++killCount; };
	auto refresh = [&] { ++refreshCount; };
	EXPECT_EQ(OutlineRefreshScheduleResult::Armed,
		scheduler.NotifyChange(true, true, {7, 1}, [](std::uint64_t) { return true; },
			kill, refresh, [] {}));
	const auto oldTimerToken = scheduler.TimerToken();
	scheduler.Stop(kill);
	EXPECT_EQ(1, killCount);
	EXPECT_FALSE(scheduler.ConsumeTimer(oldTimerToken, true, true, {7, 1}, kill, refresh));
	EXPECT_EQ(0, refreshCount);

	EXPECT_EQ(OutlineRefreshScheduleResult::Armed,
		scheduler.NotifyChange(true, true, {8, 0}, [](std::uint64_t) { return true; },
			kill, refresh, [] {}));
	const auto currentTimerToken = scheduler.TimerToken();
	ASSERT_NE(oldTimerToken, currentTimerToken);
	// A WM_TIMER queued before Stop must not consume or shorten the new document's debounce.
	EXPECT_FALSE(scheduler.ConsumeTimer(oldTimerToken, true, true, {8, 0}, kill, refresh));
	EXPECT_TRUE(scheduler.IsTimerArmed());
	EXPECT_EQ(1, killCount);
	EXPECT_EQ(0, refreshCount);
	EXPECT_TRUE(scheduler.ConsumeTimer(currentTimerToken, true, true, {8, 0}, kill, refresh));
	EXPECT_EQ(2, killCount);
	EXPECT_EQ(1, refreshCount);
}

TEST(OutlineRefreshScheduler, TimerFailureFallsBackExactlyOnce)
{
	OutlineRefreshScheduler scheduler;
	int immediateCount = 0;
	int closedCount = 0;
	EXPECT_EQ(OutlineRefreshScheduleResult::ImmediateFallback,
		scheduler.NotifyChange(true, true, {5, 4}, [](std::uint64_t) { return false; },
			[](std::uint64_t) {}, [&] { ++immediateCount; }, [&] { ++closedCount; }));
	EXPECT_EQ(1, immediateCount);
	EXPECT_EQ(0, closedCount);
	EXPECT_FALSE(scheduler.IsTimerArmed());
}

TEST(OutlineWorkbenchTool, NestedExplorerViewUsesSideBarBackgroundForEverySurface)
{
	auto palette = theme::CThemeService::PaletteFor(theme::ThemeMode::Dark);
	palette.panel = { 0x10, 0x20, 0x30 };
	palette.sideBar = { 0x40, 0x50, 0x60 };
	EXPECT_EQ(palette.sideBar.ToColorRef(), OutlineBackgroundColor(palette));
}

TEST(OutlineWorkbenchTool, SymbolKindsUseCanonicalCodiconImageSlots)
{
	EXPECT_EQ(10, CDlgFuncList::WorkbenchSymbolImageCount());
	EXPECT_EQ(0, CDlgFuncList::WorkbenchSymbolImageIndex(FL_OBJ_DEFINITION));
	EXPECT_EQ(1, CDlgFuncList::WorkbenchSymbolImageIndex(FL_OBJ_DECLARE));
	EXPECT_EQ(2, CDlgFuncList::WorkbenchSymbolImageIndex(FL_OBJ_FUNCTION));
	EXPECT_EQ(3, CDlgFuncList::WorkbenchSymbolImageIndex(FL_OBJ_CLASS));
	EXPECT_EQ(4, CDlgFuncList::WorkbenchSymbolImageIndex(FL_OBJ_STRUCT));
	EXPECT_EQ(4, CDlgFuncList::WorkbenchSymbolImageIndex(FL_OBJ_UNION));
	EXPECT_EQ(5, CDlgFuncList::WorkbenchSymbolImageIndex(FL_OBJ_ENUM));
	EXPECT_EQ(6, CDlgFuncList::WorkbenchSymbolImageIndex(FL_OBJ_NAMESPACE));
	EXPECT_EQ(7, CDlgFuncList::WorkbenchSymbolImageIndex(FL_OBJ_INTERFACE));
	EXPECT_EQ(8, CDlgFuncList::WorkbenchSymbolImageIndex(FL_OBJ_GLOBAL));
	EXPECT_EQ(9, CDlgFuncList::WorkbenchSymbolImageIndex(FL_OBJ_ELEMENT_MAX));
	EXPECT_EQ(2, CDlgFuncList::WorkbenchSymbolImageIndex(FL_OBJ_FUNCTION | FUNCINFO_NOCLIPTEXT));
}

TEST(OutlineWorkbenchTool, SymbolImageSlotsResolveToCodiconNames)
{
	EXPECT_EQ(L"symbol-misc", CDlgFuncList::WorkbenchSymbolCodiconName(0));
	EXPECT_EQ(L"symbol-method", CDlgFuncList::WorkbenchSymbolCodiconName(1));
	EXPECT_EQ(L"symbol-function", CDlgFuncList::WorkbenchSymbolCodiconName(2));
	EXPECT_EQ(L"symbol-class", CDlgFuncList::WorkbenchSymbolCodiconName(3));
	EXPECT_EQ(L"symbol-structure", CDlgFuncList::WorkbenchSymbolCodiconName(4));
	EXPECT_EQ(L"symbol-enum", CDlgFuncList::WorkbenchSymbolCodiconName(5));
	EXPECT_EQ(L"symbol-namespace", CDlgFuncList::WorkbenchSymbolCodiconName(6));
	EXPECT_EQ(L"symbol-interface", CDlgFuncList::WorkbenchSymbolCodiconName(7));
	EXPECT_EQ(L"symbol-variable", CDlgFuncList::WorkbenchSymbolCodiconName(8));
	EXPECT_EQ(L"symbol-property", CDlgFuncList::WorkbenchSymbolCodiconName(9));
	EXPECT_EQ(L"symbol-misc", CDlgFuncList::WorkbenchSymbolCodiconName(99));
}

TEST(OutlineWorkbenchTool, ChildGeometryExactlyFillsClientWithoutBorder)
{
	EXPECT_EQ((OutlineChildLayout{RECT{0, 0, 640, 360}, false}),
		MakeOutlineChildLayout(640, 360));
	EXPECT_EQ((OutlineChildLayout{RECT{0, 0, 0, 0}, false}),
		MakeOutlineChildLayout(-10, -20));
}

TEST(OutlineWorkbenchTool, TwentyCollapseReopenCyclesRetainCachedModelWithoutRefresh)
{
	OutlineRefreshCoordinator coordinator;
	constexpr OutlineDocumentVersion document{1, 7};
	coordinator.AdoptCommitted(document);
	for( int cycle = 0; cycle < 20; ++cycle ){
		coordinator.SetVisible(false);
		EXPECT_TRUE(coordinator.Snapshot().hasCommittedModel);
		coordinator.SetVisible(true);
		EXPECT_EQ(OutlineRefreshRequestStatus::Cached, coordinator.Request(document).status);
	}

	const auto snapshot = coordinator.Snapshot();
	EXPECT_TRUE(snapshot.visible);
	EXPECT_TRUE(snapshot.hasCommittedModel);
	EXPECT_EQ(document, snapshot.committedVersion);
	EXPECT_EQ(0u, snapshot.startedCount);
	EXPECT_EQ(20u, snapshot.cachedCount);
}

TEST(OutlineWorkbenchTool, RefreshStartsOncePerDocumentVersionAndDeduplicatesInFlightWork)
{
	OutlineRefreshCoordinator coordinator;
	constexpr OutlineDocumentVersion firstVersion{4, 10};
	const auto first = coordinator.Request(firstVersion);
	ASSERT_EQ(OutlineRefreshRequestStatus::Started, first.status);
	const auto duplicate = coordinator.Request(firstVersion);
	EXPECT_EQ(OutlineRefreshRequestStatus::InFlight, duplicate.status);
	EXPECT_EQ(first.generation, duplicate.generation);
	EXPECT_EQ(OutlineRefreshCompletion::Committed,
		coordinator.Complete(first.generation, true, firstVersion));
	EXPECT_EQ(OutlineRefreshRequestStatus::Cached, coordinator.Request(firstVersion).status);

	constexpr OutlineDocumentVersion secondVersion{4, 11};
	const auto second = coordinator.Request(secondVersion);
	ASSERT_EQ(OutlineRefreshRequestStatus::Started, second.status);
	EXPECT_EQ(OutlineRefreshRequestStatus::InFlight, coordinator.Request(secondVersion).status);
	EXPECT_EQ(OutlineRefreshCompletion::Committed,
		coordinator.Complete(second.generation, true, secondVersion));

	const auto snapshot = coordinator.Snapshot();
	EXPECT_EQ(2u, snapshot.startedCount);
	EXPECT_EQ(2u, snapshot.committedCount);
	EXPECT_EQ(2u, snapshot.deduplicatedCount);
}

TEST(OutlineWorkbenchTool, SupersededAndDocumentChangedGenerationsAreDiscarded)
{
	OutlineRefreshCoordinator coordinator;
	constexpr OutlineDocumentVersion firstVersion{9, 1};
	constexpr OutlineDocumentVersion secondVersion{9, 2};
	const auto first = coordinator.Request(firstVersion);
	const auto second = coordinator.Request(secondVersion);
	ASSERT_EQ(OutlineRefreshRequestStatus::Started, first.status);
	ASSERT_EQ(OutlineRefreshRequestStatus::Started, second.status);
	EXPECT_EQ(OutlineRefreshCompletion::Stale,
		coordinator.Complete(first.generation, true, firstVersion));
	EXPECT_TRUE(coordinator.Snapshot().refreshInFlight);
	EXPECT_EQ(OutlineRefreshCompletion::Committed,
		coordinator.Complete(second.generation, true, secondVersion));

	constexpr OutlineDocumentVersion thirdVersion{9, 3};
	constexpr OutlineDocumentVersion changedWhileParsing{9, 4};
	const auto third = coordinator.Request(thirdVersion);
	ASSERT_EQ(OutlineRefreshRequestStatus::Started, third.status);
	EXPECT_EQ(OutlineRefreshCompletion::Stale,
		coordinator.Complete(third.generation, true, changedWhileParsing));
	const auto stale = coordinator.Snapshot();
	EXPECT_FALSE(stale.refreshInFlight);
	EXPECT_TRUE(stale.hiddenRefreshPending);
	EXPECT_EQ(changedWhileParsing, stale.pendingVersion);
	EXPECT_EQ(secondVersion, stale.committedVersion);
}

TEST(OutlineWorkbenchTool, HiddenVersionChangeDefersWorkUntilVisibleAndCloseIsTerminal)
{
	OutlineRefreshCoordinator coordinator;
	coordinator.AdoptCommitted({12, 1});
	coordinator.SetVisible(false);
	EXPECT_EQ(OutlineRefreshRequestStatus::HiddenPending,
		coordinator.Request({12, 2}).status);
	EXPECT_EQ(0u, coordinator.Snapshot().startedCount);
	coordinator.SetVisible(true);
	EXPECT_EQ(OutlineRefreshRequestStatus::Started, coordinator.Request({12, 2}).status);
	coordinator.Close();
	EXPECT_EQ(OutlineRefreshRequestStatus::Closed, coordinator.Request({12, 3}).status);
	EXPECT_EQ(OutlineRefreshCompletion::Closed, coordinator.Complete(1, true, {12, 3}));
}

TEST(OutlineWorkbenchTool, NativeOutlineControlsAreBornWithoutNonClientBorders)
{
	std::vector<std::byte> dialog;
	dialog.reserve(256);
	AppendTemplateValue<DWORD>(dialog, WS_POPUP);
	AppendTemplateValue<DWORD>(dialog, 0);
	AppendTemplateValue<WORD>(dialog, 2);
	for( int i = 0; i < 4; ++i ) AppendTemplateValue<short>(dialog, 0);
	AppendTemplateString(dialog, L"");
	AppendTemplateString(dialog, L"");
	AppendTemplateString(dialog, L"");

	constexpr DWORD edgeStyles = WS_EX_CLIENTEDGE | WS_EX_STATICEDGE | WS_EX_WINDOWEDGE;
	const auto tree = AppendStandardTemplateItem(
		dialog, IDC_TREE_FL, WS_CHILD | WS_BORDER | TVS_HASLINES | TVS_SHOWSELALWAYS, edgeStyles );
	const auto list = AppendStandardTemplateItem(
		dialog, IDC_LIST_FL, WS_CHILD | WS_BORDER | LVS_REPORT, edgeStyles );

	ASSERT_TRUE( NormalizeWorkbenchOutlineDialogTemplate(dialog.data(), dialog.size()) );
	const DWORD treeStyle = ReadTemplateValue<DWORD>(dialog, tree.style);
	EXPECT_EQ(0u, treeStyle & (WS_BORDER | TVS_HASLINES | TVS_SHOWSELALWAYS));
	EXPECT_EQ(static_cast<DWORD>(TVS_HASBUTTONS | TVS_LINESATROOT | TVS_FULLROWSELECT),
		treeStyle & (TVS_HASBUTTONS | TVS_LINESATROOT | TVS_FULLROWSELECT));
	EXPECT_EQ(0u, ReadTemplateValue<DWORD>(dialog, tree.exStyle) & edgeStyles);
	EXPECT_EQ(0u, ReadTemplateValue<DWORD>(dialog, list.style) & WS_BORDER);
	EXPECT_EQ(0u, ReadTemplateValue<DWORD>(dialog, list.exStyle) & edgeStyles);
}

TEST(OutlineWorkbenchTool, MalformedDialogTemplateFailsClosed)
{
	std::vector<std::byte> malformed(4);
	EXPECT_FALSE( NormalizeWorkbenchOutlineDialogTemplate(malformed.data(), malformed.size()) );
}

TEST(OutlineParserWorker, OwnerThreadCloseJoinsNormallyAndIsIdempotent)
{
	OutlineParserWorker worker;
	worker.SetNotificationWindow(reinterpret_cast<HWND>(static_cast<std::uintptr_t>(1)), true);
	EXPECT_TRUE(worker.GetStateSnapshot().acceptingNotifications);

	// This call must return normally. The worker id is expected to differ from
	// the owner id; only the calling thread and self-join conditions are invalid.
	worker.Close();
	const auto state = worker.GetStateSnapshot();
	EXPECT_TRUE(state.closed);
	EXPECT_FALSE(state.active);
	EXPECT_FALSE(state.pending);
	EXPECT_FALSE(state.acceptingNotifications);
	worker.Close();
}

TEST(OutlineParserWorker, SameVersionDeduplicatesAndLatestPendingWins)
{
	// The active parse deliberately ignores cancellation in this test.  That
	// barrier makes the v2 -> v3 pending replacement happen before v1 can exit;
	// Close has a separate cancellation-responsive parser test below.
	BlockingOutlineParser parser(false);
	OutlineParserWorker worker(parser.Function());
	worker.SetNotificationWindow(reinterpret_cast<HWND>(static_cast<std::uintptr_t>(1)), true);
	const auto first = MakeWorkerSnapshot(41, 1);
	const auto second = MakeWorkerSnapshot(41, 2);
	const auto latest = MakeWorkerSnapshot(41, 3);

	EXPECT_EQ(OutlineWorkerRequestStatus::Started,
		worker.Submit(first, OUTLINE_CPP, OUTLINE_CPP, 11).status);
	const auto firstState = worker.GetStateSnapshot();
	EXPECT_EQ(1u, firstState.active ? firstState.activeGeneration : firstState.pendingGeneration);
	ASSERT_TRUE(parser.WaitForCalls(1));
	EXPECT_EQ(OutlineWorkerRequestStatus::ActiveDeduplicated,
		worker.Submit(first, OUTLINE_CPP, OUTLINE_CPP, 11).status);
	EXPECT_EQ(OutlineWorkerRequestStatus::Queued,
		worker.Submit(second, OUTLINE_CPP, OUTLINE_CPP, 12).status);
	EXPECT_EQ(OutlineWorkerRequestStatus::PendingReplaced,
		worker.Submit(latest, OUTLINE_CPP, OUTLINE_CPP, 13).status);

	const auto queued = worker.GetStateSnapshot();
	EXPECT_TRUE(queued.active);
	EXPECT_TRUE(queued.pending);
	EXPECT_EQ(latest->documentVersion, queued.pendingVersion);
	EXPECT_FALSE(queued.resultPending);
	EXPECT_FALSE(queued.resultMessagePosted);
	parser.Release();
	ASSERT_TRUE(parser.WaitForCalls(2));
	ASSERT_TRUE(WaitForWorkerIdle(worker));

	const auto done = worker.GetStateSnapshot();
	EXPECT_EQ(2u, done.startedCount);
	EXPECT_EQ(1u, done.cancelledCount);
	EXPECT_EQ(1u, done.completedCount);
	EXPECT_EQ(1u, done.supersededCount);
	EXPECT_FALSE(done.active);
	EXPECT_FALSE(done.pending);
	EXPECT_FALSE(done.resultPending);
	worker.Close();
}

TEST(OutlineParserWorker, CloseCancelsRunningAndPendingJobsBeforeReturning)
{
	BlockingOutlineParser parser;
	OutlineParserWorker worker(parser.Function());
	worker.SetNotificationWindow(reinterpret_cast<HWND>(static_cast<std::uintptr_t>(1)), true);
	EXPECT_EQ(OutlineWorkerRequestStatus::Started,
		worker.Submit(MakeWorkerSnapshot(52, 1), OUTLINE_CPP, OUTLINE_CPP, 0).status);
	ASSERT_TRUE(parser.WaitForCalls(1));
	EXPECT_EQ(OutlineWorkerRequestStatus::Queued,
		worker.Submit(MakeWorkerSnapshot(52, 2), OUTLINE_CPP, OUTLINE_CPP, 0).status);
	worker.Close();

	const auto state = worker.GetStateSnapshot();
	EXPECT_TRUE(state.closed);
	EXPECT_FALSE(state.active);
	EXPECT_FALSE(state.pending);
	EXPECT_FALSE(state.resultPending);
	EXPECT_EQ(0u, state.completedCount);
}

TEST(OutlineParserWorker, EditCancelsObsoleteActiveWorkBeforeDebounceFires)
{
	BlockingOutlineParser parser;
	OutlineParserWorker worker(parser.Function());
	worker.SetNotificationWindow(reinterpret_cast<HWND>(static_cast<std::uintptr_t>(1)), true);
	EXPECT_EQ(OutlineWorkerRequestStatus::Started,
		worker.Submit(MakeWorkerSnapshot(55, 1), OUTLINE_CPP, OUTLINE_CPP, 0).status);
	ASSERT_TRUE(parser.WaitForCalls(1));

	const auto cancellation = worker.CancelObsolete({55, 2});
	EXPECT_TRUE(cancellation.activeCancelled);
	EXPECT_FALSE(cancellation.pendingDiscarded);
	ASSERT_TRUE(WaitForWorkerIdle(worker));
	const auto state = worker.GetStateSnapshot();
	EXPECT_EQ(1u, state.cancelledCount);
	EXPECT_EQ(0u, state.completedCount);
	EXPECT_FALSE(state.resultPending);
	worker.Close();
}

TEST(OutlineParserWorker, EditDiscardsObsoletePendingWorkWithoutStartingIt)
{
	BlockingOutlineParser parser;
	OutlineParserWorker worker(parser.Function());
	worker.SetNotificationWindow(reinterpret_cast<HWND>(static_cast<std::uintptr_t>(1)), true);
	worker.SetPromotionPausedForTest(true);
	EXPECT_EQ(OutlineWorkerRequestStatus::Started,
		worker.Submit(MakeWorkerSnapshot(56, 1), OUTLINE_CPP, OUTLINE_CPP, 0).status);

	const auto cancellation = worker.CancelObsolete({56, 2});
	EXPECT_FALSE(cancellation.activeCancelled);
	EXPECT_TRUE(cancellation.pendingDiscarded);
	const auto state = worker.GetStateSnapshot();
	EXPECT_FALSE(state.active);
	EXPECT_FALSE(state.pending);
	EXPECT_EQ(0u, state.startedCount);
	EXPECT_EQ(1u, state.supersededCount);
	worker.SetPromotionPausedForTest(false);
	worker.Close();
}

TEST(OutlineParserWorker, PendingOnlyReplacementIsLatestWinsAndCounted)
{
	BlockingOutlineParser parser(false);
	OutlineParserWorker worker(parser.Function());
	worker.SetNotificationWindow(reinterpret_cast<HWND>(static_cast<std::uintptr_t>(1)), true);
	worker.SetPromotionPausedForTest(true);

	const auto first = worker.Submit(MakeWorkerSnapshot(57, 1), OUTLINE_CPP, OUTLINE_CPP, 0);
	ASSERT_EQ(OutlineWorkerRequestStatus::Started, first.status);
	ASSERT_EQ(1u, first.generation);
	const auto latest = worker.Submit(MakeWorkerSnapshot(57, 2), OUTLINE_CPP, OUTLINE_CPP, 0);
	EXPECT_EQ(OutlineWorkerRequestStatus::PendingReplaced, latest.status);
	EXPECT_EQ(2u, latest.generation);
	const auto pending = worker.GetStateSnapshot();
	EXPECT_FALSE(pending.active);
	EXPECT_TRUE(pending.pending);
	EXPECT_EQ(2u, pending.pendingGeneration);
	EXPECT_EQ(1u, pending.supersededCount);

	worker.SetPromotionPausedForTest(false);
	ASSERT_TRUE(parser.WaitForCalls(1));
	parser.Release();
	ASSERT_TRUE(WaitForWorkerIdle(worker));
	worker.Close();
}

TEST(OutlineParserWorker, GenerationExhaustionFailsClosedWithoutZeroGeneration)
{
	OutlineParserWorker worker;
	worker.SetNotificationWindow(reinterpret_cast<HWND>(static_cast<std::uintptr_t>(1)), true);
	worker.SetNextGenerationForTest((std::numeric_limits<std::uint64_t>::max)());
	const auto submission = worker.Submit(
		MakeWorkerSnapshot(58, 1), OUTLINE_CPP, OUTLINE_CPP, 0);
	EXPECT_EQ(OutlineWorkerRequestStatus::GenerationExhausted, submission.status);
	EXPECT_EQ(0u, submission.generation);
	const auto state = worker.GetStateSnapshot();
	EXPECT_FALSE(state.active);
	EXPECT_FALSE(state.pending);
	worker.Close();
}

TEST(OutlineParserWorker, CloseDropsPostedResultAndKeepsDeliveryBounded)
{
	MessageOnlyWindow target;
	ASSERT_NE(nullptr, target.Get());
	OutlineParserWorker worker([](
		const OutlineDocumentSnapshot& snapshot,
		int outlineType,
		int listType,
		const OutlineParserWorker::CancelToken& ) {
			OutlineParseResult result;
			result.documentVersion = snapshot.documentVersion;
			result.outlineType = outlineType;
			result.listType = listType;
			return result;
		});
	worker.SetNotificationWindow(target.Get(), true);
	EXPECT_EQ(OutlineWorkerRequestStatus::Started,
		worker.Submit(MakeWorkerSnapshot(63, 1), OUTLINE_CPP, OUTLINE_CPP, 0).status);
	ASSERT_TRUE(WaitForPostedResult(worker));
	const auto posted = worker.GetStateSnapshot();
	EXPECT_TRUE(posted.resultPending);
	EXPECT_TRUE(posted.resultMessagePosted);
	worker.Close();
	target.Drain();
	EXPECT_FALSE(worker.TakePendingResult(nullptr));
}

TEST(OutlineParserWorker, LatestFailureIsDeliveredAsOneTerminalOutcome)
{
	MessageOnlyWindow target;
	ASSERT_NE(nullptr, target.Get());
	OutlineParserWorker worker([](
		const OutlineDocumentSnapshot&, int, int,
		const OutlineParserWorker::CancelToken& ) -> OutlineParseResult {
			throw std::runtime_error("fixture failure");
		});
	worker.SetNotificationWindow(target.Get(), true);
	const auto submission = worker.Submit(
		MakeWorkerSnapshot(69, 1), OUTLINE_CPP, OUTLINE_CPP, 7);
	ASSERT_EQ(OutlineWorkerRequestStatus::Started, submission.status);
	ASSERT_TRUE(WaitForPostedResult(worker));
	const auto state = worker.GetStateSnapshot();
	EXPECT_EQ(1u, state.failedCount);
	EXPECT_EQ(OutlineWorkerTerminal::Failed, state.lastTerminal);
	const auto result = worker.TakePendingResult(nullptr);
	ASSERT_NE(nullptr, result);
	EXPECT_EQ(OutlineWorkerTerminal::Failed, result->terminal);
	EXPECT_EQ(submission.generation, result->generation);
	worker.Close();
}

TEST(OutlineParserWorker, SnapshotParserReturnsLogicalPositionsWithoutLayoutFence)
{
	OutlineDocumentSnapshot snapshot;
	snapshot.documentVersion = { 74, 1 };
	snapshot.filePath = L"fixture.cpp";
	snapshot.cppAnonymousName = L"<anonymous>";
	snapshot.cppDefinitionPosition = L"<definition>";
	snapshot.javaDefinitionPosition = L"<definition>";
	snapshot.appendText.emplace(FL_OBJ_CLASS, L" class");
	ASSERT_TRUE(snapshot.AppendLine(L"class Sample {\r\n"));
	ASSERT_TRUE(snapshot.AppendLine(L"\r\n"));
	ASSERT_TRUE(snapshot.AppendLine(L"\r\n"));
	ASSERT_TRUE(snapshot.AppendLine(L"\tvoid Method() {}\r\n"));
	ASSERT_TRUE(snapshot.AppendLine(L"};"));
	ASSERT_TRUE(snapshot.IsValid());
	ASSERT_EQ(5, snapshot.LineCount());
	for( const auto& span : snapshot.lineSpans ) {
		ASSERT_LT(span.offset + span.length, snapshot.textWithEol.size());
		EXPECT_EQ(L'\0', snapshot.textWithEol[span.offset + span.length]);
	}
	const auto cppResult = OutlineParserWorker::ParseSnapshot(
		snapshot, OUTLINE_CPP, OUTLINE_CPP, std::make_shared<std::atomic_bool>(false));
	ASSERT_EQ(2u, cppResult.symbols.size());
	EXPECT_EQ(1, cppResult.symbols[0].logicalLine);
	EXPECT_EQ(1, cppResult.symbols[0].logicalColumn);
	EXPECT_EQ(L"Sample", cppResult.symbols[0].name);
	EXPECT_EQ(FL_OBJ_CLASS, cppResult.symbols[0].info);
	EXPECT_EQ(0, cppResult.symbols[0].depth);
	EXPECT_EQ(4, cppResult.symbols[1].logicalLine);
	EXPECT_EQ(1, cppResult.symbols[1].logicalColumn);
	EXPECT_EQ(L"Sample::Method", cppResult.symbols[1].name);
	EXPECT_EQ(FL_OBJ_FUNCTION, cppResult.symbols[1].info);
	EXPECT_EQ(0, cppResult.symbols[1].depth);
	EXPECT_EQ(L" class", cppResult.appendText.at(FL_OBJ_CLASS));
	EXPECT_EQ(0u, cppResult.timings.nativeCommitUs);

	OutlineDocumentSnapshot javaSnapshot;
	javaSnapshot.documentVersion = { 75, 1 };
	javaSnapshot.filePath = L"fixture.java";
	javaSnapshot.javaDefinitionPosition = L"<definition>";
	javaSnapshot.appendText.emplace(FL_OBJ_FUNCTION, L" function");
	ASSERT_TRUE(javaSnapshot.AppendLine(L"class Sample {\r\n"));
	ASSERT_TRUE(javaSnapshot.AppendLine(L"\r\n"));
	ASSERT_TRUE(javaSnapshot.AppendLine(L"\r\n"));
	ASSERT_TRUE(javaSnapshot.AppendLine(L"    void Method() {}\r\n"));
	ASSERT_TRUE(javaSnapshot.AppendLine(L"}"));
	const auto javaResult = OutlineParserWorker::ParseSnapshot(
		javaSnapshot, OUTLINE_JAVA, OUTLINE_JAVA, std::make_shared<std::atomic_bool>(false));
	ASSERT_EQ(2u, javaResult.symbols.size());
	EXPECT_EQ(1, javaResult.symbols[0].logicalLine);
	EXPECT_EQ(1, javaResult.symbols[0].logicalColumn);
	EXPECT_EQ(L"Sample::<definition>", javaResult.symbols[0].name);
	EXPECT_EQ(FL_OBJ_DEFINITION, javaResult.symbols[0].info);
	EXPECT_EQ(0, javaResult.symbols[0].depth);
	EXPECT_EQ(4, javaResult.symbols[1].logicalLine);
	EXPECT_EQ(1, javaResult.symbols[1].logicalColumn);
	EXPECT_EQ(L"Sample::Method", javaResult.symbols[1].name);
	EXPECT_EQ(FL_OBJ_FUNCTION, javaResult.symbols[1].info);
	EXPECT_EQ(0, javaResult.symbols[1].depth);
	EXPECT_EQ(L" function", javaResult.appendText.at(FL_OBJ_FUNCTION));
}

TEST(OutlineDocumentSnapshot, RejectsMalformedRangesAndPreservesEmptyLineBoundaries)
{
	OutlineDocumentSnapshot snapshot;
	snapshot.documentVersion = { 81, 1 };
	ASSERT_TRUE(snapshot.AppendLine(L"first\r\n"));
	ASSERT_TRUE(snapshot.AppendLine(L""));
	ASSERT_TRUE(snapshot.AppendLine(L""));
	ASSERT_TRUE(snapshot.AppendLine(L"last"));
	ASSERT_TRUE(snapshot.IsValid());
	ASSERT_EQ(4, snapshot.LineCount());
	EXPECT_LT(snapshot.lineSpans[0].offset + snapshot.lineSpans[0].length,
		snapshot.lineSpans[1].offset);
	EXPECT_EQ(snapshot.lineSpans[1].offset + 1, snapshot.lineSpans[2].offset);

	auto outOfRange = snapshot;
	outOfRange.lineSpans[0].offset = outOfRange.textWithEol.size();
	EXPECT_FALSE(outOfRange.IsValid());

	auto missingSeparator = snapshot;
	ASSERT_FALSE(missingSeparator.textWithEol.empty());
	missingSeparator.textWithEol.pop_back();
	EXPECT_FALSE(missingSeparator.IsValid());

	auto tooLong = snapshot;
	tooLong.lineSpans[0].length = static_cast<std::size_t>((std::numeric_limits<int>::max)()) + 1u;
	EXPECT_FALSE(tooLong.IsValid());
	EXPECT_THROW(
		(void)OutlineParserWorker::ParseSnapshot(
			tooLong, OUTLINE_CPP, OUTLINE_CPP, std::make_shared<std::atomic_bool>(false)),
		std::invalid_argument);

	OutlineDocumentSnapshot empty;
	empty.documentVersion = { 82, 1 };
	EXPECT_TRUE(empty.IsValid());
	EXPECT_EQ(0, empty.LineCount());
	const auto emptyResult = OutlineParserWorker::ParseSnapshot(
		empty, OUTLINE_JAVA, OUTLINE_JAVA, std::make_shared<std::atomic_bool>(false));
	EXPECT_TRUE(emptyResult.symbols.empty());
}

} // namespace
} // namespace workbench::outline

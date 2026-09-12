/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#include "pch.h"
#include <gtest/gtest.h>
#include "workbench/viewcontainer/SenpViewContainer.h"
#include "workbench/viewcontainer/ViewContainerPagePool.h"
#include "workbench/layout/WorkbenchLayoutStateService.h"
#include <CommCtrl.h>
#include <oleacc.h>
#include <wrl/client.h>
#include <UIAutomation.h>

namespace workbench::viewcontainer {
namespace {
using namespace layout;
using Microsoft::WRL::ComPtr;

void Pump()
{
	MSG message{};
	for (int count = 0; count < 1000 && ::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE); ++count) {
		::TranslateMessage(&message); ::DispatchMessageW(&message);
	}
}
class NativeViewTestBody final : public ISenpViewBody {
public:
	explicit NativeViewTestBody(SenpViewBodyHost host) : m_host(std::move(host))
	{
		m_window = ::CreateWindowExW(0, L"EDIT", L"Retained issue body\r\nSecond line\r\nThird line", WS_CHILD | WS_TABSTOP | ES_MULTILINE | ES_AUTOVSCROLL,
			0, 0, 0, 0, m_host.parent, nullptr, ::GetModuleHandleW(nullptr), nullptr);
		if (m_window) ::SetWindowSubclass(m_window, Procedure, 1, reinterpret_cast<DWORD_PTR>(this));
	}
	~NativeViewTestBody() override { Close(); }
	HWND Window() const noexcept override { return m_window; }
	void Layout(const RECT& bounds, unsigned int) noexcept override
	{ ::SetWindowPos(m_window, nullptr, bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top, SWP_NOACTIVATE | SWP_NOZORDER); }
	void SetVisible(bool visible) noexcept override { if (m_window) ::ShowWindow(m_window, visible ? SW_SHOWNOACTIVATE : SW_HIDE); }
	void SetPalette(const theme::ThemePalette&, EViewContainerLocation location) noexcept override { m_location = location; }
	EViewContainerLocation Location() const noexcept { return m_location; }
	bool Focus() noexcept override { ::SetFocus(m_window); return ::GetFocus() == m_window; }
	bool PreTranslate(MSG&) noexcept override { return false; }
	void Close() noexcept override { m_host.interactionChanged = {}; m_host.projectionFailed = {}; if (m_window) ::DestroyWindow(m_window); m_window = nullptr; }
private:
	static LRESULT CALLBACK Procedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR data)
	{
		auto& body = *reinterpret_cast<NativeViewTestBody*>(data);
		const auto result = ::DefSubclassProc(window, message, wParam, lParam);
		if (message == WM_NCDESTROY) { body.m_window = nullptr; ::RemoveWindowSubclass(window, Procedure, 1); }
		if ((message == WM_SETFOCUS || message == WM_KILLFOCUS || message == WM_MOUSEMOVE || message == WM_MOUSELEAVE) && body.m_host.interactionChanged) {
			if (message == WM_MOUSEMOVE) { TRACKMOUSEEVENT track{ sizeof(track), TME_LEAVE, window, 0 }; ::TrackMouseEvent(&track); }
			body.m_host.interactionChanged();
		}
		return result;
	}
	HWND m_window{};
	SenpViewBodyHost m_host;
	EViewContainerLocation m_location{ EViewContainerLocation::Sidebar };
};

class SenpViewContainer : public testing::Test {
protected:
	HRESULT com = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
	HWND window{}, left{}, right{};
	std::shared_ptr<CSenpViewContainers> owner;
	ViewContainerPageRegistry pages;
	std::unique_ptr<ViewContainerPagePool> pool;
	std::unique_ptr<WorkbenchLayoutStateService> model;
	std::string focusedView, commandView, command;
	int factories{};
	std::vector<NativeViewTestBody*> nativeBodies;
	bool allowFocus{ true };
	bool probeDone{}, longTitles{};
	unsigned int probeOperation{};
	static constexpr UINT kProbeMessage = WM_APP + 0x296;
	static constexpr UINT_PTR kFrameProbeId = 0x298;
	// What WatchFrames observed: the frames the watched windows actually
	// published, and where the page sat when its root became visible (#298).
	int paints{};
	RECT shown{};
	void SetUp() override
	{
		ASSERT_TRUE(SUCCEEDED(com) || com == RPC_E_CHANGED_MODE);
		window = ::CreateWindowExW(0, L"STATIC", L"SENP native ViewContainer verification", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
			100, 80, 820, 700, nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
		ASSERT_NE(nullptr, window);
		left = ::CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, 0, 0, 380, 620, window, nullptr, ::GetModuleHandleW(nullptr), nullptr);
		right = ::CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, 400, 0, 380, 620, window, nullptr, ::GetModuleHandleW(nullptr), nullptr);
	}
	SenpViewContainerOptions Options()
	{
		SenpViewContainerOptions options;
		options.parkingParent = window; options.owner = { "test.github", 1 };
		for (const auto* id : { "test.issues", "test.actions" })
			options.containers.push_back({ id, id, EViewContainerLocation::Sidebar, 0, "github", false,
				{ EViewContainerLocation::Sidebar, EViewContainerLocation::AuxiliaryBar, EViewContainerLocation::Panel } });
		for (const auto* id : { "test.issueList", "test.pullRequests", "test.runs" }) {
			SenpNativeViewDefinition view;
			view.descriptor = { id, id == std::string_view("test.runs") ? "test.actions" : "test.issues", id, static_cast<int>(options.views.size()), true, true, "test.native" };
			if (longTitles) view.descriptor.title = "A long native View title that must ellipsize before its title actions and retain its accessible name";
			view.createBody = [this](SenpViewBodyHost host) {
				++factories; auto body = std::make_unique<NativeViewTestBody>(std::move(host));
				nativeBodies.push_back(body.get()); return body;
			};
			view.actions = { { "test.refresh", L"Refresh", L"refresh", true }, { "test.disabled", L"Disabled", L"close", false } };
			options.views.push_back(std::move(view));
		}
		options.requestFocus = [this](std::string_view id) { focusedView = id; return allowFocus; };
		options.execute = [this](std::string_view id, std::string_view action) { commandView = id; command = action; return true; };
		return options;
	}
	void Create(bool visible = false)
	{
		auto options = Options();
		WorkbenchContributionRegistry registry;
		std::vector<WorkbenchViewDescriptor> views;
		for (const auto& view : options.views) views.push_back(view.descriptor);
		ASSERT_TRUE(registry.RegisterExtensionContributions(options.containers, views));
		model = std::make_unique<WorkbenchLayoutStateService>(registry.Snapshot());
		owner = CSenpViewContainers::Create(std::move(options));
		ASSERT_NE(nullptr, owner);
		ASSERT_TRUE(pages.RegisterBatch(owner->PageDescriptors()).Succeeded());
		pool = std::make_unique<ViewContainerPagePool>(pages);
		for (const auto* id : { "test.issues", "test.actions" }) {
			const auto parent = id == std::string_view("test.issues") ? left : right;
			ASSERT_TRUE(pool->Attach(id, { id, EViewContainerLocation::Sidebar, reinterpret_cast<ViewContainerNativeHandle>(parent) }).Succeeded());
			auto* projection = Projection(id);
			ASSERT_NE(nullptr, projection);
			projection->LayoutProjection({ 0, 0, 380, 620 }, { 0, 0, 380, 620 }, 96);
			projection->ActivateProjection(); projection->SetProjectionVisible(true);
		}
		ASSERT_EQ(ESenpViewProjectionStatus::Applied, owner->ApplyLayout(model->Snapshot()));
		if (visible) { ::ShowWindow(window, SW_SHOWNORMAL); ::SetForegroundWindow(window); Pump(); }
	}
	IViewContainerPageProjection* Projection(std::string_view id)
	{ return dynamic_cast<IViewContainerPageProjection*>(pool->Acquire(id).page); }
	// The visual probe has no CEditWnd, so it stands in for the workbench frame
	// commit: a container only reserves its repaint (#298), and something has to
	// publish the finished frame before the capture reads the screen.
	void CommitFrame() { ::RedrawWindow(window, nullptr, nullptr, RDW_UPDATENOW | RDW_ALLCHILDREN); }
	// Watches one page and its container root. The windows belong to the owner,
	// which destroys them in TearDown, so the subclass needs no separate removal.
	void WatchFrames(const SenpViewPaneSnapshot& page)
	{
		::SetWindowSubclass(::GetParent(page.pane), ProbeProcedure, kFrameProbeId, reinterpret_cast<DWORD_PTR>(this));
		::SetWindowSubclass(page.pane, ProbeProcedure, kFrameProbeId, reinterpret_cast<DWORD_PTR>(this));
	}
	static LRESULT CALLBACK ProbeProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR data)
	{
		auto& fixture = *reinterpret_cast<SenpViewContainer*>(data);
		if (message == WM_PAINT) ++fixture.paints;
		if (message == WM_SHOWWINDOW && wParam && fixture.owner) {
			// A page must already sit at its final geometry when its container root
			// becomes visible, so record where it is at that exact moment (#298).
			const auto page = fixture.owner->Snapshot("test.issueList");
			if (page && window == ::GetParent(page->pane)) ::GetWindowRect(page->pane, &fixture.shown);
		}
		if (message != kProbeMessage) return ::DefSubclassProc(window, message, wParam, lParam);
		switch (wParam) {
		case 0: return fixture.owner->IsUsable();
		case 1: {
			const int width = LOWORD(lParam); const unsigned int dpi = HIWORD(lParam) & 0x3ff;
			const unsigned int themeId = HIWORD(lParam) >> 10;
			if (width < 40 || width > 380 || dpi < 96 || dpi > 192 || themeId > 2) return 0;
			auto palette = theme::CThemeService::PaletteFor(themeId == 1 ? theme::ThemeMode::Light : theme::ThemeMode::Dark);
			if (themeId == 2) palette = theme::CThemeService::HighContrastPalette();
			for (const auto* id : { "test.issues", "test.actions" }) {
				fixture.Projection(id)->SetProjectionPalette(palette);
				fixture.Projection(id)->LayoutProjection({ 0, 0, width, 620 }, { 0, 0, width, 620 }, dpi);
			}
			fixture.CommitFrame();
			return fixture.owner->IsUsable();
		}
		case 2: {
			auto changed = fixture.model->MoveView({ { "probe.move." + std::to_string(++fixture.probeOperation) }, "test.issueList", lParam ? "test.actions" : "test.issues", 0 });
			const bool applied = fixture.owner->ApplyLayout(changed.snapshot) == ESenpViewProjectionStatus::Applied;
			fixture.CommitFrame();
			return applied;
		}
		case 3: {
			const HWND issueHost = lParam ? fixture.right : fixture.left;
			const HWND actionHost = lParam ? fixture.left : fixture.right;
			const bool attached = fixture.pool->Attach("test.issues", { "probe.issues", lParam ? EViewContainerLocation::AuxiliaryBar : EViewContainerLocation::Sidebar, reinterpret_cast<ViewContainerNativeHandle>(issueHost) }).Succeeded()
				&& fixture.pool->Attach("test.actions", { "probe.actions", lParam ? EViewContainerLocation::Sidebar : EViewContainerLocation::AuxiliaryBar, reinterpret_cast<ViewContainerNativeHandle>(actionHost) }).Succeeded();
			fixture.CommitFrame();
			return attached;
		}
		case 4: fixture.probeDone = true; return 1;
		case 5: return reinterpret_cast<LRESULT>(fixture.owner->Snapshot("test.issueList")->body);
		case 6: return reinterpret_cast<LRESULT>(fixture.owner->Snapshot("test.pullRequests")->header);
		case 7: return reinterpret_cast<LRESULT>(fixture.owner->Snapshot("test.issueList")->header);
		default: return 0;
		}
	}
	void TearDown() override
	{
		pool.reset(); if (owner) owner->Close(); owner.reset();
		if (::IsWindow(window)) ::DestroyWindow(window);
		Pump();
		EXPECT_FALSE(::IsWindow(window)); EXPECT_FALSE(::IsWindow(left)); EXPECT_FALSE(::IsWindow(right));
		if (SUCCEEDED(com)) ::CoUninitialize();
	}
};

TEST(ViewPaneStackLayout, SharesSurplusAndKeepsEveryCollapsedHeaderReachable)
{
	const std::array<ViewPaneSize, 3> sizes{ { { false, 180, 120 }, { true, 900, 120 }, { false, 300, 120 } } };
	const auto layout = BuildViewPaneStackLayout(sizes, 600, 22);
	ASSERT_TRUE(layout.valid); EXPECT_EQ(600, layout.contentHeight);
	EXPECT_EQ(22, layout.panes[1].bottom - layout.panes[1].top);
	EXPECT_GT(layout.panes[2].bottom - layout.panes[2].headerBottom, layout.panes[0].bottom - layout.panes[0].headerBottom);
	const auto narrow = BuildViewPaneStackLayout(sizes, 10, 22);
	EXPECT_EQ(306, narrow.contentHeight);
	EXPECT_FALSE(BuildViewPaneStackLayout(sizes, -1, 22).valid);
	EXPECT_FALSE(BuildViewPaneStackLayout(sizes, 600, 0).valid);
}

TEST_F(SenpViewContainer, InvalidBatchAndMissingBodyNeverPublishPartialResources)
{
	auto options = Options(); options.views[1].createBody = {};
	EXPECT_EQ(nullptr, CSenpViewContainers::Create(std::move(options))); EXPECT_EQ(0, factories);
	options = Options(); options.views[1].createBody = [](SenpViewBodyHost) -> std::unique_ptr<ISenpViewBody> { throw std::runtime_error("body creation failure"); };
	EXPECT_EQ(nullptr, CSenpViewContainers::Create(std::move(options))); EXPECT_EQ(1, factories);
	int children{}; for (auto child = ::GetWindow(window, GW_CHILD); child; child = ::GetWindow(child, GW_HWNDNEXT)) ++children;
	EXPECT_EQ(2, children);
	options = Options(); options.views[1].descriptor.id = options.views[0].descriptor.id;
	EXPECT_EQ(nullptr, CSenpViewContainers::Create(std::move(options)));
}

TEST_F(SenpViewContainer, CollapseAndVisibilityRetainIndependentNativeBodyState)
{
	Create(); ASSERT_NE(nullptr, owner);
	const auto first = owner->Snapshot("test.issueList").value();
	const auto second = owner->Snapshot("test.pullRequests").value();
	::SendMessageW(first.body, EM_SETSEL, 3, 11);
	ASSERT_TRUE(owner->SetCollapsed(first.viewId, true));
	EXPECT_FALSE((::GetWindowLongPtrW(first.body, GWL_STYLE) & WS_VISIBLE) != 0);
	EXPECT_FALSE(owner->Snapshot(second.viewId)->collapsed);
	ASSERT_TRUE(owner->SetCollapsed(first.viewId, false));
	DWORD start{}, end{}; ::SendMessageW(first.body, EM_GETSEL, reinterpret_cast<WPARAM>(&start), reinterpret_cast<LPARAM>(&end));
	EXPECT_EQ(3, start); EXPECT_EQ(11, end); EXPECT_EQ(first.body, owner->Snapshot(first.viewId)->body);
	auto hidden = model->SetViewVisibility({ { "hide" }, first.viewId, false }); ASSERT_EQ(EWorkbenchLayoutOperationStatus::Succeeded, hidden.status);
	EXPECT_EQ(ESenpViewProjectionStatus::Applied, owner->ApplyLayout(hidden.snapshot));
	EXPECT_FALSE(owner->Snapshot(first.viewId)->visible); EXPECT_TRUE(owner->Snapshot(second.viewId)->visible);
	auto restored = model->SetViewVisibility({ { "show" }, first.viewId, true }); ASSERT_EQ(EWorkbenchLayoutOperationStatus::Succeeded, restored.status);
	EXPECT_EQ(ESenpViewProjectionStatus::Applied, owner->ApplyLayout(restored.snapshot));
	EXPECT_EQ(first.body, owner->Snapshot(first.viewId)->body); EXPECT_EQ(3, factories);
}

TEST_F(SenpViewContainer, RealLayoutViewMoveSurvivesClosingItsOriginalContainer)
{
	Create(); ASSERT_NE(nullptr, owner);
	const auto first = owner->Snapshot("test.issueList").value();
	const auto oldParent = ::GetParent(first.pane);
	ASSERT_TRUE(owner->SetCollapsed(first.viewId, true));
	auto moved = model->MoveView({ { "move" }, first.viewId, "test.actions", 5 }); ASSERT_EQ(EWorkbenchLayoutOperationStatus::Succeeded, moved.status);
	ASSERT_EQ(ESenpViewProjectionStatus::Applied, owner->ApplyLayout(moved.snapshot));
	EXPECT_NE(oldParent, ::GetParent(first.pane)); EXPECT_TRUE(owner->Snapshot(first.viewId)->collapsed);
	EXPECT_EQ(EViewContainerPagePoolCloseStatus::Closed, pool->Close("test.issues").status);
	EXPECT_TRUE(::IsWindow(first.body)); EXPECT_TRUE(owner->SetCollapsed(first.viewId, false));
	EXPECT_EQ(first.body, owner->Snapshot(first.viewId)->body);
	auto descriptors = owner->PageDescriptors(); owner->Close();
	EXPECT_FALSE(::IsWindow(first.body)); EXPECT_FALSE(::IsWindow(first.pane));
	for (auto& descriptor : descriptors) EXPECT_EQ(nullptr, descriptor.factory());
}

TEST_F(SenpViewContainer, InvalidAndConflictingSnapshotsKeepExistingParents)
{
	Create(); ASSERT_NE(nullptr, owner);
	const auto pane = owner->Snapshot("test.issueList")->pane;
	const auto parent = ::GetParent(pane);
	auto invalid = model->Snapshot();
	invalid.views.push_back(*std::ranges::find(invalid.views, "test.issueList", &WorkbenchViewState::viewId));
	EXPECT_EQ(ESenpViewProjectionStatus::Invalid, owner->ApplyLayout(invalid));
	invalid = model->Snapshot(); std::ranges::find(invalid.views, "test.issueList", &WorkbenchViewState::viewId)->containerId = "other.extension";
	EXPECT_EQ(ESenpViewProjectionStatus::Unsupported, owner->ApplyLayout(invalid));
	invalid = model->Snapshot(); ++invalid.generation;
	EXPECT_EQ(ESenpViewProjectionStatus::Conflict, owner->ApplyLayout(invalid));
	EXPECT_EQ(parent, ::GetParent(pane)); EXPECT_TRUE(owner->IsUsable());
}

TEST_F(SenpViewContainer, HeaderKeyboardAndBodyFocusRouteToTheActualView)
{
	Create(true); ASSERT_NE(nullptr, owner);
	const auto first = owner->Snapshot("test.issueList").value();
	const auto second = owner->Snapshot("test.pullRequests").value();
	ASSERT_TRUE(owner->FocusView(first.viewId)); Pump(); EXPECT_EQ(first.viewId, focusedView);
	ASSERT_TRUE(owner->SetCollapsed(first.viewId, true)); EXPECT_EQ(first.header, ::GetFocus());
	::SendMessageW(first.header, WM_KEYDOWN, VK_RIGHT, 0); EXPECT_FALSE(owner->Snapshot(first.viewId)->collapsed);
	::SendMessageW(first.header, WM_KEYDOWN, VK_SPACE, 0); ::SendMessageW(first.header, WM_KEYUP, VK_SPACE, 0);
	EXPECT_TRUE(owner->Snapshot(first.viewId)->collapsed);
	::SendMessageW(first.header, WM_KEYDOWN, VK_RETURN, 0); EXPECT_FALSE(owner->Snapshot(first.viewId)->collapsed);
	::SendMessageW(first.header, WM_KEYDOWN, VK_DOWN, 0); EXPECT_EQ(second.header, ::GetFocus());
	::SendMessageW(second.header, WM_KEYDOWN, VK_UP, 0); EXPECT_EQ(first.header, ::GetFocus());
	allowFocus = false;
	::SendMessageW(first.header, WM_KEYDOWN, VK_LEFT, 0); EXPECT_FALSE(owner->Snapshot(first.viewId)->collapsed);
	allowFocus = true;
	Projection("test.issues")->LayoutProjection({ 0, 0, 80, 10 }, { 0, 0, 80, 10 }, 96);
	::SendMessageW(first.header, WM_KEYDOWN, VK_DOWN, 0); EXPECT_EQ(second.header, ::GetFocus());
	EXPECT_TRUE(owner->IsUsable());
	Pump();
}

TEST_F(SenpViewContainer, RetainedPagePoolMoveRestoresTheSameFocusedChild)
{
	Create(true); ASSERT_NE(nullptr, owner);
	const auto first = owner->Snapshot("test.issueList").value();
	ASSERT_TRUE(owner->FocusView(first.viewId)); Pump();
	auto* page = pool->Acquire("test.issues").page;
	const auto token = page->CaptureFocusToken(); ASSERT_EQ(EViewContainerFocusCaptureStatus::Captured, token.status);
	ASSERT_TRUE(pool->Attach("test.issues", { "secondary", EViewContainerLocation::AuxiliaryBar, reinterpret_cast<ViewContainerNativeHandle>(right) }).Succeeded());
	EXPECT_EQ(first.body, ::GetFocus()); EXPECT_EQ(first.body, owner->Snapshot(first.viewId)->body);
	EXPECT_EQ(right, ::GetParent(::GetParent(first.pane)));
	owner->Close(); EXPECT_EQ(EViewContainerFocusRestoreStatus::Failed, page->RestoreFocusToken(*token.token));
}

TEST_F(SenpViewContainer, TitleActionsRespectEnabledStateAndRestoreFocusWhenRemoved)
{
	Create(true); ASSERT_NE(nullptr, owner);
	const auto first = owner->Snapshot("test.issueList").value();
	::SetFocus(first.header); Pump();
	const auto refresh = ::GetDlgItem(first.pane, 2);
	ASSERT_TRUE(::IsWindowVisible(refresh));
	::SendMessageW(refresh, BM_CLICK, 0, 0); EXPECT_EQ("test.refresh", command); EXPECT_EQ(first.viewId, commandView);
	command.clear(); ::SendMessageW(first.pane, WM_COMMAND, MAKEWPARAM(3, BN_CLICKED), reinterpret_cast<LPARAM>(::GetDlgItem(first.pane, 3)));
	EXPECT_TRUE(command.empty());
	::SetFocus(refresh); ASSERT_EQ(refresh, ::GetFocus());
	EXPECT_EQ(ESenpViewProjectionStatus::Applied, owner->SetTitleActions(first.viewId, {})); EXPECT_EQ(first.header, ::GetFocus());
	EXPECT_EQ(ESenpViewProjectionStatus::Invalid, owner->SetTitleActions(first.viewId, { { "test.duplicate", L"One", L"refresh" }, { "test.duplicate", L"Two", L"refresh" } }));
}

TEST_F(SenpViewContainer, LayoutReservesItsRepaintAndShowsAPageOnlyAtItsFinalGeometry)
{
	Create(true); ASSERT_NE(nullptr, owner);
	const auto first = owner->Snapshot("test.issueList").value();
	const auto root = ::GetParent(first.pane);
	ASSERT_NE(nullptr, root);
	WatchFrames(first);
	auto* projection = Projection("test.issues");
	ASSERT_NE(nullptr, projection);

	// A relayout of a visible container must not publish a frame of its own.
	projection->LayoutProjection({ 0, 0, 300, 620 }, { 0, 0, 300, 620 }, 96);
	EXPECT_EQ(0, paints);
	EXPECT_TRUE(::GetUpdateRect(root, nullptr, FALSE));
	Pump(); EXPECT_GT(paints, 0);

	// A page switch lays the panes out first, so the page is never shown at its
	// previous geometry, and it publishes nothing until the message loop runs.
	projection->SetProjectionVisible(false); Pump();
	paints = 0; shown = RECT{};
	projection->LayoutProjection({ 0, 0, 380, 620 }, { 0, 0, 380, 620 }, 96);
	projection->SetProjectionVisible(true);
	EXPECT_EQ(0, paints);
	RECT settled{}; ::GetWindowRect(first.pane, &settled);
	EXPECT_EQ(settled.right - settled.left, shown.right - shown.left);
	EXPECT_TRUE(::IsWindowVisible(root));
	Pump(); EXPECT_GT(paints, 0);
	RECT afterPaint{}; ::GetWindowRect(first.pane, &afterPaint);
	EXPECT_EQ(settled.right - settled.left, afterPaint.right - afterPaint.left);
}

TEST_F(SenpViewContainer, PanelPlacementUpdatesBodyThemeContextWithoutReplacingIt)
{
	Create(); ASSERT_NE(nullptr, owner);
	const auto body = owner->Snapshot("test.issueList")->body;
	ASSERT_EQ(3, nativeBodies.size());
	ASSERT_TRUE(pool->Attach("test.issues", { "panel", EViewContainerLocation::Panel, reinterpret_cast<ViewContainerNativeHandle>(right) }).Succeeded());
	EXPECT_EQ(EViewContainerLocation::Panel, nativeBodies[0]->Location());
	EXPECT_EQ(EViewContainerLocation::Panel, nativeBodies[1]->Location());
	EXPECT_EQ(EViewContainerLocation::Sidebar, nativeBodies[2]->Location());
	EXPECT_EQ(body, owner->Snapshot("test.issueList")->body);
}

TEST_F(SenpViewContainer, MsaaHeaderReportsNameCollapseStateAndCachedLifetime)
{
	Create(true); ASSERT_NE(nullptr, owner);
	const auto first = owner->Snapshot("test.issueList").value();
	const WPARAM client = ::GetCurrentProcessId();
	const auto reference = ::SendMessageW(first.header, WM_GETOBJECT, client, OBJID_CLIENT);
	ASSERT_GT(reference, 0);
	ComPtr<IAccessible> accessible;
	ASSERT_HRESULT_SUCCEEDED(::ObjectFromLresult(reference, __uuidof(IAccessible), client, reinterpret_cast<void**>(accessible.GetAddressOf())));
	VARIANT self{}; self.vt = VT_I4; self.lVal = CHILDID_SELF;
	BSTR name{}; ASSERT_HRESULT_SUCCEEDED(accessible->get_accName(self, &name)); EXPECT_STREQ(L"test.issueList", name); ::SysFreeString(name);
	VARIANT state{}; ASSERT_HRESULT_SUCCEEDED(accessible->get_accState(self, &state)); EXPECT_NE(0, state.lVal & STATE_SYSTEM_EXPANDED);
	ASSERT_HRESULT_SUCCEEDED(accessible->accDoDefaultAction(self)); EXPECT_TRUE(owner->Snapshot(first.viewId)->collapsed);
	ASSERT_HRESULT_SUCCEEDED(accessible->get_accState(self, &state)); EXPECT_NE(0, state.lVal & STATE_SYSTEM_COLLAPSED);
	owner->Close(); EXPECT_TRUE(FAILED(accessible->accDoDefaultAction(self)));
}

TEST_F(SenpViewContainer, UiaExpandCollapsePatternTracksStateAndRevokesCachedProvider)
{
	Create(true); ASSERT_NE(nullptr, owner);
	const auto first = owner->Snapshot("test.issueList").value();
	ComPtr<IUIAutomation> automation;
	ASSERT_HRESULT_SUCCEEDED(::CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER,
		__uuidof(IUIAutomation), reinterpret_cast<void**>(automation.GetAddressOf())));
	ComPtr<IUIAutomationElement> element;
	ASSERT_HRESULT_SUCCEEDED(automation->ElementFromHandle(first.header, &element));
	ComPtr<IUIAutomationExpandCollapsePattern> pattern;
	ASSERT_HRESULT_SUCCEEDED(element->GetCurrentPatternAs(UIA_ExpandCollapsePatternId,
		__uuidof(IUIAutomationExpandCollapsePattern), reinterpret_cast<void**>(pattern.GetAddressOf())));
	ASSERT_NE(nullptr, pattern);
	ExpandCollapseState state{};
	ASSERT_HRESULT_SUCCEEDED(pattern->get_CurrentExpandCollapseState(&state)); EXPECT_EQ(ExpandCollapseState_Expanded, state);
	ASSERT_HRESULT_SUCCEEDED(pattern->Collapse()); EXPECT_TRUE(owner->Snapshot(first.viewId)->collapsed);
	ASSERT_HRESULT_SUCCEEDED(pattern->get_CurrentExpandCollapseState(&state)); EXPECT_EQ(ExpandCollapseState_Collapsed, state);
	ASSERT_HRESULT_SUCCEEDED(pattern->Expand()); EXPECT_FALSE(owner->Snapshot(first.viewId)->collapsed);
	owner->Close(); EXPECT_TRUE(FAILED(pattern->Expand()));
}

TEST_F(SenpViewContainer, DpiNarrowClientsAndUnexpectedNativeDestructionEndExplicitly)
{
	Create(); ASSERT_NE(nullptr, owner);
	const auto first = owner->Snapshot("test.issueList").value();
	for (const auto dpi : { 96u, 144u, 192u }) {
		Projection("test.issues")->LayoutProjection({ 0, 0, 90, 260 }, { 0, 0, 90, 260 }, dpi);
		RECT header{}; ::GetWindowRect(first.header, &header); EXPECT_EQ(::MulDiv(22, dpi, 96), header.bottom - header.top);
		EXPECT_TRUE(owner->IsUsable());
	}
	::DestroyWindow(first.pane); EXPECT_FALSE(owner->IsUsable());
	EXPECT_EQ(ESenpViewProjectionStatus::Stopped, owner->ApplyLayout(model->Snapshot()));
	owner->Close(); EXPECT_FALSE(::IsWindow(first.body));
}

// Explicit external dual-capture probe, excluded from ordinary tests. The
// canonical rendering script owns this one PID; this loop also has a deadline.
TEST_F(SenpViewContainer, DISABLED_VisualCaptureProbe)
{
	wchar_t enabled[2]{};
	if (::GetEnvironmentVariableW(L"SAKURA_SENP_VIEW_PROBE", enabled, 2) != 1 || enabled[0] != L'1') GTEST_SKIP() << "Use tools/verify-senp-view-rendering.ps1";
	longTitles = true; Create(true); ASSERT_NE(nullptr, owner);
	ASSERT_TRUE(::SetWindowSubclass(window, ProbeProcedure, 296, reinterpret_cast<DWORD_PTR>(this)));
	const auto deadline = ::GetTickCount64() + 120000;
	while (!probeDone && ::GetTickCount64() < deadline && owner->IsUsable()) {
		(void)::MsgWaitForMultipleObjectsEx(0, nullptr, 50, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
		Pump();
	}
	::RemoveWindowSubclass(window, ProbeProcedure, 296);
	EXPECT_TRUE(probeDone); EXPECT_TRUE(owner->IsUsable());
}

} // namespace
} // namespace workbench::viewcontainer

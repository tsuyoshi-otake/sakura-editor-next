/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#include "pch.h"
#include <gtest/gtest.h>
#include "workbench/tree/SenpTreeView.h"
#include "workbench/SenpDeclaredTreeViews.h"
#include "workbench/SenpExtensionActivation.h"
#include "workbench/viewcontainer/ViewContainerPagePool.h"
#include "workbench/layout/WorkbenchLayoutStateService.h"
#include <CommCtrl.h>
#include <oleacc.h>
#include <UIAutomation.h>
#include <wrl/client.h>

namespace workbench::tree {
namespace {
using namespace viewcontainer;
using namespace layout;
using Microsoft::WRL::ComPtr;
void DispatchTreeMessages()
{
	MSG message{};
	for (int i = 0; i < 1000 && ::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE); ++i) {
		::TranslateMessage(&message); ::DispatchMessageW(&message);
	}
}
class NativeTreeRuntime final : public ISenpTreeRuntime {
public:
	struct Call { senp::effect::TreeRequest request; senp::effect::OperationContext context; };
	std::vector<Call> calls;
	std::vector<senp::effect::OperationContext> cancelled;
	std::vector<senp::effect::CommandInvoked> commands;
	bool IsCurrent() const noexcept override { return true; }
	bool CanSubmit() const noexcept override { return true; }
	SenpTreeAdmission Submit(senp::effect::TreeRequest request, SenpTreeProvider::Time) noexcept override
	{
		const auto sequence = static_cast<std::int64_t>(calls.size() + 1);
		senp::effect::OperationContext context{ L"s1:o" + std::to_wstring(sequence), 1, 2, 3, sequence };
		calls.push_back({ std::move(request), context }); return { senp::AdmissionStatus::Accepted, context };
	}
	void Cancel(const senp::effect::OperationContext& context) noexcept override { cancelled.push_back(context); }
	bool Execute(senp::effect::CommandInvoked command) noexcept override { commands.push_back(std::move(command)); return true; }
};
senp::effect::TreeItem NativeItem(std::wstring id, bool branch = false, bool command = false)
{
	senp::effect::TreeItem item; item.id = std::move(id); item.label = item.id; item.description = L"Description";
	item.icon = branch ? L"folder" : L"issues"; item.tooltip = L"Full tooltip for " + item.id;
	item.collapsibleState = branch ? senp::effect::CollapsibleState::Collapsed : senp::effect::CollapsibleState::Leaf;
	if (command) { item.commandId = L"test.open"; item.arguments = { item.id }; }
	return item;
}
class SenpTreeView : public testing::Test {
protected:
	HRESULT com{ ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED) };
	HWND window{}, left{}, right{}, tree{};
	std::shared_ptr<NativeTreeRuntime> runtime = std::make_shared<NativeTreeRuntime>();
	std::shared_ptr<SenpTreeProvider> provider = std::make_shared<SenpTreeProvider>(SenpTreeProviderOptions{ L"test.tree", { 1, 2, 3 }, { L"test.open" }, runtime });
	CSenpTreeView* body{};
	std::shared_ptr<CSenpViewContainers> owner;
	ViewContainerPageRegistry pages;
	std::unique_ptr<ViewContainerPagePool> pool;
	bool singleClick{ true };
	bool probeDone{};
	std::int64_t probeRevision{ 1 };
	std::vector<senp::effect::TreeItem> ProbeItems(bool changed = false)
	{
		std::vector<senp::effect::TreeItem> result{ NativeItem(L"group", true) };
		result[0].label = L"Assigned to me"; result[0].description = L"50";
		for (int i = 0; i < 50; ++i) {
			auto item = NativeItem(L"issue" + std::to_wstring(i), false, true);
			item.label = (changed ? L"Updated: " : L"") + std::wstring(i % 2 ? L"Preserve selection while a delayed tree page refreshes" : L"Native detail view with bounded loading and retry");
			item.description = L"#" + std::to_wstring(1200 + i) + L"  open"; result.push_back(std::move(item));
		}
		return result;
	}
	LRESULT NativeFingerprint()
	{
		std::uint64_t hash = 2166136261;
		const auto first = TreeView_GetFirstVisible(tree);
		for (auto item = first; item; item = TreeView_GetNextVisible(tree, item)) {
			for (const auto letter : Text(item)) hash = (hash ^ letter) * 16777619;
			hash ^= TreeView_GetItemState(tree, item, TVIS_EXPANDED | TVIS_SELECTED) & (TVIS_EXPANDED | TVIS_SELECTED);
		}
		return static_cast<LRESULT>((hash & 0x7fffffffffffffff) | 1);
	}
	static LRESULT CALLBACK ProbeProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR data)
	{
		if (message != WM_APP + 0x296) return ::DefSubclassProc(window, message, wParam, lParam);
		auto& fixture = *reinterpret_cast<SenpTreeView*>(data);
		switch (wParam) {
		case 0: return fixture.owner->IsUsable() && fixture.body->IsUsable();
		case 1: {
			const int width = LOWORD(lParam); const unsigned int dpi = HIWORD(lParam) & 0x3ff, themeId = HIWORD(lParam) >> 10;
			if (width < 40 || width > 380 || dpi < 96 || dpi > 192 || themeId > 2) return 0;
			auto palette = theme::CThemeService::PaletteFor(themeId == 1 ? theme::ThemeMode::Light : theme::ThemeMode::Dark);
			if (themeId == 2) palette = theme::CThemeService::HighContrastPalette();
			fixture.Projection()->SetProjectionPalette(palette);
			fixture.Projection()->LayoutProjection({ 0, 0, width, 620 }, { 0, 0, width, 620 }, dpi); DispatchTreeMessages(); return fixture.body->IsUsable();
		}
		case 2: {
			const auto before = fixture.runtime->calls.size();
			TreeView_Expand(fixture.tree, TreeView_GetRoot(fixture.tree), lParam ? TVE_EXPAND : TVE_COLLAPSE); DispatchTreeMessages();
			if (fixture.runtime->calls.size() > before) fixture.Complete({ NativeItem(L"nested1", false, true), NativeItem(L"nested2", false, true) });
			return fixture.body->IsUsable();
		}
		case 3: {
			auto item = TreeView_GetRoot(fixture.tree);
			if (lParam) for (int i = 0; i < 30 && item; ++i) item = TreeView_GetNextVisible(fixture.tree, item);
			if (!item) return 0;
			TreeView_SelectSetFirstVisible(fixture.tree, item); ::SendMessageW(fixture.tree, WM_VSCROLL, SB_ENDSCROLL, 0); DispatchTreeMessages(); return 1;
		}
		case 4: fixture.probeDone = true; return 1;
		case 5: return reinterpret_cast<LRESULT>(fixture.body->Window());
		case 7: return reinterpret_cast<LRESULT>(fixture.owner->Snapshot("test.tree")->header);
		case 8: fixture.provider->Refresh(std::chrono::steady_clock::now()); fixture.Complete(fixture.ProbeItems(lParam != 0), L"", ++fixture.probeRevision); return fixture.body->IsUsable();
		case 9: DispatchTreeMessages(); return fixture.NativeFingerprint();
		default: return 0;
		}
	}
	void SetUp() override
	{
		ASSERT_TRUE(SUCCEEDED(com) || com == RPC_E_CHANGED_MODE);
		window = ::CreateWindowExW(0, L"STATIC", L"SENP native TreeView verification", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
			100, 80, 820, 700, nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
		ASSERT_NE(nullptr, window);
		left = ::CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, 0, 0, 380, 620, window, nullptr, ::GetModuleHandleW(nullptr), nullptr);
		right = ::CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, 400, 0, 380, 620, window, nullptr, ::GetModuleHandleW(nullptr), nullptr);
	}
	IViewContainerPageProjection* Projection() { return dynamic_cast<IViewContainerPageProjection*>(pool->Acquire("test.container").page); }
	void Create(bool show = false)
	{
		SenpViewContainerOptions options; options.parkingParent = window; options.owner = { "test.extension", 1 };
		options.containers = { { "test.container", "Tree", EViewContainerLocation::Sidebar, 0, "github", false,
			{ EViewContainerLocation::Sidebar, EViewContainerLocation::AuxiliaryBar, EViewContainerLocation::Panel } } };
		SenpNativeViewDefinition view;
		view.descriptor = { "test.tree", "test.container", "Issues and pull requests", 0, true, true, "test.tree" };
		view.createBody = [this](SenpViewBodyHost host) {
			auto result = CSenpTreeView::Create({ std::move(host), provider, L"Issues and pull requests", singleClick }); body = result.get(); return result;
		};
		view.actions = { { "test.refresh", L"Refresh", L"refresh", true } }; options.views.push_back(std::move(view));
		options.requestFocus = [](std::string_view) { return true; };
		options.execute = [this](std::string_view, std::string_view) { provider->Refresh(std::chrono::steady_clock::now()); return true; };
		WorkbenchContributionRegistry registry; const std::array views{ options.views[0].descriptor };
		ASSERT_TRUE(registry.RegisterExtensionContributions(options.containers, views));
		WorkbenchLayoutStateService model(registry.Snapshot());
		owner = CSenpViewContainers::Create(std::move(options)); ASSERT_NE(nullptr, owner);
		ASSERT_TRUE(pages.RegisterBatch(owner->PageDescriptors()).Succeeded()); pool = std::make_unique<ViewContainerPagePool>(pages);
		ASSERT_TRUE(pool->Attach("test.container", { "initial", EViewContainerLocation::Sidebar, reinterpret_cast<ViewContainerNativeHandle>(left) }).Succeeded());
		ASSERT_NE(nullptr, Projection()); Projection()->LayoutProjection({ 0, 0, 380, 620 }, { 0, 0, 380, 620 }, 96);
		Projection()->ActivateProjection(); Projection()->SetProjectionVisible(true);
		ASSERT_EQ(ESenpViewProjectionStatus::Applied, owner->ApplyLayout(model.Snapshot()));
		tree = body->TreeWindow(); ASSERT_NE(nullptr, tree); DispatchTreeMessages();
		if (show) { ::ShowWindow(window, SW_SHOWNORMAL); ::ShowWindow(window, SW_SHOWNORMAL); ::SetForegroundWindow(window); DispatchTreeMessages(); }
	}
	void Complete(std::vector<senp::effect::TreeItem> items, std::wstring cursor = L"", std::int64_t revision = 1)
	{
		ASSERT_FALSE(runtime->calls.empty()); const auto call = runtime->calls.back();
		senp::effect::PublishTreePage page{ L"test.tree", call.request.parentId, std::move(items), std::move(cursor), revision, senp::effect::PageStatus::Complete, L"" };
		if (!page.nextCursor.empty()) page.status = senp::effect::PageStatus::Partial;
		ASSERT_EQ(TreeResult::Applied, provider->Apply(call.context, std::move(page), std::chrono::steady_clock::now())); DispatchTreeMessages();
	}
	std::wstring Text(HTREEITEM item)
	{
		wchar_t buffer[4096]{}; TVITEMW value{}; value.mask = TVIF_TEXT; value.hItem = item; value.pszText = buffer; value.cchTextMax = _countof(buffer);
		EXPECT_TRUE(TreeView_GetItem(tree, &value)); return buffer;
	}
	void Key(UINT key) { ::SendMessageW(tree, WM_KEYDOWN, key, 0); DispatchTreeMessages(); }
	void Click(HTREEITEM item, bool complete = true)
	{
		RECT rect{}; ASSERT_TRUE(TreeView_GetItemRect(tree, item, &rect, TRUE)); const auto point = MAKELPARAM(rect.left + 4, (rect.top + rect.bottom) / 2);
		// Native TreeView enters a drag-detection message loop on button down.
		// Queue the whole gesture before dispatch so its terminal is available.
		::PostMessageW(tree, WM_LBUTTONDOWN, MK_LBUTTON, point);
		if (!complete) ::PostMessageW(tree, WM_CANCELMODE, 0, 0);
		::PostMessageW(tree, WM_LBUTTONUP, 0, point);
		DispatchTreeMessages();
	}
	void TearDown() override
	{
		pool.reset(); if (owner) owner->Close(); owner.reset(); provider->Close();
		if (::IsWindow(window)) ::DestroyWindow(window); DispatchTreeMessages();
		EXPECT_FALSE(::IsWindow(window)); EXPECT_FALSE(::IsWindow(tree)); if (SUCCEEDED(com)) ::CoUninitialize();
	}
};
TEST_F(SenpTreeView, LazyNativeHierarchyKeepsLeafAndCommandSemantics)
{
	Create(); ASSERT_NE(nullptr, body); ASSERT_EQ(1, runtime->calls.size()); EXPECT_EQ(L"Loading...", Text(TreeView_GetRoot(tree)));
	Complete({ NativeItem(L"folder", true), NativeItem(L"command", true, true), NativeItem(L"leaf", false, true) });
	const auto folder = TreeView_GetRoot(tree), command = TreeView_GetNextSibling(tree, folder), leaf = TreeView_GetNextSibling(tree, command);
	TreeView_SelectItem(tree, folder); Key(VK_RIGHT); ASSERT_EQ(2, runtime->calls.size()); EXPECT_EQ(L"folder", runtime->calls.back().request.parentId);
	EXPECT_EQ(L"Loading...", Text(TreeView_GetChild(tree, folder))); Complete({ NativeItem(L"child", false, true) });
	EXPECT_TRUE(TreeView_GetItemState(tree, folder, TVIS_EXPANDED) & TVIS_EXPANDED);
	TreeView_SelectItem(tree, leaf); EXPECT_EQ(L"leaf", provider->Model().Selection()); EXPECT_TRUE(runtime->commands.empty());
	Key(VK_RETURN); ASSERT_EQ(1, runtime->commands.size()); EXPECT_EQ(L"leaf", runtime->commands.back().arguments[0]);
	Click(command); ASSERT_EQ(2, runtime->commands.size()); EXPECT_FALSE(provider->Model().Node(L"command")->expanded); EXPECT_EQ(2, runtime->calls.size());
	Click(folder); EXPECT_FALSE(provider->Model().Node(L"folder")->expanded);
	const auto child = TreeView_GetChild(tree, folder);
	for (int repeat = 0; repeat < 4; ++repeat) {
		TreeView_Expand(tree, folder, TVE_EXPAND); DispatchTreeMessages(); EXPECT_TRUE(provider->Model().Node(L"folder")->expanded);
		EXPECT_TRUE(TreeView_GetItemState(tree, folder, TVIS_EXPANDED) & TVIS_EXPANDED);
		TreeView_Expand(tree, folder, TVE_COLLAPSE); DispatchTreeMessages(); EXPECT_FALSE(provider->Model().Node(L"folder")->expanded);
		EXPECT_FALSE(TreeView_GetItemState(tree, folder, TVIS_EXPANDED) & TVIS_EXPANDED); EXPECT_EQ(child, TreeView_GetChild(tree, folder));
	}
	EXPECT_EQ(2, runtime->calls.size());
}
TEST_F(SenpTreeView, PageAndRetryAreNativeActionsWithExplicitTerminals)
{
	Create(); Complete({ NativeItem(L"one") }, L"page2");
	auto more = TreeView_GetNextSibling(tree, TreeView_GetRoot(tree)); ASSERT_NE(nullptr, more); EXPECT_EQ(L"Load more...", Text(more));
	TreeView_SelectItem(tree, more); Key(VK_RETURN); ASSERT_EQ(2, runtime->calls.size()); EXPECT_EQ(L"page2", runtime->calls.back().request.cursor);
	ASSERT_EQ(TreeResult::Applied, provider->Failed(runtime->calls.back().context, senp::InvocationStatus::TimedOut, std::chrono::steady_clock::now())); DispatchTreeMessages();
	auto retry = TreeView_GetNextSibling(tree, TreeView_GetRoot(tree)); ASSERT_NE(nullptr, retry); EXPECT_EQ(0, Text(retry).find(L"Retry"));
	TreeView_SelectItem(tree, retry); Key(VK_RETURN); ASSERT_EQ(3, runtime->calls.size()); EXPECT_EQ(L"page2", runtime->calls.back().request.cursor);
	Complete({ NativeItem(L"two") }); EXPECT_EQ(2, TreeView_GetCount(tree)); EXPECT_EQ(L"one - Description", Text(TreeView_GetRoot(tree)));
}
TEST_F(SenpTreeView, CollapseAndHideCancelSubscribersAndNeverAcceptTheirLatePage)
{
	Create(); Complete({ NativeItem(L"branch", true) }); TreeView_SelectItem(tree, TreeView_GetRoot(tree)); Key(VK_RIGHT); const auto pending = runtime->calls.back();
	Key(VK_LEFT); EXPECT_EQ(1, runtime->cancelled.size()); EXPECT_EQ(0, provider->Model().PendingCount());
	senp::effect::PublishTreePage late{ L"test.tree", L"branch", { NativeItem(L"late") }, L"", 1, senp::effect::PageStatus::Complete, L"" };
	EXPECT_EQ(TreeResult::Stale, provider->Apply(pending.context, late, std::chrono::steady_clock::now()));
	Key(VK_RIGHT); ASSERT_EQ(3, runtime->calls.size()); ASSERT_TRUE(owner->SetCollapsed("test.tree", true)); DispatchTreeMessages();
	EXPECT_EQ(2, runtime->cancelled.size()); EXPECT_FALSE(provider->IsVisible());
	ASSERT_TRUE(owner->SetCollapsed("test.tree", false)); DispatchTreeMessages(); EXPECT_EQ(4, runtime->calls.size());
}
TEST_F(SenpTreeView, StableNativeSelectionFocusAndScrollSurviveRefreshAndContainerMove)
{
	Create(true); std::vector<senp::effect::TreeItem> items;
	for (int i = 0; i < 70; ++i) items.push_back(NativeItem(L"item" + std::to_wstring(i), false, true));
	Complete(items); ASSERT_TRUE(body->Focus());
	auto selected = TreeView_GetRoot(tree); for (int i = 0; i < 40; ++i) selected = TreeView_GetNextSibling(tree, selected);
	TreeView_SelectItem(tree, selected); TreeView_SelectSetFirstVisible(tree, selected); DispatchTreeMessages();
	const auto anchor = TreeView_GetFirstVisible(tree); const auto originalTree = tree; const auto selectedId = provider->Model().Selection();
	provider->Refresh(std::chrono::steady_clock::now()); DispatchTreeMessages(); items[0].label = L"Updated"; Complete(items, L"", 2);
	EXPECT_EQ(selected, TreeView_GetSelection(tree)); EXPECT_EQ(anchor, TreeView_GetFirstVisible(tree)); EXPECT_EQ(selectedId, provider->Model().Selection()); EXPECT_EQ(tree, ::GetFocus());
	ASSERT_TRUE(pool->Attach("test.container", { "moved", EViewContainerLocation::AuxiliaryBar, reinterpret_cast<ViewContainerNativeHandle>(right) }).Succeeded());
	Projection()->LayoutProjection({ 0, 0, 380, 620 }, { 0, 0, 380, 620 }, 144); DispatchTreeMessages();
	EXPECT_EQ(originalTree, body->TreeWindow()); EXPECT_EQ(selected, TreeView_GetSelection(tree)); EXPECT_EQ(tree, ::GetFocus()); EXPECT_EQ(33, TreeView_GetItemHeight(tree));
}
TEST_F(SenpTreeView, InterruptedPointerAndDoubleClickModeDoNotAccidentallyOpenOrExpand)
{
	singleClick = false; Create(); Complete({ NativeItem(L"branch", true), NativeItem(L"command", true, true) });
	const auto branch = TreeView_GetRoot(tree), command = TreeView_GetNextSibling(tree, branch);
	Click(command, false); EXPECT_TRUE(runtime->commands.empty()); Click(branch); EXPECT_FALSE(provider->Model().Node(L"branch")->expanded); EXPECT_EQ(1, runtime->calls.size());
	TreeView_SelectItem(tree, branch); Key(VK_RETURN); EXPECT_TRUE(provider->Model().Node(L"branch")->expanded); EXPECT_EQ(2, runtime->calls.size());
}
TEST_F(SenpTreeView, FailedSecondBodyDoesNotRevokeTheExistingObserver)
{
	Create(); auto second = CSenpTreeView::Create({ { right, {} }, provider, L"Conflicting owner" });
	EXPECT_EQ(nullptr, second); EXPECT_FALSE(provider->Model().IsClosed()); EXPECT_TRUE(body->IsUsable());
	Complete({ NativeItem(L"still-current") }); EXPECT_EQ(L"still-current - Description", Text(TreeView_GetRoot(tree)));
	const auto oldWindow = body->Window(); body->Layout({ 0, 0, -1, 20 }, 96);
	EXPECT_FALSE(body->IsUsable()); EXPECT_FALSE(::IsWindow(oldWindow)); EXPECT_TRUE(provider->Model().IsClosed()); EXPECT_FALSE(owner->IsUsable());
}
TEST_F(SenpTreeView, NativeAccessibilityReportsRealNamesSelectionAndExpandCollapse)
{
	Create(true); Complete({ NativeItem(L"branch", true), NativeItem(L"leaf") }); ASSERT_TRUE(body->Focus());
	ComPtr<IAccessible> accessible; ASSERT_EQ(S_OK, ::AccessibleObjectFromWindow(tree, OBJID_CLIENT, IID_PPV_ARGS(&accessible)));
	long children{}; ASSERT_EQ(S_OK, accessible->get_accChildCount(&children)); EXPECT_GE(children, 2);
	ComPtr<IUIAutomation> automation; ASSERT_EQ(S_OK, ::CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&automation)));
	ComPtr<IUIAutomationElement> root; ASSERT_EQ(S_OK, automation->ElementFromHandle(tree, &root));
	ComPtr<IUIAutomationTreeWalker> walker; ASSERT_EQ(S_OK, automation->get_ControlViewWalker(&walker));
	ComPtr<IUIAutomationElement> row; ASSERT_EQ(S_OK, walker->GetFirstChildElement(root.Get(), &row)); ASSERT_NE(nullptr, row.Get());
	BSTR name{}; ASSERT_EQ(S_OK, row->get_CurrentName(&name)); EXPECT_NE(std::wstring::npos, std::wstring(name).find(L"branch")); ::SysFreeString(name);
	ComPtr<IUIAutomationExpandCollapsePattern> expansion; ASSERT_EQ(S_OK, row->GetCurrentPatternAs(UIA_ExpandCollapsePatternId, IID_PPV_ARGS(&expansion)));
	ASSERT_EQ(S_OK, expansion->Expand()); DispatchTreeMessages(); EXPECT_TRUE(provider->Model().Node(L"branch")->expanded); EXPECT_EQ(2, runtime->calls.size());
	ExpandCollapseState state{}; ASSERT_EQ(S_OK, expansion->get_CurrentExpandCollapseState(&state)); EXPECT_EQ(ExpandCollapseState_Expanded, state);
	owner->Close(); EXPECT_TRUE(provider->Model().IsClosed()); EXPECT_FALSE(::IsWindow(tree));
}
TEST_F(SenpTreeView, DISABLED_VisualCaptureProbe)
{
	wchar_t enabled[2]{};
	if (::GetEnvironmentVariableW(L"SAKURA_SENP_VIEW_PROBE", enabled, 2) != 1 || enabled[0] != L'1') GTEST_SKIP() << "Use tools/verify-senp-view-rendering.ps1 -ProbeSet TreeViews";
	Create(true); ASSERT_NE(nullptr, owner); Complete(ProbeItems());
	ASSERT_TRUE(::SetWindowSubclass(window, ProbeProcedure, 296, reinterpret_cast<DWORD_PTR>(this)));
	const auto deadline = ::GetTickCount64() + 120000;
	while (!probeDone && ::GetTickCount64() < deadline && owner->IsUsable() && body->IsUsable()) {
		(void)::MsgWaitForMultipleObjectsEx(0, nullptr, 50, QS_ALLINPUT, MWMO_INPUTAVAILABLE); DispatchTreeMessages();
	}
	::RemoveWindowSubclass(window, ProbeProcedure, 296); EXPECT_TRUE(probeDone); EXPECT_TRUE(owner->IsUsable()); EXPECT_TRUE(body->IsUsable());
}
class SenpDeclaredTreeViewsTest : public SenpTreeView {
protected:
	std::shared_ptr<CSenpDeclaredTreeViews> declarations;
	std::unique_ptr<ISenpViewBody> declaredBody;
	std::unique_ptr<ISenpDeclaredTreePublication> visualPublication;
	std::int64_t visualGeneration{};
	int activationRequests{}, retryRequests{};
	void CreateDeclaration()
	{
		declarations = CSenpDeclaredTreeViews::Create(L"test.extension",
			{ { "test.tree", "test.container", "Projects", 0, true, true, "senp.tree" } },
			[this](std::wstring_view viewId, bool retry) {
				EXPECT_EQ(L"test.tree", viewId); ++activationRequests; if (retry) ++retryRequests;
				return SenpExtensionActivationState::Preparing;
			});
		ASSERT_NE(nullptr, declarations);
		declaredBody = declarations->CreateBody(L"test.tree", { left, {}, {} });
		ASSERT_NE(nullptr, declaredBody);
		declaredBody->Layout({ 0, 0, 380, 420 }, 96);
	}
	void TearDown() override
	{
		if (visualPublication) visualPublication->Close();
		visualPublication.reset();
		if (declarations) declarations->Close();
		declaredBody.reset(); declarations.reset(); SenpTreeView::TearDown();
	}
	std::wstring Status() const
	{
		wchar_t text[512]{}; ::GetWindowTextW(::GetDlgItem(declaredBody->Window(), 1), text, _countof(text)); return text;
	}
	static senp::ContributionOwnerIdentity Identity(std::int64_t generation)
	{ return { L"test.extension", std::wstring(64, L'a'), generation, 2, 3 }; }
	void BindVisual()
	{
		runtime = std::make_shared<NativeTreeRuntime>();
		provider = std::make_shared<SenpTreeProvider>(SenpTreeProviderOptions{ L"test.tree", { 1, 2, 3 }, { L"test.open" }, runtime });
		auto replacement = declarations->PrepareBinding(Identity(++visualGeneration), { { L"test.tree", provider } });
		ASSERT_NE(nullptr, replacement); ASSERT_TRUE(replacement->Commit());
		if (visualPublication) visualPublication->Close();
		visualPublication = std::move(replacement);
		ASSERT_TRUE(visualPublication->Pump());
		ASSERT_TRUE(declarations->Pump(SenpExtensionActivationState::Active));
		const HWND root = ::FindWindowExW(declaredBody->Window(), nullptr, L"SakuraSenpTreeView", nullptr);
		tree = ::FindWindowExW(root, nullptr, WC_TREEVIEWW, nullptr);
		ASSERT_NE(nullptr, tree); Complete(ProbeItems());
	}
	void UnbindVisual(SenpExtensionActivationState state)
	{
		if (visualPublication) visualPublication->Close();
		visualPublication.reset(); ASSERT_TRUE(declarations->Pump(state)); tree = nullptr;
	}
	LRESULT DeclarationFingerprint() const
	{
		std::uint64_t hash = 2166136261;
		for (const auto letter : Status()) hash = (hash ^ letter) * 16777619;
		hash ^= static_cast<std::uint64_t>(::IsWindowVisible(declaredBody->Window())) << 20;
		if (::IsWindow(tree)) hash ^= static_cast<std::uint64_t>(TreeView_GetCount(tree)) << 10;
		return static_cast<LRESULT>((hash & 0x7fffffffffffffff) | 1);
	}
	static LRESULT CALLBACK DeclarationProbe(HWND window, UINT message, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR data)
	{
		if (message != WM_APP + 0x296) return ::DefSubclassProc(window, message, w, l);
		auto& self = *reinterpret_cast<SenpDeclaredTreeViewsTest*>(data);
		switch (w) {
		case 0: return self.declarations->IsUsable();
		case 1: {
			const int width = LOWORD(l); const unsigned int dpi = HIWORD(l) & 0x3ff, themeId = HIWORD(l) >> 10;
			if (width < 40 || width > 380 || dpi < 96 || dpi > 192 || themeId > 2) return 0;
			auto palette = themeId == 2 ? theme::CThemeService::HighContrastPalette()
				: theme::CThemeService::PaletteFor(themeId == 1 ? theme::ThemeMode::Light : theme::ThemeMode::Dark);
			self.UnbindVisual(SenpExtensionActivationState::Preparing);
			self.declaredBody->SetPalette(palette, EViewContainerLocation::Sidebar);
			self.declaredBody->Layout({ 0, 0, width, 620 }, dpi); DispatchTreeMessages(); return self.declarations->IsUsable();
		}
		case 2: self.UnbindVisual(l ? SenpExtensionActivationState::Failed : SenpExtensionActivationState::Preparing); DispatchTreeMessages(); return 1;
		case 3: if (l) self.BindVisual(); else self.UnbindVisual(SenpExtensionActivationState::Failed); DispatchTreeMessages(); return 1;
		case 4: self.probeDone = true; return 1;
		case 5: return reinterpret_cast<LRESULT>(self.declaredBody->Window());
		case 7: return reinterpret_cast<LRESULT>(::GetDlgItem(self.declaredBody->Window(), 2));
		case 8: self.declaredBody->SetVisible(!l); DispatchTreeMessages(); return 1;
		case 9: return self.DeclarationFingerprint();
		}
		return 0;
	}
};

TEST_F(SenpDeclaredTreeViewsTest, ActivationIsPostedAndFailureRequiresExplicitRetry)
{
	CreateDeclaration(); ASSERT_NE(nullptr, declaredBody);
	EXPECT_EQ(0, activationRequests);
	declaredBody->SetVisible(true);
	EXPECT_EQ(0, activationRequests);
	DispatchTreeMessages();
	EXPECT_EQ(1, activationRequests);
	EXPECT_EQ(L"Activating extension...", Status());
	for (int i = 0; i < 5; ++i) { declaredBody->SetVisible(false); declaredBody->SetVisible(true); DispatchTreeMessages(); }
	EXPECT_EQ(1, activationRequests);
	ASSERT_TRUE(declarations->Pump(SenpExtensionActivationState::Failed));
	const HWND retry = ::GetDlgItem(declaredBody->Window(), 2);
	EXPECT_TRUE(::GetWindowLongPtrW(retry, GWL_STYLE) & WS_VISIBLE);
	EXPECT_TRUE(::IsWindowEnabled(retry));
	::SendMessageW(retry, BM_CLICK, 0, 0); ::SendMessageW(retry, BM_CLICK, 0, 0);
	EXPECT_FALSE(::IsWindowEnabled(retry));
	DispatchTreeMessages();
	EXPECT_EQ(2, activationRequests); EXPECT_EQ(1, retryRequests);
	EXPECT_EQ(L"Activating extension...", Status());
	ASSERT_TRUE(declarations->Pump(SenpExtensionActivationState::Failed));
	::SendMessageW(retry, BM_CLICK, 0, 0);
	declaredBody->SetVisible(false); DispatchTreeMessages();
	declaredBody->SetVisible(true); DispatchTreeMessages();
	EXPECT_EQ(2, activationRequests); EXPECT_TRUE(::IsWindowEnabled(retry));
	ASSERT_TRUE(declarations->Pump(SenpExtensionActivationState::Disabled));
	EXPECT_FALSE(::GetWindowLongPtrW(retry, GWL_STYLE) & WS_VISIBLE);
	const HWND retained = declaredBody->Window();
	declarations->Close(); DispatchTreeMessages();
	EXPECT_FALSE(::IsWindow(retained)); EXPECT_EQ(2, activationRequests);
	EXPECT_FALSE(declarations->Pump(SenpExtensionActivationState::Dormant));
}

TEST_F(SenpDeclaredTreeViewsTest, RuntimeReplacementDefersNativeSwapAndRetainsDeclarationBody)
{
	CreateDeclaration(); ASSERT_NE(nullptr, declaredBody);
	const HWND retained = declaredBody->Window();
	declaredBody->SetVisible(true); DispatchTreeMessages();
	auto initial = declarations->PrepareBinding(Identity(1), { { L"test.tree", provider } });
	ASSERT_NE(nullptr, initial); ASSERT_TRUE(initial->Commit());
	EXPECT_TRUE(runtime->calls.empty());
	ASSERT_TRUE(initial->Pump()); DispatchTreeMessages();
	ASSERT_EQ(1U, runtime->calls.size());
	const HWND firstTree = ::FindWindowExW(retained, nullptr, L"SakuraSenpTreeView", nullptr);
	ASSERT_NE(nullptr, firstTree);
	EXPECT_FALSE(declarations->PrepareBinding(Identity(1), { { L"test.tree", provider } }));
	auto newerRuntime = std::make_shared<NativeTreeRuntime>();
	auto newerProvider = std::make_shared<SenpTreeProvider>(SenpTreeProviderOptions{ L"test.tree", { 1, 2, 3 }, {}, newerRuntime });
	auto replacement = declarations->PrepareBinding(Identity(2), { { L"test.tree", newerProvider } });
	ASSERT_NE(nullptr, replacement); ASSERT_TRUE(replacement->Commit());
	initial->Close();
	EXPECT_TRUE(::IsWindow(firstTree)); EXPECT_TRUE(newerRuntime->calls.empty());
	ASSERT_TRUE(replacement->Pump()); DispatchTreeMessages();
	EXPECT_EQ(retained, declaredBody->Window()); EXPECT_FALSE(::IsWindow(firstTree));
	EXPECT_TRUE(provider->Model().IsClosed()); EXPECT_EQ(1U, runtime->cancelled.size());
	EXPECT_EQ(1U, newerRuntime->calls.size());
	const HWND secondTree = ::FindWindowExW(retained, nullptr, L"SakuraSenpTreeView", nullptr);
	ASSERT_NE(nullptr, secondTree);
	replacement->Close();
	EXPECT_TRUE(::IsWindow(secondTree));
	ASSERT_TRUE(declarations->Pump(SenpExtensionActivationState::Failed));
	EXPECT_FALSE(::IsWindow(secondTree)); EXPECT_TRUE(newerProvider->Model().IsClosed());
	EXPECT_EQ(retained, declaredBody->Window()); EXPECT_TRUE(::IsWindow(retained));
	EXPECT_EQ(L"The extension could not be activated.", Status());
	EXPECT_FALSE(replacement->Pump());
}

TEST_F(SenpDeclaredTreeViewsTest, LosingPreparedCandidateCannotCloseWinningRuntime)
{
	CreateDeclaration(); ASSERT_NE(nullptr, declaredBody);
	declaredBody->SetVisible(true); DispatchTreeMessages();
	auto winner = declarations->PrepareBinding(Identity(1), { { L"test.tree", provider } });
	ASSERT_NE(nullptr, winner);
	auto losingRuntime = std::make_shared<NativeTreeRuntime>();
	auto losingProvider = std::make_shared<SenpTreeProvider>(SenpTreeProviderOptions{ L"test.tree", { 1, 2, 3 }, {}, losingRuntime });
	auto loser = declarations->PrepareBinding(Identity(2), { { L"test.tree", losingProvider } });
	ASSERT_NE(nullptr, loser); ASSERT_TRUE(loser->CanCommit());
	ASSERT_TRUE(winner->Commit()); ASSERT_TRUE(winner->Pump()); DispatchTreeMessages();
	const HWND winningTree = ::FindWindowExW(declaredBody->Window(), nullptr, L"SakuraSenpTreeView", nullptr);
	ASSERT_NE(nullptr, winningTree);
	EXPECT_FALSE(loser->CanCommit()); EXPECT_FALSE(loser->Commit());
	loser->Close();
	EXPECT_TRUE(losingRuntime->calls.empty()); EXPECT_TRUE(losingProvider->Model().IsClosed());
	EXPECT_FALSE(provider->Model().IsClosed()); EXPECT_TRUE(runtime->cancelled.empty());
	EXPECT_TRUE(::IsWindow(winningTree)); EXPECT_TRUE(winner->Pump());
	EXPECT_EQ(1U, runtime->calls.size());
}

TEST_F(SenpDeclaredTreeViewsTest, DISABLED_VisualCaptureProbe)
{
	wchar_t enabled[2]{};
	if (::GetEnvironmentVariableW(L"SAKURA_SENP_VIEW_PROBE", enabled, 2) != 1 || enabled[0] != L'1')
		GTEST_SKIP() << "Use tools/verify-senp-view-rendering.ps1 -ProbeSet DeclaredTreeViews";
	CreateDeclaration(); ASSERT_NE(nullptr, declaredBody);
	::SetWindowTextW(window, L"SENP declared TreeView verification");
	declaredBody->SetVisible(true); DispatchTreeMessages();
	ASSERT_TRUE(::SetWindowSubclass(window, DeclarationProbe, 297, reinterpret_cast<DWORD_PTR>(this)));
	const auto deadline = ::GetTickCount64() + 120000;
	while (!probeDone && ::GetTickCount64() < deadline && declarations->IsUsable()) {
		(void)::MsgWaitForMultipleObjectsEx(0, nullptr, 50, QS_ALLINPUT, MWMO_INPUTAVAILABLE); DispatchTreeMessages();
	}
	::RemoveWindowSubclass(window, DeclarationProbe, 297);
	EXPECT_TRUE(probeDone); EXPECT_TRUE(declarations->IsUsable());
}

} // namespace
} // namespace workbench::tree

/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include "env/ShareDataTestSuite.hpp"
#include "extension/CExtensionViewRegistry.h"
#include "outline/CDlgFuncList.h"
#include "workbench/viewcontainer/CViewContainerHost.h"

#include <memory>

namespace {

using workbench::viewcontainer::ContributedViewContainer;
using workbench::viewcontainer::CViewContainerHost;
using workbench::viewcontainer::CViewContainerPages;
namespace pageIds = workbench::viewcontainer::pageIds;

/*!
	@brief Covers the model fact behind Issue #23 Phase 4's webview-Unsupported boundary.

	Root `CLAUDE.md` forbids rendering nothing for a capability this product cannot support:
	a `"type": "webview"` contributed View must show a visible, typed Unsupported state, never
	an empty tree indistinguishable from "this extension has not populated its view yet". These
	tests cover the model fact (`ContributedViewContainer::webviewOnly` ->
	`CViewContainerPages::IsWebviewOnly`) and its one consumer, `CViewContainerHost::LayoutChildren`,
	which is what decides whether the permanently empty contributed tree is ever shown. Pixel-level
	proof that `Paint` actually draws the unsupported message belongs to the manual
	PrintWindow/screen-capture protocol documented in root `CLAUDE.md`, not this suite.
*/
class WebviewUnsupportedViewContainer : public ::testing::Test, public env::ShareDataTestSuite {
protected:
	static void SetUpTestSuite()
	{
		SetUpShareData();
	}

	static void TearDownTestSuite()
	{
		TearDownShareData();
	}

	//! The Primary Side Bar hosts `workbench.view.extensions`, so the shared page pool always
	//! owns an extension View registry. These tests create no contributed tree items, but the
	//! registry must still be a real object rather than null so construction matches production.
	std::shared_ptr<CExtensionViewRegistry> m_views = std::make_shared<CExtensionViewRegistry>();

	std::shared_ptr<CViewContainerPages> MakePages(CDlgFuncList& dialog)
	{
		return std::make_shared<CViewContainerPages>(dialog, m_views);
	}

	//! A real, on-screen top-level window, matching `ExplorerToolTest`'s
	//! `CreatesVisibleContainerAndTreeForVisibleParent`. A hidden (non-`WS_VISIBLE`) parent
	//! would make every descendant's `::IsWindowVisible` report false regardless of its own
	//! style bit, which would hide the very distinction these tests exist to prove.
	static HWND CreateVisibleParentWindow()
	{
		return ::CreateWindowExW(0, L"STATIC", L"WebviewUnsupported test parent",
			WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, 800, 600,
			nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
	}
};

TEST_F(WebviewUnsupportedViewContainer, ReportsFalseForBuiltinAndUnknownContainersWithoutCreate)
{
	CDlgFuncList dialog;
	auto pages = MakePages(dialog);

	// Built-in pages never carry manifest-declared views, so they must never be reported as
	// webview-only even though the pool has not been `Create`d yet.
	EXPECT_FALSE(pages->IsWebviewOnly(pageIds::Explorer));
	EXPECT_FALSE(pages->IsWebviewOnly(pageIds::SourceControl));
	EXPECT_FALSE(pages->IsWebviewOnly(pageIds::Extensions));
	EXPECT_FALSE(pages->IsWebviewOnly("no.such.container"));
}

TEST_F(WebviewUnsupportedViewContainer, SyncMirrorsTheWebviewOnlyFlagAndReportsChangeOnFlip)
{
	CDlgFuncList dialog;
	auto pages = MakePages(dialog);
	const HWND parent = CreateVisibleParentWindow();
	ASSERT_NE(nullptr, parent);
	ASSERT_TRUE(pages->Create(parent));

	EXPECT_TRUE(pages->SyncContributedContainers({
		ContributedViewContainer{ .id = "claude.webview", .title = L"Claude Code", .webviewOnly = true },
		ContributedViewContainer{ .id = "acme.tree", .title = L"Acme Explorer", .webviewOnly = false },
	}));
	EXPECT_TRUE(pages->IsWebviewOnly("claude.webview"));
	EXPECT_FALSE(pages->IsWebviewOnly("acme.tree"));

	// Re-syncing the identical set must not report a change: an unrelated extension
	// re-registering the same containers must not disturb this one's state or force a
	// needless re-layout.
	EXPECT_FALSE(pages->SyncContributedContainers({
		ContributedViewContainer{ .id = "claude.webview", .title = L"Claude Code", .webviewOnly = true },
		ContributedViewContainer{ .id = "acme.tree", .title = L"Acme Explorer", .webviewOnly = false },
	}));

	// A later manifest re-registration can turn a tree container into a webview-only one (or
	// back). The pool must pick that up on the existing page, not only compute it once at
	// first creation.
	EXPECT_TRUE(pages->SyncContributedContainers({
		ContributedViewContainer{ .id = "claude.webview", .title = L"Claude Code", .webviewOnly = true },
		ContributedViewContainer{ .id = "acme.tree", .title = L"Acme Explorer", .webviewOnly = true },
	}));
	EXPECT_TRUE(pages->IsWebviewOnly("acme.tree"));

	pages->Close();
	::DestroyWindow(parent);
}

TEST_F(WebviewUnsupportedViewContainer, ShowingAWebviewOnlyContainerKeepsItsPermanentlyEmptyTreeHidden)
{
	CDlgFuncList dialog;
	auto pages = MakePages(dialog);
	const HWND parent = CreateVisibleParentWindow();
	ASSERT_NE(nullptr, parent);
	ASSERT_TRUE(pages->Create(parent));
	ASSERT_TRUE(pages->SyncContributedContainers({
		ContributedViewContainer{ .id = "claude.webview", .title = L"Claude Code", .webviewOnly = true },
		ContributedViewContainer{ .id = "acme.tree", .title = L"Acme Explorer", .webviewOnly = false },
	}));

	CViewContainerHost host(pages);
	ASSERT_TRUE(host.Create(parent));
	host.Layout(RECT{ 0, 0, 320, 480 }, 96);

	const HWND webviewTree = pages->ContributedViews("claude.webview")->GetHwnd();
	ASSERT_NE(nullptr, webviewTree);
	const HWND treeWindow = pages->ContributedViews("acme.tree")->GetHwnd();
	ASSERT_NE(nullptr, treeWindow);

	host.ShowPage("claude.webview");
	EXPECT_EQ("claude.webview", host.ActivePage());
	// The permanently empty tree must never be shown for a webview-only container: showing it
	// would be indistinguishable from a tree container an extension has not populated yet,
	// exactly the "fake it" defect root CLAUDE.md forbids. The fixed message is drawn directly
	// by the host instead (see `CViewContainerHost::Paint`, `kWebviewUnsupportedMessage`).
	EXPECT_FALSE(::IsWindowVisible(webviewTree));

	// A normal contributed tree container is unaffected: it still lays out and shows exactly
	// as it always has, proving this change is additive rather than a regression for the
	// existing tree-backed contributed containers.
	host.ShowPage("acme.tree");
	EXPECT_EQ("acme.tree", host.ActivePage());
	EXPECT_TRUE(::IsWindowVisible(treeWindow));
	EXPECT_FALSE(::IsWindowVisible(webviewTree));

	// Switching back must re-hide it, proving the suppression is not a one-shot creation-time
	// effect but is re-applied on every activation.
	host.ShowPage("claude.webview");
	EXPECT_FALSE(::IsWindowVisible(webviewTree));
	EXPECT_FALSE(::IsWindowVisible(treeWindow));

	host.Close();
	pages->Close();
	::DestroyWindow(parent);
}

} // namespace

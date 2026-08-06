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
#include "workbench/extension/CExtensionBottomPanelTool.h"
#include "workbench/win32/ProblemsOutputPanelProjection.h"

#include <memory>
#include <stdexcept>

namespace {

using workbench::viewcontainer::CViewContainerHost;
using workbench::viewcontainer::CViewContainerPages;
using workbench::extension::CExtensionBottomPanelTool;
using workbench::extension::ExtensionBottomPanelTab;

class NativeWorkbenchToolRequest : public ::testing::Test, public env::ShareDataTestSuite {
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
	//! owns an extension View registry. These tests create no HWND, but the registry must
	//! still be a real object rather than null so construction matches production.
	std::shared_ptr<CExtensionViewRegistry> m_views = std::make_shared<CExtensionViewRegistry>();

	//! A side-bar host renders pages it borrows from the shared pool, so a test host is
	//! only meaningful together with one. The pool is deliberately not `Create`d: these
	//! tests cover the request/projection contract, which owns no window.
	std::shared_ptr<CViewContainerPages> MakePages(CDlgFuncList& dialog)
	{
		return std::make_shared<CViewContainerPages>(dialog, m_views);
	}
};

TEST_F(NativeWorkbenchToolRequest, OutlineRequestCallsOwnerBeforeApplyingNativeState)
{
	CDlgFuncList dialog;
	auto pages = MakePages(dialog);
	CViewContainerHost* observedHost = nullptr;
	int callbackCalls = 0;
	CViewContainerHost host(pages, [&](bool expanded) {
		++callbackCalls;
		EXPECT_TRUE(expanded);
		EXPECT_NE(nullptr, observedHost);
		EXPECT_FALSE(observedHost->IsOutlineExpanded());
		return true;
	});
	observedHost = &host;

	host.SetOutlineExpanded(false);

	EXPECT_TRUE(host.RequestOutlineExpanded(true));
	EXPECT_EQ(1, callbackCalls);
	EXPECT_TRUE(host.IsOutlineExpanded());
}

TEST_F(NativeWorkbenchToolRequest, OutlineRequestWithoutOwnerPreservesLegacyApply)
{
	CDlgFuncList dialog;
	CViewContainerHost host(MakePages(dialog));
	host.SetOutlineExpanded(false);

	EXPECT_TRUE(host.RequestOutlineExpanded(true));
	EXPECT_TRUE(host.IsOutlineExpanded());
}

TEST_F(NativeWorkbenchToolRequest, OutlineVetoLeavesStateUnchanged)
{
	CDlgFuncList dialog;
	CViewContainerHost host(MakePages(dialog), [](bool) { return false; });
	host.SetOutlineExpanded(false);

	EXPECT_FALSE(host.RequestOutlineExpanded(true));
	EXPECT_FALSE(host.IsOutlineExpanded());
}

TEST_F(NativeWorkbenchToolRequest, OutlineCallbackExceptionLeavesStateUnchanged)
{
	CDlgFuncList dialog;
	CViewContainerHost host(MakePages(dialog), [](bool) -> bool {
		throw std::runtime_error("outline callback failure");
	});
	host.SetOutlineExpanded(false);

	EXPECT_FALSE(host.RequestOutlineExpanded(true));
	EXPECT_FALSE(host.IsOutlineExpanded());
}

TEST_F(NativeWorkbenchToolRequest, OutlineProjectionDoesNotCallOwner)
{
	CDlgFuncList dialog;
	int callbackCalls = 0;
	CViewContainerHost host(MakePages(dialog), [&](bool) {
		++callbackCalls;
		return true;
	});

	host.SetOutlineExpanded(false);

	EXPECT_EQ(0, callbackCalls);
	EXPECT_FALSE(host.IsOutlineExpanded());
}

//! A ViewContainer is rendered by exactly one side bar. VS Code moves the composite
//! between the Primary and the Secondary Side Bar, so the host that no longer owns the
//! page must stop claiming it even before its owner applies a new selection.
TEST_F(NativeWorkbenchToolRequest, SideBarHostsShareOnePagePoolWithoutBothClaimingIt)
{
	namespace pageIds = workbench::viewcontainer::pageIds;
	CDlgFuncList dialog;
	auto pages = MakePages(dialog);
	CViewContainerHost primary(pages);
	CViewContainerHost secondary(pages);

	primary.ShowPage(pageIds::Extensions);
	EXPECT_EQ(pageIds::Extensions, primary.ActivePage());
	EXPECT_TRUE(secondary.ActivePage().empty());

	primary.ShowPage({});
	secondary.ShowPage(pageIds::Extensions);
	EXPECT_TRUE(primary.ActivePage().empty());
	EXPECT_EQ(pageIds::Extensions, secondary.ActivePage());
}

//! Outline visibility is one model fact, not a per-Part one: it must survive the Explorer
//! container moving to the other side bar.
TEST_F(NativeWorkbenchToolRequest, OutlineExpansionIsSharedByBothSideBars)
{
	namespace pageIds = workbench::viewcontainer::pageIds;
	CDlgFuncList dialog;
	auto pages = MakePages(dialog);
	CViewContainerHost primary(pages);
	CViewContainerHost secondary(pages);

	primary.ShowPage(pageIds::Explorer);
	primary.SetOutlineExpanded(false);
	EXPECT_FALSE(secondary.IsOutlineExpanded());

	primary.ShowPage({});
	secondary.ShowPage(pageIds::Explorer);
	secondary.SetOutlineExpanded(true);
	EXPECT_TRUE(primary.IsOutlineExpanded());
}

TEST_F(NativeWorkbenchToolRequest, BottomPanelRequestWaitsForCommittedProjection)
{
	CExtensionBottomPanelTool tool;
	int callbackCalls = 0;
	tool.SetTabSelectionCallback([&](ExtensionBottomPanelTab tab) {
		++callbackCalls;
		EXPECT_EQ(ExtensionBottomPanelTab::Problems, tab);
		EXPECT_EQ(ExtensionBottomPanelTab::Terminal, tool.ActiveTab());
		return true;
	});

	EXPECT_TRUE(tool.RequestTabSelection(ExtensionBottomPanelTab::Problems));
	EXPECT_EQ(1, callbackCalls);
	EXPECT_EQ(ExtensionBottomPanelTab::Terminal, tool.ActiveTab());

	// The owning Workbench model projects the accepted request back as a committed
	// snapshot. A native tab is never changed optimistically by the request path.
	tool.SetActiveTab(ExtensionBottomPanelTab::Problems);
	EXPECT_EQ(ExtensionBottomPanelTab::Problems, tool.ActiveTab());
}

TEST_F(NativeWorkbenchToolRequest, BottomPanelRequestWithoutOwnerPreservesLegacyApply)
{
	CExtensionBottomPanelTool tool;

	EXPECT_TRUE(tool.RequestTabSelection(ExtensionBottomPanelTab::Problems));
	EXPECT_EQ(ExtensionBottomPanelTab::Problems, tool.ActiveTab());
}

TEST_F(NativeWorkbenchToolRequest, BottomPanelVetoLeavesStateUnchanged)
{
	CExtensionBottomPanelTool tool;
	tool.SetTabSelectionCallback([](ExtensionBottomPanelTab) { return false; });

	EXPECT_FALSE(tool.RequestTabSelection(ExtensionBottomPanelTab::Problems));
	EXPECT_EQ(ExtensionBottomPanelTab::Terminal, tool.ActiveTab());
}

TEST_F(NativeWorkbenchToolRequest, BottomPanelCallbackExceptionLeavesStateUnchanged)
{
	CExtensionBottomPanelTool tool;
	tool.SetTabSelectionCallback([](ExtensionBottomPanelTab) -> bool {
		throw std::runtime_error("bottom panel callback failure");
	});

	EXPECT_FALSE(tool.RequestTabSelection(ExtensionBottomPanelTab::Problems));
	EXPECT_EQ(ExtensionBottomPanelTab::Terminal, tool.ActiveTab());
}

TEST_F(NativeWorkbenchToolRequest, BottomPanelProjectionDoesNotCallOwner)
{
	CExtensionBottomPanelTool tool;
	int callbackCalls = 0;
	int outputSelectionCalls = 0;
	tool.SetTabSelectionCallback([&](ExtensionBottomPanelTab) {
		++callbackCalls;
		return true;
	});
	tool.SetOutputChannelSelectionCallback([&](const std::string&) {
		++outputSelectionCalls;
		return true;
	});

	tool.SetActiveTab(ExtensionBottomPanelTab::Output);

	EXPECT_EQ(0, callbackCalls);
	EXPECT_EQ(ExtensionBottomPanelTab::Output, tool.ActiveTab());
}

TEST_F(NativeWorkbenchToolRequest, BottomPanelOutputProjectionUsesActiveChannelWithoutCallingOwner)
{
	CExtensionBottomPanelTool tool;
	int callbackCalls = 0;
	int outputSelectionCalls = 0;
	tool.SetTabSelectionCallback([&](ExtensionBottomPanelTab) {
		++callbackCalls;
		return true;
	});
	tool.SetOutputChannelSelectionCallback([&](const std::string&) {
		++outputSelectionCalls;
		return true;
	});

	workbench::win32::OutputPanelSnapshot snapshot;
	snapshot.activeChannelId = "active";
	snapshot.channels = {
		{ .channelId = "first", .label = L"First", .projectedText = L"first" },
		{ .channelId = "active", .label = L"Active", .projectedText = L"active", .visible = true,
			.lastShowPreservedFocus = true },
	};

	tool.SetOutputSnapshot(std::move(snapshot));

	EXPECT_EQ(0, callbackCalls);
	EXPECT_EQ(0, outputSelectionCalls);
	ASSERT_TRUE(tool.SelectedOutputChannelId().has_value());
	EXPECT_EQ("active", *tool.SelectedOutputChannelId());
}

TEST_F(NativeWorkbenchToolRequest, BottomPanelOutputSelectionWaitsForCommittedSnapshot)
{
	CExtensionBottomPanelTool tool;
	workbench::win32::OutputPanelSnapshot snapshot;
	snapshot.channels = {
		{ .channelId = "first", .label = L"First", .projectedText = L"first" },
		{ .channelId = "second", .label = L"Second", .projectedText = L"second" },
	};
	tool.SetOutputSnapshot(std::move(snapshot));

	int callbackCalls = 0;
	tool.SetOutputChannelSelectionCallback([&](const std::string& channelId) {
		++callbackCalls;
		EXPECT_EQ("second", channelId);
		const auto selectedChannelId = tool.SelectedOutputChannelId();
		EXPECT_TRUE(selectedChannelId.has_value());
		if (selectedChannelId.has_value()) {
			EXPECT_EQ("first", *selectedChannelId);
		}
		return true;
	});

	EXPECT_TRUE(tool.RequestOutputChannelSelection("second"));
	EXPECT_EQ(1, callbackCalls);
	ASSERT_TRUE(tool.SelectedOutputChannelId().has_value());
	EXPECT_EQ("first", *tool.SelectedOutputChannelId());

	// OutputService's accepted value is projected through the next snapshot.
	workbench::win32::OutputPanelSnapshot committed;
	committed.activeChannelId = "second";
	committed.channels = {
		{ .channelId = "first", .label = L"First", .projectedText = L"first" },
		{ .channelId = "second", .label = L"Second", .projectedText = L"second" },
	};
	tool.SetOutputSnapshot(std::move(committed));
	ASSERT_TRUE(tool.SelectedOutputChannelId().has_value());
	EXPECT_EQ("second", *tool.SelectedOutputChannelId());
}

TEST_F(NativeWorkbenchToolRequest, BottomPanelOutputSelectionVetoOrExceptionRestoresCachedSelection)
{
	CExtensionBottomPanelTool tool;
	workbench::win32::OutputPanelSnapshot snapshot;
	snapshot.channels = {
		{ .channelId = "first", .label = L"First", .projectedText = L"first" },
		{ .channelId = "second", .label = L"Second", .projectedText = L"second" },
	};
	tool.SetOutputSnapshot(std::move(snapshot));

	tool.SetOutputChannelSelectionCallback([](const std::string&) { return false; });
	EXPECT_FALSE(tool.RequestOutputChannelSelection("second"));
	ASSERT_TRUE(tool.SelectedOutputChannelId().has_value());
	EXPECT_EQ("first", *tool.SelectedOutputChannelId());

	tool.SetOutputChannelSelectionCallback([](const std::string&) -> bool {
		throw std::runtime_error("output selection callback failure");
	});
	EXPECT_FALSE(tool.RequestOutputChannelSelection("second"));
	ASSERT_TRUE(tool.SelectedOutputChannelId().has_value());
	EXPECT_EQ("first", *tool.SelectedOutputChannelId());
}

} // namespace

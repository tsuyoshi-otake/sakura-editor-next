/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include "env/ShareDataTestSuite.hpp"
#include "outline/CDlgFuncList.h"
#include "workbench/explorer/CExplorerOutlineTool.h"
#include "workbench/extension/CExtensionBottomPanelTool.h"
#include "workbench/win32/ProblemsOutputPanelProjection.h"

#include <stdexcept>

namespace {

using workbench::explorer::CExplorerOutlineTool;
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
};

TEST_F(NativeWorkbenchToolRequest, OutlineRequestCallsOwnerBeforeApplyingNativeState)
{
	CDlgFuncList dialog;
	CExplorerOutlineTool* observedTool = nullptr;
	int callbackCalls = 0;
	CExplorerOutlineTool tool(dialog, [&](bool expanded) {
		++callbackCalls;
		EXPECT_TRUE(expanded);
		EXPECT_NE(nullptr, observedTool);
		EXPECT_FALSE(observedTool->IsOutlineExpanded());
		return true;
	});
	observedTool = &tool;

	tool.SetOutlineExpanded(false);

	EXPECT_TRUE(tool.RequestOutlineExpanded(true));
	EXPECT_EQ(1, callbackCalls);
	EXPECT_TRUE(tool.IsOutlineExpanded());
}

TEST_F(NativeWorkbenchToolRequest, OutlineRequestWithoutOwnerPreservesLegacyApply)
{
	CDlgFuncList dialog;
	CExplorerOutlineTool tool(dialog);
	tool.SetOutlineExpanded(false);

	EXPECT_TRUE(tool.RequestOutlineExpanded(true));
	EXPECT_TRUE(tool.IsOutlineExpanded());
}

TEST_F(NativeWorkbenchToolRequest, OutlineVetoLeavesStateUnchanged)
{
	CDlgFuncList dialog;
	CExplorerOutlineTool tool(dialog, [](bool) { return false; });
	tool.SetOutlineExpanded(false);

	EXPECT_FALSE(tool.RequestOutlineExpanded(true));
	EXPECT_FALSE(tool.IsOutlineExpanded());
}

TEST_F(NativeWorkbenchToolRequest, OutlineCallbackExceptionLeavesStateUnchanged)
{
	CDlgFuncList dialog;
	CExplorerOutlineTool tool(dialog, [](bool) -> bool {
		throw std::runtime_error("outline callback failure");
	});
	tool.SetOutlineExpanded(false);

	EXPECT_FALSE(tool.RequestOutlineExpanded(true));
	EXPECT_FALSE(tool.IsOutlineExpanded());
}

TEST_F(NativeWorkbenchToolRequest, OutlineProjectionDoesNotCallOwner)
{
	CDlgFuncList dialog;
	int callbackCalls = 0;
	CExplorerOutlineTool tool(dialog, [&](bool) {
		++callbackCalls;
		return true;
	});

	tool.SetOutlineExpanded(false);

	EXPECT_EQ(0, callbackCalls);
	EXPECT_FALSE(tool.IsOutlineExpanded());
}

TEST_F(NativeWorkbenchToolRequest, BottomPanelRequestCallsOwnerBeforeApplyingNativeState)
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

TEST_F(NativeWorkbenchToolRequest, BottomPanelOutputSelectionCallsOwnerBeforeApplyingLocalSelection)
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

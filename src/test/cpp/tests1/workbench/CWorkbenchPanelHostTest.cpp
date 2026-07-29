/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include <gtest/gtest.h>

#include "workbench/CWorkbenchPanelHost.h"

#include <memory>

namespace {

class RecordingTool final : public workbench::IWorkbenchTool {
public:
	bool Create(HWND parent) override { createParent = parent; return parent != nullptr; }
	void Layout(const RECT& rect, unsigned int dpi) override { lastLayout = rect; lastDpi = dpi; ++layoutCalls; }
	void Activate() override { ++activateCalls; }
	void Deactivate() override { ++deactivateCalls; }
	bool PreTranslateMessage(MSG&) override { ++preTranslateCalls; return true; }
	void Close() override { ++closeCalls; }

	HWND createParent = nullptr;
	RECT lastLayout{};
	unsigned int lastDpi = 0;
	int layoutCalls = 0;
	int activateCalls = 0;
	int deactivateCalls = 0;
	int preTranslateCalls = 0;
	int closeCalls = 0;
};

TEST(WorkbenchPanelHost, UsesHideWithoutClosingOwnedToolAndPersistsOnlyCommittedResize)
{
	int callbackCount = 0;
	int persistedExtent = 0;
	workbench::CWorkbenchPanelHost host(workbench::WorkbenchEdge::Left, 280,
		[&](workbench::WorkbenchEdge edge, int extent) {
			EXPECT_EQ(workbench::WorkbenchEdge::Left, edge);
			++callbackCount;
			persistedExtent = extent;
		});
	auto tool = std::make_unique<RecordingTool>();
	auto* recordingTool = tool.get();
	const HINSTANCE instance = ::GetModuleHandleW(nullptr);
	ASSERT_TRUE(host.Create(::GetDesktopWindow(), instance, std::move(tool)));

	const RECT bounds{ 0, 0, 360, 500 };
	host.Layout(bounds, 144);
	host.Show();
	EXPECT_EQ(workbench::WorkbenchPanelState::Visible, host.GetState());
	EXPECT_EQ(45, host.GetHeaderHeightPixels());
	EXPECT_EQ(0, recordingTool->activateCalls);
	host.ActivateTool();
	EXPECT_EQ(1, recordingTool->activateCalls);
	host.BeginResize();
	host.UpdateResize(320);
	EXPECT_EQ(0, callbackCount);
	host.CancelResize();
	EXPECT_EQ(280, host.GetExtentDip());
	host.BeginResize();
	host.UpdateResize(320);
	host.CommitResize();
	EXPECT_EQ(1, callbackCount);
	EXPECT_EQ(320, persistedExtent);
	host.Hide();
	EXPECT_EQ(workbench::WorkbenchPanelState::Hidden, host.GetState());
	EXPECT_EQ(1, callbackCount);
	host.Close();
	host.Close();
}

TEST(WorkbenchPanelHost, SharedExtentApplicationDoesNotPersistOrEnterResize)
{
	int persistCount = 0;
	workbench::CWorkbenchPanelHost host(workbench::WorkbenchEdge::Right, 260,
		[&](workbench::WorkbenchEdge, int) { ++persistCount; });

	host.ApplyExtentDip(315);

	EXPECT_EQ(315, host.GetExtentDip());
	EXPECT_EQ(315, host.GetPendingExtentDip());
	EXPECT_EQ(workbench::WorkbenchPanelState::Hidden, host.GetState());
	EXPECT_EQ(0, persistCount);
}

} // namespace

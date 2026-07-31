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

TEST(WorkbenchPanelHost, UsesHideWithoutClosingOwnedToolAndCommitsOnlyAcceptedResize)
{
	int callbackCount = 0;
	int persistedExtent = 0;
	workbench::CWorkbenchPanelHost* hostForCallback = nullptr;
	workbench::CWorkbenchPanelHost host(workbench::WorkbenchEdge::Left, 280,
		[&](workbench::WorkbenchEdge edge, int extent) {
			EXPECT_EQ(workbench::WorkbenchEdge::Left, edge);
			EXPECT_NE(nullptr, hostForCallback);
			if (hostForCallback != nullptr) EXPECT_EQ(280, hostForCallback->GetExtentDip());
			++callbackCount;
			persistedExtent = extent;
			return true;
		});
	hostForCallback = &host;
	auto tool = std::make_unique<RecordingTool>();
	auto* recordingTool = tool.get();
	const HINSTANCE instance = ::GetModuleHandleW(nullptr);
	ASSERT_TRUE(host.Create(::GetDesktopWindow(), instance, std::move(tool)));

	const RECT bounds{ 0, 0, 360, 500 };
	host.Layout(bounds, 144);
	host.Show();
	EXPECT_EQ(workbench::WorkbenchPanelState::Visible, host.GetState());
	EXPECT_EQ(45, host.GetHeaderHeightPixels());
	EXPECT_EQ(45, recordingTool->lastLayout.top);
	EXPECT_EQ(500, recordingTool->lastLayout.bottom);
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
	EXPECT_TRUE(host.CommitResize());
	EXPECT_EQ(1, callbackCount);
	EXPECT_EQ(320, persistedExtent);
	EXPECT_EQ(320, host.GetExtentDip());
	host.Hide();
	EXPECT_EQ(workbench::WorkbenchPanelState::Hidden, host.GetState());
	EXPECT_EQ(1, callbackCount);
	host.Close();
	host.Close();
}

TEST(WorkbenchPanelHost, RejectedResizeRestoresPriorExtentAndLeavesVisible)
{
	int callbackCount = 0;
	workbench::CWorkbenchPanelHost host(workbench::WorkbenchEdge::Bottom, 220,
		[&](workbench::WorkbenchEdge edge, int extent) {
			EXPECT_EQ(workbench::WorkbenchEdge::Bottom, edge);
			EXPECT_EQ(280, extent);
			++callbackCount;
			return false;
		});
	auto tool = std::make_unique<RecordingTool>();
	ASSERT_TRUE(host.Create(::GetDesktopWindow(), ::GetModuleHandleW(nullptr), std::move(tool)));
	host.Show();

	host.BeginResize();
	host.UpdateResize(280);
	EXPECT_FALSE(host.CommitResize());

	EXPECT_EQ(1, callbackCount);
	EXPECT_EQ(220, host.GetExtentDip());
	EXPECT_EQ(220, host.GetPendingExtentDip());
	EXPECT_EQ(workbench::WorkbenchPanelState::Visible, host.GetState());
	host.Close();
}

TEST(WorkbenchPanelHost, CancelResizeDoesNotCallModelCommit)
{
	int callbackCount = 0;
	workbench::CWorkbenchPanelHost host(workbench::WorkbenchEdge::Left, 280,
		[&](workbench::WorkbenchEdge, int) { ++callbackCount; return true; });
	auto tool = std::make_unique<RecordingTool>();
	ASSERT_TRUE(host.Create(::GetDesktopWindow(), ::GetModuleHandleW(nullptr), std::move(tool)));
	host.Show();

	host.BeginResize();
	host.UpdateResize(320);
	host.CancelResize();

	EXPECT_EQ(0, callbackCount);
	EXPECT_EQ(280, host.GetExtentDip());
	EXPECT_EQ(280, host.GetPendingExtentDip());
	EXPECT_EQ(workbench::WorkbenchPanelState::Visible, host.GetState());
	host.Close();
}

TEST(WorkbenchPanelHost, GivesBottomToolTheCompleteClientForItsOwnedHeader)
{
	workbench::CWorkbenchPanelHost host(workbench::WorkbenchEdge::Bottom, 220);
	auto tool = std::make_unique<RecordingTool>();
	auto* recordingTool = tool.get();
	ASSERT_TRUE(host.Create(::GetDesktopWindow(), ::GetModuleHandleW(nullptr), std::move(tool)));

	host.Layout(RECT{ 0, 0, 640, 260 }, 192);

	EXPECT_EQ(0, host.GetHeaderHeightPixels());
	EXPECT_EQ(0, recordingTool->lastLayout.left);
	EXPECT_EQ(0, recordingTool->lastLayout.top);
	EXPECT_EQ(640, recordingTool->lastLayout.right);
	EXPECT_EQ(260, recordingTool->lastLayout.bottom);
	EXPECT_EQ(192U, recordingTool->lastDpi);
	host.Close();
}

TEST(WorkbenchPanelHost, SharedExtentApplicationDoesNotPersistOrEnterResize)
{
	int persistCount = 0;
	workbench::CWorkbenchPanelHost host(workbench::WorkbenchEdge::Right, 260,
		[&](workbench::WorkbenchEdge, int) { ++persistCount; return true; });

	host.ApplyExtentDip(315);

	EXPECT_EQ(315, host.GetExtentDip());
	EXPECT_EQ(315, host.GetPendingExtentDip());
	EXPECT_EQ(workbench::WorkbenchPanelState::Hidden, host.GetState());
	EXPECT_EQ(0, persistCount);
}

} // namespace

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

struct MouseDownRecorder {
	int count = 0;
	POINT point{};
};

LRESULT CALLBACK RecordingParentProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
	if (message == WM_NCCREATE) {
		const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
		::SetWindowLongPtrW(window, GWLP_USERDATA,
			reinterpret_cast<LONG_PTR>(create->lpCreateParams));
	}
	auto* recorder = reinterpret_cast<MouseDownRecorder*>(
		::GetWindowLongPtrW(window, GWLP_USERDATA));
	if (message == WM_LBUTTONDOWN && recorder != nullptr) {
		++recorder->count;
		recorder->point = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
		return 0;
	}
	if (message == WM_NCDESTROY) ::SetWindowLongPtrW(window, GWLP_USERDATA, 0);
	return ::DefWindowProcW(window, message, wParam, lParam);
}

HWND CreateRecordingParent(MouseDownRecorder& recorder)
{
	constexpr wchar_t className[] = L"SakuraWorkbenchPanelHostTestParent";
	const HINSTANCE instance = ::GetModuleHandleW(nullptr);
	WNDCLASSEXW windowClass{};
	windowClass.cbSize = sizeof(windowClass);
	windowClass.hInstance = instance;
	windowClass.lpfnWndProc = RecordingParentProc;
	windowClass.lpszClassName = className;
	if (::RegisterClassExW(&windowClass) == 0
		&& ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return nullptr;
	return ::CreateWindowExW(0, className, L"", WS_POPUP,
		0, 0, 800, 600, nullptr, nullptr, instance, &recorder);
}

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

TEST(WorkbenchPanelHost, SashOverlaysAdjacentChildrenAndForwardsInitialPress)
{
	MouseDownRecorder recorder;
	const HWND parent = CreateRecordingParent(recorder);
	ASSERT_NE(nullptr, parent);
	workbench::CWorkbenchPanelHost host(workbench::WorkbenchEdge::Left, 280);
	auto tool = std::make_unique<RecordingTool>();
	ASSERT_TRUE(host.Create(parent, ::GetModuleHandleW(nullptr), std::move(tool)));
	host.Layout(RECT{ 0, 0, 360, 500 }, 144);
	host.Show();
	host.LayoutSash(RECT{ 360, 0, 361, 500 });

	const HWND sash = host.GetSashHwnd();
	ASSERT_NE(nullptr, sash);
	EXPECT_NE(0L, ::GetWindowLongPtrW(sash, GWL_STYLE) & WS_VISIBLE);
	RECT sashBounds{};
	ASSERT_TRUE(::GetWindowRect(sash, &sashBounds));
	POINT corners[2]{
		{ sashBounds.left, sashBounds.top },
		{ sashBounds.right, sashBounds.bottom },
	};
	(void)::MapWindowPoints(HWND_DESKTOP, parent, corners, 2);
	EXPECT_EQ(6, corners[1].x - corners[0].x);
	EXPECT_LE(corners[0].x, 360);
	EXPECT_GT(corners[1].x, 360);

	(void)::SendMessageW(sash, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(0, 12));
	EXPECT_EQ(1, recorder.count);
	EXPECT_EQ(corners[0].x, recorder.point.x);
	EXPECT_EQ(12, recorder.point.y);

	host.Hide();
	EXPECT_EQ(0L, ::GetWindowLongPtrW(sash, GWL_STYLE) & WS_VISIBLE);
	host.Close();
	::DestroyWindow(parent);
}

} // namespace

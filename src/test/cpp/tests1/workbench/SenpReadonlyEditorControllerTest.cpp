/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#include "pch.h"
#include <gtest/gtest.h>
#include "workbench/editor/SenpReadonlyEditorController.h"
#include "workbench/editor/EditorCommandIds.h"

#include <CommCtrl.h>

namespace workbench::editor::tests {
namespace {

class SenpReadonlyEditorControllerTest : public testing::Test {
protected:
	EditorCoreService core;
	HWND parent{}, legacy{}, detail{};
	std::unique_ptr<SenpReadonlyEditorController> controller;
	std::string detailId;
	int copied{}, selected{}, findShown{}, next{}, previous{}, closed{};
	bool closeDuringShow{};
	SenpReadonlyStatus reentrantCloseStatus{ SenpReadonlyStatus::Failed };

	static LRESULT CALLBACK Hook(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
		UINT_PTR, DWORD_PTR context)
	{
		auto& fixture = *reinterpret_cast<SenpReadonlyEditorControllerTest*>(context);
		if (message == WM_SHOWWINDOW && wParam && fixture.closeDuringShow) {
			fixture.closeDuringShow = false;
			fixture.reentrantCloseStatus = fixture.controller->Close(fixture.detailId);
		}
		return ::DefSubclassProc(window, message, wParam, lParam);
	}

	void SetUp() override
	{
		const EditorDocumentIdentity identity{ .opaqueId = "controller-legacy" };
		ASSERT_EQ(EEditorOperationStatus::Succeeded, core.OpenResolvedInput({
			.operation = { "controller.fixture.open" }, .input = { "legacy", identity },
			.resolvedDocument = ResolvedEditorDocument{ identity, 7, true },
		}).status);
		parent = ::CreateWindowExW(0, L"STATIC", L"SENP editor controller", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
			80, 80, 560, 380, nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
		ASSERT_NE(nullptr, parent);
		legacy = Child(L"EDIT", L"legacy", ES_MULTILINE);
		ASSERT_NE(nullptr, legacy);
		::ShowWindow(parent, SW_SHOWNOACTIVATE);
		controller = std::make_unique<SenpReadonlyEditorController>(core, parent, legacy, legacy, "legacy");
		ASSERT_EQ(SenpSurfaceProjection::Applied, controller->Layout({ 4, 8, 500, 320 }));
	}

	void TearDown() override
	{
		if (controller) { (void)controller->Shutdown(); controller.reset(); }
		if (detail && ::IsWindow(detail)) ::DestroyWindow(detail);
		if (parent) { ::DestroyWindow(parent); EXPECT_FALSE(::IsWindow(parent)); }
	}

	HWND Child(const wchar_t* kind, const wchar_t* text, DWORD style)
	{
		return ::CreateWindowExW(0, kind, text, WS_CHILD | WS_TABSTOP | style,
			0, 0, 0, 0, parent, nullptr, ::GetModuleHandleW(nullptr), nullptr);
	}

	SenpReadonlyOpenResult OpenDetail()
	{
		detail = Child(L"EDIT", L"readonly", ES_MULTILINE | ES_READONLY);
		EXPECT_NE(nullptr, detail);
		auto result = controller->Open({ "sample.details", 3, 4, 5 }, L"issue/296", L"Issue 296", detail, detail, {
			.copy = [this] { ++copied; return true; },
			.selectAll = [this] { ++selected; },
			.showFind = [this] { ++findShown; },
			.find = [this](bool backward) { backward ? ++previous : ++next; return true; },
			.closed = [this] {
				EXPECT_EQ(nullptr, ::GetPropW(detail, L"Sakura.Senp.EditorSurfaceOwner"));
				++closed;
			},
		});
		detailId = result.inputId;
		return result;
	}
};

TEST_F(SenpReadonlyEditorControllerTest, ProjectsTheCoreSelectionAndRoutesReadonlyCommands)
{
	ASSERT_EQ(SenpReadonlyStatus::Succeeded, OpenDetail().status);
	EXPECT_TRUE(::IsWindowVisible(legacy));
	ASSERT_EQ(SenpReadonlyStatus::Succeeded, controller->Show(detailId));
	EXPECT_FALSE(::IsWindowVisible(legacy));
	EXPECT_TRUE(::IsWindowVisible(detail));
	EXPECT_TRUE(controller->IsReadonlyActive());
	EXPECT_EQ(SenpReadonlyStatus::Succeeded, controller->Show("legacy"));
	EXPECT_TRUE(::IsWindowVisible(legacy));
	EXPECT_FALSE(controller->IsReadonlyActive());
	ASSERT_EQ(SenpReadonlyStatus::Succeeded, controller->Show(detailId));
	EXPECT_EQ(SenpReadonlyCommandStatus::Succeeded, controller->Execute("editor.action.clipboardCopyAction"));
	EXPECT_EQ(SenpReadonlyCommandStatus::Succeeded, controller->Execute("editor.action.selectAll"));
	EXPECT_EQ(SenpReadonlyCommandStatus::Succeeded, controller->Execute("actions.find"));
	EXPECT_EQ(SenpReadonlyCommandStatus::Succeeded, controller->Execute("editor.action.nextMatchFindAction"));
	EXPECT_EQ(SenpReadonlyCommandStatus::Succeeded, controller->Execute("editor.action.previousMatchFindAction"));
	EXPECT_EQ(SenpReadonlyCommandStatus::NotApplicable, controller->Execute(command_ids::Save));
	EXPECT_EQ(1, copied); EXPECT_EQ(1, selected); EXPECT_EQ(1, findShown); EXPECT_EQ(1, next); EXPECT_EQ(1, previous);
	EXPECT_EQ(SenpReadonlyCommandStatus::Succeeded, controller->Execute(command_ids::CloseActiveEditor));
	EXPECT_EQ(1, closed);
	EXPECT_TRUE(::IsWindowVisible(legacy));
	EXPECT_FALSE(controller->IsReadonlyActive());
}

TEST_F(SenpReadonlyEditorControllerTest, FailedSurfaceBindingRollsBackTheNewCoreInput)
{
	HWND foreign = ::CreateWindowExW(0, L"STATIC", L"foreign", WS_OVERLAPPEDWINDOW,
		0, 0, 100, 100, nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
	ASSERT_NE(nullptr, foreign);
	const auto before = core.Snapshot().group.inputs.size();
	const auto result = controller->Open({ "sample.details", 3, 4, 5 }, L"failed", L"Failed", foreign, foreign, {});
	EXPECT_EQ(SenpReadonlyStatus::Failed, result.status);
	EXPECT_EQ(before, core.Snapshot().group.inputs.size());
	::DestroyWindow(foreign);
}

TEST_F(SenpReadonlyEditorControllerTest, ExternalCoreCloseReleasesTheBorrowedSurfaceExactlyOnce)
{
	ASSERT_EQ(SenpReadonlyStatus::Succeeded, OpenDetail().status);
	ASSERT_EQ(SenpReadonlyStatus::Succeeded, controller->Show(detailId));
	const auto snapshot = core.Snapshot();
	ASSERT_EQ(EEditorOperationStatus::Succeeded,
		core.CloseInput({ { "controller.external.close", snapshot.revision }, detailId }).status);
	EXPECT_EQ(SenpSurfaceProjection::Applied, controller->Apply(true));
	EXPECT_EQ(1, closed);
	EXPECT_TRUE(::IsWindowVisible(legacy));
	EXPECT_EQ(SenpSurfaceProjection::Applied, controller->Apply());
	EXPECT_EQ(1, closed);
}

TEST_F(SenpReadonlyEditorControllerTest, HiddenEditorPartRetainsReadonlySelectionAndBounds)
{
	ASSERT_EQ(SenpReadonlyStatus::Succeeded, OpenDetail().status);
	ASSERT_EQ(SenpReadonlyStatus::Succeeded, controller->Show(detailId));
	controller->SetVisible(false);
	EXPECT_FALSE(::IsWindowVisible(detail));
	EXPECT_EQ(detailId, *core.Snapshot().group.activeInputId);
	EXPECT_EQ(SenpSurfaceProjection::Applied, controller->Layout({ 20, 30, 420, 280 }));
	controller->SetVisible(true);
	EXPECT_TRUE(::IsWindowVisible(detail));
	RECT bounds{}, expected{ 20, 30, 420, 280 };
	ASSERT_TRUE(::GetWindowRect(detail, &bounds));
	::MapWindowPoints(nullptr, parent, reinterpret_cast<POINT*>(&bounds), 2);
	EXPECT_TRUE(::EqualRect(&bounds, &expected));
}

TEST_F(SenpReadonlyEditorControllerTest, ReentrantCoreCloseDefersSurfaceFinalizationUntilProjectionUnwinds)
{
	ASSERT_EQ(SenpReadonlyStatus::Succeeded, OpenDetail().status);
	ASSERT_TRUE(::SetWindowSubclass(detail, Hook, 1, reinterpret_cast<DWORD_PTR>(this)));
	closeDuringShow = true;
	EXPECT_EQ(SenpReadonlyStatus::Succeeded, controller->Show(detailId));
	EXPECT_EQ(SenpReadonlyStatus::Succeeded, reentrantCloseStatus);
	EXPECT_EQ(0, closed);
	EXPECT_NE(nullptr, ::GetPropW(detail, L"Sakura.Senp.EditorSurfaceOwner"));
	EXPECT_EQ(SenpSurfaceProjection::Applied, controller->Apply());
	EXPECT_EQ(1, closed);
	EXPECT_EQ(nullptr, ::GetPropW(detail, L"Sakura.Senp.EditorSurfaceOwner"));
}

} // namespace
} // namespace workbench::editor::tests

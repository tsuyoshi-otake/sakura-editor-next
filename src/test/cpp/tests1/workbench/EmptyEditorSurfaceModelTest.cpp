/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include <gtest/gtest.h>

#include "workbench/editor/EmptyEditorSurfaceModel.h"

namespace workbench::editor {
namespace {

TEST(EmptyEditorSurfaceModel, ExposesStableVsCodeCommandsAndJapaneseActions)
{
	EmptyEditorSurfaceModel model;
	EXPECT_EQ(5U, model.GetActionCount());
	EXPECT_EQ("workbench.action.files.newUntitledFile", EmptyEditorSurfaceModel::CommandId(EmptyEditorSurfaceAction::NewFile));
	EXPECT_EQ("workbench.action.files.openFile", EmptyEditorSurfaceModel::CommandId(EmptyEditorSurfaceAction::OpenFile));
	EXPECT_EQ("workbench.action.files.openFolder", EmptyEditorSurfaceModel::CommandId(EmptyEditorSurfaceAction::OpenFolder));
	EXPECT_EQ("workbench.action.showCommands", EmptyEditorSurfaceModel::CommandId(EmptyEditorSurfaceAction::ShowAllCommands));
	EXPECT_EQ("workbench.action.openSettings", EmptyEditorSurfaceModel::CommandId(EmptyEditorSurfaceAction::OpenSettings));
	EXPECT_STREQ(L"新しいファイル", EmptyEditorSurfaceModel::Label(EmptyEditorSurfaceAction::NewFile));
	EXPECT_STREQ(L"フォルダーを開く...", EmptyEditorSurfaceModel::Label(EmptyEditorSurfaceAction::OpenFolder));
	EXPECT_STREQ(L"Ctrl+Shift+P", EmptyEditorSurfaceModel::Shortcut(EmptyEditorSurfaceAction::ShowAllCommands));
	// F_OPEN_WORKSPACE_FOLDER has no default key binding, so no shortcut may be advertised.
	EXPECT_STREQ(L"", EmptyEditorSurfaceModel::Shortcut(EmptyEditorSurfaceAction::OpenFolder));
}

TEST(EmptyEditorSurfaceModel, CentersActionRectsAndHitTestsOnlyEnabledActions)
{
	EmptyEditorSurfaceModel model;
	model.SetViewport(800, 600, 96);
	const auto first = model.GetAction(0);
	const auto second = model.GetAction(1);
	EXPECT_EQ(256, first.bounds.left);
	EXPECT_EQ(544, first.bounds.right);
	EXPECT_LT(first.bounds.top, second.bounds.top);
	EXPECT_EQ(EmptyEditorSurfaceAction::NewFile, *model.HitTest(first.bounds.left, first.bounds.top));

	model.SetEnabled(EmptyEditorSurfaceAction::NewFile, false);
	EXPECT_FALSE(model.HitTest(first.bounds.left, first.bounds.top));
	EXPECT_FALSE(model.Invoke(EmptyEditorSurfaceAction::NewFile));
}

TEST(EmptyEditorSurfaceModel, CentersASquareLetterpressAboveTheActionListLikeVsCode)
{
	EmptyEditorSurfaceModel model;
	model.SetViewport(800, 600, 96);
	const auto letterpress = model.GetLetterpressBounds();
	const auto first = model.GetAction(0);
	const auto last = model.GetAction(model.GetActionCount() - 1);

	// VS Code's `.letterpress` is a centered square capped at 256px with a 24px gap below it.
	EXPECT_EQ(256, letterpress.Width());
	EXPECT_EQ(letterpress.Width(), letterpress.Height());
	EXPECT_EQ(800 - letterpress.right, letterpress.left);
	EXPECT_EQ(24, first.bounds.top - letterpress.bottom);
	// The logo and the list form one vertically centered column, so the space above the logo
	// matches the space below the last action row.
	EXPECT_EQ(600 - last.bounds.bottom, letterpress.top);
	EXPECT_FALSE(model.HitTest(letterpress.left, letterpress.top));
}

TEST(EmptyEditorSurfaceModel, DropsTheLetterpressBeforeTheActionListWhenSpaceRunsOut)
{
	EmptyEditorSurfaceModel model;
	// 288 DIP of action rows leave under the 48 DIP logo minimum, so only the list survives.
	model.SetViewport(800, 200, 96);
	EXPECT_EQ(EmptyEditorSurfaceRect{}, model.GetLetterpressBounds());
	EXPECT_EQ(22, model.GetAction(0).bounds.top);
	EXPECT_EQ(28, model.GetAction(0).bounds.Height());

	model.SetViewport(0, 0, 0);
	EXPECT_EQ(EmptyEditorSurfaceRect{}, model.GetLetterpressBounds());
}

TEST(EmptyEditorSurfaceModel, HandlesNarrowAndZeroClientsWithoutNegativeGeometry)
{
	EmptyEditorSurfaceModel model;
	model.SetViewport(120, 52, 192);
	for (std::size_t index = 0; index < model.GetActionCount(); ++index) {
		const auto bounds = model.GetAction(index).bounds;
		EXPECT_GE(bounds.left, 0);
		EXPECT_GE(bounds.top, 0);
		EXPECT_LE(bounds.right, 120);
		EXPECT_LE(bounds.bottom, 52);
		EXPECT_GE(bounds.Width(), 0);
		EXPECT_GE(bounds.Height(), 0);
	}

	model.SetViewport(0, 0, 0);
	EXPECT_EQ(96U, model.GetDpi());
	for (std::size_t index = 0; index < model.GetActionCount(); ++index) {
		const auto bounds = model.GetAction(index).bounds;
		EXPECT_EQ(0, bounds.Width());
		EXPECT_EQ(0, bounds.Height());
	}
	EXPECT_FALSE(model.HitTest(0, 0));
}

TEST(EmptyEditorSurfaceModel, FocusWrapsAndSkipsDisabledActions)
{
	EmptyEditorSurfaceModel model;
	model.SetEnabled(EmptyEditorSurfaceAction::OpenFile, false);
	model.SetFocused(EmptyEditorSurfaceAction::NewFile);
	EXPECT_EQ(EmptyEditorSurfaceAction::OpenFolder, *model.MoveFocus(1));
	EXPECT_EQ(EmptyEditorSurfaceAction::ShowAllCommands, *model.MoveFocus(1));
	EXPECT_EQ(EmptyEditorSurfaceAction::OpenSettings, *model.MoveFocus(1));
	EXPECT_EQ(EmptyEditorSurfaceAction::NewFile, *model.MoveFocus(1));
	EXPECT_EQ(EmptyEditorSurfaceAction::OpenSettings, *model.MoveFocus(-1));

	model.SetEnabled(EmptyEditorSurfaceAction::NewFile, false);
	model.SetEnabled(EmptyEditorSurfaceAction::OpenFolder, false);
	model.SetEnabled(EmptyEditorSurfaceAction::ShowAllCommands, false);
	model.SetEnabled(EmptyEditorSurfaceAction::OpenSettings, false);
	EXPECT_FALSE(model.MoveFocus(1));
	EXPECT_FALSE(model.GetFocused());
}

} // namespace
} // namespace workbench::editor

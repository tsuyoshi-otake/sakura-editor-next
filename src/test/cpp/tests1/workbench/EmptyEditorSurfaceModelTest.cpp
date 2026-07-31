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
	EXPECT_EQ(4U, model.GetActionCount());
	EXPECT_EQ("workbench.action.files.newUntitledFile", EmptyEditorSurfaceModel::CommandId(EmptyEditorSurfaceAction::NewFile));
	EXPECT_EQ("workbench.action.files.openFile", EmptyEditorSurfaceModel::CommandId(EmptyEditorSurfaceAction::OpenFile));
	EXPECT_EQ("workbench.action.showCommands", EmptyEditorSurfaceModel::CommandId(EmptyEditorSurfaceAction::ShowAllCommands));
	EXPECT_EQ("workbench.action.openSettings", EmptyEditorSurfaceModel::CommandId(EmptyEditorSurfaceAction::OpenSettings));
	EXPECT_STREQ(L"新しいファイル", EmptyEditorSurfaceModel::Label(EmptyEditorSurfaceAction::NewFile));
	EXPECT_STREQ(L"Ctrl+Shift+P", EmptyEditorSurfaceModel::Shortcut(EmptyEditorSurfaceAction::ShowAllCommands));
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
	EXPECT_EQ(EmptyEditorSurfaceAction::ShowAllCommands, *model.MoveFocus(1));
	EXPECT_EQ(EmptyEditorSurfaceAction::OpenSettings, *model.MoveFocus(1));
	EXPECT_EQ(EmptyEditorSurfaceAction::NewFile, *model.MoveFocus(1));
	EXPECT_EQ(EmptyEditorSurfaceAction::OpenSettings, *model.MoveFocus(-1));

	model.SetEnabled(EmptyEditorSurfaceAction::NewFile, false);
	model.SetEnabled(EmptyEditorSurfaceAction::ShowAllCommands, false);
	model.SetEnabled(EmptyEditorSurfaceAction::OpenSettings, false);
	EXPECT_FALSE(model.MoveFocus(1));
	EXPECT_FALSE(model.GetFocused());
}

} // namespace
} // namespace workbench::editor

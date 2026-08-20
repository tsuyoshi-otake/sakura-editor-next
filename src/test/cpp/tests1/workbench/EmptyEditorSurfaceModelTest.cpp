/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include <gtest/gtest.h>

#include "workbench/commands/WorkbenchCommandRegistry.h"
#include "workbench/editor/EmptyEditorSurfaceModel.h"
#include "workbench/editor/WorkbenchKeybindingState.h"

namespace workbench::editor {
namespace {

TEST(EmptyEditorSurfaceModel, ExposesStableVsCodeCommandsAndPresentationNeutralFallbackLabels)
{
	EmptyEditorSurfaceModel model;
	EXPECT_EQ(5U, model.GetActionCount());
	EXPECT_EQ("workbench.action.files.newUntitledFile", EmptyEditorSurfaceModel::CommandId(EmptyEditorSurfaceAction::NewFile));
	EXPECT_EQ("workbench.action.files.openFile", EmptyEditorSurfaceModel::CommandId(EmptyEditorSurfaceAction::OpenFile));
	EXPECT_EQ("workbench.action.files.openFolder", EmptyEditorSurfaceModel::CommandId(EmptyEditorSurfaceAction::OpenFolder));
	EXPECT_EQ("workbench.action.showCommands", EmptyEditorSurfaceModel::CommandId(EmptyEditorSurfaceAction::ShowAllCommands));
	EXPECT_EQ("workbench.action.openSettings", EmptyEditorSurfaceModel::CommandId(EmptyEditorSurfaceAction::OpenSettings));
	EXPECT_STREQ(L"New File", EmptyEditorSurfaceModel::Label(EmptyEditorSurfaceAction::NewFile));
	EXPECT_STREQ(L"Open Folder...", EmptyEditorSurfaceModel::Label(EmptyEditorSurfaceAction::OpenFolder));
	EXPECT_STREQ(L"Ctrl+Shift+P", EmptyEditorSurfaceModel::Shortcut(EmptyEditorSurfaceAction::ShowAllCommands));
	EXPECT_STREQ(L"Ctrl+K Ctrl+O", EmptyEditorSurfaceModel::Shortcut(EmptyEditorSurfaceAction::OpenFolder));
}

TEST(WorkbenchKeybindingState, ExpiresAtVsCodesFiveSecondInactivityDeadline)
{
	CtrlKChordState state;
	state.Begin(100);
	EXPECT_EQ(5000U, CtrlKChordState::TimeoutMs);
	EXPECT_TRUE(state.IsPending());
	EXPECT_FALSE(state.ExpireIfNeeded(100 + CtrlKChordState::TimeoutMs - 1));
	EXPECT_TRUE(state.IsPending());
	EXPECT_TRUE(state.ExpireIfNeeded(100 + CtrlKChordState::TimeoutMs));
	EXPECT_FALSE(state.IsPending());
	EXPECT_EQ(0U, state.StartedAt());
}

TEST(WorkbenchKeybindingState, FocusChangeCancelsThePendingChord)
{
	CtrlKChordState state;
	state.Begin(1000, 11);

	EXPECT_FALSE(state.CancelIfFocusChanged(11));
	EXPECT_TRUE(state.IsPending());
	EXPECT_TRUE(state.CancelIfFocusChanged(12));
	EXPECT_FALSE(state.IsPending());
	EXPECT_FALSE(state.ExpireIfNeeded(2500));
}

TEST(WorkbenchKeybindingState, CtrlKSecondStrokesSelectTheExactStableCommands)
{
	const auto available = [](std::string_view) { return true; };
	WorkbenchKeybindingState state;
	const WorkbenchKeyModifiers controlOnly{ .control = true };

	// Ctrl-up has no chord transition. Its re-press is a pass-through key-down,
	// so Ctrl+K, Ctrl-up, Ctrl-down, Ctrl+O remains a valid VS Code chord.
	EXPECT_EQ(EWorkbenchKeyInputDecision::BeginOrRestartChordAndConsume,
		state.HandleKeyDown('K', controlOnly, 1000, 11, available).decision);
	EXPECT_EQ(EWorkbenchKeyInputDecision::PassThrough,
		state.HandleKeyDown(0x11, controlOnly, 1001, 11, available).decision); // VK_CONTROL
	EXPECT_TRUE(state.IsChordPending());
	auto result = state.HandleKeyDown('O', controlOnly, 1002, 11, available);
	EXPECT_EQ(EWorkbenchKeyInputDecision::ExecuteStableCommandAndConsume, result.decision);
	EXPECT_EQ(command_ids::OpenFolder, result.commandId);
	EXPECT_FALSE(state.IsChordPending());

	EXPECT_EQ(EWorkbenchKeyInputDecision::BeginOrRestartChordAndConsume,
		state.HandleKeyDown('K', controlOnly, 1100, 11, available).decision);
	result = state.HandleKeyDown('S', {}, 1101, 11, available);
	EXPECT_EQ(EWorkbenchKeyInputDecision::ExecuteStableCommandAndConsume, result.decision);
	EXPECT_EQ(command_ids::SaveAll, result.commandId);

	EXPECT_EQ(EWorkbenchKeyInputDecision::BeginOrRestartChordAndConsume,
		state.HandleKeyDown('K', controlOnly, 1200, 11, available).decision);
	result = state.HandleKeyDown('F', {}, 1201, 11, available);
	EXPECT_EQ(EWorkbenchKeyInputDecision::ExecuteStableCommandAndConsume, result.decision);
	EXPECT_EQ(command_ids::CloseFolder, result.commandId);

	EXPECT_EQ(EWorkbenchKeyInputDecision::BeginOrRestartChordAndConsume,
		state.HandleKeyDown('K', controlOnly, 1300, 11, available).decision);
	result = state.HandleKeyDown('V', {}, 1301, 11, available);
	EXPECT_EQ(EWorkbenchKeyInputDecision::ExecuteStableCommandAndConsume, result.decision);
	EXPECT_EQ(command_ids::MarkdownShowPreviewToSide, result.commandId);
}

TEST(WorkbenchKeybindingState, WrongModifiersAndSecondStrokesClearAndAreConsumed)
{
	const auto available = [](std::string_view) { return true; };
	const WorkbenchKeyModifiers controlOnly{ .control = true };
	WorkbenchKeybindingState state;
	const auto begin = [&] {
		EXPECT_EQ(EWorkbenchKeyInputDecision::BeginOrRestartChordAndConsume,
			state.HandleKeyDown('K', controlOnly, 500, 1, available).decision);
	};
	begin();
	EXPECT_EQ(EWorkbenchKeyInputDecision::CancelChordAndConsume,
		state.HandleKeyDown('X', controlOnly, 501, 1, available).decision);
	EXPECT_FALSE(state.IsChordPending());
	begin();
	EXPECT_EQ(EWorkbenchKeyInputDecision::CancelChordAndConsume,
		state.HandleKeyDown('O', {}, 601, 1, available).decision);
	begin();
	EXPECT_EQ(EWorkbenchKeyInputDecision::CancelChordAndConsume,
		state.HandleKeyDown('O', { .control = true, .shift = true }, 701, 1, available).decision);
	begin();
	EXPECT_EQ(EWorkbenchKeyInputDecision::CancelChordAndConsume,
		state.HandleKeyDown('S', controlOnly, 801, 1, available).decision);
	begin();
	EXPECT_EQ(EWorkbenchKeyInputDecision::CancelChordAndConsume,
		state.HandleKeyDown('F', { .alt = true }, 901, 1, available).decision);
}

TEST(WorkbenchKeybindingState, RepeatedFirstStrokeRestartsAndRenewsTheTimeout)
{
	const auto available = [](std::string_view) { return true; };
	WorkbenchKeybindingState state;
	const WorkbenchKeyModifiers controlOnly{ .control = true };
	EXPECT_EQ(EWorkbenchKeyInputDecision::BeginOrRestartChordAndConsume,
		state.HandleKeyDown('K', controlOnly, 500, 11, available).decision);
	EXPECT_EQ(EWorkbenchKeyInputDecision::BeginOrRestartChordAndConsume,
		state.HandleKeyDown('K', controlOnly, 700, 11, available).decision);
	EXPECT_FALSE(state.ExpireIfNeeded(700 + CtrlKChordState::TimeoutMs - 1));
	EXPECT_TRUE(state.ExpireIfNeeded(700 + CtrlKChordState::TimeoutMs));
	EXPECT_FALSE(state.IsChordPending());
}

TEST(WorkbenchKeybindingState, FocusCancellationAndExactDirectBindingsAreModeledWithoutAnHwnd)
{
	const auto available = [](std::string_view) { return true; };
	WorkbenchKeybindingState state;
	const WorkbenchKeyModifiers controlOnly{ .control = true };
	EXPECT_EQ(EWorkbenchKeyInputDecision::BeginOrRestartChordAndConsume,
		state.HandleKeyDown('K', controlOnly, 100, 11, available).decision);
	EXPECT_TRUE(state.CancelIfFocusChanged(12));
	EXPECT_FALSE(state.IsChordPending());

	auto result = state.HandleKeyDown('R', controlOnly, 200, 12, available);
	EXPECT_EQ(EWorkbenchKeyInputDecision::ExecuteStableCommandAndConsume, result.decision);
	EXPECT_EQ(command_ids::OpenRecent, result.commandId);
	result = state.HandleKeyDown('W', controlOnly, 201, 12, available);
	EXPECT_EQ(EWorkbenchKeyInputDecision::ExecuteStableCommandAndConsume, result.decision);
	EXPECT_EQ(command_ids::CloseActiveEditor, result.commandId);
	result = state.HandleKeyDown('V', { .control = true, .shift = true }, 202, 12, available);
	EXPECT_EQ(EWorkbenchKeyInputDecision::ExecuteStableCommandAndConsume, result.decision);
	EXPECT_EQ(command_ids::MarkdownTogglePreview, result.commandId);
	EXPECT_EQ(EWorkbenchKeyInputDecision::PassThrough,
		state.HandleKeyDown('R', { .control = true, .shift = true }, 203, 12, available).decision);
	EXPECT_EQ(EWorkbenchKeyInputDecision::PassThrough,
		state.HandleKeyDown('W', { .alt = true }, 204, 12, available).decision);
}

TEST(WorkbenchKeybindingState, RegisteredDisabledCommandIsStillATerminalConsumedBinding)
{
	using workbench::commands::EWorkbenchCommandExecutionStatus;
	using workbench::commands::EWorkbenchCommandRegistrationStatus;
	using workbench::commands::WorkbenchCommandRegistry;
	using workbench::commands::WorkbenchContextKeySnapshot;

	WorkbenchCommandRegistry registry;
	ASSERT_EQ(EWorkbenchCommandRegistrationStatus::Succeeded, registry.RegisterBuiltinCommands().status);
	WorkbenchKeybindingState state;
	const auto result = state.HandleKeyDown('W', { .control = true }, 100, 1,
		[&registry](std::string_view commandId) { return registry.Find(commandId).has_value(); });
	EXPECT_EQ(EWorkbenchKeyInputDecision::ExecuteStableCommandAndConsume, result.decision);
	EXPECT_EQ(command_ids::CloseActiveEditor, result.commandId);

	WorkbenchContextKeySnapshot disabled;
	disabled.values.emplace("workbenchReady", true);
	EXPECT_EQ(EWorkbenchCommandExecutionStatus::Disabled, registry.Execute(result.commandId, disabled).status);
	// The model's explicit consumed decision is the contract used by CEditWnd:
	// terminal registry outcomes never reach TranslateAccelerator afterward.
	EXPECT_EQ(EWorkbenchKeyInputDecision::ExecuteStableCommandAndConsume, result.decision);
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

TEST(EmptyEditorSurfaceModel, CentersTheActionListAloneWithNoLetterpressColumn)
{
	EmptyEditorSurfaceModel model;
	model.SetViewport(800, 600, 96);
	const auto first = model.GetAction(0);
	const auto last = model.GetAction(model.GetActionCount() - 1);

	// This fork does not draw upstream's `.letterpress`, so the action list is the whole
	// centered column: the space above it matches the space below it.
	EXPECT_EQ(600 - last.bounds.bottom, first.bounds.top);
	// Five 28 DIP rows separated by 4 DIP gaps center at (600 - 156) / 2.
	EXPECT_EQ(222, first.bounds.top);
	EXPECT_EQ(28, first.bounds.Height());
}

TEST(EmptyEditorSurfaceModel, KeepsTheActionListCenteredInAShortViewport)
{
	EmptyEditorSurfaceModel model;
	model.SetViewport(800, 200, 96);
	EXPECT_EQ(22, model.GetAction(0).bounds.top);
	EXPECT_EQ(28, model.GetAction(0).bounds.Height());
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

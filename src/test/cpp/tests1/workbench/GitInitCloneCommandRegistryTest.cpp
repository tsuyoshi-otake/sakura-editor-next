/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include "workbench/commands/WorkbenchCommandRegistry.h"

#include <string>
#include <string_view>

//!
//! @brief Registry coverage for `git.init` and `git.clone`.
//!
//! Every other entry `RegisterGitCommands` registers requires an open
//! repository (`gitOpenRepositoryCount != 0`); `git.init` and `git.clone` are
//! the two commands whose entire purpose is to create that repository state,
//! so they are the one pair in the batch gated on `workbenchReady` alone
//! (`MakeGitAlwaysAvailableDescriptor` in `WorkbenchCommandRegistry.cpp`).
//! This file is a distinct suite from `WorkbenchCommandRegistryTest.cpp`
//! (out of this pass's edit scope) so the pair's own contract has a home that
//! does not require touching a file this pass may not modify. See
//! `sakura_core/workbench/scm/CLAUDE.md`'s "Wiring `git.init` and `git.clone`
//! into the Command Registry" section for the source of these expectations.
//!

using workbench::commands::EWorkbenchCommandExecutionStatus;
using workbench::commands::EWorkbenchCommandRegistrationStatus;
using workbench::commands::EWorkbenchCommandSurface;
using workbench::commands::WorkbenchCommandExecutionResult;
using workbench::commands::WorkbenchCommandRegistry;
using workbench::commands::WorkbenchContextKeySnapshot;
using workbench::commands::WorkbenchGitCommandExecutors;

namespace {

//! `workbenchReady` alone, exactly the clause `git.init`/`git.clone` carry -
//! deliberately without `gitOpenRepositoryCount`, so a context that would
//! leave every other Git command `NotApplicable` still lets these two run.
WorkbenchContextKeySnapshot WorkbenchReadyWithNoRepositoryContext()
{
	WorkbenchContextKeySnapshot context;
	context.values.emplace("workbenchReady", true);
	context.values.emplace("gitOpenRepositoryCount", std::int64_t{ 0 });
	return context;
}

WorkbenchContextKeySnapshot NotReadyContext()
{
	return {};
}

WorkbenchCommandExecutionResult Succeeded()
{
	return { EWorkbenchCommandExecutionStatus::Succeeded, {} };
}

} // namespace

TEST(GitInitCloneCommandRegistry, GitInitAndGitCloneCarryUpstreamIdsTitlesAndTheWorkbenchReadyClause)
{
	WorkbenchCommandRegistry registry;
	ASSERT_EQ(EWorkbenchCommandRegistrationStatus::Succeeded, registry.RegisterGitCommands({}).status);

	// `command.init` / `command.clone` in `extensions/git/package.nls.json`,
	// prefixed with the `Git` category `package.json` declares for both.
	const auto init = registry.Find("git.init");
	ASSERT_TRUE(init.has_value());
	EXPECT_EQ("Git: Initialize Repository", init->title);
	// Unlike `MakeGitDescriptor`'s `gitOpenRepositoryCount != 0`, both clauses
	// here are `workbenchReady` alone - the pair is available precisely when
	// no repository is open yet.
	EXPECT_EQ("workbenchReady", init->whenClause);
	EXPECT_EQ("workbenchReady", init->enablementClause);

	const auto clone = registry.Find("git.clone");
	ASSERT_TRUE(clone.has_value());
	EXPECT_EQ("Git: Clone", clone->title);
	EXPECT_EQ("workbenchReady", clone->whenClause);
	EXPECT_EQ("workbenchReady", clone->enablementClause);

	// Unlike the API commands (`vscode.diff`/`vscode.open`), these are real
	// Command Palette entries upstream, so both keep the palette surface an
	// API command deliberately omits.
	for (const auto* commandId : { "git.init", "git.clone" }) {
		const auto slot = registry.ResolveSurface(
			EWorkbenchCommandSurface::CommandPalette, std::string(commandId) + ".palette");
		ASSERT_TRUE(slot.has_value()) << commandId;
		EXPECT_EQ(commandId, slot->commandId) << commandId;
	}
}

TEST(GitInitCloneCommandRegistry, BothCommandsRunWithNoRepositoryOpenUnlikeEveryOtherGitCommand)
{
	WorkbenchCommandRegistry registry;
	int initCalls = 0;
	int cloneCalls = 0;
	ASSERT_EQ(EWorkbenchCommandRegistrationStatus::Succeeded, registry.RegisterGitCommands({
		.init = [&initCalls](std::string_view) { ++initCalls; return Succeeded(); },
		.clone = [&cloneCalls] { ++cloneCalls; return Succeeded(); },
	}).status);

	const auto context = WorkbenchReadyWithNoRepositoryContext();
	EXPECT_EQ(EWorkbenchCommandExecutionStatus::Succeeded, registry.Execute("git.init", context).status);
	EXPECT_EQ(EWorkbenchCommandExecutionStatus::Succeeded, registry.Execute("git.clone", context).status);
	EXPECT_EQ(1, initCalls);
	EXPECT_EQ(1, cloneCalls);
}

TEST(GitInitCloneCommandRegistry, GitInitReceivesTheWelcomeContentSkipFolderPromptArgumentVerbatim)
{
	WorkbenchCommandRegistry registry;
	std::string received;
	ASSERT_EQ(EWorkbenchCommandRegistrationStatus::Succeeded, registry.RegisterGitCommands({
		.init = [&received](std::string_view arguments) { received = arguments; return Succeeded(); },
	}).status);

	const auto context = WorkbenchReadyWithNoRepositoryContext();
	// The Source Control welcome content's `Initialize Repository` button
	// dispatches `git.init` with `argumentsJson == "[true]"` - see
	// `GitInitCloneCommands.h`'s `ParseGitInitSkipFolderPromptArgument` doc
	// comment and `CScmWorkbenchTool`'s welcome-content action table.
	EXPECT_EQ(EWorkbenchCommandExecutionStatus::Succeeded,
		registry.Execute("git.init", context, "[true]").status);
	EXPECT_EQ("[true]", received);

	// A Command Palette invocation carries no arguments at all.
	EXPECT_EQ(EWorkbenchCommandExecutionStatus::Succeeded, registry.Execute("git.init", context).status);
	EXPECT_TRUE(received.empty());
}

TEST(GitInitCloneCommandRegistry, GitCloneIgnoresAnArgumentsPayloadExactlyAsAnArgumentLessUpstreamHandlerWould)
{
	WorkbenchCommandRegistry registry;
	int cloneCalls = 0;
	ASSERT_EQ(EWorkbenchCommandRegistrationStatus::Succeeded, registry.RegisterGitCommands({
		.clone = [&cloneCalls] { ++cloneCalls; return Succeeded(); },
	}).status);

	const auto context = WorkbenchReadyWithNoRepositoryContext();
	// `git.clone` takes no argument, exactly like upstream's own zero-
	// parameter handler; a stray payload must not change the outcome.
	EXPECT_EQ(EWorkbenchCommandExecutionStatus::Succeeded,
		registry.Execute("git.clone", context, R"({"unexpected":true})").status);
	EXPECT_EQ(1, cloneCalls);
}

TEST(GitInitCloneCommandRegistry, UnboundGitInitAndGitCloneAreUnsupportedRatherThanASilentNoOp)
{
	WorkbenchCommandRegistry registry;
	ASSERT_EQ(EWorkbenchCommandRegistrationStatus::Succeeded, registry.RegisterGitCommands({}).status);

	const auto context = WorkbenchReadyWithNoRepositoryContext();
	for (const auto* commandId : { "git.init", "git.clone" }) {
		EXPECT_EQ(EWorkbenchCommandExecutionStatus::Unsupported,
			registry.Execute(commandId, context).status) << commandId;
	}
}

TEST(GitInitCloneCommandRegistry, BothCommandsAreNotApplicableBeforeTheWorkbenchIsReady)
{
	WorkbenchCommandRegistry registry;
	ASSERT_EQ(EWorkbenchCommandRegistrationStatus::Succeeded, registry.RegisterGitCommands({
		.init = [](std::string_view) { return Succeeded(); },
		.clone = [] { return Succeeded(); },
	}).status);

	const auto context = NotReadyContext();
	for (const auto* commandId : { "git.init", "git.clone" }) {
		EXPECT_EQ(EWorkbenchCommandExecutionStatus::NotApplicable,
			registry.Execute(commandId, context).status) << commandId;
	}
}

TEST(GitInitCloneCommandRegistry, GitInitAndGitCloneRegisterInTheSameAtomicBatchAsEveryOtherGitCommand)
{
	WorkbenchCommandRegistry registry;
	// `RegisterGitCommands` is one atomic batch; `git.checkout` is an arbitrary
	// other member of that same batch used here only to prove the two new
	// entries did not require - and did not get - a second, separate call.
	ASSERT_EQ(EWorkbenchCommandRegistrationStatus::Succeeded, registry.RegisterGitCommands({}).status);
	EXPECT_TRUE(registry.Find("git.init").has_value());
	EXPECT_TRUE(registry.Find("git.clone").has_value());
	EXPECT_TRUE(registry.Find("git.checkout").has_value());
}

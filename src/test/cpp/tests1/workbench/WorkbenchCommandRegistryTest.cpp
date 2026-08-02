/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include "workbench/commands/WorkbenchCommandRegistry.h"

#include "workbench/layout/WorkbenchIds.h"

#include <array>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace {

using workbench::commands::EWorkbenchCommandExecutionStatus;
using workbench::commands::EWorkbenchCommandExecutorTarget;
using workbench::commands::EWorkbenchCommandRegistrationStatus;
using workbench::commands::EWorkbenchCommandSurface;
using workbench::commands::EWorkbenchContextMutationStatus;
using workbench::commands::WorkbenchCommandDescriptor;
using workbench::commands::WorkbenchCommandOwner;
using workbench::commands::WorkbenchCommandRegistry;
using workbench::commands::WorkbenchContextKeyService;
using workbench::commands::WorkbenchContextKeySnapshot;
using workbench::commands::WorkbenchContextValue;
using workbench::commands::WorkbenchWhenClauseEvaluator;
using workbench::layout::EWorkbenchPartPosition;
using workbench::layout::WorkbenchLayoutStateSnapshot;

const WorkbenchCommandOwner kExtensionGenerationOne{ "publisher.extension", 1 };
const WorkbenchCommandOwner kExtensionGenerationTwo{ "publisher.extension", 2 };

WorkbenchCommandDescriptor SampleDescriptor(std::string id = "publisher.extension.run")
{
	return {
		std::move(id),
		"Extension Run",
		kExtensionGenerationOne,
		"workbenchReady",
		"extension.enabled",
		EWorkbenchCommandExecutorTarget::ExtensionHost,
		{ { EWorkbenchCommandSurface::CommandPalette, "publisher.extension.run.palette", std::nullopt } },
	};
}

WorkbenchContextKeySnapshot EnabledContext()
{
	WorkbenchContextKeySnapshot context;
	context.values.emplace("workbenchReady", true);
	context.values.emplace("extension.enabled", true);
	context.values.emplace("resource.langId", std::string("cpp"));
	return context;
}

} // namespace

TEST(WorkbenchContextKeyService, ProjectsLayoutKeysAsOneCoreSnapshot)
{
	WorkbenchLayoutStateSnapshot layout;
	layout.parts = { { std::string(workbench::layout::ids::part::Sidebar), true, EWorkbenchPartPosition::Left, 300 } };
	layout.activeContainers.sideBar = std::string(workbench::layout::ids::viewContainer::Explorer);
	layout.containers = { { std::string(workbench::layout::ids::viewContainer::Explorer),
		workbench::layout::EWorkbenchViewContainerLocation::SideBar, 0, true,
		std::string(workbench::layout::ids::view::Explorer) } };

	WorkbenchContextKeyService service;
	const auto result = service.SetCoreProjection(layout);
	const auto snapshot = service.Snapshot();

	EXPECT_EQ(EWorkbenchContextMutationStatus::Succeeded, result.status);
	EXPECT_EQ(result.revision, snapshot.revision);
	EXPECT_EQ(WorkbenchContextValue(true), snapshot.values.at("workbenchReady"));
	EXPECT_EQ(WorkbenchContextValue(true), snapshot.values.at("workbench.sidebarVisible"));
	EXPECT_EQ(WorkbenchContextValue(std::string(workbench::layout::ids::view::Explorer)), snapshot.values.at("workbench.activeView"));
	EXPECT_EQ(WorkbenchContextValue(true), snapshot.values.at("workbench.explorerActive"));
}

TEST(WorkbenchContextKeyService, CoreNamespaceRejectsExtensionWritesAndRequiresExactGenerationDisposal)
{
	WorkbenchContextKeyService service;
	EXPECT_EQ(EWorkbenchContextMutationStatus::Invalid,
		service.SetExtensionOverlay(kExtensionGenerationOne, { { "workbench.sidebarVisible", false } }).status);
	ASSERT_EQ(EWorkbenchContextMutationStatus::Succeeded,
		service.SetExtensionOverlay(kExtensionGenerationOne, { { "extension.enabled", true } }).status);
	EXPECT_EQ(EWorkbenchContextMutationStatus::NotApplicable, service.DisposeExtensionOverlay(kExtensionGenerationTwo).status);
	EXPECT_TRUE(std::get<bool>(service.Snapshot().values.at("extension.enabled")));
	EXPECT_EQ(EWorkbenchContextMutationStatus::Succeeded, service.DisposeExtensionOverlay(kExtensionGenerationOne).status);
	EXPECT_FALSE(service.Snapshot().values.contains("extension.enabled"));
}

TEST(WorkbenchWhenClauseEvaluator, SupportsBoundedBooleanComparisonAndRegexSubsetFailClosed)
{
	const auto context = EnabledContext();
	EXPECT_TRUE(WorkbenchWhenClauseEvaluator::Evaluate(
		"workbenchReady && (extension.enabled || resource.langId == 'text')", context));
	EXPECT_TRUE(WorkbenchWhenClauseEvaluator::Evaluate("resource.langId =~ '^c..$'", context));
	EXPECT_TRUE(WorkbenchWhenClauseEvaluator::Evaluate("!missing && resource.langId != 'text'", context));
	EXPECT_FALSE(WorkbenchWhenClauseEvaluator::Evaluate("resource.langId =~ '['", context));
	EXPECT_FALSE(WorkbenchWhenClauseEvaluator::Evaluate("workbenchReady &&", context));
}

TEST(WorkbenchCommandRegistry, BuiltinsResolveEverySurfaceToTheSameStableCommandId)
{
	WorkbenchCommandRegistry registry;
	ASSERT_EQ(EWorkbenchCommandRegistrationStatus::Succeeded, registry.RegisterBuiltinCommands().status);
	const auto iconTheme = registry.Find("workbench.action.selectIconTheme");
	ASSERT_TRUE(iconTheme.has_value());
	EXPECT_EQ("Preferences: File Icon Theme", iconTheme->title);
	EXPECT_EQ("workbench.action.selectIconTheme.palette",
		iconTheme->surfaceBindings.front().slotId);
	const auto colorTheme = registry.Find("workbench.action.selectTheme");
	ASSERT_TRUE(colorTheme.has_value());
	EXPECT_EQ("Preferences: Color Theme", colorTheme->title);
	EXPECT_EQ("workbench.action.selectTheme.palette",
		colorTheme->surfaceBindings.front().slotId);
	EXPECT_FALSE(registry.Find("workbench.action.selectColorTheme").has_value());

	const std::array<std::pair<EWorkbenchCommandSurface, std::string_view>, 4> bindings = {
		std::pair{ EWorkbenchCommandSurface::CommandPalette, "workbench.view.explorer.palette" },
		std::pair{ EWorkbenchCommandSurface::Menu, "workbench.view.explorer.menu" },
		std::pair{ EWorkbenchCommandSurface::ActivityBar, "workbench.view.explorer.activity" },
		std::pair{ EWorkbenchCommandSurface::Keybinding, "workbench.view.explorer.key" },
	};
	for (const auto& [surface, slot] : bindings) {
		const auto resolved = registry.ResolveSurface(surface, slot);
		ASSERT_TRUE(resolved.has_value());
		EXPECT_EQ("workbench.view.explorer", resolved->commandId);
		if (surface == EWorkbenchCommandSurface::Menu || surface == EWorkbenchCommandSurface::Keybinding) {
			ASSERT_TRUE(resolved->binding.legacyFunctionCode.has_value());
			EXPECT_EQ(30991, *resolved->binding.legacyFunctionCode);
		} else {
			EXPECT_FALSE(resolved->binding.legacyFunctionCode.has_value());
		}
	}
	EXPECT_EQ(EWorkbenchCommandRegistrationStatus::Conflict, registry.RegisterBuiltinCommands().status);
}

TEST(WorkbenchCommandRegistry, ManageMenuSurfacesResolveToTheirCanonicalVsCodeCommands)
{
	WorkbenchCommandRegistry registry;
	ASSERT_EQ(EWorkbenchCommandRegistrationStatus::Succeeded, registry.RegisterBuiltinCommands().status);
	const std::array<std::pair<std::string_view, std::string_view>, 6> bindings = {
		std::pair{ "workbench.manage.commandPalette", "workbench.action.showCommands" },
		std::pair{ "workbench.manage.settings", "workbench.action.openSettings" },
		std::pair{ "workbench.manage.extensions", "workbench.view.extensions" },
		std::pair{ "workbench.manage.keybindings", "workbench.action.openGlobalKeybindings" },
		std::pair{ "workbench.manage.colorTheme", "workbench.action.selectTheme" },
		std::pair{ "workbench.manage.fileIconTheme", "workbench.action.selectIconTheme" },
	};
	for (const auto& [slot, commandId] : bindings) {
		const auto resolved = registry.ResolveSurface(EWorkbenchCommandSurface::Menu, slot);
		ASSERT_TRUE(resolved.has_value());
		EXPECT_EQ(commandId, resolved->commandId);
		EXPECT_FALSE(resolved->binding.legacyFunctionCode.has_value());
	}
}

TEST(WorkbenchCommandRegistry, ProblemsAndOutputUseCanonicalIdsAcrossTheirSupportedSurfaces)
{
	WorkbenchCommandRegistry registry;
	ASSERT_EQ(EWorkbenchCommandRegistrationStatus::Succeeded, registry.RegisterBuiltinCommands().status);

	const std::array<std::pair<EWorkbenchCommandSurface, std::string_view>, 3> problemsBindings = {
		std::pair{ EWorkbenchCommandSurface::CommandPalette, "workbench.actions.view.problems.palette" },
		std::pair{ EWorkbenchCommandSurface::Menu, "workbench.actions.view.problems.menu" },
		std::pair{ EWorkbenchCommandSurface::Keybinding, "workbench.actions.view.problems.key" },
	};
	for (const auto& [surface, slot] : problemsBindings) {
		const auto resolved = registry.ResolveSurface(surface, slot);
		ASSERT_TRUE(resolved.has_value());
		EXPECT_EQ("workbench.actions.view.problems", resolved->commandId);
		EXPECT_FALSE(resolved->binding.legacyFunctionCode.has_value());
	}

	const std::array<std::pair<EWorkbenchCommandSurface, std::string_view>, 3> outputBindings = {
		std::pair{ EWorkbenchCommandSurface::CommandPalette, "workbench.action.output.toggleOutput.palette" },
		std::pair{ EWorkbenchCommandSurface::Menu, "workbench.action.output.toggleOutput.menu" },
		std::pair{ EWorkbenchCommandSurface::Keybinding, "workbench.action.output.toggleOutput.key" },
	};
	for (const auto& [surface, slot] : outputBindings) {
		const auto resolved = registry.ResolveSurface(surface, slot);
		ASSERT_TRUE(resolved.has_value());
		EXPECT_EQ("workbench.action.output.toggleOutput", resolved->commandId);
		EXPECT_FALSE(resolved->binding.legacyFunctionCode.has_value());
	}

	EXPECT_FALSE(registry.ResolveSurface(EWorkbenchCommandSurface::ActivityBar,
		"workbench.actions.view.problems.activity").has_value());
	EXPECT_FALSE(registry.ResolveSurface(EWorkbenchCommandSurface::ActivityBar,
		"workbench.action.output.toggleOutput.activity").has_value());
}

TEST(WorkbenchCommandRegistry, BuiltinExecutorsAreBoundAtomicallyAndReachTypedSuccess)
{
	WorkbenchCommandRegistry registry;
	int sidebarCalls{};
	int explorerCalls{};
	int problemsCalls{};
	int outputCalls{};
	ASSERT_EQ(EWorkbenchCommandRegistrationStatus::Succeeded, registry.RegisterBuiltinCommands({
		.toggleSidebarVisibility = [&sidebarCalls] {
			++sidebarCalls;
			return workbench::commands::WorkbenchCommandExecutionResult{
				EWorkbenchCommandExecutionStatus::Succeeded, {} };
		},
		.showExplorer = [&explorerCalls] {
			++explorerCalls;
			return workbench::commands::WorkbenchCommandExecutionResult{
				EWorkbenchCommandExecutionStatus::Succeeded, {} };
		},
		.showProblems = [&problemsCalls] {
			++problemsCalls;
			return workbench::commands::WorkbenchCommandExecutionResult{
				EWorkbenchCommandExecutionStatus::Succeeded, {} };
		},
		.toggleOutput = [&outputCalls] {
			++outputCalls;
			return workbench::commands::WorkbenchCommandExecutionResult{
				EWorkbenchCommandExecutionStatus::Succeeded, {} };
		},
	}).status);
	const auto context = EnabledContext();
	EXPECT_EQ(EWorkbenchCommandExecutionStatus::Succeeded,
		registry.Execute("workbench.action.toggleSidebarVisibility", context).status);
	EXPECT_EQ(EWorkbenchCommandExecutionStatus::Succeeded,
		registry.Execute("workbench.view.explorer", context).status);
	EXPECT_EQ(EWorkbenchCommandExecutionStatus::Succeeded,
		registry.Execute("workbench.actions.view.problems", context).status);
	EXPECT_EQ(EWorkbenchCommandExecutionStatus::Succeeded,
		registry.Execute("workbench.action.output.toggleOutput", context).status);
	EXPECT_EQ(1, sidebarCalls);
	EXPECT_EQ(1, explorerCalls);
	EXPECT_EQ(1, problemsCalls);
	EXPECT_EQ(1, outputCalls);
}

TEST(WorkbenchCommandRegistry, ManageExecutorsReachOnlyTheirBoundStableCommands)
{
	WorkbenchCommandRegistry registry;
	int commandPaletteCalls{};
	int settingsCalls{};
	int extensionsCalls{};
	int keybindingsCalls{};
	int colorThemeCalls{};
	int fileIconThemeCalls{};
	ASSERT_EQ(EWorkbenchCommandRegistrationStatus::Succeeded, registry.RegisterBuiltinCommands({
		.showCommands = [&commandPaletteCalls] {
			++commandPaletteCalls;
			return workbench::commands::WorkbenchCommandExecutionResult{
				EWorkbenchCommandExecutionStatus::Succeeded, {} };
		},
		.openSettings = [&settingsCalls] {
			++settingsCalls;
			return workbench::commands::WorkbenchCommandExecutionResult{
				EWorkbenchCommandExecutionStatus::Succeeded, {} };
		},
		.showExtensions = [&extensionsCalls] {
			++extensionsCalls;
			return workbench::commands::WorkbenchCommandExecutionResult{
				EWorkbenchCommandExecutionStatus::Succeeded, {} };
		},
		.openGlobalKeybindings = [&keybindingsCalls] {
			++keybindingsCalls;
			return workbench::commands::WorkbenchCommandExecutionResult{
				EWorkbenchCommandExecutionStatus::Succeeded, {} };
		},
		.selectTheme = [&colorThemeCalls] {
			++colorThemeCalls;
			return workbench::commands::WorkbenchCommandExecutionResult{
				EWorkbenchCommandExecutionStatus::Succeeded, {} };
		},
		.selectFileIconTheme = [&fileIconThemeCalls] {
			++fileIconThemeCalls;
			return workbench::commands::WorkbenchCommandExecutionResult{
				EWorkbenchCommandExecutionStatus::Succeeded, {} };
		},
	}).status);

	const auto context = EnabledContext();
	for (const auto commandId : {
		"workbench.action.showCommands",
		"workbench.action.openSettings",
		"workbench.view.extensions",
		"workbench.action.openGlobalKeybindings",
		"workbench.action.selectTheme",
		"workbench.action.selectIconTheme",
	}) {
		EXPECT_EQ(EWorkbenchCommandExecutionStatus::Succeeded, registry.Execute(commandId, context).status);
	}
	EXPECT_EQ(1, commandPaletteCalls);
	EXPECT_EQ(1, settingsCalls);
	EXPECT_EQ(1, extensionsCalls);
	EXPECT_EQ(1, keybindingsCalls);
	EXPECT_EQ(1, colorThemeCalls);
	EXPECT_EQ(1, fileIconThemeCalls);
}

TEST(WorkbenchCommandRegistry, DuplicateAndExactOwnerGenerationDisposalAreTerminal)
{
	WorkbenchCommandRegistry registry;
	ASSERT_EQ(EWorkbenchCommandRegistrationStatus::Succeeded, registry.Register(SampleDescriptor()).status);
	EXPECT_EQ(EWorkbenchCommandRegistrationStatus::Conflict, registry.Register(SampleDescriptor()).status);
	EXPECT_EQ(EWorkbenchCommandRegistrationStatus::NotApplicable, registry.DisposeOwner(kExtensionGenerationTwo).status);
	ASSERT_TRUE(registry.Find("publisher.extension.run").has_value());
	EXPECT_EQ(EWorkbenchCommandRegistrationStatus::Succeeded, registry.DisposeOwner(kExtensionGenerationOne).status);
	EXPECT_FALSE(registry.Find("publisher.extension.run").has_value());
}

TEST(WorkbenchCommandRegistry, DisabledCommandNeverInvokesExecutorAndUnknownUnsupportedAndThrownAreTyped)
{
	WorkbenchCommandRegistry registry;
	int calls{};
	ASSERT_EQ(EWorkbenchCommandRegistrationStatus::Succeeded,
		registry.Register(SampleDescriptor(), [&calls] {
			++calls;
			return workbench::commands::WorkbenchCommandExecutionResult{ EWorkbenchCommandExecutionStatus::Succeeded, {} };
		}).status);

	auto disabled = EnabledContext();
	disabled.values.insert_or_assign("extension.enabled", false);
	EXPECT_EQ(EWorkbenchCommandExecutionStatus::Disabled, registry.Execute("publisher.extension.run", disabled).status);
	EXPECT_EQ(0, calls);
	EXPECT_EQ(EWorkbenchCommandExecutionStatus::UnknownCommand, registry.Execute("missing.command", EnabledContext()).status);

	ASSERT_EQ(EWorkbenchCommandRegistrationStatus::Succeeded, registry.RegisterBuiltinCommands().status);
	EXPECT_EQ(EWorkbenchCommandExecutionStatus::Unsupported,
		registry.Execute("workbench.view.explorer", EnabledContext()).status);
	EXPECT_EQ(EWorkbenchCommandExecutionStatus::Unsupported,
		registry.Execute("workbench.actions.view.problems", EnabledContext()).status);
	EXPECT_EQ(EWorkbenchCommandExecutionStatus::Unsupported,
		registry.Execute("workbench.action.output.toggleOutput", EnabledContext()).status);

	WorkbenchContextKeySnapshot unavailable;
	EXPECT_EQ(EWorkbenchCommandExecutionStatus::NotApplicable,
		registry.Execute("workbench.actions.view.problems", unavailable).status);

	WorkbenchCommandRegistry failedBuiltin;
	ASSERT_EQ(EWorkbenchCommandRegistrationStatus::Succeeded, failedBuiltin.RegisterBuiltinCommands({
		.toggleOutput = [] {
			return workbench::commands::WorkbenchCommandExecutionResult{
				EWorkbenchCommandExecutionStatus::Failed, "projection failed" };
		},
	}).status);
	EXPECT_EQ(EWorkbenchCommandExecutionStatus::Failed,
		failedBuiltin.Execute("workbench.action.output.toggleOutput", EnabledContext()).status);

	auto throwing = SampleDescriptor("publisher.extension.throwing");
	throwing.surfaceBindings[0].slotId = "publisher.extension.throwing.palette";
	ASSERT_EQ(EWorkbenchCommandRegistrationStatus::Succeeded, registry.Register(std::move(throwing), []() -> workbench::commands::WorkbenchCommandExecutionResult {
		throw std::runtime_error("test");
	}).status);
	EXPECT_EQ(EWorkbenchCommandExecutionStatus::Failed, registry.Execute("publisher.extension.throwing", EnabledContext()).status);
}

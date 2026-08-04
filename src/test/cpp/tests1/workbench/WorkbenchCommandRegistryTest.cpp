/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include "workbench/commands/WorkbenchCommandRegistry.h"
#include "workbench/editor/WorkbenchCommandPaletteModel.h"
#include "Funccode_enum.h"

#include "workbench/layout/WorkbenchIds.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {

using workbench::commands::EWorkbenchCommandExecutionStatus;
using workbench::commands::EWorkbenchCommandExecutorTarget;
using workbench::commands::EWorkbenchCommandRegistrationStatus;
using workbench::commands::EWorkbenchCommandSurface;
using workbench::commands::EWorkbenchContextMutationStatus;
using workbench::commands::WorkbenchCommandDescriptor;
using workbench::commands::WorkbenchCommandExecutionResult;
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

WorkbenchCommandDescriptor NativePaletteDescriptor()
{
	return {
		"sakura.test.nativePaletteCommand",
		"Native Palette Command",
		{ "sakura.test", 1 },
		"workbenchReady",
		"workbenchReady",
		EWorkbenchCommandExecutorTarget::Editor,
		{ { EWorkbenchCommandSurface::CommandPalette, "sakura.test.nativePaletteCommand.palette", std::nullopt } },
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

WorkbenchContextKeySnapshot EnabledFileCommandContext(std::string workbenchState = "folder")
{
	auto context = EnabledContext();
	context.values.insert_or_assign("editorHasActiveEditor", true);
	context.values.insert_or_assign("editorIsDirty", true);
	context.values.insert_or_assign("workbenchState", std::move(workbenchState));
	context.values.insert_or_assign("workspaceFolderCount", std::int64_t{ 1 });
	context.values.insert_or_assign("workbench.recentlyOpenedAvailable", true);
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
	for (const auto key : {
		std::string_view("workbench.sidebarVisible"),
		std::string_view("workbenchState"),
		std::string_view("workspaceFolderCount"),
		std::string_view("editorHasActiveEditor"),
		std::string_view("editorIsDirty"),
	}) {
		EXPECT_EQ(EWorkbenchContextMutationStatus::Invalid,
			service.SetExtensionOverlay(kExtensionGenerationOne,
				{ { std::string(key), WorkbenchContextValue(false) } }).status) << key;
	}

	EXPECT_TRUE(WorkbenchContextKeyService::IsReservedCoreKey("workbenchState"));
	EXPECT_TRUE(WorkbenchContextKeyService::IsReservedCoreKey("workspaceFolderCount"));
	EXPECT_TRUE(WorkbenchContextKeyService::IsReservedCoreKey("editorHasActiveEditor"));
	EXPECT_TRUE(WorkbenchContextKeyService::IsReservedCoreKey("editorIsDirty"));
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

TEST(WorkbenchWhenClauseEvaluator, ComparesIntegerContextKeysWithoutChangingBooleanOrStringSemantics)
{
	auto context = EnabledContext();
	context.values.emplace("workspaceFolderCount", std::int64_t{ 2 });

	EXPECT_TRUE(WorkbenchWhenClauseEvaluator::Evaluate("workspaceFolderCount == 2", context));
	EXPECT_TRUE(WorkbenchWhenClauseEvaluator::Evaluate("workspaceFolderCount != 1", context));
	EXPECT_TRUE(WorkbenchWhenClauseEvaluator::Evaluate("workspaceFolderCount > 1 && workspaceFolderCount <= 2", context));
	EXPECT_TRUE(WorkbenchWhenClauseEvaluator::Evaluate("workspaceFolderCount >= 2 && workspaceFolderCount < 3", context));
	EXPECT_TRUE(WorkbenchWhenClauseEvaluator::Evaluate("workspaceFolderCount > -1", context));
	EXPECT_FALSE(WorkbenchWhenClauseEvaluator::Evaluate("workspaceFolderCount == '2'", context));
	EXPECT_FALSE(WorkbenchWhenClauseEvaluator::Evaluate("workspaceFolderCount < '3'", context));
	EXPECT_TRUE(WorkbenchWhenClauseEvaluator::Evaluate("extension.enabled && resource.langId == 'cpp'", context));
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
	const auto openFolder = registry.Find("workbench.action.files.openFolder");
	ASSERT_TRUE(openFolder.has_value());
	EXPECT_EQ("Open Folder...", openFolder->title);
	EXPECT_EQ(EWorkbenchCommandExecutorTarget::Editor, openFolder->executorTarget);
	const std::array<std::pair<EWorkbenchCommandSurface, std::string_view>, 3> openFolderBindings = {
		std::pair{ EWorkbenchCommandSurface::CommandPalette, "workbench.action.files.openFolder.palette" },
		std::pair{ EWorkbenchCommandSurface::Menu, "workbench.action.files.openFolder.menu" },
		std::pair{ EWorkbenchCommandSurface::Keybinding, "workbench.action.files.openFolder.key" },
	};
	for (const auto& [surface, slot] : openFolderBindings) {
		const auto resolved = registry.ResolveSurface(surface, slot);
		ASSERT_TRUE(resolved.has_value());
		EXPECT_EQ("workbench.action.files.openFolder", resolved->commandId);
		if (surface == EWorkbenchCommandSurface::Menu || surface == EWorkbenchCommandSurface::Keybinding) {
			ASSERT_TRUE(resolved->binding.legacyFunctionCode.has_value());
			EXPECT_EQ(30997, *resolved->binding.legacyFunctionCode);
		} else {
			EXPECT_FALSE(resolved->binding.legacyFunctionCode.has_value());
		}
	}
	EXPECT_TRUE(registry.Find("notifications.showList").has_value());
	EXPECT_TRUE(registry.Find("notifications.hideList").has_value());
	EXPECT_TRUE(registry.Find("workbench.action.toggleStatusbarVisibility").has_value());

	const std::array<std::pair<EWorkbenchCommandSurface, std::string_view>, 4> toggleBindings = {
		std::pair{ EWorkbenchCommandSurface::CommandPalette, "workbench.action.toggleSidebarVisibility.palette" },
		std::pair{ EWorkbenchCommandSurface::Menu, "workbench.action.toggleSidebarVisibility.menu" },
		std::pair{ EWorkbenchCommandSurface::ActivityBar, "workbench.action.toggleSidebarVisibility.activity" },
		std::pair{ EWorkbenchCommandSurface::Keybinding, "workbench.action.toggleSidebarVisibility.key" },
	};
	for (const auto& [surface, slot] : toggleBindings) {
		const auto resolved = registry.ResolveSurface(surface, slot);
		ASSERT_TRUE(resolved.has_value());
		EXPECT_EQ("workbench.action.toggleSidebarVisibility", resolved->commandId);
		if (surface == EWorkbenchCommandSurface::Menu || surface == EWorkbenchCommandSurface::Keybinding) {
			ASSERT_TRUE(resolved->binding.legacyFunctionCode.has_value());
			EXPECT_EQ(30991, *resolved->binding.legacyFunctionCode);
		} else {
			EXPECT_FALSE(resolved->binding.legacyFunctionCode.has_value());
		}
	}

	const std::array<std::pair<EWorkbenchCommandSurface, std::string_view>, 2> explorerBindings = {
		std::pair{ EWorkbenchCommandSurface::CommandPalette, "workbench.view.explorer.palette" },
		std::pair{ EWorkbenchCommandSurface::ActivityBar, "workbench.view.explorer.activity" },
	};
	for (const auto& [surface, slot] : explorerBindings) {
		const auto resolved = registry.ResolveSurface(surface, slot);
		ASSERT_TRUE(resolved.has_value());
		EXPECT_EQ("workbench.view.explorer", resolved->commandId);
		EXPECT_FALSE(resolved->binding.legacyFunctionCode.has_value());
	}
	EXPECT_EQ(EWorkbenchCommandRegistrationStatus::Conflict, registry.RegisterBuiltinCommands().status);
}

TEST(WorkbenchCommandRegistry, FileCommandsRegisterStableIdsAliasesSurfacesAndOnlyTheirBoundExecutors)
{
	struct ExpectedCommand final {
		std::string_view id;
		std::int32_t legacyFunctionCode;
	};
	const std::array<ExpectedCommand, 16> expected = {{
		{ "workbench.action.files.newUntitledFile", 30101 },
		{ "workbench.action.newWindow", 30110 },
		{ "workbench.action.files.openFile", 30102 },
		{ "workbench.action.files.openFolder", 30997 },
		{ "workbench.action.openWorkspace", 31002 },
		{ "workbench.action.openRecent", 29007 },
		{ "workbench.action.addRootFolder", 31003 },
		{ "workbench.action.saveWorkspaceAs", 31004 },
		{ "workbench.action.duplicateWorkspaceInNewWindow", 31005 },
		{ "workbench.action.files.save", 30103 },
		{ "workbench.action.files.saveAs", 30104 },
		{ "workbench.action.files.saveAll", 30120 },
		{ "workbench.action.closeActiveEditor", 31007 },
		{ "workbench.action.closeFolder", 31006 },
		{ "workbench.action.closeWindow", 31320 },
		{ "workbench.action.quit", 30195 },
	}};

	std::map<std::string, int, std::less<>> calls;
	const auto executor = [&calls](std::string_view id) {
		return [&calls, id = std::string(id)] {
			++calls[id];
			return workbench::commands::WorkbenchCommandExecutionResult{
				EWorkbenchCommandExecutionStatus::Succeeded, {} };
		};
	};
	workbench::commands::WorkbenchBuiltinCommandExecutors executors;
	executors.newUntitledFile = executor(expected[0].id);
	executors.newWindow = executor(expected[1].id);
	executors.openFile = executor(expected[2].id);
	executors.openFolder = executor(expected[3].id);
	executors.openWorkspace = executor(expected[4].id);
	executors.openRecent = executor(expected[5].id);
	executors.addRootFolder = executor(expected[6].id);
	executors.saveWorkspaceAs = executor(expected[7].id);
	executors.duplicateWorkspaceInNewWindow = executor(expected[8].id);
	executors.save = executor(expected[9].id);
	executors.saveAs = executor(expected[10].id);
	executors.saveAll = executor(expected[11].id);
	executors.closeActiveEditor = executor(expected[12].id);
	executors.closeFolder = executor(expected[13].id);
	executors.closeWindow = executor(expected[14].id);
	executors.quit = executor(expected[15].id);

	WorkbenchCommandRegistry registry;
	ASSERT_EQ(EWorkbenchCommandRegistrationStatus::Succeeded,
		registry.RegisterBuiltinCommands(std::move(executors)).status);
	const auto context = EnabledFileCommandContext();
	std::set<std::int32_t> aliases;
	for (const auto& command : expected) {
		EXPECT_TRUE(aliases.insert(command.legacyFunctionCode).second);
		const auto descriptor = registry.Find(command.id);
		ASSERT_TRUE(descriptor.has_value()) << command.id;
		EXPECT_EQ(EWorkbenchCommandExecutorTarget::Editor, descriptor->executorTarget);

		const auto palette = registry.ResolveSurface(EWorkbenchCommandSurface::CommandPalette,
			std::string(command.id) + ".palette");
		const auto menu = registry.ResolveSurface(EWorkbenchCommandSurface::Menu,
			std::string(command.id) + ".menu");
		const auto key = registry.ResolveSurface(EWorkbenchCommandSurface::Keybinding,
			std::string(command.id) + ".key");
		ASSERT_TRUE(palette.has_value());
		ASSERT_TRUE(menu.has_value());
		ASSERT_TRUE(key.has_value());
		EXPECT_EQ(command.id, palette->commandId);
		EXPECT_EQ(command.id, menu->commandId);
		EXPECT_EQ(command.id, key->commandId);
		EXPECT_FALSE(palette->binding.legacyFunctionCode.has_value());
		ASSERT_TRUE(menu->binding.legacyFunctionCode.has_value());
		ASSERT_TRUE(key->binding.legacyFunctionCode.has_value());
		EXPECT_EQ(command.legacyFunctionCode, *menu->binding.legacyFunctionCode);
		EXPECT_EQ(command.legacyFunctionCode, *key->binding.legacyFunctionCode);
		const auto resolvedAlias = registry.ResolveLegacyFunctionCode(command.legacyFunctionCode);
		ASSERT_TRUE(resolvedAlias.has_value());
		EXPECT_EQ(command.id, *resolvedAlias);
		EXPECT_EQ(EWorkbenchCommandExecutionStatus::Succeeded, registry.Execute(command.id, context).status);
	}
	for (const auto& command : expected) {
		EXPECT_EQ(1, calls[std::string(command.id)]) << command.id;
	}

	const auto revision = registry.Revision();
	EXPECT_EQ(EWorkbenchCommandRegistrationStatus::Conflict, registry.RegisterBuiltinCommands().status);
	EXPECT_EQ(revision, registry.Revision());
}

TEST(WorkbenchCommandRegistry, OpenRecentRemainsEnabledForEmptyHistoryWhileOtherFileCommandsFailClosed)
{
	int recentCalls{};
	int editorCalls{};
	int closeFolderCalls{};
	WorkbenchCommandRegistry registry;
	ASSERT_EQ(EWorkbenchCommandRegistrationStatus::Succeeded, registry.RegisterBuiltinCommands({
		.openRecent = [&recentCalls] {
			++recentCalls;
			return workbench::commands::WorkbenchCommandExecutionResult{
				EWorkbenchCommandExecutionStatus::NotApplicable, "recent history is empty or selection was cancelled" };
		},
		.save = [&editorCalls] {
			++editorCalls;
			return workbench::commands::WorkbenchCommandExecutionResult{ EWorkbenchCommandExecutionStatus::Succeeded, {} };
		},
		.closeFolder = [&closeFolderCalls] {
			++closeFolderCalls;
			return workbench::commands::WorkbenchCommandExecutionResult{ EWorkbenchCommandExecutionStatus::Succeeded, {} };
		},
	}).status);

	auto empty = EnabledContext();
	empty.values.insert_or_assign("workbenchState", std::string("empty"));
	empty.values.insert_or_assign("workspaceFolderCount", std::int64_t{ 0 });
	empty.values.insert_or_assign("editorHasActiveEditor", false);
	EXPECT_EQ(EWorkbenchCommandExecutionStatus::NotApplicable,
		registry.Execute("workbench.action.openRecent", empty).status);
	EXPECT_EQ(EWorkbenchCommandExecutionStatus::Disabled,
		registry.Execute("workbench.action.files.save", empty).status);
	EXPECT_EQ(EWorkbenchCommandExecutionStatus::Disabled,
		registry.Execute("workbench.action.closeFolder", empty).status);
	EXPECT_EQ(1, recentCalls);
	EXPECT_EQ(0, editorCalls);
	EXPECT_EQ(0, closeFolderCalls);

	for (const std::string_view state : { "folder", "workspace" }) {
		auto nonEmpty = EnabledFileCommandContext(std::string(state));
		nonEmpty.values.insert_or_assign("workbench.recentlyOpenedAvailable", false);
		EXPECT_EQ(EWorkbenchCommandExecutionStatus::NotApplicable,
			registry.Execute("workbench.action.openRecent", nonEmpty).status);
		EXPECT_EQ(EWorkbenchCommandExecutionStatus::Succeeded,
			registry.Execute("workbench.action.closeFolder", nonEmpty).status);
	}
	EXPECT_EQ(3, recentCalls);
	EXPECT_EQ(2, closeFolderCalls);
}

TEST(WorkbenchCommandRegistry, FileFunctionSourceAllocationsAndDynamicRangeDoNotCollide)
{
	EXPECT_EQ(31002, static_cast<int>(F_OPEN_WORKSPACE));
	EXPECT_EQ(31003, static_cast<int>(F_ADD_FOLDER_TO_WORKSPACE));
	EXPECT_EQ(31004, static_cast<int>(F_SAVE_WORKSPACE_AS));
	EXPECT_EQ(31005, static_cast<int>(F_DUPLICATE_WORKSPACE));
	EXPECT_EQ(31006, static_cast<int>(F_CLOSE_WORKSPACE));
	EXPECT_EQ(31007, static_cast<int>(F_CLOSE_ACTIVE_EDITOR));
	EXPECT_EQ(29007, static_cast<int>(F_RECENT_WORKSPACE_LIST));
	EXPECT_EQ(13000, static_cast<int>(F_RECENT_WORKSPACE_DYNAMIC_FIRST));

	const std::array<int, 7> fixedCodes = { 29007, 31002, 31003, 31004, 31005, 31006, 31007 };
	const std::set<int> uniqueCodes(fixedCodes.begin(), fixedCodes.end());
	EXPECT_EQ(fixedCodes.size(), uniqueCodes.size());
	for (const int code : fixedCodes) {
		EXPECT_TRUE(code < 13000 || code >= 13064);
	}
}

TEST(WorkbenchContextKeyService, ProjectsWorkspaceAndEditorStateInTheSameImmutableSnapshot)
{
	WorkbenchLayoutStateSnapshot layout;
	config::WorkspaceContextSnapshot workspace;
	workspace.kind = config::EWorkspaceKind::Workspace;
	const auto firstFolder = platform::uri::Uri::FromWindowsPath(L"C:\\workspace-one");
	const auto secondFolder = platform::uri::Uri::FromWindowsPath(L"C:\\workspace-two");
	ASSERT_TRUE(firstFolder.value.has_value());
	ASSERT_TRUE(secondFolder.value.has_value());
	workspace.folders = {
		{ *firstFolder.value, L"" },
		{ *secondFolder.value, L"" },
	};

	WorkbenchContextKeyService service;
	const auto mutation = service.SetCoreProjection(layout, workspace, { true, true });
	const auto snapshot = service.Snapshot();

	ASSERT_EQ(EWorkbenchContextMutationStatus::Succeeded, mutation.status);
	EXPECT_EQ(mutation.revision, snapshot.revision);
	EXPECT_EQ(WorkbenchContextValue(std::string("workspace")), snapshot.values.at("workbenchState"));
	EXPECT_EQ(WorkbenchContextValue(std::int64_t{ 2 }), snapshot.values.at("workspaceFolderCount"));
	EXPECT_EQ(WorkbenchContextValue(true), snapshot.values.at("editorHasActiveEditor"));
	EXPECT_EQ(WorkbenchContextValue(true), snapshot.values.at("editorIsDirty"));
}

TEST(WorkbenchCommandPalette, EnumeratesEveryRegisteredPaletteBindingAndDispatchesStableIds)
{
	WorkbenchCommandRegistry registry;
	ASSERT_EQ(EWorkbenchCommandRegistrationStatus::Succeeded, registry.RegisterBuiltinCommands().status);
	int nativePaletteCalls{};
	ASSERT_EQ(EWorkbenchCommandRegistrationStatus::Succeeded,
		registry.Register(NativePaletteDescriptor(), [&nativePaletteCalls] {
			++nativePaletteCalls;
			return workbench::commands::WorkbenchCommandExecutionResult{
				EWorkbenchCommandExecutionStatus::Succeeded, {} };
		}).status);

	const auto expectedDescriptors = registry.EnumerateSurface(EWorkbenchCommandSurface::CommandPalette);
	ASSERT_GT(expectedDescriptors.size(), 3U);
	const auto allItems = workbench::editor::SearchRegisteredCommandPalette(registry, L"");
	ASSERT_EQ(expectedDescriptors.size(), allItems.size());
	const auto nativeItem = std::find_if(allItems.begin(), allItems.end(),
		[](const workbench::editor::WorkbenchCommandPaletteItem& item) {
			return item.id == "sakura.test.nativePaletteCommand";
		});
	ASSERT_NE(allItems.end(), nativeItem);
	EXPECT_EQ(L"Native Palette Command", nativeItem->label);
	const auto outputItem = std::find_if(allItems.begin(), allItems.end(),
		[](const workbench::editor::WorkbenchCommandPaletteItem& item) {
			return item.id == "workbench.action.output.toggleOutput";
		});
	ASSERT_NE(allItems.end(), outputItem);
	EXPECT_EQ(L"Toggle Output", outputItem->label);

	const auto filtered = workbench::editor::SearchRegisteredCommandPalette(registry, L"native palette");
	ASSERT_EQ(1U, filtered.size());
	EXPECT_EQ("sakura.test.nativePaletteCommand", filtered.front().id);
	const auto idFiltered = workbench::editor::SearchRegisteredCommandPalette(registry, L"toggleoutput");
	ASSERT_EQ(1U, idFiltered.size());
	EXPECT_EQ("workbench.action.output.toggleOutput", idFiltered.front().id);

	workbench::commands::WorkbenchCommandExecutionResult accepted;
	EXPECT_TRUE(workbench::editor::DispatchRegisteredCommandPaletteSelection(registry,
		L"sakura.test.nativePaletteCommand", [&registry, &accepted](std::string_view commandId) {
			accepted = registry.Execute(commandId, EnabledContext());
		}));
	EXPECT_EQ(EWorkbenchCommandExecutionStatus::Succeeded, accepted.status);
	EXPECT_EQ(1, nativePaletteCalls);

	std::string acceptedCommand;
	EXPECT_TRUE(workbench::editor::DispatchRegisteredCommandPaletteSelection(registry,
		L"workbench.action.files.openFolder", [&acceptedCommand](std::string_view commandId) {
			acceptedCommand.assign(commandId);
		}));
	EXPECT_EQ("workbench.action.files.openFolder", acceptedCommand);
	EXPECT_FALSE(workbench::editor::DispatchRegisteredCommandPaletteSelection(registry,
		L"workbench.action.showCommands", [](std::string_view) {}));
	EXPECT_FALSE(workbench::editor::DispatchRegisteredCommandPaletteSelection(registry,
		L"publisher.extension.notRegistered", [](std::string_view) {}));
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
	int openFolderCalls{};
	int showNotificationsCalls{};
	int hideNotificationsCalls{};
	int toggleStatusbarCalls{};
	ASSERT_EQ(EWorkbenchCommandRegistrationStatus::Succeeded, registry.RegisterBuiltinCommands({
		.openFolder = [&openFolderCalls] {
			++openFolderCalls;
			return workbench::commands::WorkbenchCommandExecutionResult{
				EWorkbenchCommandExecutionStatus::Succeeded, {} };
		},
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
		.showNotifications = [&showNotificationsCalls] {
			++showNotificationsCalls;
			return workbench::commands::WorkbenchCommandExecutionResult{
				EWorkbenchCommandExecutionStatus::Succeeded, {} };
		},
		.hideNotifications = [&hideNotificationsCalls] {
			++hideNotificationsCalls;
			return workbench::commands::WorkbenchCommandExecutionResult{
				EWorkbenchCommandExecutionStatus::Succeeded, {} };
		},
		.toggleStatusbarVisibility = [&toggleStatusbarCalls] {
			++toggleStatusbarCalls;
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
	EXPECT_EQ(EWorkbenchCommandExecutionStatus::Succeeded,
		registry.Execute("workbench.action.files.openFolder", context).status);
	EXPECT_EQ(EWorkbenchCommandExecutionStatus::Succeeded,
		registry.Execute("notifications.showList", context).status);
	EXPECT_EQ(EWorkbenchCommandExecutionStatus::Succeeded,
		registry.Execute("notifications.hideList", context).status);
	EXPECT_EQ(EWorkbenchCommandExecutionStatus::Succeeded,
		registry.Execute("workbench.action.toggleStatusbarVisibility", context).status);
	EXPECT_EQ(1, sidebarCalls);
	EXPECT_EQ(1, explorerCalls);
	EXPECT_EQ(1, problemsCalls);
	EXPECT_EQ(1, outputCalls);
	EXPECT_EQ(1, openFolderCalls);
	EXPECT_EQ(1, showNotificationsCalls);
	EXPECT_EQ(1, hideNotificationsCalls);
	EXPECT_EQ(1, toggleStatusbarCalls);
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
	EXPECT_EQ(EWorkbenchCommandExecutionStatus::Unsupported,
		registry.Execute("workbench.action.files.openFolder", EnabledContext()).status);

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

TEST(WorkbenchCommandRegistry, RegistersExactMarkdownCommandIdsWithTheirOwnExecutors)
{
	WorkbenchCommandRegistry registry;
	std::array<int, 10> calls{};
	ASSERT_EQ(EWorkbenchCommandRegistrationStatus::Succeeded, registry.RegisterBuiltinCommands({
		.markdownShowPreview = [&calls] { ++calls[0]; return WorkbenchCommandExecutionResult{ EWorkbenchCommandExecutionStatus::Succeeded, {} }; },
		.markdownShowPreviewToSide = [&calls] { ++calls[1]; return WorkbenchCommandExecutionResult{ EWorkbenchCommandExecutionStatus::Succeeded, {} }; },
		.markdownShowLockedPreviewToSide = [&calls] { ++calls[2]; return WorkbenchCommandExecutionResult{ EWorkbenchCommandExecutionStatus::Succeeded, {} }; },
		.markdownShowSource = [&calls] { ++calls[3]; return WorkbenchCommandExecutionResult{ EWorkbenchCommandExecutionStatus::Succeeded, {} }; },
		.markdownShowPreviewSecuritySelector = [&calls] { ++calls[4]; return WorkbenchCommandExecutionResult{ EWorkbenchCommandExecutionStatus::Succeeded, {} }; },
		.markdownPreviewRefresh = [&calls] { ++calls[5]; return WorkbenchCommandExecutionResult{ EWorkbenchCommandExecutionStatus::Succeeded, {} }; },
		.markdownPreviewToggleLock = [&calls] { ++calls[6]; return WorkbenchCommandExecutionResult{ EWorkbenchCommandExecutionStatus::Succeeded, {} }; },
		.markdownReopenAsPreview = [&calls] { ++calls[7]; return WorkbenchCommandExecutionResult{ EWorkbenchCommandExecutionStatus::Succeeded, {} }; },
		.markdownReopenAsSource = [&calls] { ++calls[8]; return WorkbenchCommandExecutionResult{ EWorkbenchCommandExecutionStatus::Succeeded, {} }; },
		.markdownTogglePreview = [&calls] { ++calls[9]; return WorkbenchCommandExecutionResult{ EWorkbenchCommandExecutionStatus::Succeeded, {} }; },
	}).status);

	constexpr std::array commandIds{
		"markdown.showPreview",
		"markdown.showPreviewToSide",
		"markdown.showLockedPreviewToSide",
		"markdown.showSource",
		"markdown.showPreviewSecuritySelector",
		"markdown.preview.refresh",
		"markdown.preview.toggleLock",
		"markdown.reopenAsPreview",
		"markdown.reopenAsSource",
		"markdown.togglePreview",
	};
	const auto context = EnabledFileCommandContext();
	for (std::size_t index = 0; index < commandIds.size(); ++index) {
		const auto descriptor = registry.Find(commandIds[index]);
		ASSERT_TRUE(descriptor.has_value()) << commandIds[index];
		EXPECT_EQ(EWorkbenchCommandExecutorTarget::Editor, descriptor->executorTarget);
		EXPECT_EQ(EWorkbenchCommandExecutionStatus::Succeeded,
			registry.Execute(commandIds[index], context).status) << commandIds[index];
		EXPECT_EQ(1, calls[index]) << commandIds[index];
	}
}

TEST(WorkbenchCommandRegistry, MarkdownDefaultKeybindingsUseOnlyVsCodeCommandSlots)
{
	WorkbenchCommandRegistry registry;
	ASSERT_TRUE(registry.RegisterBuiltinCommands().Succeeded());
	const auto side = registry.ResolveSurface(EWorkbenchCommandSurface::Keybinding,
		"markdown.showPreviewToSide.key");
	ASSERT_TRUE(side.has_value());
	EXPECT_EQ("markdown.showPreviewToSide", side->commandId);
	const auto toggle = registry.ResolveSurface(EWorkbenchCommandSurface::Keybinding,
		"markdown.togglePreview.key");
	ASSERT_TRUE(toggle.has_value());
	EXPECT_EQ("markdown.togglePreview", toggle->commandId);
	EXPECT_FALSE(registry.ResolveSurface(EWorkbenchCommandSurface::Keybinding,
		"markdown.showPreview.key").has_value());
	EXPECT_EQ(EWorkbenchCommandExecutionStatus::Unsupported,
		registry.Execute("markdown.showPreview", EnabledFileCommandContext()).status);
}

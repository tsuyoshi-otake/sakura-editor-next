/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include "workbench/commands/ApiCommandArguments.h"
#include "workbench/commands/WorkbenchCommandRegistry.h"
#include "workbench/editor/WorkbenchCommandPaletteModel.h"
#include "Funccode_enum.h"

#include "workbench/layout/WorkbenchIds.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <optional>
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

const WorkbenchCommandOwner kContributorGenerationOne{ "sakura.test.contributor", 1 };
const WorkbenchCommandOwner kContributorGenerationTwo{ "sakura.test.contributor", 2 };

WorkbenchCommandDescriptor SampleDescriptor(std::string id = "sakura.test.contributor.run")
{
	return {
		std::move(id),
		"Contributor Run",
		kContributorGenerationOne,
		"workbenchReady",
		"contributor.enabled",
		EWorkbenchCommandExecutorTarget::Editor,
		{ { EWorkbenchCommandSurface::CommandPalette, "sakura.test.contributor.run.palette", std::nullopt } },
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
	context.values.emplace("contributor.enabled", true);
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

TEST(WorkbenchContextKeyService, RecognizesReservedCoreNamespace)
{
	EXPECT_TRUE(WorkbenchContextKeyService::IsReservedCoreKey("workbenchState"));
	EXPECT_TRUE(WorkbenchContextKeyService::IsReservedCoreKey("workspaceFolderCount"));
	EXPECT_TRUE(WorkbenchContextKeyService::IsReservedCoreKey("editorHasActiveEditor"));
	EXPECT_TRUE(WorkbenchContextKeyService::IsReservedCoreKey("editorIsDirty"));
}

TEST(WorkbenchWhenClauseEvaluator, SupportsBoundedBooleanComparisonAndRegexSubsetFailClosed)
{
	const auto context = EnabledContext();
	EXPECT_TRUE(WorkbenchWhenClauseEvaluator::Evaluate(
		"workbenchReady && (contributor.enabled || resource.langId == 'text')", context));
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
	EXPECT_TRUE(WorkbenchWhenClauseEvaluator::Evaluate("contributor.enabled && resource.langId == 'cpp'", context));
}

TEST(WorkbenchCommandRegistry, BuiltinsResolveEverySurfaceToTheSameStableCommandId)
{
	WorkbenchCommandRegistry registry;
	ASSERT_EQ(EWorkbenchCommandRegistrationStatus::Succeeded, registry.RegisterBuiltinCommands().status);
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
	class ExpectedCommand final {
	public:
		constexpr ExpectedCommand(std::string_view id, std::int32_t legacyFunctionCode) noexcept
			: m_id(id), m_legacyFunctionCode(legacyFunctionCode) {}

		[[nodiscard]] constexpr std::string_view Id() const noexcept { return m_id; }
		[[nodiscard]] constexpr std::int32_t LegacyFunctionCode() const noexcept { return m_legacyFunctionCode; }

	private:
		std::string_view m_id;
		std::int32_t m_legacyFunctionCode;
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
	executors.newUntitledFile = executor(expected[0].Id());
	executors.newWindow = executor(expected[1].Id());
	executors.openFile = executor(expected[2].Id());
	executors.openFolder = executor(expected[3].Id());
	executors.openWorkspace = executor(expected[4].Id());
	executors.openRecent = executor(expected[5].Id());
	executors.addRootFolder = executor(expected[6].Id());
	executors.saveWorkspaceAs = executor(expected[7].Id());
	executors.duplicateWorkspaceInNewWindow = executor(expected[8].Id());
	executors.save = executor(expected[9].Id());
	executors.saveAs = executor(expected[10].Id());
	executors.saveAll = executor(expected[11].Id());
	executors.closeActiveEditor = executor(expected[12].Id());
	executors.closeFolder = executor(expected[13].Id());
	executors.closeWindow = executor(expected[14].Id());
	executors.quit = executor(expected[15].Id());

	WorkbenchCommandRegistry registry;
	ASSERT_EQ(EWorkbenchCommandRegistrationStatus::Succeeded,
		registry.RegisterBuiltinCommands(std::move(executors)).status);
	const auto context = EnabledFileCommandContext();
	std::set<std::int32_t> aliases;
	for (const auto& command : expected) {
		EXPECT_TRUE(aliases.insert(command.LegacyFunctionCode()).second);
		const auto descriptor = registry.Find(command.Id());
		ASSERT_TRUE(descriptor.has_value()) << command.Id();
		EXPECT_EQ(EWorkbenchCommandExecutorTarget::Editor, descriptor->executorTarget);

		const auto palette = registry.ResolveSurface(EWorkbenchCommandSurface::CommandPalette,
			std::string(command.Id()) + ".palette");
		const auto menu = registry.ResolveSurface(EWorkbenchCommandSurface::Menu,
			std::string(command.Id()) + ".menu");
		const auto key = registry.ResolveSurface(EWorkbenchCommandSurface::Keybinding,
			std::string(command.Id()) + ".key");
		ASSERT_TRUE(palette.has_value());
		ASSERT_TRUE(menu.has_value());
		ASSERT_TRUE(key.has_value());
		EXPECT_EQ(command.Id(), palette->commandId);
		EXPECT_EQ(command.Id(), menu->commandId);
		EXPECT_EQ(command.Id(), key->commandId);
		EXPECT_FALSE(palette->binding.legacyFunctionCode.has_value());
		ASSERT_TRUE(menu->binding.legacyFunctionCode.has_value());
		ASSERT_TRUE(key->binding.legacyFunctionCode.has_value());
		EXPECT_EQ(command.LegacyFunctionCode(), *menu->binding.legacyFunctionCode);
		EXPECT_EQ(command.LegacyFunctionCode(), *key->binding.legacyFunctionCode);
		const auto resolvedAlias = registry.ResolveLegacyFunctionCode(command.LegacyFunctionCode());
		ASSERT_TRUE(resolvedAlias.has_value());
		EXPECT_EQ(command.Id(), *resolvedAlias);
		EXPECT_EQ(EWorkbenchCommandExecutionStatus::Succeeded, registry.Execute(command.Id(), context).status);
	}
	for (const auto& command : expected) {
		EXPECT_EQ(1, calls[std::string(command.Id())]) << command.Id();
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
	const auto localized = workbench::editor::SearchRegisteredCommandPalette(registry, L"",
		[](const WorkbenchCommandDescriptor& descriptor) {
			if (descriptor.id == "workbench.action.output.toggleOutput") return std::wstring(L"出力の表示を切り替える");
			return std::wstring{};
		});
	const auto localizedOutput = std::find_if(localized.begin(), localized.end(),
		[](const workbench::editor::WorkbenchCommandPaletteItem& item) {
			return item.id == "workbench.action.output.toggleOutput";
		});
	ASSERT_NE(localized.end(), localizedOutput);
	EXPECT_EQ(L"出力の表示を切り替える", localizedOutput->label);

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
		L"sakura.test.contributor.notRegistered", [](std::string_view) {}));
}

TEST(WorkbenchCommandRegistry, ManageMenuSurfacesResolveToTheirCanonicalVsCodeCommands)
{
	WorkbenchCommandRegistry registry;
	ASSERT_EQ(EWorkbenchCommandRegistrationStatus::Succeeded, registry.RegisterBuiltinCommands().status);
	const std::array<std::pair<std::string_view, std::string_view>, 4> bindings = {
		std::pair{ "workbench.manage.commandPalette", "workbench.action.showCommands" },
		std::pair{ "workbench.manage.settings", "workbench.action.openSettings" },
		std::pair{ "workbench.manage.keybindings", "workbench.action.openGlobalKeybindings" },
		std::pair{ "workbench.manage.colorTheme", "workbench.action.selectTheme" },
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
	int keybindingsCalls{};
	int colorThemeCalls{};
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
	}).status);

	const auto context = EnabledContext();
	for (const auto commandId : {
		"workbench.action.showCommands",
		"workbench.action.openSettings",
		"workbench.action.openGlobalKeybindings",
		"workbench.action.selectTheme",
	}) {
		EXPECT_EQ(EWorkbenchCommandExecutionStatus::Succeeded, registry.Execute(commandId, context).status);
	}
	EXPECT_EQ(1, commandPaletteCalls);
	EXPECT_EQ(1, settingsCalls);
	EXPECT_EQ(1, keybindingsCalls);
	EXPECT_EQ(1, colorThemeCalls);
}

TEST(WorkbenchCommandRegistry, DuplicateAndExactOwnerGenerationDisposalAreTerminal)
{
	WorkbenchCommandRegistry registry;
	ASSERT_EQ(EWorkbenchCommandRegistrationStatus::Succeeded, registry.Register(SampleDescriptor()).status);
	EXPECT_EQ(EWorkbenchCommandRegistrationStatus::Conflict, registry.Register(SampleDescriptor()).status);
	EXPECT_EQ(EWorkbenchCommandRegistrationStatus::NotApplicable, registry.DisposeOwner(kContributorGenerationTwo).status);
	ASSERT_TRUE(registry.Find("sakura.test.contributor.run").has_value());
	EXPECT_EQ(EWorkbenchCommandRegistrationStatus::Succeeded, registry.DisposeOwner(kContributorGenerationOne).status);
	EXPECT_FALSE(registry.Find("sakura.test.contributor.run").has_value());
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
	disabled.values.insert_or_assign("contributor.enabled", false);
	EXPECT_EQ(EWorkbenchCommandExecutionStatus::Disabled, registry.Execute("sakura.test.contributor.run", disabled).status);
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

	auto throwing = SampleDescriptor("sakura.test.contributor.throwing");
	throwing.surfaceBindings[0].slotId = "sakura.test.contributor.throwing.palette";
	ASSERT_EQ(EWorkbenchCommandRegistrationStatus::Succeeded, registry.Register(std::move(throwing), []() -> workbench::commands::WorkbenchCommandExecutionResult {
		throw std::runtime_error("test");
	}).status);
	EXPECT_EQ(EWorkbenchCommandExecutionStatus::Failed, registry.Execute("sakura.test.contributor.throwing", EnabledContext()).status);
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

namespace {

WorkbenchContextKeySnapshot GitRepositoryContext()
{
	auto context = EnabledContext();
	// `git.*` is gated on `gitOpenRepositoryCount != 0`, which the core context
	// projection owns because this product's Git provider is native.
	context.values.insert_or_assign("gitOpenRepositoryCount", std::int64_t{ 1 });
	return context;
}

WorkbenchCommandExecutionResult Succeeded()
{
	return { EWorkbenchCommandExecutionStatus::Succeeded, {} };
}

} // namespace

TEST(WorkbenchCommandRegistry, DeliversTheArgumentsPayloadToGitResourceCommands)
{
	WorkbenchCommandRegistry registry;
	std::map<std::string, std::string> received;
	ASSERT_EQ(EWorkbenchCommandRegistrationStatus::Succeeded, registry.RegisterGitCommands({
		.stage = [&received](std::string_view arguments) {
			received.emplace("git.stage", arguments); return Succeeded(); },
		.unstage = [&received](std::string_view arguments) {
			received.emplace("git.unstage", arguments); return Succeeded(); },
		.clean = [&received](std::string_view arguments) {
			received.emplace("git.clean", arguments); return Succeeded(); },
	}).status);

	// A resource-scoped command is meaningless without its rows, so the payload
	// has to reach the executor byte for byte rather than being dropped en route.
	constexpr std::string_view payload = R"([{"group":"workingTree","path":"src/a.cpp","untracked":false,"deleted":false}])";
	const auto context = GitRepositoryContext();
	for (const auto* commandId : { "git.stage", "git.unstage", "git.clean" }) {
		EXPECT_EQ(EWorkbenchCommandExecutionStatus::Succeeded,
			registry.Execute(commandId, context, payload).status) << commandId;
		ASSERT_TRUE(received.contains(commandId)) << commandId;
		EXPECT_EQ(payload, received[commandId]) << commandId;
	}
}

TEST(WorkbenchCommandRegistry, DeliversTheOptionalPostCommitPayloadToGitCommit)
{
	WorkbenchCommandRegistry registry;
	std::optional<std::string> received;
	ASSERT_EQ(EWorkbenchCommandRegistrationStatus::Succeeded, registry.RegisterGitCommands({
		.commit = [&received](std::string_view arguments) {
			received = arguments; return Succeeded(); },
	}).status);

	constexpr std::string_view payload = "[\"git.push\"]";
	EXPECT_EQ(EWorkbenchCommandExecutionStatus::Succeeded,
		registry.Execute("git.commit", GitRepositoryContext(), payload).status);
	ASSERT_TRUE(received.has_value());
	EXPECT_EQ(payload, *received);
}

TEST(WorkbenchCommandRegistry, TheArgumentLessOverloadDeliversAnEmptyPayload)
{
	WorkbenchCommandRegistry registry;
	std::optional<std::string> received;
	ASSERT_EQ(EWorkbenchCommandRegistrationStatus::Succeeded, registry.RegisterGitCommands({
		.stage = [&received](std::string_view arguments) { received = arguments; return Succeeded(); },
	}).status);

	// Every surface but the SCM view invokes without arguments; that is an empty
	// selection the command reports as not applicable, not a malformed payload.
	EXPECT_EQ(EWorkbenchCommandExecutionStatus::Succeeded,
		registry.Execute("git.stage", GitRepositoryContext()).status);
	ASSERT_TRUE(received.has_value());
	EXPECT_TRUE(received->empty());
}

TEST(WorkbenchCommandRegistry, GitGroupCommandsIgnoreAnArgumentsPayload)
{
	WorkbenchCommandRegistry registry;
	int calls = 0;
	ASSERT_EQ(EWorkbenchCommandRegistrationStatus::Succeeded, registry.RegisterGitCommands({
		.stageAll = [&calls] { ++calls; return Succeeded(); },
		.unstageAll = [&calls] { ++calls; return Succeeded(); },
		.cleanAll = [&calls] { ++calls; return Succeeded(); },
	}).status);

	// Upstream's group-scoped handlers take only the repository, so binding them
	// to an argument-less executor is the faithful shape; a payload aimed at one
	// of them is simply not part of its contract.
	const auto context = GitRepositoryContext();
	for (const auto* commandId : { "git.stageAll", "git.unstageAll", "git.cleanAll" }) {
		EXPECT_EQ(EWorkbenchCommandExecutionStatus::Succeeded,
			registry.Execute(commandId, context, R"([{"group":"index","path":"a.txt"}])").status) << commandId;
	}
	EXPECT_EQ(3, calls);
}

TEST(WorkbenchCommandRegistry, AnUnboundGitCommandIsUnsupportedRatherThanASilentNoOp)
{
	WorkbenchCommandRegistry registry;
	ASSERT_EQ(EWorkbenchCommandRegistrationStatus::Succeeded, registry.RegisterGitCommands({}).status);

	const auto context = GitRepositoryContext();
	for (const auto* commandId : { "git.stage", "git.unstage", "git.clean",
			"git.stageAll", "git.unstageAll", "git.cleanAll" }) {
		const auto descriptor = registry.Find(commandId);
		ASSERT_TRUE(descriptor.has_value()) << commandId;
		EXPECT_EQ("gitOpenRepositoryCount != 0", descriptor->whenClause) << commandId;
		EXPECT_EQ(EWorkbenchCommandExecutionStatus::Unsupported,
			registry.Execute(commandId, context).status) << commandId;
	}
	// With the count published as zero the whole batch is out of scope, not
	// merely disabled.
	auto noRepository = EnabledContext();
	noRepository.values.insert_or_assign("gitOpenRepositoryCount", std::int64_t{ 0 });
	EXPECT_EQ(EWorkbenchCommandExecutionStatus::NotApplicable,
		registry.Execute("git.stage", noRepository).status);

	// An *absent* key is a different fact and must not be mistaken for zero:
	// upstream evaluates `!=` loosely, so `undefined != 0` is true and the clause
	// matches. The core projection always publishes the key, so production never
	// reaches this state; the assertion pins the semantics rather than blessing it.
	EXPECT_EQ(EWorkbenchCommandExecutionStatus::Unsupported,
		registry.Execute("git.stage", EnabledContext()).status);
}

TEST(WorkbenchCommandRegistry, GitStagingCommandsCarryUpstreamTitlesAndPaletteSlots)
{
	WorkbenchCommandRegistry registry;
	ASSERT_EQ(EWorkbenchCommandRegistrationStatus::Succeeded, registry.RegisterGitCommands({}).status);

	// `extensions/git/package.nls.json` titles under the `Git` category every one
	// of these commands declares in `package.json`.
	const std::map<std::string, std::string> titles{
		{ "git.stage", "Git: Stage Changes" },
		{ "git.stageAll", "Git: Stage All Changes" },
		{ "git.unstage", "Git: Unstage Changes" },
		{ "git.unstageAll", "Git: Unstage All Changes" },
		{ "git.clean", "Git: Discard Changes" },
		{ "git.cleanAll", "Git: Discard All Changes" },
	};
	for (const auto& [commandId, title] : titles) {
		const auto descriptor = registry.Find(commandId);
		ASSERT_TRUE(descriptor.has_value()) << commandId;
		EXPECT_EQ(title, descriptor->title) << commandId;
		const auto slot = registry.ResolveSurface(EWorkbenchCommandSurface::CommandPalette, commandId + ".palette");
		ASSERT_TRUE(slot.has_value()) << commandId;
		EXPECT_EQ(commandId, slot->commandId) << commandId;
	}
}

TEST(WorkbenchCommandRegistry, GitCloneRecursiveIsAnAlwaysAvailableRepositoryCreationCommand)
{
	WorkbenchCommandRegistry registry;
	int calls{};
	ASSERT_EQ(EWorkbenchCommandRegistrationStatus::Succeeded, registry.RegisterGitCommands({
		.cloneRecursive = [&calls] { ++calls; return Succeeded(); },
	}).status);

	const auto descriptor = registry.Find("git.cloneRecursive");
	ASSERT_TRUE(descriptor.has_value());
	// `command.cloneRecursive` in the Git extension is `Clone (Recursive)`;
	// it is the distinct command linked by the SCM empty-workbench welcome view.
	EXPECT_EQ("Git: Clone (Recursive)", descriptor->title);
	EXPECT_EQ("workbenchReady", descriptor->whenClause);
	const auto slot = registry.ResolveSurface(
		EWorkbenchCommandSurface::CommandPalette, "git.cloneRecursive.palette");
	ASSERT_TRUE(slot.has_value());
	EXPECT_EQ("git.cloneRecursive", slot->commandId);

	// Its job is to create a repository, so it must not inherit the regular
	// Git command's `gitOpenRepositoryCount != 0` gate.
	EXPECT_EQ(EWorkbenchCommandExecutionStatus::Succeeded,
		registry.Execute("git.cloneRecursive", EnabledContext()).status);
	EXPECT_EQ(1, calls);
}

TEST(WorkbenchCommandRegistry, ApiCommandsAreRegisteredByTheWorkbenchWithNoSurfaceOfTheirOwn)
{
	WorkbenchCommandRegistry registry;
	std::map<std::string, std::string> received;
	ASSERT_EQ(EWorkbenchCommandRegistrationStatus::Succeeded, registry.RegisterBuiltinCommands({
		.vscodeDiff = [&received](std::string_view arguments) {
			received.emplace("vscode.diff", arguments); return Succeeded(); },
		.vscodeOpen = [&received](std::string_view arguments) {
			received.emplace("vscode.open", arguments); return Succeeded(); },
		.vscodeOpenFolder = [&received](std::string_view arguments) {
			received.emplace("vscode.openFolder", arguments); return Succeeded(); },
	}).status);

	// Upstream registers these in `workbench/api/common/apiCommands.ts` through
	// `CommandsRegistry` alone: no `MenuRegistry` contribution, no category, and
	// no keybinding. A palette slot here would put a raw URI-taking command in
	// front of a user who has no way to supply one.
	const std::map<std::string, std::string> titles{
		{ "vscode.diff", "Opens the provided resources in the diff editor to compare their contents." },
		{ "vscode.open", "Opens the provided resource in the editor." },
		{ "vscode.openFolder", "Opens a folder as a workspace." },
	};
	const std::map<std::string, std::string_view> payloads{
		{ "vscode.diff", R"(["git:/C:/repo/a.cpp?%7B%22ref%22%3A%22HEAD%22%7D","file:///C:/repo/a.cpp"])" },
		{ "vscode.open", R"(["git:/C:/repo/a.cpp?%7B%22ref%22%3A%22HEAD%22%7D","file:///C:/repo/a.cpp"])" },
		// The SCM ViewWelcome invokes `vscode.openFolder` without an operand, so
		// the native handler opens its folder picker rather than inventing a URI.
		{ "vscode.openFolder", "" },
	};
	const auto context = EnabledContext();
	for (const auto& [commandId, title] : titles) {
		const auto descriptor = registry.Find(commandId);
		ASSERT_TRUE(descriptor.has_value()) << commandId;
		EXPECT_EQ(title, descriptor->title) << commandId;
		EXPECT_TRUE(descriptor->surfaceBindings.empty()) << commandId;
		// They are the Git provider's route to a comparison, but they are not the
		// Git provider's commands: `workbenchReady` is the whole condition, so an
		// extension can issue one in a window with no repository open.
		EXPECT_EQ("workbenchReady", descriptor->whenClause) << commandId;

		const std::string_view payload = payloads.at(commandId);
		EXPECT_EQ(EWorkbenchCommandExecutionStatus::Succeeded,
			registry.Execute(commandId, context, payload).status) << commandId;
		ASSERT_TRUE(received.contains(commandId)) << commandId;
		EXPECT_EQ(payload, received[commandId]) << commandId;
	}
}

TEST(WorkbenchCommandRegistry, GitOpenChangeIsAResourceScopedGitCommandLikeStage)
{
	WorkbenchCommandRegistry registry;
	std::optional<std::string> received;
	ASSERT_EQ(EWorkbenchCommandRegistrationStatus::Succeeded, registry.RegisterGitCommands({
		.openChange = [&received](std::string_view arguments) { received = arguments; return Succeeded(); },
	}).status);

	const auto descriptor = registry.Find("git.openChange");
	ASSERT_TRUE(descriptor.has_value());
	// `command.openChange` in `extensions/git/package.nls.json`, under the `Git`
	// category `package.json` declares for it.
	EXPECT_EQ("Git: Open Changes", descriptor->title);
	EXPECT_EQ("gitOpenRepositoryCount != 0", descriptor->whenClause);
	const auto slot = registry.ResolveSurface(
		EWorkbenchCommandSurface::CommandPalette, "git.openChange.palette");
	ASSERT_TRUE(slot.has_value());
	EXPECT_EQ("git.openChange", slot->commandId);

	// It names rows exactly as the staging commands do, because upstream's
	// handler takes the same `SourceControlResourceState[]`.
	constexpr std::string_view payload = R"([{"group":"index","path":"src/a.cpp","untracked":false,"deleted":false}])";
	EXPECT_EQ(EWorkbenchCommandExecutionStatus::Succeeded,
		registry.Execute("git.openChange", GitRepositoryContext(), payload).status);
	ASSERT_TRUE(received.has_value());
	EXPECT_EQ(payload, *received);

	// Unbound it is the same typed `Unsupported` every other Git command is, not
	// a silent no-op that would look like a diff the user simply cannot see.
	WorkbenchCommandRegistry unbound;
	ASSERT_EQ(EWorkbenchCommandRegistrationStatus::Succeeded, unbound.RegisterGitCommands({}).status);
	EXPECT_EQ(EWorkbenchCommandExecutionStatus::Unsupported,
		unbound.Execute("git.openChange", GitRepositoryContext(), payload).status);
}

namespace {

using workbench::commands::ApiDiffArguments;
using workbench::commands::ApiOpenArguments;
using workbench::commands::BuildApiDiffArguments;
using workbench::commands::BuildApiOpenArguments;
using workbench::commands::ParseApiDiffArguments;
using workbench::commands::ParseApiOpenArguments;

} // namespace

TEST(ApiCommandArguments, DiffCarriesBothSidesAndUpstreamsTitle)
{
	const ApiDiffArguments arguments{ L"git:///C:/repo/a.cpp?x", L"file:///C:/repo/a.cpp", L"Open" };
	const auto encoded = BuildApiDiffArguments(arguments);
	EXPECT_EQ(R"(["git:///C:/repo/a.cpp?x","file:///C:/repo/a.cpp","Open"])", encoded);

	const auto parsed = ParseApiDiffArguments(encoded);
	ASSERT_TRUE(parsed.has_value());
	EXPECT_EQ(arguments, *parsed);
}

TEST(ApiCommandArguments, DiffAcceptsAnAbsentTitleButNeverOneSide)
{
	const auto twoElements = ParseApiDiffArguments(R"(["left","right"])");
	ASSERT_TRUE(twoElements.has_value());
	EXPECT_TRUE(twoElements->title.empty());

	// A comparison needs both sides; one URI is not a shorter `vscode.diff`.
	EXPECT_FALSE(ParseApiDiffArguments(R"(["left"])").has_value());
	EXPECT_FALSE(ParseApiDiffArguments("[]").has_value());
	EXPECT_FALSE(ParseApiDiffArguments(R"(["a","b","c","d"])").has_value());
	EXPECT_FALSE(ParseApiDiffArguments(R"(["a","b"] x)").has_value());
	EXPECT_FALSE(ParseApiDiffArguments(R"(["a",5])").has_value());
	EXPECT_FALSE(ParseApiDiffArguments("").has_value());
}

TEST(ApiCommandArguments, OpenKeepsAnAbsentOverrideApartFromAFalseOne)
{
	// `JSON.stringify({ override: undefined })` is `{}`, so absent travels as an
	// empty object rather than as a member whose value is null.
	const ApiOpenArguments absent{ L"file:///C:/repo/a.cpp", std::nullopt, L"Open" };
	EXPECT_EQ(R"(["file:///C:/repo/a.cpp",{},"Open"])", BuildApiOpenArguments(absent));

	// The built-in Git provider sets `false` for a both-modified merge resource,
	// which forces the default text editor. That is a different request.
	const ApiOpenArguments forced{ L"file:///C:/repo/a.cpp", false, L"Open" };
	const auto encoded = BuildApiOpenArguments(forced);
	EXPECT_EQ(R"(["file:///C:/repo/a.cpp",{"override":false},"Open"])", encoded);

	const auto parsedAbsent = ParseApiOpenArguments(BuildApiOpenArguments(absent));
	ASSERT_TRUE(parsedAbsent.has_value());
	EXPECT_EQ(absent, *parsedAbsent);

	const auto parsedForced = ParseApiOpenArguments(encoded);
	ASSERT_TRUE(parsedForced.has_value());
	EXPECT_EQ(forced, *parsedForced);
	EXPECT_NE(*parsedAbsent, *parsedForced);
}

TEST(ApiCommandArguments, OpenNeedsOnlyItsResourceAndRefusesAnythingElse)
{
	const auto bare = ParseApiOpenArguments(R"(["file:///C:/a.cpp"])");
	ASSERT_TRUE(bare.has_value());
	EXPECT_FALSE(bare->overrideEditor.has_value());
	EXPECT_TRUE(bare->label.empty());

	EXPECT_FALSE(ParseApiOpenArguments("[]").has_value());
	// A show option this command does not define is a request it cannot honour.
	EXPECT_FALSE(ParseApiOpenArguments(R"(["a",{"preview":true}])").has_value());
	EXPECT_FALSE(ParseApiOpenArguments(R"(["a",{"override":1}])").has_value());
	EXPECT_FALSE(ParseApiOpenArguments(R"(["a","b"])").has_value());
	EXPECT_FALSE(ParseApiOpenArguments(R"(["a",{},"b","c"])").has_value());
}

TEST(ApiCommandArguments, AnOverLongStringIsRefusedRatherThanTruncated)
{
	const std::string overLong(workbench::commands::kMaximumApiCommandStringLength + 1, 'a');
	EXPECT_FALSE(ParseApiDiffArguments(R"([")" + overLong + R"(","b"])").has_value());
	EXPECT_TRUE(ParseApiDiffArguments(R"([")" + overLong.substr(1) + R"(","b"])").has_value());
}

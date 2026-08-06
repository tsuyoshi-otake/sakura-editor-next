/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/

#include "pch.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include "workbench/commands/WorkbenchCommandRegistry.h"
#include "workbench/commands/WorkbenchContextKeyService.h"
#include "workbench/layout/WorkbenchLayoutStateTypes.h"

namespace {

using workbench::commands::EWorkbenchCommandExecutionStatus;
using workbench::commands::EWorkbenchCommandRegistrationStatus;
using workbench::commands::EWorkbenchCommandSurface;
using workbench::commands::ResolveUpdateIndicatorCommand;
using workbench::commands::WorkbenchCommandExecutionResult;
using workbench::commands::WorkbenchCommandExecutor;
using workbench::commands::WorkbenchCommandRegistry;
using workbench::commands::WorkbenchContextKeyService;
using workbench::commands::WorkbenchContextKeySnapshot;
using workbench::commands::WorkbenchUpdateCommandContext;
using workbench::commands::WorkbenchUpdateCommandExecutors;
using workbench::commands::kUpdateIndicatorCommandId;
using workbench::layout::WorkbenchLayoutStateSnapshot;

//! Every one of upstream's twelve `StateType` values, spelled the way the
//! `updateState` context key carries them. The `when` clauses compare against
//! these exact strings, spaces included.
constexpr std::array<std::string_view, 12> kAllStates{
	"uninitialized",
	"disabled",
	"idle",
	"checking for updates",
	"available for download",
	"downloading",
	"downloaded",
	"updating",
	"ready",
	"overwriting",
	"cancelling",
	"restarting",
};

/*!
	Builds the context through the production projection rather than by hand.
	A hand-assembled map could open a `when` clause the composition root would
	never actually publish, which would prove nothing about the real surfaces.
*/
WorkbenchContextKeySnapshot UpdateContext(std::string state)
{
	const WorkbenchLayoutStateSnapshot layout;
	WorkbenchContextKeyService service;
	const auto result = service.SetCoreProjection(
		layout, {}, {}, false, {}, WorkbenchUpdateCommandContext{ std::move(state) });
	EXPECT_TRUE(result.Succeeded());
	return service.Snapshot();
}

//! Records which executor ran, so a command that dispatches to the wrong one is
//! a failure rather than an indistinguishable success.
class ExecutorLog final {
public:
	[[nodiscard]] WorkbenchCommandExecutor Bind(std::string name)
	{
		return [this, name = std::move(name)]() -> WorkbenchCommandExecutionResult {
			m_ran.push_back(name);
			return { EWorkbenchCommandExecutionStatus::Succeeded, {} };
		};
	}

	[[nodiscard]] const std::vector<std::string>& Ran() const noexcept { return m_ran; }
	void Clear() noexcept { m_ran.clear(); }

private:
	std::vector<std::string> m_ran;
};

WorkbenchUpdateCommandExecutors BoundExecutors(ExecutorLog& log)
{
	WorkbenchUpdateCommandExecutors executors;
	executors.checkForUpdates = log.Bind("checkForUpdates");
	executors.downloadUpdate = log.Bind("downloadUpdate");
	executors.applyUpdate = log.Bind("applyUpdate");
	executors.quitAndInstall = log.Bind("quitAndInstall");
	executors.showUpdateInfo = log.Bind("showUpdateInfo");
	return executors;
}

EWorkbenchCommandExecutionStatus StatusOf(
	const WorkbenchCommandRegistry& registry, std::string_view commandId, std::string_view state)
{
	return registry.Execute(commandId, UpdateContext(std::string(state))).status;
}

} // namespace

TEST(WorkbenchUpdateCommands, RegistersUpstreamsPaletteActionsGearGroupAndTitleBarEntryAsOneBatch)
{
	ExecutorLog log;
	WorkbenchCommandRegistry registry;
	const auto registration = registry.RegisterUpdateCommands(BoundExecutors(log));
	ASSERT_EQ(EWorkbenchCommandRegistrationStatus::Succeeded, registration.status);

	// The five Command Palette actions from upstream's `update.contribution.ts`.
	const std::array<std::pair<std::string_view, std::string_view>, 5> palette{ {
		{ "update.checkForUpdate", "Check for Updates..." },
		{ "update.downloadUpdate", "Download Update" },
		{ "update.installUpdate", "Install Update" },
		{ "update.restartToUpdate", "Restart to Update" },
		{ "update.showUpdateInfo", "Show Update Info" },
	} };
	for (const auto& [id, title] : palette) {
		const auto descriptor = registry.Find(id);
		ASSERT_TRUE(descriptor.has_value()) << id;
		EXPECT_EQ(title, descriptor->title) << id;
		const auto slot = std::string(id) + ".palette";
		const auto resolved = registry.ResolveSurface(EWorkbenchCommandSurface::CommandPalette, slot);
		ASSERT_TRUE(resolved.has_value()) << slot;
		EXPECT_EQ(id, resolved->commandId);
	}

	// The eight `7_update` gear entries from upstream's `update.ts`, with its own
	// literal titles - the trailing `(1)` included.
	const std::array<std::pair<std::string_view, std::string_view>, 8> gear{ {
		{ "update.check", "Check for Updates..." },
		{ "update.checking", "Checking for Updates..." },
		{ "update.downloadNow", "Download Update (1)" },
		{ "update.downloading", "Downloading Update..." },
		{ "update.install", "Install Update... (1)" },
		{ "update.updating", "Installing Update..." },
		{ "update.cancelling", "Cancelling Update..." },
		{ "update.restart", "Restart to Update (1)" },
	} };
	for (const auto& [id, title] : gear) {
		const auto descriptor = registry.Find(id);
		ASSERT_TRUE(descriptor.has_value()) << id;
		EXPECT_EQ(title, descriptor->title) << id;
		const auto slot = "workbench.manage.7_update." + std::string(id);
		const auto resolved = registry.ResolveSurface(EWorkbenchCommandSurface::Menu, slot);
		ASSERT_TRUE(resolved.has_value()) << slot;
		EXPECT_EQ(id, resolved->commandId);
	}

	const auto indicator = registry.Find(kUpdateIndicatorCommandId);
	ASSERT_TRUE(indicator.has_value());
	EXPECT_EQ("Update", indicator->title);
	const auto indicatorSlot = registry.ResolveSurface(EWorkbenchCommandSurface::Menu, "workbench.titleBar.update");
	ASSERT_TRUE(indicatorSlot.has_value());
	EXPECT_EQ(kUpdateIndicatorCommandId, indicatorSlot->commandId);
}

TEST(WorkbenchUpdateCommands, RegistersTheWholeUpdateBatchOrNoneOfIt)
{
	ExecutorLog log;
	WorkbenchCommandRegistry registry;
	const auto first = registry.RegisterUpdateCommands(BoundExecutors(log));
	ASSERT_EQ(EWorkbenchCommandRegistrationStatus::Succeeded, first.status);

	// A second registration collides on every ID and slot. It must leave the
	// first batch intact rather than half-replacing it.
	const auto second = registry.RegisterUpdateCommands(BoundExecutors(log));
	EXPECT_EQ(EWorkbenchCommandRegistrationStatus::Conflict, second.status);
	EXPECT_EQ(first.revision, registry.Revision());
	EXPECT_EQ(EWorkbenchCommandExecutionStatus::Succeeded, StatusOf(registry, "update.check", "idle"));
}

TEST(WorkbenchUpdateCommands, ListsExactlyOnePaletteActionPerUpdateState)
{
	ExecutorLog log;
	WorkbenchCommandRegistry registry;
	ASSERT_TRUE(registry.RegisterUpdateCommands(BoundExecutors(log)).Succeeded());

	// Upstream gives each palette action a precondition on one `updateState`, so
	// in any other state it is not listed at all - `NotApplicable`, not disabled.
	const std::array<std::pair<std::string_view, std::string_view>, 4> actions{ {
		{ "update.checkForUpdate", "idle" },
		{ "update.downloadUpdate", "available for download" },
		{ "update.installUpdate", "downloaded" },
		{ "update.restartToUpdate", "ready" },
	} };
	for (const auto& [id, openState] : actions) {
		for (const auto state : kAllStates) {
			const auto expected = state == openState
				? EWorkbenchCommandExecutionStatus::Succeeded
				: EWorkbenchCommandExecutionStatus::NotApplicable;
			EXPECT_EQ(expected, StatusOf(registry, id, state)) << id << " in " << state;
		}
	}

	const std::array<std::string_view, 4> expectedRuns{
		"checkForUpdates", "downloadUpdate", "applyUpdate", "quitAndInstall"
	};
	ASSERT_EQ(expectedRuns.size(), log.Ran().size());
	for (std::size_t index = 0; index < expectedRuns.size(); ++index) {
		EXPECT_EQ(expectedRuns[index], log.Ran()[index]);
	}
}

TEST(WorkbenchUpdateCommands, OpensExactlyOneGearEntryPerUpdateState)
{
	ExecutorLog log;
	WorkbenchCommandRegistry registry;
	ASSERT_TRUE(registry.RegisterUpdateCommands(BoundExecutors(log)).Succeeded());

	// Four entries act; four are upstream's `precondition: false` progress labels.
	// A progress entry is listed and greyed out, so its own state must terminate
	// as `Disabled` - being unavailable right now is not the same fact as being
	// unimplemented, which is what `Unsupported` would claim.
	const std::array<std::tuple<std::string_view, std::string_view, EWorkbenchCommandExecutionStatus>, 8> entries{ {
		{ "update.check", "idle", EWorkbenchCommandExecutionStatus::Succeeded },
		{ "update.checking", "checking for updates", EWorkbenchCommandExecutionStatus::Disabled },
		{ "update.downloadNow", "available for download", EWorkbenchCommandExecutionStatus::Succeeded },
		{ "update.downloading", "downloading", EWorkbenchCommandExecutionStatus::Disabled },
		{ "update.install", "downloaded", EWorkbenchCommandExecutionStatus::Succeeded },
		{ "update.updating", "updating", EWorkbenchCommandExecutionStatus::Disabled },
		{ "update.cancelling", "cancelling", EWorkbenchCommandExecutionStatus::Disabled },
		{ "update.restart", "ready", EWorkbenchCommandExecutionStatus::Succeeded },
	} };
	for (const auto& [id, openState, openStatus] : entries) {
		for (const auto state : kAllStates) {
			const auto expected = state == openState ? openStatus : EWorkbenchCommandExecutionStatus::NotApplicable;
			EXPECT_EQ(expected, StatusOf(registry, id, state)) << id << " in " << state;
		}
	}

	const std::array<std::string_view, 4> expectedRuns{
		"checkForUpdates", "downloadUpdate", "applyUpdate", "quitAndInstall"
	};
	ASSERT_EQ(expectedRuns.size(), log.Ran().size());
	for (std::size_t index = 0; index < expectedRuns.size(); ++index) {
		EXPECT_EQ(expectedRuns[index], log.Ran()[index]);
	}
}

TEST(WorkbenchUpdateCommands, KeepsShowUpdateInfoAvailableInEveryStateIncludingDisabled)
{
	ExecutorLog log;
	WorkbenchCommandRegistry registry;
	ASSERT_TRUE(registry.RegisterUpdateCommands(BoundExecutors(log)).Succeeded());

	// What the editor knows about updating is a meaningful answer in every state,
	// and in `disabled` or after a failure it is the only way to read the
	// diagnostic. So this one is gated on `workbenchReady`, not on a state.
	for (const auto state : kAllStates) {
		EXPECT_EQ(EWorkbenchCommandExecutionStatus::Succeeded,
			StatusOf(registry, "update.showUpdateInfo", state)) << state;
	}
	EXPECT_EQ(kAllStates.size(), log.Ran().size());
}

TEST(WorkbenchUpdateCommands, ReportsAnUnboundUpdateStackAsUnsupportedRatherThanUnknown)
{
	WorkbenchCommandRegistry registry;
	// An installation with no writable staging root composes no update stack, so
	// the commands register with no executors. Answering `UnknownCommand` would
	// say the surface does not exist, which is a different and wrong fact.
	ASSERT_TRUE(registry.RegisterUpdateCommands().Succeeded());

	EXPECT_EQ(EWorkbenchCommandExecutionStatus::Unsupported, StatusOf(registry, "update.check", "idle"));
	EXPECT_EQ(EWorkbenchCommandExecutionStatus::Unsupported, StatusOf(registry, "update.checkForUpdate", "idle"));
	EXPECT_EQ(EWorkbenchCommandExecutionStatus::Unsupported, StatusOf(registry, "update.restart", "ready"));
	EXPECT_EQ(EWorkbenchCommandExecutionStatus::Unsupported,
		StatusOf(registry, "update.showUpdateInfo", "uninitialized"));
	// The indicator still delegates, and the delegate is what is unsupported.
	EXPECT_EQ(EWorkbenchCommandExecutionStatus::Unsupported,
		StatusOf(registry, kUpdateIndicatorCommandId, "ready"));
	// A command that was never registered is still the other answer entirely.
	EXPECT_EQ(EWorkbenchCommandExecutionStatus::UnknownCommand,
		StatusOf(registry, "update.checkForUpdates", "idle"));
}

TEST(WorkbenchUpdateCommands, ShowsTheTitleBarIndicatorOnlyInTheThreeActionableStates)
{
	ExecutorLog log;
	WorkbenchCommandRegistry registry;
	ASSERT_TRUE(registry.RegisterUpdateCommands(BoundExecutors(log)).Succeeded());

	const std::array<std::string_view, 3> actionable{ "available for download", "downloaded", "ready" };
	for (const auto state : kAllStates) {
		const bool visible = std::find(actionable.begin(), actionable.end(), state) != actionable.end();
		const auto expected = visible
			? EWorkbenchCommandExecutionStatus::Succeeded
			: EWorkbenchCommandExecutionStatus::NotApplicable;
		EXPECT_EQ(expected, StatusOf(registry, kUpdateIndicatorCommandId, state)) << state;
	}

	// One button, three different meanings, chosen from the same snapshot its own
	// visibility clause was evaluated against.
	const std::array<std::string_view, 3> expectedRuns{ "downloadUpdate", "applyUpdate", "quitAndInstall" };
	ASSERT_EQ(expectedRuns.size(), log.Ran().size());
	for (std::size_t index = 0; index < expectedRuns.size(); ++index) {
		EXPECT_EQ(expectedRuns[index], log.Ran()[index]);
	}
}

TEST(WorkbenchUpdateCommands, ResolvesTheIndicatorToUpstreamsThreeCommandsAndToNothingElse)
{
	const std::array<std::pair<std::string_view, std::string_view>, 3> actionable{ {
		{ "available for download", "update.downloadNow" },
		{ "downloaded", "update.install" },
		{ "ready", "update.restart" },
	} };
	for (const auto& [state, expected] : actionable) {
		const auto resolved = ResolveUpdateIndicatorCommand(UpdateContext(std::string(state)));
		ASSERT_TRUE(resolved.has_value()) << state;
		EXPECT_EQ(expected, *resolved) << state;
	}

	for (const auto state : kAllStates) {
		if (state == "available for download" || state == "downloaded" || state == "ready") continue;
		EXPECT_FALSE(ResolveUpdateIndicatorCommand(UpdateContext(std::string(state))).has_value()) << state;
	}

	// A missing key is the pre-projection state and a non-string value could only
	// come from a corrupted overlay. Neither may resolve to a nearest-looking
	// action, because there is no update to act on in either case.
	WorkbenchContextKeySnapshot empty;
	EXPECT_FALSE(ResolveUpdateIndicatorCommand(empty).has_value());

	WorkbenchContextKeySnapshot wrongType;
	wrongType.values.emplace("updateState", true);
	EXPECT_FALSE(ResolveUpdateIndicatorCommand(wrongType).has_value());
}

TEST(WorkbenchUpdateCommands, LetsAStaleIndicatorPressDoNothingRatherThanGuessAnUpdateAction)
{
	ExecutorLog log;
	WorkbenchCommandRegistry registry;
	ASSERT_TRUE(registry.RegisterUpdateCommands(BoundExecutors(log)).Succeeded());

	// `ExecuteUpdateIndicator` is also reachable directly from the title bar's
	// press callback, which cannot re-check the `when` clause first. A state that
	// moved on between paint and click must produce no action at all.
	for (const auto state : { "uninitialized", "disabled", "idle", "checking for updates",
			 "downloading", "updating", "overwriting", "cancelling", "restarting" }) {
		const auto result = registry.ExecuteUpdateIndicator(UpdateContext(state));
		EXPECT_EQ(EWorkbenchCommandExecutionStatus::NotApplicable, result.status) << state;
		EXPECT_EQ("update state is not actionable", result.detail) << state;
	}
	EXPECT_TRUE(log.Ran().empty());

	EXPECT_TRUE(registry.ExecuteUpdateIndicator(UpdateContext("ready")).Succeeded());
	ASSERT_EQ(1u, log.Ran().size());
	EXPECT_EQ("quitAndInstall", log.Ran().front());
}

TEST(WorkbenchUpdateCommands, PublishesUninitializedRatherThanAnEmptyUpdateState)
{
	// Upstream's `RawContextKey<string>('updateState', StateType.Uninitialized)`
	// has a default. Publishing `""` instead would quietly close every `update.*`
	// surface at once, which is a different fact from "nothing has checked yet".
	const auto defaulted = UpdateContext("");
	const auto found = defaulted.values.find("updateState");
	ASSERT_NE(defaulted.values.end(), found);
	ASSERT_TRUE(std::holds_alternative<std::string>(found->second));
	EXPECT_EQ("uninitialized", std::get<std::string>(found->second));

	const WorkbenchLayoutStateSnapshot layout;
	WorkbenchContextKeyService service;
	ASSERT_TRUE(service.SetCoreProjection(layout).Succeeded());
	const auto snapshot = service.Snapshot();
	const auto minimal = snapshot.values.find("updateState");
	ASSERT_NE(snapshot.values.end(), minimal);
	ASSERT_TRUE(std::holds_alternative<std::string>(minimal->second));
	EXPECT_EQ("uninitialized", std::get<std::string>(minimal->second));
}

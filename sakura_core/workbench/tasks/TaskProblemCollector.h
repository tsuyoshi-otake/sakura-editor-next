/*! @file
 * @brief Tasks -> Problems producer: captures drained task output, runs the
 * problem matcher engine, and reconciles `MarkerService` per run.
 */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#pragma once

#include "workbench/output/IOutputService.h"
#include "workbench/problems/MarkerService.h"
#include "workbench/tasks/TaskExecutionService.h"

#include <sakura/uri/UriIdentity.h>

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace workbench::tasks {

//! The core Tasks output channel identity, registered with the same `tasks`
//! id as upstream's `task.contribution.ts` (`log: false`). Task-system
//! messages only: configuration errors, unsupported matcher names, and run
//! diagnostics. Process output belongs to the terminal, never this channel.
inline constexpr char kTasksOutputChannelId[] = "tasks";
inline constexpr char kTasksOutputChannelLabel[] = "Tasks";
inline constexpr char kTasksOutputOwnerId[] = "core.tasks";
inline constexpr std::uint64_t kTasksOutputOwnerGeneration = 1;

//! Creates the `tasks` channel idempotently under the core owner generation.
[[nodiscard]] output::OutputOperationResult EnsureTasksOutputChannel(output::IOutputService& service);

//! Appends one task-system message line to the `tasks` channel, creating it on
//! first use. A `text` without a trailing LF gets one appended, matching how
//! the channel renders entries. Returns the append result; channel creation
//! failure surfaces as the append's terminal result.
[[nodiscard]] output::OutputOperationResult AppendTasksOutputLine(
	output::IOutputService& service, std::string_view text);

//! Per-owner-id run book. `MarkerService` fences every owner id against a
//! monotonically increasing generation, so handing each run a fresh generation
//! is what makes a rerun replace the previous run's markers, drop resources
//! the new run no longer reports, and turn a stale run's late mutation into a
//! typed `StaleGeneration` no-op. The delivered-resource ledger is the local
//! copy of upstream `problemCollectors.ts`'s delivered set: it is what lets a
//! run that produced no replacements still emit the explicit per-resource
//! removes that carry the new generation. Shared ownership lets a retained
//! terminal run's observer outlive the composition member safely.
class CTaskMarkerGenerationBook final {
public:
	//! First call for an owner id returns 1; later calls increment. Saturates
	//! at `std::numeric_limits<std::uint64_t>::max()` instead of wrapping.
	[[nodiscard]] std::uint64_t Next(std::string_view ownerId);
	//! Resources the owner's last committed run delivered, in stored order.
	[[nodiscard]] std::vector<platform::uri::Uri> Resources(std::string_view ownerId) const;
	//! Replaces the owner's delivered set with the run's resources. Only the
	//! latest minted generation may commit: a delayed older run's commit is a
	//! no-op so its stale ledger can never shadow the newer run's record.
	void Commit(std::string_view ownerId, std::uint64_t generation,
		std::vector<platform::uri::Uri> resources);

private:
	struct OwnerBook final {
		std::uint64_t generation = 0;
		std::vector<platform::uri::Uri> resources;
	};

	mutable std::mutex m_mutex;
	std::map<std::string, OwnerBook, std::less<>> m_owners;
};

//! Resolves the workspace root a `Relative` matcher file location resolves
//! against: `<folder>/.vscode/tasks.json` -> `<folder>`. Tasks contributed by
//! a `.code-workspace` artifact have no per-task folder and return `nullopt`;
//! a `Relative` matcher then fails typed `MissingWorkspaceRoot` at apply time.
[[nodiscard]] std::optional<platform::uri::Uri> DeriveTaskWorkspaceRootUri(
	const TaskConfigurationDefinition& definition);

/*!
	@brief Composition seam for `TaskExecutionService` run observation.

	The returned factory creates a `ITaskRunObserver` only for definitions that
	carry resolvable `problemMatchers`; other runs stay unobserved at zero
	overhead. The observer buffers drained output, and on the run's single
	terminal transition runs every matcher through `ProblemMatcherEngine` and
	applies each accepted `ReplaceMarkersRequest` to `markers` under a fresh
	owner generation, mirroring upstream `problemCollectors.ts`
	`changeOne`/`remove` semantics through generation adoption. Engine and
	marker failures are reported to the `tasks` channel, never guessed away.
*/
[[nodiscard]] TaskRunObserverFactory MakeTaskRunObserverFactory(
	problems::MarkerService& markers,
	output::IOutputService& output,
	std::shared_ptr<CTaskMarkerGenerationBook> generations);

} // namespace workbench::tasks

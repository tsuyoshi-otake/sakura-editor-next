/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "workbench/layout/WorkbenchLayoutStateTypes.h"
#include "config/WorkspaceContextTypes.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace workbench::commands {

inline constexpr std::size_t kMaxWorkbenchContextKeyLength = 160;
inline constexpr std::size_t kMaxWorkbenchContextValueLength = 1'024;

using WorkbenchContextValue = std::variant<bool, std::int64_t, std::string>;
using WorkbenchContextKeyMap = std::map<std::string, WorkbenchContextValue, std::less<>>;

//! Editor state supplied by the native composition root with the layout and
//! workspace snapshots. This pure value type avoids an editor implementation
//! dependency in the command/context layer.
struct WorkbenchEditorCommandContext {
	bool hasActiveEditor = false;
	bool activeEditorDirty = false;
	//! Upstream's `EditorContextKeys.inDiffEditor` (`isInDiffEditor`). It is a core
	//! editor key, not a Git one, even though the Git extension is what gates its
	//! selected-range commands on it: the question it answers is what kind of
	//! editor is active, which only the editor layer can know.
	bool inDiffEditor = false;
};

//! Source-control state the command surfaces gate on. Upstream's Git extension
//! publishes `gitOpenRepositoryCount` from the extension host; our Git provider
//! is native, so the core projection owns the key instead. Same key, same
//! meaning, different owner - see `workbench/scm/CLAUDE.md`.
struct WorkbenchScmCommandContext {
	std::int64_t gitOpenRepositoryCount = 0;
};

//! An owner/generation pair deliberately independent of the extension transport.
struct WorkbenchCommandOwner {
	std::string ownerId;
	std::uint64_t generation{};

	[[nodiscard]] bool IsValid() const noexcept;
	[[nodiscard]] bool operator==(const WorkbenchCommandOwner&) const noexcept = default;
	[[nodiscard]] bool operator<(const WorkbenchCommandOwner& other) const noexcept
	{
		return ownerId < other.ownerId || (ownerId == other.ownerId && generation < other.generation);
	}
};

enum class EWorkbenchContextMutationStatus : std::uint8_t {
	Succeeded,
	NotApplicable,
	Invalid,
	Conflict,
};

struct WorkbenchContextMutationResult {
	EWorkbenchContextMutationStatus status = EWorkbenchContextMutationStatus::Invalid;
	std::uint64_t revision{};

	[[nodiscard]] bool Succeeded() const noexcept { return status == EWorkbenchContextMutationStatus::Succeeded; }
};

//! An immutable read used by command evaluation. It cannot expose mutable service state.
struct WorkbenchContextKeySnapshot {
	std::uint64_t revision{};
	WorkbenchContextKeyMap values;
};

/*! 
	@brief Window-local context keys with a core-owned `workbench.*` namespace.

	Only SetCoreProjection can publish keys in the core namespace. Extension overlays
	are owner-generation scoped and may only publish non-reserved keys. Every
	mutation replaces one complete immutable map under a lock, so observers never
	see a partly refreshed layout projection.
*/
class WorkbenchContextKeyService final {
public:
	WorkbenchContextKeyService() = default;
	WorkbenchContextKeyService(const WorkbenchContextKeyService&) = delete;
	WorkbenchContextKeyService& operator=(const WorkbenchContextKeyService&) = delete;

	[[nodiscard]] WorkbenchContextMutationResult SetCoreProjection(
		const layout::WorkbenchLayoutStateSnapshot& snapshot);
	[[nodiscard]] WorkbenchContextMutationResult SetCoreProjection(
		const layout::WorkbenchLayoutStateSnapshot& snapshot,
		const config::WorkspaceContextSnapshot& workspace);
	[[nodiscard]] WorkbenchContextMutationResult SetCoreProjection(
		const layout::WorkbenchLayoutStateSnapshot& snapshot,
		const config::WorkspaceContextSnapshot& workspace,
		WorkbenchEditorCommandContext editor,
		bool recentlyOpenedAvailable = false,
		WorkbenchScmCommandContext scm = {});
	[[nodiscard]] WorkbenchContextMutationResult SetExtensionOverlay(
		const WorkbenchCommandOwner& owner, WorkbenchContextKeyMap values);
	[[nodiscard]] WorkbenchContextMutationResult DisposeExtensionOverlay(
		const WorkbenchCommandOwner& owner);
	[[nodiscard]] WorkbenchContextKeySnapshot Snapshot() const;
	[[nodiscard]] static bool IsValidKey(std::string_view key) noexcept;
	[[nodiscard]] static bool IsReservedCoreKey(std::string_view key) noexcept;

private:
	struct Overlay {
		WorkbenchContextKeyMap values;
	};

	mutable std::mutex m_mutex;
	std::uint64_t m_revision{};
	WorkbenchContextKeyMap m_coreValues;
	std::map<WorkbenchCommandOwner, Overlay> m_overlays;
};

//! Fail-closed subset of VS Code `when` expression semantics for the native command boundary.
class WorkbenchWhenClauseEvaluator final {
public:
	[[nodiscard]] static bool Evaluate(std::string_view expression,
		const WorkbenchContextKeySnapshot& context) noexcept;
};

} // namespace workbench::commands

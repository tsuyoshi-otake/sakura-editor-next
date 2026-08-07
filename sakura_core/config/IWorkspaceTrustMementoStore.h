/*! @file
 * @brief Persistence contract for the per-workspace Workspace Trust memento.
 */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace config {

/*!
	@brief The durable per-workspace record the trust prompts consult.

	Upstream keeps these as two separate workspace-storage mementos --
	@c workspace.trust.startupPrompt.shown and @c acceptsOutOfWorkspaceFiles --
	both at @c StorageScope.WORKSPACE with @c StorageTarget.MACHINE. They are one
	document here because they share a scope, a target, an owner, and a lifetime:
	both answer "what has this workspace already been asked?", and splitting them
	across two keys would mean two independent compare-and-swap transactions that
	could disagree about which workspace revision they belong to.

	This is deliberately not the Trusted Folders list. That list records what the
	user *granted* and is profile-scoped, because a granted folder is trusted in
	every window. This record only remembers what the user was *asked*, and an
	answer to one workspace says nothing about another.

	Both fields default to false, which is the direction that asks again. A record
	that was never written must never read as "already handled".
 */
struct WorkspaceTrustMemento final {
	//! The startup trust prompt has been shown for this workspace at least once.
	bool startupPromptShown = false;
	//! The user accepted opening files from outside this trusted workspace's roots.
	bool untrustedFilesAccepted = false;

	[[nodiscard]] bool operator==(const WorkspaceTrustMemento&) const noexcept = default;
};

//! A load never exposes a partially decoded record.
enum class EWorkspaceTrustMementoLoadStatus : std::uint8_t {
	Loaded,
	//! This workspace has never been asked anything. Normal, and not a failure.
	NotFound,
	//! The stored bytes are preserved for diagnosis and never replaced with defaults.
	InvalidStoredMemento,
	Unavailable,
	//! The window has no workspace identity to key a record by -- an empty window.
	NoWorkspaceScope,
	Failed,
};

struct WorkspaceTrustMementoLoadResult final {
	EWorkspaceTrustMementoLoadStatus status = EWorkspaceTrustMementoLoadStatus::Failed;
	std::optional<WorkspaceTrustMemento> memento;
	//! Diagnostics deliberately exclude profile paths, storage addresses, and workspace identity.
	std::wstring diagnostic;

	/*!
		@brief Whether the caller may act on a record at all.

		@c NotFound counts: an absent record is a coherent answer, and its value is
		the default. A caller that treated only @c Loaded as readable would prompt
		forever under @c once, because the first launch always finds nothing.
	 */
	[[nodiscard]] bool Readable() const noexcept
	{
		return (status == EWorkspaceTrustMementoLoadStatus::Loaded && memento.has_value())
			|| status == EWorkspaceTrustMementoLoadStatus::NotFound;
	}

	//! The record's value, or the ask-again default when nothing readable exists.
	[[nodiscard]] WorkspaceTrustMemento ValueOrDefault() const noexcept
	{
		return memento.value_or(WorkspaceTrustMemento{});
	}
};

//! A save has one terminal result; conflicts are not retried with a new memento.
enum class EWorkspaceTrustMementoSaveStatus : std::uint8_t {
	Persisted,
	NotDirty,
	Conflict,
	RetryExhausted,
	Unavailable,
	NoWorkspaceScope,
	Stopped,
	Failed,
};

struct WorkspaceTrustMementoSaveResult final {
	EWorkspaceTrustMementoSaveStatus status = EWorkspaceTrustMementoSaveStatus::Failed;
	std::wstring diagnostic;
};

/*!
	@brief Persistence boundary owned by the workbench composition edge.

	A caller loads once before saving, so the load can capture the storage revision
	the next compare-and-swap save writes against.

	Failing to save is not a reason to pretend the prompt did not happen, and it is
	not a reason to claim it did. The caller shows the prompt it already decided to
	show and reports that the record could not be kept; the next launch asks again.
	That is the same fail-closed direction @c GrantWorkspaceTrust takes when it
	refuses a grant it cannot persist -- except inverted, because here the
	conservative answer is to ask more often rather than less.
 */
class IWorkspaceTrustMementoStore {
public:
	virtual ~IWorkspaceTrustMementoStore() = default;
	[[nodiscard]] virtual WorkspaceTrustMementoLoadResult Load() = 0;
	[[nodiscard]] virtual WorkspaceTrustMementoSaveResult Save(const WorkspaceTrustMemento& memento) = 0;
};

} // namespace config

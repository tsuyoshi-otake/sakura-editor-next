/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#pragma once

#include <sakura/uri/UriIdentity.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace config {

//! The semantic shape of the editor window's current workspace context.
enum class EWorkspaceKind : std::uint8_t {
	Empty,
	Folder,
	Workspace,
};

//! Every context operation reaches one of these terminal outcomes.
enum class EWorkspaceContextOutcome : std::uint8_t {
	Succeeded,
	Failed,
	Conflict,
	NotApplicable,
};

//! A display identifier is caller-provided metadata; URI identity is derived
//! only from uri through UriIdentityService.
struct WorkspaceFolderDescriptor final {
	platform::uri::Uri uri;
	std::wstring displayName;
};

//! Immutable value returned by the service. workspaceIdentityKey is a bounded,
//! canonical structured key and is never a display path or a filesystem lookup.
struct WorkspaceContextSnapshot final {
	std::uint64_t generation = 0;
	std::uint64_t revision = 0;
	EWorkspaceKind kind = EWorkspaceKind::Empty;
	std::optional<platform::uri::Uri> workspaceConfigUri;
	std::vector<WorkspaceFolderDescriptor> folders;
	std::wstring workspaceIdentityKey;
};

//! Common idempotency/CAS fields for a mutation request.
struct WorkspaceContextOperation final {
	std::string operationId;
	std::optional<std::uint64_t> expectedRevision;
};

struct SetFolderRequest final {
	WorkspaceContextOperation operation;
	platform::uri::Uri folderUri;
	std::wstring displayName;
};

struct SetWorkspaceRequest final {
	WorkspaceContextOperation operation;
	std::optional<platform::uri::Uri> workspaceConfigUri;
	std::vector<WorkspaceFolderDescriptor> folders;
};

//! One successful state transition, delivered after the service lock is released.
struct WorkspaceContextChange final {
	std::uint64_t revision = 0;
	WorkspaceContextSnapshot previous;
	WorkspaceContextSnapshot current;
};

struct WorkspaceContextResult final {
	EWorkspaceContextOutcome outcome = EWorkspaceContextOutcome::Failed;
	std::uint64_t revision = 0;
	std::string reason;
	bool replayed = false;
	WorkspaceContextSnapshot snapshot;
	std::optional<WorkspaceContextChange> change;
};

using WorkspaceContextListener = std::function<void(const WorkspaceContextChange&)>;

//! Move-only RAII subscription. Listener faults during destruction are contained.
class WorkspaceContextSubscription final {
public:
	WorkspaceContextSubscription() = default;
	explicit WorkspaceContextSubscription(std::function<void()> unsubscribe) : m_unsubscribe(std::move(unsubscribe)) {}
	~WorkspaceContextSubscription() { Reset(); }
	WorkspaceContextSubscription(const WorkspaceContextSubscription&) = delete;
	WorkspaceContextSubscription& operator=(const WorkspaceContextSubscription&) = delete;
	WorkspaceContextSubscription(WorkspaceContextSubscription&& other) noexcept;
	WorkspaceContextSubscription& operator=(WorkspaceContextSubscription&& other) noexcept;

	void Reset() noexcept;
	bool IsActive() const noexcept { return static_cast<bool>(m_unsubscribe); }

private:
	std::function<void()> m_unsubscribe;
};

} // namespace config

/*! @file
 * @brief Persistence contract for the Trusted Folders and Workspaces list.
 */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "config/WorkspaceTrustPolicy.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace config {

/*!
	@brief The durable form of VS Code's Trusted Folders and Workspaces list.

	This is deliberately a struct around one vector rather than the bare vector.
	The list is the only thing that persists today, but the durable record is a
	document with a schema, and a later field (an ordering, a last-used stamp)
	must be addable without changing the store port or every caller's type.

	It carries no window, session, or process identity. Trust is a property of a
	resource, and a durable record keyed by anything else would make the same
	folder trusted in one window and not another.
 */
struct TrustedFoldersSnapshot final {
	std::vector<WorkspaceTrustEntry> entries;
};

//! A load never exposes a partially decoded or incoherent list.
enum class ETrustedFoldersLoadStatus : std::uint8_t {
	Loaded,
	NotFound,
	InvalidStoredList,
	Unavailable,
	Failed,
};

struct TrustedFoldersLoadResult final {
	ETrustedFoldersLoadStatus status = ETrustedFoldersLoadStatus::Failed;
	std::optional<TrustedFoldersSnapshot> snapshot;
	//! Diagnostics deliberately exclude profile paths, storage addresses, and the trusted URIs themselves.
	std::wstring diagnostic;

	[[nodiscard]] bool Loaded() const noexcept
	{
		return status == ETrustedFoldersLoadStatus::Loaded && snapshot.has_value();
	}
};

//! A save has one terminal result; conflicts are not retried with a new snapshot.
enum class ETrustedFoldersSaveStatus : std::uint8_t {
	Persisted,
	NotDirty,
	Conflict,
	RetryExhausted,
	Unavailable,
	Stopped,
	Failed,
};

struct TrustedFoldersSaveResult final {
	ETrustedFoldersSaveStatus status = ETrustedFoldersSaveStatus::Failed;
	std::wstring diagnostic;
};

/*!
	@brief Persistence boundary owned by the workbench composition edge.

	A caller loads once before saving. A successful @c NotFound or @c Loaded result
	captures the storage revision used for the next compare-and-swap save.

	@c InvalidStoredList must never become a replacement write. A trust list that
	failed to decode is the one case where overwriting is most tempting and most
	wrong: the durable bytes are the user's record of which folders they chose to
	trust, and silently replacing them with an empty list would erase that record
	while presenting a clean UI. The correct behavior is to resolve every folder as
	untrusted for this session and leave the payload for diagnosis and recovery.

	@c NotFound is a different and entirely normal state: no folder has ever been
	trusted in this profile. It is not a failure and must not be diagnosed as one.
 */
class ITrustedFoldersStore {
public:
	virtual ~ITrustedFoldersStore() = default;
	[[nodiscard]] virtual TrustedFoldersLoadResult Load() = 0;
	[[nodiscard]] virtual TrustedFoldersSaveResult Save(const TrustedFoldersSnapshot& snapshot) = 0;
};

} // namespace config

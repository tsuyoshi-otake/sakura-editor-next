/*! @file
 * @brief Typed, UI-independent VS Code recently opened Folder/Workspace state.
 */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 * SPDX-License-Identifier: Zlib
 */
#pragma once

#include <sakura/uri/UriIdentity.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace workbench::recent {

inline constexpr std::size_t kMaximumRecentlyOpenedWorkspaces = 64;
inline constexpr char kRecentlyOpenedWorkspaceStorageKey[] = "workbench.recentlyOpened";
inline constexpr std::uint32_t kRecentlyOpenedWorkspaceSchemaVersion = 1;

enum class ERecentlyOpenedWorkspaceKind : std::uint8_t {
	Folder,
	Workspace,
};

struct RecentlyOpenedWorkspaceEntry final {
	ERecentlyOpenedWorkspaceKind kind = ERecentlyOpenedWorkspaceKind::Folder;
	platform::uri::Uri uri;
	std::optional<std::wstring> label;
};

enum class ERecentlyOpenedWorkspaceOutcome : std::uint8_t {
	Succeeded,
	Cancelled,
	Failed,
};

struct RecentlyOpenedWorkspaceResult final {
	ERecentlyOpenedWorkspaceOutcome outcome = ERecentlyOpenedWorkspaceOutcome::Failed;
	std::string diagnostic;
};

enum class ERecentlyOpenedWorkspaceStoreLoadStatus : std::uint8_t {
	Succeeded,
	NotFound,
	Unavailable,
	Failed,
};

struct RecentlyOpenedWorkspaceStoreLoadResult final {
	ERecentlyOpenedWorkspaceStoreLoadStatus status = ERecentlyOpenedWorkspaceStoreLoadStatus::Failed;
	std::optional<std::string> payload;
	std::string diagnostic;
};

enum class ERecentlyOpenedWorkspaceStoreSaveStatus : std::uint8_t {
	Succeeded,
	Unavailable,
	Conflict,
	Failed,
};

struct RecentlyOpenedWorkspaceStoreSaveResult final {
	ERecentlyOpenedWorkspaceStoreSaveStatus status = ERecentlyOpenedWorkspaceStoreSaveStatus::Failed;
	std::string diagnostic;
};

//! Durable I/O is composed outside the workbench.  Implementers are control-process
//! clients; this UI-independent service never sees a path, HWND, or storage backend.
class IRecentlyOpenedWorkspaceStore {
public:
	virtual ~IRecentlyOpenedWorkspaceStore() = default;
	virtual RecentlyOpenedWorkspaceStoreLoadResult Load() = 0;
	virtual RecentlyOpenedWorkspaceStoreSaveResult Save(std::string payload) = 0;
};

class IRecentlyOpenedWorkspaceService {
public:
	virtual ~IRecentlyOpenedWorkspaceService() = default;
	virtual RecentlyOpenedWorkspaceResult Load() = 0;
	[[nodiscard]] virtual std::vector<RecentlyOpenedWorkspaceEntry> Snapshot() const = 0;
	//! Call only after the target editor has crossed its ready boundary.
	virtual RecentlyOpenedWorkspaceResult RecordSuccessfulOpen(RecentlyOpenedWorkspaceEntry entry) = 0;
	//! Call only after the opener has established NotFound.  Access-denied, timeout,
	//! cancellation, and all other failures deliberately retain the entry.
	virtual RecentlyOpenedWorkspaceResult RemoveConfirmedNotFound(const platform::uri::Uri& uri) = 0;
	//! `workbench.action.clearRecentFiles`.  The user has already confirmed; an
	//! already-empty history is a success that performs no durable write.
	virtual RecentlyOpenedWorkspaceResult Clear() = 0;
};

/*! Bounded, success-only MRU service.  Mutations are transactional: a failed
 * control-store write leaves the previously accepted snapshot untouched. */
class CRecentlyOpenedWorkspaceService final : public IRecentlyOpenedWorkspaceService {
public:
	explicit CRecentlyOpenedWorkspaceService(std::unique_ptr<IRecentlyOpenedWorkspaceStore> store) noexcept;

	RecentlyOpenedWorkspaceResult Load() override;
	[[nodiscard]] std::vector<RecentlyOpenedWorkspaceEntry> Snapshot() const override;
	RecentlyOpenedWorkspaceResult RecordSuccessfulOpen(RecentlyOpenedWorkspaceEntry entry) override;
	RecentlyOpenedWorkspaceResult RemoveConfirmedNotFound(const platform::uri::Uri& uri) override;
	RecentlyOpenedWorkspaceResult Clear() override;

	[[nodiscard]] static std::optional<RecentlyOpenedWorkspaceEntry> Normalize(RecentlyOpenedWorkspaceEntry entry);

private:
	[[nodiscard]] static std::optional<std::vector<RecentlyOpenedWorkspaceEntry>> Decode(
		std::string_view payload, std::string& diagnostic);
	[[nodiscard]] static std::optional<std::string> Encode(
		const std::vector<RecentlyOpenedWorkspaceEntry>& entries);
	[[nodiscard]] static bool IsValidLabel(const std::optional<std::wstring>& label) noexcept;
	[[nodiscard]] static bool SameIdentity(const platform::uri::Uri& left, const platform::uri::Uri& right) noexcept;
	[[nodiscard]] RecentlyOpenedWorkspaceStoreSaveResult SaveEntries(
		const std::vector<RecentlyOpenedWorkspaceEntry>& entries);

	std::unique_ptr<IRecentlyOpenedWorkspaceStore> m_store;
	std::vector<RecentlyOpenedWorkspaceEntry> m_entries;
};

} // namespace workbench::recent

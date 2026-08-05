/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace platform::storage {

//! Transport-safe limits shared by in-process and control-process storage adapters.
inline constexpr std::size_t kMaximumStorageItems = 1024;
inline constexpr std::size_t kMaximumStorageOperationIdBytes = 256;
inline constexpr std::size_t kMaximumStorageAddressPartBytes = 4 * 1024;
inline constexpr std::size_t kMaximumStorageStringBytes = 64 * 1024;
//! Leaves room in the 1 MiB IPC frame for the outer TLV envelope and response metadata.
inline constexpr std::size_t kMaximumStorageSnapshotPayloadBytes = 768 * 1024;
//! A change response repeats address metadata, so requests use a smaller conservative budget.
inline constexpr std::size_t kMaximumStorageMutationPayloadBytes = 384 * 1024;

[[nodiscard]] bool IsValidStorageUtf8(std::string_view value, bool allowEmpty = true) noexcept;

enum class EStorageScope : std::uint8_t {
	ApplicationShared,
	Application,
	Profile,
	Workspace,
};

enum class EStorageTarget : std::uint8_t {
	User,
	Machine,
};

[[nodiscard]] constexpr bool IsValidStorageScope(EStorageScope scope) noexcept
{
	return scope == EStorageScope::ApplicationShared || scope == EStorageScope::Application
		|| scope == EStorageScope::Profile || scope == EStorageScope::Workspace;
}

[[nodiscard]] constexpr bool IsValidStorageTarget(EStorageTarget target) noexcept
{
	return target == EStorageTarget::User || target == EStorageTarget::Machine;
}

struct StorageAddress {
	EStorageScope scope = EStorageScope::Application;
	std::string scopeId;
	std::string owner;
	std::string key;

	[[nodiscard]] bool IsValid() const noexcept;
	[[nodiscard]] bool operator==(const StorageAddress&) const noexcept = default;
	[[nodiscard]] bool operator<(const StorageAddress& other) const noexcept;
};

struct StorageEntry {
	StorageAddress address;
	EStorageTarget target = EStorageTarget::Machine;
	std::string value;
	std::uint64_t revision = 0;

	[[nodiscard]] bool operator==(const StorageEntry&) const noexcept = default;
};

struct StorageMutation {
	StorageAddress address;
	EStorageTarget target = EStorageTarget::Machine;
	//! A missing value removes the entry.
	std::optional<std::string> value;

	[[nodiscard]] bool operator==(const StorageMutation&) const noexcept = default;
};

struct StorageMutationRequest {
	std::string operationId;
	std::optional<std::uint64_t> expectedRevision;
	std::vector<StorageMutation> mutations;

	[[nodiscard]] bool operator==(const StorageMutationRequest&) const noexcept = default;
};

enum class EStorageMutationStatus : std::uint8_t {
	Succeeded,
	Conflict,
	Failed,
	NotApplicable,
};

struct StorageChange {
	StorageAddress address;
	//! The target that held (or now holds) this value; retained for deletions.
	EStorageTarget target = EStorageTarget::Machine;
	std::optional<StorageEntry> entry;

	[[nodiscard]] bool operator==(const StorageChange&) const noexcept = default;
};

struct StorageChangeBatch {
	std::uint64_t generation = 0;
	std::uint64_t baseRevision = 0;
	std::uint64_t revision = 0;
	std::vector<StorageChange> changes;

	[[nodiscard]] bool operator==(const StorageChangeBatch&) const noexcept = default;
};

//! Invoked after an authoritative mutation commits. The batch is immutable.
using StorageChangeCallback = std::function<void(const StorageChangeBatch&)>;

struct StorageMutationResult {
	EStorageMutationStatus status = EStorageMutationStatus::Failed;
	std::uint64_t revision = 0;
	bool replayed = false;
	std::string diagnostic;
	std::optional<StorageChangeBatch> changeBatch;
};

struct StorageSnapshot {
	std::uint64_t generation = 0;
	std::uint64_t revision = 0;
	std::vector<StorageEntry> entries;
};

enum class EStorageChangeApplyStatus : std::uint8_t {
	Applied,
	IgnoredStale,
	ResyncRequired,
};

} // namespace platform::storage

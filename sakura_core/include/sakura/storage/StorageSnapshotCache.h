/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <sakura/storage/StorageTypes.h>

#include <cstdint>
#include <map>
#include <optional>
#include <shared_mutex>

namespace platform::storage {

/*!
	@brief Thread-safe read cache for an authoritative storage snapshot.

	This cache owns only editor/extension-side copied state. It never writes the
	authority, persists data, or owns a Control IPC endpoint. Replace() is the
	fail-closed resynchronization boundary; Apply() accepts exactly the next
	contiguous committed change batch for the current generation.

	All public operations are bounded by the storage limits in StorageTypes.h.
	Readers receive value copies, and Replace()/Apply() publish a complete state
	at one operation boundary. The cache has no worker thread or callback
	ownership and may be destroyed independently of the authority service.
*/
class CStorageSnapshotCache final {
public:
	void Replace(StorageSnapshot snapshot);
	[[nodiscard]] EStorageChangeApplyStatus Apply(const StorageChangeBatch& batch);
	[[nodiscard]] std::optional<StorageEntry> Find(const StorageAddress& address) const;
	[[nodiscard]] std::uint64_t GetGeneration() const;
	[[nodiscard]] std::uint64_t GetRevision() const;
	//! Tests both snapshot coordinates while holding one cache-state lock.
	[[nodiscard]] bool Matches(std::uint64_t generation, std::uint64_t revision) const;

private:
	// All cache state is observed and replaced atomically at an operation
	// boundary. Entries leave this class only as value copies.
	mutable std::shared_mutex m_mutex;
	std::uint64_t m_generation = 0;
	std::uint64_t m_revision = 0;
	std::map<StorageAddress, StorageEntry> m_entries;
};

} // namespace platform::storage

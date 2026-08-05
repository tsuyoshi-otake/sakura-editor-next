/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include <sakura/storage/StorageSnapshotCache.h>

#include <cstddef>
#include <limits>
#include <mutex>
#include <set>

namespace platform::storage {
namespace {

constexpr std::size_t kEncodedSnapshotHeaderBytes = sizeof(std::uint64_t) * 2 + sizeof(std::uint32_t);
constexpr std::size_t kEncodedAddressFixedBytes = sizeof(std::uint8_t) + sizeof(std::uint32_t) * 3;
constexpr std::size_t kEncodedEntryFixedBytes = kEncodedAddressFixedBytes + sizeof(std::uint8_t)
	+ sizeof(std::uint32_t) + sizeof(std::uint64_t);

[[nodiscard]] bool CheckedAdd(std::size_t& value, std::size_t addend, std::size_t maximum) noexcept
{
	if (value > maximum || addend > maximum - value) {
		return false;
	}
	value += addend;
	return true;
}

[[nodiscard]] std::size_t EncodedEntryBytes(const StorageEntry& entry) noexcept
{
	return kEncodedEntryFixedBytes + entry.address.scopeId.size() + entry.address.owner.size()
		+ entry.address.key.size() + entry.value.size();
}

[[nodiscard]] bool IsValidEntry(const StorageEntry& entry, std::uint64_t snapshotRevision) noexcept
{
	return entry.address.IsValid() && IsValidStorageTarget(entry.target)
		&& entry.value.size() <= kMaximumStorageStringBytes && IsValidStorageUtf8(entry.value)
		&& entry.revision != 0 && entry.revision <= snapshotRevision;
}

} // namespace

void CStorageSnapshotCache::Replace(StorageSnapshot snapshot)
{
	std::map<StorageAddress, StorageEntry> entries;
	std::size_t encodedBytes = kEncodedSnapshotHeaderBytes;
	bool valid = snapshot.generation != 0 && snapshot.entries.size() <= kMaximumStorageItems;
	for (auto& entry : snapshot.entries) {
		if (!valid || !IsValidEntry(entry, snapshot.revision)
			|| !CheckedAdd(encodedBytes, EncodedEntryBytes(entry), kMaximumStorageSnapshotPayloadBytes)
			|| !entries.emplace(entry.address, std::move(entry)).second) {
			valid = false;
			break;
		}
	}
	std::unique_lock lock(m_mutex);
	if (!valid) {
		m_entries.clear();
		m_generation = 0;
		m_revision = 0;
		return;
	}
	m_entries = std::move(entries);
	m_generation = snapshot.generation;
	m_revision = snapshot.revision;
}

EStorageChangeApplyStatus CStorageSnapshotCache::Apply(const StorageChangeBatch& batch)
{
	std::unique_lock lock(m_mutex);
	if (m_generation == 0 || batch.generation == 0 || batch.generation > m_generation) {
		return EStorageChangeApplyStatus::ResyncRequired;
	}
	if (batch.generation < m_generation || batch.revision <= m_revision) {
		return EStorageChangeApplyStatus::IgnoredStale;
	}
	if (batch.baseRevision != m_revision || batch.baseRevision == std::numeric_limits<std::uint64_t>::max()
		|| batch.revision != batch.baseRevision + 1 || batch.changes.empty()
		|| batch.changes.size() > kMaximumStorageItems) {
		return EStorageChangeApplyStatus::ResyncRequired;
	}

	std::set<StorageAddress> addresses;
	std::size_t projectedEntryCount = m_entries.size();
	std::size_t projectedPayloadBytes = kEncodedSnapshotHeaderBytes;
	for (const auto& [address, entry] : m_entries) {
		(void)address;
		if (!CheckedAdd(projectedPayloadBytes, EncodedEntryBytes(entry), kMaximumStorageSnapshotPayloadBytes)) {
			return EStorageChangeApplyStatus::ResyncRequired;
		}
	}
	for (const auto& change : batch.changes) {
		if (!change.address.IsValid() || !IsValidStorageTarget(change.target)
			|| !addresses.emplace(change.address).second) {
			return EStorageChangeApplyStatus::ResyncRequired;
		}
		const auto existing = m_entries.find(change.address);
		if (existing != m_entries.end()) {
			const auto oldBytes = EncodedEntryBytes(existing->second);
			if (oldBytes > projectedPayloadBytes) {
				return EStorageChangeApplyStatus::ResyncRequired;
			}
			projectedPayloadBytes -= oldBytes;
		}
		if (change.entry) {
			if (change.entry->address != change.address || change.entry->target != change.target
				|| change.entry->revision != batch.revision || !IsValidEntry(*change.entry, batch.revision)
				|| !CheckedAdd(projectedPayloadBytes, EncodedEntryBytes(*change.entry),
					kMaximumStorageSnapshotPayloadBytes)) {
				return EStorageChangeApplyStatus::ResyncRequired;
			}
			if (existing == m_entries.end()) {
				++projectedEntryCount;
			}
		} else if (existing != m_entries.end()) {
			--projectedEntryCount;
		}
	}
	if (projectedEntryCount > kMaximumStorageItems) {
		return EStorageChangeApplyStatus::ResyncRequired;
	}

	for (const auto& change : batch.changes) {
		if (change.entry) {
			m_entries.insert_or_assign(change.address, *change.entry);
		} else {
			m_entries.erase(change.address);
		}
	}
	m_revision = batch.revision;
	return EStorageChangeApplyStatus::Applied;
}

std::optional<StorageEntry> CStorageSnapshotCache::Find(const StorageAddress& address) const
{
	std::shared_lock lock(m_mutex);
	const auto entry = m_entries.find(address);
	return entry == m_entries.end() ? std::nullopt : std::optional<StorageEntry>{ entry->second };
}

std::uint64_t CStorageSnapshotCache::GetGeneration() const
{
	std::shared_lock lock(m_mutex);
	return m_generation;
}

std::uint64_t CStorageSnapshotCache::GetRevision() const
{
	std::shared_lock lock(m_mutex);
	return m_revision;
}

bool CStorageSnapshotCache::Matches(std::uint64_t generation, std::uint64_t revision) const
{
	std::shared_lock lock(m_mutex);
	return m_generation == generation && m_revision == revision;
}

} // namespace platform::storage

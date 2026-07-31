/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"

#include "workbench/editor/DocumentRegistry.h"

namespace workbench::editor {

DocumentRegistry::AcquireResult DocumentRegistry::AcquireResolver(const ResolvedEditorDocument& document)
{
	return Acquire(document, true);
}

DocumentRegistry::AcquireResult DocumentRegistry::AcquireInput(const ResolvedEditorDocument& document)
{
	return Acquire(document, false);
}

std::optional<DocumentRegistry::AcquireResult> DocumentRegistry::AcquireExistingInput(const std::wstring& documentKey)
{
	const auto found = m_entries.find(documentKey);
	if (found == m_entries.end()) return std::nullopt;
	// Copy the public result before mutating the reference count so a failed snapshot
	// allocation cannot leave an unpaired acquired reference behind.
	auto document = ToSnapshot(found->first, found->second);
	++found->second.inputReferenceCount;
	++document.inputReferenceCount;
	return AcquireResult{ .document = std::move(document) };
}

DocumentRegistry::AcquireResult DocumentRegistry::Acquire(const ResolvedEditorDocument& document, bool resolverReference)
{
	std::wstring key;
	if (!document.identity.TryComparisonKey(key)) {
		return { .authoritativeStateConflict = true };
	}
	if (const auto found = m_entries.find(key); found != m_entries.end()) {
		auto snapshot = ToSnapshot(found->first, found->second);
		if (found->second.documentRevision != document.documentRevision || found->second.dirty != document.dirty) {
			return { .document = std::move(snapshot), .authoritativeStateConflict = true };
		}
		if (resolverReference) {
			++found->second.resolverReferenceCount;
			++snapshot.resolverReferenceCount;
		} else {
			++found->second.inputReferenceCount;
			++snapshot.inputReferenceCount;
		}
		return { .document = std::move(snapshot), .added = false };
	}

	Entry entry{
		.identity = document.identity,
		.documentRevision = document.documentRevision,
		.dirty = document.dirty,
		.inputReferenceCount = resolverReference ? std::size_t{ 0 } : std::size_t{ 1 },
		.resolverReferenceCount = resolverReference ? std::size_t{ 1 } : std::size_t{ 0 },
	};
	// Materialize the result before insertion. This makes a failed snapshot copy
	// leave the registry unchanged, matching the service's acquire/open rollback.
	auto snapshot = ToSnapshot(key, entry);
	const auto [inserted, wasInserted] = m_entries.emplace(key, std::move(entry));
	(void)wasInserted;
	(void)inserted;
	return { .document = std::move(snapshot), .added = true };
}

std::optional<DocumentRegistry::ReleaseResult> DocumentRegistry::ReleaseResolver(const std::wstring& documentKey)
{
	return Release(documentKey, true);
}

std::optional<DocumentRegistry::ReleaseResult> DocumentRegistry::ReleaseInput(const std::wstring& documentKey)
{
	return Release(documentKey, false);
}

bool DocumentRegistry::ReleaseInputReferenceWithoutSnapshot(const std::wstring& documentKey) noexcept
{
	const auto found = m_entries.find(documentKey);
	if (found == m_entries.end() || found->second.inputReferenceCount == 0) return false;
	--found->second.inputReferenceCount;
	if (found->second.inputReferenceCount == 0 && found->second.resolverReferenceCount == 0) {
		m_entries.erase(found);
	}
	return true;
}

std::optional<DocumentRegistry::ReleaseResult> DocumentRegistry::Release(const std::wstring& documentKey, bool resolverReference)
{
	const auto found = m_entries.find(documentKey);
	if (found == m_entries.end()) return std::nullopt;
	auto snapshot = ToSnapshot(found->first, found->second);
	if (resolverReference) {
		if (found->second.resolverReferenceCount == 0) return std::nullopt;
		--found->second.resolverReferenceCount;
		--snapshot.resolverReferenceCount;
	} else {
		if (found->second.inputReferenceCount == 0) return std::nullopt;
		--found->second.inputReferenceCount;
		--snapshot.inputReferenceCount;
	}

	const bool disposed = snapshot.inputReferenceCount == 0 && snapshot.resolverReferenceCount == 0;
	if (disposed) m_entries.erase(found);
	return ReleaseResult{ .document = std::move(snapshot), .disposed = disposed };
}

std::optional<EditorDocumentSnapshot> DocumentRegistry::Find(const std::wstring& documentKey) const
{
	if (const auto found = m_entries.find(documentKey); found != m_entries.end()) {
		return ToSnapshot(found->first, found->second);
	}
	return std::nullopt;
}

bool DocumentRegistry::SetState(const std::wstring& documentKey, bool dirty, std::uint64_t documentRevision)
{
	const auto found = m_entries.find(documentKey);
	if (found == m_entries.end()) return false;
	if (found->second.dirty == dirty && found->second.documentRevision == documentRevision) return false;
	found->second.dirty = dirty;
	found->second.documentRevision = documentRevision;
	return true;
}

std::vector<EditorDocumentSnapshot> DocumentRegistry::Snapshot() const
{
	std::vector<EditorDocumentSnapshot> snapshot;
	snapshot.reserve(m_entries.size());
	for (const auto& [key, entry] : m_entries) {
		snapshot.push_back(ToSnapshot(key, entry));
	}
	return snapshot;
}

EditorDocumentSnapshot DocumentRegistry::ToSnapshot(const std::wstring& key, const Entry& entry)
{
	return {
		.documentKey = key,
		.identity = entry.identity,
		.documentRevision = entry.documentRevision,
		.dirty = entry.dirty,
		.inputReferenceCount = entry.inputReferenceCount,
		.resolverReferenceCount = entry.resolverReferenceCount,
	};
}

} // namespace workbench::editor

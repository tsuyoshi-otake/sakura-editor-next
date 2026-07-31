/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "workbench/editor/EditorCoreTypes.h"

#include <map>
#include <optional>

namespace workbench::editor {

//! Shared resolved documents, deliberately separate from visible editor inputs and active selection.
//! The owning EditorCoreService serializes all access to this pure model.
class DocumentRegistry final {
public:
	struct AcquireResult {
		EditorDocumentSnapshot document;
		bool added = false;
		bool authoritativeStateConflict = false;
	};
	struct ReleaseResult {
		EditorDocumentSnapshot document;
		bool disposed = false;
	};

	//! Adds a resolver-owned reference. This is intentionally independent of visible editor inputs.
	[[nodiscard]] AcquireResult AcquireResolver(const ResolvedEditorDocument& document);
	//! Adds an editor-input reference. Existing canonical documents must agree with the resolver's state.
	[[nodiscard]] AcquireResult AcquireInput(const ResolvedEditorDocument& document);
	//! Adds an input reference to a resolver/opened document without receiving a second resolver state payload.
	[[nodiscard]] std::optional<AcquireResult> AcquireExistingInput(const std::wstring& documentKey);
	[[nodiscard]] std::optional<ReleaseResult> ReleaseResolver(const std::wstring& documentKey);
	[[nodiscard]] std::optional<ReleaseResult> ReleaseInput(const std::wstring& documentKey);
	//! Non-allocating reference rollback/transfer primitive for an already prepared editor-group commit.
	//! Callers must already have captured every public snapshot they need for notifications.
	[[nodiscard]] bool ReleaseInputReferenceWithoutSnapshot(const std::wstring& documentKey) noexcept;
	[[nodiscard]] std::optional<EditorDocumentSnapshot> Find(const std::wstring& documentKey) const;
	[[nodiscard]] bool SetState(const std::wstring& documentKey, bool dirty, std::uint64_t documentRevision);
	[[nodiscard]] std::vector<EditorDocumentSnapshot> Snapshot() const;

private:
	struct Entry {
		EditorDocumentIdentity identity;
		std::uint64_t documentRevision = 0;
		bool dirty = false;
		std::size_t inputReferenceCount = 0;
		std::size_t resolverReferenceCount = 0;
	};

	[[nodiscard]] AcquireResult Acquire(const ResolvedEditorDocument& document, bool resolverReference);
	[[nodiscard]] std::optional<ReleaseResult> Release(const std::wstring& documentKey, bool resolverReference);
	[[nodiscard]] static EditorDocumentSnapshot ToSnapshot(const std::wstring& key, const Entry& entry);

	std::map<std::wstring, Entry> m_entries;
};

} // namespace workbench::editor

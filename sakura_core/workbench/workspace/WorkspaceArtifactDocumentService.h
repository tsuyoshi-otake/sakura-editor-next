/*! @file
 * @brief Pure routing and retention for workspace task/debug/recommendation documents.
 */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#pragma once

#include <sakura/serialization/JsoncDocument.h>
#include <sakura/uri/UriIdentity.h>

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace workbench::workspace {

//! These documents belong to their eventual task and debug owners. They are
//! intentionally not configuration sources.
enum class EWorkspaceArtifactDocumentKind : std::uint8_t {
	Tasks,
	Launch,
};

//! A workspace-file member is the fallback for a folder-specific `.vscode`
//! document. A folder document wins only for its matching canonical folder URI.
enum class EWorkspaceArtifactDocumentSource : std::uint8_t {
	WorkspaceFile,
	Folder,
};

enum class EWorkspaceArtifactDocumentStatus : std::uint8_t {
	Applied,
	Cleared,
	StaleGeneration,
	StaleRevision,
	Stopped,
	InvalidSource,
	InvalidResource,
	InputTooLarge,
	InvalidUtf8,
	JsoncParseFailed,
	DuplicateKey,
	RootMustBeObject,
	ArtifactMustBeObject,
	InvalidSchema,
	DuplicateSource,
	MaximumDocumentsExceeded,
};

//! Input is supplied by a filesystem/watch adapter. The service owns neither
//! I/O nor watch handles, and does not expose Win32 concepts.
struct WorkspaceArtifactDocumentUpdate final {
	EWorkspaceArtifactDocumentKind kind = EWorkspaceArtifactDocumentKind::Tasks;
	EWorkspaceArtifactDocumentSource source = EWorkspaceArtifactDocumentSource::WorkspaceFile;
	std::optional<platform::uri::Uri> folderUri;
	platform::uri::Uri resource;
	std::uint64_t generation = 0;
	std::uint64_t revision = 0;
	std::string utf8;
};

//! A confirmed missing resource is distinct from corrupt content: it removes
//! only that source contribution so normal workspace-file fallback can resume.
struct WorkspaceArtifactDocumentRemoval final {
	EWorkspaceArtifactDocumentKind kind = EWorkspaceArtifactDocumentKind::Tasks;
	EWorkspaceArtifactDocumentSource source = EWorkspaceArtifactDocumentSource::WorkspaceFile;
	std::optional<platform::uri::Uri> folderUri;
	platform::uri::Uri resource;
	std::uint64_t generation = 0;
	std::uint64_t revision = 0;
};

//! Retains the complete JSONC source and root object. `artifact` is either the
//! whole folder document or the corresponding `.code-workspace` section;
//! unrelated workspace-file fields are therefore preserved in `root`.
struct WorkspaceArtifactDocument final {
	EWorkspaceArtifactDocumentKind kind = EWorkspaceArtifactDocumentKind::Tasks;
	EWorkspaceArtifactDocumentSource source = EWorkspaceArtifactDocumentSource::WorkspaceFile;
	std::optional<platform::uri::Uri> folderUri;
	platform::uri::Uri resource;
	std::uint64_t generation = 0;
	std::uint64_t revision = 0;
	std::string rawJsonc;
	platform::serialization::JsoncValue::Object root;
	platform::serialization::JsoncValue::Object artifact;
};

struct TasksDocumentSnapshot final { std::optional<WorkspaceArtifactDocument> document; };
struct LaunchDocumentSnapshot final { std::optional<WorkspaceArtifactDocument> document; };

struct WorkspaceArtifactDocumentServiceSnapshot final {
	std::uint64_t generation = 0;
	bool stopped = false;
	std::size_t acceptedDocuments = 0;
};

//! A bounded, order-preserving Tasks selection copied under one service lock.
//! `documents[index]` belongs only to `folderUris[index]`; no caller can infer
//! a default folder from the batch.  A stopped service still returns an empty
//! entry for each requested folder so positional correspondence is explicit.
enum class ETasksDocumentBatchStatus : std::uint8_t {
	Applied,
	Stopped,
	TooManyFolders,
	InvalidFolder,
};

struct TasksDocumentBatchSnapshot final {
	ETasksDocumentBatchStatus status = ETasksDocumentBatchStatus::InvalidFolder;
	WorkspaceArtifactDocumentServiceSnapshot service;
	std::vector<TasksDocumentSnapshot> documents;

	[[nodiscard]] bool Succeeded() const noexcept { return status == ETasksDocumentBatchStatus::Applied; }
};

struct WorkspaceArtifactDocumentResult final {
	EWorkspaceArtifactDocumentStatus status = EWorkspaceArtifactDocumentStatus::InvalidSource;
	std::optional<platform::serialization::JsoncDiagnostic> jsoncDiagnostic;
	std::string diagnostic;

	[[nodiscard]] bool Succeeded() const noexcept
	{
		return status == EWorkspaceArtifactDocumentStatus::Applied
			|| status == EWorkspaceArtifactDocumentStatus::Cleared;
	}
};

//! A pure, revisioned router. Folder documents have explicit higher precedence
//! than workspace-file members for their folder only. Invalid incoming data never
//! replaces a last accepted document. There is deliberately no Task/DAP execution
//! adapter here.
class CWorkspaceArtifactDocumentService final {
public:
	using Listener = std::function<void(const WorkspaceArtifactDocumentServiceSnapshot&)>;

	[[nodiscard]] WorkspaceArtifactDocumentResult Apply(WorkspaceArtifactDocumentUpdate update);
	[[nodiscard]] WorkspaceArtifactDocumentResult Remove(WorkspaceArtifactDocumentRemoval removal);
	//! Establishes an empty newer workspace generation before any documents exist.
	[[nodiscard]] WorkspaceArtifactDocumentResult BeginGeneration(std::uint64_t generation) noexcept;
	[[nodiscard]] WorkspaceArtifactDocumentResult Stop() noexcept;
	[[nodiscard]] WorkspaceArtifactDocumentResult Dispose() noexcept { return Stop(); }

	[[nodiscard]] TasksDocumentSnapshot Tasks(const std::optional<platform::uri::Uri>& folderUri = std::nullopt) const;
	//! Resolves at most 64 explicit folder URIs with the corresponding service
	//! generation/stopped state under one mutex acquisition.  The legacy Tasks()
	//! API remains available for single-source consumers.
	[[nodiscard]] TasksDocumentBatchSnapshot TasksForFolders(const std::vector<platform::uri::Uri>& folderUris) const;
	[[nodiscard]] LaunchDocumentSnapshot Launch(const std::optional<platform::uri::Uri>& folderUri = std::nullopt) const;
	[[nodiscard]] WorkspaceArtifactDocumentServiceSnapshot Snapshot() const;

	//! Listener delivery is model-only. Filesystem watchers remain external and
	//! call Apply themselves. Stop clears subscribers before future updates.
	[[nodiscard]] std::optional<std::uint64_t> Subscribe(Listener listener);
	void Unsubscribe(std::uint64_t subscriptionId) noexcept;

private:
	struct TrackedDocument final {
		std::wstring resourceIdentity;
		std::uint64_t revision = 0;
		std::optional<WorkspaceArtifactDocument> document;
	};

	[[nodiscard]] static std::optional<std::wstring> MakeKey(
		EWorkspaceArtifactDocumentKind kind,
		EWorkspaceArtifactDocumentSource source,
		const std::optional<platform::uri::Uri>& folderUri);
	[[nodiscard]] static bool IsResourceValid(const platform::uri::Uri& resource) noexcept;
	[[nodiscard]] static bool IsSchemaValid(
		EWorkspaceArtifactDocumentKind kind,
		const platform::serialization::JsoncValue::Object& artifact) noexcept;
	[[nodiscard]] std::optional<WorkspaceArtifactDocument> Resolve(
		EWorkspaceArtifactDocumentKind kind,
		const std::optional<platform::uri::Uri>& folderUri) const;
	[[nodiscard]] WorkspaceArtifactDocumentServiceSnapshot SnapshotLocked() const;
	void Notify(const WorkspaceArtifactDocumentServiceSnapshot& snapshot);

	mutable std::mutex m_mutex;
	std::uint64_t m_generation = 0;
	bool m_stopped = false;
	std::map<std::wstring, TrackedDocument, std::less<>> m_documents;
	std::map<std::uint64_t, Listener> m_listeners;
	std::uint64_t m_nextSubscriptionId = 1;
};

} // namespace workbench::workspace

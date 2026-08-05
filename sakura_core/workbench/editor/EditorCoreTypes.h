/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <sakura/uri/UriIdentity.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

//! UI-independent editor input and resolved-document state for the workbench.
namespace workbench::editor {

//! External identifiers are deliberately bounded before they become replay keys or registry identities.
inline constexpr std::size_t kMaxEditorOperationIdLength = 256;
inline constexpr std::size_t kMaxEditorInputIdLength = 256;
inline constexpr std::size_t kMaxEditorOpaqueDocumentIdLength = 512;
inline constexpr std::size_t kMaxEditorResourceComparisonKeyLength = 64 * 1024;

[[nodiscard]] constexpr bool IsValidEditorExternalId(std::string_view value, std::size_t maximumLength) noexcept
{
	return !value.empty() && value.size() <= maximumLength && value.find('\0') == std::string_view::npos;
}

//! The identity of a resolved document. A resource URI and an opaque identity are mutually exclusive.
struct EditorDocumentIdentity {
	std::optional<platform::uri::Uri> resource;
	std::optional<std::string> opaqueId;

	[[nodiscard]] bool IsValid() const noexcept;
	//! Builds the canonical, bounded registry key. It catches allocation failures so request validation stays terminal.
	[[nodiscard]] bool TryComparisonKey(std::wstring& key) const noexcept;
};

//! A visible editor input. inputId is opaque and unique within the one current editor group.
struct EditorInputDescriptor {
	std::string inputId;
	EditorDocumentIdentity documentIdentity;

	[[nodiscard]] bool IsValid() const noexcept;
};

//! Resolver output accepted by the synchronous first slice. Open never creates a document before this exists.
struct ResolvedEditorDocument {
	EditorDocumentIdentity identity;
	std::uint64_t documentRevision = 0;
	bool dirty = false;

	[[nodiscard]] bool IsValid() const noexcept;
};

//! Public snapshot of one shared document registry entry.
struct EditorDocumentSnapshot {
	std::wstring documentKey;
	EditorDocumentIdentity identity;
	std::uint64_t documentRevision = 0;
	bool dirty = false;
	std::size_t inputReferenceCount = 0;
	std::size_t resolverReferenceCount = 0;
};

//! Public snapshot of a visible input. It intentionally does not duplicate the resolved document state.
struct EditorInputSnapshot {
	EditorInputDescriptor descriptor;
	std::wstring documentKey;
};

struct EditorGroupSnapshot {
	std::vector<EditorInputSnapshot> inputs;
	std::optional<std::string> activeInputId;
};

struct EditorCoreSnapshot {
	std::uint64_t generation = 0;
	std::uint64_t revision = 0;
	EditorGroupSnapshot group;
	std::vector<EditorDocumentSnapshot> documents;
};

enum class EEditorOperationStatus : std::uint8_t {
	Succeeded,
	Cancelled,
	Failed,
	NotApplicable,
};

enum class EEditorOperationReason : std::uint8_t {
	None,
	InvalidOperationId,
	OperationIdConflict,
	RevisionConflict,
	InvalidInput,
	DocumentNotResolved,
	DocumentStateConflict,
	ResolverNotFound,
	InputAlreadyOpen,
	InputNotFound,
	AlreadyActive,
	DocumentNotFound,
	NoDocumentStateChange,
	LegacyBackendFailure,
};

struct EditorOperationMetadata {
	std::string operationId;
	std::optional<std::uint64_t> expectedModelRevision;
};

struct OpenResolvedInputRequest {
	EditorOperationMetadata operation;
	EditorInputDescriptor input;
	std::optional<ResolvedEditorDocument> resolvedDocument;
	//! False adopts the input into its group without changing that group's active selection.
	bool activate = true;
};

//! Equivalent to openTextDocument: retain a resolved model but do not make it visible or active.
struct ResolveDocumentRequest {
	EditorOperationMetadata operation;
	std::optional<ResolvedEditorDocument> resolvedDocument;
};

//! Releases one resolver-owned reference. A document is disposed only after this and all input references release.
struct ReleaseDocumentRequest {
	EditorOperationMetadata operation;
	EditorDocumentIdentity identity;
};

struct ShowInputRequest {
	EditorOperationMetadata operation;
	std::string inputId;
};

struct SetDocumentStateRequest {
	EditorOperationMetadata operation;
	std::string inputId;
	bool dirty = false;
	std::uint64_t documentRevision = 0;
};

struct CloseInputRequest {
	EditorOperationMetadata operation;
	std::string inputId;
};

//! Atomically retargets one visible input to another resolved working-copy document (for example Save As).
struct ReplaceInputDocumentRequest {
	EditorOperationMetadata operation;
	std::string inputId;
	std::optional<ResolvedEditorDocument> resolvedDocument;
};

enum class EEditorCoreChangeKind : std::uint8_t {
	DocumentAdded,
	DocumentResolved,
	DocumentResolverReleased,
	InputOpened,
	ActiveInputChanged,
	DocumentStateChanged,
	//! The input remains in its group position and keeps active selection while its document identity changes.
	InputDocumentReplaced,
	InputClosed,
	DocumentReleased,
};

//! A compact, revisioned event. activeInputId is meaningful for ActiveInputChanged, including an absent value.
struct EditorCoreChange {
	EEditorCoreChangeKind kind = EEditorCoreChangeKind::InputOpened;
	std::optional<std::string> inputId;
	std::optional<std::wstring> documentKey;
	std::optional<std::string> activeInputId;
	std::optional<std::uint64_t> documentRevision;
	std::optional<bool> dirty;
};

struct EditorCoreChangeBatch {
	std::uint64_t generation = 0;
	std::uint64_t baseRevision = 0;
	std::uint64_t revision = 0;
	std::vector<EditorCoreChange> changes;
};

struct EditorOperationResult {
	EEditorOperationStatus status = EEditorOperationStatus::Failed;
	EEditorOperationReason reason = EEditorOperationReason::None;
	std::uint64_t revision = 0;
	//! True only when a completed request result is returned from the bounded operation replay table.
	bool replayed = false;
	std::optional<EditorCoreChangeBatch> changeBatch;
};

using EditorCoreChangeCallback = std::function<void(const EditorCoreChangeBatch&)>;

class IEditorCoreSubscription {
public:
	virtual ~IEditorCoreSubscription() = default;
	virtual void Unsubscribe() noexcept = 0;
	[[nodiscard]] virtual bool IsSubscribed() const noexcept = 0;
};

} // namespace workbench::editor

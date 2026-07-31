/*! @file
 * @brief UI-neutral composition bridge for editor working-copy lifecycle boundaries.
 */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "workbench/editor/persistence/EditorWorkingCopyLifecycle.h"

#include <cstdint>
#include <optional>

namespace workbench::editor::persistence {

/*! @brief Current working-copy metadata sampled at an edit/save boundary.
 *
 * This deliberately has no content, encoding, title, window, native document,
 * or filesystem field. Full text capture remains exclusively in lifecycle Flush.
 */
struct EditorWorkingCopyCurrentChange final {
	WorkingCopyPersistenceIdentity identity;
	std::uint64_t contentVersion = 0;

	[[nodiscard]] bool IsValid() const noexcept;
};

/*! @brief Minimal metadata source borrowed by the lifecycle bridge.
 *
 * Return nullopt when there is no current input or when a stable, valid change
 * boundary cannot be supplied. Implementations must not capture document text.
 */
class IEditorWorkingCopyCurrentChangeSource {
public:
	virtual ~IEditorWorkingCopyCurrentChangeSource() = default;
	[[nodiscard]] virtual std::optional<EditorWorkingCopyCurrentChange> CurrentChange() const = 0;
};

/*! @brief Restore suppression policy supplied by the process composition root. */
struct EditorWorkingCopyRestorePolicy final {
	bool explicitCommandLine = false;
	bool multipleFiles = false;
	bool debugOrGrep = false;
};

/*! @brief Immutable identity/version pair captured before save or core close starts. */
struct EditorWorkingCopyCompletionToken final {
	WorkingCopyPersistenceIdentity identity;
	std::uint64_t contentVersion = 0;
	//! Opaque lifecycle revision captured with the identity/version boundary.
	std::uint64_t persistenceFence = 0;

	[[nodiscard]] bool IsValid() const noexcept;
};

//! Determines whether a successful save may replace the document identity.
enum class EEditorWorkingCopySaveCompletionMode : std::uint8_t {
	PreserveIdentity,
	AllowIdentityReplacement,
};

/*! @brief Binds one logical persistence scope to current editor change boundaries.
 *
 * The bridge owns the scope value and only borrows lifecycle services. It has no
 * UI/native/document/filesystem dependency. A composition root must construct it
 * only with a valid scope; methods still reject invalid input defensively.
 */
class EditorWorkingCopyLifecycleBridge final {
public:
	EditorWorkingCopyLifecycleBridge(WorkingCopyPersistenceScope scope,
		EditorWorkingCopyLifecycle& lifecycle,
		const IEditorWorkingCopyCurrentChangeSource& currentChangeSource) noexcept;

	[[nodiscard]] EditorWorkingCopyLifecycleResult Restore(
		const EditorWorkingCopyRestorePolicy& policy, bool layoutAndGroupReady);
	//! Records only identity and content version; it never captures document text.
	[[nodiscard]] EditorWorkingCopyLifecycleResult NotifyCurrentChanged(std::uint64_t nowTicks);
	//! The only bridge method that permits lifecycle to capture full document content.
	[[nodiscard]] EditorWorkingCopyLifecycleResult Flush(std::uint64_t nowTicks, bool force = false);
	//! Call before beginning an asynchronous save or before asking the core to close.
	[[nodiscard]] std::optional<EditorWorkingCopyCompletionToken> CaptureCurrentCompletionToken() const;
	//! Call only after a successful save. A newer version is stale and remains dirty.
	//! Save As must opt in to identity replacement; ordinary Save must preserve it.
	[[nodiscard]] EditorWorkingCopyLifecycleResult CompleteCurrentSave(
		const EditorWorkingCopyCompletionToken& token,
		EEditorWorkingCopySaveCompletionMode mode = EEditorWorkingCopySaveCompletionMode::PreserveIdentity);
	//! Call only after a successful core close. Cancelled/failed close must not call this method.
	[[nodiscard]] EditorWorkingCopyLifecycleResult CompletePreClose(
		const EditorWorkingCopyCompletionToken& token);

	void BeginShutdown() noexcept;
	void WillShutdown() noexcept;
	void Stop() noexcept;
	[[nodiscard]] EEditorWorkingCopyShutdownState ShutdownState() const noexcept;

private:
	[[nodiscard]] EditorWorkingCopyLifecycleResult InvalidScopeResult() const noexcept;
	[[nodiscard]] static EditorWorkingCopyLifecycleResult StaleResult() noexcept;

	WorkingCopyPersistenceScope m_scope;
	EditorWorkingCopyLifecycle& m_lifecycle;
	const IEditorWorkingCopyCurrentChangeSource& m_currentChangeSource;
};

} // namespace workbench::editor::persistence

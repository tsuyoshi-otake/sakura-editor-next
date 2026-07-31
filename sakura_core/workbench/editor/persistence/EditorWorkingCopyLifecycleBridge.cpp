/*! @file
 * @brief UI-neutral composition bridge for editor working-copy lifecycle boundaries.
 */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"

#include "workbench/editor/persistence/EditorWorkingCopyLifecycleBridge.h"

#include <utility>

namespace workbench::editor::persistence {
namespace {

[[nodiscard]] EditorWorkingCopyLifecycleResult Result(EEditorWorkingCopyLifecycleStatus status,
	EEditorWorkingCopyLifecycleReason reason = EEditorWorkingCopyLifecycleReason::None) noexcept
{
	return { .status = status, .reason = reason };
}

[[nodiscard]] bool CanCompleteSave(const EditorWorkingCopyCurrentChange& current,
	const EditorWorkingCopyCompletionToken& token,
	EEditorWorkingCopySaveCompletionMode mode) noexcept
{
	if (current.contentVersion != token.contentVersion) return false;
	return mode == EEditorWorkingCopySaveCompletionMode::AllowIdentityReplacement
		|| current.identity == token.identity;
}

} // namespace

bool EditorWorkingCopyCurrentChange::IsValid() const noexcept
{
	return identity.IsValid() && contentVersion != 0
		&& contentVersion <= kMaximumWorkingCopyPersistenceGeneration;
}

bool EditorWorkingCopyCompletionToken::IsValid() const noexcept
{
	return identity.IsValid() && contentVersion != 0
		&& contentVersion <= kMaximumWorkingCopyPersistenceGeneration
		&& persistenceFence != 0 && persistenceFence <= kMaximumWorkingCopyPersistenceGeneration;
}

EditorWorkingCopyLifecycleBridge::EditorWorkingCopyLifecycleBridge(WorkingCopyPersistenceScope scope,
	EditorWorkingCopyLifecycle& lifecycle,
	const IEditorWorkingCopyCurrentChangeSource& currentChangeSource) noexcept
	: m_scope(std::move(scope))
	, m_lifecycle(lifecycle)
	, m_currentChangeSource(currentChangeSource)
{
}

EditorWorkingCopyLifecycleResult EditorWorkingCopyLifecycleBridge::Restore(
	const EditorWorkingCopyRestorePolicy& policy, bool layoutAndGroupReady)
{
	if (!m_scope.IsValid()) return InvalidScopeResult();
	try {
		return m_lifecycle.Restore({
			.scope = m_scope,
			.layoutAndGroupReady = layoutAndGroupReady,
			.explicitCommandLine = policy.explicitCommandLine,
			.multipleFiles = policy.multipleFiles,
			.debugOrGrep = policy.debugOrGrep,
		});
	}
	catch (...) {
		return Result(EEditorWorkingCopyLifecycleStatus::Failed,
			EEditorWorkingCopyLifecycleReason::StoreFailed);
	}
}

EditorWorkingCopyLifecycleResult EditorWorkingCopyLifecycleBridge::NotifyCurrentChanged(std::uint64_t nowTicks)
{
	if (!m_scope.IsValid()) return InvalidScopeResult();
	std::optional<EditorWorkingCopyCurrentChange> current;
	try {
		current = m_currentChangeSource.CurrentChange();
	}
	catch (...) {
		return Result(EEditorWorkingCopyLifecycleStatus::Failed,
			EEditorWorkingCopyLifecycleReason::InvalidSnapshot);
	}
	if (!current) return Result(EEditorWorkingCopyLifecycleStatus::NotApplicable);
	if (!current->IsValid()) {
		return Result(EEditorWorkingCopyLifecycleStatus::Failed,
			EEditorWorkingCopyLifecycleReason::InvalidSnapshot);
	}
	try {
		return m_lifecycle.NotifyChanged(m_scope, current->identity, current->contentVersion, nowTicks);
	}
	catch (...) {
		return Result(EEditorWorkingCopyLifecycleStatus::Failed,
			EEditorWorkingCopyLifecycleReason::StoreFailed);
	}
}

EditorWorkingCopyLifecycleResult EditorWorkingCopyLifecycleBridge::Flush(std::uint64_t nowTicks, bool force)
{
	if (!m_scope.IsValid()) return InvalidScopeResult();
	try {
		return m_lifecycle.Flush(m_scope, nowTicks, force);
	}
	catch (...) {
		return Result(EEditorWorkingCopyLifecycleStatus::Failed,
			EEditorWorkingCopyLifecycleReason::StoreFailed);
	}
}

std::optional<EditorWorkingCopyCompletionToken>
EditorWorkingCopyLifecycleBridge::CaptureCurrentCompletionToken() const
{
	if (!m_scope.IsValid()) return std::nullopt;
	std::optional<EditorWorkingCopyCurrentChange> current;
	try {
		current = m_currentChangeSource.CurrentChange();
	}
	catch (...) {
		return std::nullopt;
	}
	if (!current || !current->IsValid()) return std::nullopt;
	const auto persistenceFence = m_lifecycle.CaptureCompletionFence(m_scope);
	if (!persistenceFence) return std::nullopt;
	return EditorWorkingCopyCompletionToken{
		.identity = current->identity,
		.contentVersion = current->contentVersion,
		.persistenceFence = *persistenceFence,
	};
}

EditorWorkingCopyLifecycleResult EditorWorkingCopyLifecycleBridge::CompleteCurrentSave(
	const EditorWorkingCopyCompletionToken& token,
	EEditorWorkingCopySaveCompletionMode mode)
{
	if (!m_scope.IsValid()) return InvalidScopeResult();
	if (!token.IsValid()) {
		return Result(EEditorWorkingCopyLifecycleStatus::Failed,
			EEditorWorkingCopyLifecycleReason::InvalidSnapshot);
	}
	std::optional<EditorWorkingCopyCurrentChange> current;
	try {
		current = m_currentChangeSource.CurrentChange();
	}
	catch (...) {
		return Result(EEditorWorkingCopyLifecycleStatus::Failed,
			EEditorWorkingCopyLifecycleReason::InvalidSnapshot);
	}
	if (!current || !current->IsValid() || !CanCompleteSave(*current, token, mode)) return StaleResult();
	try {
		return m_lifecycle.OnSavedOrClosed(m_scope,
			{ .identity = current->identity, .contentVersion = current->contentVersion }, token.persistenceFence,
			mode == EEditorWorkingCopySaveCompletionMode::AllowIdentityReplacement);
	}
	catch (...) {
		return Result(EEditorWorkingCopyLifecycleStatus::Failed,
			EEditorWorkingCopyLifecycleReason::StoreFailed);
	}
}

EditorWorkingCopyLifecycleResult EditorWorkingCopyLifecycleBridge::CompletePreClose(
	const EditorWorkingCopyCompletionToken& token)
{
	if (!m_scope.IsValid()) return InvalidScopeResult();
	if (!token.IsValid()) {
		return Result(EEditorWorkingCopyLifecycleStatus::Failed,
			EEditorWorkingCopyLifecycleReason::InvalidSnapshot);
	}
	try {
		return m_lifecycle.OnSavedOrClosed(m_scope,
			{ .identity = token.identity, .contentVersion = token.contentVersion }, token.persistenceFence);
	}
	catch (...) {
		return Result(EEditorWorkingCopyLifecycleStatus::Failed,
			EEditorWorkingCopyLifecycleReason::StoreFailed);
	}
}

void EditorWorkingCopyLifecycleBridge::BeginShutdown() noexcept
{
	m_lifecycle.BeginShutdown();
}

void EditorWorkingCopyLifecycleBridge::WillShutdown() noexcept
{
	m_lifecycle.WillShutdown();
}

void EditorWorkingCopyLifecycleBridge::Stop() noexcept
{
	m_lifecycle.Stop();
}

EEditorWorkingCopyShutdownState EditorWorkingCopyLifecycleBridge::ShutdownState() const noexcept
{
	return m_lifecycle.ShutdownState();
}

EditorWorkingCopyLifecycleResult EditorWorkingCopyLifecycleBridge::InvalidScopeResult() const noexcept
{
	return Result(EEditorWorkingCopyLifecycleStatus::Failed,
		EEditorWorkingCopyLifecycleReason::InvalidScope);
}

EditorWorkingCopyLifecycleResult EditorWorkingCopyLifecycleBridge::StaleResult() noexcept
{
	return Result(EEditorWorkingCopyLifecycleStatus::Conflict,
		EEditorWorkingCopyLifecycleReason::StaleSnapshot);
}

} // namespace workbench::editor::persistence

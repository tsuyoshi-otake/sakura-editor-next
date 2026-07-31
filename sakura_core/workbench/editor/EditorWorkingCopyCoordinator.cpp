/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"

#include "workbench/editor/EditorWorkingCopyCoordinator.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace workbench::editor {

struct EditorWorkingCopyCoordinator::LocatedInput {
	EditorInputSnapshot input;
	EditorDocumentSnapshot document;
	std::uint64_t coreRevision = 0;
};

namespace {

EditorWorkingCopyOperationResult CoreResult(const EditorOperationResult& result)
{
	EditorWorkingCopyOperationResult converted{ .coreRevision = result.revision };
	if (result.status == EEditorOperationStatus::Succeeded) {
		converted.status = EEditorWorkingCopyOperationStatus::Succeeded;
		return converted;
	}
	if (result.status == EEditorOperationStatus::NotApplicable) {
		converted.status = EEditorWorkingCopyOperationStatus::NotApplicable;
		converted.reason = result.reason == EEditorOperationReason::InputNotFound
			? EEditorWorkingCopyOperationReason::InputNotFound
			: EEditorWorkingCopyOperationReason::CoreRejected;
		return converted;
	}
	converted.status = result.reason == EEditorOperationReason::RevisionConflict
		|| result.reason == EEditorOperationReason::DocumentStateConflict
		? EEditorWorkingCopyOperationStatus::Conflict
		: EEditorWorkingCopyOperationStatus::Failed;
	converted.reason = result.reason == EEditorOperationReason::RevisionConflict
		? EEditorWorkingCopyOperationReason::RevisionConflict
		: result.reason == EEditorOperationReason::DocumentStateConflict
			? EEditorWorkingCopyOperationReason::DocumentStateConflict
			: EEditorWorkingCopyOperationReason::CoreRejected;
	return converted;
}

} // namespace

EditorWorkingCopyCoordinator::InFlightEffect::InFlightEffect(EditorWorkingCopyCoordinator& owner, std::wstring documentKey,
	std::string operationId) noexcept
	: m_owner(&owner)
	, m_documentKey(std::move(documentKey))
	, m_operationId(std::move(operationId))
{
}

EditorWorkingCopyCoordinator::InFlightEffect::InFlightEffect(InFlightEffect&& other) noexcept
	: m_owner(std::exchange(other.m_owner, nullptr))
	, m_documentKey(std::move(other.m_documentKey))
	, m_operationId(std::move(other.m_operationId))
{
}

EditorWorkingCopyCoordinator::InFlightEffect& EditorWorkingCopyCoordinator::InFlightEffect::operator=(InFlightEffect&& other) noexcept
{
	if (this != &other) {
		if (m_owner != nullptr) m_owner->EndEffect(m_documentKey, m_operationId);
		m_owner = std::exchange(other.m_owner, nullptr);
		m_documentKey = std::move(other.m_documentKey);
		m_operationId = std::move(other.m_operationId);
	}
	return *this;
}

EditorWorkingCopyCoordinator::InFlightEffect::~InFlightEffect()
{
	if (m_owner != nullptr) m_owner->EndEffect(m_documentKey, m_operationId);
}

EditorWorkingCopyCoordinator::PreparedRevert::PreparedRevert(IEditorWorkingCopyBackend& backend,
	EditorWorkingCopyRevertTransaction transaction) noexcept
	: m_backend(&backend)
	, m_transaction(transaction)
{
}

EditorWorkingCopyCoordinator::PreparedRevert::~PreparedRevert()
{
	Rollback();
}

void EditorWorkingCopyCoordinator::PreparedRevert::Finalize() noexcept
{
	if (m_backend == nullptr) return;
	m_backend->FinalizePreparedRevert(m_transaction);
	m_backend = nullptr;
}

void EditorWorkingCopyCoordinator::PreparedRevert::Rollback() noexcept
{
	if (m_backend == nullptr) return;
	m_backend->RollbackPreparedRevert(m_transaction);
	m_backend = nullptr;
}

EditorWorkingCopyCoordinator::EditorWorkingCopyCoordinator(EditorCoreService& core, IEditorWorkingCopyBackend& backend,
	std::size_t maxCompletedOperations)
	: m_core(core)
	, m_backend(backend)
	, m_maxCompletedOperations(std::max<std::size_t>(1, maxCompletedOperations))
{
}

EditorWorkingCopyCoordinatorSnapshot EditorWorkingCopyCoordinator::Snapshot() const
{
	const auto coreSnapshot = m_core.Snapshot();
	EditorWorkingCopyCoordinatorSnapshot result{ .coreRevision = coreSnapshot.revision };
	std::map<std::wstring, std::vector<std::string>> inputIds;
	for (const auto& input : coreSnapshot.group.inputs) inputIds[input.documentKey].push_back(input.descriptor.inputId);

	std::scoped_lock lock(m_mutex);
	result.workingCopies.reserve(coreSnapshot.documents.size() + coreSnapshot.group.inputs.size());
	for (const auto& document : coreSnapshot.documents) {
		EditorWorkingCopySnapshot copy{ .identity = document.identity, .version = document.documentRevision,
			.state = document.dirty ? EEditorWorkingCopyState::Dirty : EEditorWorkingCopyState::Saved };
		if (const auto inputs = inputIds.find(document.documentKey); inputs != inputIds.end()) copy.inputIds = inputs->second;
		if (const auto transient = m_transientStates.find(document.documentKey); transient != m_transientStates.end()) {
			copy.state = transient->second;
		}
		result.workingCopies.push_back(std::move(copy));
	}
	// The pure core normally prevents this. Keep an explicit projection should a future backend observe a detached input.
	for (const auto& input : coreSnapshot.group.inputs) {
		const auto found = std::find_if(coreSnapshot.documents.begin(), coreSnapshot.documents.end(), [&input](const auto& document) {
			return document.documentKey == input.documentKey;
		});
		if (found == coreSnapshot.documents.end()) {
			result.workingCopies.push_back({ .identity = input.descriptor.documentIdentity, .state = EEditorWorkingCopyState::Orphaned,
				.inputIds = { input.descriptor.inputId } });
		}
	}
	return result;
}

EditorWorkingCopyOperationResult EditorWorkingCopyCoordinator::SetDirty(const SetWorkingCopyDirtyRequest& request)
{
	const auto fingerprint = Fingerprint("dirty", request.operation, request.inputId) + std::to_string(request.version);
	std::optional<LocatedInput> located;
	bool handled = false;
	auto result = ValidateAndFind(request.operation, request.inputId, fingerprint, located, handled);
	if (handled) return result;
	if (!located) return result;

	const auto coreResult = m_core.SetDirty({ .operation = { .operationId = NextCoreOperationId(),
		.expectedModelRevision = located->coreRevision }, .inputId = request.inputId, .dirty = true, .documentRevision = request.version });
	result = CoreResult(coreResult);
	if (result.status == EEditorWorkingCopyOperationStatus::Succeeded) {
		{
			std::scoped_lock lock(m_mutex);
			m_transientStates.erase(located->input.documentKey);
		}
		const auto after = m_core.Snapshot();
		if (const auto current = Locate(after, request.inputId)) result.workingCopy = ToWorkingCopySnapshot(current->document, after);
	}
	{
		std::scoped_lock lock(m_mutex);
		RememberCompleted(request.operation.operationId, fingerprint, result);
	}
	return result;
}

EditorWorkingCopyOperationResult EditorWorkingCopyCoordinator::Save(const SaveWorkingCopyRequest& request)
{
	return SaveOrRevert(request.operation, request.inputId, request.options);
}

EditorWorkingCopyOperationResult EditorWorkingCopyCoordinator::SaveAs(const SaveWorkingCopyAsRequest& request)
{
	auto fingerprint = Fingerprint("save-as", request.operation, request.inputId);
	AppendSaveOptionsFingerprint(fingerprint, request.options);
	std::wstring targetKey;
	if (!request.targetIdentity) {
		fingerprint.append("|select-target");
	} else if (request.targetIdentity->TryComparisonKey(targetKey)) {
		fingerprint.append("|target:");
		for (const auto character : targetKey) {
			fingerprint.append(std::to_string(static_cast<std::uint32_t>(character)));
			fingerprint.push_back(',');
		}
	} else {
		fingerprint.append("invalid-target");
	}
	if (!IsValidSaveOptions(request.options)) {
		return { .status = EEditorWorkingCopyOperationStatus::Failed,
			.reason = EEditorWorkingCopyOperationReason::InvalidInput, .coreRevision = m_core.Snapshot().revision };
	}
	std::optional<LocatedInput> located;
	bool handled = false;
	auto result = ValidateAndFind(request.operation, request.inputId, fingerprint, located, handled);
	if (handled) return result;
	if (!located) return result;
	const auto effect = BeginEffect(*located, request.operation);
	if (!effect) return InFlightConflict(request.operation, fingerprint, *located);
	if (request.targetIdentity && !request.targetIdentity->IsValid()) {
		result = { .status = EEditorWorkingCopyOperationStatus::Failed,
			.reason = EEditorWorkingCopyOperationReason::InvalidInput, .coreRevision = located->coreRevision };
	} else {
		SetTransientState(located->input.documentKey, EEditorWorkingCopyState::PendingSave);
		EditorWorkingCopyBackendResult backendResult;
		try {
			backendResult = m_backend.SaveAs({
				.source = { .identity = located->document.identity, .version = located->document.documentRevision,
					.dirty = located->document.dirty, .saveOptions = request.options },
				.targetIdentity = request.targetIdentity,
			});
		} catch (...) {
			backendResult.status = EEditorWorkingCopyBackendStatus::Failed;
		}
		if (backendResult.status != EEditorWorkingCopyBackendStatus::Succeeded) {
			result = BackendTerminal(backendResult, located->coreRevision);
			if (result.status == EEditorWorkingCopyOperationStatus::Failed
				|| result.status == EEditorWorkingCopyOperationStatus::Unsupported) {
				SetTransientState(located->input.documentKey, EEditorWorkingCopyState::Error);
			} else {
				std::scoped_lock lock(m_mutex);
				m_transientStates.erase(located->input.documentKey);
			}
		} else {
			result = CommitSuccessfulSave(*located, request.inputId, backendResult, request.targetIdentity, true);
		}
	}
	{
		std::scoped_lock lock(m_mutex);
		RememberCompleted(request.operation.operationId, std::move(fingerprint), result);
	}
	return result;
}

EditorWorkingCopyOperationResult EditorWorkingCopyCoordinator::Revert(const RevertWorkingCopyRequest& request)
{
	return RevertTransactional(request.operation, request.inputId);
}

EditorWorkingCopyOperationResult EditorWorkingCopyCoordinator::Close(const CloseWorkingCopyRequest& request)
{
	const auto fingerprint = Fingerprint("close", request.operation, request.inputId)
		+ (request.suppressConfirmation ? "|suppress-confirmation" : "|confirm")
		+ (request.disposition == EEditorWorkingCopyCloseDisposition::DisposeWindow ? "|dispose-window" : "|initialize-empty-document");
	std::optional<LocatedInput> located;
	bool handled = false;
	auto result = ValidateAndFind(request.operation, request.inputId, fingerprint, located, handled);
	if (handled) return result;
	if (!located) return result;
	const auto effect = BeginEffect(*located, request.operation);
	if (!effect) return InFlightConflict(request.operation, fingerprint, *located);
	const EditorWorkingCopyBackendRequest backendRequest{ .identity = located->document.identity,
		.version = located->document.documentRevision, .dirty = located->document.dirty,
		.suppressCloseConfirmation = request.suppressConfirmation, .closeDisposition = request.disposition };

	EditorWorkingCopyBackendResult backendResult;
	try {
		backendResult = m_backend.PrepareClose(backendRequest);
	} catch (...) {
		backendResult.status = EEditorWorkingCopyBackendStatus::Failed;
	}
	if (backendResult.status != EEditorWorkingCopyBackendStatus::Succeeded) {
		result = BackendTerminal(backendResult, located->coreRevision);
		if (result.status == EEditorWorkingCopyOperationStatus::Failed || result.status == EEditorWorkingCopyOperationStatus::Unsupported) {
			SetTransientState(located->input.documentKey, EEditorWorkingCopyState::Error);
		} else {
			std::scoped_lock lock(m_mutex);
			m_transientStates.erase(located->input.documentKey);
		}
	} else {
		const auto currentSnapshot = m_core.Snapshot();
		const auto current = Locate(currentSnapshot, request.inputId);
		if (!current || !SameDocumentState(*located, *current)) {
			result = { .status = EEditorWorkingCopyOperationStatus::Conflict,
				.reason = EEditorWorkingCopyOperationReason::DocumentStateConflict, .coreRevision = currentSnapshot.revision };
			SetTransientState(located->input.documentKey, EEditorWorkingCopyState::Conflict);
		} else {
			result = CoreResult(m_core.CloseInput({ .operation = { .operationId = NextCoreOperationId(),
				.expectedModelRevision = current->coreRevision }, .inputId = request.inputId }));
			if (result.status == EEditorWorkingCopyOperationStatus::Succeeded) {
				m_backend.CommitClose(backendRequest);
				std::scoped_lock lock(m_mutex);
				m_transientStates.erase(located->input.documentKey);
			} else if (result.status == EEditorWorkingCopyOperationStatus::Conflict) {
				SetTransientState(located->input.documentKey, EEditorWorkingCopyState::Conflict);
			}
		}
	}
	{
		std::scoped_lock lock(m_mutex);
		RememberCompleted(request.operation.operationId, fingerprint, result);
	}
	return result;
}

EditorWorkingCopyOperationResult EditorWorkingCopyCoordinator::SaveOrRevert(const EditorWorkingCopyOperationMetadata& operation,
	const std::string& inputId, const EditorWorkingCopySaveOptions& saveOptions)
{
	auto fingerprint = Fingerprint("save", operation, inputId);
	AppendSaveOptionsFingerprint(fingerprint, saveOptions);
	if (!IsValidSaveOptions(saveOptions)) {
		return { .status = EEditorWorkingCopyOperationStatus::Failed,
			.reason = EEditorWorkingCopyOperationReason::InvalidInput, .coreRevision = m_core.Snapshot().revision };
	}
	std::optional<LocatedInput> located;
	bool handled = false;
	auto result = ValidateAndFind(operation, inputId, fingerprint, located, handled);
	if (handled) return result;
	if (!located) return result;
	if (saveOptions.targetPolicy == EEditorWorkingCopySaveTargetPolicy::ExistingOnly
		&& !located->document.identity.resource) {
		result = { .status = EEditorWorkingCopyOperationStatus::NotApplicable,
			.reason = EEditorWorkingCopyOperationReason::NoWorkingCopyStateChange,
			.coreRevision = located->coreRevision,
			.workingCopy = ToWorkingCopySnapshot(located->document, m_core.Snapshot()) };
		std::scoped_lock lock(m_mutex);
		RememberCompleted(operation.operationId, std::move(fingerprint), result);
		return result;
	}
	if (!located->document.dirty && !saveOptions.forceWrite) {
		result = { .status = EEditorWorkingCopyOperationStatus::NotApplicable,
			.reason = EEditorWorkingCopyOperationReason::NoWorkingCopyStateChange,
			.coreRevision = located->coreRevision,
			.workingCopy = ToWorkingCopySnapshot(located->document, m_core.Snapshot()) };
		std::scoped_lock lock(m_mutex);
		RememberCompleted(operation.operationId, fingerprint, result);
		return result;
	}
	const auto effect = BeginEffect(*located, operation);
	if (!effect) return InFlightConflict(operation, fingerprint, *located);
	const EditorWorkingCopyBackendRequest backendRequest{ .identity = located->document.identity,
		.version = located->document.documentRevision, .dirty = located->document.dirty,
		.saveOptions = saveOptions };
	SetTransientState(located->input.documentKey, EEditorWorkingCopyState::PendingSave);

	EditorWorkingCopyBackendResult backendResult;
	try {
		backendResult = m_backend.Save(backendRequest);
	} catch (...) {
		backendResult.status = EEditorWorkingCopyBackendStatus::Failed;
	}
	if (backendResult.status != EEditorWorkingCopyBackendStatus::Succeeded) {
		result = BackendTerminal(backendResult, located->coreRevision);
		if (result.status == EEditorWorkingCopyOperationStatus::Failed || result.status == EEditorWorkingCopyOperationStatus::Unsupported) {
			SetTransientState(located->input.documentKey, EEditorWorkingCopyState::Error);
		} else {
			std::scoped_lock lock(m_mutex);
			m_transientStates.erase(located->input.documentKey);
		}
	} else {
		result = CommitSuccessfulSave(*located, inputId, backendResult, std::nullopt, true);
	}
	{
		std::scoped_lock lock(m_mutex);
		RememberCompleted(operation.operationId, fingerprint, result);
	}
	return result;
}

EditorWorkingCopyOperationResult EditorWorkingCopyCoordinator::RevertTransactional(
	const EditorWorkingCopyOperationMetadata& operation, const std::string& inputId)
{
	const auto fingerprint = Fingerprint("revert", operation, inputId);
	std::optional<LocatedInput> located;
	bool handled = false;
	auto result = ValidateAndFind(operation, inputId, fingerprint, located, handled);
	if (handled) return result;
	if (!located) return result;
	const auto effect = BeginEffect(*located, operation);
	if (!effect) return InFlightConflict(operation, fingerprint, *located);

	const EditorWorkingCopyBackendRequest backendRequest{
		.identity = located->document.identity,
		.version = located->document.documentRevision,
		.dirty = located->document.dirty,
	};
	EditorWorkingCopyBackendRevertPrepareResult prepared;
	try {
		prepared = m_backend.PrepareRevert(backendRequest);
	}
	catch (...) {
		prepared.result.status = EEditorWorkingCopyBackendStatus::Failed;
	}

	if (prepared.result.status != EEditorWorkingCopyBackendStatus::Succeeded) {
		result = BackendTerminal(prepared.result, located->coreRevision);
		if (result.status == EEditorWorkingCopyOperationStatus::Failed
			|| result.status == EEditorWorkingCopyOperationStatus::Unsupported) {
			SetTransientState(located->input.documentKey, EEditorWorkingCopyState::Error);
		} else {
			std::scoped_lock lock(m_mutex);
			m_transientStates.erase(located->input.documentKey);
		}
	} else if (!prepared.transaction.IsValid()) {
		// A successful prepare without a token violates the backend contract.  There is no native apply to undo.
		result = { .status = EEditorWorkingCopyOperationStatus::Failed,
			.reason = EEditorWorkingCopyOperationReason::BackendApplyFailed, .coreRevision = located->coreRevision };
		SetTransientState(located->input.documentKey, EEditorWorkingCopyState::Error);
	} else {
		// From this point the guard has sole terminal ownership of the staged native state.  It rolls back on every
		// validation failure, Core rejection, and stack unwind until Finalize transfers ownership back to the backend.
		PreparedRevert transaction(m_backend, prepared.transaction);
		if (prepared.result.resultingIdentity && !SameIdentity(*prepared.result.resultingIdentity, located->document.identity)) {
			result = { .status = EEditorWorkingCopyOperationStatus::Conflict,
				.reason = EEditorWorkingCopyOperationReason::BackendIdentityMismatch, .coreRevision = located->coreRevision };
			SetTransientState(located->input.documentKey, EEditorWorkingCopyState::Conflict);
		} else if (prepared.result.successfulVersion <= located->document.documentRevision) {
			// Revert replaces text from outside the Core model, so it must publish one strictly newer content generation.
			// File timestamps and the pre-revert version are not valid content-version values.
			result = { .status = EEditorWorkingCopyOperationStatus::Conflict,
				.reason = EEditorWorkingCopyOperationReason::BackendVersionMismatch, .coreRevision = located->coreRevision };
			SetTransientState(located->input.documentKey, EEditorWorkingCopyState::Conflict);
		} else {
			const auto currentSnapshot = m_core.Snapshot();
			const auto current = Locate(currentSnapshot, inputId);
			if (!current || !SameDocumentState(*located, *current)) {
				result = { .status = EEditorWorkingCopyOperationStatus::Conflict,
					.reason = EEditorWorkingCopyOperationReason::DocumentStateConflict, .coreRevision = currentSnapshot.revision };
				SetTransientState(located->input.documentKey, EEditorWorkingCopyState::Conflict);
			} else {
				std::string coreOperationId;
				try {
					// Allocate the Core operation ID before the no-throw native swap, so allocation failure cannot leave
					// native state applied without a rollback owner.
					coreOperationId = NextCoreOperationId();
				}
				catch (...) {
					result = { .status = EEditorWorkingCopyOperationStatus::Failed,
						.reason = EEditorWorkingCopyOperationReason::CoreRejected, .coreRevision = currentSnapshot.revision };
					SetTransientState(located->input.documentKey, EEditorWorkingCopyState::Error);
				}
				if (coreOperationId.empty()) {
					// The catch above supplied this operation's terminal result; transaction destruction discards the stage.
				} else if (m_backend.ApplyPreparedRevert(prepared.transaction)
					!= EEditorWorkingCopyBackendRevertApplyStatus::Applied) {
					transaction.Rollback();
					result = { .status = EEditorWorkingCopyOperationStatus::Failed,
						.reason = EEditorWorkingCopyOperationReason::BackendApplyFailed, .coreRevision = currentSnapshot.revision };
					SetTransientState(located->input.documentKey, EEditorWorkingCopyState::Error);
				} else {
					try {
						result = CoreResult(m_core.SetDocumentState({
							.operation = { .operationId = std::move(coreOperationId), .expectedModelRevision = current->coreRevision },
							.inputId = inputId,
							.dirty = false,
							.documentRevision = prepared.result.successfulVersion,
						}));
					}
					catch (...) {
						result = { .status = EEditorWorkingCopyOperationStatus::Failed,
							.reason = EEditorWorkingCopyOperationReason::CoreRejected, .coreRevision = m_core.Snapshot().revision };
					}
					if (result.status == EEditorWorkingCopyOperationStatus::Succeeded) {
						// Finalize is noexcept by contract.  It only releases retained rollback state; all visible native state was
						// already installed by the prevalidated Apply phase.
						transaction.Finalize();
						{
							std::scoped_lock lock(m_mutex);
							m_transientStates.erase(located->input.documentKey);
						}
						const auto after = m_core.Snapshot();
						if (const auto changed = Locate(after, inputId)) result.workingCopy = ToWorkingCopySnapshot(changed->document, after);
					} else {
						// Core is authoritative for whether a staged native swap becomes visible.  Revert exact native state before
						// publishing a conflict/error result to callers.
						transaction.Rollback();
						SetTransientState(located->input.documentKey,
							result.status == EEditorWorkingCopyOperationStatus::Conflict
								? EEditorWorkingCopyState::Conflict
								: EEditorWorkingCopyState::Error);
					}
				}
			}
		}
	}
	{
		std::scoped_lock lock(m_mutex);
		RememberCompleted(operation.operationId, fingerprint, result);
	}
	return result;
}

EditorWorkingCopyOperationResult EditorWorkingCopyCoordinator::CommitSuccessfulSave(const LocatedInput& located,
	const std::string& inputId, const EditorWorkingCopyBackendResult& backendResult,
	const std::optional<EditorDocumentIdentity>& requiredIdentity, bool allowIdentityReplacement)
{
	const auto currentSnapshot = m_core.Snapshot();
	const auto current = Locate(currentSnapshot, inputId);
	if (!current || !SameDocumentState(located, *current)) {
		SetTransientState(located.input.documentKey, EEditorWorkingCopyState::Conflict);
		return { .status = EEditorWorkingCopyOperationStatus::Conflict,
			.reason = EEditorWorkingCopyOperationReason::DocumentStateConflict,
			.coreRevision = currentSnapshot.revision };
	}
	if (backendResult.successfulVersion != located.document.documentRevision) {
		SetTransientState(located.input.documentKey, EEditorWorkingCopyState::Conflict);
		return { .status = EEditorWorkingCopyOperationStatus::Conflict,
			.reason = EEditorWorkingCopyOperationReason::BackendVersionMismatch,
			.coreRevision = currentSnapshot.revision };
	}

	const auto& resultingIdentity = backendResult.resultingIdentity.value_or(located.document.identity);
	if (!resultingIdentity.IsValid()
		|| (requiredIdentity && !SameIdentity(resultingIdentity, *requiredIdentity))
		|| (!allowIdentityReplacement && !SameIdentity(resultingIdentity, located.document.identity))) {
		SetTransientState(located.input.documentKey, EEditorWorkingCopyState::Conflict);
		return { .status = EEditorWorkingCopyOperationStatus::Conflict,
			.reason = EEditorWorkingCopyOperationReason::BackendIdentityMismatch,
			.coreRevision = currentSnapshot.revision };
	}
	if (!located.document.dirty && SameIdentity(resultingIdentity, located.document.identity)) {
		// A forced physical write of a clean document is a successful backend effect but intentionally has no Core
		// document-state transition.  The revalidation above still prevents a callback from hiding a newer edit.
		{
			std::scoped_lock lock(m_mutex);
			m_transientStates.erase(located.input.documentKey);
		}
		return { .status = EEditorWorkingCopyOperationStatus::Succeeded, .coreRevision = currentSnapshot.revision,
			.workingCopy = ToWorkingCopySnapshot(current->document, currentSnapshot) };
	}

	EditorWorkingCopyOperationResult result;
	if (SameIdentity(resultingIdentity, located.document.identity)) {
		result = CoreResult(m_core.SetDocumentState({
			.operation = { .operationId = NextCoreOperationId(), .expectedModelRevision = current->coreRevision },
			.inputId = inputId,
			.dirty = false,
			.documentRevision = backendResult.successfulVersion,
		}));
	} else {
		result = CoreResult(m_core.ReplaceInputDocument({
			.operation = { .operationId = NextCoreOperationId(), .expectedModelRevision = current->coreRevision },
			.inputId = inputId,
			.resolvedDocument = ResolvedEditorDocument{
				.identity = resultingIdentity,
				.documentRevision = backendResult.successfulVersion,
				.dirty = false,
			},
		}));
	}
	if (result.status == EEditorWorkingCopyOperationStatus::Succeeded) {
		{
			std::scoped_lock lock(m_mutex);
			m_transientStates.erase(located.input.documentKey);
		}
		const auto after = m_core.Snapshot();
		if (const auto changed = Locate(after, inputId)) result.workingCopy = ToWorkingCopySnapshot(changed->document, after);
	} else if (result.status == EEditorWorkingCopyOperationStatus::Conflict) {
		SetTransientState(located.input.documentKey, EEditorWorkingCopyState::Conflict);
	} else {
		SetTransientState(located.input.documentKey, EEditorWorkingCopyState::Error);
	}
	return result;
}

std::optional<EditorWorkingCopyCoordinator::LocatedInput> EditorWorkingCopyCoordinator::Locate(
	const EditorCoreSnapshot& snapshot, const std::string& inputId) const
{
	const auto input = std::find_if(snapshot.group.inputs.begin(), snapshot.group.inputs.end(), [&inputId](const auto& candidate) {
		return candidate.descriptor.inputId == inputId;
	});
	if (input == snapshot.group.inputs.end()) return std::nullopt;
	const auto document = std::find_if(snapshot.documents.begin(), snapshot.documents.end(), [&input](const auto& candidate) {
		return candidate.documentKey == input->documentKey;
	});
	if (document == snapshot.documents.end()) return std::nullopt;
	return LocatedInput{ .input = *input, .document = *document, .coreRevision = snapshot.revision };
}

EditorWorkingCopyOperationResult EditorWorkingCopyCoordinator::ValidateAndFind(const EditorWorkingCopyOperationMetadata& operation,
	const std::string& inputId, const std::string& fingerprint, std::optional<LocatedInput>& located, bool& handled)
{
	const auto snapshot = m_core.Snapshot();
	handled = false;
	if (!IsValidOperation(operation)) {
		handled = true;
		return { .status = EEditorWorkingCopyOperationStatus::Failed, .reason = EEditorWorkingCopyOperationReason::InvalidOperationId,
			.coreRevision = snapshot.revision };
	}
	if (!IsValidInputId(inputId)) {
		handled = true;
		return { .status = EEditorWorkingCopyOperationStatus::Failed, .reason = EEditorWorkingCopyOperationReason::InvalidInput,
			.coreRevision = snapshot.revision };
	}
	{
		std::scoped_lock lock(m_mutex);
		auto replay = CheckCompletedOrConflict(operation, fingerprint, handled);
		if (handled) return replay;
	}
	if (operation.expectedModelRevision && *operation.expectedModelRevision != snapshot.revision) {
		handled = true;
		const EditorWorkingCopyOperationResult result{ .status = EEditorWorkingCopyOperationStatus::Conflict,
			.reason = EEditorWorkingCopyOperationReason::RevisionConflict, .coreRevision = snapshot.revision };
		std::scoped_lock lock(m_mutex);
		RememberCompleted(operation.operationId, fingerprint, result);
		return result;
	}
	located = Locate(snapshot, inputId);
	if (!located) {
		handled = true;
		const EditorWorkingCopyOperationResult result{ .status = EEditorWorkingCopyOperationStatus::NotApplicable,
			.reason = EEditorWorkingCopyOperationReason::InputNotFound, .coreRevision = snapshot.revision };
		std::scoped_lock lock(m_mutex);
		RememberCompleted(operation.operationId, fingerprint, result);
		return result;
	}
	return { .coreRevision = snapshot.revision };
}

EditorWorkingCopyOperationResult EditorWorkingCopyCoordinator::CheckCompletedOrConflict(
	const EditorWorkingCopyOperationMetadata& operation, const std::string& fingerprint, bool& handled) const
{
	handled = true;
	if (const auto completed = m_completedOperations.find(operation.operationId); completed != m_completedOperations.end()) {
		if (completed->second.fingerprint == fingerprint) {
			auto replay = completed->second.result;
			replay.replayed = true;
			return replay;
		}
		return { .status = EEditorWorkingCopyOperationStatus::Conflict,
			.reason = EEditorWorkingCopyOperationReason::OperationIdConflict, .coreRevision = m_core.Snapshot().revision };
	}
	handled = false;
	return {};
}

std::optional<EditorWorkingCopyCoordinator::InFlightEffect> EditorWorkingCopyCoordinator::BeginEffect(const LocatedInput& located,
	const EditorWorkingCopyOperationMetadata& operation)
{
	std::wstring documentKey;
	std::string operationId;
	try {
		documentKey = located.input.documentKey;
		operationId = operation.operationId;
	} catch (...) {
		return std::nullopt;
	}
	std::scoped_lock lock(m_mutex);
	if (m_inFlightDocumentOperations.contains(documentKey)
		|| m_inFlightOperationIds.contains(operationId)) {
		return std::nullopt;
	}
	const auto [documentIt, inserted] = m_inFlightDocumentOperations.emplace(documentKey, operationId);
	if (!inserted) return std::nullopt;
	try {
		m_inFlightOperationIds.emplace(operationId, documentKey);
	} catch (...) {
		m_inFlightDocumentOperations.erase(documentIt);
		return std::nullopt;
	}
	return InFlightEffect(*this, std::move(documentKey), std::move(operationId));
}

EditorWorkingCopyOperationResult EditorWorkingCopyCoordinator::InFlightConflict(const EditorWorkingCopyOperationMetadata& operation,
	const std::string& fingerprint, const LocatedInput& located)
{
	const EditorWorkingCopyOperationResult result{ .status = EEditorWorkingCopyOperationStatus::Conflict,
		.reason = EEditorWorkingCopyOperationReason::OperationInProgress, .coreRevision = located.coreRevision };
	std::scoped_lock lock(m_mutex);
	// The owner of an in-flight operation ID is solely responsible for its final replay entry.
	if (!m_inFlightOperationIds.contains(operation.operationId)) RememberCompleted(operation.operationId, fingerprint, result);
	return result;
}

void EditorWorkingCopyCoordinator::EndEffect(const std::wstring& documentKey, const std::string& operationId) noexcept
{
	std::scoped_lock lock(m_mutex);
	m_inFlightDocumentOperations.erase(documentKey);
	m_inFlightOperationIds.erase(operationId);
}

void EditorWorkingCopyCoordinator::RememberCompleted(const std::string& operationId, std::string fingerprint,
	const EditorWorkingCopyOperationResult& result)
{
	m_completedOperations.emplace(operationId, CompletedOperation{ .fingerprint = std::move(fingerprint), .result = result });
	m_completedOperationOrder.push_back(operationId);
	while (m_completedOperationOrder.size() > m_maxCompletedOperations) {
		m_completedOperations.erase(m_completedOperationOrder.front());
		m_completedOperationOrder.pop_front();
	}
}

std::string EditorWorkingCopyCoordinator::NextCoreOperationId()
{
	std::scoped_lock lock(m_mutex);
	return "working-copy:" + std::to_string(m_nextCoreOperation++);
}

void EditorWorkingCopyCoordinator::SetTransientState(const std::wstring& documentKey, EEditorWorkingCopyState state)
{
	std::scoped_lock lock(m_mutex);
	m_transientStates[documentKey] = state;
}

EditorWorkingCopyOperationResult EditorWorkingCopyCoordinator::BackendTerminal(const EditorWorkingCopyBackendResult& result,
	std::uint64_t coreRevision) const
{
	switch (result.status) {
	case EEditorWorkingCopyBackendStatus::Cancelled:
		return { .status = EEditorWorkingCopyOperationStatus::Cancelled, .reason = EEditorWorkingCopyOperationReason::BackendCancelled,
			.coreRevision = coreRevision };
	case EEditorWorkingCopyBackendStatus::Unsupported:
		return { .status = EEditorWorkingCopyOperationStatus::Unsupported, .reason = EEditorWorkingCopyOperationReason::BackendUnsupported,
			.coreRevision = coreRevision };
	case EEditorWorkingCopyBackendStatus::Failed:
	default:
		return { .status = EEditorWorkingCopyOperationStatus::Failed, .reason = EEditorWorkingCopyOperationReason::BackendFailed,
			.coreRevision = coreRevision };
	}
}

EditorWorkingCopySnapshot EditorWorkingCopyCoordinator::ToWorkingCopySnapshot(const EditorDocumentSnapshot& document,
	const EditorCoreSnapshot& coreSnapshot) const
{
	EditorWorkingCopySnapshot result{ .identity = document.identity, .version = document.documentRevision,
		.state = document.dirty ? EEditorWorkingCopyState::Dirty : EEditorWorkingCopyState::Saved };
	for (const auto& input : coreSnapshot.group.inputs) {
		if (input.documentKey == document.documentKey) result.inputIds.push_back(input.descriptor.inputId);
	}
	std::scoped_lock lock(m_mutex);
	if (const auto transient = m_transientStates.find(document.documentKey); transient != m_transientStates.end()) {
		result.state = transient->second;
	}
	return result;
}

bool EditorWorkingCopyCoordinator::SameDocumentState(const LocatedInput& before, const LocatedInput& after) noexcept
{
	return before.input.documentKey == after.input.documentKey
		&& before.document.documentRevision == after.document.documentRevision
		&& before.document.dirty == after.document.dirty;
}

bool EditorWorkingCopyCoordinator::SameIdentity(
	const EditorDocumentIdentity& left, const EditorDocumentIdentity& right) noexcept
{
	std::wstring leftKey;
	std::wstring rightKey;
	return left.TryComparisonKey(leftKey) && right.TryComparisonKey(rightKey) && leftKey == rightKey;
}

bool EditorWorkingCopyCoordinator::IsValidOperation(const EditorWorkingCopyOperationMetadata& operation) noexcept
{
	return IsValidEditorExternalId(operation.operationId, kMaxEditorOperationIdLength);
}

bool EditorWorkingCopyCoordinator::IsValidInputId(const std::string& inputId) noexcept
{
	return IsValidEditorExternalId(inputId, kMaxEditorInputIdLength);
}

bool EditorWorkingCopyCoordinator::IsValidSaveOptions(const EditorWorkingCopySaveOptions& options) noexcept
{
	if (options.targetPolicy != EEditorWorkingCopySaveTargetPolicy::AcquireIfMissing
		&& options.targetPolicy != EEditorWorkingCopySaveTargetPolicy::ExistingOnly) return false;
	if (options.lineEnding != EEditorWorkingCopyLineEnding::Preserve
		&& options.lineEnding != EEditorWorkingCopyLineEnding::CrLf
		&& options.lineEnding != EEditorWorkingCopyLineEnding::Lf
		&& options.lineEnding != EEditorWorkingCopyLineEnding::Cr) return false;
	const auto isStrictUtf8 = [](const std::string& value) noexcept {
		for (std::size_t index = 0; index < value.size();) {
			const auto first = static_cast<unsigned char>(value[index]);
			if (first <= 0x7fU) {
				++index;
				continue;
			}
			std::size_t trailing = 0;
			std::uint32_t scalar = 0;
			if (first >= 0xc2U && first <= 0xdfU) { trailing = 1; scalar = first & 0x1fU; }
			else if (first >= 0xe0U && first <= 0xefU) { trailing = 2; scalar = first & 0x0fU; }
			else if (first >= 0xf0U && first <= 0xf4U) { trailing = 3; scalar = first & 0x07U; }
			else return false;
			if (index + trailing >= value.size()) return false;
			for (std::size_t offset = 1; offset <= trailing; ++offset) {
				const auto next = static_cast<unsigned char>(value[index + offset]);
				if ((next & 0xc0U) != 0x80U) return false;
				scalar = (scalar << 6U) | (next & 0x3fU);
			}
			if ((trailing == 2 && scalar < 0x800U) || (trailing == 3 && scalar < 0x10000U)
				|| (scalar >= 0xd800U && scalar <= 0xdfffU) || scalar > 0x10ffffU) return false;
			index += trailing + 1;
		}
		return true;
	};
	if (options.suggestedTarget.size() > 4096 || options.suggestedTarget.find('\0') != std::string::npos
		|| !isStrictUtf8(options.suggestedTarget)) return false;
	if (!options.encodingId) return true;
	const auto& encoding = *options.encodingId;
	if (encoding.empty() || encoding.size() > 64 || encoding.find('\0') != std::string::npos || !isStrictUtf8(encoding)) return false;
	return std::all_of(encoding.begin(), encoding.end(), [](unsigned char value) {
		return (value >= 'a' && value <= 'z') || (value >= '0' && value <= '9') || value == '-';
	});
}

void EditorWorkingCopyCoordinator::AppendSaveOptionsFingerprint(std::string& fingerprint, const EditorWorkingCopySaveOptions& options)
{
	fingerprint.append("|target-policy:").append(options.targetPolicy == EEditorWorkingCopySaveTargetPolicy::ExistingOnly ? "existing" : "acquire");
	fingerprint.append(options.suppressFeedback ? "|suppress-feedback" : "|feedback");
	fingerprint.append(options.forceWrite ? "|force-write" : "|skip-clean-write");
	fingerprint.append("|suggested:").append(std::to_string(options.suggestedTarget.size())).push_back(':');
	fingerprint.append(options.suggestedTarget);
	if (options.encodingId) {
		fingerprint.append("|encoding:").append(std::to_string(options.encodingId->size())).push_back(':');
		fingerprint.append(*options.encodingId);
	} else {
		fingerprint.append("|encoding:absent");
	}
	fingerprint.append("|line-ending:").append(std::to_string(static_cast<std::uint8_t>(options.lineEnding)));
}

std::string EditorWorkingCopyCoordinator::Fingerprint(const char* kind, const EditorWorkingCopyOperationMetadata& operation,
	const std::string& inputId)
{
	return std::string(kind) + "|" + std::to_string(operation.expectedModelRevision.value_or(~std::uint64_t{ 0 }))
		+ "|" + std::to_string(inputId.size()) + ":" + inputId;
}

} // namespace workbench::editor

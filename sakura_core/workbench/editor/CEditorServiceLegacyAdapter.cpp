/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/

#include "StdAfx.h"
#include "workbench/editor/CEditorServiceLegacyAdapter.h"

namespace workbench::editor {

CEditorServiceLegacyAdapter::CEditorServiceLegacyAdapter(
	EditorCoreService& core, ILegacyEditorBackend& legacy) noexcept
	: m_core(core)
	, m_legacy(legacy)
{
}

EditorCoreSnapshot CEditorServiceLegacyAdapter::Snapshot() const
{
	return m_core.Snapshot();
}

EditorOperationResult CEditorServiceLegacyAdapter::AdoptCurrentDocument(
	EditorOperationMetadata operation, std::string inputId)
{
	std::optional<ResolvedEditorDocument> current;
	try {
		current = m_legacy.TryGetCurrentDocument();
	}
	catch (...) {
		return {
			.status = EEditorOperationStatus::Failed,
			.reason = EEditorOperationReason::LegacyBackendFailure,
			.revision = m_core.Snapshot().revision,
		};
	}
	if (!current) {
		return {
			.status = EEditorOperationStatus::NotApplicable,
			.reason = EEditorOperationReason::DocumentNotResolved,
			.revision = m_core.Snapshot().revision,
		};
	}
	return m_core.OpenResolvedInput({
		.operation = std::move(operation),
		.input = { .inputId = std::move(inputId), .documentIdentity = current->identity },
		.resolvedDocument = std::move(current),
	});
}

EditorOperationResult CEditorServiceLegacyAdapter::ReplaceInputDocumentWithCurrent(
	EditorOperationMetadata operation, std::string inputId)
{
	std::optional<ResolvedEditorDocument> current;
	try {
		current = m_legacy.TryGetCurrentDocument();
	}
	catch (...) {
		return {
			.status = EEditorOperationStatus::Failed,
			.reason = EEditorOperationReason::LegacyBackendFailure,
			.revision = m_core.Snapshot().revision,
		};
	}
	if (!current) {
		return {
			.status = EEditorOperationStatus::NotApplicable,
			.reason = EEditorOperationReason::DocumentNotResolved,
			.revision = m_core.Snapshot().revision,
		};
	}
	return m_core.ReplaceInputDocument({
		.operation = std::move(operation),
		.inputId = std::move(inputId),
		.resolvedDocument = std::move(current),
	});
}

EditorOperationResult CEditorServiceLegacyAdapter::ResolveCurrentDocument(
	const EditorOperationMetadata& operation)
{
	std::optional<ResolvedEditorDocument> current;
	try {
		current = m_legacy.TryGetCurrentDocument();
	}
	catch (...) {
		return {
			.status = EEditorOperationStatus::Failed,
			.reason = EEditorOperationReason::LegacyBackendFailure,
			.revision = m_core.Snapshot().revision,
		};
	}
	if (!current) {
		return {
			.status = EEditorOperationStatus::NotApplicable,
			.reason = EEditorOperationReason::DocumentNotResolved,
			.revision = m_core.Snapshot().revision,
		};
	}
	return m_core.ResolveDocument({ .operation = operation, .resolvedDocument = std::move(current) });
}

EditorOperationResult CEditorServiceLegacyAdapter::ReleaseDocument(const ReleaseDocumentRequest& request)
{
	return m_core.ReleaseDocument(request);
}

EditorOperationResult CEditorServiceLegacyAdapter::OpenResolvedInput(const OpenResolvedInputRequest& request)
{
	return m_core.OpenResolvedInput(request);
}

EditorOperationResult CEditorServiceLegacyAdapter::ShowInput(const ShowInputRequest& request)
{
	return m_core.ShowInput(request);
}

EditorOperationResult CEditorServiceLegacyAdapter::SetDocumentState(const SetDocumentStateRequest& request)
{
	return m_core.SetDocumentState(request);
}

EditorOperationResult CEditorServiceLegacyAdapter::CloseInput(const CloseInputRequest& request)
{
	return m_core.CloseInput(request);
}

std::unique_ptr<IEditorCoreSubscription> CEditorServiceLegacyAdapter::Subscribe(EditorCoreChangeCallback callback)
{
	return m_core.Subscribe(std::move(callback));
}

} // namespace workbench::editor

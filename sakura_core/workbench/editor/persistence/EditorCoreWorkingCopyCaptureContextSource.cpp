/*! @file
 * @brief Editor Core snapshot source for native working-copy capture metadata.
 */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"

#include "workbench/editor/persistence/EditorCoreWorkingCopyCaptureContextSource.h"

#include "workbench/editor/persistence/WorkingCopyPersistenceCodec.h"

#include <algorithm>
#include <limits>

namespace workbench::editor::persistence {
namespace {

[[nodiscard]] bool IsSameDocumentIdentity(const EditorDocumentIdentity& left,
	const EditorDocumentIdentity& right) noexcept
{
	try {
		std::wstring leftKey;
		std::wstring rightKey;
		return left.IsValid() && right.IsValid()
			&& left.TryComparisonKey(leftKey) && right.TryComparisonKey(rightKey)
			&& leftKey == rightKey;
	}
	catch (...) {
		return false;
	}
}

[[nodiscard]] std::optional<WorkingCopyPersistenceIdentity> ToPersistenceIdentity(
	const EditorDocumentIdentity& identity) noexcept
{
	try {
		WorkingCopyPersistenceIdentity result{
			.typeId = std::string(CWorkingCopyPersistenceCodec::kTextInputTypeId),
		};
		if (identity.resource) {
			const auto text = identity.resource->ToString();
			if (text.empty() || text.find(L'\0') != std::wstring::npos
				|| text.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
				return std::nullopt;
			}
			const int required = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
				text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
			if (required <= 0
				|| static_cast<std::size_t>(required) > kMaximumWorkingCopyPersistenceResourceBytes) {
				return std::nullopt;
			}
			std::string encoded(static_cast<std::size_t>(required), '\0');
			if (::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
					static_cast<int>(text.size()), encoded.data(), required, nullptr, nullptr) != required) {
				return std::nullopt;
			}
			result.canonicalResource = std::move(encoded);
		}
		else if (identity.opaqueId
			&& IsValidWorkingCopyPersistenceUtf8(*identity.opaqueId, false,
				kMaximumWorkingCopyPersistenceIdBytes)) {
			result.opaqueId = *identity.opaqueId;
		}
		else {
			return std::nullopt;
		}
		return result.IsValid() ? std::optional{ std::move(result) } : std::nullopt;
	}
	catch (...) {
		return std::nullopt;
	}
}

} // namespace

EditorCoreWorkingCopyCaptureContextSource::EditorCoreWorkingCopyCaptureContextSource(
	const EditorCoreService& editorCore) noexcept
	: m_editorCore(editorCore)
{
}

std::optional<CEditDocWorkingCopyCaptureContext>
EditorCoreWorkingCopyCaptureContextSource::CurrentCaptureContext() const
{
	try {
		const auto snapshot = m_editorCore.Snapshot();
		if (!snapshot.group.activeInputId
			|| !IsValidWorkingCopyPersistenceId(*snapshot.group.activeInputId)) {
			return std::nullopt;
		}

		const auto input = std::find_if(snapshot.group.inputs.begin(), snapshot.group.inputs.end(),
			[&](const EditorInputSnapshot& candidate) {
				return candidate.descriptor.inputId == *snapshot.group.activeInputId;
			});
		if (input == snapshot.group.inputs.end() || !input->descriptor.IsValid()
			|| input->documentKey.empty()) {
			return std::nullopt;
		}

		const auto document = std::find_if(snapshot.documents.begin(), snapshot.documents.end(),
			[&](const EditorDocumentSnapshot& candidate) {
				return candidate.documentKey == input->documentKey;
			});
		if (document == snapshot.documents.end()
			|| document->documentRevision == 0
			|| document->documentRevision > kMaximumWorkingCopyPersistenceGeneration
			|| !IsSameDocumentIdentity(input->descriptor.documentIdentity, document->identity)) {
			return std::nullopt;
		}

		return CEditDocWorkingCopyCaptureContext{
			.inputId = input->descriptor.inputId,
			.inputTypeId = std::string(CWorkingCopyPersistenceCodec::kTextInputTypeId),
			.documentIdentity = input->descriptor.documentIdentity,
			.documentRevision = document->documentRevision,
		};
	}
	catch (...) {
		return std::nullopt;
	}
}

std::optional<EditorWorkingCopyCurrentChange>
EditorCoreWorkingCopyCaptureContextSource::CurrentChange() const
{
	const auto context = CurrentCaptureContext();
	if (!context) return std::nullopt;
	const auto identity = ToPersistenceIdentity(context->documentIdentity);
	if (!identity) return std::nullopt;
	return EditorWorkingCopyCurrentChange{
		.identity = *identity,
		.contentVersion = context->documentRevision,
	};
}

} // namespace workbench::editor::persistence

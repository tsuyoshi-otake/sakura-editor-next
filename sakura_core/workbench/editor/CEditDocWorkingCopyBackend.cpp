/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/

#include "StdAfx.h"
#include "workbench/editor/CEditDocWorkingCopyBackend.h"

#include "CEditApp.h"
#include "basis/CEol.h"
#include "charset/charset.h"
#include "doc/CEditDoc.h"
#include <limits>

namespace workbench::editor {

namespace {

class SoundMuteGuard final {
public:
	explicit SoundMuteGuard(bool active) noexcept
		: m_active(active)
	{
		if (m_active) CEditApp::getInstance()->m_cSoundSet.MuteOn();
	}
	~SoundMuteGuard()
	{
		if (m_active) CEditApp::getInstance()->m_cSoundSet.MuteOff();
	}
	SoundMuteGuard(const SoundMuteGuard&) = delete;
	SoundMuteGuard& operator=(const SoundMuteGuard&) = delete;

private:
	bool m_active = false;
};

} // namespace

CEditDocWorkingCopyBackend::CEditDocWorkingCopyBackend(CEditDoc& document) noexcept
	: m_document(document)
{
}

EditorWorkingCopyBackendResult CEditDocWorkingCopyBackend::Save(const EditorWorkingCopyBackendRequest& request)
{
	if (!request.identity.IsValid()) {
		return { .status = EEditorWorkingCopyBackendStatus::Failed };
	}

	try {
		SoundMuteGuard feedback(request.saveOptions.suppressFeedback);
		if (m_document.m_cDocFileOperation.FileSave()) {
			auto resultingIdentity = CurrentFileIdentity();
			if (!resultingIdentity) return { .status = EEditorWorkingCopyBackendStatus::Failed };
			return {
				.status = EEditorWorkingCopyBackendStatus::Succeeded,
				.successfulVersion = request.version,
				.resultingIdentity = std::move(resultingIdentity),
			};
		}
		return FromLegacyResult(m_document.m_cDocFileOperation.GetLastSaveResult(), request.version);
	}
	catch (...) {
		return { .status = EEditorWorkingCopyBackendStatus::Failed };
	}
}

EditorWorkingCopyBackendResult CEditDocWorkingCopyBackend::SaveAs(const EditorWorkingCopySaveAsBackendRequest& request)
{
	if (!request.source.identity.IsValid()) {
		return { .status = EEditorWorkingCopyBackendStatus::Failed };
	}

	try {
		const auto encoding = ToLegacyEncoding(request.source.saveOptions.encodingId);
		if (!encoding) return { .status = EEditorWorkingCopyBackendStatus::Failed };
		const auto lineEnding = ToLegacyLineEnding(request.source.saveOptions.lineEnding);
		bool saved = false;
		SoundMuteGuard feedback(request.source.saveOptions.suppressFeedback);
		if (request.targetIdentity) {
			const auto& target = *request.targetIdentity;
			if (!target.IsValid() || !target.resource || target.opaqueId) {
				return { .status = EEditorWorkingCopyBackendStatus::Failed };
			}
			auto targetPath = target.resource->ToWindowsPath();
			if (!targetPath) return { .status = EEditorWorkingCopyBackendStatus::Failed };
			saved = m_document.m_cDocFileOperation.FileSaveAs(
				targetPath.value->c_str(), *encoding, lineEnding, false);
		}
		else {
			std::wstring suggestedTarget;
			if (!request.source.saveOptions.suggestedTarget.empty()
				&& !Utf8ToWideStrict(request.source.saveOptions.suggestedTarget, suggestedTarget)) {
				return { .status = EEditorWorkingCopyBackendStatus::Failed };
			}
			saved = m_document.m_cDocFileOperation.FileSaveAs(
				suggestedTarget.empty() ? nullptr : suggestedTarget.c_str(), *encoding, lineEnding, true);
		}

		if (!saved) {
			return FromLegacyResult(m_document.m_cDocFileOperation.GetLastSaveResult(), request.source.version);
		}

		auto resultingIdentity = CurrentFileIdentity();
		if (!resultingIdentity) return { .status = EEditorWorkingCopyBackendStatus::Failed };
		return {
			.status = EEditorWorkingCopyBackendStatus::Succeeded,
			.successfulVersion = request.source.version,
			.resultingIdentity = std::move(resultingIdentity),
		};
	}
	catch (...) {
		return { .status = EEditorWorkingCopyBackendStatus::Failed };
	}
}

EditorWorkingCopyBackendRevertPrepareResult CEditDocWorkingCopyBackend::PrepareRevert(const EditorWorkingCopyBackendRequest& request)
{
	(void)request;
	// Do not delegate to CReadManager/FileOpen here: both legacy paths mutate the live document and/or run callbacks.
	// The production bridge remains safely unavailable until CEditWnd owns the complete staged swap, exact rollback,
	// and accepted-state layout/view/caret/caption projection as one transaction.
	return { .result = { .status = EEditorWorkingCopyBackendStatus::Unsupported } };
}

EEditorWorkingCopyBackendRevertApplyStatus CEditDocWorkingCopyBackend::ApplyPreparedRevert(
	EditorWorkingCopyRevertTransaction transaction) noexcept
{
	(void)transaction;
	return EEditorWorkingCopyBackendRevertApplyStatus::NotPrepared;
}

void CEditDocWorkingCopyBackend::FinalizePreparedRevert(EditorWorkingCopyRevertTransaction transaction) noexcept
{
	(void)transaction;
}

void CEditDocWorkingCopyBackend::RollbackPreparedRevert(EditorWorkingCopyRevertTransaction transaction) noexcept
{
	(void)transaction;
}

EditorWorkingCopyBackendResult CEditDocWorkingCopyBackend::PrepareClose(const EditorWorkingCopyBackendRequest& request)
{
	if (!request.identity.IsValid()) {
		return { .status = EEditorWorkingCopyBackendStatus::Failed };
	}

	try {
		if (m_document.m_cDocFileOperation.PrepareFileClose(request.suppressCloseConfirmation)) {
			return { .status = EEditorWorkingCopyBackendStatus::Succeeded, .successfulVersion = request.version };
		}
		return FromLegacyResult(m_document.m_cDocFileOperation.GetLastCloseResult(), request.version);
	}
	catch (...) {
		return { .status = EEditorWorkingCopyBackendStatus::Failed };
	}
}

void CEditDocWorkingCopyBackend::CommitClose(const EditorWorkingCopyBackendRequest& request) noexcept
{
	// CDocFileOperation contains the best-effort per-step isolation. Keep a final guard here because
	// the IEditorWorkingCopyBackend contract guarantees no exception after the core close commits.
	try {
		m_document.m_cDocFileOperation.CommitFileClose(
			request.closeDisposition == EEditorWorkingCopyCloseDisposition::InitializeEmptyDocument);
	}
	catch (...) {
	}
}

std::optional<EditorDocumentIdentity> CEditDocWorkingCopyBackend::CurrentFileIdentity() const
{
	const auto& file = m_document.m_cDocFile;
	if (!file.GetFilePathClass().IsValidPath()) return std::nullopt;
	auto uri = platform::uri::Uri::FromWindowsPath(file.GetFilePath());
	if (!uri) return std::nullopt;
	return EditorDocumentIdentity{ .resource = std::move(*uri.value) };
}

EditorWorkingCopyBackendResult CEditDocWorkingCopyBackend::FromLegacyResult(EDocFileOperationResult result,
	std::uint64_t successfulVersion) noexcept
{
	switch (result) {
	case EDocFileOperationResult::Succeeded:
		return { .status = EEditorWorkingCopyBackendStatus::Succeeded, .successfulVersion = successfulVersion };
	case EDocFileOperationResult::Cancelled:
		return { .status = EEditorWorkingCopyBackendStatus::Cancelled };
	case EDocFileOperationResult::Failed:
	default:
		return { .status = EEditorWorkingCopyBackendStatus::Failed };
	}
}

std::optional<ECodeType> CEditDocWorkingCopyBackend::ToLegacyEncoding(const std::optional<std::string>& encodingId) noexcept
{
	if (!encodingId) return CODE_NONE;
	if (*encodingId == "shift_jis") return CODE_SJIS;
	if (*encodingId == "iso-2022-jp") return CODE_JIS;
	if (*encodingId == "euc-jp") return CODE_EUC;
	if (*encodingId == "utf-16le") return CODE_UTF16LE;
	if (*encodingId == "utf-16be") return CODE_UTF16BE;
	if (*encodingId == "utf-8") return CODE_UTF8;
	if (*encodingId == "utf-7") return CODE_UTF7;
	if (*encodingId == "cesu-8") return CODE_CESU8;
	if (*encodingId == "windows-1252") return CODE_LATIN1;
	return std::nullopt;
}

EEolType CEditDocWorkingCopyBackend::ToLegacyLineEnding(EEditorWorkingCopyLineEnding lineEnding) noexcept
{
	switch (lineEnding) {
	case EEditorWorkingCopyLineEnding::CrLf: return EEolType::cr_and_lf;
	case EEditorWorkingCopyLineEnding::Lf: return EEolType::line_feed;
	case EEditorWorkingCopyLineEnding::Cr: return EEolType::carriage_return;
	case EEditorWorkingCopyLineEnding::Preserve:
	default: return EEolType::none;
	}
}

bool CEditDocWorkingCopyBackend::Utf8ToWideStrict(const std::string& value, std::wstring& converted) noexcept
{
	if (value.empty() || value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) return false;
	const auto length = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
	if (length <= 0) return false;
	try {
		converted.resize(static_cast<std::size_t>(length));
	} catch (...) {
		return false;
	}
	return ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
		converted.data(), length) == length;
}

} // namespace workbench::editor

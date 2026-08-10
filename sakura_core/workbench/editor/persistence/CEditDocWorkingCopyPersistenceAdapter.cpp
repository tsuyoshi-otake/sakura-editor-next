/*! @file
 * @brief Native CEditDoc snapshot and non-destructive Hot Exit recovery adapter.
 */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/

#include "StdAfx.h"

#include "workbench/editor/persistence/CEditDocWorkingCopyPersistenceAdapter.h"

#include "doc/CEditDoc.h"
#include "doc/logic/CDocLine.h"
#include <sakura/editor/document/DocumentSession.h>
#include <sakura/uri/UriIdentity.h>
#include "workbench/editor/persistence/WorkingCopyPersistenceCodec.h"

#include <limits>
#include <memory>
#include <string_view>
#include <utility>

namespace workbench::editor::persistence {
namespace {

using workbench::editor::EditorDocumentIdentity;

struct NativeTextMetadata final {
	ECodeType encoding = CODE_ERROR;
	bool bom = false;
	EEolType eol = EEolType::none;
};

[[nodiscard]] std::optional<EWorkingCopyTextEncoding> ToPersistenceEncoding(
	ECodeType encoding, bool bom) noexcept
{
	switch (encoding) {
	case CODE_UTF8:
		return bom ? EWorkingCopyTextEncoding::Utf8WithBom : EWorkingCopyTextEncoding::Utf8;
	case CODE_UNICODE:
		// The V1 persistence DTO has no independent BOM bit.  Do not silently
		// change a BOM-less UTF-16 document into a BOM-bearing document.
		return bom ? std::optional(EWorkingCopyTextEncoding::Utf16Le) : std::nullopt;
	case CODE_UNICODEBE:
		return bom ? std::optional(EWorkingCopyTextEncoding::Utf16Be) : std::nullopt;
	case CODE_LATIN1:
		return !bom ? std::optional(EWorkingCopyTextEncoding::Windows1252) : std::nullopt;
	default:
		return std::nullopt;
	}
}

[[nodiscard]] std::optional<NativeTextMetadata> ToNativeMetadata(
	EWorkingCopyTextEncoding encoding, EWorkingCopyEol eol) noexcept
{
	NativeTextMetadata result;
	switch (encoding) {
	case EWorkingCopyTextEncoding::Utf8:
		result.encoding = CODE_UTF8;
		result.bom = false;
		break;
	case EWorkingCopyTextEncoding::Utf8WithBom:
		result.encoding = CODE_UTF8;
		result.bom = true;
		break;
	case EWorkingCopyTextEncoding::Utf16Le:
		result.encoding = CODE_UNICODE;
		result.bom = true;
		break;
	case EWorkingCopyTextEncoding::Utf16Be:
		result.encoding = CODE_UNICODEBE;
		result.bom = true;
		break;
	case EWorkingCopyTextEncoding::Windows1252:
		result.encoding = CODE_LATIN1;
		result.bom = false;
		break;
	case EWorkingCopyTextEncoding::Unknown:
	default:
		return std::nullopt;
	}

	switch (eol) {
	case EWorkingCopyEol::Lf:
		result.eol = EEolType::line_feed;
		break;
	case EWorkingCopyEol::CrLf:
		result.eol = EEolType::cr_and_lf;
		break;
	case EWorkingCopyEol::Cr:
		result.eol = EEolType::carriage_return;
		break;
	case EWorkingCopyEol::Unknown:
	default:
		return std::nullopt;
	}
	return result;
}

[[nodiscard]] std::optional<EWorkingCopyEol> ToPersistenceEol(EEolType eol) noexcept
{
	switch (eol) {
	case EEolType::line_feed:
		return EWorkingCopyEol::Lf;
	case EEolType::cr_and_lf:
		return EWorkingCopyEol::CrLf;
	case EEolType::carriage_return:
		return EWorkingCopyEol::Cr;
	default:
		return std::nullopt;
	}
}

[[nodiscard]] bool EncodeUtf8(std::wstring_view value, std::string& output) noexcept
{
	output.clear();
	if (value.empty()) return true;
	if (value.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) return false;
	if (value.find(L'\0') != std::wstring_view::npos) return false;

	const int required = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
		static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
	if (required <= 0
		|| static_cast<std::size_t>(required) > kMaximumWorkingCopyPersistenceContentBytes) {
		return false;
	}
	try {
		output.resize(static_cast<std::size_t>(required));
	}
	catch (const std::exception&) {
		return false;
	}
	return ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
		static_cast<int>(value.size()), output.data(), required, nullptr, nullptr) == required;
}

[[nodiscard]] bool DecodeUtf8(std::string_view value, std::wstring& output) noexcept
{
	output.clear();
	if (!IsValidWorkingCopyPersistenceUtf8(
			value, true, kMaximumWorkingCopyPersistenceContentBytes)) {
		return false;
	}
	if (value.empty()) return true;
	if (value.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) return false;

	const int required = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
		static_cast<int>(value.size()), nullptr, 0);
	if (required <= 0) return false;
	try {
		output.resize(static_cast<std::size_t>(required));
	}
	catch (const std::exception&) {
		return false;
	}
	return ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
		static_cast<int>(value.size()), output.data(), required) == required;
}

//! Projects legacy logical text into the Editor Core document aggregate for the
//! working-copy capture read path.  This is intentionally one-way: the legacy
//! line model remains the only mutable owner until an edit transaction can
//! preserve its layout, selection, and native undo invariants.
//!
//! A failed core import maps to Capture()'s established Failed terminal; the
//! caller therefore publishes no partial snapshot and leaves CEditDoc untouched.
[[nodiscard]] bool CaptureNativeText(const CEditDoc& document,
	::editor::document::DocumentSession& session) noexcept
{
	try {
		std::wstring text;
		for (const CDocLine* line = document.m_cDocLineMgr.GetDocLineTop();
			line != nullptr; line = line->GetNextLine()) {
			const auto lineLength = static_cast<int>(line->GetLengthWithEOL());
			if (lineLength < 0
				|| static_cast<std::size_t>(lineLength)
					> kMaximumWorkingCopyPersistenceContentBytes
				|| text.size() > kMaximumWorkingCopyPersistenceContentBytes
					- static_cast<std::size_t>(lineLength)) {
				return false;
			}
			text.append(line->GetPtr(), static_cast<std::size_t>(lineLength));
		}
		std::string content;
		if (!EncodeUtf8(text, content)) return false;

		const auto imported = session.Replace({ 0, 0 }, content);
		return imported.Terminal() == ::editor::document::EDocumentSessionTerminal::Succeeded
			|| imported.Terminal() == ::editor::document::EDocumentSessionTerminal::NoChange;
	}
	catch (const std::exception&) {
		return false;
	}
}

[[nodiscard]] bool EncodeUri(const platform::uri::Uri& uri, std::string& encoded) noexcept
{
	try {
		const auto text = uri.ToString();
		if (text.empty() || text.find(L'\0') != std::wstring::npos
			|| text.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
			return false;
		}
		const int required = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
			static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
		if (required <= 0
			|| static_cast<std::size_t>(required) > kMaximumWorkingCopyPersistenceResourceBytes) {
			return false;
		}
		encoded.resize(static_cast<std::size_t>(required));
		return ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
			static_cast<int>(text.size()), encoded.data(), required, nullptr, nullptr) == required;
	}
	catch (const std::exception&) {
		return false;
	}
}

[[nodiscard]] std::optional<platform::uri::Uri> DecodeResourceUri(
	std::string_view encoded, std::wstring& filePath) noexcept
{
	try {
		if (!IsValidWorkingCopyPersistenceUtf8(
				encoded, false, kMaximumWorkingCopyPersistenceResourceBytes)
			|| encoded.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
			return std::nullopt;
		}
		const int required = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, encoded.data(),
			static_cast<int>(encoded.size()), nullptr, 0);
		if (required <= 0) return std::nullopt;
		std::wstring decoded(static_cast<std::size_t>(required), L'\0');
		if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, encoded.data(),
				static_cast<int>(encoded.size()), decoded.data(), required) != required) {
			return std::nullopt;
		}
		auto parsed = platform::uri::Uri::Parse(decoded);
		if (!parsed || parsed.value->Query() || parsed.value->Fragment()) return std::nullopt;
		auto path = parsed.value->ToWindowsPath();
		if (!path || path.value->empty()
			|| path.value->size() >= static_cast<std::size_t>(CFilePath::BUFFER_COUNT)) {
			return std::nullopt;
		}
		filePath = std::move(*path.value);
		// Only accept the exact file-URI form Capture emits.  Accepting aliases
		// such as file://localhost here would make the next backup use a
		// different durable key even though the native path is unchanged.
		auto canonical = platform::uri::Uri::FromWindowsPath(filePath);
		std::string canonicalText;
		if (!canonical || !EncodeUri(*canonical.value, canonicalText)
			|| canonicalText != encoded) {
			return std::nullopt;
		}
		return std::move(*canonical.value);
	}
	catch (const std::exception&) {
		return std::nullopt;
	}
}

[[nodiscard]] bool IsSameRequest(
	const EditorWorkingCopyRecoveryRequest& left,
	const EditorWorkingCopyRecoveryRequest& right) noexcept
{
	return left.input == right.input
		&& left.backup.scope == right.backup.scope
		&& left.backup.identity == right.backup.identity
		&& left.backup.generation == right.backup.generation
		&& left.backup.contentVersion == right.backup.contentVersion
		&& left.backup.checksum == right.backup.checksum
		&& left.backup.encoding == right.backup.encoding
		&& left.backup.eol == right.backup.eol
		&& left.backup.dirty == right.backup.dirty
		&& left.backup.content == right.backup.content;
}

[[nodiscard]] bool AppendDecodedLines(CDocLineMgr& lines, std::wstring_view content)
{
	CDocEditAgent editor(&lines);
	const bool extendedEol = GetDllShareData().m_Common.m_sEdit.m_bEnableExtEol;
	std::size_t lineStart = 0;
	std::size_t index = 0;
	while (index < content.size()) {
		std::size_t delimiterLength = 0;
		const wchar_t character = content[index];
		if (character == WCODE::CR) {
			delimiterLength = index + 1 < content.size() && content[index + 1] == WCODE::LF ? 2 : 1;
		} else if (character == WCODE::LF
			|| (extendedEol && (character == WCODE::NEL
				|| character == WCODE::LS_ || character == WCODE::PS_))) {
			delimiterLength = 1;
		}
		if (delimiterLength == 0) {
			++index;
			continue;
		}

		const std::size_t lineLength = index + delimiterLength - lineStart;
		if (lineLength > static_cast<std::size_t>((std::numeric_limits<int>::max)())) return false;
		editor.AddLineStrX(content.data() + lineStart, static_cast<int>(lineLength));
		index += delimiterLength;
		lineStart = index;
	}
	if (lineStart < content.size()) {
		const std::size_t lineLength = content.size() - lineStart;
		if (lineLength > static_cast<std::size_t>((std::numeric_limits<int>::max)())) return false;
		editor.AddLineStrX(content.data() + lineStart, static_cast<int>(lineLength));
	}
	return true;
}

} // namespace

struct CEditDocWorkingCopyPersistenceAdapter::PreparedDocument final {
	EditorWorkingCopyRecoveryRequest request;
	EditorDocumentIdentity coreIdentity;
	CDocLineMgr lines;
	std::wstring filePath;
	NativeTextMetadata metadata;

	PreparedDocument(EditorWorkingCopyRecoveryRequest recoveryRequest,
		EditorDocumentIdentity identity, std::shared_ptr<std::pmr::memory_resource> memoryResource)
		: request(std::move(recoveryRequest))
		, coreIdentity(std::move(identity))
	{
		// AppendAsMove transfers line nodes without changing their allocator.
		// Sharing the target resource before creating any node is therefore mandatory.
		lines.SetMemoryResource(std::move(memoryResource));
	}
};

CEditDocWorkingCopyPersistenceAdapter::CEditDocWorkingCopyPersistenceAdapter(
	CEditDoc& document, const ICEditDocWorkingCopyCaptureContextSource& contextSource) noexcept
	: m_document(document)
	, m_contextSource(contextSource)
{
}

CEditDocWorkingCopyPersistenceAdapter::~CEditDocWorkingCopyPersistenceAdapter() = default;

EditorWorkingCopySnapshotResult CEditDocWorkingCopyPersistenceAdapter::Capture()
{
	try {
		const auto context = m_contextSource.CurrentCaptureContext();
		if (!context) {
			return { .status = EEditorWorkingCopySnapshotStatus::NoInput };
		}
		if (!IsValidWorkingCopyPersistenceId(context->inputId)
			|| context->inputTypeId != CWorkingCopyPersistenceCodec::kTextInputTypeId
			|| !context->documentIdentity.IsValid() || context->documentRevision == 0) {
			return { .status = EEditorWorkingCopySnapshotStatus::Failed };
		}

		const auto encoding = ToPersistenceEncoding(
			m_document.GetDocumentEncoding(), m_document.GetDocumentBomExist());
		const auto eol = ToPersistenceEol(m_document.m_cDocEditor.GetNewLineCode().GetType());
		if (!encoding || !eol) {
			return { .status = EEditorWorkingCopySnapshotStatus::Unsupported };
		}

		WorkingCopyPersistenceIdentity identity{
			.typeId = std::string(CWorkingCopyPersistenceCodec::kTextInputTypeId),
		};
		if (context->documentIdentity.resource) {
			std::string encoded;
			if (!EncodeUri(*context->documentIdentity.resource, encoded)) {
				return { .status = EEditorWorkingCopySnapshotStatus::Failed };
			}
			identity.canonicalResource = std::move(encoded);
		} else {
			if (!context->documentIdentity.opaqueId
				|| !IsValidWorkingCopyPersistenceId(*context->documentIdentity.opaqueId)) {
				return { .status = EEditorWorkingCopySnapshotStatus::Failed };
			}
			identity.opaqueId = *context->documentIdentity.opaqueId;
		}
		if (!identity.IsValid()) {
			return { .status = EEditorWorkingCopySnapshotStatus::Failed };
		}

		::editor::document::DocumentSession coreDocument;
		if (!CaptureNativeText(m_document, coreDocument)) {
			return { .status = EEditorWorkingCopySnapshotStatus::Failed };
		}
		EditorWorkingCopyPersistenceSnapshot snapshot{
			.identity = std::move(identity),
			.inputId = context->inputId,
			.inputTypeId = context->inputTypeId,
			.contentVersion = context->documentRevision,
			.dirty = m_document.m_cDocEditor.IsModified(),
			.encoding = *encoding,
			.eol = *eol,
			.content = coreDocument.Document().Text(),
		};
		if (!snapshot.IsValid()) {
			return { .status = EEditorWorkingCopySnapshotStatus::Failed };
		}
		return {
			.status = EEditorWorkingCopySnapshotStatus::Captured,
			.snapshot = std::move(snapshot),
		};
	}
	catch (const std::exception&) {
		return { .status = EEditorWorkingCopySnapshotStatus::Failed };
	}
}

EditorWorkingCopyRecoveryPrepareResult CEditDocWorkingCopyPersistenceAdapter::Prepare(
	const EditorWorkingCopyRecoveryRequest& request)
{
	m_prepared.reset();
	try {
		if (!request.input.IsValid() || !request.backup.IsValid()
			|| request.input.inputTypeId != CWorkingCopyPersistenceCodec::kTextInputTypeId
			|| request.input.workingCopyIdentity != request.backup.identity
			|| !request.input.backupGeneration
			|| *request.input.backupGeneration != request.backup.generation
			|| !request.backup.dirty
			|| request.backup.checksum
				!= CWorkingCopyPersistenceCodec::ComputeContentChecksum(request.backup.content)) {
			return { .status = EEditorWorkingCopyRecoveryStatus::Failed };
		}
		if (!IsInertTarget()) {
			return { .status = EEditorWorkingCopyRecoveryStatus::Failed };
		}
		const auto metadata = ToNativeMetadata(request.backup.encoding, request.backup.eol);
		if (!metadata) {
			return { .status = EEditorWorkingCopyRecoveryStatus::Unsupported };
		}

		EditorDocumentIdentity coreIdentity;
		std::wstring filePath;
		if (request.backup.identity.canonicalResource) {
			auto uri = DecodeResourceUri(*request.backup.identity.canonicalResource, filePath);
			if (!uri) {
				return { .status = EEditorWorkingCopyRecoveryStatus::Unsupported };
			}
			coreIdentity.resource = std::move(*uri);
		} else if (request.backup.identity.opaqueId) {
			if (!IsValidEditorExternalId(
					*request.backup.identity.opaqueId, kMaxEditorOpaqueDocumentIdLength)) {
				return { .status = EEditorWorkingCopyRecoveryStatus::Failed };
			}
			coreIdentity.opaqueId = *request.backup.identity.opaqueId;
		} else {
			return { .status = EEditorWorkingCopyRecoveryStatus::Failed };
		}
		if (!coreIdentity.IsValid()) {
			return { .status = EEditorWorkingCopyRecoveryStatus::Failed };
		}

		std::wstring decoded;
		if (!DecodeUtf8(request.backup.content, decoded)) {
			return { .status = EEditorWorkingCopyRecoveryStatus::Failed };
		}
		auto prepared = std::make_unique<PreparedDocument>(
			request, coreIdentity, m_document.m_cDocLineMgr.GetMemoryResource());
		prepared->filePath = std::move(filePath);
		prepared->metadata = *metadata;
		if (!AppendDecodedLines(prepared->lines, decoded)) {
			return { .status = EEditorWorkingCopyRecoveryStatus::Failed };
		}

		auto resultIdentity = prepared->coreIdentity;
		m_prepared = std::move(prepared);
		return {
			.status = EEditorWorkingCopyRecoveryStatus::Prepared,
			.coreIdentity = std::move(resultIdentity),
		};
	}
	catch (const std::exception&) {
		m_prepared.reset();
		return { .status = EEditorWorkingCopyRecoveryStatus::Failed };
	}
}

void CEditDocWorkingCopyPersistenceAdapter::AbortPrepared() noexcept
{
	m_prepared.reset();
}

EEditorWorkingCopyRecoveryCommitStatus CEditDocWorkingCopyPersistenceAdapter::Commit(
	const EditorWorkingCopyRecoveryRequest& request) noexcept
{
	if (!m_prepared) return EEditorWorkingCopyRecoveryCommitStatus::NotPrepared;
	if (!IsInertTarget() || !IsSameRequest(m_prepared->request, request)
		|| m_document.m_cDocLineMgr.GetMemoryResource() != m_prepared->lines.GetMemoryResource()) {
		m_prepared.reset();
		return EEditorWorkingCopyRecoveryCommitStatus::TargetChanged;
	}

	// Every operation below is fixed-capacity or ownership transfer.  Prepare
	// bounded the path before CFilePath::Assign, created every line with the
	// target allocator, and verified that the target has no data to destroy.
	m_document.m_cDocFile.SetFilePath(m_prepared->filePath.c_str());
	m_document.m_cDocFile.ClearFileTime();
	m_document.m_cDocFile.SetCodeSet(m_prepared->metadata.encoding, m_prepared->metadata.bom);
	m_document.m_cDocEditor.SetNewLineCode(m_prepared->metadata.eol);
	m_document.m_cDocLineMgr.AppendAsMove(m_prepared->lines);
	m_document.m_cDocEditor.SetModified(true, false);
	m_prepared.reset();
	return EEditorWorkingCopyRecoveryCommitStatus::Committed;
}

bool CEditDocWorkingCopyPersistenceAdapter::IsInertTarget() const noexcept
{
	return m_document.m_cDocLineMgr.GetLineCount() == CLogicInt(0)
		&& !m_document.m_cDocFile.GetFilePathClass().IsValidPath()
		&& !m_document.m_cDocFile.IsFileLocking()
		&& m_document.m_cDocFile.IsFileTimeZero()
		&& !m_document.m_cDocEditor.IsModified()
		&& !m_document.m_cDocEditor.IsEnableUndo()
		&& !m_document.m_cDocEditor.IsEnableRedo();
}

} // namespace workbench::editor::persistence

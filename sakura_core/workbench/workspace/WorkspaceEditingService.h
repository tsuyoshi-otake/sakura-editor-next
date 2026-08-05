/*! @file
 * @brief Bounded, UI-independent edits of VS Code .code-workspace documents.
 */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 * SPDX-License-Identifier: Zlib
 */
#pragma once

#include "config/editing/CJsoncConfigurationEditor.h"
#include <sakura/filesystem/IFileService.h>
#include <sakura/uri/UriIdentity.h>
#include "workbench/workspace/WorkspaceConfigurationDocumentParser.h"
#include "workbench/workspace/WorkspaceFolderLimits.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace workbench::workspace {

enum class EWorkspaceEditingOutcome : std::uint8_t {
	Succeeded,
	Cancelled,
	Failed,
};

struct WorkspaceFolderEdit final {
	platform::uri::Uri uri;
	//! An omitted name stays omitted.  In particular, do not manufacture a
	//! display name while copying a workspace document.
	std::optional<std::wstring> label;
};

struct WorkspaceFoldersEditRequest final {
	platform::uri::Uri source;
	platform::uri::Uri target;
	std::vector<WorkspaceFolderEdit> folders;
};

struct WorkspaceEditingResult final {
	EWorkspaceEditingOutcome outcome = EWorkspaceEditingOutcome::Failed;
	std::optional<platform::filesystem::EFileResultStatus> fileStatus;
	std::optional<platform::serialization::EJsoncDiagnosticCode> jsoncDiagnostic;
	//! Present only when Succeeded identifies the exact bytes and provider
	//! version accepted by the CAS. Runtime callers use this pair to apply the
	//! semantic workspace synchronously without racing an advisory file watch.
	std::optional<platform::filesystem::FileVersionToken> committedVersion;
	std::optional<std::string> committedDocument;
	std::string diagnostic;
};

class IWorkspaceEditingService {
public:
	virtual ~IWorkspaceEditingService() = default;
	virtual WorkspaceEditingResult ReplaceFolders(const WorkspaceFoldersEditRequest& request) = 0;
};

/*! A one-read/one-CAS editor. It delegates all JSONC lexical handling and
 * UTF-8 validation to CJsoncConfigurationEditor/CJsoncDocument.  The only
 * bytes it changes in an existing source document are the folders value. */
class CWorkspaceEditingService final : public IWorkspaceEditingService {
public:
	explicit CWorkspaceEditingService(platform::filesystem::IFileService& files) noexcept : m_files(files) {}

	WorkspaceEditingResult ReplaceFolders(const WorkspaceFoldersEditRequest& request) override
	{
		if (!IsWorkspaceResource(request.source) || !IsWorkspaceResource(request.target)) {
			return Failed("workspace resource is invalid");
		}
		bool invalidFolder = false;
		auto folders = NormalizeFolders(request.folders, invalidFolder);
		if (invalidFolder) return Failed("workspace folder URI or label is invalid");
		if (folders.size() > kMaximumWorkspaceFolders) return Failed("workspace folder limit exceeded");

		const bool writesSource = platform::uri::UriIdentityService::IsEqual(request.source, request.target);
		const auto source = m_files.ReadVersioned(request.source, { kMaximumInputBytes });
		const bool sourceMissing = source.status == platform::filesystem::EFileResultStatus::NotFound;
		if ((!source.Succeeded() || !source.value) && !(sourceMissing && writesSource)) {
			return Failed(source.status, "workspace source read failed");
		}
		if (!sourceMissing && source.value->bytes.size() > kMaximumInputBytes) {
			return Failed("workspace source exceeds the byte limit");
		}

		const std::string sourceDocument = sourceMissing ? "{}" : ToText(source.value->bytes);
		const auto sourceMetadata = ReadSourceFolderMetadata(sourceDocument, request.source);
		const auto replacement = RenderFolders(request.target, folders, sourceMetadata, DetectLineEnding(sourceDocument));
		if (!replacement) return Failed("workspace folder serialization is invalid");
		const auto planned = config::editing::CJsoncConfigurationEditor::ReplaceTopLevelObjectMemberWithDiagnostics(
			sourceDocument, "folders", *replacement);
		if (!planned.Succeeded()) return Failed(planned.jsoncDiagnostic, "workspace JSONC document is malformed or cannot preserve trivia");
		if (planned.document->size() > kMaximumOutputBytes) return Failed("workspace output exceeds the byte limit");
		if (writesSource && !sourceMissing && *planned.document == sourceDocument) {
			return Succeeded(source.value->version, sourceDocument, "workspace folders already match");
		}

		WorkspaceEditingResult expectationFailure;
		const auto expectation = TargetExpectation(request.target, writesSource, sourceMissing, source, expectationFailure);
		if (!expectation) return expectationFailure;
		const auto published = m_files.ConditionalAtomicReplace(request.target, ToBytes(*planned.document), *expectation);
		switch (published.status) {
		case platform::filesystem::EFileConditionalReplaceStatus::Succeeded:
			if (!published.committedVersion) return Failed("workspace publish did not identify its committed version");
			return Succeeded(*published.committedVersion, *planned.document, "workspace folders updated");
		case platform::filesystem::EFileConditionalReplaceStatus::Conflict:
			return Failed("workspace changed before publish");
		case platform::filesystem::EFileConditionalReplaceStatus::Unsupported:
			return Failed("workspace conditional replace is unsupported");
		case platform::filesystem::EFileConditionalReplaceStatus::Failed:
			return Failed("workspace conditional replace failed");
		}
		return Failed("workspace conditional replace failed");
	}

private:
	using FileReadResult = platform::filesystem::FileResult<platform::filesystem::FileContentSnapshot>;
	using ReplaceOptions = platform::filesystem::FileConditionalReplaceOptions;
	enum class ESourceFolderLocation : std::uint8_t { RelativePath, AbsolutePath, Uri };
	struct SourceFolderMetadata final {
		std::wstring identity;
		ESourceFolderLocation location = ESourceFolderLocation::Uri;
		std::optional<std::wstring> label;
	};
	static constexpr std::size_t kMaximumInputBytes = config::editing::CJsoncConfigurationEditor::kMaximumInputBytes;
	static constexpr std::size_t kMaximumOutputBytes = config::editing::CJsoncConfigurationEditor::kMaximumOutputBytes;
	platform::filesystem::IFileService& m_files;

	static WorkspaceEditingResult Failed(std::string diagnostic)
	{
		return { EWorkspaceEditingOutcome::Failed, std::nullopt, std::nullopt,
			std::nullopt, std::nullopt, std::move(diagnostic) };
	}
	static WorkspaceEditingResult Failed(platform::filesystem::EFileResultStatus status, std::string diagnostic)
	{
		return { EWorkspaceEditingOutcome::Failed, status, std::nullopt,
			std::nullopt, std::nullopt, std::move(diagnostic) };
	}
	static WorkspaceEditingResult Failed(std::optional<platform::serialization::EJsoncDiagnosticCode> diagnosticCode,
		std::string diagnostic)
	{
		return { EWorkspaceEditingOutcome::Failed, std::nullopt, diagnosticCode,
			std::nullopt, std::nullopt, std::move(diagnostic) };
	}
	static WorkspaceEditingResult Succeeded(platform::filesystem::FileVersionToken version,
		std::string document, std::string diagnostic)
	{
		return { EWorkspaceEditingOutcome::Succeeded, std::nullopt, std::nullopt,
			std::move(version), std::move(document), std::move(diagnostic) };
	}
	static bool IsWorkspaceResource(const platform::uri::Uri& resource) noexcept
	{
		return !resource.Scheme().empty() && !resource.Path().empty()
			&& !resource.Query() && !resource.Fragment();
	}
	static std::vector<WorkspaceFolderEdit> NormalizeFolders(
		const std::vector<WorkspaceFolderEdit>& folders, bool& invalid)
	{
		invalid = false;
		std::vector<WorkspaceFolderEdit> result;
		result.reserve(folders.size());
		std::vector<std::wstring> identities;
		identities.reserve(folders.size());
		for (const auto& folder : folders) {
			if (!IsWorkspaceResource(folder.uri)
				|| (folder.label && !IsValidLabel(*folder.label))) {
				invalid = true;
				return {};
			}
			auto identity = platform::uri::UriIdentityService::MakeComparisonKey(folder.uri);
			if (identity.empty()) {
				invalid = true;
				return {};
			}
			if (std::ranges::find(identities, identity) != identities.end()) continue;
			identities.push_back(std::move(identity));
			result.push_back(folder);
		}
		return result;
	}
	static bool IsValidLabel(std::wstring_view value) noexcept
	{
		return value.size() <= 256U && IsWellFormedUtf16(value);
	}
	static bool IsWellFormedUtf16(std::wstring_view value) noexcept
	{
		for (std::size_t index = 0; index < value.size(); ++index) {
			const auto unit = static_cast<std::uint32_t>(value[index]);
			if (unit >= 0xd800U && unit <= 0xdbffU) {
				if (++index == value.size()) return false;
				const auto low = static_cast<std::uint32_t>(value[index]);
				if (low < 0xdc00U || low > 0xdfffU) return false;
			} else if (unit >= 0xdc00U && unit <= 0xdfffU) return false;
		}
		return true;
	}
	static std::string ToText(const platform::filesystem::FileBytes& bytes)
	{
		return { bytes.begin(), bytes.end() };
	}
	static platform::filesystem::FileBytes ToBytes(std::string_view text)
	{
		return { text.begin(), text.end() };
	}
	static std::string_view DetectLineEnding(std::string_view document) noexcept
	{
		return document.find("\r\n") != std::string_view::npos ? "\r\n" : "\n";
	}
	static bool IsAbsoluteWorkspacePath(std::wstring_view path) noexcept
	{
		return (!path.empty() && (path.front() == L'/' || path.front() == L'\\'))
			|| (path.size() >= 3U && path[1] == L':' && (path[2] == L'/' || path[2] == L'\\'));
	}
	static std::vector<SourceFolderMetadata> ReadSourceFolderMetadata(
		std::string_view document, const platform::uri::Uri& workspaceConfigUri)
	{
		std::vector<SourceFolderMetadata> result;
		const auto parsed = platform::serialization::CJsoncDocument::Parse(document);
		if (!parsed.Succeeded()) return result;
		const auto* root = std::get_if<platform::serialization::JsoncValue::Object>(&parsed.value->Value());
		if (root == nullptr) return result;
		const auto member = root->find(L"folders");
		if (member == root->end()) return result;
		const auto* entries = std::get_if<platform::serialization::JsoncValue::Array>(&member->second.Value());
		if (entries == nullptr) return result;
		for (const auto& entry : *entries) {
			const auto* object = std::get_if<platform::serialization::JsoncValue::Object>(&entry.Value());
			if (object == nullptr) continue;
			const auto path = object->find(L"path");
			const auto uri = object->find(L"uri");
			if ((path == object->end()) == (uri == object->end())) continue;
			std::optional<platform::uri::Uri> resolved;
			ESourceFolderLocation location = ESourceFolderLocation::Uri;
			if (path != object->end()) {
				const auto* raw = std::get_if<std::wstring>(&path->second.Value());
				if (raw == nullptr) continue;
				resolved = CWorkspaceConfigurationDocumentParser::ResolveFolderPath(workspaceConfigUri, *raw);
				location = IsAbsoluteWorkspacePath(*raw)
					? ESourceFolderLocation::AbsolutePath : ESourceFolderLocation::RelativePath;
			} else {
				const auto* raw = std::get_if<std::wstring>(&uri->second.Value());
				if (raw == nullptr) continue;
				const auto resource = platform::uri::Uri::Parse(*raw);
				if (resource) resolved = CWorkspaceConfigurationDocumentParser::NormalizeFolderUri(*resource.value);
			}
			if (!resolved) continue;
			auto identity = platform::uri::UriIdentityService::MakeComparisonKey(*resolved);
			if (identity.empty() || std::ranges::any_of(result, [&](const auto& item) { return item.identity == identity; })) continue;
			std::optional<std::wstring> label;
			if (const auto name = object->find(L"name"); name != object->end()) {
				if (const auto* raw = std::get_if<std::wstring>(&name->second.Value())) label = *raw;
			}
			result.push_back({ std::move(identity), location, std::move(label) });
		}
		return result;
	}
	static bool IsLocalFileUri(const platform::uri::Uri& uri) noexcept
	{
		return _wcsicmp(uri.Scheme().c_str(), L"file") == 0
			&& (uri.Authority().empty() || _wcsicmp(uri.Authority().c_str(), L"localhost") == 0);
	}
	static bool HasSameFileAuthority(const platform::uri::Uri& left, const platform::uri::Uri& right) noexcept
	{
		return IsLocalFileUri(left) && IsLocalFileUri(right)
			&& _wcsicmp(left.Authority().c_str(), right.Authority().c_str()) == 0;
	}
	static bool HasSameSchemeAndAuthority(const platform::uri::Uri& left, const platform::uri::Uri& right) noexcept
	{
		return _wcsicmp(left.Scheme().c_str(), right.Scheme().c_str()) == 0
			&& _wcsicmp(left.Authority().c_str(), right.Authority().c_str()) == 0;
	}
	static bool IsUncPath(std::wstring_view path) noexcept
	{
		return path.size() >= 2U && ((path[0] == L'\\' && path[1] == L'\\') || (path[0] == L'/' && path[1] == L'/'));
	}
	static std::wstring NormalizeAbsoluteWindowsPath(std::wstring path)
	{
		std::replace(path.begin(), path.end(), L'\\', L'/');
		if (path.size() >= 2U && path[1] == L':' && path[0] >= L'a' && path[0] <= L'z') {
			path[0] = static_cast<wchar_t>(path[0] - L'a' + L'A');
		}
		return path;
	}
	static std::optional<std::wstring> SerializePath(const platform::uri::Uri& target, const platform::uri::Uri& folder)
	{
		if (!HasSameFileAuthority(target, folder)) {
			if (!HasSameSchemeAndAuthority(target, folder) || _wcsicmp(folder.Scheme().c_str(), L"file") == 0) {
				return std::nullopt;
			}
			const auto separator = target.Path().find_last_of(L'/');
			if (separator == std::wstring::npos) return std::nullopt;
			const auto directory = std::filesystem::path(target.Path().substr(0, separator + 1U));
			const auto relative = std::filesystem::path(folder.Path()).lexically_relative(directory);
			if (relative.empty() || relative.is_absolute()) return std::nullopt;
			return relative.generic_wstring();
		}
		const auto targetPath = target.ToWindowsPath();
		const auto folderPath = folder.ToWindowsPath();
		if (!targetPath.value || !folderPath.value || IsUncPath(*folderPath.value)) return std::nullopt;
		const auto directory = std::filesystem::path(*targetPath.value).parent_path();
		if (directory.empty()) return std::nullopt;
		const auto folderFilesystemPath = std::filesystem::path(*folderPath.value);
		if (_wcsicmp(folderFilesystemPath.root_name().c_str(), directory.root_name().c_str()) != 0) {
			return NormalizeAbsoluteWindowsPath(*folderPath.value);
		}
		const auto relative = folderFilesystemPath.lexically_relative(directory);
		if (!relative.empty() && !relative.is_absolute() && !IsUncPath(relative.native())) {
			return relative.generic_wstring();
		}
		return NormalizeAbsoluteWindowsPath(*folderPath.value);
	}
	static std::optional<std::wstring> SerializeAbsolutePath(const platform::uri::Uri& folder)
	{
		if (_wcsicmp(folder.Scheme().c_str(), L"file") == 0) {
			const auto path = folder.ToWindowsPath();
			if (!path.value) return std::nullopt;
			return NormalizeAbsoluteWindowsPath(*path.value);
		}
		if (!folder.Path().empty() && folder.Path().front() == L'/') return folder.Path();
		return std::nullopt;
	}
	static bool AppendUtf8(std::string& target, std::uint32_t codePoint)
	{
		if (codePoint > 0x10ffffU || (codePoint >= 0xd800U && codePoint <= 0xdfffU)) return false;
		if (codePoint <= 0x7fU) target.push_back(static_cast<char>(codePoint));
		else if (codePoint <= 0x7ffU) {
			target.push_back(static_cast<char>(0xc0U | (codePoint >> 6U)));
			target.push_back(static_cast<char>(0x80U | (codePoint & 0x3fU)));
		} else if (codePoint <= 0xffffU) {
			target.push_back(static_cast<char>(0xe0U | (codePoint >> 12U)));
			target.push_back(static_cast<char>(0x80U | ((codePoint >> 6U) & 0x3fU)));
			target.push_back(static_cast<char>(0x80U | (codePoint & 0x3fU)));
		} else {
			target.push_back(static_cast<char>(0xf0U | (codePoint >> 18U)));
			target.push_back(static_cast<char>(0x80U | ((codePoint >> 12U) & 0x3fU)));
			target.push_back(static_cast<char>(0x80U | ((codePoint >> 6U) & 0x3fU)));
			target.push_back(static_cast<char>(0x80U | (codePoint & 0x3fU)));
		}
		return true;
	}
	static std::optional<std::string> QuoteJson(std::wstring_view value)
	{
		if (!IsWellFormedUtf16(value)) return std::nullopt;
		std::string result;
		result.push_back('"');
		for (std::size_t index = 0; index < value.size(); ++index) {
			const auto unit = static_cast<std::uint32_t>(value[index]);
			if (unit == L'"' || unit == L'\\') { result.push_back('\\'); result.push_back(static_cast<char>(unit)); }
			else if (unit == L'\b') result += "\\b";
			else if (unit == L'\f') result += "\\f";
			else if (unit == L'\n') result += "\\n";
			else if (unit == L'\r') result += "\\r";
			else if (unit == L'\t') result += "\\t";
			else if (unit <= 0x1fU) {
				static constexpr char hex[] = "0123456789abcdef";
				result += "\\u00";
				result.push_back(hex[(unit >> 4U) & 0x0fU]);
				result.push_back(hex[unit & 0x0fU]);
			}
			else {
				std::uint32_t codePoint = unit;
				if (unit >= 0xd800U && unit <= 0xdbffU) {
					if (++index == value.size()) return std::nullopt;
					const auto low = static_cast<std::uint32_t>(value[index]);
					if (low < 0xdc00U || low > 0xdfffU) return std::nullopt;
					codePoint = 0x10000U + ((unit - 0xd800U) << 10U) + (low - 0xdc00U);
				} else if (unit >= 0xdc00U && unit <= 0xdfffU) return std::nullopt;
				if (!AppendUtf8(result, codePoint)) return std::nullopt;
			}
		}
		result.push_back('"');
		return result;
	}
	static std::optional<std::string> RenderFolders(const platform::uri::Uri& target,
		const std::vector<WorkspaceFolderEdit>& folders,
		const std::vector<SourceFolderMetadata>& sourceMetadata, std::string_view eol)
	{
		std::string result = "[";
		if (folders.empty()) return result + "]";
		result += eol;
		for (std::size_t index = 0; index < folders.size(); ++index) {
			const auto identity = platform::uri::UriIdentityService::MakeComparisonKey(folders[index].uri);
			const auto metadata = std::ranges::find_if(sourceMetadata,
				[&](const auto& candidate) { return candidate.identity == identity; });
			std::optional<std::wstring> location;
			if (metadata == sourceMetadata.end()) {
				location = SerializePath(target, folders[index].uri);
			} else if (metadata->location == ESourceFolderLocation::RelativePath) {
				location = SerializePath(target, folders[index].uri);
				if (!location && _wcsicmp(folders[index].uri.Scheme().c_str(), L"file") == 0) {
					location = SerializeAbsolutePath(folders[index].uri);
				}
			} else if (metadata->location == ESourceFolderLocation::AbsolutePath
				&& (_wcsicmp(folders[index].uri.Scheme().c_str(), L"file") == 0
					|| HasSameSchemeAndAuthority(target, folders[index].uri))) {
				location = SerializeAbsolutePath(folders[index].uri);
			}
			const auto serializedLocation = QuoteJson(location ? *location : folders[index].uri.ToString());
			if (!serializedLocation) return std::nullopt;
			result += "    { \"";
			result += location ? "path" : "uri";
			result += "\": ";
			result += *serializedLocation;
			const auto& labelValue = metadata != sourceMetadata.end() ? metadata->label : folders[index].label;
			if (labelValue) {
				const auto label = QuoteJson(*labelValue);
				if (!label) return std::nullopt;
				result += ", \"name\": ";
				result += *label;
			}
			result += " }";
			if (index + 1U != folders.size()) result.push_back(',');
			result += eol;
		}
		return result + "  ]";
	}
	std::optional<ReplaceOptions> TargetExpectation(const platform::uri::Uri& target, bool writesSource,
		bool sourceMissing, const FileReadResult& source, WorkspaceEditingResult& failure)
	{
		if (writesSource) {
			if (sourceMissing) return ReplaceOptions::ForMissing();
			return ReplaceOptions::ForCurrent(source.value->version);
		}
		const auto targetRead = m_files.ReadVersioned(target, { kMaximumInputBytes });
		if (targetRead.status == platform::filesystem::EFileResultStatus::NotFound) {
			return ReplaceOptions::ForMissing();
		}
		if (!targetRead.Succeeded() || !targetRead.value) {
			failure = Failed(targetRead.status, "workspace target read failed");
			return std::nullopt;
		}
		if (targetRead.value->bytes.size() > kMaximumInputBytes) {
			failure = Failed("workspace target exceeds the byte limit");
			return std::nullopt;
		}
		return ReplaceOptions::ForCurrent(targetRead.value->version);
	}
};

} // namespace workbench::workspace

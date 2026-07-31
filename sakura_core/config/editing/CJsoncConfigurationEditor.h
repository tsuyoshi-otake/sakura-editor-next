/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#pragma once

#include "config/ConfigurationTypes.h"
#include "platform/filesystem/IFileService.h"
#include "platform/serialization/JsoncDocument.h"

#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

namespace config::editing {

//! The document kind is deliberately separate from ConfigurationSource.  A UI
//! or extension host can therefore select a concrete settings resource without
//! coupling this textual editor to effective-value resolution.
enum class EConfigurationDocumentScope : std::uint8_t {
	Profile,
	User,
	Workspace,
	Folder,
	LanguageOverride,
};

struct ConfigurationDocumentEditTarget final {
	EConfigurationDocumentScope scope = EConfigurationDocumentScope::Profile;
	ConfigurationTarget target;
	std::optional<platform::uri::Uri> resource;
};

//! An absent value removes the exact member.  A LanguageOverride request edits
//! the requested member inside VS Code's top-level "[languageId]" object; it
//! never rewrites the surrounding JSONC document.
struct ConfigurationDocumentEditRequest final {
	ConfigurationDocumentEditTarget target;
	std::string key;
	std::optional<ConfigurationValue> value;
};

enum class EConfigurationDocumentEditStatus : std::uint8_t {
	Applied,
	NoChange,
	ReadFailed,
	ParseFailed,
	InvalidRequest,
	InputTooLarge,
	OutputTooLarge,
	Conflict,
	Unsupported,
	Failed,
};

//! Diagnostics are category-only.  In particular, they deliberately never
//! contain a resource URI, local path, setting key, or serialized value.
struct ConfigurationDocumentEditResult final {
	EConfigurationDocumentEditStatus status = EConfigurationDocumentEditStatus::Failed;
	std::optional<platform::filesystem::EFileResultStatus> fileStatus;
	std::optional<platform::serialization::EJsoncDiagnosticCode> jsoncDiagnostic;
	std::string diagnostic;

	[[nodiscard]] bool Succeeded() const noexcept
	{
		return status == EConfigurationDocumentEditStatus::Applied
			|| status == EConfigurationDocumentEditStatus::NoChange;
	}
};

//! Bounded, synchronous, UI-independent JSONC preserving editor.  Each call
//! performs one versioned read and at most one conditional atomic replace.  A
//! conflict is terminal: callers must deliberately reread and replan.
class CJsoncConfigurationEditor final {
public:
	static constexpr std::size_t kMaximumInputBytes = platform::serialization::CJsoncDocument::kMaximumInputBytes;
	static constexpr std::size_t kMaximumOutputBytes = platform::serialization::CJsoncDocument::kMaximumInputBytes;

	explicit CJsoncConfigurationEditor(platform::filesystem::IFileService& fileService) noexcept
		: m_fileService(fileService) {}

	[[nodiscard]] ConfigurationDocumentEditResult Edit(const ConfigurationDocumentEditRequest& request)
	{
		if (!IsValidTarget(request.target)) return Result(EConfigurationDocumentEditStatus::InvalidRequest, "invalid configuration document target");
		if (!IsCanonicalAsciiKey(request.key) || (request.value && !request.value->IsValid())) {
			return Result(EConfigurationDocumentEditStatus::InvalidRequest, "invalid configuration edit request");
		}

		auto read = m_fileService.ReadVersioned(*request.target.resource, { kMaximumInputBytes });
		if (!read.Succeeded()) {
			if (read.status == platform::filesystem::EFileResultStatus::NotFound) return EditMissing(request);
			return FileReadFailure(read.status);
		}
		if (!read.value) return Result(EConfigurationDocumentEditStatus::Failed, "versioned read returned no snapshot");
		if (read.value->bytes.size() > kMaximumInputBytes) return Result(EConfigurationDocumentEditStatus::InputTooLarge, "configuration document input exceeds the byte limit");

		std::string document(read.value->bytes.begin(), read.value->bytes.end());
		auto parsed = platform::serialization::CJsoncDocument::Parse(document);
		if (!parsed.Succeeded()) return ParseFailure(parsed.diagnostic);
		if (!std::holds_alternative<platform::serialization::JsoncValue::Object>(parsed.value->Value())) {
			return Result(EConfigurationDocumentEditStatus::ParseFailed, "configuration document root is not an object");
		}
		if (request.target.scope == EConfigurationDocumentScope::LanguageOverride) {
			return EditLanguageOverride(request, document, read.value->version);
		}

		auto layout = ScanTopLevelObject(document);
		if (!layout) return Result(EConfigurationDocumentEditStatus::Unsupported, "cannot safely locate top-level configuration member");
		const auto found = FindMember(*layout, request.key);
		std::optional<std::string> replacement;
		if (request.value) {
			replacement.emplace();
			if (!SerializeValue(*request.value, *replacement, kMaximumOutputBytes)) {
				return Result(EConfigurationDocumentEditStatus::OutputTooLarge, "serialized configuration value exceeds the byte limit");
			}
		}

		std::optional<std::string> planned;
		if (request.value) planned = found ? ReplaceValue(document, *found, *replacement) : InsertMember(document, *layout, request.key, *replacement);
		else planned = found ? RemoveMember(document, *layout, *found) : std::optional<std::string>(document);
		if (!planned) return Result(EConfigurationDocumentEditStatus::Unsupported, "cannot safely preserve surrounding JSONC trivia");
		if (*planned == document) return Result(EConfigurationDocumentEditStatus::NoChange, "configuration document already has the requested edit");
		if (planned->size() > kMaximumOutputBytes) return Result(EConfigurationDocumentEditStatus::OutputTooLarge, "edited configuration document exceeds the byte limit");

		auto validated = platform::serialization::CJsoncDocument::Parse(*planned);
		if (!validated.Succeeded() || !std::holds_alternative<platform::serialization::JsoncValue::Object>(validated.value->Value())) {
			return ParseFailure(validated.diagnostic);
		}
		platform::filesystem::FileBytes bytes(planned->begin(), planned->end());
		auto published = m_fileService.ConditionalAtomicReplace(
			*request.target.resource, bytes, platform::filesystem::FileConditionalReplaceOptions::ForCurrent(read.value->version));
		return PublishResult(published);
	}

private:
	struct Member final {
		std::string key;
		std::size_t keyBegin = 0;
		std::size_t keyEnd = 0;
		std::size_t valueBegin = 0;
		std::size_t valueEnd = 0;
		std::size_t comma = 0;
		bool hasComma = false;
	};
	struct ObjectLayout final {
		std::size_t open = 0;
		std::size_t close = 0;
		std::vector<Member> members;
	};

	static ConfigurationDocumentEditResult Result(EConfigurationDocumentEditStatus status, const char* diagnostic)
	{
		return { status, std::nullopt, std::nullopt, diagnostic };
	}
	static bool IsValidTarget(const ConfigurationDocumentEditTarget& target) noexcept
	{
		if (!target.resource) return false;
		const auto& identity = target.target;
		if (identity.folderUri && !identity.workspaceUri) return false;
		switch (target.scope) {
		case EConfigurationDocumentScope::Profile:
		case EConfigurationDocumentScope::User:
			return !identity.profileId.empty() && !identity.workspaceUri && !identity.folderUri && !identity.languageId;
		case EConfigurationDocumentScope::Workspace:
			return !identity.profileId.empty() && identity.workspaceUri.has_value() && !identity.folderUri && !identity.languageId;
		case EConfigurationDocumentScope::Folder:
			return !identity.profileId.empty() && identity.workspaceUri.has_value() && identity.folderUri.has_value() && !identity.languageId;
		case EConfigurationDocumentScope::LanguageOverride:
			return !identity.profileId.empty() && identity.languageId && IsValidLanguageId(*identity.languageId);
		}
		return false;
	}
	static bool IsValidLanguageId(std::wstring_view languageId) noexcept
	{
		if (languageId.empty()) return false;
		for (std::size_t index = 0; index < languageId.size(); ++index) {
			const auto unit = static_cast<std::uint32_t>(languageId[index]);
			if (unit == L'[' || unit == L']') return false;
			if (unit >= 0xd800U && unit <= 0xdbffU) {
				if (index + 1 == languageId.size()) return false;
				const auto low = static_cast<std::uint32_t>(languageId[++index]);
				if (low < 0xdc00U || low > 0xdfffU) return false;
			} else if (unit >= 0xdc00U && unit <= 0xdfffU) {
				return false;
			}
		}
		return true;
	}
	static bool IsCanonicalAsciiKey(std::string_view key) noexcept
	{
		if (key.empty()) return false;
		for (const char character : key) {
			const bool allowed = (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z')
				|| (character >= '0' && character <= '9') || character == '.' || character == '_' || character == '-';
			if (!allowed) return false;
		}
		return true;
	}
	static ConfigurationDocumentEditResult FileReadFailure(platform::filesystem::EFileResultStatus status)
	{
		if (status == platform::filesystem::EFileResultStatus::Unsupported) {
			auto result = Result(EConfigurationDocumentEditStatus::Unsupported, "versioned read is unsupported"); result.fileStatus = status; return result;
		}
		if (status == platform::filesystem::EFileResultStatus::Failed) {
			auto result = Result(EConfigurationDocumentEditStatus::Failed, "versioned read failed"); result.fileStatus = status; return result;
		}
		auto result = Result(EConfigurationDocumentEditStatus::ReadFailed, "versioned read did not succeed"); result.fileStatus = status; return result;
	}
	static ConfigurationDocumentEditResult ParseFailure(const std::optional<platform::serialization::JsoncDiagnostic>& diagnostic)
	{
		auto result = Result(EConfigurationDocumentEditStatus::ParseFailed, "configuration document JSONC parsing failed");
		if (diagnostic) result.jsoncDiagnostic = diagnostic->code;
		return result;
	}
	ConfigurationDocumentEditResult EditMissing(const ConfigurationDocumentEditRequest& request)
	{
		if (!request.value) return Result(EConfigurationDocumentEditStatus::NoChange, "configuration member is absent from a missing document");
		std::string value;
		if (!SerializeValue(*request.value, value, kMaximumOutputBytes)) return Result(EConfigurationDocumentEditStatus::OutputTooLarge, "serialized configuration value exceeds the byte limit");
		std::string document;
		if (request.target.scope == EConfigurationDocumentScope::LanguageOverride) {
			auto selector = LanguageSelector(*request.target.target.languageId);
			if (!selector) return Result(EConfigurationDocumentEditStatus::InvalidRequest, "invalid language override selector");
			document = "{\n  " + QuoteJsonKey(*selector) + ": {\n    \"" + request.key + "\": " + value + "\n  }\n}\n";
		} else {
			document = "{\n  \"" + request.key + "\": " + value + "\n}\n";
		}
		if (document.size() > kMaximumOutputBytes) return Result(EConfigurationDocumentEditStatus::OutputTooLarge, "edited configuration document exceeds the byte limit");
		auto published = m_fileService.ConditionalAtomicReplace(*request.target.resource,
			platform::filesystem::FileBytes(document.begin(), document.end()), platform::filesystem::FileConditionalReplaceOptions::ForMissing());
		return PublishResult(published);
	}
	static ConfigurationDocumentEditResult PublishResult(const platform::filesystem::FileConditionalReplaceResult& published)
	{
		switch (published.status) {
		case platform::filesystem::EFileConditionalReplaceStatus::Succeeded: return Result(EConfigurationDocumentEditStatus::Applied, "configuration document edit applied");
		case platform::filesystem::EFileConditionalReplaceStatus::Conflict: return Result(EConfigurationDocumentEditStatus::Conflict, "configuration document changed before publish");
		case platform::filesystem::EFileConditionalReplaceStatus::Unsupported: return Result(EConfigurationDocumentEditStatus::Unsupported, "conditional atomic replace is unsupported");
		case platform::filesystem::EFileConditionalReplaceStatus::Failed: return Result(EConfigurationDocumentEditStatus::Failed, "conditional atomic replace failed");
		}
		return Result(EConfigurationDocumentEditStatus::Failed, "conditional atomic replace failed");
	}
	static std::optional<std::string> LanguageSelector(std::wstring_view languageId)
	{
		if (!IsValidLanguageId(languageId)) return std::nullopt;
		// Each UTF-16 code unit can become at most four UTF-8 bytes.  Bound this
		// before allocation so a caller-controlled language ID cannot bypass the
		// document output limit while planning an otherwise small edit.
		if (languageId.size() > (kMaximumOutputBytes - 2) / 4) return std::nullopt;
		std::string selector;
		selector.push_back('[');
		for (std::size_t index = 0; index < languageId.size(); ++index) {
			std::uint32_t codePoint = static_cast<std::uint32_t>(languageId[index]);
			if (codePoint >= 0xd800U && codePoint <= 0xdbffU) {
				const auto low = static_cast<std::uint32_t>(languageId[++index]);
				codePoint = 0x10000U + ((codePoint - 0xd800U) << 10U) + (low - 0xdc00U);
			}
			if (!AppendCodePointUtf8(selector, codePoint)) return std::nullopt;
		}
		selector.push_back(']');
		return selector;
	}
	static bool SelectorContainsLanguage(std::string_view selector, std::string_view language) noexcept
	{
		if (selector.empty() || selector.front() != '[') return false;
		for (std::size_t position = 0; position < selector.size();) {
			if (selector[position] != '[') return false;
			const auto close = selector.find(']', position + 1);
			if (close == std::string_view::npos || close == position + 1) return false;
			if (selector.substr(position + 1, close - position - 1) == language) return true;
			position = close + 1;
		}
		return false;
	}
	static bool BuildLanguageObject(std::string_view key, std::string_view value, std::string& output, std::size_t maximum)
	{
		return Append(output, "{", maximum)
			&& Append(output, "\"", maximum)
			&& Append(output, key, maximum)
			&& Append(output, "\":", maximum)
			&& Append(output, value, maximum)
			&& Append(output, "}", maximum);
	}
	ConfigurationDocumentEditResult EditLanguageOverride(
		const ConfigurationDocumentEditRequest& request,
		const std::string& document,
		const platform::filesystem::FileVersionToken& version)
	{
		auto selector = LanguageSelector(*request.target.target.languageId);
		if (!selector) return Result(EConfigurationDocumentEditStatus::InvalidRequest, "invalid language override selector");
		auto root = ScanTopLevelObject(document);
		if (!root) return Result(EConfigurationDocumentEditStatus::Unsupported, "cannot safely locate top-level configuration member");
		const auto found = FindMember(*root, *selector);
		for (const auto& member : root->members) {
			if (member.key != *selector && SelectorContainsLanguage(member.key,
				selector->substr(1, selector->size() - 2))) {
				return Result(EConfigurationDocumentEditStatus::Unsupported, "language override is part of a combined selector");
			}
		}
		std::optional<std::string> replacement;
		if (request.value) {
			replacement.emplace();
			if (!SerializeValue(*request.value, *replacement, kMaximumOutputBytes)) {
				return Result(EConfigurationDocumentEditStatus::OutputTooLarge, "serialized configuration value exceeds the byte limit");
			}
		}
		std::optional<std::string> planned;
		if (!found) {
			if (!request.value) return Result(EConfigurationDocumentEditStatus::NoChange, "language override member is absent");
			std::string object;
			if (!BuildLanguageObject(request.key, *replacement, object, kMaximumOutputBytes)) {
				return Result(EConfigurationDocumentEditStatus::OutputTooLarge, "edited configuration document exceeds the byte limit");
			}
			planned = InsertMember(document, *root, *selector, object);
		} else {
			auto overrideObject = ScanObjectAt(document, found->valueBegin);
			if (!overrideObject || overrideObject->close + 1 != found->valueEnd) {
				return Result(EConfigurationDocumentEditStatus::Unsupported, "language override value is not an object");
			}
			const auto member = FindMember(*overrideObject, request.key);
			if (request.value) planned = member
				? ReplaceValue(document, *member, *replacement)
				: InsertMember(document, *overrideObject, request.key, *replacement);
			else planned = member ? RemoveMember(document, *overrideObject, *member) : std::optional<std::string>(document);
		}
		if (!planned) return Result(EConfigurationDocumentEditStatus::Unsupported, "cannot safely preserve surrounding JSONC trivia");
		if (*planned == document) return Result(EConfigurationDocumentEditStatus::NoChange, "configuration document already has the requested edit");
		if (planned->size() > kMaximumOutputBytes) return Result(EConfigurationDocumentEditStatus::OutputTooLarge, "edited configuration document exceeds the byte limit");
		auto validated = platform::serialization::CJsoncDocument::Parse(*planned);
		if (!validated.Succeeded() || !std::holds_alternative<platform::serialization::JsoncValue::Object>(validated.value->Value())) {
			return ParseFailure(validated.diagnostic);
		}
		platform::filesystem::FileBytes bytes(planned->begin(), planned->end());
		return PublishResult(m_fileService.ConditionalAtomicReplace(
			*request.target.resource, bytes, platform::filesystem::FileConditionalReplaceOptions::ForCurrent(version)));
	}

	static bool IsSpace(char character) noexcept { return character == ' ' || character == '\t' || character == '\r' || character == '\n'; }
	static bool SkipTrivia(std::string_view text, std::size_t& position) noexcept
	{
		while (position < text.size()) {
			while (position < text.size() && IsSpace(text[position])) ++position;
			if (position + 1 >= text.size() || text[position] != '/') return true;
			if (text[position + 1] == '/') { position += 2; while (position < text.size() && text[position] != '\r' && text[position] != '\n') ++position; continue; }
			if (text[position + 1] != '*') return true;
			position += 2;
			while (position + 1 < text.size() && !(text[position] == '*' && text[position + 1] == '/')) ++position;
			if (position + 1 >= text.size()) return false;
			position += 2;
		}
		return true;
	}
	static bool ScanString(std::string_view text, std::size_t& position) noexcept
	{
		if (position >= text.size() || text[position] != '"') return false;
		++position;
		while (position < text.size()) {
			if (text[position] == '"') { ++position; return true; }
			if (text[position++] != '\\') continue;
			if (position >= text.size()) return false;
			if (text[position++] == 'u') { if (text.size() - position < 4) return false; position += 4; }
		}
		return false;
	}
	static bool AppendCodePointUtf8(std::string& output, std::uint32_t codePoint)
	{
		if (codePoint <= 0x7fU) output.push_back(static_cast<char>(codePoint));
		else if (codePoint <= 0x7ffU) {
			output.push_back(static_cast<char>(0xc0U | (codePoint >> 6U)));
			output.push_back(static_cast<char>(0x80U | (codePoint & 0x3fU)));
		} else if (codePoint <= 0xffffU) {
			output.push_back(static_cast<char>(0xe0U | (codePoint >> 12U)));
			output.push_back(static_cast<char>(0x80U | ((codePoint >> 6U) & 0x3fU)));
			output.push_back(static_cast<char>(0x80U | (codePoint & 0x3fU)));
		} else if (codePoint <= 0x10ffffU) {
			output.push_back(static_cast<char>(0xf0U | (codePoint >> 18U)));
			output.push_back(static_cast<char>(0x80U | ((codePoint >> 12U) & 0x3fU)));
			output.push_back(static_cast<char>(0x80U | ((codePoint >> 6U) & 0x3fU)));
			output.push_back(static_cast<char>(0x80U | (codePoint & 0x3fU)));
		} else return false;
		return true;
	}
	static bool DecodeJsonKey(std::string_view text, std::size_t begin, std::size_t end, std::string& output)
	{
		if (end <= begin + 1 || text[begin] != '"' || text[end - 1] != '"') return false;
		output.clear();
		for (std::size_t position = begin + 1; position + 1 < end;) {
			unsigned int value = static_cast<unsigned char>(text[position++]);
			if (value == '\\') {
				if (position + 1 > end) return false;
				const char escaped = text[position++];
				switch (escaped) {
				case '"': value = '"'; break; case '\\': value = '\\'; break; case '/': value = '/'; break;
				case 'b': value = '\b'; break; case 'f': value = '\f'; break; case 'n': value = '\n'; break; case 'r': value = '\r'; break; case 't': value = '\t'; break;
				case 'u': {
					if (end - position < 5) return false;
					value = 0;
					for (int index = 0; index < 4; ++index) { const char digit = text[position++]; value <<= 4; if (digit >= '0' && digit <= '9') value |= digit - '0'; else if (digit >= 'a' && digit <= 'f') value |= digit - 'a' + 10; else if (digit >= 'A' && digit <= 'F') value |= digit - 'A' + 10; else return false; }
					if (value >= 0xd800U && value <= 0xdbffU) {
						if (position + 6 > end || text[position] != '\\' || text[position + 1] != 'u') return false;
						position += 2;
						unsigned int low = 0;
						for (int index = 0; index < 4; ++index) { const char digit = text[position++]; low <<= 4; if (digit >= '0' && digit <= '9') low |= digit - '0'; else if (digit >= 'a' && digit <= 'f') low |= digit - 'a' + 10; else if (digit >= 'A' && digit <= 'F') low |= digit - 'A' + 10; else return false; }
						if (low < 0xdc00U || low > 0xdfffU) return false;
						if (!AppendCodePointUtf8(output, 0x10000U + ((value - 0xd800U) << 10U) + (low - 0xdc00U))) return false;
						continue;
					}
					if (value >= 0xdc00U && value <= 0xdfffU || !AppendCodePointUtf8(output, value)) return false;
					continue;
				}
				default: return false;
				}
			}
			output.push_back(static_cast<char>(value));
		}
		return true;
	}
	static bool ScanValue(std::string_view text, std::size_t& position) noexcept
	{
		if (position >= text.size()) return false;
		if (text[position] == '"') return ScanString(text, position);
		if (text[position] != '{' && text[position] != '[') {
			const auto begin = position;
			while (position < text.size() && !IsSpace(text[position]) && text[position] != ',' && text[position] != '}' && text[position] != ']'
				&& !(text[position] == '/' && position + 1 < text.size() && (text[position + 1] == '/' || text[position + 1] == '*'))) ++position;
			return position != begin;
		}
		std::vector<char> closing;
		closing.push_back(text[position++] == '{' ? '}' : ']');
		while (!closing.empty() && position < text.size()) {
			if (text[position] == '"') { if (!ScanString(text, position)) return false; continue; }
			if (text[position] == '/' && position + 1 < text.size() && (text[position + 1] == '/' || text[position + 1] == '*')) { if (!SkipTrivia(text, position)) return false; continue; }
			if (text[position] == '{') { closing.push_back('}'); ++position; continue; }
			if (text[position] == '[') { closing.push_back(']'); ++position; continue; }
			if (text[position] != closing.back()) { ++position; continue; }
			closing.pop_back(); ++position;
		}
		return closing.empty();
	}
	static std::optional<ObjectLayout> ScanObjectAt(std::string_view text, std::size_t position)
	{
		if (position >= text.size() || text[position] != '{') return std::nullopt;
		ObjectLayout layout; layout.open = position++;
		if (!SkipTrivia(text, position)) return std::nullopt;
		if (position < text.size() && text[position] == '}') { layout.close = position; return layout; }
		while (position < text.size()) {
			Member member; member.keyBegin = position;
			if (!ScanString(text, position)) return std::nullopt;
			member.keyEnd = position;
			if (!DecodeJsonKey(text, member.keyBegin, member.keyEnd, member.key)) member.key.clear();
			if (!SkipTrivia(text, position) || position >= text.size() || text[position++] != ':') return std::nullopt;
			if (!SkipTrivia(text, position)) return std::nullopt;
			member.valueBegin = position;
			if (!ScanValue(text, position)) return std::nullopt;
			member.valueEnd = position;
			if (!SkipTrivia(text, position)) return std::nullopt;
			if (position < text.size() && text[position] == ',') { member.hasComma = true; member.comma = position++; }
			layout.members.push_back(std::move(member));
			if (!SkipTrivia(text, position)) return std::nullopt;
			if (position < text.size() && text[position] == '}') { layout.close = position; return layout; }
			if (!layout.members.back().hasComma) return std::nullopt;
		}
		return std::nullopt;
	}
	static std::optional<ObjectLayout> ScanTopLevelObject(std::string_view text)
	{
		std::size_t position = text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xefU && static_cast<unsigned char>(text[1]) == 0xbbU && static_cast<unsigned char>(text[2]) == 0xbfU ? 3 : 0;
		if (!SkipTrivia(text, position)) return std::nullopt;
		return ScanObjectAt(text, position);
	}
	static const Member* FindMember(const ObjectLayout& layout, std::string_view key) noexcept
	{
		for (const auto& member : layout.members) if (member.key == key) return &member;
		return nullptr;
	}
	static bool HasOnlyWhitespace(std::string_view text, std::size_t begin, std::size_t end) noexcept
	{
		while (begin < end) if (!IsSpace(text[begin++])) return false;
		return true;
	}
	static std::string ReplaceValue(const std::string& document, const Member& member, const std::string& replacement)
	{
		std::string result = document;
		result.replace(member.valueBegin, member.valueEnd - member.valueBegin, replacement);
		return result;
	}
	static std::string QuoteJsonKey(std::string_view key)
	{
		static constexpr char hex[] = "0123456789ABCDEF";
		std::string result;
		result.reserve(key.size() + 2);
		result.push_back('"');
		for (const auto character : key) {
			const auto unit = static_cast<unsigned char>(character);
			if (character == '"') result += "\\\"";
			else if (character == '\\') result += "\\\\";
			else if (unit < 0x20U) {
				result += "\\u00";
				result.push_back(hex[(unit >> 4U) & 15U]);
				result.push_back(hex[unit & 15U]);
			} else result.push_back(character);
		}
		result.push_back('"');
		return result;
	}
	static std::string Newline(const std::string& document)
	{
		if (document.find("\r\n") != std::string::npos) return "\r\n";
		if (document.find('\n') != std::string::npos) return "\n";
		if (document.find('\r') != std::string::npos) return "\r";
		return "\n";
	}
	static bool EndsInNewline(std::string_view text) noexcept { return !text.empty() && (text.back() == '\n' || text.back() == '\r'); }
	static std::string IndentBefore(const std::string& document, std::size_t position)
	{
		const auto line = document.find_last_of("\r\n", position == 0 ? 0 : position - 1);
		const auto begin = line == std::string::npos ? 0 : line + 1;
		std::string indent;
		for (std::size_t index = begin; index < position && (document[index] == ' ' || document[index] == '\t'); ++index) indent.push_back(document[index]);
		return indent.empty() ? "  " : indent;
	}
	static std::optional<std::string> InsertMember(const std::string& document, const ObjectLayout& layout, const std::string& key, const std::string& value)
	{
		std::string result = document;
		const std::string member = QuoteJsonKey(key) + ": " + value;
		if (layout.members.empty()) {
			const auto inside = std::string_view(document).substr(layout.open + 1, layout.close - layout.open - 1);
			if (EndsInNewline(inside)) result.insert(layout.close, IndentBefore(document, layout.close) + member + Newline(document));
			else result.insert(layout.close, member);
			return result;
		}
		const auto& last = layout.members.back();
		if (!last.hasComma) result.insert(last.valueEnd, ",");
		const auto shiftedClose = layout.close + (last.hasComma ? 0U : 1U);
		const auto tail = std::string_view(result).substr(last.valueEnd + (last.hasComma ? 0U : 1U), shiftedClose - (last.valueEnd + (last.hasComma ? 0U : 1U)));
		result.insert(shiftedClose, (EndsInNewline(tail) ? IndentBefore(document, last.keyBegin) : " ") + member);
		return result;
	}
	static std::optional<std::string> RemoveMember(const std::string& document, const ObjectLayout& layout, const Member& member)
	{
		// Removing a member must not discard a comment embedded between its key,
		// colon, value, or outgoing comma.  Keeping only independently safe edits
		// is preferable to a broad reserialization that could erase user trivia.
		std::size_t colon = member.keyEnd;
		if (!SkipTrivia(document, colon) || colon >= document.size() || document[colon] != ':') return std::nullopt;
		if (!HasOnlyWhitespace(document, member.keyEnd, colon)) return std::nullopt;
		++colon;
		if (!HasOnlyWhitespace(document, colon, member.valueBegin)) return std::nullopt;
		std::string result = document;
		if (member.hasComma) {
			if (!HasOnlyWhitespace(document, member.valueEnd, member.comma)) return std::nullopt;
			result.erase(member.keyBegin, member.comma + 1 - member.keyBegin);
			return result;
		}
		if (layout.members.size() == 1) { result.erase(member.keyBegin, member.valueEnd - member.keyBegin); return result; }
		const Member* previous = nullptr;
		for (const auto& candidate : layout.members) { if (&candidate == &member) break; previous = &candidate; }
		if (!previous || !previous->hasComma) return std::nullopt;
		result.erase(member.keyBegin, member.valueEnd - member.keyBegin);
		result.erase(previous->comma, 1);
		return result;
	}
	static bool Append(std::string& output, std::string_view text, std::size_t maximum)
	{
		if (text.size() > maximum - output.size()) return false;
		output.append(text); return true;
	}
	static bool AppendQuoted(std::string& output, const std::wstring& value, std::size_t maximum)
	{
		if (!Append(output, "\"", maximum)) return false;
		static constexpr char hex[] = "0123456789ABCDEF";
		for (std::size_t index = 0; index < value.size(); ++index) {
			const auto unit = static_cast<std::uint32_t>(value[index]);
			if (unit >= 0xd800U && unit <= 0xdbffU) { if (index + 1 == value.size() || value[index + 1] < 0xdc00 || value[index + 1] > 0xdfff) return false; }
			if (unit >= 0xdc00U && unit <= 0xdfffU) { if (index == 0 || value[index - 1] < 0xd800 || value[index - 1] > 0xdbff) return false; }
			if (unit == '"') { if (!Append(output, "\\\"", maximum)) return false; }
			else if (unit == '\\') { if (!Append(output, "\\\\", maximum)) return false; }
			else if (unit == '\b') { if (!Append(output, "\\b", maximum)) return false; }
			else if (unit == '\f') { if (!Append(output, "\\f", maximum)) return false; }
			else if (unit == '\n') { if (!Append(output, "\\n", maximum)) return false; }
			else if (unit == '\r') { if (!Append(output, "\\r", maximum)) return false; }
			else if (unit == '\t') { if (!Append(output, "\\t", maximum)) return false; }
			else if (unit < 0x20U || unit > 0x7eU) { char escaped[] { '\\', 'u', hex[(unit >> 12U) & 15U], hex[(unit >> 8U) & 15U], hex[(unit >> 4U) & 15U], hex[unit & 15U] }; if (!Append(output, std::string_view(escaped, 6), maximum)) return false; }
			else { const char ascii = static_cast<char>(unit); if (!Append(output, std::string_view(&ascii, 1), maximum)) return false; }
		}
		return Append(output, "\"", maximum);
	}
	static bool SerializeValue(const ConfigurationValue& value, std::string& output, std::size_t maximum)
	{
		const auto& storage = value.Value();
		if (std::holds_alternative<std::monostate>(storage)) return Append(output, "null", maximum);
		if (const auto boolean = std::get_if<bool>(&storage)) return Append(output, *boolean ? "true" : "false", maximum);
		char buffer[std::numeric_limits<double>::max_digits10 + 32] {};
		if (const auto integer = std::get_if<std::int64_t>(&storage)) { const auto converted = std::to_chars(std::begin(buffer), std::end(buffer), *integer); return converted.ec == std::errc{} && Append(output, std::string_view(buffer, converted.ptr - buffer), maximum); }
		if (const auto number = std::get_if<double>(&storage)) { if (!std::isfinite(*number)) return false; const auto converted = std::to_chars(std::begin(buffer), std::end(buffer), *number, std::chars_format::general); return converted.ec == std::errc{} && Append(output, std::string_view(buffer, converted.ptr - buffer), maximum); }
		if (const auto string = std::get_if<std::wstring>(&storage)) return AppendQuoted(output, *string, maximum);
		if (const auto array = std::get_if<ConfigurationValue::Array>(&storage)) {
			if (!Append(output, "[", maximum)) return false;
			for (std::size_t index = 0; index < array->size(); ++index) { if ((index && !Append(output, ",", maximum)) || !SerializeValue((*array)[index], output, maximum)) return false; }
			return Append(output, "]", maximum);
		}
		const auto& object = std::get<ConfigurationValue::Object>(storage);
		if (!Append(output, "{", maximum)) return false;
		for (auto iterator = object.begin(); iterator != object.end(); ++iterator) {
			if ((iterator != object.begin() && !Append(output, ",", maximum)) || !AppendQuoted(output, iterator->first, maximum) || !Append(output, ":", maximum) || !SerializeValue(iterator->second, output, maximum)) return false;
		}
		return Append(output, "}", maximum);
	}

	platform::filesystem::IFileService& m_fileService;
};

} // namespace config::editing

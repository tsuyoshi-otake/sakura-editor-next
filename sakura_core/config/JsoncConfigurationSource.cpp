/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#include "StdAfx.h"

#include "config/JsoncConfigurationSource.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <utility>

namespace config {
namespace {

EJsoncConfigurationDiagnosticCode MapDiagnostic(platform::serialization::EJsoncDiagnosticCode code) noexcept
{
	using Input = platform::serialization::EJsoncDiagnosticCode;
	switch (code) {
	case Input::None: return EJsoncConfigurationDiagnosticCode::None;
	case Input::InputTooLarge: return EJsoncConfigurationDiagnosticCode::InputTooLarge;
	case Input::InvalidUtf8: return EJsoncConfigurationDiagnosticCode::InvalidUtf8;
	case Input::UnexpectedToken: return EJsoncConfigurationDiagnosticCode::UnexpectedToken;
	case Input::UnexpectedEndOfInput: return EJsoncConfigurationDiagnosticCode::UnexpectedEndOfInput;
	case Input::InvalidEscape: return EJsoncConfigurationDiagnosticCode::InvalidEscape;
	case Input::InvalidNumber: return EJsoncConfigurationDiagnosticCode::InvalidNumber;
	case Input::DuplicateKey: return EJsoncConfigurationDiagnosticCode::DuplicateKey;
	case Input::MaximumDepthExceeded: return EJsoncConfigurationDiagnosticCode::MaximumDepthExceeded;
	case Input::MaximumNodesExceeded: return EJsoncConfigurationDiagnosticCode::MaximumNodesExceeded;
	case Input::MaximumStringLengthExceeded: return EJsoncConfigurationDiagnosticCode::MaximumStringLengthExceeded;
	case Input::MaximumKeyLengthExceeded: return EJsoncConfigurationDiagnosticCode::MaximumKeyLengthExceeded;
	}
	return EJsoncConfigurationDiagnosticCode::UnexpectedToken;
}

JsoncConfigurationParseResult Failure(EJsoncConfigurationDiagnosticCode code, std::string message)
{
	JsoncConfigurationParseResult result;
	result.diagnostic = JsoncConfigurationDiagnostic { code, 0, std::move(message) };
	return result;
}

JsoncConfigurationParseResult Failure(const platform::serialization::JsoncDiagnostic& diagnostic)
{
	JsoncConfigurationParseResult result;
	result.diagnostic = JsoncConfigurationDiagnostic { MapDiagnostic(diagnostic.code), diagnostic.byteOffset, diagnostic.message };
	return result;
}

ConfigurationValue ToConfigurationValue(const platform::serialization::JsoncValue& value)
{
	using JsoncStorage = platform::serialization::JsoncValue::Storage;
	const auto& storage = value.Value();
	if (std::holds_alternative<std::monostate>(storage)) return ConfigurationValue(nullptr);
	if (const auto* boolean = std::get_if<bool>(&storage)) return ConfigurationValue(*boolean);
	if (const auto* integer = std::get_if<std::int64_t>(&storage)) return ConfigurationValue(*integer);
	if (const auto* number = std::get_if<double>(&storage)) return ConfigurationValue(*number);
	if (const auto* string = std::get_if<std::wstring>(&storage)) return ConfigurationValue(*string);
	if (const auto* array = std::get_if<platform::serialization::JsoncValue::Array>(&storage)) {
		ConfigurationValue::Array result;
		result.reserve(array->size());
		for (const auto& item : *array) result.emplace_back(ToConfigurationValue(item));
		return ConfigurationValue(std::move(result));
	}
	const auto& object = std::get<platform::serialization::JsoncValue::Object>(storage);
	ConfigurationValue::Object result;
	for (const auto& [key, item] : object) result.emplace(key, ToConfigurationValue(item));
	return ConfigurationValue(std::move(result));
}

bool ParseLanguageOverrideKey(const std::wstring& key, std::vector<std::wstring>& languageIds)
{
	if (key.empty() || key.front() != L'[') return false;
	for (std::size_t position = 0; position < key.size();) {
		if (key[position] != L'[') return false;
		const auto closing = key.find(L']', position + 1);
		if (closing == std::wstring::npos || closing == position + 1) return false;
		auto languageId = key.substr(position + 1, closing - position - 1);
		if (languageId.find(L'[') != std::wstring::npos || languageId.find(L']') != std::wstring::npos) return false;
		languageIds.emplace_back(std::move(languageId));
		position = closing + 1;
	}
	return !languageIds.empty();
}

std::string LanguageSourceId(const std::string& baseSourceId, const std::wstring& languageId)
{
	std::string encoded;
	for (std::size_t index = 0; index < languageId.size(); ++index) {
		auto code = static_cast<std::uint32_t>(languageId[index]);
		if (code >= 0xd800U && code <= 0xdbffU && index + 1 < languageId.size()) {
			const auto low = static_cast<std::uint32_t>(languageId[index + 1]);
			if (low >= 0xdc00U && low <= 0xdfffU) { code = 0x10000U + ((code - 0xd800U) << 10U) + (low - 0xdc00U); ++index; }
		}
		if (code <= 0x7fU) encoded.push_back(static_cast<char>(code));
		else if (code <= 0x7ffU) { encoded.push_back(static_cast<char>(0xc0U | (code >> 6U))); encoded.push_back(static_cast<char>(0x80U | (code & 0x3fU))); }
		else if (code <= 0xffffU) { encoded.push_back(static_cast<char>(0xe0U | (code >> 12U))); encoded.push_back(static_cast<char>(0x80U | ((code >> 6U) & 0x3fU))); encoded.push_back(static_cast<char>(0x80U | (code & 0x3fU))); }
		else { encoded.push_back(static_cast<char>(0xf0U | (code >> 18U))); encoded.push_back(static_cast<char>(0x80U | ((code >> 12U) & 0x3fU))); encoded.push_back(static_cast<char>(0x80U | ((code >> 6U) & 0x3fU))); encoded.push_back(static_cast<char>(0x80U | (code & 0x3fU))); }
	}
	return baseSourceId + ":language:" + std::to_string(encoded.size()) + ":" + encoded;
}

bool IsAsciiConfigurationKey(const std::wstring& key) noexcept
{
	return key.size() <= std::numeric_limits<std::string::size_type>::max()
		&& std::all_of(key.begin(), key.end(), [](wchar_t character) { return character <= 0x7f; });
}

std::string NarrowAscii(const std::wstring& value)
{
	std::string narrowed;
	narrowed.reserve(value.size());
	for (const auto character : value) narrowed.push_back(static_cast<char>(character));
	return narrowed;
}

} // namespace

JsoncConfigurationParseResult CJsoncConfigurationSource::Parse(
	std::string_view utf8, const ConfigurationSource& source, std::string operationId,
	std::optional<JsoncConfigurationSourceRevisions> revisions)
{
	auto parsed = platform::serialization::CJsoncDocument::Parse(utf8);
	if (!parsed.Succeeded()) return parsed.diagnostic ? Failure(*parsed.diagnostic)
		: Failure(EJsoncConfigurationDiagnosticCode::UnexpectedToken, "JSONC parsing failed");
	const auto* object = std::get_if<platform::serialization::JsoncValue::Object>(&parsed.value->Value());
	if (!object) return Failure(EJsoncConfigurationDiagnosticCode::RootMustBeObject, "configuration document root must be an object");
	return ParseObject(*object, source, std::move(operationId), std::move(revisions));
}

JsoncConfigurationParseResult CJsoncConfigurationSource::ParseObject(
	const platform::serialization::JsoncValue::Object& object, const ConfigurationSource& source, std::string operationId,
	std::optional<JsoncConfigurationSourceRevisions> revisions)
{
	JsoncConfigurationParseResult result;
	ConfigurationSourceReplacement base { source, {}, revisions ? std::optional<std::uint64_t>(revisions->baseRevision) : std::nullopt };
	std::map<std::wstring, bool, std::less<>> languageReplacement;
	for (const auto& [key, value] : object) {
		std::vector<std::wstring> languageIds;
		if (!ParseLanguageOverrideKey(key, languageIds)) {
			if (!key.empty() && key.front() == L'[') return Failure(EJsoncConfigurationDiagnosticCode::InvalidLanguageOverride, "invalid combined language override selector");
			if (!IsAsciiConfigurationKey(key)) return Failure(EJsoncConfigurationDiagnosticCode::UnexpectedToken, "configuration keys must be canonical ASCII identifiers");
			base.entries.push_back({ NarrowAscii(key), ToConfigurationValue(value) });
			continue;
		}
		const auto* overrideObject = std::get_if<platform::serialization::JsoncValue::Object>(&value.Value());
		if (!overrideObject) return Failure(EJsoncConfigurationDiagnosticCode::InvalidLanguageOverride, "language override value must be an object");
		if (source.target.profileId.empty()) return Failure(EJsoncConfigurationDiagnosticCode::InvalidLanguageOverride, "language overrides require a profile-scoped source target");
		std::vector<ConfigurationEntry> entries;
		for (const auto& [overrideKey, overrideValue] : *overrideObject) {
			if (!IsAsciiConfigurationKey(overrideKey)) return Failure(EJsoncConfigurationDiagnosticCode::UnexpectedToken, "configuration keys must be canonical ASCII identifiers");
			entries.push_back({ NarrowAscii(overrideKey), ToConfigurationValue(overrideValue) });
		}
		for (const auto& languageId : languageIds) {
			if (!languageReplacement.emplace(languageId, true).second) return Failure(EJsoncConfigurationDiagnosticCode::DuplicateKey, "language override has overlapping contributions");
			ConfigurationSource overrideSource = source;
			overrideSource.scope = EConfigurationScope::LanguageOverride;
			overrideSource.target.languageId = languageId;
			overrideSource.sourceId = LanguageSourceId(source.sourceId, languageId);
			const auto expected = revisions ? std::optional<std::uint64_t>([&]() { const auto found = revisions->languageRevisions.find(languageId); return found == revisions->languageRevisions.end() ? std::uint64_t {} : found->second; }()) : std::nullopt;
			result.replacements.push_back({ std::move(overrideSource), entries, expected });
		}
	}
	if (revisions) for (const auto& [languageId, revision] : revisions->languageRevisions) {
		if (languageReplacement.find(languageId) != languageReplacement.end()) continue;
		if (source.target.profileId.empty()) return Failure(EJsoncConfigurationDiagnosticCode::InvalidLanguageOverride, "tracked language overrides require a profile-scoped source target");
		ConfigurationSource overrideSource = source;
		overrideSource.scope = EConfigurationScope::LanguageOverride;
		overrideSource.target.languageId = languageId;
		overrideSource.sourceId = LanguageSourceId(source.sourceId, languageId);
		result.replacements.push_back({ std::move(overrideSource), {}, revision });
	}
	result.replacements.insert(result.replacements.begin(), std::move(base));
	result.operationId = std::move(operationId);
	return result;
}

JsoncConfigurationApplyResult CJsoncConfigurationSource::Apply(
	IConfigurationService& service, std::string_view utf8, const ConfigurationSource& source, std::string operationId,
	std::optional<JsoncConfigurationSourceRevisions> revisions)
{
	auto parsed = Parse(utf8, source, std::move(operationId), std::move(revisions));
	if (!parsed.Succeeded()) return { std::move(parsed.diagnostic), {} };
	return { std::nullopt, service.ReplaceSources({ std::move(parsed.replacements), std::move(parsed.operationId) }) };
}

JsoncConfigurationApplyResult CJsoncConfigurationSource::ApplyObject(
	IConfigurationService& service, const platform::serialization::JsoncValue::Object& object,
	const ConfigurationSource& source, std::string operationId,
	std::optional<JsoncConfigurationSourceRevisions> revisions)
{
	auto parsed = ParseObject(object, source, std::move(operationId), std::move(revisions));
	if (!parsed.Succeeded()) return { std::move(parsed.diagnostic), {} };
	return { std::nullopt, service.ReplaceSources({ std::move(parsed.replacements), std::move(parsed.operationId) }) };
}

} // namespace config

/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"

#include "config/WorkspaceTrustMementoCodec.h"

#include <sakura/storage/StorageTypes.h>

#include <picojson/picojson.h>

#include <cmath>
#include <cstdint>
#include <utility>

namespace config {
namespace {

using JsonObject = picojson::object;
using JsonValue = picojson::value;

//! The document is one flat object. Anything nested is not this schema.
constexpr std::size_t kMaximumWorkspaceTrustMementoJsonDepth = 2;

WorkspaceTrustMementoEncodeResult EncodeFailure(
	EWorkspaceTrustMementoCodecStatus status, std::string diagnostic)
{
	return { status, {}, std::move(diagnostic) };
}

WorkspaceTrustMementoDecodeResult DecodeFailure(
	EWorkspaceTrustMementoCodecStatus status, std::string diagnostic)
{
	return { status, std::nullopt, std::move(diagnostic) };
}

const JsonValue* Find(const JsonObject& object, std::string_view key) noexcept
{
	const auto found = object.find(std::string(key));
	return found == object.end() ? nullptr : &found->second;
}

bool IsJsonWhitespace(char character) noexcept
{
	return character == ' ' || character == '\t' || character == '\r' || character == '\n';
}

bool HasBoundedJsonNesting(std::string_view payload) noexcept
{
	std::size_t depth = 0;
	bool inString = false;
	bool escaped = false;
	for (const char character : payload) {
		if (inString) {
			if (escaped) {
				escaped = false;
			} else if (character == '\\') {
				escaped = true;
			} else if (character == '"') {
				inString = false;
			}
			continue;
		}
		if (character == '"') {
			inString = true;
		} else if (character == '{' || character == '[') {
			if (++depth > kMaximumWorkspaceTrustMementoJsonDepth) return false;
		} else if (character == '}' || character == ']') {
			if (depth == 0) return false;
			--depth;
		}
	}
	return !inString && !escaped && depth == 0;
}

/*!
	@brief Read one optional boolean flag.

	Absent leaves @p output at its ask-again default; present-but-not-a-boolean is
	reported so the caller can reject the whole document.
 */
bool ReadOptionalFlag(const JsonObject& root, std::string_view key, bool& output) noexcept
{
	const auto* value = Find(root, key);
	if (value == nullptr) return true;
	if (!value->is<bool>()) return false;
	output = value->get<bool>();
	return true;
}

} // namespace

WorkspaceTrustMementoEncodeResult CWorkspaceTrustMementoCodec::Encode(
	const WorkspaceTrustMemento& memento) noexcept
{
	try {
		JsonObject root;
		root["formatVersion"] = JsonValue(static_cast<double>(kWorkspaceTrustMementoFormatVersion));
		root["startupPromptShown"] = JsonValue(memento.startupPromptShown);
		root["untrustedFilesAccepted"] = JsonValue(memento.untrustedFilesAccepted);
		auto payload = JsonValue(std::move(root)).serialize(false);
		if (payload.size() > platform::storage::kMaximumStorageMutationPayloadBytes) {
			return EncodeFailure(EWorkspaceTrustMementoCodecStatus::PayloadTooLarge,
				"workspace trust memento payload exceeds the storage mutation limit");
		}
		return { EWorkspaceTrustMementoCodecStatus::Succeeded, std::move(payload), {} };
	} catch (...) {
		return EncodeFailure(EWorkspaceTrustMementoCodecStatus::CorruptPayload,
			"workspace trust memento encoding failed");
	}
}

WorkspaceTrustMementoDecodeResult CWorkspaceTrustMementoCodec::Decode(std::string_view payload) noexcept
{
	try {
		if (payload.size() > platform::storage::kMaximumStorageMutationPayloadBytes) {
			return DecodeFailure(EWorkspaceTrustMementoCodecStatus::PayloadTooLarge,
				"workspace trust memento payload exceeds the storage mutation limit");
		}
		if (!platform::storage::IsValidStorageUtf8(payload, false) || !HasBoundedJsonNesting(payload)) {
			return DecodeFailure(EWorkspaceTrustMementoCodecStatus::CorruptPayload,
				"workspace trust memento payload is not bounded valid UTF-8 JSON");
		}

		JsonValue rootValue;
		std::string parseError;
		auto position = picojson::parse(rootValue, payload.begin(), payload.end(), &parseError);
		while (position != payload.end() && IsJsonWhitespace(*position)) ++position;
		if (!parseError.empty() || position != payload.end() || !rootValue.is<JsonObject>()) {
			return DecodeFailure(EWorkspaceTrustMementoCodecStatus::CorruptPayload,
				"workspace trust memento JSON is malformed");
		}
		const auto& root = rootValue.get<JsonObject>();

		const auto* formatVersionValue = Find(root, "formatVersion");
		if (formatVersionValue == nullptr || !formatVersionValue->is<double>()) {
			return DecodeFailure(EWorkspaceTrustMementoCodecStatus::UnsupportedSchema,
				"workspace trust memento formatVersion is missing or not numeric");
		}
		const double formatVersionNumber = formatVersionValue->get<double>();
		if (!std::isfinite(formatVersionNumber) || formatVersionNumber < 0.0
			|| formatVersionNumber > static_cast<double>(UINT32_MAX)
			|| std::floor(formatVersionNumber) != formatVersionNumber) {
			return DecodeFailure(EWorkspaceTrustMementoCodecStatus::UnsupportedSchema,
				"workspace trust memento formatVersion is not a valid unsigned integer");
		}
		if (static_cast<std::uint32_t>(formatVersionNumber) != kWorkspaceTrustMementoFormatVersion) {
			return DecodeFailure(EWorkspaceTrustMementoCodecStatus::UnsupportedSchema,
				"workspace trust memento formatVersion is unsupported");
		}

		WorkspaceTrustMemento memento;
		if (!ReadOptionalFlag(root, "startupPromptShown", memento.startupPromptShown)) {
			return DecodeFailure(EWorkspaceTrustMementoCodecStatus::CorruptPayload,
				"workspace trust memento startupPromptShown is not a boolean");
		}
		if (!ReadOptionalFlag(root, "untrustedFilesAccepted", memento.untrustedFilesAccepted)) {
			return DecodeFailure(EWorkspaceTrustMementoCodecStatus::CorruptPayload,
				"workspace trust memento untrustedFilesAccepted is not a boolean");
		}

		return { EWorkspaceTrustMementoCodecStatus::Succeeded, memento, {} };
	} catch (...) {
		return DecodeFailure(EWorkspaceTrustMementoCodecStatus::CorruptPayload,
			"workspace trust memento decoding failed");
	}
}

} // namespace config

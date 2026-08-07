/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"

#include "config/TrustedFoldersCodec.h"

#include <sakura/storage/StorageTypes.h>
#include <sakura/uri/UriIdentity.h>

#include <picojson/picojson.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>

namespace config {
namespace {

using JsonArray = picojson::array;
using JsonObject = picojson::object;
using JsonValue = picojson::value;

//! The document is at most root-object -> "entries" array -> entry object deep. A
//! generous bound still rejects a maliciously deep payload before picojson's
//! recursive-descent parser is asked to walk it.
constexpr std::size_t kMaximumTrustedFoldersJsonDepth = 8;

TrustedFoldersEncodeResult EncodeFailure(ETrustedFoldersCodecStatus status, std::string diagnostic)
{
	return { status, {}, std::move(diagnostic) };
}

TrustedFoldersDecodeResult DecodeFailure(ETrustedFoldersCodecStatus status, std::string diagnostic)
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
			if (++depth > kMaximumTrustedFoldersJsonDepth) return false;
		} else if (character == '}' || character == ']') {
			if (depth == 0) return false;
			--depth;
		}
	}
	return !inString && !escaped && depth == 0;
}

//! Structural + bounds validation for a URI already converted to UTF-8. Never
//! inspects the original wide text and never appears in a diagnostic string.
bool IsValidUriUtf8(std::string_view value) noexcept
{
	if (value.empty() || value.size() > kMaximumTrustedFolderUriBytes) return false;
	if (!platform::storage::IsValidStorageUtf8(value, false)) return false;
	return std::none_of(value.begin(), value.end(), [](char character) {
		const auto byte = static_cast<unsigned char>(character);
		return byte < 0x20 || byte == 0x7f;
	});
}

bool EncodeUriUtf8(const platform::uri::Uri& uri, std::string& output) noexcept
{
	output.clear();
	try {
		const std::wstring text = uri.ToString();
		if (text.empty() || text.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
			return false;
		}
		const int required = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
			static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
		if (required <= 0) return false;
		output.resize(static_cast<std::size_t>(required));
		if (::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
				static_cast<int>(text.size()), output.data(), required, nullptr, nullptr) != required) {
			return false;
		}
	} catch (...) {
		return false;
	}
	return IsValidUriUtf8(output);
}

bool DecodeUriUtf8(std::string_view value, std::wstring& output) noexcept
{
	output.clear();
	if (value.empty() || value.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
		return false;
	}
	try {
		const int required = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
			static_cast<int>(value.size()), nullptr, 0);
		if (required <= 0) return false;
		output.resize(static_cast<std::size_t>(required));
		return ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
			static_cast<int>(value.size()), output.data(), required) == required;
	} catch (...) {
		return false;
	}
}

} // namespace

TrustedFoldersEncodeResult CTrustedFoldersCodec::Encode(const TrustedFoldersSnapshot& snapshot) noexcept
{
	try {
		if (snapshot.entries.size() > kMaximumTrustedFolderEntries) {
			return EncodeFailure(ETrustedFoldersCodecStatus::InvalidSnapshot,
				"entries exceeds the maximum count");
		}

		JsonArray encodedEntries;
		encodedEntries.reserve(snapshot.entries.size());
		for (const auto& entry : snapshot.entries) {
			std::string uriUtf8;
			if (!EncodeUriUtf8(entry.uri, uriUtf8)) {
				return EncodeFailure(ETrustedFoldersCodecStatus::InvalidSnapshot,
					"uri failed to encode as bounded valid UTF-8");
			}
			JsonObject encodedEntry;
			encodedEntry["includesDescendants"] = JsonValue(entry.includesDescendants);
			encodedEntry["uri"] = JsonValue(std::move(uriUtf8));
			encodedEntries.emplace_back(std::move(encodedEntry));
		}

		JsonObject root;
		root["entries"] = JsonValue(std::move(encodedEntries));
		root["formatVersion"] = JsonValue(static_cast<double>(kTrustedFoldersFormatVersion));
		auto payload = JsonValue(std::move(root)).serialize(false);
		if (payload.size() > platform::storage::kMaximumStorageMutationPayloadBytes) {
			return EncodeFailure(ETrustedFoldersCodecStatus::PayloadTooLarge,
				"trusted folders payload exceeds the storage mutation limit");
		}
		return { ETrustedFoldersCodecStatus::Succeeded, std::move(payload), {} };
	} catch (...) {
		return EncodeFailure(ETrustedFoldersCodecStatus::InvalidSnapshot,
			"trusted folders encoding failed");
	}
}

TrustedFoldersDecodeResult CTrustedFoldersCodec::Decode(std::string_view payload) noexcept
{
	try {
		if (payload.size() > platform::storage::kMaximumStorageMutationPayloadBytes) {
			return DecodeFailure(ETrustedFoldersCodecStatus::PayloadTooLarge,
				"trusted folders payload exceeds the storage mutation limit");
		}
		if (!platform::storage::IsValidStorageUtf8(payload, false) || !HasBoundedJsonNesting(payload)) {
			return DecodeFailure(ETrustedFoldersCodecStatus::CorruptPayload,
				"trusted folders payload is not bounded valid UTF-8 JSON");
		}

		JsonValue rootValue;
		std::string parseError;
		auto position = picojson::parse(rootValue, payload.begin(), payload.end(), &parseError);
		while (position != payload.end() && IsJsonWhitespace(*position)) ++position;
		if (!parseError.empty() || position != payload.end() || !rootValue.is<JsonObject>()) {
			return DecodeFailure(ETrustedFoldersCodecStatus::CorruptPayload,
				"trusted folders JSON is malformed");
		}
		const auto& root = rootValue.get<JsonObject>();

		const auto* formatVersionValue = Find(root, "formatVersion");
		if (formatVersionValue == nullptr || !formatVersionValue->is<double>()) {
			return DecodeFailure(ETrustedFoldersCodecStatus::UnsupportedSchema,
				"trusted folders formatVersion is missing or not numeric");
		}
		const double formatVersionNumber = formatVersionValue->get<double>();
		if (!std::isfinite(formatVersionNumber) || formatVersionNumber < 0.0
			|| formatVersionNumber > static_cast<double>(UINT32_MAX)
			|| std::floor(formatVersionNumber) != formatVersionNumber) {
			return DecodeFailure(ETrustedFoldersCodecStatus::UnsupportedSchema,
				"trusted folders formatVersion is not a valid unsigned integer");
		}
		if (static_cast<std::uint32_t>(formatVersionNumber) != kTrustedFoldersFormatVersion) {
			return DecodeFailure(ETrustedFoldersCodecStatus::UnsupportedSchema,
				"trusted folders formatVersion is unsupported");
		}

		const auto* entriesValue = Find(root, "entries");
		if (entriesValue == nullptr || !entriesValue->is<JsonArray>()) {
			return DecodeFailure(ETrustedFoldersCodecStatus::CorruptPayload,
				"trusted folders entries field is invalid");
		}
		const auto& entriesArray = entriesValue->get<JsonArray>();
		if (entriesArray.size() > kMaximumTrustedFolderEntries) {
			return DecodeFailure(ETrustedFoldersCodecStatus::CorruptPayload,
				"trusted folders entries exceeds the maximum count");
		}

		TrustedFoldersSnapshot snapshot;
		snapshot.entries.reserve(entriesArray.size());
		for (const auto& value : entriesArray) {
			if (!value.is<JsonObject>()) {
				return DecodeFailure(ETrustedFoldersCodecStatus::CorruptPayload,
					"trusted folders entry is not an object");
			}
			const auto& object = value.get<JsonObject>();

			const auto* uriValue = Find(object, "uri");
			if (uriValue == nullptr || !uriValue->is<std::string>()) {
				return DecodeFailure(ETrustedFoldersCodecStatus::CorruptPayload,
					"trusted folders entry uri is not a string");
			}
			const auto& uriUtf8 = uriValue->get<std::string>();
			if (!IsValidUriUtf8(uriUtf8)) {
				return DecodeFailure(ETrustedFoldersCodecStatus::CorruptPayload,
					"trusted folders entry uri failed bounds or UTF-8 validation");
			}
			std::wstring uriText;
			if (!DecodeUriUtf8(uriUtf8, uriText)) {
				return DecodeFailure(ETrustedFoldersCodecStatus::CorruptPayload,
					"trusted folders entry uri failed to decode as UTF-8");
			}
			auto parsedUri = platform::uri::Uri::Parse(uriText);
			if (!parsedUri) {
				return DecodeFailure(ETrustedFoldersCodecStatus::CorruptPayload,
					"trusted folders entry uri failed to parse");
			}

			bool includesDescendants = false;
			const auto* includesDescendantsValue = Find(object, "includesDescendants");
			if (includesDescendantsValue != nullptr) {
				if (!includesDescendantsValue->is<bool>()) {
					return DecodeFailure(ETrustedFoldersCodecStatus::CorruptPayload,
						"trusted folders entry includesDescendants is not a boolean");
				}
				includesDescendants = includesDescendantsValue->get<bool>();
			}

			// Aggregate-initialized in place: WorkspaceTrustEntry has no default
			// constructor, because a trust entry without a resource is meaningless.
			snapshot.entries.push_back(
				WorkspaceTrustEntry{ std::move(*parsedUri.value), includesDescendants });
		}

		return { ETrustedFoldersCodecStatus::Succeeded, std::move(snapshot), {} };
	} catch (...) {
		return DecodeFailure(ETrustedFoldersCodecStatus::CorruptPayload,
			"trusted folders decoding failed");
	}
}

} // namespace config

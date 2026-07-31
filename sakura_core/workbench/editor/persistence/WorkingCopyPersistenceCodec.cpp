/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"

#include "workbench/editor/persistence/WorkingCopyPersistenceCodec.h"

#include "platform/storage/StorageTypes.h"

#include <picojson/picojson.h>

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>
#include <utility>

namespace workbench::editor::persistence {
namespace {

using JsonArray = picojson::array;
using JsonObject = picojson::object;
using JsonValue = picojson::value;

constexpr std::size_t kMaximumPayloadBytes = platform::storage::kMaximumStorageSnapshotPayloadBytes;
constexpr std::size_t kMaximumJsonDepth = 16;

bool IsValidUtf8(std::string_view value, bool allowEmpty, std::size_t maximum) noexcept
{
	if ((!allowEmpty && value.empty()) || value.size() > maximum
		|| value.find('\0') != std::string_view::npos) {
		return false;
	}
	const auto* bytes = reinterpret_cast<const unsigned char*>(value.data());
	for (std::size_t index = 0; index < value.size();) {
		const auto first = bytes[index];
		if (first <= 0x7f) {
			++index;
			continue;
		}
		std::size_t tails = 0;
		std::uint32_t point = 0;
		if ((first & 0xe0) == 0xc0) {
			tails = 1;
			point = first & 0x1f;
		} else if ((first & 0xf0) == 0xe0) {
			tails = 2;
			point = first & 0x0f;
		} else if ((first & 0xf8) == 0xf0) {
			tails = 3;
			point = first & 0x07;
		} else {
			return false;
		}
		if (index + tails >= value.size()) return false;
		for (std::size_t tail = 1; tail <= tails; ++tail) {
			const auto byte = bytes[index + tail];
			if ((byte & 0xc0) != 0x80) return false;
			point = (point << 6) | (byte & 0x3f);
		}
		const auto minimum = tails == 1 ? 0x80u : tails == 2 ? 0x800u : 0x10000u;
		if (point < minimum || point > 0x10ffff || (point >= 0xd800 && point <= 0xdfff)) {
			return false;
		}
		index += tails + 1;
	}
	return true;
}

template <class Result>
Result EncodeFailure(EWorkingCopyPersistenceCodecStatus status, std::string diagnostic)
{
	return { status, {}, std::move(diagnostic) };
}

template <class Result>
Result DecodeFailure(EWorkingCopyPersistenceCodecStatus status, std::string diagnostic)
{
	return { status, std::nullopt, std::move(diagnostic) };
}

bool IsValidUtf8Id(std::string_view value, std::size_t maximum = kMaximumWorkingCopyPersistenceIdBytes) noexcept
{
	return IsValidWorkingCopyPersistenceId(value, maximum)
		&& IsValidUtf8(value, false, maximum);
}

bool IsKnownInputType(std::string_view typeId) noexcept
{
	return typeId == CWorkingCopyPersistenceCodec::kTextInputTypeId;
}

const char* EncodingText(EWorkingCopyTextEncoding value) noexcept
{
	switch (value) {
	case EWorkingCopyTextEncoding::Utf8: return "utf8";
	case EWorkingCopyTextEncoding::Utf8WithBom: return "utf8bom";
	case EWorkingCopyTextEncoding::Utf16Le: return "utf16le";
	case EWorkingCopyTextEncoding::Utf16Be: return "utf16be";
	case EWorkingCopyTextEncoding::Windows1252: return "windows1252";
	case EWorkingCopyTextEncoding::Unknown: return "unknown";
	}
	return "";
}

std::optional<EWorkingCopyTextEncoding> ParseEncoding(std::string_view value) noexcept
{
	if (value == "utf8") return EWorkingCopyTextEncoding::Utf8;
	if (value == "utf8bom") return EWorkingCopyTextEncoding::Utf8WithBom;
	if (value == "utf16le") return EWorkingCopyTextEncoding::Utf16Le;
	if (value == "utf16be") return EWorkingCopyTextEncoding::Utf16Be;
	if (value == "windows1252") return EWorkingCopyTextEncoding::Windows1252;
	if (value == "unknown") return EWorkingCopyTextEncoding::Unknown;
	return std::nullopt;
}

const char* EolText(EWorkingCopyEol value) noexcept
{
	switch (value) {
	case EWorkingCopyEol::Lf: return "lf";
	case EWorkingCopyEol::CrLf: return "crlf";
	case EWorkingCopyEol::Cr: return "cr";
	case EWorkingCopyEol::Unknown: return "unknown";
	}
	return "";
}

std::optional<EWorkingCopyEol> ParseEol(std::string_view value) noexcept
{
	if (value == "lf") return EWorkingCopyEol::Lf;
	if (value == "crlf") return EWorkingCopyEol::CrLf;
	if (value == "cr") return EWorkingCopyEol::Cr;
	if (value == "unknown") return EWorkingCopyEol::Unknown;
	return std::nullopt;
}

const JsonValue* Find(const JsonObject& object, std::string_view key) noexcept
{
	const auto found = object.find(std::string(key));
	return found == object.end() ? nullptr : &found->second;
}

bool ReadRequiredString(const JsonObject& object, std::string_view key, std::string& value,
	std::size_t maximum = kMaximumWorkingCopyPersistenceIdBytes)
{
	const auto* field = Find(object, key);
	if (field == nullptr || !field->is<std::string>()) return false;
	value = field->get<std::string>();
	return IsValidUtf8Id(value, maximum);
}

bool ReadRequiredUtf8String(const JsonObject& object, std::string_view key, std::string& value,
	std::size_t maximum)
{
	const auto* field = Find(object, key);
	if (field == nullptr || !field->is<std::string>()) return false;
	value = field->get<std::string>();
	return IsValidUtf8(value, true, maximum);
}

bool ReadRequiredBoolean(const JsonObject& object, std::string_view key, bool& value) noexcept
{
	const auto* field = Find(object, key);
	if (field == nullptr || !field->is<bool>()) return false;
	value = field->get<bool>();
	return true;
}

bool ReadUnsigned(const JsonValue& field, std::uint64_t& value) noexcept
{
	if (!field.is<double>()) return false;
	const auto number = field.get<double>();
	if (!std::isfinite(number) || number < 0.0
		|| number > static_cast<double>(kMaximumWorkingCopyPersistenceGeneration)
		|| std::floor(number) != number) {
		return false;
	}
	value = static_cast<std::uint64_t>(number);
	return true;
}

bool ReadRequiredUnsigned(const JsonObject& object, std::string_view key, std::uint64_t& value) noexcept
{
	const auto* field = Find(object, key);
	return field != nullptr && ReadUnsigned(*field, value);
}

bool ReadRequiredUInt32(const JsonObject& object, std::string_view key, std::uint32_t& value) noexcept
{
	std::uint64_t raw = 0;
	if (!ReadRequiredUnsigned(object, key, raw) || raw > (std::numeric_limits<std::uint32_t>::max)()) return false;
	value = static_cast<std::uint32_t>(raw);
	return true;
}

bool ReadOptionalUnsigned(const JsonObject& object, std::string_view key, std::optional<std::uint64_t>& value) noexcept
{
	const auto* field = Find(object, key);
	if (field == nullptr || field->is<picojson::null>()) {
		value.reset();
		return true;
	}
	std::uint64_t result = 0;
	if (!ReadUnsigned(*field, result)) return false;
	value = result;
	return true;
}

bool ReadOptionalString(const JsonObject& object, std::string_view key, std::optional<std::string>& value,
	std::size_t maximum = kMaximumWorkingCopyPersistenceIdBytes)
{
	const auto* field = Find(object, key);
	if (field == nullptr || field->is<picojson::null>()) {
		value.reset();
		return true;
	}
	if (!field->is<std::string>() || !IsValidUtf8Id(field->get<std::string>(), maximum)) return false;
	value = field->get<std::string>();
	return true;
}

bool HasBoundedJsonNesting(std::string_view payload) noexcept
{
	std::size_t depth = 0;
	bool inString = false;
	bool escaped = false;
	for (const char character : payload) {
		if (inString) {
			if (escaped) escaped = false;
			else if (character == '\\') escaped = true;
			else if (character == '"') inString = false;
			continue;
		}
		if (character == '"') inString = true;
		else if (character == '{' || character == '[') {
			if (++depth > kMaximumJsonDepth) return false;
		} else if (character == '}' || character == ']') {
			if (depth == 0) return false;
			--depth;
		}
	}
	return !inString && !escaped && depth == 0;
}

bool IsJsonWhitespace(char character) noexcept
{
	return character == ' ' || character == '\t' || character == '\r' || character == '\n';
}

bool ParseRoot(std::string_view payload, JsonObject& root)
{
	if (!IsValidUtf8(payload, false, kMaximumPayloadBytes)
		|| !HasBoundedJsonNesting(payload)) return false;
	JsonValue rootValue;
	std::string parseError;
	auto position = picojson::parse(rootValue, payload.begin(), payload.end(), &parseError);
	while (position != payload.end() && IsJsonWhitespace(*position)) ++position;
	if (!parseError.empty() || position != payload.end() || !rootValue.is<JsonObject>()) return false;
	root = rootValue.get<JsonObject>();
	return true;
}

JsonObject EncodeScope(const WorkingCopyPersistenceScope& scope)
{
	JsonObject result;
	result["profileId"] = JsonValue(scope.profileId);
	if (scope.workspaceId) result["workspaceId"] = JsonValue(*scope.workspaceId);
	return result;
}

JsonObject EncodeIdentity(const WorkingCopyPersistenceIdentity& identity)
{
	JsonObject result;
	if (identity.canonicalResource) result["resource"] = JsonValue(*identity.canonicalResource);
	if (identity.opaqueId) result["opaqueId"] = JsonValue(*identity.opaqueId);
	result["typeId"] = JsonValue(identity.typeId);
	return result;
}

bool ReadScope(const JsonValue* field, WorkingCopyPersistenceScope& scope)
{
	if (field == nullptr || !field->is<JsonObject>()) return false;
	const auto& object = field->get<JsonObject>();
	return ReadRequiredString(object, "profileId", scope.profileId)
		&& ReadOptionalString(object, "workspaceId", scope.workspaceId) && scope.IsValid();
}

bool ReadIdentity(const JsonValue* field, WorkingCopyPersistenceIdentity& identity)
{
	if (field == nullptr || !field->is<JsonObject>()) return false;
	const auto& object = field->get<JsonObject>();
	return ReadRequiredString(object, "typeId", identity.typeId)
		&& ReadOptionalString(object, "resource", identity.canonicalResource, kMaximumWorkingCopyPersistenceResourceBytes)
		&& ReadOptionalString(object, "opaqueId", identity.opaqueId)
		&& identity.IsValid();
}

bool HasUniqueInputIds(const std::vector<EditorSessionInputDescriptor>& inputs)
{
	std::set<std::string, std::less<>> ids;
	for (const auto& input : inputs) {
		if (!input.IsValid() || !ids.emplace(input.inputId).second) return false;
	}
	return true;
}

bool IsChecksumText(std::string_view value) noexcept
{
	if (value.size() != 16) return false;
	return std::all_of(value.begin(), value.end(), [](char character) {
		return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
	});
}

} // namespace

bool IsValidWorkingCopyPersistenceUtf8(
	std::string_view value, bool allowEmpty, std::size_t maximumLength) noexcept
{
	return IsValidUtf8(value, allowEmpty, maximumLength);
}

bool WorkingCopyPersistenceScope::IsValid() const noexcept
{
	return IsValidUtf8Id(profileId) && (!workspaceId || IsValidUtf8Id(*workspaceId));
}

bool WorkingCopyPersistenceIdentity::IsValid() const noexcept
{
	return IsValidUtf8Id(typeId) && (canonicalResource.has_value() != opaqueId.has_value())
		&& (!canonicalResource || IsValidUtf8Id(*canonicalResource, kMaximumWorkingCopyPersistenceResourceBytes))
		&& (!opaqueId || IsValidUtf8Id(*opaqueId));
}

bool WorkingCopyBackup::IsValid() const noexcept
{
	return scope.IsValid() && identity.IsValid() && dirty && generation != 0 && generation <= kMaximumWorkingCopyPersistenceGeneration
		&& contentVersion != 0 && contentVersion <= kMaximumWorkingCopyPersistenceGeneration
		&& *EncodingText(encoding) != '\0' && *EolText(eol) != '\0' && IsChecksumText(checksum)
		&& IsValidUtf8(content, true, kMaximumWorkingCopyPersistenceContentBytes)
		&& checksum == CWorkingCopyPersistenceCodec::ComputeContentChecksum(content);
}

bool EditorSessionInputDescriptor::IsValid() const noexcept
{
	return IsValidUtf8Id(inputId) && IsValidUtf8Id(inputTypeId) && workingCopyIdentity.IsValid() && stateVersion != 0
		&& (!backupGeneration || (*backupGeneration != 0 && *backupGeneration <= kMaximumWorkingCopyPersistenceGeneration))
		&& IsValidUtf8(state, true, kMaximumEditorSessionUntypedStateBytes);
}

bool EditorSessionManifest::IsValid() const noexcept
{
	if (!scope.IsValid() || generation == 0 || generation > kMaximumWorkingCopyPersistenceGeneration || !IsValidUtf8Id(logicalGroupId)
		|| inputs.size() > kMaximumEditorSessionInputs || !HasUniqueInputIds(inputs)
		|| !std::ranges::all_of(inputs, [](const EditorSessionInputDescriptor& input) { return input.IsValid(); })) return false;
	if (!activeInputId) return true;
	return IsValidUtf8Id(*activeInputId) && std::ranges::any_of(inputs,
		[this](const EditorSessionInputDescriptor& input) { return input.inputId == *activeInputId; });
}

std::string CWorkingCopyPersistenceCodec::ComputeContentChecksum(std::string_view content) noexcept
{
	try {
		std::uint64_t value = UINT64_C(14695981039346656037);
		for (const unsigned char byte : content) {
			value ^= byte;
			value *= UINT64_C(1099511628211);
		}
		char result[17]{};
		constexpr char kHex[] = "0123456789abcdef";
		for (std::size_t index = 0; index < 16; ++index) {
			const auto shift = static_cast<unsigned>((15 - index) * 4);
			result[index] = kHex[(value >> shift) & 0x0fU];
		}
		return std::string(result, 16);
	} catch (...) {
		return {};
	}
}

WorkingCopyBackupEncodeResult CWorkingCopyPersistenceCodec::EncodeBackup(const WorkingCopyBackup& backup) noexcept
{
	try {
		if (!backup.IsValid()) return EncodeFailure<WorkingCopyBackupEncodeResult>(
			EWorkingCopyPersistenceCodecStatus::InvalidRecord, "working-copy backup validation failed");
		JsonObject root;
		root["checksum"] = JsonValue(backup.checksum);
		root["content"] = JsonValue(backup.content);
		root["contentVersion"] = JsonValue(static_cast<double>(backup.contentVersion));
		root["dirty"] = JsonValue(backup.dirty);
		root["encoding"] = JsonValue(EncodingText(backup.encoding));
		root["eol"] = JsonValue(EolText(backup.eol));
		root["formatVersion"] = JsonValue(static_cast<double>(kWorkingCopyBackupFormatVersion));
		root["generation"] = JsonValue(static_cast<double>(backup.generation));
		root["identity"] = JsonValue(EncodeIdentity(backup.identity));
		root["scope"] = JsonValue(EncodeScope(backup.scope));
		auto payload = JsonValue(std::move(root)).serialize(false);
		if (payload.size() > kMaximumPayloadBytes) return EncodeFailure<WorkingCopyBackupEncodeResult>(
			EWorkingCopyPersistenceCodecStatus::PayloadTooLarge, "working-copy backup exceeds the persistence payload limit");
		return { EWorkingCopyPersistenceCodecStatus::Succeeded, std::move(payload), {} };
	} catch (...) {
		return EncodeFailure<WorkingCopyBackupEncodeResult>(
			EWorkingCopyPersistenceCodecStatus::InvalidRecord, "working-copy backup encoding failed");
	}
}

WorkingCopyBackupDecodeResult CWorkingCopyPersistenceCodec::DecodeBackup(std::string_view payload) noexcept
{
	try {
		if (payload.size() > kMaximumPayloadBytes) return DecodeFailure<WorkingCopyBackupDecodeResult>(
			EWorkingCopyPersistenceCodecStatus::PayloadTooLarge, "working-copy backup exceeds the persistence payload limit");
		JsonObject root;
		if (!ParseRoot(payload, root)) return DecodeFailure<WorkingCopyBackupDecodeResult>(
			EWorkingCopyPersistenceCodecStatus::CorruptPayload, "working-copy backup JSON is malformed");
		std::uint32_t version = 0;
		if (!ReadRequiredUInt32(root, "formatVersion", version)) return DecodeFailure<WorkingCopyBackupDecodeResult>(
			EWorkingCopyPersistenceCodecStatus::CorruptPayload, "working-copy backup format version is invalid");
		if (version != kWorkingCopyBackupFormatVersion) return DecodeFailure<WorkingCopyBackupDecodeResult>(
			EWorkingCopyPersistenceCodecStatus::UnsupportedSchema, "working-copy backup format version is unsupported");

		WorkingCopyBackup backup;
		std::string encoding;
		std::string eol;
		if (!ReadScope(Find(root, "scope"), backup.scope) || !ReadIdentity(Find(root, "identity"), backup.identity)
			|| !ReadRequiredUnsigned(root, "generation", backup.generation)
			|| !ReadRequiredUnsigned(root, "contentVersion", backup.contentVersion)
			|| !ReadRequiredString(root, "checksum", backup.checksum, 16)
			|| !ReadRequiredString(root, "encoding", encoding, 32)
			|| !ReadRequiredString(root, "eol", eol, 32)
			|| !ReadRequiredBoolean(root, "dirty", backup.dirty)) return DecodeFailure<WorkingCopyBackupDecodeResult>(
				EWorkingCopyPersistenceCodecStatus::CorruptPayload, "working-copy backup metadata is invalid");
		const auto* content = Find(root, "content");
		if (content == nullptr || !content->is<std::string>()
			|| !IsValidUtf8(content->get<std::string>(), true, kMaximumWorkingCopyPersistenceContentBytes)) return DecodeFailure<WorkingCopyBackupDecodeResult>(
				EWorkingCopyPersistenceCodecStatus::CorruptPayload, "working-copy backup content is invalid");
		backup.content = content->get<std::string>();
		const auto parsedEncoding = ParseEncoding(encoding);
		const auto parsedEol = ParseEol(eol);
		if (!parsedEncoding || !parsedEol || !IsChecksumText(backup.checksum)) return DecodeFailure<WorkingCopyBackupDecodeResult>(
			EWorkingCopyPersistenceCodecStatus::CorruptPayload, "working-copy backup text metadata is invalid");
		backup.encoding = *parsedEncoding;
		backup.eol = *parsedEol;
		if (backup.checksum != ComputeContentChecksum(backup.content)) return DecodeFailure<WorkingCopyBackupDecodeResult>(
			EWorkingCopyPersistenceCodecStatus::ChecksumMismatch, "working-copy backup checksum does not match content");
		if (!backup.IsValid()) return DecodeFailure<WorkingCopyBackupDecodeResult>(
			EWorkingCopyPersistenceCodecStatus::CorruptPayload, "working-copy backup record is invalid");
		return { EWorkingCopyPersistenceCodecStatus::Succeeded, std::move(backup), {} };
	} catch (...) {
		return DecodeFailure<WorkingCopyBackupDecodeResult>(
			EWorkingCopyPersistenceCodecStatus::CorruptPayload, "working-copy backup decoding failed");
	}
}

EditorSessionManifestEncodeResult CWorkingCopyPersistenceCodec::EncodeSession(const EditorSessionManifest& manifest) noexcept
{
	try {
		if (!manifest.IsValid()) return EncodeFailure<EditorSessionManifestEncodeResult>(
			EWorkingCopyPersistenceCodecStatus::InvalidRecord, "editor session manifest validation failed");
		JsonArray inputs;
		inputs.reserve(manifest.inputs.size());
		for (const auto& input : manifest.inputs) {
			if (!IsKnownInputType(input.inputTypeId)) return EncodeFailure<EditorSessionManifestEncodeResult>(
				EWorkingCopyPersistenceCodecStatus::UnknownInputType, "editor session input type is not registered");
			JsonObject item;
			if (input.backupGeneration) item["backupGeneration"] = JsonValue(static_cast<double>(*input.backupGeneration));
			item["identity"] = JsonValue(EncodeIdentity(input.workingCopyIdentity));
			item["inputId"] = JsonValue(input.inputId);
			item["inputTypeId"] = JsonValue(input.inputTypeId);
			item["state"] = JsonValue(input.state);
			item["stateVersion"] = JsonValue(static_cast<double>(input.stateVersion));
			inputs.emplace_back(std::move(item));
		}
		JsonObject root;
		if (manifest.activeInputId) root["activeInputId"] = JsonValue(*manifest.activeInputId);
		root["formatVersion"] = JsonValue(static_cast<double>(kEditorSessionManifestFormatVersion));
		root["generation"] = JsonValue(static_cast<double>(manifest.generation));
		root["inputs"] = JsonValue(std::move(inputs));
		root["logicalGroupId"] = JsonValue(manifest.logicalGroupId);
		root["scope"] = JsonValue(EncodeScope(manifest.scope));
		auto payload = JsonValue(std::move(root)).serialize(false);
		if (payload.size() > kMaximumPayloadBytes) return EncodeFailure<EditorSessionManifestEncodeResult>(
			EWorkingCopyPersistenceCodecStatus::PayloadTooLarge, "editor session manifest exceeds the persistence payload limit");
		return { EWorkingCopyPersistenceCodecStatus::Succeeded, std::move(payload), {} };
	} catch (...) {
		return EncodeFailure<EditorSessionManifestEncodeResult>(
			EWorkingCopyPersistenceCodecStatus::InvalidRecord, "editor session manifest encoding failed");
	}
}

EditorSessionManifestDecodeResult CWorkingCopyPersistenceCodec::DecodeSession(std::string_view payload) noexcept
{
	try {
		if (payload.size() > kMaximumPayloadBytes) return DecodeFailure<EditorSessionManifestDecodeResult>(
			EWorkingCopyPersistenceCodecStatus::PayloadTooLarge, "editor session manifest exceeds the persistence payload limit");
		JsonObject root;
		if (!ParseRoot(payload, root)) return DecodeFailure<EditorSessionManifestDecodeResult>(
			EWorkingCopyPersistenceCodecStatus::CorruptPayload, "editor session manifest JSON is malformed");
		std::uint32_t version = 0;
		if (!ReadRequiredUInt32(root, "formatVersion", version)) return DecodeFailure<EditorSessionManifestDecodeResult>(
			EWorkingCopyPersistenceCodecStatus::CorruptPayload, "editor session manifest format version is invalid");
		if (version != kEditorSessionManifestFormatVersion) return DecodeFailure<EditorSessionManifestDecodeResult>(
			EWorkingCopyPersistenceCodecStatus::UnsupportedSchema, "editor session manifest format version is unsupported");
		const auto* inputsValue = Find(root, "inputs");
		if (inputsValue == nullptr || !inputsValue->is<JsonArray>()
			|| inputsValue->get<JsonArray>().size() > kMaximumEditorSessionInputs) return DecodeFailure<EditorSessionManifestDecodeResult>(
				EWorkingCopyPersistenceCodecStatus::CorruptPayload, "editor session input collection is invalid");

		EditorSessionManifest manifest;
		if (!ReadScope(Find(root, "scope"), manifest.scope)
			|| !ReadRequiredUnsigned(root, "generation", manifest.generation)
			|| !ReadRequiredString(root, "logicalGroupId", manifest.logicalGroupId)
			|| !ReadOptionalString(root, "activeInputId", manifest.activeInputId)) return DecodeFailure<EditorSessionManifestDecodeResult>(
				EWorkingCopyPersistenceCodecStatus::CorruptPayload, "editor session manifest metadata is invalid");
		for (const auto& value : inputsValue->get<JsonArray>()) {
			if (!value.is<JsonObject>()) return DecodeFailure<EditorSessionManifestDecodeResult>(
				EWorkingCopyPersistenceCodecStatus::CorruptPayload, "editor session input is invalid");
			const auto& object = value.get<JsonObject>();
			EditorSessionInputDescriptor input;
			if (!ReadRequiredString(object, "inputId", input.inputId)
				|| !ReadRequiredString(object, "inputTypeId", input.inputTypeId)
				|| !ReadIdentity(Find(object, "identity"), input.workingCopyIdentity)
				|| !ReadRequiredUInt32(object, "stateVersion", input.stateVersion)
				|| !ReadRequiredUtf8String(object, "state", input.state, kMaximumEditorSessionUntypedStateBytes)
				|| !ReadOptionalUnsigned(object, "backupGeneration", input.backupGeneration)) return DecodeFailure<EditorSessionManifestDecodeResult>(
					EWorkingCopyPersistenceCodecStatus::CorruptPayload, "editor session input metadata is invalid");
			if (!IsKnownInputType(input.inputTypeId)) return DecodeFailure<EditorSessionManifestDecodeResult>(
				EWorkingCopyPersistenceCodecStatus::UnknownInputType, "editor session input type is not registered");
			manifest.inputs.emplace_back(std::move(input));
		}
		if (!manifest.IsValid()) return DecodeFailure<EditorSessionManifestDecodeResult>(
			EWorkingCopyPersistenceCodecStatus::CorruptPayload, "editor session manifest record is invalid");
		return { EWorkingCopyPersistenceCodecStatus::Succeeded, std::move(manifest), {} };
	} catch (...) {
		return DecodeFailure<EditorSessionManifestDecodeResult>(
			EWorkingCopyPersistenceCodecStatus::CorruptPayload, "editor session manifest decoding failed");
	}
}

} // namespace workbench::editor::persistence

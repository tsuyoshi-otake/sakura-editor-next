/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "platform/profiles/UserDataProfileRegistryCodec.h"

#include <array>
#include <limits>

namespace platform::profiles {
namespace {

constexpr std::string_view kMagic = "sakura-profile-registry";
constexpr std::uint32_t kVersion = 1;
// IStorageService accepts UTF-8 values up to 64 KiB.  The binary payload is
// Base64 encoded at the boundary, so keep its worst-case textual form bounded.
constexpr std::size_t kMaximumDocumentBytes = 48 * 1024;
constexpr std::size_t kMaximumTextDocumentBytes = 64 * 1024;
constexpr std::uint32_t kMaximumProfiles = 256;
constexpr std::uint32_t kMaximumAssociations = 1024;
constexpr std::uint32_t kMaximumStringBytes = 16 * 1024;

void PutU32(std::string& out, std::uint32_t value)
{
	for (int shift = 0; shift != 32; shift += 8) out.push_back(static_cast<char>((value >> shift) & 0xff));
}
void PutU64(std::string& out, std::uint64_t value)
{
	for (int shift = 0; shift != 64; shift += 8) out.push_back(static_cast<char>((value >> shift) & 0xff));
}
bool GetU32(std::string_view input, std::size_t& offset, std::uint32_t& value)
{
	if (offset > input.size() || input.size() - offset < 4) return false;
	value = 0;
	for (int shift = 0; shift != 32; shift += 8) value |= static_cast<std::uint32_t>(static_cast<unsigned char>(input[offset++])) << shift;
	return true;
}
bool GetU64(std::string_view input, std::size_t& offset, std::uint64_t& value)
{
	if (offset > input.size() || input.size() - offset < 8) return false;
	value = 0;
	for (int shift = 0; shift != 64; shift += 8) value |= static_cast<std::uint64_t>(static_cast<unsigned char>(input[offset++])) << shift;
	return true;
}
bool ToUtf8(std::wstring_view value, std::string& output)
{
	const int bytes = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
	if (bytes < 0 || (bytes == 0 && !value.empty()) || static_cast<std::uint32_t>(bytes) > kMaximumStringBytes) return false;
	output.resize(static_cast<std::size_t>(bytes));
	return bytes == 0 || ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), output.data(), bytes, nullptr, nullptr) == bytes;
}
bool FromUtf8(std::string_view value, std::wstring& output)
{
	if (value.size() > kMaximumStringBytes) return false;
	const int characters = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
	if (characters < 0 || (characters == 0 && !value.empty())) return false;
	output.resize(static_cast<std::size_t>(characters));
	return characters == 0 || ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), output.data(), characters) == characters;
}
char ToBase64Char(unsigned char value)
{
	constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	return alphabet[value];
}
int FromBase64Char(char value)
{
	if (value >= 'A' && value <= 'Z') return value - 'A';
	if (value >= 'a' && value <= 'z') return value - 'a' + 26;
	if (value >= '0' && value <= '9') return value - '0' + 52;
	if (value == '+') return 62;
	if (value == '/') return 63;
	return -1;
}
std::string EncodeBase64(std::string_view input)
{
	std::string output;
	output.reserve(((input.size() + 2) / 3) * 4);
	for (std::size_t offset = 0; offset < input.size(); offset += 3) {
		const auto first = static_cast<unsigned char>(input[offset]);
		const bool hasSecond = offset + 1 < input.size();
		const bool hasThird = offset + 2 < input.size();
		const auto second = hasSecond ? static_cast<unsigned char>(input[offset + 1]) : 0;
		const auto third = hasThird ? static_cast<unsigned char>(input[offset + 2]) : 0;
		output.push_back(ToBase64Char(first >> 2));
		output.push_back(ToBase64Char(static_cast<unsigned char>(((first & 0x03) << 4) | (second >> 4))));
		output.push_back(hasSecond ? ToBase64Char(static_cast<unsigned char>(((second & 0x0f) << 2) | (third >> 6))) : '=');
		output.push_back(hasThird ? ToBase64Char(third & 0x3f) : '=');
	}
	return output;
}
bool DecodeBase64(std::string_view input, std::string& output)
{
	if (input.empty() || input.size() > kMaximumTextDocumentBytes || input.size() % 4 != 0) return false;
	output.clear();
	output.reserve(input.size() / 4 * 3);
	for (std::size_t offset = 0; offset < input.size(); offset += 4) {
		const int first = FromBase64Char(input[offset]);
		const int second = FromBase64Char(input[offset + 1]);
		const bool paddedThird = input[offset + 2] == '=';
		const bool paddedFourth = input[offset + 3] == '=';
		const int third = paddedThird ? 0 : FromBase64Char(input[offset + 2]);
		const int fourth = paddedFourth ? 0 : FromBase64Char(input[offset + 3]);
		if (first < 0 || second < 0 || third < 0 || fourth < 0 || (paddedThird && !paddedFourth)
			|| ((paddedThird || paddedFourth) && offset + 4 != input.size())
			|| (paddedThird && (second & 0x0f) != 0) || (paddedFourth && !paddedThird && (third & 0x03) != 0)) return false;
		output.push_back(static_cast<char>((first << 2) | (second >> 4)));
		if (!paddedThird) output.push_back(static_cast<char>((second << 4) | (third >> 2)));
		if (!paddedFourth) output.push_back(static_cast<char>((third << 6) | fourth));
	}
	return output.size() <= kMaximumDocumentBytes;
}
bool PutString(std::string& output, std::wstring_view value)
{
	std::string utf8;
	if (!ToUtf8(value, utf8)) return false;
	PutU32(output, static_cast<std::uint32_t>(utf8.size()));
	output.append(utf8);
	return output.size() <= kMaximumDocumentBytes;
}
bool GetString(std::string_view input, std::size_t& offset, std::wstring& value)
{
	std::uint32_t bytes = 0;
	if (!GetU32(input, offset, bytes) || bytes > kMaximumStringBytes || offset > input.size() || input.size() - offset < bytes) return false;
	const auto encoded = input.substr(offset, bytes);
	offset += bytes;
	return FromUtf8(encoded, value);
}
bool PutProfile(std::string& output, const UserDataProfileDescriptor& profile)
{
	if (!PutString(output, profile.profileId) || !PutString(output, profile.displayName)) return false;
	output.push_back(static_cast<char>(profile.kind));
	unsigned char inheritance = (profile.resourceInheritance.settings ? 1 : 0) | (profile.resourceInheritance.keybindings ? 2 : 0)
		| (profile.resourceInheritance.tasks ? 4 : 0) | (profile.resourceInheritance.snippets ? 8 : 0)
		| (profile.resourceInheritance.extensions ? 16 : 0) | (profile.resourceInheritance.globalState ? 32 : 0);
	output.push_back(static_cast<char>(inheritance));
	if (profile.legacyAliases.size() > kMaximumAssociations) return false;
	PutU32(output, static_cast<std::uint32_t>(profile.legacyAliases.size()));
	for (const auto& alias : profile.legacyAliases) if (!PutString(output, alias)) return false;
	return true;
}
bool GetProfile(std::string_view input, std::size_t& offset, UserDataProfileDescriptor& profile)
{
	if (!GetString(input, offset, profile.profileId) || !GetString(input, offset, profile.displayName)
		|| offset > input.size() || input.size() - offset < 2) return false;
	const auto kind = static_cast<unsigned char>(input[offset++]);
	const auto inheritance = static_cast<unsigned char>(input[offset++]);
	if (kind > static_cast<unsigned char>(UserDataProfileKind::Transient) || (inheritance & ~0x3f) != 0) return false;
	profile.kind = static_cast<UserDataProfileKind>(kind);
	profile.resourceInheritance = { (inheritance & 1) != 0, (inheritance & 2) != 0, (inheritance & 4) != 0,
		(inheritance & 8) != 0, (inheritance & 16) != 0, (inheritance & 32) != 0 };
	std::uint32_t aliases = 0;
	if (!GetU32(input, offset, aliases) || aliases > kMaximumAssociations) return false;
	profile.legacyAliases.resize(aliases);
	for (auto& alias : profile.legacyAliases) if (!GetString(input, offset, alias)) return false;
	return true;
}

} // namespace

std::string EncodeUserDataProfileRegistryDocument(const UserDataProfileRegistrySnapshot& snapshot)
{
	if (snapshot.profiles.size() > kMaximumProfiles || snapshot.workspaceAssociations.size() > kMaximumAssociations
		|| snapshot.emptyWindowAssociations.size() > kMaximumAssociations) return {};
	std::string output(kMagic);
	PutU32(output, kVersion);
	PutU64(output, snapshot.revision);
	if (!PutString(output, snapshot.defaultProfileId)) return {};
	PutU32(output, static_cast<std::uint32_t>(snapshot.profiles.size()));
	for (const auto& profile : snapshot.profiles) if (!PutProfile(output, profile)) return {};
	PutU32(output, static_cast<std::uint32_t>(snapshot.workspaceAssociations.size()));
	for (const auto& [uri, profileId] : snapshot.workspaceAssociations) {
		if (!PutString(output, uri.ToString()) || !PutString(output, profileId)) return {};
	}
	PutU32(output, static_cast<std::uint32_t>(snapshot.emptyWindowAssociations.size()));
	for (const auto& [windowId, profileId] : snapshot.emptyWindowAssociations) {
		if (!PutString(output, windowId) || !PutString(output, profileId)) return {};
	}
	if (output.size() > kMaximumDocumentBytes) return {};
	const auto encoded = EncodeBase64(output);
	return encoded.size() <= kMaximumTextDocumentBytes ? encoded : std::string{};
}

UserDataProfileRegistryDecodeResult DecodeUserDataProfileRegistryDocument(std::string_view input)
{
	if (input.size() > kMaximumTextDocumentBytes) return { UserDataProfileRegistryCodecStatus::TooLarge, {} };
	std::string decodedInput;
	if (!DecodeBase64(input, decodedInput)) return {};
	input = decodedInput;
	if (!input.starts_with(kMagic)) return {};
	std::size_t offset = kMagic.size();
	std::uint32_t version = 0;
	if (!GetU32(input, offset, version)) return {};
	if (version != kVersion) return { UserDataProfileRegistryCodecStatus::UnsupportedVersion, {} };
	UserDataProfileRegistrySnapshot snapshot;
	if (!GetU64(input, offset, snapshot.revision) || !GetString(input, offset, snapshot.defaultProfileId)) return {};
	std::uint32_t profiles = 0;
	if (!GetU32(input, offset, profiles) || profiles == 0 || profiles > kMaximumProfiles) return {};
	snapshot.profiles.resize(profiles);
	for (auto& profile : snapshot.profiles) if (!GetProfile(input, offset, profile)) return {};
	std::uint32_t workspaces = 0;
	if (!GetU32(input, offset, workspaces) || workspaces > kMaximumAssociations) return {};
	for (std::uint32_t index = 0; index < workspaces; ++index) {
		std::wstring uri; UserDataProfileId profileId;
		if (!GetString(input, offset, uri) || !GetString(input, offset, profileId)) return {};
		auto parsed = ::platform::uri::Uri::Parse(uri);
		if (!parsed) return {};
		snapshot.workspaceAssociations.emplace_back(std::move(*parsed.value), std::move(profileId));
	}
	std::uint32_t emptyWindows = 0;
	if (!GetU32(input, offset, emptyWindows) || emptyWindows > kMaximumAssociations) return {};
	for (std::uint32_t index = 0; index < emptyWindows; ++index) {
		EmptyWindowId windowId; UserDataProfileId profileId;
		if (!GetString(input, offset, windowId) || !GetString(input, offset, profileId)) return {};
		snapshot.emptyWindowAssociations.emplace_back(std::move(windowId), std::move(profileId));
	}
	if (offset != input.size()) return {};
	UserDataProfileRegistry validator;
	if (validator.ReplaceSnapshot(snapshot) != UserDataProfileSnapshotStatus::Applied) return {};
	return { UserDataProfileRegistryCodecStatus::Decoded, std::move(snapshot) };
}

} // namespace platform::profiles

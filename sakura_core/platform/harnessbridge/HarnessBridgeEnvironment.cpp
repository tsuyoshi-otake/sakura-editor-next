/*! @file */
#include <sakura/harnessbridge/HarnessBridgeEnvironment.h>

#include <sakura/harnessbridge/HarnessBridgeProtocol.h>
#include <sakura/harnessbridge/HarnessBridgeSecurity.h>

#include <algorithm>
#include <limits>
#include <span>
#include <vector>

namespace platform::harnessbridge {
namespace {

constexpr std::wstring_view kEndpointPrefix = L"she1.";
constexpr std::wstring_view kTargetPrefix = L"sht1.";
constexpr std::wstring_view kCapabilityPrefix = L"shc1.";
constexpr std::size_t kMaximumProfileIdBytes = 256;

constexpr char kBase64UrlAlphabet[] =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

bool IsValidEndpointHash(std::wstring_view value) noexcept
{
	if (value.size() != 64) return false;
	return std::all_of(value.begin(), value.end(), [](const wchar_t value) {
		return (value >= L'0' && value <= L'9') || (value >= L'a' && value <= L'f');
	});
}

std::wstring Base64UrlEncode(std::span<const std::uint8_t> input)
{
	std::wstring result;
	result.reserve((input.size() * 4 + 2) / 3);
	for (std::size_t offset = 0; offset < input.size(); offset += 3) {
		const auto remaining = input.size() - offset;
		const std::uint32_t value = static_cast<std::uint32_t>(input[offset]) << 16
			| (remaining > 1 ? static_cast<std::uint32_t>(input[offset + 1]) << 8 : 0)
			| (remaining > 2 ? static_cast<std::uint32_t>(input[offset + 2]) : 0);
		result.push_back(static_cast<wchar_t>(kBase64UrlAlphabet[(value >> 18) & 0x3f]));
		result.push_back(static_cast<wchar_t>(kBase64UrlAlphabet[(value >> 12) & 0x3f]));
		if (remaining > 1) result.push_back(static_cast<wchar_t>(kBase64UrlAlphabet[(value >> 6) & 0x3f]));
		if (remaining > 2) result.push_back(static_cast<wchar_t>(kBase64UrlAlphabet[value & 0x3f]));
	}
	return result;
}

int DecodeBase64UrlCharacter(const wchar_t value) noexcept
{
	if (value >= L'A' && value <= L'Z') return value - L'A';
	if (value >= L'a' && value <= L'z') return value - L'a' + 26;
	if (value >= L'0' && value <= L'9') return value - L'0' + 52;
	if (value == L'-') return 62;
	if (value == L'_') return 63;
	return -1;
}

std::optional<std::vector<std::uint8_t>> Base64UrlDecode(std::wstring_view input)
{
	if (input.empty() || input.size() % 4 == 1) return std::nullopt;
	std::vector<std::uint8_t> result;
	result.reserve(input.size() * 3 / 4);
	std::uint32_t accumulator = 0;
	unsigned int bits = 0;
	for (const auto value : input) {
		const auto decoded = DecodeBase64UrlCharacter(value);
		if (decoded < 0) return std::nullopt;
		accumulator = (accumulator << 6) | static_cast<std::uint32_t>(decoded);
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			result.push_back(static_cast<std::uint8_t>(accumulator >> bits));
			accumulator &= bits == 0 ? 0 : (std::uint32_t{ 1 } << bits) - 1;
		}
	}
	if (bits != 0 && accumulator != 0) return std::nullopt;
	if (Base64UrlEncode(result) != input) return std::nullopt;
	return result;
}

void AppendU16(std::vector<std::uint8_t>& output, std::uint16_t value)
{
	output.push_back(static_cast<std::uint8_t>(value));
	output.push_back(static_cast<std::uint8_t>(value >> 8));
}

void AppendU32(std::vector<std::uint8_t>& output, std::uint32_t value)
{
	for (unsigned int shift = 0; shift < 32; shift += 8) {
		output.push_back(static_cast<std::uint8_t>(value >> shift));
	}
}

void AppendU64(std::vector<std::uint8_t>& output, std::uint64_t value)
{
	for (unsigned int shift = 0; shift < 64; shift += 8) {
		output.push_back(static_cast<std::uint8_t>(value >> shift));
	}
}

template<typename Value>
bool ReadInteger(std::span<const std::uint8_t> input, std::size_t& offset, Value& value) noexcept
{
	if (sizeof(Value) > input.size() - offset) return false;
	value = 0;
	for (std::size_t index = 0; index < sizeof(Value); ++index) {
		value |= static_cast<Value>(input[offset + index]) << (index * 8);
	}
	offset += sizeof(Value);
	return true;
}

bool HasNonzero(std::span<const std::uint8_t> value) noexcept
{
	return std::any_of(value.begin(), value.end(), [](const std::uint8_t value) { return value != 0; });
}

bool ValidTarget(const HarnessBridgeTargetDescriptor& value) noexcept
{
	return !value.profileId.empty() && value.profileId.size() <= kMaximumProfileIdBytes
		&& IsValidHarnessBridgeUtf8(std::span<const std::uint8_t>(
			reinterpret_cast<const std::uint8_t*>(value.profileId.data()), value.profileId.size()))
		&& value.profileGeneration != 0 && HasNonzero(value.editorId)
		&& value.bridgeEpoch != 0 && value.runtimeGeneration != 0
		&& value.instanceGeneration != 0 && value.sessionId != 0
		&& value.windowId != 0 && value.paneId != 0 && value.instanceId != 0;
}

} // namespace

std::optional<std::wstring> EncodeHarnessEndpointEnvironment(const std::wstring_view endpointHash)
{
	if (!IsValidEndpointHash(endpointHash)) return std::nullopt;
	return std::wstring(kEndpointPrefix) + std::wstring(endpointHash);
}

std::optional<std::wstring> DecodeHarnessEndpointEnvironment(const std::wstring_view encoded)
{
	if (!encoded.starts_with(kEndpointPrefix)) return std::nullopt;
	const auto hash = encoded.substr(kEndpointPrefix.size());
	return IsValidEndpointHash(hash) ? std::optional{ std::wstring(hash) } : std::nullopt;
}

std::optional<std::wstring> EncodeHarnessTargetEnvironment(
	const HarnessBridgeTargetDescriptor& descriptor)
{
	if (!ValidTarget(descriptor)) return std::nullopt;
	std::vector<std::uint8_t> payload;
	payload.reserve(3 + descriptor.profileId.size() + descriptor.editorId.size() + 64);
	payload.push_back(1);
	AppendU16(payload, static_cast<std::uint16_t>(descriptor.profileId.size()));
	payload.insert(payload.end(), descriptor.profileId.begin(), descriptor.profileId.end());
	payload.insert(payload.end(), descriptor.editorId.begin(), descriptor.editorId.end());
	AppendU64(payload, descriptor.profileGeneration);
	AppendU64(payload, descriptor.bridgeEpoch);
	AppendU64(payload, descriptor.runtimeGeneration);
	AppendU64(payload, descriptor.instanceGeneration);
	AppendU64(payload, descriptor.sessionId);
	AppendU64(payload, descriptor.windowId);
	AppendU64(payload, descriptor.paneId);
	AppendU64(payload, descriptor.instanceId);
	return std::wstring(kTargetPrefix) + Base64UrlEncode(payload);
}

std::optional<HarnessBridgeTargetDescriptor> DecodeHarnessTargetEnvironment(
	const std::wstring_view encoded)
{
	if (!encoded.starts_with(kTargetPrefix)) return std::nullopt;
	const auto payload = Base64UrlDecode(encoded.substr(kTargetPrefix.size()));
	if (!payload || payload->size() < 3 + 16 + 64 || payload->front() != 1) return std::nullopt;
	std::size_t offset = 1;
	std::uint16_t profileBytes = 0;
	if (!ReadInteger<std::uint16_t>(*payload, offset, profileBytes)
		|| profileBytes == 0 || profileBytes > kMaximumProfileIdBytes
		|| profileBytes > payload->size() - offset) return std::nullopt;
	HarnessBridgeTargetDescriptor result;
	result.profileId.assign(reinterpret_cast<const char*>(payload->data() + offset), profileBytes);
	offset += profileBytes;
	if (result.editorId.size() > payload->size() - offset) return std::nullopt;
	std::copy_n(payload->begin() + static_cast<std::ptrdiff_t>(offset), result.editorId.size(), result.editorId.begin());
	offset += result.editorId.size();
	if (!ReadInteger<std::uint64_t>(*payload, offset, result.profileGeneration)
		|| !ReadInteger<std::uint64_t>(*payload, offset, result.bridgeEpoch)
		|| !ReadInteger<std::uint64_t>(*payload, offset, result.runtimeGeneration)
		|| !ReadInteger<std::uint64_t>(*payload, offset, result.instanceGeneration)
		|| !ReadInteger<std::uint64_t>(*payload, offset, result.sessionId)
		|| !ReadInteger<std::uint64_t>(*payload, offset, result.windowId)
		|| !ReadInteger<std::uint64_t>(*payload, offset, result.paneId)
		|| !ReadInteger<std::uint64_t>(*payload, offset, result.instanceId)
		|| offset != payload->size() || !ValidTarget(result)) return std::nullopt;
	return result;
}

std::optional<std::wstring> EncodeHarnessCapabilityEnvironment(
	const HarnessCapabilityCredential& credential)
{
	if (!credential.id.IsValid() || credential.grants == EHarnessGrant::None
		|| !HasNonzero(credential.secret)) return std::nullopt;
	std::vector<std::uint8_t> payload;
	payload.reserve(1 + credential.id.value.size() + credential.secret.size() + 4);
	payload.push_back(1);
	payload.insert(payload.end(), credential.id.value.begin(), credential.id.value.end());
	payload.insert(payload.end(), credential.secret.begin(), credential.secret.end());
	AppendU32(payload, static_cast<std::uint32_t>(credential.grants));
	return std::wstring(kCapabilityPrefix) + Base64UrlEncode(payload);
}

std::optional<HarnessCapabilityCredential> DecodeHarnessCapabilityEnvironment(
	const std::wstring_view encoded)
{
	if (!encoded.starts_with(kCapabilityPrefix)) return std::nullopt;
	const auto payload = Base64UrlDecode(encoded.substr(kCapabilityPrefix.size()));
	constexpr std::size_t expectedBytes = 1 + 16 + 32 + 4;
	if (!payload || payload->size() != expectedBytes || payload->front() != 1) return std::nullopt;
	HarnessCapabilityCredential result;
	std::size_t offset = 1;
	std::copy_n(payload->begin() + static_cast<std::ptrdiff_t>(offset), result.id.value.size(), result.id.value.begin());
	offset += result.id.value.size();
	std::copy_n(payload->begin() + static_cast<std::ptrdiff_t>(offset), result.secret.size(), result.secret.begin());
	offset += result.secret.size();
	std::uint32_t grants = 0;
	if (!ReadInteger<std::uint32_t>(*payload, offset, grants) || offset != payload->size()) return std::nullopt;
	const auto known = static_cast<std::uint32_t>(EHarnessGrant::Message)
		| static_cast<std::uint32_t>(EHarnessGrant::ConsoleRead)
		| static_cast<std::uint32_t>(EHarnessGrant::SendInput)
		| static_cast<std::uint32_t>(EHarnessGrant::ManageTerminal);
	if (grants == 0 || (grants & ~known) != 0 || !result.id.IsValid() || !HasNonzero(result.secret)) {
		return std::nullopt;
	}
	result.grants = static_cast<EHarnessGrant>(grants);
	return result;
}

} // namespace platform::harnessbridge

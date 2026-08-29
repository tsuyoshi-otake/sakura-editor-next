/*! @file */
#include <sakura/harnessbridge/HarnessBridgeOperationDispatcher.h>

#include <algorithm>
#include <limits>
#include <utility>

namespace platform::harnessbridge {
namespace {

constexpr std::uint8_t kCodecVersion = 1;
constexpr std::size_t kMaximumCodecBytes = kHarnessBridgeMaximumPayloadBytes;

void AppendU8(std::vector<std::uint8_t>& out, const std::uint8_t value) { out.push_back(value); }
void AppendU16(std::vector<std::uint8_t>& out, const std::uint16_t value)
{
	out.push_back(static_cast<std::uint8_t>(value));
	out.push_back(static_cast<std::uint8_t>(value >> 8));
}
void AppendU32(std::vector<std::uint8_t>& out, const std::uint32_t value)
{
	for (unsigned shift = 0; shift < 32; shift += 8) out.push_back(static_cast<std::uint8_t>(value >> shift));
}
void AppendU64(std::vector<std::uint8_t>& out, const std::uint64_t value)
{
	for (unsigned shift = 0; shift < 64; shift += 8) out.push_back(static_cast<std::uint8_t>(value >> shift));
}
void AppendI32(std::vector<std::uint8_t>& out, const std::int32_t value)
{
	AppendU32(out, static_cast<std::uint32_t>(value));
}
void AppendId(std::vector<std::uint8_t>& out, const HarnessOpaqueId& id)
{
	out.insert(out.end(), id.value.begin(), id.value.end());
}

class Cursor final {
public:
	explicit Cursor(const std::span<const std::uint8_t> bytes) : m_bytes(bytes) {}
	bool ReadU8(std::uint8_t& value) noexcept
	{
		if (m_offset >= m_bytes.size()) return false;
		value = m_bytes[m_offset++];
		return true;
	}
	bool ReadU16(std::uint16_t& value) noexcept
	{
		if (m_bytes.size() - m_offset < 2) return false;
		value = static_cast<std::uint16_t>(m_bytes[m_offset])
			| static_cast<std::uint16_t>(m_bytes[m_offset + 1] << 8);
		m_offset += 2;
		return true;
	}
	bool ReadU32(std::uint32_t& value) noexcept
	{
		if (m_bytes.size() - m_offset < 4) return false;
		value = static_cast<std::uint32_t>(m_bytes[m_offset])
			| (static_cast<std::uint32_t>(m_bytes[m_offset + 1]) << 8)
			| (static_cast<std::uint32_t>(m_bytes[m_offset + 2]) << 16)
			| (static_cast<std::uint32_t>(m_bytes[m_offset + 3]) << 24);
		m_offset += 4;
		return true;
	}
	bool ReadU64(std::uint64_t& value) noexcept
	{
		if (m_bytes.size() - m_offset < 8) return false;
		value = 0;
		for (unsigned shift = 0; shift < 64; shift += 8) value |= static_cast<std::uint64_t>(m_bytes[m_offset++]) << shift;
		return true;
	}
	bool ReadBytes(std::size_t size, std::span<const std::uint8_t>& value) noexcept
	{
		if (size > m_bytes.size() - m_offset) return false;
		value = m_bytes.subspan(m_offset, size);
		m_offset += size;
		return true;
	}
	[[nodiscard]] bool AtEnd() const noexcept { return m_offset == m_bytes.size(); }

private:
	std::span<const std::uint8_t> m_bytes;
	std::size_t m_offset = 0;
};

bool ValidText(const std::span<const std::uint8_t> bytes, const std::size_t limit) noexcept
{
	return !bytes.empty() && bytes.size() <= limit && std::none_of(bytes.begin(), bytes.end(),
		[](const std::uint8_t value) { return value == 0; }) && IsValidHarnessBridgeUtf8(bytes);
}

bool ReadId(Cursor& cursor, HarnessOpaqueId& id) noexcept
{
	std::span<const std::uint8_t> bytes;
	if (!cursor.ReadBytes(id.value.size(), bytes)) return false;
	std::copy(bytes.begin(), bytes.end(), id.value.begin());
	return id.IsValid();
}

bool ReadOptionalId(Cursor& cursor, HarnessOpaqueId& id) noexcept
{
	std::span<const std::uint8_t> bytes;
	if (!cursor.ReadBytes(id.value.size(), bytes)) return false;
	std::copy(bytes.begin(), bytes.end(), id.value.begin());
	return id.IsValid() || std::all_of(id.value.begin(), id.value.end(), [](const auto byte) { return byte == 0; });
}

bool ReadString(Cursor& cursor, std::string& value, const std::size_t limit)
{
	std::uint16_t size = 0;
	std::span<const std::uint8_t> bytes;
	if (!cursor.ReadU16(size)) return false;
	if (size > limit || !cursor.ReadBytes(size, bytes) || !ValidText(bytes, limit)) return false;
	value.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
	return true;
}

bool AppendString(std::vector<std::uint8_t>& out, const std::string& value, const std::size_t limit)
{
	if (!ValidText(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(value.data()), value.size()), limit)
		|| value.size() > (std::numeric_limits<std::uint16_t>::max)()) return false;
	AppendU16(out, static_cast<std::uint16_t>(value.size()));
	out.insert(out.end(), value.begin(), value.end());
	return true;
}

bool ReadBytes32(Cursor& cursor, std::vector<std::uint8_t>& value, const std::size_t limit)
{
	std::uint32_t size = 0;
	std::span<const std::uint8_t> bytes;
	if (!cursor.ReadU32(size) || size > limit || !cursor.ReadBytes(size, bytes)) return false;
	value.assign(bytes.begin(), bytes.end());
	return true;
}

void AppendBytes32(std::vector<std::uint8_t>& out, const std::vector<std::uint8_t>& value)
{
	AppendU32(out, static_cast<std::uint32_t>(value.size()));
	out.insert(out.end(), value.begin(), value.end());
}

bool DecodeEndpointRegistrationImpl(std::span<const std::uint8_t> bytes, HarnessEndpointRegistration& result) noexcept
{
	try {
		Cursor cursor(bytes);
		std::uint8_t version = 0;
		std::uint32_t grants = 0;
		std::uint32_t maximumQueue = 0;
		if (!cursor.ReadU8(version) || version != kCodecVersion || !ReadId(cursor, result.endpointId)
			|| !ReadString(cursor, result.displayName, kHarnessBridgeMaximumEndpointNameBytes)
			|| !ReadString(cursor, result.scope, kHarnessBridgeMaximumEndpointScopeBytes)
			|| !cursor.ReadU32(grants) || !cursor.ReadU32(maximumQueue) || !cursor.AtEnd()) return false;
		result.grants = static_cast<EHarnessGrant>(grants);
		result.maximumQueue = maximumQueue;
		return true;
	} catch (...) {
		return false;
	}
}

bool DecodeMessageImpl(std::span<const std::uint8_t> bytes, HarnessMessage& result) noexcept
{
	try {
		Cursor cursor(bytes);
		std::uint8_t version = 0;
		if (!cursor.ReadU8(version) || version != kCodecVersion || !ReadId(cursor, result.messageId)
			|| !ReadOptionalId(cursor, result.runId) || !ReadId(cursor, result.sender) || !ReadId(cursor, result.recipient)
			|| !ReadOptionalId(cursor, result.replyTo) || !cursor.ReadU8(result.hopCount)
			|| !ReadString(cursor, result.type, kHarnessBridgeMaximumMessageTypeBytes)
			|| !ReadBytes32(cursor, result.payload, kMaximumCodecBytes)
			|| !cursor.AtEnd()) return false;
		return true;
	} catch (...) {
		return false;
	}
}

bool DecodeRunPublishImpl(std::span<const std::uint8_t> bytes, bool& begin, HarnessRunResult& result) noexcept
{
	try {
		Cursor cursor(bytes);
		std::uint8_t version = 0;
		std::uint8_t action = 0;
		std::uint8_t status = 0;
		std::uint32_t exitCode = 0;
		if (!cursor.ReadU8(version) || version != kCodecVersion || !cursor.ReadU8(action)
			|| !ReadId(cursor, result.runId)) return false;
		if (action == 0) {
			begin = true;
			return cursor.AtEnd();
		}
		if (action != 1 || !cursor.ReadU8(status) || !cursor.ReadU32(exitCode)
			|| !cursor.ReadU64(result.completedAtTick) || !cursor.AtEnd()
			|| status > static_cast<std::uint8_t>(EHarnessRunTerminalStatus::HarnessExited)) return false;
		begin = false;
		result.status = static_cast<EHarnessRunTerminalStatus>(status);
		result.exitCode = static_cast<std::int32_t>(exitCode);
		return true;
	} catch (...) {
		return false;
	}
}

std::optional<std::vector<std::uint8_t>> EncodeEndpoints(const std::vector<HarnessEndpointInfo>& endpoints)
{
	if (endpoints.size() > (std::numeric_limits<std::uint16_t>::max)()) return std::nullopt;
	std::vector<std::uint8_t> result{ kCodecVersion };
	AppendU16(result, static_cast<std::uint16_t>(endpoints.size()));
	for (const auto& endpoint : endpoints) {
		AppendId(result, endpoint.endpointId);
		if (!AppendString(result, endpoint.displayName, kHarnessBridgeMaximumEndpointNameBytes)
			|| !AppendString(result, endpoint.scope, kHarnessBridgeMaximumEndpointScopeBytes)) return std::nullopt;
		AppendU32(result, static_cast<std::uint32_t>(endpoint.grants));
		if (result.size() > kMaximumCodecBytes) return std::nullopt;
	}
	return result;
}

std::optional<std::vector<std::uint8_t>> EncodeDeliveries(const std::vector<HarnessMessageDelivery>& deliveries)
{
	if (deliveries.size() > (std::numeric_limits<std::uint16_t>::max)()) return std::nullopt;
	std::vector<std::uint8_t> result{ kCodecVersion };
	AppendU16(result, static_cast<std::uint16_t>(deliveries.size()));
	for (const auto& delivery : deliveries) {
		const auto& message = delivery.message;
		AppendId(result, message.messageId);
		AppendId(result, message.runId);
		AppendId(result, message.sender);
		AppendId(result, message.recipient);
		AppendId(result, message.replyTo);
		AppendU8(result, message.hopCount);
		AppendU64(result, delivery.deliveryAttempt);
		if (!AppendString(result, message.type, kHarnessBridgeMaximumMessageTypeBytes)) return std::nullopt;
		if (message.payload.size() > kMaximumCodecBytes) return std::nullopt;
		AppendBytes32(result, message.payload);
		if (result.size() > kMaximumCodecBytes) return std::nullopt;
	}
	return result;
}

std::optional<std::vector<std::uint8_t>> EncodeRun(const std::optional<HarnessRunResult>& run)
{
	std::vector<std::uint8_t> result{ kCodecVersion, static_cast<std::uint8_t>(run.has_value()) };
	if (run) {
		AppendId(result, run->runId);
		AppendU8(result, static_cast<std::uint8_t>(run->status));
		AppendI32(result, run->exitCode);
		AppendU64(result, run->completedAtTick);
	}
	return result.size() <= kMaximumCodecBytes ? std::optional{ std::move(result) } : std::nullopt;
}

} // namespace

std::optional<std::vector<std::uint8_t>> EncodeHarnessBridgeEndpointRegistration(
	const HarnessEndpointRegistration& registration)
{
	try {
		if (!registration.endpointId.IsValid() || registration.displayName.empty()
			|| registration.displayName.size() > kHarnessBridgeMaximumEndpointNameBytes
			|| registration.scope.empty() || registration.scope.size() > kHarnessBridgeMaximumEndpointScopeBytes
			|| registration.grants == EHarnessGrant::None
			|| registration.maximumQueue == 0
			|| registration.maximumQueue > (std::numeric_limits<std::uint32_t>::max)()) return std::nullopt;
		std::vector<std::uint8_t> result{ kHarnessBridgeBrokerCodecVersion };
		AppendId(result, registration.endpointId);
		if (!AppendString(result, registration.displayName, kHarnessBridgeMaximumEndpointNameBytes)
			|| !AppendString(result, registration.scope, kHarnessBridgeMaximumEndpointScopeBytes)) return std::nullopt;
		AppendU32(result, static_cast<std::uint32_t>(registration.grants));
		AppendU32(result, static_cast<std::uint32_t>(registration.maximumQueue));
		return result.size() <= kMaximumCodecBytes ? std::optional{ std::move(result) } : std::nullopt;
	} catch (...) {
		return std::nullopt;
	}
}

bool DecodeHarnessBridgeEndpointRegistration(
	const std::span<const std::uint8_t> bytes, HarnessEndpointRegistration& registration) noexcept
{
	return DecodeEndpointRegistrationImpl(bytes, registration);
}

std::optional<std::vector<std::uint8_t>> EncodeHarnessBridgeMessage(const HarnessMessage& message)
{
	try {
		if (!message.messageId.IsValid() || !message.sender.IsValid() || !message.recipient.IsValid()
			|| (!message.runId.IsValid() && !std::all_of(message.runId.value.begin(), message.runId.value.end(),
				[](const auto byte) { return byte == 0; }))
			|| (!message.replyTo.IsValid() && !std::all_of(message.replyTo.value.begin(), message.replyTo.value.end(),
				[](const auto byte) { return byte == 0; }))
			|| message.type.empty() || message.type.size() > kHarnessBridgeMaximumMessageTypeBytes
			|| message.payload.size() > kMaximumCodecBytes
			|| message.payload.size() > (std::numeric_limits<std::uint32_t>::max)()) return std::nullopt;
		std::vector<std::uint8_t> result{ kHarnessBridgeBrokerCodecVersion };
		AppendId(result, message.messageId);
		AppendId(result, message.runId);
		AppendId(result, message.sender);
		AppendId(result, message.recipient);
		AppendId(result, message.replyTo);
		AppendU8(result, message.hopCount);
		if (!AppendString(result, message.type, kHarnessBridgeMaximumMessageTypeBytes)) return std::nullopt;
		AppendBytes32(result, message.payload);
		return result.size() <= kMaximumCodecBytes ? std::optional{ std::move(result) } : std::nullopt;
	} catch (...) {
		return std::nullopt;
	}
}

bool DecodeHarnessBridgeMessage(
	const std::span<const std::uint8_t> bytes, HarnessMessage& message) noexcept
{
	return DecodeMessageImpl(bytes, message);
}

std::optional<std::vector<std::uint8_t>> EncodeHarnessBridgeReceiveRequest(
	const HarnessEndpointId& endpoint, const std::size_t maximumMessages)
{
	if (!endpoint.IsValid() || maximumMessages == 0
		|| maximumMessages > (std::numeric_limits<std::uint16_t>::max)()) return std::nullopt;
	std::vector<std::uint8_t> result{ kHarnessBridgeBrokerCodecVersion };
	AppendId(result, endpoint);
	AppendU16(result, static_cast<std::uint16_t>(maximumMessages));
	return result;
}

bool DecodeHarnessBridgeReceiveRequest(
	const std::span<const std::uint8_t> bytes, HarnessEndpointId& endpoint,
	std::uint16_t& maximumMessages) noexcept
{
	try {
		Cursor cursor(bytes);
		std::uint8_t version = 0;
		return cursor.ReadU8(version) && version == kHarnessBridgeBrokerCodecVersion
			&& ReadId(cursor, endpoint) && cursor.ReadU16(maximumMessages)
			&& maximumMessages != 0 && cursor.AtEnd();
	} catch (...) {
		return false;
	}
}

std::optional<std::vector<std::uint8_t>> EncodeHarnessBridgeAcknowledgeRequest(
	const HarnessEndpointId& endpoint, const HarnessMessageId& message)
{
	if (!endpoint.IsValid() || !message.IsValid()) return std::nullopt;
	std::vector<std::uint8_t> result{ kHarnessBridgeBrokerCodecVersion };
	AppendId(result, endpoint);
	AppendId(result, message);
	return result;
}

bool DecodeHarnessBridgeAcknowledgeRequest(
	const std::span<const std::uint8_t> bytes, HarnessEndpointId& endpoint,
	HarnessMessageId& message) noexcept
{
	try {
		Cursor cursor(bytes);
		std::uint8_t version = 0;
		return cursor.ReadU8(version) && version == kHarnessBridgeBrokerCodecVersion
			&& ReadId(cursor, endpoint) && ReadId(cursor, message) && cursor.AtEnd();
	} catch (...) {
		return false;
	}
}

std::optional<std::vector<std::uint8_t>> EncodeHarnessBridgeRunPublish(
	const bool begin, const HarnessRunResult& result)
{
	if (!result.runId.IsValid()) return std::nullopt;
	if (!begin && static_cast<std::uint8_t>(result.status)
		> static_cast<std::uint8_t>(EHarnessRunTerminalStatus::HarnessExited)) return std::nullopt;
	std::vector<std::uint8_t> bytes{ kHarnessBridgeBrokerCodecVersion, static_cast<std::uint8_t>(begin ? 0 : 1) };
	AppendId(bytes, result.runId);
	if (!begin) {
		AppendU8(bytes, static_cast<std::uint8_t>(result.status));
		AppendI32(bytes, result.exitCode);
		AppendU64(bytes, result.completedAtTick);
	}
	return bytes;
}

bool DecodeHarnessBridgeRunPublish(
	const std::span<const std::uint8_t> bytes, bool& begin, HarnessRunResult& result) noexcept
{
	return DecodeRunPublishImpl(bytes, begin, result);
}

std::optional<std::vector<std::uint8_t>> EncodeHarnessBridgeRunRequest(const HarnessRunId& run)
{
	if (!run.IsValid()) return std::nullopt;
	std::vector<std::uint8_t> result{ kHarnessBridgeBrokerCodecVersion };
	AppendId(result, run);
	return result;
}

bool DecodeHarnessBridgeRunRequest(
	const std::span<const std::uint8_t> bytes, HarnessRunId& run) noexcept
{
	try {
		Cursor cursor(bytes);
		std::uint8_t version = 0;
		return cursor.ReadU8(version) && version == kHarnessBridgeBrokerCodecVersion
			&& ReadId(cursor, run) && cursor.AtEnd();
	} catch (...) {
		return false;
	}
}

std::optional<std::vector<std::uint8_t>> EncodeHarnessBridgeEndpointList(
	const std::vector<HarnessEndpointInfo>& endpoints)
{
	return EncodeEndpoints(endpoints);
}

bool DecodeHarnessBridgeEndpointList(
	const std::span<const std::uint8_t> bytes, std::vector<HarnessEndpointInfo>& endpoints) noexcept
{
	try {
		if (bytes.size() > kMaximumCodecBytes) return false;
		Cursor cursor(bytes);
		std::uint8_t version = 0;
		std::uint16_t count = 0;
		if (!cursor.ReadU8(version) || version != kHarnessBridgeBrokerCodecVersion || !cursor.ReadU16(count)) return false;
		std::vector<HarnessEndpointInfo> decoded;
		decoded.reserve(count);
		for (std::uint16_t index = 0; index < count; ++index) {
			HarnessEndpointInfo endpoint;
			if (!ReadId(cursor, endpoint.endpointId)
				|| !ReadString(cursor, endpoint.displayName, kHarnessBridgeMaximumEndpointNameBytes)
				|| !ReadString(cursor, endpoint.scope, kHarnessBridgeMaximumEndpointScopeBytes)) return false;
			std::uint32_t grants = 0;
			if (!cursor.ReadU32(grants)) return false;
			endpoint.grants = static_cast<EHarnessGrant>(grants);
			decoded.push_back(std::move(endpoint));
		}
		if (!cursor.AtEnd()) return false;
		endpoints = std::move(decoded);
		return true;
	} catch (...) {
		return false;
	}
}

std::optional<std::vector<std::uint8_t>> EncodeHarnessBridgeDeliveries(
	const std::vector<HarnessMessageDelivery>& deliveries)
{
	return EncodeDeliveries(deliveries);
}

bool DecodeHarnessBridgeDeliveries(
	const std::span<const std::uint8_t> bytes, std::vector<HarnessMessageDelivery>& deliveries) noexcept
{
	try {
		if (bytes.size() > kMaximumCodecBytes) return false;
		Cursor cursor(bytes);
		std::uint8_t version = 0;
		std::uint16_t count = 0;
		if (!cursor.ReadU8(version) || version != kHarnessBridgeBrokerCodecVersion || !cursor.ReadU16(count)) return false;
		std::vector<HarnessMessageDelivery> decoded;
		decoded.reserve(count);
		for (std::uint16_t index = 0; index < count; ++index) {
			HarnessMessageDelivery delivery;
			if (!ReadId(cursor, delivery.message.messageId) || !ReadOptionalId(cursor, delivery.message.runId)
				|| !ReadId(cursor, delivery.message.sender) || !ReadId(cursor, delivery.message.recipient)
				|| !ReadOptionalId(cursor, delivery.message.replyTo) || !cursor.ReadU8(delivery.message.hopCount)
				|| !cursor.ReadU64(delivery.deliveryAttempt)
				|| !ReadString(cursor, delivery.message.type, kHarnessBridgeMaximumMessageTypeBytes)
				|| !ReadBytes32(cursor, delivery.message.payload, kMaximumCodecBytes)) return false;
			decoded.push_back(std::move(delivery));
		}
		if (!cursor.AtEnd()) return false;
		deliveries = std::move(decoded);
		return true;
	} catch (...) {
		return false;
	}
}

std::optional<std::vector<std::uint8_t>> EncodeHarnessBridgeRun(
	const std::optional<HarnessRunResult>& run)
{
	return EncodeRun(run);
}

bool DecodeHarnessBridgeRun(
	const std::span<const std::uint8_t> bytes, std::optional<HarnessRunResult>& run) noexcept
{
	try {
		if (bytes.size() > kMaximumCodecBytes) return false;
		Cursor cursor(bytes);
		std::uint8_t version = 0;
		std::uint8_t present = 0;
		if (!cursor.ReadU8(version) || version != kHarnessBridgeBrokerCodecVersion || !cursor.ReadU8(present)
			|| present > 1) return false;
		if (present == 0) {
			if (!cursor.AtEnd()) return false;
			run.reset();
			return true;
		}
		HarnessRunResult decoded;
		std::uint8_t status = 0;
		std::uint32_t exitCode = 0;
		if (!ReadId(cursor, decoded.runId) || !cursor.ReadU8(status) || status > static_cast<std::uint8_t>(EHarnessRunTerminalStatus::HarnessExited)
			|| !cursor.ReadU32(exitCode) || !cursor.ReadU64(decoded.completedAtTick) || !cursor.AtEnd()) return false;
		decoded.status = static_cast<EHarnessRunTerminalStatus>(status);
		decoded.exitCode = static_cast<std::int32_t>(exitCode);
		run = decoded;
		return true;
	} catch (...) {
		return false;
	}
}

std::optional<std::vector<std::uint8_t>> EncodeHarnessBridgeTmuxArgv(const std::vector<std::string>& argv)
{
	try {
		if (argv.empty() || argv.size() > kHarnessBridgeMaximumTmuxArgc) return std::nullopt;
		std::vector<std::uint8_t> result{ kCodecVersion };
		AppendU16(result, static_cast<std::uint16_t>(argv.size()));
		for (const auto& argument : argv) {
			if (!AppendString(result, argument, kHarnessBridgeMaximumTmuxArgBytes)) return std::nullopt;
			if (result.size() > kMaximumCodecBytes) return std::nullopt;
		}
		return result;
	} catch (...) {
		return std::nullopt;
	}
}

HarnessBridgePayloadDecodeResult DecodeHarnessBridgeTmuxArgv(const std::span<const std::uint8_t> bytes)
{
	HarnessBridgePayloadDecodeResult result;
	try {
		if (bytes.size() > kMaximumCodecBytes) return result;
		Cursor cursor(bytes);
		std::uint8_t version = 0;
		std::uint16_t count = 0;
		if (!cursor.ReadU8(version)) return result;
		if (version != kCodecVersion) { result.outcome = EHarnessBridgePayloadDecodeOutcome::UnsupportedVersion; return result; }
		if (!cursor.ReadU16(count)) return result;
		if (count == 0 || count > kHarnessBridgeMaximumTmuxArgc) {
			result.outcome = EHarnessBridgePayloadDecodeOutcome::TooManyArguments;
			return result;
		}
		result.argv.reserve(count);
		for (std::uint16_t index = 0; index < count; ++index) {
			std::string argument;
			if (!ReadString(cursor, argument, kHarnessBridgeMaximumTmuxArgBytes)) {
				result.outcome = EHarnessBridgePayloadDecodeOutcome::FieldTooLarge;
				return result;
			}
			result.argv.push_back(std::move(argument));
		}
		if (!cursor.AtEnd()) { result.outcome = EHarnessBridgePayloadDecodeOutcome::TrailingBytes; return result; }
		result.outcome = EHarnessBridgePayloadDecodeOutcome::Decoded;
		return result;
	} catch (...) {
		result.argv.clear();
		return result;
	}
}

std::optional<std::vector<std::uint8_t>> EncodeHarnessBridgeTmuxResponse(
	const HarnessBridgeTmuxResponse& response)
{
	try {
		const auto valid = [](const std::string& value) {
			return ValidText(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(value.data()), value.size()),
				kMaximumCodecBytes);
		};
		if ((!response.stdoutText.empty() && !valid(response.stdoutText))
			|| (!response.stderrText.empty() && !valid(response.stderrText))
			|| response.stdoutText.size() > kMaximumCodecBytes || response.stderrText.size() > kMaximumCodecBytes) return std::nullopt;
		std::vector<std::uint8_t> result{ kCodecVersion };
		AppendI32(result, response.exitCode);
		AppendU32(result, static_cast<std::uint32_t>(response.stdoutText.size()));
		result.insert(result.end(), response.stdoutText.begin(), response.stdoutText.end());
		AppendU32(result, static_cast<std::uint32_t>(response.stderrText.size()));
		result.insert(result.end(), response.stderrText.begin(), response.stderrText.end());
		return result.size() <= kMaximumCodecBytes ? std::optional{ std::move(result) } : std::nullopt;
	} catch (...) {
		return std::nullopt;
	}
}

std::optional<HarnessBridgeTmuxResponse> DecodeHarnessBridgeTmuxResponse(
	const std::span<const std::uint8_t> bytes)
{
	try {
		Cursor cursor(bytes);
		std::uint8_t version = 0;
		std::uint32_t ignored = 0;
		HarnessBridgeTmuxResponse result;
		std::vector<std::uint8_t> output;
		if (!cursor.ReadU8(version) || version != kCodecVersion || !cursor.ReadU32(ignored)
			|| !ReadBytes32(cursor, output, kMaximumCodecBytes)) return std::nullopt;
		if (!output.empty() && !ValidText(output, kMaximumCodecBytes)) return std::nullopt;
		result.exitCode = static_cast<std::int32_t>(ignored);
		result.stdoutText.assign(reinterpret_cast<const char*>(output.data()), output.size());
		if (!ReadBytes32(cursor, output, kMaximumCodecBytes) || !cursor.AtEnd()) return std::nullopt;
		if (!output.empty() && !ValidText(output, kMaximumCodecBytes)) return std::nullopt;
		result.stderrText.assign(reinterpret_cast<const char*>(output.data()), output.size());
		return result;
	} catch (...) {
		return std::nullopt;
	}
}

CHarnessBridgeOperationDispatcher::CHarnessBridgeOperationDispatcher(
	CHarnessBridgeBroker* broker, terminal::tmux::ITmuxRuntimePort* tmuxRuntime,
	HarnessBridgeOperationDispatcherOptions options)
	: m_broker(broker), m_tmuxRuntime(tmuxRuntime), m_options(std::move(options))
{
	if (m_options.maximumResponseBytes == 0 || m_options.maximumResponseBytes > kHarnessBridgeMaximumPayloadBytes) {
		m_options.maximumResponseBytes = kHarnessBridgeMaximumPayloadBytes;
	}
	if (m_options.tmuxLimits.maximumOutputBytes > m_options.maximumResponseBytes) {
		m_options.tmuxLimits.maximumOutputBytes = m_options.maximumResponseBytes;
	}
}

HarnessBridgeOperationResponseDto CHarnessBridgeOperationDispatcher::Dispatch(
	const HarnessBridgeSessionContext&, const HarnessBridgeOperationRequestDto& request)
{
	std::uint64_t generation = 0;
	{
		std::lock_guard lock(m_mutex);
		generation = m_cancelGeneration;
	}
	if (request.payload.size() > kHarnessBridgeMaximumPayloadBytes
		|| (request.deadline != std::chrono::steady_clock::time_point{} && std::chrono::steady_clock::now() >= request.deadline)) {
		return { request.payload.size() > kHarnessBridgeMaximumPayloadBytes
			? EHarnessTerminalStatus::ResourceExhausted : EHarnessTerminalStatus::DeadlineExceeded, {} };
	}
	try {
		HarnessBridgeOperationResponseDto result;
		switch (request.operation) {
		case EHarnessOperationKind::ExecuteTmux: result = DispatchTmux(request); break;
		case EHarnessOperationKind::RegisterEndpoint:
		case EHarnessOperationKind::ListEndpoints:
		case EHarnessOperationKind::SendEndpointMessage:
		case EHarnessOperationKind::ReceiveMessages:
		case EHarnessOperationKind::AcknowledgeMessage:
		case EHarnessOperationKind::PublishRun:
		case EHarnessOperationKind::WaitRun: result = DispatchBroker(request.target, request); break;
		default: result = { EHarnessTerminalStatus::OperationUnknown, {} }; break;
		}
		if (Cancelled(generation)) return { EHarnessTerminalStatus::Cancelled, {} };
		if (request.deadline != std::chrono::steady_clock::time_point{} && std::chrono::steady_clock::now() >= request.deadline) {
			return { EHarnessTerminalStatus::DeadlineExceeded, {} };
		}
		return result;
	} catch (...) {
		return { EHarnessTerminalStatus::InternalError, {} };
	}
}

void CHarnessBridgeOperationDispatcher::Cancel(std::uint64_t) noexcept
{
	std::lock_guard lock(m_mutex);
	++m_cancelGeneration;
}

HarnessBridgeOperationResponseDto CHarnessBridgeOperationDispatcher::DispatchTmux(
	const HarnessBridgeOperationRequestDto& request)
{
	if (m_tmuxRuntime == nullptr) return { EHarnessTerminalStatus::UnsupportedCapability, {} };
	const auto decoded = DecodeHarnessBridgeTmuxArgv(request.payload);
	if (decoded.outcome != EHarnessBridgePayloadDecodeOutcome::Decoded) return { EHarnessTerminalStatus::InvalidRequest, {} };
	auto tmuxLimits = m_options.tmuxLimits;
	if (request.deadline != std::chrono::steady_clock::time_point{}) {
		const auto remaining = std::chrono::duration_cast<std::chrono::seconds>(
			request.deadline - std::chrono::steady_clock::now());
		tmuxLimits.maximumWait = (std::min)(tmuxLimits.maximumWait, (std::max)(std::chrono::seconds::zero(), remaining));
	}
	const auto response = terminal::tmux::TmuxCli::Run(decoded.argv, *m_tmuxRuntime,
		m_options.tmuxProfile, tmuxLimits);
	HarnessBridgeTmuxResponse encodedResponse{ response.exitCode, response.stdoutText, response.stderrText };
	const auto payload = EncodeHarnessBridgeTmuxResponse(encodedResponse);
	if (!payload) return { EHarnessTerminalStatus::ResourceExhausted, {} };
	if (payload->size() > m_options.maximumResponseBytes) return { EHarnessTerminalStatus::ResourceExhausted, {} };
	return { response.Succeeded() ? EHarnessTerminalStatus::Succeeded : MapTmuxFailure(response.stderrText), *payload };
}

HarnessBridgeOperationResponseDto CHarnessBridgeOperationDispatcher::DispatchBroker(
	const HarnessBridgeTargetDescriptor& authority,
	const HarnessBridgeOperationRequestDto& request)
{
	if (m_broker == nullptr || m_broker->State() != EHarnessBrokerState::Running) return { EHarnessTerminalStatus::ServerStopping, {} };
	HarnessBrokerResult result;
	switch (request.operation) {
	case EHarnessOperationKind::RegisterEndpoint: {
		HarnessEndpointRegistration registration;
		if (!DecodeEndpointRegistrationImpl(request.payload, registration)) return { EHarnessTerminalStatus::InvalidRequest, {} };
		result = m_broker->RegisterEndpoint(registration, authority);
		break;
	}
	case EHarnessOperationKind::ListEndpoints: {
		if (!request.payload.empty()) return { EHarnessTerminalStatus::InvalidRequest, {} };
		const auto payload = EncodeEndpoints(m_broker->ListEndpoints());
		return payload ? HarnessBridgeOperationResponseDto{ EHarnessTerminalStatus::Succeeded, *payload }
			: HarnessBridgeOperationResponseDto{ EHarnessTerminalStatus::ResourceExhausted, {} };
	}
	case EHarnessOperationKind::SendEndpointMessage: {
		HarnessMessage message;
		if (!DecodeMessageImpl(request.payload, message)) return { EHarnessTerminalStatus::InvalidRequest, {} };
		message.deadline = request.deadline;
		result = m_broker->SendEndpointMessage(message, authority);
		break;
	}
	case EHarnessOperationKind::ReceiveMessages: {
		Cursor cursor(request.payload);
		std::uint8_t version = 0;
		std::uint16_t maximum = 0;
		HarnessEndpointId endpoint;
		if (!cursor.ReadU8(version) || version != kCodecVersion || !ReadId(cursor, endpoint)
			|| !cursor.ReadU16(maximum) || maximum == 0 || !cursor.AtEnd()) return { EHarnessTerminalStatus::InvalidRequest, {} };
		result = m_broker->ReceiveMessages(endpoint, maximum, request.deadline, authority);
		if (result.status == EHarnessBrokerStatus::Succeeded) {
			const auto payload = EncodeDeliveries(result.messages);
			return payload ? HarnessBridgeOperationResponseDto{ EHarnessTerminalStatus::Succeeded, *payload }
				: HarnessBridgeOperationResponseDto{ EHarnessTerminalStatus::ResourceExhausted, {} };
		}
		break;
	}
	case EHarnessOperationKind::AcknowledgeMessage: {
		Cursor cursor(request.payload);
		std::uint8_t version = 0;
		HarnessEndpointId endpoint;
		HarnessMessageId message;
		if (!cursor.ReadU8(version) || version != kCodecVersion || !ReadId(cursor, endpoint)
			|| !ReadId(cursor, message) || !cursor.AtEnd()) return { EHarnessTerminalStatus::InvalidRequest, {} };
		result = m_broker->AcknowledgeMessage(endpoint, message, authority);
		break;
	}
	case EHarnessOperationKind::PublishRun: {
		bool begin = false;
		HarnessRunResult run;
		if (!DecodeRunPublishImpl(request.payload, begin, run)) return { EHarnessTerminalStatus::InvalidRequest, {} };
		result = begin ? HarnessBrokerResult{ m_broker->BeginRun(run.runId) } : m_broker->PublishRunResult(run);
		break;
	}
	case EHarnessOperationKind::WaitRun: {
		Cursor cursor(request.payload);
		std::uint8_t version = 0;
		HarnessRunId run;
		if (!cursor.ReadU8(version) || version != kCodecVersion || !ReadId(cursor, run) || !cursor.AtEnd()) return { EHarnessTerminalStatus::InvalidRequest, {} };
		result = m_broker->WaitRun(run, request.deadline);
		if (result.status == EHarnessBrokerStatus::Succeeded) {
			const auto payload = EncodeRun(result.run);
			return payload ? HarnessBridgeOperationResponseDto{ EHarnessTerminalStatus::Succeeded, *payload }
				: HarnessBridgeOperationResponseDto{ EHarnessTerminalStatus::ResourceExhausted, {} };
		}
		break;
	}
	default:
		return { EHarnessTerminalStatus::OperationUnknown, {} };
	}
	return BrokerFailure(result.status);
}

HarnessBridgeOperationResponseDto CHarnessBridgeOperationDispatcher::BrokerFailure(const EHarnessBrokerStatus status) noexcept
{
	return { MapBrokerStatus(status), {} };
}

EHarnessTerminalStatus CHarnessBridgeOperationDispatcher::MapTmuxFailure(const std::string_view diagnostic) noexcept
{
	if (diagnostic.find("invalid-usage") != std::string_view::npos) return EHarnessTerminalStatus::InvalidRequest;
	if (diagnostic.find("unsupported") != std::string_view::npos) return EHarnessTerminalStatus::UnsupportedTmuxSurface;
	if (diagnostic.find("target-missing") != std::string_view::npos) return EHarnessTerminalStatus::TargetMissing;
	if (diagnostic.find("topology-changed") != std::string_view::npos) return EHarnessTerminalStatus::TopologyChanged;
	if (diagnostic.find("deadline-exceeded") != std::string_view::npos) return EHarnessTerminalStatus::DeadlineExceeded;
	if (diagnostic.find("resource-exhausted") != std::string_view::npos) return EHarnessTerminalStatus::ResourceExhausted;
	if (diagnostic.find("access-denied") != std::string_view::npos) return EHarnessTerminalStatus::AccessDenied;
	if (diagnostic.find("ambiguous") != std::string_view::npos) return EHarnessTerminalStatus::Ambiguous;
	return EHarnessTerminalStatus::InternalError;
}

EHarnessTerminalStatus CHarnessBridgeOperationDispatcher::MapBrokerStatus(const EHarnessBrokerStatus status) noexcept
{
	switch (status) {
	case EHarnessBrokerStatus::Accepted:
	case EHarnessBrokerStatus::Succeeded: return EHarnessTerminalStatus::Succeeded;
	case EHarnessBrokerStatus::InvalidRequest: return EHarnessTerminalStatus::InvalidRequest;
	case EHarnessBrokerStatus::UnknownEndpoint: return EHarnessTerminalStatus::TargetMissing;
	case EHarnessBrokerStatus::UnknownMessage:
	case EHarnessBrokerStatus::UnknownRun: return EHarnessTerminalStatus::OperationUnknown;
	case EHarnessBrokerStatus::AccessDenied: return EHarnessTerminalStatus::AccessDenied;
	case EHarnessBrokerStatus::ResourceExhausted: return EHarnessTerminalStatus::ResourceExhausted;
	case EHarnessBrokerStatus::AlreadyTerminal: return EHarnessTerminalStatus::AlreadyTerminal;
	case EHarnessBrokerStatus::DeadlineExceeded: return EHarnessTerminalStatus::DeadlineExceeded;
	case EHarnessBrokerStatus::Cancelled: return EHarnessTerminalStatus::Cancelled;
	case EHarnessBrokerStatus::BrokerStopping: return EHarnessTerminalStatus::ServerStopping;
	case EHarnessBrokerStatus::Duplicate:
	case EHarnessBrokerStatus::Conflict: return EHarnessTerminalStatus::Conflict;
	}
	return EHarnessTerminalStatus::InternalError;
}

bool CHarnessBridgeOperationDispatcher::Cancelled(const std::uint64_t generation) const noexcept
{
	std::lock_guard lock(m_mutex);
	return m_cancelGeneration != generation;
}

} // namespace platform::harnessbridge

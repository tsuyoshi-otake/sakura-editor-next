/*! @file */
#include <sakura/harnessbridge/HarnessBridgeProtocol.h>

#include <algorithm>
#include <cstring>

namespace platform::harnessbridge {
namespace {

template<typename T>
void PutLe(std::vector<std::uint8_t>& out, T value)
{
	for (std::size_t i = 0; i < sizeof(T); ++i) out.push_back(static_cast<std::uint8_t>(value >> (i * 8)));
}

template<typename T>
T GetLe(const std::uint8_t* data) noexcept
{
	T value = 0;
	for (std::size_t i = 0; i < sizeof(T); ++i) value |= static_cast<T>(data[i]) << (i * 8);
	return value;
}

bool IsKnownKind(const std::uint16_t kind) noexcept
{
	switch (static_cast<EHarnessBridgeFrameKind>(kind)) {
	case EHarnessBridgeFrameKind::Hello:
	case EHarnessBridgeFrameKind::Challenge:
	case EHarnessBridgeFrameKind::Authenticate:
	case EHarnessBridgeFrameKind::Ready:
	case EHarnessBridgeFrameKind::OperationRequest:
	case EHarnessBridgeFrameKind::OperationResponse:
	case EHarnessBridgeFrameKind::CancelRequest:
	case EHarnessBridgeFrameKind::CancelAck:
	case EHarnessBridgeFrameKind::MessageEvent:
	case EHarnessBridgeFrameKind::MessageAck:
	case EHarnessBridgeFrameKind::Error:
		return true;
	default:
		return false;
	}
}

bool IsValidFlags(const HarnessBridgeFrameHeader& frameHeader) noexcept
{
	constexpr auto directions = EHarnessBridgeFrameFlags::Request
		| EHarnessBridgeFrameFlags::Response | EHarnessBridgeFrameFlags::Event;
	const auto raw = static_cast<std::uint16_t>(frameHeader.flags);
	if ((raw & ~static_cast<std::uint16_t>(EHarnessBridgeFrameFlags::Request
		| EHarnessBridgeFrameFlags::Response | EHarnessBridgeFrameFlags::Event
		| EHarnessBridgeFrameFlags::Terminal | EHarnessBridgeFrameFlags::More))) return false;
	const auto direction = frameHeader.flags & directions;
	const auto directionRaw = static_cast<std::uint16_t>(direction);
	if (directionRaw == 0 || (directionRaw & (directionRaw - 1)) != 0) return false;
	if (static_cast<std::uint16_t>(frameHeader.flags & EHarnessBridgeFrameFlags::Terminal)
		&& static_cast<std::uint16_t>(frameHeader.flags & EHarnessBridgeFrameFlags::More)) return false;
	if (static_cast<std::uint16_t>(frameHeader.flags & EHarnessBridgeFrameFlags::Terminal)
		&& direction != EHarnessBridgeFrameFlags::Response) return false;
	return true;
}

bool IsSingletonTag(const std::uint16_t tag) noexcept
{
	return tag != 0;
}

std::size_t MaximumFieldBytes(const std::uint16_t tag) noexcept
{
	// Field tag 7 is the binary operation payload; text fields retain 64 KiB.
	return tag == 7
		? kHarnessBridgeMaximumPayloadBytes : kHarnessBridgeMaximumUtf8Bytes;
}

} // namespace

HarnessBridgeEncodeResult EncodeHarnessBridgeFrame(const HarnessBridgeFrame& frame)
{
	HarnessBridgeEncodeResult result;
	if (frame.header.majorVersion != kHarnessBridgeMajorVersion
		|| frame.header.requestId == 0
		|| !IsKnownKind(static_cast<std::uint16_t>(frame.header.kind))
		|| !IsValidFlags(frame.header)
		|| (frame.header.bridgeEpoch == 0 && frame.header.kind != EHarnessBridgeFrameKind::Hello)) {
		result.outcome = EHarnessBridgeEncodeOutcome::InvalidHeader;
		return result;
	}
	if (frame.payload.size() > kHarnessBridgeMaximumFrameBytes - kHarnessBridgeHeaderBytes) {
		result.outcome = EHarnessBridgeEncodeOutcome::PayloadTooLarge;
		return result;
	}
	result.bytes.reserve(kHarnessBridgeLengthPrefixBytes + kHarnessBridgeHeaderBytes + frame.payload.size());
	PutLe<std::uint32_t>(result.bytes, static_cast<std::uint32_t>(kHarnessBridgeHeaderBytes + frame.payload.size()));
	PutLe<std::uint32_t>(result.bytes, kHarnessBridgeMagic);
	PutLe<std::uint16_t>(result.bytes, frame.header.majorVersion);
	PutLe<std::uint16_t>(result.bytes, frame.header.minorVersion);
	PutLe<std::uint16_t>(result.bytes, static_cast<std::uint16_t>(frame.header.kind));
	PutLe<std::uint16_t>(result.bytes, static_cast<std::uint16_t>(frame.header.flags));
	PutLe<std::uint64_t>(result.bytes, frame.header.requestId);
	PutLe<std::uint64_t>(result.bytes, frame.header.bridgeEpoch);
	result.bytes.insert(result.bytes.end(), frame.payload.begin(), frame.payload.end());
	result.outcome = EHarnessBridgeEncodeOutcome::Encoded;
	return result;
}

CHarnessBridgeFrameDecoder::CHarnessBridgeFrameDecoder(const std::size_t maximumFrameBytes) noexcept
	: m_maximumFrameBytes((std::max)(maximumFrameBytes, kHarnessBridgeHeaderBytes))
{
}

HarnessBridgeDecodeResult CHarnessBridgeFrameDecoder::Feed(const std::span<const std::uint8_t> bytes)
{
	HarnessBridgeDecodeResult result;
	if (m_failed) {
		result.outcome = EHarnessBridgeDecodeOutcome::MalformedFrame;
		return result;
	}
	m_buffer.insert(m_buffer.end(), bytes.begin(), bytes.end());
	while (true) {
		if (m_buffer.size() < kHarnessBridgeLengthPrefixBytes) {
			result.outcome = result.frames.empty() ? EHarnessBridgeDecodeOutcome::NeedMoreData : EHarnessBridgeDecodeOutcome::Decoded;
			return result;
		}
		const auto frameBytes = GetLe<std::uint32_t>(m_buffer.data());
		if (frameBytes == 0) {
			m_failed = true;
			result.outcome = EHarnessBridgeDecodeOutcome::ZeroLengthFrame;
			return result;
		}
		if (frameBytes < kHarnessBridgeHeaderBytes || frameBytes > m_maximumFrameBytes) {
			m_failed = true;
			result.outcome = EHarnessBridgeDecodeOutcome::OversizeFrame;
			return result;
		}
		const std::size_t totalBytes = kHarnessBridgeLengthPrefixBytes + frameBytes;
		if (m_buffer.size() < totalBytes) {
			result.outcome = result.frames.empty() ? EHarnessBridgeDecodeOutcome::NeedMoreData : EHarnessBridgeDecodeOutcome::Decoded;
			return result;
		}
		const auto* header = m_buffer.data() + kHarnessBridgeLengthPrefixBytes;
		if (GetLe<std::uint32_t>(header) != kHarnessBridgeMagic) {
			m_failed = true;
			result.outcome = EHarnessBridgeDecodeOutcome::MalformedFrame;
			return result;
		}
		HarnessBridgeFrame frame;
		frame.header.majorVersion = GetLe<std::uint16_t>(header + 4);
		frame.header.minorVersion = GetLe<std::uint16_t>(header + 6);
		frame.header.kind = static_cast<EHarnessBridgeFrameKind>(GetLe<std::uint16_t>(header + 8));
		frame.header.flags = static_cast<EHarnessBridgeFrameFlags>(GetLe<std::uint16_t>(header + 10));
		frame.header.requestId = GetLe<std::uint64_t>(header + 12);
		frame.header.bridgeEpoch = GetLe<std::uint64_t>(header + 20);
		if (frame.header.majorVersion != kHarnessBridgeMajorVersion) {
			m_failed = true;
			result.outcome = EHarnessBridgeDecodeOutcome::UnsupportedMajorVersion;
			return result;
		}
		if (!IsKnownKind(static_cast<std::uint16_t>(frame.header.kind))) {
			m_failed = true;
			result.outcome = EHarnessBridgeDecodeOutcome::UnknownKind;
			return result;
		}
		const auto flags = static_cast<std::uint16_t>(frame.header.flags);
		if (flags & 0xffe0u) {
			m_failed = true;
			result.outcome = EHarnessBridgeDecodeOutcome::UnknownRequiredFlags;
			return result;
		}
		if (frame.header.requestId == 0) {
			m_failed = true;
			result.outcome = EHarnessBridgeDecodeOutcome::InvalidRequestId;
			return result;
		}
		if (frame.header.bridgeEpoch == 0 && frame.header.kind != EHarnessBridgeFrameKind::Hello) {
			m_failed = true;
			result.outcome = EHarnessBridgeDecodeOutcome::InvalidEpoch;
			return result;
		}
		if (!IsValidFlags(frame.header)) {
			m_failed = true;
			result.outcome = EHarnessBridgeDecodeOutcome::InvalidDirection;
			return result;
		}
		const auto payloadSize = frameBytes - kHarnessBridgeHeaderBytes;
		frame.payload.assign(header + kHarnessBridgeHeaderBytes, header + kHarnessBridgeHeaderBytes + payloadSize);
		result.frames.push_back(std::move(frame));
		m_buffer.erase(m_buffer.begin(), m_buffer.begin() + static_cast<std::ptrdiff_t>(totalBytes));
	}
}

void CHarnessBridgeFrameDecoder::Reset() noexcept
{
	m_buffer.clear();
	m_failed = false;
}

std::optional<std::vector<std::uint8_t>> EncodeHarnessBridgeFields(const HarnessBridgeFields& fields)
{
	if (fields.size() > kHarnessBridgeMaximumFieldCount) return std::nullopt;
	std::vector<std::uint8_t> result;
	std::vector<std::uint16_t> tags;
	for (const auto& field : fields) {
		if (!IsSingletonTag(field.tag) || field.value.size() > MaximumFieldBytes(field.tag)
			|| std::find(tags.begin(), tags.end(), field.tag) != tags.end()) return std::nullopt;
		tags.push_back(field.tag);
		PutLe<std::uint16_t>(result, field.tag);
		PutLe<std::uint32_t>(result, static_cast<std::uint32_t>(field.value.size()));
		result.insert(result.end(), field.value.begin(), field.value.end());
		if (result.size() > kHarnessBridgeMaximumFrameBytes - kHarnessBridgeHeaderBytes) return std::nullopt;
	}
	return result;
}

HarnessBridgeFieldDecodeResult DecodeHarnessBridgeFields(const std::span<const std::uint8_t> bytes)
{
	HarnessBridgeFieldDecodeResult result;
	std::size_t offset = 0;
	while (offset < bytes.size()) {
		if (result.fields.size() >= kHarnessBridgeMaximumFieldCount || bytes.size() - offset < 6) {
			result.outcome = result.fields.size() >= kHarnessBridgeMaximumFieldCount
				? EHarnessBridgeFieldDecodeOutcome::TooManyFields
				: EHarnessBridgeFieldDecodeOutcome::MalformedField;
			return result;
		}
		const auto tag = GetLe<std::uint16_t>(bytes.data() + offset);
		const auto size = GetLe<std::uint32_t>(bytes.data() + offset + 2);
		offset += 6;
		if (!IsSingletonTag(tag)) { result.outcome = EHarnessBridgeFieldDecodeOutcome::InvalidTag; return result; }
		if (size > MaximumFieldBytes(tag) || size > bytes.size() - offset) {
			result.outcome = size > MaximumFieldBytes(tag)
				? EHarnessBridgeFieldDecodeOutcome::FieldTooLarge
				: EHarnessBridgeFieldDecodeOutcome::MalformedField;
			return result;
		}
		if (std::find_if(result.fields.begin(), result.fields.end(), [tag](const auto& field) { return field.tag == tag; }) != result.fields.end()) {
			result.outcome = EHarnessBridgeFieldDecodeOutcome::DuplicateTag;
			return result;
		}
		result.fields.push_back({ tag, std::vector<std::uint8_t>(bytes.begin() + static_cast<std::ptrdiff_t>(offset), bytes.begin() + static_cast<std::ptrdiff_t>(offset + size)) });
		offset += size;
	}
	return result;
}

bool AddHarnessBridgeBytesField(HarnessBridgeFields& fields, const EHarnessBridgeFieldTag tag,
	const std::span<const std::uint8_t> value)
{
	if (fields.size() >= kHarnessBridgeMaximumFieldCount || value.size() > MaximumFieldBytes(static_cast<std::uint16_t>(tag))
		|| FindHarnessBridgeField(fields, tag)) return false;
	fields.push_back({ static_cast<std::uint16_t>(tag), std::vector<std::uint8_t>(value.begin(), value.end()) });
	return true;
}

bool AddHarnessBridgeUtf8Field(HarnessBridgeFields& fields, const EHarnessBridgeFieldTag tag,
	const std::string_view value)
{
	if (value.size() > kHarnessBridgeMaximumUtf8Bytes || !IsValidHarnessBridgeUtf8(
		std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(value.data()), value.size()))) return false;
	return AddHarnessBridgeBytesField(fields, tag, std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(value.data()), value.size()));
}

const HarnessBridgeField* FindHarnessBridgeField(const HarnessBridgeFields& fields, const EHarnessBridgeFieldTag tag) noexcept
{
	const auto numericTag = static_cast<std::uint16_t>(tag);
	const auto it = std::find_if(fields.begin(), fields.end(), [numericTag](const auto& field) { return field.tag == numericTag; });
	return it == fields.end() ? nullptr : &*it;
}

std::optional<std::string> GetHarnessBridgeUtf8Field(const HarnessBridgeFields& fields,
	const EHarnessBridgeFieldTag tag, EHarnessBridgeFieldDecodeOutcome* failure)
{
	if (failure) *failure = EHarnessBridgeFieldDecodeOutcome::Decoded;
	const auto* field = FindHarnessBridgeField(fields, tag);
	if (!field) return std::nullopt;
	if (field->value.size() > kHarnessBridgeMaximumUtf8Bytes || !IsValidHarnessBridgeUtf8(field->value)) {
		if (failure) *failure = EHarnessBridgeFieldDecodeOutcome::InvalidUtf8;
		return std::nullopt;
	}
	return std::string(field->value.begin(), field->value.end());
}

bool IsValidHarnessBridgeUtf8(const std::span<const std::uint8_t> bytes) noexcept
{
	std::size_t i = 0;
	while (i < bytes.size()) {
		const auto first = bytes[i++];
		if (first == 0) return false;
		std::size_t continuation = 0;
		std::uint32_t codepoint = 0;
		if (first <= 0x7f) continue;
		if (first >= 0xc2 && first <= 0xdf) { continuation = 1; codepoint = first & 0x1f; }
		else if (first >= 0xe0 && first <= 0xef) { continuation = 2; codepoint = first & 0x0f; }
		else if (first >= 0xf0 && first <= 0xf4) { continuation = 3; codepoint = first & 0x07; }
		else return false;
		for (std::size_t j = 0; j < continuation; ++j) {
			if (i == bytes.size() || (bytes[i] & 0xc0) != 0x80) return false;
			codepoint = (codepoint << 6) | (bytes[i++] & 0x3f);
		}
		if ((continuation == 2 && codepoint < 0x800) || (continuation == 3 && codepoint < 0x10000)
			|| codepoint > 0x10ffff || (codepoint >= 0xd800 && codepoint <= 0xdfff)) return false;
	}
	return true;
}

} // namespace platform::harnessbridge

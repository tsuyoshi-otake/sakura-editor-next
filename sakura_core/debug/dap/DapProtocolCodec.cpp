/*! @file @brief Strict, bounded Debug Adapter Protocol transport codec. */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"

#include "debug/dap/DapProtocolCodec.h"

#include <charconv>
#include <cctype>
#include <limits>
#include <map>
#include <type_traits>
#include <utility>

namespace debug::dap {
namespace {

enum class EJsonValueKind : std::uint8_t { Null, Boolean, Number, String, Array, Object };

struct JsonMember final {
	std::string name;
	EJsonValueKind kind = EJsonValueKind::Null;
	std::string stringValue;
	std::string_view rawValue;
};

enum class EDecodeResult : std::uint8_t {
	Succeeded,
	InvalidJson,
	InvalidEnvelope,
};

bool IsJsonWhitespace(char value) noexcept
{
	return value == ' ' || value == '\t' || value == '\r' || value == '\n';
}

bool ValidateUtf8(std::string_view input) noexcept
{
	for (std::size_t index = 0; index < input.size();) {
		const auto lead = static_cast<unsigned char>(input[index]);
		if (lead <= 0x7fU) { ++index; continue; }
		std::size_t continuationCount = 0;
		std::uint32_t codePoint = 0;
		if (lead >= 0xc2U && lead <= 0xdfU) { continuationCount = 1; codePoint = lead & 0x1fU; }
		else if (lead >= 0xe0U && lead <= 0xefU) { continuationCount = 2; codePoint = lead & 0x0fU; }
		else if (lead >= 0xf0U && lead <= 0xf4U) { continuationCount = 3; codePoint = lead & 0x07U; }
		else return false;
		if (continuationCount >= input.size() - index) return false;
		for (std::size_t offset = 1; offset <= continuationCount; ++offset) {
			const auto continuation = static_cast<unsigned char>(input[index + offset]);
			if ((continuation & 0xc0U) != 0x80U) return false;
			codePoint = (codePoint << 6U) | (continuation & 0x3fU);
		}
		if ((continuationCount == 2 && codePoint < 0x800U) || (continuationCount == 3 && codePoint < 0x10000U)
			|| (codePoint >= 0xd800U && codePoint <= 0xdfffU) || codePoint > 0x10ffffU) return false;
		index += continuationCount + 1;
	}
	return true;
}

void AppendUtf8(std::uint32_t codePoint, std::string& value)
{
	if (codePoint <= 0x7fU) value.push_back(static_cast<char>(codePoint));
	else if (codePoint <= 0x7ffU) {
		value.push_back(static_cast<char>(0xc0U | (codePoint >> 6U)));
		value.push_back(static_cast<char>(0x80U | (codePoint & 0x3fU)));
	} else if (codePoint <= 0xffffU) {
		value.push_back(static_cast<char>(0xe0U | (codePoint >> 12U)));
		value.push_back(static_cast<char>(0x80U | ((codePoint >> 6U) & 0x3fU)));
		value.push_back(static_cast<char>(0x80U | (codePoint & 0x3fU)));
	} else {
		value.push_back(static_cast<char>(0xf0U | (codePoint >> 18U)));
		value.push_back(static_cast<char>(0x80U | ((codePoint >> 12U) & 0x3fU)));
		value.push_back(static_cast<char>(0x80U | ((codePoint >> 6U) & 0x3fU)));
		value.push_back(static_cast<char>(0x80U | (codePoint & 0x3fU)));
	}
}

class StrictJsonParser final {
public:
	StrictJsonParser(std::string_view input, std::size_t maximumDepth) : m_input(input), m_maximumDepth(maximumDepth) {}

	bool ParseRootObject(std::vector<JsonMember>& members)
	{
		SkipWhitespace();
		if (!Consume('{')) return false;
		SkipWhitespace();
		if (Consume('}')) { SkipWhitespace(); return AtEnd(); }
		while (true) {
			JsonMember member;
			if (!ParseString(member.name)) return false;
			if (!m_memberNames.emplace(member.name, true).second) return false;
			SkipWhitespace();
			if (!Consume(':')) return false;
			SkipWhitespace();
			const auto valueStart = m_position;
			if (!ParseValue(1, member.kind, &member.stringValue)) return false;
			member.rawValue = m_input.substr(valueStart, m_position - valueStart);
			members.emplace_back(std::move(member));
			SkipWhitespace();
			if (Consume('}')) { SkipWhitespace(); return AtEnd(); }
			if (!Consume(',')) return false;
			SkipWhitespace();
		}
	}

private:
	bool AtEnd() const noexcept { return m_position == m_input.size(); }
	char Peek() const noexcept { return AtEnd() ? '\0' : m_input[m_position]; }
	void SkipWhitespace() noexcept { while (!AtEnd() && IsJsonWhitespace(Peek())) ++m_position; }
	bool Consume(char expected) noexcept { if (Peek() != expected) return false; ++m_position; return true; }

	bool ParseValue(std::size_t depth, EJsonValueKind& kind, std::string* stringValue)
	{
		if (depth > m_maximumDepth || AtEnd()) return false;
		switch (Peek()) {
		case 'n': return ParseLiteral("null", EJsonValueKind::Null, kind);
		case 't': return ParseLiteral("true", EJsonValueKind::Boolean, kind);
		case 'f': return ParseLiteral("false", EJsonValueKind::Boolean, kind);
		case '"': kind = EJsonValueKind::String; return ParseString(*stringValue);
		case '[': kind = EJsonValueKind::Array; return ParseArray(depth);
		case '{': kind = EJsonValueKind::Object; return ParseObject(depth);
		default: kind = EJsonValueKind::Number; return ParseNumber();
		}
	}

	bool ParseLiteral(std::string_view literal, EJsonValueKind valueKind, EJsonValueKind& kind)
	{
		if (m_input.substr(m_position, literal.size()) != literal) return false;
		m_position += literal.size(); kind = valueKind; return true;
	}

	bool ParseString(std::string& value)
	{
		if (!Consume('"')) return false;
		value.clear();
		while (!AtEnd()) {
			const auto character = static_cast<unsigned char>(m_input[m_position++]);
			if (character == '"') return true;
			if (character < 0x20U) return false;
			if (character != '\\') { value.push_back(static_cast<char>(character)); continue; }
			if (AtEnd()) return false;
			switch (m_input[m_position++]) {
			case '"': value.push_back('"'); break;
			case '\\': value.push_back('\\'); break;
			case '/': value.push_back('/'); break;
			case 'b': value.push_back('\b'); break;
			case 'f': value.push_back('\f'); break;
			case 'n': value.push_back('\n'); break;
			case 'r': value.push_back('\r'); break;
			case 't': value.push_back('\t'); break;
			case 'u': if (!ParseUnicodeEscape(value)) return false; break;
			default: return false;
			}
		}
		return false;
	}

	bool ParseUnicodeEscape(std::string& value)
	{
		auto parseUnit = [this](std::uint16_t& unit) {
			if (m_input.size() - m_position < 4) return false;
			unit = 0;
			for (std::size_t offset = 0; offset < 4; ++offset) {
				const char digit = m_input[m_position++];
				unit = static_cast<std::uint16_t>(unit << 4U);
				if (digit >= '0' && digit <= '9') unit = static_cast<std::uint16_t>(unit | (digit - '0'));
				else if (digit >= 'a' && digit <= 'f') unit = static_cast<std::uint16_t>(unit | (digit - 'a' + 10));
				else if (digit >= 'A' && digit <= 'F') unit = static_cast<std::uint16_t>(unit | (digit - 'A' + 10));
				else return false;
			}
			return true;
		};
		std::uint16_t first = 0;
		if (!parseUnit(first)) return false;
		if (first >= 0xd800U && first <= 0xdbffU) {
			if (m_input.size() - m_position < 6 || m_input[m_position++] != '\\' || m_input[m_position++] != 'u') return false;
			std::uint16_t second = 0;
			if (!parseUnit(second) || second < 0xdc00U || second > 0xdfffU) return false;
			AppendUtf8(0x10000U + ((first - 0xd800U) << 10U) + (second - 0xdc00U), value);
			return true;
		}
		if (first >= 0xdc00U && first <= 0xdfffU) return false;
		AppendUtf8(first, value);
		return true;
	}

	bool ParseNumber()
	{
		const auto start = m_position;
		Consume('-');
		if (Consume('0')) { }
		else {
			if (Peek() < '1' || Peek() > '9') return false;
			do { ++m_position; } while (Peek() >= '0' && Peek() <= '9');
		}
		if (Consume('.')) {
			if (Peek() < '0' || Peek() > '9') return false;
			do { ++m_position; } while (Peek() >= '0' && Peek() <= '9');
		}
		if (Peek() == 'e' || Peek() == 'E') {
			++m_position; if (Peek() == '+' || Peek() == '-') ++m_position;
			if (Peek() < '0' || Peek() > '9') return false;
			do { ++m_position; } while (Peek() >= '0' && Peek() <= '9');
		}
		return m_position != start;
	}

	bool ParseArray(std::size_t depth)
	{
		if (!Consume('[')) return false;
		SkipWhitespace();
		if (Consume(']')) return true;
		while (true) {
			EJsonValueKind ignoredKind;
			std::string ignoredString;
			if (!ParseValue(depth + 1, ignoredKind, &ignoredString)) return false;
			SkipWhitespace();
			if (Consume(']')) return true;
			if (!Consume(',')) return false;
			SkipWhitespace();
		}
	}

	bool ParseObject(std::size_t depth)
	{
		if (!Consume('{')) return false;
		SkipWhitespace();
		if (Consume('}')) return true;
		std::map<std::string, bool, std::less<>> memberNames;
		while (true) {
			std::string ignoredKey;
			if (!ParseString(ignoredKey)) return false;
			if (!memberNames.emplace(std::move(ignoredKey), true).second) return false;
			SkipWhitespace();
			if (!Consume(':')) return false;
			SkipWhitespace();
			EJsonValueKind ignoredKind;
			std::string ignoredString;
			if (!ParseValue(depth + 1, ignoredKind, &ignoredString)) return false;
			SkipWhitespace();
			if (Consume('}')) return true;
			if (!Consume(',')) return false;
			SkipWhitespace();
		}
	}

	std::string_view m_input;
	std::size_t m_maximumDepth;
	std::size_t m_position = 0;
	std::map<std::string, bool, std::less<>> m_memberNames;
};

const JsonMember* FindMember(const std::vector<JsonMember>& members, std::string_view name)
{
	for (const auto& member : members) if (member.name == name) return &member;
	return nullptr;
}

bool PositiveSequence(const JsonMember* member, std::uint64_t& result) noexcept
{
	if (member == nullptr || member->kind != EJsonValueKind::Number || member->rawValue.empty()) return false;
	const auto [end, error] = std::from_chars(member->rawValue.data(), member->rawValue.data() + member->rawValue.size(), result);
	return error == std::errc{} && end == member->rawValue.data() + member->rawValue.size() && result != 0;
}

bool NonEmptyString(const JsonMember* member, std::string& result)
{
	if (member == nullptr || member->kind != EJsonValueKind::String || member->stringValue.empty()) return false;
	result = member->stringValue;
	return true;
}

EDecodeResult DecodeMessage(std::string_view body, std::size_t maximumDepth, DapMessage& message)
{
	std::vector<JsonMember> members;
	StrictJsonParser parser(body, maximumDepth);
	if (!parser.ParseRootObject(members)) return EDecodeResult::InvalidJson;
	const auto* type = FindMember(members, "type");
	if (type == nullptr || type->kind != EJsonValueKind::String) return EDecodeResult::InvalidEnvelope;
	std::uint64_t sequence = 0;
	if (!PositiveSequence(FindMember(members, "seq"), sequence)) return EDecodeResult::InvalidEnvelope;
	if (type->stringValue == "request") {
		DapRequest request;
		request.seq = sequence;
		if (!NonEmptyString(FindMember(members, "command"), request.command)) return EDecodeResult::InvalidEnvelope;
		if (const auto* arguments = FindMember(members, "arguments")) {
			if (arguments->kind != EJsonValueKind::Object) return EDecodeResult::InvalidEnvelope;
			request.argumentsJson = std::string(arguments->rawValue);
		}
		request.rawJson.assign(body);
		message = std::move(request);
		return EDecodeResult::Succeeded;
	}
	if (type->stringValue == "response") {
		DapResponse response;
		response.seq = sequence;
		if (!PositiveSequence(FindMember(members, "request_seq"), response.requestSeq)
			|| !NonEmptyString(FindMember(members, "command"), response.command)) return EDecodeResult::InvalidEnvelope;
		const auto* success = FindMember(members, "success");
		if (success == nullptr || success->kind != EJsonValueKind::Boolean) return EDecodeResult::InvalidEnvelope;
		response.success = success->rawValue == "true";
		if (const auto* text = FindMember(members, "message")) {
			if (text->kind != EJsonValueKind::String) return EDecodeResult::InvalidEnvelope;
			response.message = text->stringValue;
		}
		if (const auto* bodyMember = FindMember(members, "body")) {
			if (bodyMember->kind != EJsonValueKind::Object) return EDecodeResult::InvalidEnvelope;
			response.bodyJson = std::string(bodyMember->rawValue);
		}
		response.rawJson.assign(body);
		message = std::move(response);
		return EDecodeResult::Succeeded;
	}
	if (type->stringValue == "event") {
		DapEvent event;
		event.seq = sequence;
		if (!NonEmptyString(FindMember(members, "event"), event.event)) return EDecodeResult::InvalidEnvelope;
		if (const auto* bodyMember = FindMember(members, "body")) {
			if (bodyMember->kind != EJsonValueKind::Object) return EDecodeResult::InvalidEnvelope;
			event.bodyJson = std::string(bodyMember->rawValue);
		}
		event.rawJson.assign(body);
		message = std::move(event);
		return EDecodeResult::Succeeded;
	}
	return EDecodeResult::InvalidEnvelope;
}

void AppendJsonString(std::string_view value, std::string& json)
{
	json.push_back('"');
	for (const auto character : value) {
		switch (character) {
		case '"': json += "\\\""; break;
		case '\\': json += "\\\\"; break;
		case '\b': json += "\\b"; break;
		case '\f': json += "\\f"; break;
		case '\n': json += "\\n"; break;
		case '\r': json += "\\r"; break;
		case '\t': json += "\\t"; break;
		default:
			if (static_cast<unsigned char>(character) < 0x20U) {
				constexpr char hex[] = "0123456789abcdef";
				json += "\\u00";
				json.push_back(hex[(static_cast<unsigned char>(character) >> 4U) & 0x0fU]);
				json.push_back(hex[static_cast<unsigned char>(character) & 0x0fU]);
			} else json.push_back(character);
		}
	}
	json.push_back('"');
}

bool IsValidObject(std::string_view value, std::size_t maximumDepth)
{
	if (!ValidateUtf8(value)) return false;
	std::vector<JsonMember> members;
	StrictJsonParser parser(value, maximumDepth);
	return parser.ParseRootObject(members);
}

bool EncodeMessage(const DapMessage& message, std::size_t maximumDepth, std::string& json)
{
	json.clear();
	return std::visit([&](const auto& typed) -> bool {
		using T = std::decay_t<decltype(typed)>;
		if (typed.seq == 0) return false;
		json = "{\"seq\":" + std::to_string(typed.seq);
		if constexpr (std::is_same_v<T, DapRequest>) {
			if (typed.command.empty() || (typed.argumentsJson && !IsValidObject(*typed.argumentsJson, maximumDepth))) return false;
			json += ",\"type\":\"request\",\"command\":"; AppendJsonString(typed.command, json);
			if (typed.argumentsJson) json += ",\"arguments\":" + *typed.argumentsJson;
		} else if constexpr (std::is_same_v<T, DapResponse>) {
			if (typed.requestSeq == 0 || typed.command.empty() || (typed.bodyJson && !IsValidObject(*typed.bodyJson, maximumDepth))) return false;
			json += ",\"type\":\"response\",\"request_seq\":" + std::to_string(typed.requestSeq)
				+ ",\"success\":" + (typed.success ? "true" : "false") + ",\"command\":";
			AppendJsonString(typed.command, json);
			if (typed.message) { json += ",\"message\":"; AppendJsonString(*typed.message, json); }
			if (typed.bodyJson) json += ",\"body\":" + *typed.bodyJson;
		} else {
			if (typed.event.empty() || (typed.bodyJson && !IsValidObject(*typed.bodyJson, maximumDepth))) return false;
			json += ",\"type\":\"event\",\"event\":"; AppendJsonString(typed.event, json);
			if (typed.bodyJson) json += ",\"body\":" + *typed.bodyJson;
		}
		json += '}';
		return ValidateUtf8(json);
	}, message);
}

} // namespace

CDapProtocolCodec::CDapProtocolCodec(DapProtocolCodecLimits limits) noexcept
	: m_limits(limits)
{
	// Zero limits make every corresponding input fail explicitly instead of using an unbounded fallback.
}

DapProtocolCodecResult CDapProtocolCodec::Feed(std::string_view bytes, std::vector<DapMessage>& completedMessages)
{
	if (m_state == EDapProtocolCodecState::Stopped) return { EDapProtocolCodecStatus::Stopped, EDapProtocolCodecError::None, 0 };
	if (m_state == EDapProtocolCodecState::Failed) return { EDapProtocolCodecStatus::Failed, m_error, 0 };

	std::size_t completed = 0;
	for (std::size_t index = 0; index < bytes.size(); ++index) {
		const auto value = bytes[index];
		if (m_state == EDapProtocolCodecState::ReadingHeader) {
			if (m_header.size() >= m_limits.maximumHeaderBytes) { Fail(EDapProtocolCodecError::HeaderTooLarge); break; }
			if ((!m_header.empty() && m_header.back() == '\r' && value != '\n') || (value == '\n' && (m_header.empty() || m_header.back() != '\r'))) {
				Fail(EDapProtocolCodecError::MalformedHeader); break;
			}
			m_header.push_back(value);
			if (m_header.size() >= 4 && m_header.compare(m_header.size() - 4, 4, "\r\n\r\n") == 0 && !FinishHeader()) break;
			continue;
		}
		m_body.push_back(value);
		if (m_body.size() == m_expectedBodyBytes) {
			if (completed >= m_limits.maximumMessagesPerFeed) { Fail(EDapProtocolCodecError::OutputLimitExceeded); break; }
			auto result = FinishBody(completedMessages);
			if (result.status == EDapProtocolCodecStatus::Failed) return { result.status, result.error, completed };
			++completed;
			if (completed >= m_limits.maximumMessagesPerFeed && index + 1 < bytes.size()) { Fail(EDapProtocolCodecError::OutputLimitExceeded); break; }
		}
	}
	if (m_state == EDapProtocolCodecState::Failed) return { EDapProtocolCodecStatus::Failed, m_error, completed };
	return { completed == 0 ? EDapProtocolCodecStatus::NeedsMore : EDapProtocolCodecStatus::Completed, EDapProtocolCodecError::None, completed };
}

DapProtocolCodecResult CDapProtocolCodec::Encode(const DapMessage& message, std::string& frame) const
{
	if (m_state == EDapProtocolCodecState::Stopped) return { EDapProtocolCodecStatus::Stopped, EDapProtocolCodecError::None, 0 };
	if (m_state == EDapProtocolCodecState::Failed) return { EDapProtocolCodecStatus::Failed, m_error, 0 };
	std::string body;
	if (!EncodeMessage(message, m_limits.maximumJsonDepth, body)) return { EDapProtocolCodecStatus::Failed, EDapProtocolCodecError::InvalidMessage, 0 };
	if (body.size() > m_limits.maximumBodyBytes) return { EDapProtocolCodecStatus::Failed, EDapProtocolCodecError::EncodedFrameTooLarge, 0 };
	frame = "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
	return { EDapProtocolCodecStatus::Completed, EDapProtocolCodecError::None, 1 };
}

void CDapProtocolCodec::Stop() noexcept
{
	ClearPartialInput();
	m_state = EDapProtocolCodecState::Stopped;
	m_error = EDapProtocolCodecError::None;
}

void CDapProtocolCodec::Reset() noexcept
{
	ClearPartialInput();
	m_state = EDapProtocolCodecState::ReadingHeader;
	m_error = EDapProtocolCodecError::None;
}

bool CDapProtocolCodec::FinishHeader()
{
	const auto headerEnd = m_header.size() - 4;
	const std::string_view header(m_header.data(), headerEnd);
	if (header.empty()) { Fail(EDapProtocolCodecError::MalformedHeader); return false; }
	const auto firstLineEnd = header.find("\r\n");
	const std::string_view line = firstLineEnd == std::string_view::npos ? header : header.substr(0, firstLineEnd);
	if (line.empty()) { Fail(EDapProtocolCodecError::MalformedHeader); return false; }
	constexpr std::string_view prefix = "Content-Length:";
	if (line.substr(0, prefix.size()) != prefix) { Fail(EDapProtocolCodecError::UnknownHeader); return false; }
	if (firstLineEnd != std::string_view::npos) {
		const auto secondStart = firstLineEnd + 2;
		const auto secondLineEnd = header.find("\r\n", secondStart);
		const auto secondLine = header.substr(secondStart, secondLineEnd == std::string_view::npos ? std::string_view::npos : secondLineEnd - secondStart);
		Fail(secondLine.substr(0, prefix.size()) == prefix ? EDapProtocolCodecError::DuplicateHeader : EDapProtocolCodecError::UnknownHeader);
		return false;
	}
	std::string_view value = line.substr(prefix.size());
	if (value.empty()) { Fail(EDapProtocolCodecError::InvalidContentLength); return false; }
	if (value.front() == ' ') value.remove_prefix(1);
	if (value.empty()) { Fail(EDapProtocolCodecError::InvalidContentLength); return false; }
	for (const auto digit : value) if (digit < '0' || digit > '9') { Fail(EDapProtocolCodecError::InvalidContentLength); return false; }
	std::uint64_t length = 0;
	const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), length);
	if (error == std::errc::result_out_of_range) { Fail(EDapProtocolCodecError::ContentLengthOverflow); return false; }
	if (error != std::errc{} || end != value.data() + value.size() || length > std::numeric_limits<std::size_t>::max()) { Fail(EDapProtocolCodecError::InvalidContentLength); return false; }
	if (length > m_limits.maximumBodyBytes) { Fail(EDapProtocolCodecError::BodyTooLarge); return false; }
	m_expectedBodyBytes = static_cast<std::size_t>(length);
	m_body.clear();
	m_body.reserve(m_expectedBodyBytes);
	m_header.clear();
	m_state = EDapProtocolCodecState::ReadingBody;
	if (m_expectedBodyBytes == 0) { Fail(EDapProtocolCodecError::InvalidJson); return false; }
	return true;
}

DapProtocolCodecResult CDapProtocolCodec::FinishBody(std::vector<DapMessage>& completedMessages)
{
	DapMessage message;
	if (!ValidateUtf8(m_body)) { Fail(EDapProtocolCodecError::InvalidUtf8); return { EDapProtocolCodecStatus::Failed, m_error, 0 }; }
	const auto decoded = DecodeMessage(m_body, m_limits.maximumJsonDepth, message);
	if (decoded != EDecodeResult::Succeeded) {
		Fail(decoded == EDecodeResult::InvalidJson ? EDapProtocolCodecError::InvalidJson : EDapProtocolCodecError::InvalidEnvelope);
		return { EDapProtocolCodecStatus::Failed, m_error, 0 };
	}
	completedMessages.emplace_back(std::move(message));
	ClearPartialInput();
	m_state = EDapProtocolCodecState::ReadingHeader;
	return { EDapProtocolCodecStatus::Completed, EDapProtocolCodecError::None, 1 };
}

void CDapProtocolCodec::Fail(EDapProtocolCodecError error) noexcept
{
	ClearPartialInput();
	m_state = EDapProtocolCodecState::Failed;
	m_error = error;
}

void CDapProtocolCodec::ClearPartialInput() noexcept
{
	m_header.clear();
	m_expectedBodyBytes = 0;
	m_body.clear();
}

} // namespace debug::dap

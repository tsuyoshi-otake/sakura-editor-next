/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#include "terminal/cli/SakuraHarnessCli.h"

#include "terminal/cli/SakuraTmuxCli.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <limits>

namespace terminal::cli {
namespace {

using namespace platform::harnessbridge;
using BridgeHarnessMessageId = platform::harnessbridge::HarnessMessageId;
using BridgeHarnessEndpointId = platform::harnessbridge::HarnessEndpointId;

constexpr std::string_view kProduct = "sakura-harness";
constexpr std::string_view kVersion = "0.1";

[[nodiscard]] std::string Diagnostic(const std::string_view code)
{
	std::string result(kProduct);
	result.append(": ");
	result.append(code.data(), code.size());
	result.push_back('\n');
	return result;
}

[[nodiscard]] bool IsSafeEnvironmentValue(const std::wstring_view value) noexcept
{
	if (value.empty() || value.size() > 4096) return false;
	for (const auto character : value) {
		if (character < 0x21 || character > 0x7e) return false;
	}
	return true;
}

[[nodiscard]] bool IsHexDigit(const char value) noexcept
{
	return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f')
		|| (value >= 'A' && value <= 'F');
}

[[nodiscard]] std::uint8_t HexDigit(const char value) noexcept
{
	if (value >= '0' && value <= '9') return static_cast<std::uint8_t>(value - '0');
	if (value >= 'a' && value <= 'f') return static_cast<std::uint8_t>(value - 'a' + 10);
	return static_cast<std::uint8_t>(value - 'A' + 10);
}

[[nodiscard]] std::string GrantNames(const EHarnessGrant grants)
{
	std::string result;
	for (const auto grant : { EHarnessGrant::Message, EHarnessGrant::ConsoleRead,
		EHarnessGrant::SendInput, EHarnessGrant::ManageTerminal }) {
		if (!HasGrant(grants, grant)) continue;
		if (!result.empty()) result.push_back(',');
		if (grant == EHarnessGrant::Message) result += "message";
		else if (grant == EHarnessGrant::ConsoleRead) result += "read-console-output";
		else if (grant == EHarnessGrant::SendInput) result += "send-input";
		else result += "manage-terminal";
	}
	return result;
}

[[nodiscard]] std::optional<EHarnessGrant> ParseCapabilities(const std::string_view value) noexcept
{
	if (value.empty()) return std::nullopt;
	EHarnessGrant result = EHarnessGrant::None;
	std::size_t begin = 0;
	while (begin <= value.size()) {
		const auto comma = value.find(',', begin);
		const auto end = comma == std::string_view::npos ? value.size() : comma;
		const auto token = value.substr(begin, end - begin);
		EHarnessGrant grant = EHarnessGrant::None;
		if (token == "message") grant = EHarnessGrant::Message;
		else if (token == "read-console-output") grant = EHarnessGrant::ConsoleRead;
		else if (token == "send-input") grant = EHarnessGrant::SendInput;
		else if (token == "manage-terminal") grant = EHarnessGrant::ManageTerminal;
		else return std::nullopt;
		if (HasGrant(result, grant)) return std::nullopt;
		result = result | grant;
		if (comma == std::string_view::npos) break;
		begin = comma + 1;
	}
	return result == EHarnessGrant::None ? std::nullopt : std::optional{ result };
}

[[nodiscard]] std::optional<std::chrono::milliseconds> ParseDuration(
	const std::string_view value) noexcept
{
	if (value.empty()) return std::nullopt;
	std::size_t suffixOffset = value.size();
	while (suffixOffset > 0 && value[suffixOffset - 1] >= 'a' && value[suffixOffset - 1] <= 'z') --suffixOffset;
	if (suffixOffset == 0 || suffixOffset == value.size()) return std::nullopt;
	const auto number = value.substr(0, suffixOffset);
	if (number.empty() || !std::all_of(number.begin(), number.end(), [](const char c) { return c >= '0' && c <= '9'; })) {
		return std::nullopt;
	}
	std::uint64_t parsed = 0;
	const auto conversion = std::from_chars(number.data(), number.data() + number.size(), parsed);
	if (conversion.ec != std::errc{} || conversion.ptr != number.data() + number.size()) return std::nullopt;
	std::uint64_t multiplier = 0;
	const auto suffix = value.substr(suffixOffset);
	if (suffix == "ms") multiplier = 1;
	else if (suffix == "s") multiplier = 1000;
	else if (suffix == "m") multiplier = 60 * 1000;
	else if (suffix == "h") multiplier = 60 * 60 * 1000;
	else return std::nullopt;
	if (parsed > (std::numeric_limits<std::uint64_t>::max)() / multiplier) return std::nullopt;
	const auto milliseconds = parsed * multiplier;
	if (milliseconds > static_cast<std::uint64_t>(kSakuraHarnessMaximumWait.count())) return std::nullopt;
	return std::chrono::milliseconds(static_cast<std::int64_t>(milliseconds));
}

[[nodiscard]] std::optional<EHarnessRunTerminalStatus> ParseRunStatus(
	const std::string_view value) noexcept
{
	if (value == "succeeded") return EHarnessRunTerminalStatus::Succeeded;
	if (value == "failed") return EHarnessRunTerminalStatus::Failed;
	if (value == "cancelled") return EHarnessRunTerminalStatus::Cancelled;
	if (value == "timed-out") return EHarnessRunTerminalStatus::TimedOut;
	if (value == "harness-exited") return EHarnessRunTerminalStatus::HarnessExited;
	return std::nullopt;
}

[[nodiscard]] bool NextValue(const std::vector<std::string>& arguments, std::size_t& index,
	std::string& result) noexcept
{
	if (++index >= arguments.size() || arguments[index].empty()) return false;
	result = arguments[index];
	return true;
}

[[nodiscard]] SakuraHarnessParseResult InvalidParse() noexcept
{
	return { SakuraHarnessParseOutcome::InvalidUsage, {} };
}

[[nodiscard]] SakuraHarnessParseResult UnsupportedParse() noexcept
{
	return { SakuraHarnessParseOutcome::Unsupported, {} };
}

[[nodiscard]] SakuraHarnessParseResult ResourceParse() noexcept
{
	return { SakuraHarnessParseOutcome::ResourceExhausted, {} };
}

[[nodiscard]] SakuraHarnessParseResult ParseNarrowArguments(
	const std::vector<std::string>& arguments) noexcept
{
	try {
		if (arguments.empty()) return InvalidParse();
		if (arguments.size() == 1 && (arguments.front() == "-V" || arguments.front() == "--version")) {
			return { SakuraHarnessParseOutcome::Parsed, { SakuraHarnessCommandKind::Version } };
		}
		if (arguments.front() == "-V" || arguments.front() == "--version") return UnsupportedParse();
		if (arguments.size() < 2) return InvalidParse();
		const auto& area = arguments[0];
		const auto& operation = arguments[1];
		SakuraHarnessCommand command;
		std::size_t index = 2;

		if (area == "endpoint" && operation == "register") {
			command.kind = SakuraHarnessCommandKind::EndpointRegister;
			bool haveName = false;
			bool haveCapabilities = false;
			for (; index < arguments.size(); ++index) {
				if (arguments[index] == "--name") {
					if (haveName || !NextValue(arguments, index, command.endpointName)) return InvalidParse();
					haveName = true;
				} else if (arguments[index] == "--capabilities") {
					if (haveCapabilities || !NextValue(arguments, index, command.capabilities)) return InvalidParse();
					const auto grants = ParseCapabilities(command.capabilities);
					if (!grants) return InvalidParse();
					command.grants = *grants;
					haveCapabilities = true;
				} else return InvalidParse();
			}
			return haveName && haveCapabilities ? SakuraHarnessParseResult{ SakuraHarnessParseOutcome::Parsed, command }
				: InvalidParse();
		}

		if (area == "endpoint" && operation == "renew") return UnsupportedParse();
		if (area == "endpoint" && operation == "list") {
			command.kind = SakuraHarnessCommandKind::EndpointList;
			if (index < arguments.size()) {
				if (arguments[index] == "--session") return UnsupportedParse();
				return InvalidParse();
			}
			return { SakuraHarnessParseOutcome::Parsed, command };
		}

		if (area == "message" && operation == "send") {
			command.kind = SakuraHarnessCommandKind::MessageSend;
			bool haveTo = false;
			bool haveFrom = false;
			bool haveType = false;
			for (; index < arguments.size(); ++index) {
				std::string value;
				if (arguments[index] == "--to") {
					if (haveTo || !NextValue(arguments, index, value)) return InvalidParse();
					command.endpoint = ParseSakuraHarnessId(value);
					if (!command.endpoint) return InvalidParse();
					haveTo = true;
				} else if (arguments[index] == "--from") {
					if (haveFrom || !NextValue(arguments, index, value)) return InvalidParse();
					command.localEndpoint = ParseSakuraHarnessId(value);
					if (!command.localEndpoint) return InvalidParse();
					haveFrom = true;
				} else if (arguments[index] == "--type") {
					if (haveType || !NextValue(arguments, index, command.messageType)) return InvalidParse();
					haveType = true;
				} else if (arguments[index] == "--run") {
					if (command.run || !NextValue(arguments, index, value)) return InvalidParse();
					command.run = ParseSakuraHarnessId(value);
					if (!command.run) return InvalidParse();
				} else if (arguments[index] == "--reply-to") {
					if (command.replyTo || !NextValue(arguments, index, value)) return InvalidParse();
					command.replyTo = ParseSakuraHarnessId(value);
					if (!command.replyTo) return InvalidParse();
				} else if (arguments[index] == "--payload-stdin") {
					if (command.payloadFromStdin) return InvalidParse();
					command.payloadFromStdin = true;
				} else return InvalidParse();
			}
			return haveTo && haveFrom && haveType && command.payloadFromStdin
				? SakuraHarnessParseResult{ SakuraHarnessParseOutcome::Parsed, command } : InvalidParse();
		}

		if (area == "message" && operation == "receive") {
			command.kind = SakuraHarnessCommandKind::MessageReceive;
			bool haveEndpoint = false;
			for (; index < arguments.size(); ++index) {
				if (arguments[index] == "--cursor") return UnsupportedParse();
				if (arguments[index] == "--endpoint") {
					std::string value;
					if (haveEndpoint || !NextValue(arguments, index, value)) return InvalidParse();
					command.localEndpoint = ParseSakuraHarnessId(value);
					if (!command.localEndpoint) return InvalidParse();
					haveEndpoint = true;
				} else if (arguments[index] == "--wait") {
					if (command.waitSpecified || index + 1 >= arguments.size()) return InvalidParse();
					const auto duration = ParseDuration(arguments[++index]);
					if (!duration) return InvalidParse();
					command.wait = *duration;
					command.waitSpecified = true;
				} else return InvalidParse();
			}
			return haveEndpoint ? SakuraHarnessParseResult{ SakuraHarnessParseOutcome::Parsed, command } : InvalidParse();
		}

		if (area == "message" && operation == "ack") {
			command.kind = SakuraHarnessCommandKind::MessageAck;
			bool haveMessage = false;
			bool haveEndpoint = false;
			for (; index < arguments.size(); ++index) {
				std::string value;
				if (arguments[index] == "--endpoint") {
					if (haveEndpoint || !NextValue(arguments, index, value)) return InvalidParse();
					command.localEndpoint = ParseSakuraHarnessId(value);
					if (!command.localEndpoint) return InvalidParse();
					haveEndpoint = true;
					continue;
				}
				if (arguments[index] != "--message" || haveMessage) return InvalidParse();
				if (!NextValue(arguments, index, value)) return InvalidParse();
				command.message = ParseSakuraHarnessId(value);
				if (!command.message) return InvalidParse();
				haveMessage = true;
			}
			return haveMessage && haveEndpoint ? SakuraHarnessParseResult{ SakuraHarnessParseOutcome::Parsed, command } : InvalidParse();
		}

		if (area == "run" && operation == "publish") {
			command.kind = SakuraHarnessCommandKind::RunPublish;
			bool haveRun = false;
			bool haveStatus = false;
			for (; index < arguments.size(); ++index) {
				std::string value;
				if (arguments[index] == "--run") {
					if (haveRun || !NextValue(arguments, index, value)) return InvalidParse();
					command.run = ParseSakuraHarnessId(value);
					if (!command.run) return InvalidParse();
					haveRun = true;
				} else if (arguments[index] == "--status") {
					if (haveStatus || !NextValue(arguments, index, value)) return InvalidParse();
					command.runStatus = ParseRunStatus(value);
					if (!command.runStatus) return InvalidParse();
					haveStatus = true;
				} else if (arguments[index] == "--payload-stdin") {
					if (command.payloadFromStdin) return InvalidParse();
					command.payloadFromStdin = true;
				} else return InvalidParse();
			}
			return haveRun && haveStatus
				? SakuraHarnessParseResult{ SakuraHarnessParseOutcome::Parsed, command } : InvalidParse();
		}

		if (area == "run" && operation == "wait") {
			command.kind = SakuraHarnessCommandKind::RunWait;
			bool haveRun = false;
			for (; index < arguments.size(); ++index) {
				if (arguments[index] != "--run" && arguments[index] != "--timeout") return InvalidParse();
				if (arguments[index] == "--run") {
					if (haveRun || index + 1 >= arguments.size()) return InvalidParse();
					command.run = ParseSakuraHarnessId(arguments[++index]);
					if (!command.run) return InvalidParse();
					haveRun = true;
				} else {
					if (command.timeoutSpecified || index + 1 >= arguments.size()) return InvalidParse();
					const auto duration = ParseDuration(arguments[++index]);
					if (!duration) return InvalidParse();
					command.wait = *duration;
					command.timeoutSpecified = true;
				}
			}
			return haveRun ? SakuraHarnessParseResult{ SakuraHarnessParseOutcome::Parsed, command } : InvalidParse();
		}

		if (area == "run" && operation == "cancel") return UnsupportedParse();
		if (area == "console" && operation == "capture") return UnsupportedParse();
		if (area == "input" && operation == "send") return UnsupportedParse();
		return InvalidParse();
	} catch (...) {
		return ResourceParse();
	}
}

class JsonWriter final {
public:
	JsonWriter() { m_text.push_back('{'); }

	void String(const std::string_view name, const std::string_view value)
	{
		FieldName(name);
		Quote(value);
	}

	void Number(const std::string_view name, const std::uint64_t value)
	{
		FieldName(name);
		m_text += std::to_string(value);
	}

	void SignedNumber(const std::string_view name, const std::int32_t value)
	{
		FieldName(name);
		m_text += std::to_string(value);
	}

	void Raw(const std::string_view name, const std::string_view value)
	{
		FieldName(name);
		m_text.append(value.data(), value.size());
	}

	[[nodiscard]] std::optional<std::string> Finish()
	{
		m_text.push_back('}');
		m_text.push_back('\n');
		if (m_text.size() > kSakuraHarnessMaximumJsonLineBytes) return std::nullopt;
		return std::move(m_text);
	}

private:
	void FieldName(const std::string_view name)
	{
		if (!m_first) m_text.push_back(',');
		m_first = false;
		Quote(name);
		m_text.push_back(':');
	}

	void Quote(const std::string_view value)
	{
		m_text.push_back('"');
		static constexpr char digits[] = "0123456789abcdef";
		for (const auto byte : value) {
			const auto c = static_cast<unsigned char>(byte);
			switch (c) {
			case '"': m_text += "\\\""; break;
			case '\\': m_text += "\\\\"; break;
			case '\b': m_text += "\\b"; break;
			case '\f': m_text += "\\f"; break;
			case '\n': m_text += "\\n"; break;
			case '\r': m_text += "\\r"; break;
			case '\t': m_text += "\\t"; break;
			default:
				if (c < 0x20) {
					m_text += "\\u00";
					m_text.push_back(digits[c >> 4]);
					m_text.push_back(digits[c & 0x0f]);
				} else m_text.push_back(static_cast<char>(c));
			}
		}
		m_text.push_back('"');
	}

	std::string m_text;
	bool m_first = true;
};

[[nodiscard]] std::string StatusName(const EHarnessTerminalStatus status)
{
	switch (status) {
	case EHarnessTerminalStatus::Succeeded: return "succeeded";
	case EHarnessTerminalStatus::InvalidRequest: return "invalid-request";
	case EHarnessTerminalStatus::UnsupportedVersion: return "unsupported-version";
	case EHarnessTerminalStatus::UnsupportedCapability: return "unsupported-capability";
	case EHarnessTerminalStatus::UnsupportedTmuxSurface: return "unsupported-surface";
	case EHarnessTerminalStatus::ProfileMismatch: return "profile-mismatch";
	case EHarnessTerminalStatus::EditorMismatch: return "editor-mismatch";
	case EHarnessTerminalStatus::GenerationMismatch: return "generation-mismatch";
	case EHarnessTerminalStatus::TargetMissing: return "target-missing";
	case EHarnessTerminalStatus::TopologyChanged: return "topology-changed";
	case EHarnessTerminalStatus::NotRunning: return "not-running";
	case EHarnessTerminalStatus::AccessDenied: return "access-denied";
	case EHarnessTerminalStatus::DeadlineExceeded: return "deadline-exceeded";
	case EHarnessTerminalStatus::Cancelled: return "cancelled";
	case EHarnessTerminalStatus::ServerStopping: return "server-stopping";
	case EHarnessTerminalStatus::ResourceExhausted: return "resource-exhausted";
	case EHarnessTerminalStatus::OperationUnknown: return "operation-unknown";
	case EHarnessTerminalStatus::Conflict: return "conflict";
	case EHarnessTerminalStatus::AlreadyTerminal: return "already-terminal";
	case EHarnessTerminalStatus::Ambiguous: return "ambiguous";
	case EHarnessTerminalStatus::ProtocolError: return "protocol-error";
	case EHarnessTerminalStatus::InternalError: return "internal-error";
	}
	return "internal-error";
}

[[nodiscard]] std::string RunStatusName(const EHarnessRunTerminalStatus status)
{
	switch (status) {
	case EHarnessRunTerminalStatus::Succeeded: return "succeeded";
	case EHarnessRunTerminalStatus::Failed: return "failed";
	case EHarnessRunTerminalStatus::Cancelled: return "cancelled";
	case EHarnessRunTerminalStatus::TimedOut: return "timed-out";
	case EHarnessRunTerminalStatus::HarnessExited: return "harness-exited";
	}
	return "failed";
}

[[nodiscard]] std::optional<std::string> StatusJson(const std::string_view kind,
	const std::string_view status)
{
	JsonWriter writer;
	writer.Number("version", 1);
	writer.String("kind", kind);
	writer.String("status", status);
	return writer.Finish();
}

[[nodiscard]] std::optional<std::string> StatusJson(const EHarnessTerminalStatus status)
{
	return StatusJson("result", StatusName(status));
}

[[nodiscard]] std::optional<std::string> VersionJson()
{
	JsonWriter writer;
	writer.Number("version", 1);
	writer.String("kind", "version");
	writer.String("status", "succeeded");
	writer.String("product", kProduct);
	writer.String("implementation", "sakura-editor-next");
	writer.String("protocol", "harness-bridge-1");
	writer.String("release", kVersion);
	return writer.Finish();
}

[[nodiscard]] std::optional<std::string> EndpointJson(const HarnessEndpointInfo& endpoint,
	const std::string_view status, const std::string_view kind = "endpoint")
{
	JsonWriter writer;
	writer.Number("version", 1);
	writer.String("kind", kind);
	writer.String("status", status);
	writer.String("endpoint_id", FormatSakuraHarnessId(endpoint.endpointId));
	writer.String("name", endpoint.displayName);
	writer.String("scope", endpoint.scope);
	writer.String("capabilities", GrantNames(endpoint.grants));
	return writer.Finish();
}

[[nodiscard]] std::optional<std::string> EndpointListJson(
	const std::vector<HarnessEndpointInfo>& endpoints)
{
	JsonWriter writer;
	writer.Number("version", 1);
	writer.String("kind", "endpoint-list");
	writer.String("status", "succeeded");
	writer.Number("count", endpoints.size());
	std::string ids;
	std::string metadata;
	ids.reserve(endpoints.size() * 40);
	metadata.reserve(endpoints.size() * 128);
	ids.push_back('[');
	metadata.push_back('[');
	for (std::size_t index = 0; index < endpoints.size(); ++index) {
		if (index != 0) {
			ids.push_back(',');
			metadata.push_back(',');
		}
		ids.push_back('"');
		ids += FormatSakuraHarnessId(endpoints[index].endpointId);
		ids.push_back('"');
		JsonWriter item;
		item.String("endpoint_id", FormatSakuraHarnessId(endpoints[index].endpointId));
		item.String("name", endpoints[index].displayName);
		item.String("scope", endpoints[index].scope);
		item.String("capabilities", GrantNames(endpoints[index].grants));
		const auto itemText = item.Finish();
		if (!itemText) return std::nullopt;
		metadata.append(itemText->data(), itemText->size() - 1);
	}
	ids.push_back(']');
	metadata.push_back(']');
	writer.Raw("endpoint_ids", ids);
	writer.Raw("endpoints", metadata);
	return writer.Finish();
}

[[nodiscard]] std::string HexBytes(const std::span<const std::uint8_t> bytes)
{
	static constexpr char digits[] = "0123456789abcdef";
	std::string result;
	result.reserve(bytes.size() * 2);
	for (const auto byte : bytes) {
		result.push_back(digits[byte >> 4]);
		result.push_back(digits[byte & 0x0f]);
	}
	return result;
}

[[nodiscard]] std::optional<std::string> DeliveriesJson(
	const std::vector<HarnessMessageDelivery>& deliveries)
{
	JsonWriter writer;
	writer.Number("version", 1);
	writer.String("kind", "message-receive");
	writer.String("status", "succeeded");
	writer.Number("count", deliveries.size());
	std::string encoded;
	encoded.reserve(deliveries.size() * 128);
	encoded.push_back('[');
	for (std::size_t index = 0; index < deliveries.size(); ++index) {
		if (index != 0) encoded.push_back(',');
		const auto& delivery = deliveries[index];
		JsonWriter item;
		item.String("message_id", FormatSakuraHarnessId(delivery.message.messageId));
		item.String("run_id", FormatSakuraHarnessId(delivery.message.runId));
		item.String("sender", FormatSakuraHarnessId(delivery.message.sender));
		item.String("recipient", FormatSakuraHarnessId(delivery.message.recipient));
		item.String("reply_to", FormatSakuraHarnessId(delivery.message.replyTo));
		item.Number("attempt", delivery.deliveryAttempt);
		item.String("type", delivery.message.type);
		item.String("payload_hex", HexBytes(delivery.message.payload));
		const auto itemText = item.Finish();
		if (!itemText) return std::nullopt;
		encoded.append(itemText->data(), itemText->size() - 1);
	}
	encoded.push_back(']');
	writer.Raw("messages", encoded);
	return writer.Finish();
}

[[nodiscard]] std::optional<std::string> RunJson(const HarnessRunResult& run,
	const std::string_view kind)
{
	JsonWriter writer;
	writer.Number("version", 1);
	writer.String("kind", kind);
	writer.String("status", "succeeded");
	writer.String("run_id", FormatSakuraHarnessId(run.runId));
	writer.String("run_status", RunStatusName(run.status));
	writer.SignedNumber("exit_code", run.exitCode);
	writer.Number("completed_at_tick", run.completedAtTick);
	return writer.Finish();
}

[[nodiscard]] std::optional<std::string> RegisterJson(
	const HarnessEndpointRegistration& registration)
{
	HarnessEndpointInfo endpoint{ registration.endpointId, registration.displayName,
		registration.scope, registration.grants };
	return EndpointJson(endpoint, "succeeded");
}

[[nodiscard]] SakuraCliProcessResult ParseFailure(const SakuraHarnessParseOutcome outcome) noexcept
{
	SakuraCliProcessResult result;
	EHarnessTerminalStatus status = EHarnessTerminalStatus::InvalidRequest;
	int exitCode = 2;
	std::string diagnostic = "invalid-usage";
	if (outcome == SakuraHarnessParseOutcome::Unsupported) {
		status = EHarnessTerminalStatus::UnsupportedCapability;
		exitCode = 10;
		diagnostic = "unsupported-operation";
	} else if (outcome == SakuraHarnessParseOutcome::ResourceExhausted) {
		status = EHarnessTerminalStatus::ResourceExhausted;
		exitCode = 7;
		diagnostic = "resource-exhausted";
	}
	result.exitCode = exitCode;
	result.stderrText = Diagnostic(diagnostic);
	const auto line = StatusJson("result", StatusName(status));
	if (line) result.stdoutText = *line;
	return result;
}

[[nodiscard]] SakuraCliProcessResult OperationFailure(const EHarnessTerminalStatus status) noexcept
{
	SakuraCliProcessResult result;
	result.exitCode = SakuraHarnessExitCode(status);
	result.stderrText = Diagnostic(StatusName(status));
	const auto line = StatusJson(status);
	if (line) result.stdoutText = *line;
	return result;
}

[[nodiscard]] std::chrono::milliseconds EffectiveTimeout(const std::chrono::milliseconds timeout,
	const std::optional<std::chrono::milliseconds> requested = std::nullopt) noexcept
{
	auto result = (std::min)(timeout, kSakuraHarnessMaximumOperationTimeout);
	if (requested) result = (std::min)(result, *requested);
	return (std::max)(result, std::chrono::milliseconds(1));
}

[[nodiscard]] std::optional<std::string> SuccessAckJson(
	const BridgeHarnessMessageId& message)
{
	JsonWriter writer;
	writer.Number("version", 1);
	writer.String("kind", "message-ack");
	writer.String("status", "succeeded");
	writer.String("message_id", FormatSakuraHarnessId(message));
	return writer.Finish();
}

} // namespace

SakuraHarnessParseResult ParseSakuraHarnessArguments(
	const std::span<const std::wstring_view> arguments) noexcept
{
	try {
		if ((arguments.size() != 0 && arguments.data() == nullptr)
			|| arguments.size() > kSakuraHarnessMaximumArgc) return ResourceParse();
		std::vector<std::string> narrow;
		narrow.reserve(arguments.size());
		for (const auto argument : arguments) {
			const auto converted = SakuraWideToUtf8(argument, kSakuraHarnessMaximumArgumentWideChars);
			if (!converted) return ResourceParse();
			narrow.push_back(*converted);
		}
		return ParseNarrowArguments(narrow);
	} catch (...) {
		return ResourceParse();
	}
}

std::optional<HarnessOpaqueId> ParseSakuraHarnessId(const std::string_view value) noexcept
{
	try {
		if (value.size() != 32 || !std::all_of(value.begin(), value.end(), IsHexDigit)) return std::nullopt;
		HarnessOpaqueId result;
		for (std::size_t index = 0; index < result.value.size(); ++index) {
			result.value[index] = static_cast<std::uint8_t>((HexDigit(value[index * 2]) << 4)
				| HexDigit(value[index * 2 + 1]));
		}
		return result.IsValid() ? std::optional{ result } : std::nullopt;
	} catch (...) {
		return std::nullopt;
	}
}

std::string FormatSakuraHarnessId(const HarnessOpaqueId& value)
{
	return HexBytes(value.value);
}

std::string SakuraHarnessStatusName(const EHarnessTerminalStatus status)
{
	return StatusName(status);
}

int SakuraHarnessExitCode(const EHarnessTerminalStatus status) noexcept
{
	switch (status) {
	case EHarnessTerminalStatus::Succeeded: return 0;
	case EHarnessTerminalStatus::InvalidRequest: return 2;
	case EHarnessTerminalStatus::TargetMissing: return 3;
	case EHarnessTerminalStatus::AccessDenied: return 4;
	case EHarnessTerminalStatus::UnsupportedCapability:
	case EHarnessTerminalStatus::ProfileMismatch:
	case EHarnessTerminalStatus::EditorMismatch:
	case EHarnessTerminalStatus::GenerationMismatch:
	case EHarnessTerminalStatus::ServerStopping:
	case EHarnessTerminalStatus::NotRunning: return 5;
	case EHarnessTerminalStatus::DeadlineExceeded: return 6;
	case EHarnessTerminalStatus::ResourceExhausted:
	case EHarnessTerminalStatus::TopologyChanged: return 7;
	case EHarnessTerminalStatus::Cancelled: return 8;
	case EHarnessTerminalStatus::Conflict:
	case EHarnessTerminalStatus::AlreadyTerminal:
	case EHarnessTerminalStatus::Ambiguous: return 9;
	case EHarnessTerminalStatus::UnsupportedVersion:
	case EHarnessTerminalStatus::UnsupportedTmuxSurface:
	case EHarnessTerminalStatus::OperationUnknown:
	case EHarnessTerminalStatus::ProtocolError:
	case EHarnessTerminalStatus::InternalError: return 10;
	}
	return 10;
}

SakuraCliProcessResult RunSakuraHarnessCli(
	const std::span<const std::wstring_view> arguments,
	const SakuraHarnessEnvironment& environment,
	ISakuraHarnessBridgeClient& bridge,
	ISakuraHarnessIdSource& ids,
	const std::span<const std::uint8_t> stdinPayload,
	const std::chrono::milliseconds timeout) noexcept
{
	try {
		const auto parsed = ParseSakuraHarnessArguments(arguments);
		if (parsed.outcome != SakuraHarnessParseOutcome::Parsed) return ParseFailure(parsed.outcome);
		if (parsed.command.kind == SakuraHarnessCommandKind::Version) {
			const auto line = VersionJson();
			return line ? SakuraCliProcessResult{ 0, *line, {} } : SakuraCliProcessResult{ 10, {}, Diagnostic("internal-error") };
		}
		if (!environment.IsComplete() || !IsSafeEnvironmentValue(environment.endpoint)
			|| !IsSafeEnvironmentValue(environment.target) || !IsSafeEnvironmentValue(environment.capability)) {
			return OperationFailure(EHarnessTerminalStatus::InvalidRequest);
		}
		if (environment.endpointId && !environment.endpointId->IsValid()) {
			return OperationFailure(EHarnessTerminalStatus::InvalidRequest);
		}
		if (timeout <= std::chrono::milliseconds::zero()) return OperationFailure(EHarnessTerminalStatus::InvalidRequest);

		const auto& command = parsed.command;
		if (command.payloadFromStdin && stdinPayload.size() > kSakuraHarnessMaximumInputBytes) {
			return OperationFailure(EHarnessTerminalStatus::ResourceExhausted);
		}
		if (command.kind == SakuraHarnessCommandKind::RunPublish && command.payloadFromStdin) {
			// The current broker run DTO has no payload field. Do not consume or
			// discard a requested run payload as if the operation had succeeded.
			return OperationFailure(EHarnessTerminalStatus::UnsupportedCapability);
		}

		std::vector<std::uint8_t> requestPayload;
		EHarnessOperationKind operation = EHarnessOperationKind::QueryOperation;
		std::optional<std::string> successJson;
		std::optional<BridgeHarnessMessageId> acknowledgedMessage;
		HarnessEndpointRegistration registration;
		std::optional<BridgeHarnessEndpointId> localEndpoint = command.localEndpoint
			? command.localEndpoint : environment.endpointId;

		switch (command.kind) {
		case SakuraHarnessCommandKind::EndpointRegister: {
			const auto generated = ids.NextId();
			if (!generated || !generated->IsValid()) return OperationFailure(EHarnessTerminalStatus::InternalError);
			registration.endpointId = *generated;
			registration.displayName = command.endpointName;
			registration.scope = "terminal";
			registration.grants = command.grants;
			registration.maximumQueue = 128;
			const auto encoded = EncodeHarnessBridgeEndpointRegistration(registration);
			if (!encoded) return OperationFailure(EHarnessTerminalStatus::InvalidRequest);
			requestPayload = *encoded;
			operation = EHarnessOperationKind::RegisterEndpoint;
			break;
		}
		case SakuraHarnessCommandKind::EndpointList:
			operation = EHarnessOperationKind::ListEndpoints;
			break;
		case SakuraHarnessCommandKind::MessageSend: {
			if (!localEndpoint) return OperationFailure(EHarnessTerminalStatus::InvalidRequest);
			const auto generated = ids.NextId();
			if (!generated || !generated->IsValid()) return OperationFailure(EHarnessTerminalStatus::InternalError);
			HarnessMessage message;
			message.messageId = *generated;
			message.sender = *localEndpoint;
			message.recipient = *command.endpoint;
			if (command.run) message.runId = *command.run;
			if (command.replyTo) message.replyTo = *command.replyTo;
			message.type = command.messageType;
			message.payload.assign(stdinPayload.begin(), stdinPayload.end());
			const auto encoded = EncodeHarnessBridgeMessage(message);
			if (!encoded) return OperationFailure(EHarnessTerminalStatus::InvalidRequest);
			requestPayload = *encoded;
			operation = EHarnessOperationKind::SendEndpointMessage;
			acknowledgedMessage = message.messageId;
			break;
		}
		case SakuraHarnessCommandKind::MessageReceive: {
			if (!localEndpoint) return OperationFailure(EHarnessTerminalStatus::InvalidRequest);
			const auto encoded = EncodeHarnessBridgeReceiveRequest(*localEndpoint, command.maximumMessages);
			if (!encoded) return OperationFailure(EHarnessTerminalStatus::InvalidRequest);
			requestPayload = *encoded;
			operation = EHarnessOperationKind::ReceiveMessages;
			break;
		}
		case SakuraHarnessCommandKind::MessageAck: {
			if (!localEndpoint) return OperationFailure(EHarnessTerminalStatus::InvalidRequest);
			const auto encoded = EncodeHarnessBridgeAcknowledgeRequest(*localEndpoint, *command.message);
			if (!encoded) return OperationFailure(EHarnessTerminalStatus::InvalidRequest);
			requestPayload = *encoded;
			operation = EHarnessOperationKind::AcknowledgeMessage;
			acknowledgedMessage = command.message;
			break;
		}
		case SakuraHarnessCommandKind::RunPublish: {
			HarnessRunResult run;
			run.runId = *command.run;
			run.status = *command.runStatus;
			const auto encoded = EncodeHarnessBridgeRunPublish(false, run);
			if (!encoded) return OperationFailure(EHarnessTerminalStatus::InvalidRequest);
			requestPayload = *encoded;
			operation = EHarnessOperationKind::PublishRun;
			break;
		}
		case SakuraHarnessCommandKind::RunWait: {
			const auto encoded = EncodeHarnessBridgeRunRequest(*command.run);
			if (!encoded) return OperationFailure(EHarnessTerminalStatus::InvalidRequest);
			requestPayload = *encoded;
			operation = EHarnessOperationKind::WaitRun;
			break;
		}
		case SakuraHarnessCommandKind::Version: break;
		}

		struct CloseGuard final {
			ISakuraHarnessBridgeClient& client;
			~CloseGuard() { client.Close(); }
		} closeGuard{ bridge };
		const auto connected = bridge.Connect(environment);
		if (!connected.succeeded) return OperationFailure(connected.status);
		std::optional<std::chrono::milliseconds> requestedWait;
		if (command.kind == SakuraHarnessCommandKind::MessageReceive && command.waitSpecified) requestedWait = command.wait;
		if (command.kind == SakuraHarnessCommandKind::RunWait && command.timeoutSpecified) requestedWait = command.wait;
		const auto operationResult = bridge.Execute(operation, requestPayload, EffectiveTimeout(timeout, requestedWait));
		if (operationResult.status != EHarnessTerminalStatus::Succeeded) return OperationFailure(operationResult.status);

		switch (command.kind) {
		case SakuraHarnessCommandKind::EndpointRegister:
			successJson = RegisterJson(registration);
			break;
		case SakuraHarnessCommandKind::EndpointList: {
			std::vector<HarnessEndpointInfo> endpoints;
			if (!DecodeHarnessBridgeEndpointList(operationResult.payload, endpoints)) return OperationFailure(EHarnessTerminalStatus::ProtocolError);
			successJson = EndpointListJson(endpoints);
			break;
		}
		case SakuraHarnessCommandKind::MessageSend: {
			if (!acknowledgedMessage) return OperationFailure(EHarnessTerminalStatus::InternalError);
			JsonWriter writer;
			writer.Number("version", 1);
			writer.String("kind", "message-send");
			writer.String("status", "succeeded");
			writer.String("message_id", FormatSakuraHarnessId(*acknowledgedMessage));
			successJson = writer.Finish();
			break;
		}
		case SakuraHarnessCommandKind::MessageReceive: {
			std::vector<HarnessMessageDelivery> deliveries;
			if (!DecodeHarnessBridgeDeliveries(operationResult.payload, deliveries)) return OperationFailure(EHarnessTerminalStatus::ProtocolError);
			successJson = DeliveriesJson(deliveries);
			break;
		}
		case SakuraHarnessCommandKind::MessageAck:
			successJson = SuccessAckJson(*acknowledgedMessage);
			break;
		case SakuraHarnessCommandKind::RunPublish: {
			HarnessRunResult result;
			result.runId = *command.run;
			result.status = *command.runStatus;
			successJson = RunJson(result, "run-publish");
			break;
		}
		case SakuraHarnessCommandKind::RunWait: {
			std::optional<HarnessRunResult> run;
			if (!DecodeHarnessBridgeRun(operationResult.payload, run) || !run) return OperationFailure(EHarnessTerminalStatus::ProtocolError);
			successJson = RunJson(*run, "run-wait");
			break;
		}
		case SakuraHarnessCommandKind::Version: break;
		}
		if (!successJson) return OperationFailure(EHarnessTerminalStatus::ProtocolError);
		return SakuraCliProcessResult{ 0, *successJson, {} };
	} catch (...) {
		return SakuraCliProcessResult{ 10, {}, Diagnostic("internal-error") };
	}
}

SakuraCliProcessResult RunSakuraHarnessCli(
	const int argc, wchar_t* const* argv, const SakuraHarnessEnvironment& environment,
	ISakuraHarnessBridgeClient& bridge, ISakuraHarnessIdSource& ids,
	const std::span<const std::uint8_t> stdinPayload, const std::chrono::milliseconds timeout) noexcept
{
	if (argc < 1 || argv == nullptr) return SakuraCliProcessResult{ 2, {}, Diagnostic("invalid-usage") };
	try {
		if (argc - 1 > static_cast<int>(kSakuraHarnessMaximumArgc)) {
			return SakuraCliProcessResult{ 7, {}, Diagnostic("resource-exhausted") };
		}
		std::vector<std::wstring_view> arguments;
		arguments.reserve(static_cast<std::size_t>(argc - 1));
		for (int index = 1; index < argc; ++index) {
			if (argv[index] == nullptr) return SakuraCliProcessResult{ 2, {}, Diagnostic("invalid-argument") };
			std::size_t length = 0;
			while (length <= kSakuraHarnessMaximumArgumentWideChars && argv[index][length] != L'\0') ++length;
			if (length > kSakuraHarnessMaximumArgumentWideChars) return SakuraCliProcessResult{ 7, {}, Diagnostic("resource-exhausted") };
			arguments.emplace_back(argv[index], length);
		}
		return RunSakuraHarnessCli(arguments, environment, bridge, ids, stdinPayload, timeout);
	} catch (...) {
		return SakuraCliProcessResult{ 10, {}, Diagnostic("internal-error") };
	}
}

} // namespace terminal::cli

/*!	@file
	@brief 拡張ホスト用 JSON-RPC プロトコル

*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "extension/CExtensionRpcProtocol.h"

#include <picojson/picojson.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace {

const picojson::value* Find(const picojson::object& object, const char* key)
{
	const auto it = object.find(key);
	return it == object.end() ? nullptr : &it->second;
}

bool ParseJson(std::string_view json, picojson::value& value, std::string& errorMessage)
{
	if (json.empty()) {
		errorMessage = "JSON value is empty";
		return false;
	}
	const std::string parseError = picojson::parse(value, std::string(json));
	if (!parseError.empty()) {
		errorMessage = "invalid JSON: " + parseError;
		return false;
	}
	return true;
}

bool ParseOptionalParams(std::string_view paramsJson, picojson::value& params, bool& hasParams, std::string& errorMessage)
{
	hasParams = !paramsJson.empty();
	if (!hasParams) {
		return true;
	}
	if (!ParseJson(paramsJson, params, errorMessage)) {
		return false;
	}
	if (!params.is<picojson::object>() && !params.is<picojson::array>()) {
		errorMessage = "JSON-RPC params must be an object or array";
		return false;
	}
	return true;
}

bool ParseId(std::string_view idJson, picojson::value& id, std::string& canonicalId, std::string& errorMessage)
{
	if (!ParseJson(idJson, id, errorMessage)) {
		return false;
	}
	if (!id.is<std::string>() && !id.is<double>()) {
		errorMessage = "JSON-RPC id must be a string or number";
		return false;
	}
	if (id.is<double>()) {
		const double number = id.get<double>();
		if (!std::isfinite(number)) {
			errorMessage = "JSON-RPC id must be finite";
			return false;
		}
	}
	canonicalId = id.serialize();
	return true;
}

bool ValidateMethod(std::string_view method, std::string& errorMessage)
{
	if (method.empty()) {
		errorMessage = "JSON-RPC method must not be empty";
		return false;
	}
	return true;
}

bool ParseMessage(std::string_view payload, SExtensionRpcMessage& message, std::string& errorMessage)
{
	picojson::value root;
	if (!ParseJson(payload, root, errorMessage)) {
		return false;
	}
	if (!root.is<picojson::object>()) {
		errorMessage = "JSON-RPC payload root must be an object";
		return false;
	}

	const auto& object = root.get<picojson::object>();
	const picojson::value* version = Find(object, "jsonrpc");
	if (!version || !version->is<std::string>() || version->get<std::string>() != "2.0") {
		errorMessage = "JSON-RPC payload must declare jsonrpc 2.0";
		return false;
	}

	const picojson::value* method = Find(object, "method");
	const picojson::value* id = Find(object, "id");
	const picojson::value* params = Find(object, "params");
	const picojson::value* result = Find(object, "result");
	const picojson::value* error = Find(object, "error");

	if (method) {
		if (!method->is<std::string>() || method->get<std::string>().empty()) {
			errorMessage = "JSON-RPC method must be a non-empty string";
			return false;
		}
		if (result || error) {
			errorMessage = "JSON-RPC request must not contain result or error";
			return false;
		}
		if (params && !params->is<picojson::object>() && !params->is<picojson::array>()) {
			errorMessage = "JSON-RPC params must be an object or array";
			return false;
		}

		message.eKind = id ? EExtensionRpcMessageKind::Request : EExtensionRpcMessageKind::Notification;
		message.sMethod = method->get<std::string>();
		message.sParamsJson = params ? params->serialize() : std::string();
		if (id) {
			if (!id->is<std::string>() && !id->is<double>()) {
				errorMessage = "JSON-RPC request id must be a string or number";
				return false;
			}
			if (id->is<double>() && !std::isfinite(id->get<double>())) {
				errorMessage = "JSON-RPC request id must be finite";
				return false;
			}
			message.sIdJson = id->serialize();
		}
		return true;
	}

	if (!id) {
		errorMessage = "JSON-RPC response must contain id";
		return false;
	}
	if (!id->is<std::string>() && !id->is<double>()) {
		errorMessage = "JSON-RPC response id must be a string or number";
		return false;
	}
	if (id->is<double>() && !std::isfinite(id->get<double>())) {
		errorMessage = "JSON-RPC response id must be finite";
		return false;
	}
	if (params) {
		errorMessage = "JSON-RPC response must not contain params";
		return false;
	}
	if ((result == nullptr) == (error == nullptr)) {
		errorMessage = "JSON-RPC response must contain exactly one of result or error";
		return false;
	}

	message.sIdJson = id->serialize();
	if (result) {
		message.eKind = EExtensionRpcMessageKind::SuccessResponse;
		message.sResultJson = result->serialize();
		return true;
	}

	if (!error->is<picojson::object>()) {
		errorMessage = "JSON-RPC error must be an object";
		return false;
	}
	const auto& errorObject = error->get<picojson::object>();
	const picojson::value* code = Find(errorObject, "code");
	const picojson::value* errorText = Find(errorObject, "message");
	const picojson::value* data = Find(errorObject, "data");
	if (!code || !code->is<double>() || !errorText || !errorText->is<std::string>()) {
		errorMessage = "JSON-RPC error must contain numeric code and string message";
		return false;
	}
	const double numericCode = code->get<double>();
	if (!std::isfinite(numericCode) || std::floor(numericCode) != numericCode ||
		numericCode < (std::numeric_limits<int>::min)() || numericCode > (std::numeric_limits<int>::max)()) {
		errorMessage = "JSON-RPC error code must be a finite integer";
		return false;
	}

	message.eKind = EExtensionRpcMessageKind::ErrorResponse;
	message.error.nCode = static_cast<int>(numericCode);
	message.error.sMessage = errorText->get<std::string>();
	message.error.sDataJson = data ? data->serialize() : std::string();
	return true;
}

} // namespace

CExtensionRpcProtocol::CExtensionRpcProtocol(std::size_t maxPayloadBytes) noexcept
	: m_frameCodec(maxPayloadBytes)
{
}

bool CExtensionRpcProtocol::EnsureOpen(std::string& errorMessage) const
{
	if (m_state == EExtensionRpcProtocolState::Open) {
		return true;
	}
	errorMessage = m_terminalDiagnostic.empty() ? "JSON-RPC connection is not open" : m_terminalDiagnostic;
	return false;
}

bool CExtensionRpcProtocol::EncodeEnvelope(
	const std::string& json,
	SExtensionRpcOutbound& outbound,
	std::string& errorMessage) const
{
	if (!m_frameCodec.Encode(json, outbound.frame)) {
		errorMessage = "JSON-RPC payload exceeds the configured frame limit";
		return false;
	}
	return true;
}

bool CExtensionRpcProtocol::CreateRequest(
	std::string_view method,
	std::string_view paramsJson,
	SExtensionRpcOutbound& outbound,
	std::string& errorMessage)
{
	outbound = {};
	errorMessage.clear();
	if (!EnsureOpen(errorMessage) || !ValidateMethod(method, errorMessage)) {
		return false;
	}
	if (m_nextRequestId == 0) {
		errorMessage = "JSON-RPC request id space is exhausted";
		return false;
	}

	picojson::value params;
	bool hasParams = false;
	if (!ParseOptionalParams(paramsJson, params, hasParams, errorMessage)) {
		return false;
	}

	const std::string id = "sakura-" + std::to_string(m_nextRequestId);
	picojson::object object;
	object["jsonrpc"] = picojson::value("2.0");
	object["id"] = picojson::value(id);
	object["method"] = picojson::value(std::string(method));
	if (hasParams) {
		object["params"] = std::move(params);
	}

	outbound.sIdJson = picojson::value(id).serialize();
	if (!EncodeEnvelope(picojson::value(std::move(object)).serialize(), outbound, errorMessage)) {
		outbound = {};
		return false;
	}
	m_pendingRequestIds.insert(outbound.sIdJson);
	++m_nextRequestId;
	return true;
}

bool CExtensionRpcProtocol::CreateNotification(
	std::string_view method,
	std::string_view paramsJson,
	SExtensionRpcOutbound& outbound,
	std::string& errorMessage)
{
	outbound = {};
	errorMessage.clear();
	if (!EnsureOpen(errorMessage) || !ValidateMethod(method, errorMessage)) {
		return false;
	}

	picojson::value params;
	bool hasParams = false;
	if (!ParseOptionalParams(paramsJson, params, hasParams, errorMessage)) {
		return false;
	}

	picojson::object object;
	object["jsonrpc"] = picojson::value("2.0");
	object["method"] = picojson::value(std::string(method));
	if (hasParams) {
		object["params"] = std::move(params);
	}
	return EncodeEnvelope(picojson::value(std::move(object)).serialize(), outbound, errorMessage);
}

bool CExtensionRpcProtocol::CreateSuccessResponse(
	std::string_view idJson,
	std::string_view resultJson,
	SExtensionRpcOutbound& outbound,
	std::string& errorMessage)
{
	outbound = {};
	errorMessage.clear();
	if (!EnsureOpen(errorMessage)) {
		return false;
	}

	picojson::value id;
	std::string canonicalId;
	if (!ParseId(idJson, id, canonicalId, errorMessage)) {
		return false;
	}
	picojson::value result;
	if (resultJson.empty()) {
		result = picojson::value();
	}
	else if (!ParseJson(resultJson, result, errorMessage)) {
		return false;
	}

	picojson::object object;
	object["jsonrpc"] = picojson::value("2.0");
	object["id"] = std::move(id);
	object["result"] = std::move(result);
	return EncodeEnvelope(picojson::value(std::move(object)).serialize(), outbound, errorMessage);
}

bool CExtensionRpcProtocol::CreateErrorResponse(
	std::string_view idJson,
	int code,
	std::string_view message,
	std::string_view dataJson,
	SExtensionRpcOutbound& outbound,
	std::string& errorMessage)
{
	outbound = {};
	errorMessage.clear();
	if (!EnsureOpen(errorMessage)) {
		return false;
	}

	picojson::value id;
	std::string canonicalId;
	if (!ParseId(idJson, id, canonicalId, errorMessage)) {
		return false;
	}
	picojson::value data;
	const bool hasData = !dataJson.empty();
	if (hasData && !ParseJson(dataJson, data, errorMessage)) {
		return false;
	}

	picojson::object errorObject;
	errorObject["code"] = picojson::value(static_cast<double>(code));
	errorObject["message"] = picojson::value(std::string(message));
	if (hasData) {
		errorObject["data"] = std::move(data);
	}
	picojson::object object;
	object["jsonrpc"] = picojson::value("2.0");
	object["id"] = std::move(id);
	object["error"] = picojson::value(std::move(errorObject));
	return EncodeEnvelope(picojson::value(std::move(object)).serialize(), outbound, errorMessage);
}

bool CExtensionRpcProtocol::CreateCancelNotification(
	std::string_view idJson,
	SExtensionRpcOutbound& outbound,
	std::string& errorMessage)
{
	outbound = {};
	errorMessage.clear();
	if (!EnsureOpen(errorMessage)) {
		return false;
	}

	picojson::value id;
	std::string canonicalId;
	if (!ParseId(idJson, id, canonicalId, errorMessage)) {
		return false;
	}
	if (!m_pendingRequestIds.contains(canonicalId)) {
		errorMessage = "cannot cancel an unknown or completed JSON-RPC request";
		return false;
	}

	picojson::object params;
	params["id"] = std::move(id);
	picojson::object object;
	object["jsonrpc"] = picojson::value("2.0");
	object["method"] = picojson::value("$/cancelRequest");
	object["params"] = picojson::value(std::move(params));
	return EncodeEnvelope(picojson::value(std::move(object)).serialize(), outbound, errorMessage);
}

SExtensionRpcReceiveResult CExtensionRpcProtocol::Feed(std::string_view bytes)
{
	SExtensionRpcReceiveResult receiveResult;
	if (m_state != EExtensionRpcProtocolState::Open) {
		receiveResult.terminalReason = m_terminalReason;
		receiveResult.diagnostic = m_terminalDiagnostic;
		return receiveResult;
	}

	std::vector<std::string> payloads;
	if (!m_frameCodec.Feed(bytes, payloads)) {
		return Terminate(EExtensionRpcTerminalReason::ProtocolError, "JSON-RPC frame exceeds the configured payload limit");
	}

	for (const auto& payload : payloads) {
		SExtensionRpcMessage message;
		std::string errorMessage;
		if (!ParseMessage(payload, message, errorMessage)) {
			auto terminal = Terminate(EExtensionRpcTerminalReason::ProtocolError, std::move(errorMessage));
			receiveResult.failedRequests = std::move(terminal.failedRequests);
			receiveResult.terminalReason = terminal.terminalReason;
			receiveResult.diagnostic = std::move(terminal.diagnostic);
			return receiveResult;
		}

		if (message.eKind == EExtensionRpcMessageKind::SuccessResponse ||
			message.eKind == EExtensionRpcMessageKind::ErrorResponse) {
			const auto it = m_pendingRequestIds.find(message.sIdJson);
			if (it == m_pendingRequestIds.end()) {
				auto terminal = Terminate(
					EExtensionRpcTerminalReason::ProtocolError,
					"JSON-RPC response refers to an unknown or completed request id");
				receiveResult.failedRequests = std::move(terminal.failedRequests);
				receiveResult.terminalReason = terminal.terminalReason;
				receiveResult.diagnostic = std::move(terminal.diagnostic);
				return receiveResult;
			}
			m_pendingRequestIds.erase(it);
		}
		receiveResult.messages.push_back(std::move(message));
	}
	return receiveResult;
}

SExtensionRpcReceiveResult CExtensionRpcProtocol::CloseHostLost(std::string_view diagnostic)
{
	if (m_state != EExtensionRpcProtocolState::Open) {
		SExtensionRpcReceiveResult result;
		result.terminalReason = m_terminalReason;
		result.diagnostic = m_terminalDiagnostic;
		return result;
	}
	return Terminate(EExtensionRpcTerminalReason::HostLost, std::string(diagnostic));
}

SExtensionRpcReceiveResult CExtensionRpcProtocol::Terminate(
	EExtensionRpcTerminalReason reason,
	std::string diagnostic)
{
	SExtensionRpcReceiveResult result;
	if (m_state != EExtensionRpcProtocolState::Open) {
		result.terminalReason = m_terminalReason;
		result.diagnostic = m_terminalDiagnostic;
		return result;
	}

	m_state = reason == EExtensionRpcTerminalReason::HostLost
		? EExtensionRpcProtocolState::Closed
		: EExtensionRpcProtocolState::Failed;
	m_terminalReason = reason;
	m_terminalDiagnostic = std::move(diagnostic);
	result.terminalReason = reason;
	result.diagnostic = m_terminalDiagnostic;
	result.failedRequests.reserve(m_pendingRequestIds.size());
	for (const auto& id : m_pendingRequestIds) {
		result.failedRequests.push_back({ id, reason });
	}
	std::sort(result.failedRequests.begin(), result.failedRequests.end(),
		[](const SExtensionRpcPendingFailure& lhs, const SExtensionRpcPendingFailure& rhs) {
			return lhs.sIdJson < rhs.sIdJson;
		});
	m_pendingRequestIds.clear();
	return result;
}

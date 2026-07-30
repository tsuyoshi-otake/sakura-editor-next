/*!	@file
	@brief 拡張ホスト用 JSON-RPC プロトコル

*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#ifndef SAKURA_CEXTENSIONRPCPROTOCOL_61738D1B_DCA4_4CB4_91D5_A5829126C601_H_
#define SAKURA_CEXTENSIONRPCPROTOCOL_61738D1B_DCA4_4CB4_91D5_A5829126C601_H_
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "extension/CJsonRpcFrameCodec.h"

//! JSON-RPC 接続の状態
enum class EExtensionRpcProtocolState {
	Open,
	Closed,	//!< transport または host を失った
	Failed,	//!< protocol error。新しい接続を作るまで復帰しない
};

//! 接続と pending request を終了させた理由
enum class EExtensionRpcTerminalReason {
	None,
	HostLost,
	ProtocolError,
};

//! 復元した JSON-RPC message の種類
enum class EExtensionRpcMessageKind {
	Request,
	Notification,
	SuccessResponse,
	ErrorResponse,
};

//! JSON-RPC error object
struct SExtensionRpcError {
	int			nCode = 0;
	std::string	sMessage;
	std::string	sDataJson;	//!< data が無ければ空。あれば canonical JSON
};

//! framing と envelope 検証を通過した 1 message
struct SExtensionRpcMessage {
	EExtensionRpcMessageKind	eKind = EExtensionRpcMessageKind::Notification;
	std::string				sIdJson;		//!< Request/Response の canonical JSON id。Notification は空
	std::string				sMethod;
	std::string				sParamsJson;	//!< params が無ければ空
	std::string				sResultJson;	//!< SuccessResponse の canonical JSON result
	SExtensionRpcError			error;
};

//! 接続終了により応答を受け取れなくなった request
struct SExtensionRpcPendingFailure {
	std::string				sIdJson;
	EExtensionRpcTerminalReason	eReason = EExtensionRpcTerminalReason::None;
};

//! 1 回の Feed または接続終了で確定した結果
struct SExtensionRpcReceiveResult {
	std::vector<SExtensionRpcMessage>		messages;
	std::vector<SExtensionRpcPendingFailure>	failedRequests;
	EExtensionRpcTerminalReason			terminalReason = EExtensionRpcTerminalReason::None;
	std::string						diagnostic;

	bool IsTerminal() const noexcept { return terminalReason != EExtensionRpcTerminalReason::None; }
};

//! transport へ渡す 1 frame
struct SExtensionRpcOutbound {
	std::string	sIdJson;	//!< Request の id。Notification/Response は空
	std::string	frame;
};

/*!
	@brief 1 本の拡張ホスト接続に対応する双方向 JSON-RPC protocol

	Win32 handle、thread、UI には依存しない。transport は Feed に受信 byte を渡し、
	返された message を適切な thread の queue に積む。公開メソッドの thread safety は
	提供しないため、1 個の I/O owner から直列に呼ぶこと。
*/
class CExtensionRpcProtocol {
public:
	explicit CExtensionRpcProtocol(
		std::size_t maxPayloadBytes = CJsonRpcFrameCodec::kDefaultMaxPayloadBytes) noexcept;

	bool CreateRequest(
		std::string_view method,
		std::string_view paramsJson,
		SExtensionRpcOutbound& outbound,
		std::string& errorMessage);

	bool CreateNotification(
		std::string_view method,
		std::string_view paramsJson,
		SExtensionRpcOutbound& outbound,
		std::string& errorMessage);

	bool CreateSuccessResponse(
		std::string_view idJson,
		std::string_view resultJson,
		SExtensionRpcOutbound& outbound,
		std::string& errorMessage);

	bool CreateErrorResponse(
		std::string_view idJson,
		int code,
		std::string_view message,
		std::string_view dataJson,
		SExtensionRpcOutbound& outbound,
		std::string& errorMessage);

	/*!
		@brief pending request の $/cancelRequest notification を作る

		キャンセル通知だけでは request を完了扱いにしない。相手の Response、HostLost、
		または上位層の timeout が terminal outcome を所有する。
	*/
	bool CreateCancelNotification(
		std::string_view idJson,
		SExtensionRpcOutbound& outbound,
		std::string& errorMessage);

	//! 任意に分割された transport byte を処理する
	SExtensionRpcReceiveResult Feed(std::string_view bytes);

	//! pipe 切断、host crash、broker 喪失を通知し、全 pending request を一度だけ失敗させる
	SExtensionRpcReceiveResult CloseHostLost(std::string_view diagnostic = "host connection lost");

	EExtensionRpcProtocolState GetState() const noexcept { return m_state; }
	EExtensionRpcTerminalReason GetTerminalReason() const noexcept { return m_terminalReason; }
	std::size_t GetPendingRequestCount() const noexcept { return m_pendingRequestIds.size(); }

private:
	bool EnsureOpen(std::string& errorMessage) const;
	bool EncodeEnvelope(const std::string& json, SExtensionRpcOutbound& outbound, std::string& errorMessage) const;
	SExtensionRpcReceiveResult Terminate(EExtensionRpcTerminalReason reason, std::string diagnostic);

	CJsonRpcFrameCodec			m_frameCodec;
	EExtensionRpcProtocolState	m_state = EExtensionRpcProtocolState::Open;
	EExtensionRpcTerminalReason	m_terminalReason = EExtensionRpcTerminalReason::None;
	std::string				m_terminalDiagnostic;
	std::uint64_t				m_nextRequestId = 1;
	std::unordered_set<std::string>	m_pendingRequestIds;
};

#endif /* SAKURA_CEXTENSIONRPCPROTOCOL_61738D1B_DCA4_4CB4_91D5_A5829126C601_H_ */

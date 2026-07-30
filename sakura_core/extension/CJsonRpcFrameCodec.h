/*!\t@file
	@brief JSON-RPC 用の長さプレフィックス・フレーム

*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#ifndef SAKURA_CJSONRPCFRAMECODEC_3C0C1A85_6B2C_4E4B_A5E0_963ED72A1B52_H_
#define SAKURA_CJSONRPCFRAMECODEC_3C0C1A85_6B2C_4E4B_A5E0_963ED72A1B52_H_
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

//! JSON-RPC フレームの復号状態
enum class EJsonRpcFrameCodecState {
	ReadingHeader,	//!< 4 バイトの長さを待っている
	ReadingPayload,	//!< 宣言された長さのペイロードを待っている
	Failed,			//!< Reset まで復帰しないエラー状態
};

//! JSON-RPC フレームの復号エラー
enum class EJsonRpcFrameCodecError {
	None,
	PayloadTooLarge,
};

/*!
	@brief JSON-RPC のバイト・フレームを符号化・復号する

	フレームは 4 バイトの符号なしビッグエンディアン長に、そのままの UTF-8 JSON
	バイト列を続けたもの。この層では JSON の妥当性を検査しない。
*/
class CJsonRpcFrameCodec {
public:
	//! 既定のペイロード上限 (16 MiB)
	static constexpr std::size_t kDefaultMaxPayloadBytes = 16u * 1024u * 1024u;

	explicit CJsonRpcFrameCodec(std::size_t maxPayloadBytes = kDefaultMaxPayloadBytes) noexcept;

	/*!
		@brief 受信バイト列を追加し、完了したペイロードを末尾に加える
		@retval true  正常に処理した（完了フレームがなくても true）
		@retval false 上限超過を検出済み、または今回検出した

		入力はヘッダー・ペイロードの途中でもよく、複数フレームを含んでもよい。
		上限超過を検出すると Failed 状態となり、Reset まで Feed は失敗する。
	*/
	bool Feed(std::string_view bytes, std::vector<std::string>& completedPayloads);

	/*!
		@brief ペイロードを 1 フレームに符号化する
		@retval false 設定上限または uint32_t の表現上限を超えた
	*/
	bool Encode(std::string_view payload, std::string& frame) const;

	//! Failed を含む状態を初期状態へ戻す
	void Reset() noexcept;

	EJsonRpcFrameCodecState GetState() const noexcept { return m_state; }
	EJsonRpcFrameCodecError GetError() const noexcept { return m_error; }
	std::size_t GetMaxPayloadBytes() const noexcept { return m_maxPayloadBytes; }

private:
	bool FinishHeader();
	void FinishPayload(std::vector<std::string>& completedPayloads);
	void Fail(EJsonRpcFrameCodecError error) noexcept;

	std::size_t				m_maxPayloadBytes;
	EJsonRpcFrameCodecState	m_state = EJsonRpcFrameCodecState::ReadingHeader;
	EJsonRpcFrameCodecError	m_error = EJsonRpcFrameCodecError::None;
	std::array<char, 4>		m_header = {};
	std::size_t				m_headerBytes = 0;
	std::uint32_t			m_expectedPayloadBytes = 0;
	std::string				m_payload;
};

#endif /* SAKURA_CJSONRPCFRAMECODEC_3C0C1A85_6B2C_4E4B_A5E0_963ED72A1B52_H_ */

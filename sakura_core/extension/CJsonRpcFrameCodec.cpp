/*!\t@file
	@brief JSON-RPC 用の長さプレフィックス・フレーム

*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "extension/CJsonRpcFrameCodec.h"

#include <algorithm>
#include <limits>
#include <utility>

CJsonRpcFrameCodec::CJsonRpcFrameCodec(std::size_t maxPayloadBytes) noexcept
	: m_maxPayloadBytes(maxPayloadBytes)
{
}

bool CJsonRpcFrameCodec::Feed(std::string_view bytes, std::vector<std::string>& completedPayloads)
{
	if (m_state == EJsonRpcFrameCodecState::Failed) {
		return false;
	}

	std::size_t offset = 0;
	while (offset < bytes.size()) {
		if (m_state == EJsonRpcFrameCodecState::ReadingHeader) {
			const auto headerBytes = std::min(bytes.size() - offset, m_header.size() - m_headerBytes);
			std::copy_n(bytes.data() + offset, headerBytes, m_header.data() + m_headerBytes);
			offset += headerBytes;
			m_headerBytes += headerBytes;

			if (m_headerBytes != m_header.size()) {
				continue;
			}
			if (!FinishHeader()) {
				return false;
			}
			if (m_expectedPayloadBytes == 0) {
				FinishPayload(completedPayloads);
			}
			continue;
		}

		const auto payloadBytes = std::min(
			bytes.size() - offset,
			static_cast<std::size_t>(m_expectedPayloadBytes) - m_payload.size());
		m_payload.append(bytes.data() + offset, payloadBytes);
		offset += payloadBytes;

		if (m_payload.size() == m_expectedPayloadBytes) {
			FinishPayload(completedPayloads);
		}
	}
	return true;
}

bool CJsonRpcFrameCodec::Encode(std::string_view payload, std::string& frame) const
{
	if (payload.size() > m_maxPayloadBytes || payload.size() > std::numeric_limits<std::uint32_t>::max()) {
		return false;
	}

	const auto payloadLength = static_cast<std::uint32_t>(payload.size());
	frame.resize(m_header.size() + payload.size());
	frame[0] = static_cast<char>((payloadLength >> 24) & 0xffu);
	frame[1] = static_cast<char>((payloadLength >> 16) & 0xffu);
	frame[2] = static_cast<char>((payloadLength >> 8) & 0xffu);
	frame[3] = static_cast<char>(payloadLength & 0xffu);
	std::copy(payload.begin(), payload.end(), frame.begin() + m_header.size());
	return true;
}

void CJsonRpcFrameCodec::Reset() noexcept
{
	m_state = EJsonRpcFrameCodecState::ReadingHeader;
	m_error = EJsonRpcFrameCodecError::None;
	m_headerBytes = 0;
	m_expectedPayloadBytes = 0;
	m_payload.clear();
}

bool CJsonRpcFrameCodec::FinishHeader()
{
	const auto payloadLength =
		(static_cast<std::uint32_t>(static_cast<unsigned char>(m_header[0])) << 24) |
		(static_cast<std::uint32_t>(static_cast<unsigned char>(m_header[1])) << 16) |
		(static_cast<std::uint32_t>(static_cast<unsigned char>(m_header[2])) << 8) |
		 static_cast<std::uint32_t>(static_cast<unsigned char>(m_header[3]));
	if (payloadLength > m_maxPayloadBytes) {
		Fail(EJsonRpcFrameCodecError::PayloadTooLarge);
		return false;
	}

	m_expectedPayloadBytes = payloadLength;
	m_payload.clear();
	m_payload.reserve(payloadLength);
	m_state = EJsonRpcFrameCodecState::ReadingPayload;
	return true;
}

void CJsonRpcFrameCodec::FinishPayload(std::vector<std::string>& completedPayloads)
{
	completedPayloads.emplace_back(std::move(m_payload));
	m_headerBytes = 0;
	m_expectedPayloadBytes = 0;
	m_state = EJsonRpcFrameCodecState::ReadingHeader;
}

void CJsonRpcFrameCodec::Fail(EJsonRpcFrameCodecError error) noexcept
{
	m_state = EJsonRpcFrameCodecState::Failed;
	m_error = error;
	m_headerBytes = 0;
	m_expectedPayloadBytes = 0;
	m_payload.clear();
}

/*! @file
	@brief Strict, bounded Debug Adapter Protocol transport codec.
*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace debug::dap {

//! The codec has no recovery path from Failed other than Reset.
enum class EDapProtocolCodecState : std::uint8_t {
	ReadingHeader,
	ReadingBody,
	Failed,
	Stopped,
};

//! A terminal error from framing, strict JSON validation, or envelope validation.
enum class EDapProtocolCodecError : std::uint8_t {
	None,
	HeaderTooLarge,
	MalformedHeader,
	DuplicateHeader,
	UnknownHeader,
	InvalidContentLength,
	ContentLengthOverflow,
	BodyTooLarge,
	InvalidUtf8,
	InvalidJson,
	InvalidEnvelope,
	OutputLimitExceeded,
	InvalidMessage,
	EncodedFrameTooLarge,
};

//! Every Feed and Encode call reports one explicit terminal or needs-more result.
enum class EDapProtocolCodecStatus : std::uint8_t {
	NeedsMore,
	Completed,
	Failed,
	Stopped,
};

struct DapProtocolCodecResult final {
	EDapProtocolCodecStatus status = EDapProtocolCodecStatus::NeedsMore;
	EDapProtocolCodecError error = EDapProtocolCodecError::None;
	std::size_t completedMessages = 0;

	[[nodiscard]] bool Succeeded() const noexcept
	{
		return status == EDapProtocolCodecStatus::NeedsMore || status == EDapProtocolCodecStatus::Completed;
	}
};

//! A decoded DAP request. argumentsJson is absent when the optional member was absent.
struct DapRequest final {
	std::uint64_t seq = 0;
	std::string command;
	std::optional<std::string> argumentsJson;
	std::string rawJson;
};

//! A decoded DAP response. bodyJson is absent when the optional member was absent.
struct DapResponse final {
	std::uint64_t seq = 0;
	std::uint64_t requestSeq = 0;
	bool success = false;
	std::string command;
	std::optional<std::string> message;
	std::optional<std::string> bodyJson;
	std::string rawJson;
};

//! A decoded DAP event. bodyJson is absent when the optional member was absent.
struct DapEvent final {
	std::uint64_t seq = 0;
	std::string event;
	std::optional<std::string> bodyJson;
	std::string rawJson;
};

using DapMessage = std::variant<DapRequest, DapResponse, DapEvent>;

//! Resource limits apply before material is retained by the codec.
struct DapProtocolCodecLimits final {
	static constexpr std::size_t kDefaultMaximumHeaderBytes = 8U * 1024U;
	static constexpr std::size_t kDefaultMaximumBodyBytes = 1024U * 1024U;
	static constexpr std::size_t kDefaultMaximumMessagesPerFeed = 64U;
	static constexpr std::size_t kDefaultMaximumJsonDepth = 64U;

	std::size_t maximumHeaderBytes = kDefaultMaximumHeaderBytes;
	std::size_t maximumBodyBytes = kDefaultMaximumBodyBytes;
	std::size_t maximumMessagesPerFeed = kDefaultMaximumMessagesPerFeed;
	std::size_t maximumJsonDepth = kDefaultMaximumJsonDepth;
};

/*! 
	@brief Incremental DAP Content-Length framing and strict envelope codec.

	The codec accepts arbitrary input chunks and permits more than one complete
	message per chunk. A malformed partial frame transitions it to Failed and is
	discarded; already appended completed messages remain valid. Feed never scans
	for a later header after an error. Call Reset explicitly before feeding a new
	transport stream. Stop similarly discards an incomplete frame and disables
	Feed and Encode until Reset.
*/
class CDapProtocolCodec final {
public:
	explicit CDapProtocolCodec(DapProtocolCodecLimits limits = {}) noexcept;

	//! Appends only fully decoded messages. Invalid partial input is never appended.
	[[nodiscard]] DapProtocolCodecResult Feed(std::string_view bytes, std::vector<DapMessage>& completedMessages);

	//! Strictly validates and canonically frames a typed DAP envelope.
	[[nodiscard]] DapProtocolCodecResult Encode(const DapMessage& message, std::string& frame) const;

	//! Stops the codec and discards any incomplete header or body.
	void Stop() noexcept;

	//! Restores ReadingHeader and clears any failure, stop state, and partial input.
	void Reset() noexcept;

	[[nodiscard]] EDapProtocolCodecState GetState() const noexcept { return m_state; }
	[[nodiscard]] EDapProtocolCodecError GetError() const noexcept { return m_error; }
	[[nodiscard]] const DapProtocolCodecLimits& GetLimits() const noexcept { return m_limits; }

private:
	[[nodiscard]] bool FinishHeader();
	[[nodiscard]] DapProtocolCodecResult FinishBody(std::vector<DapMessage>& completedMessages);
	void Fail(EDapProtocolCodecError error) noexcept;
	void ClearPartialInput() noexcept;

	DapProtocolCodecLimits m_limits;
	EDapProtocolCodecState m_state = EDapProtocolCodecState::ReadingHeader;
	EDapProtocolCodecError m_error = EDapProtocolCodecError::None;
	std::string m_header;
	std::size_t m_expectedBodyBytes = 0;
	std::string m_body;
};

} // namespace debug::dap

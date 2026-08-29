/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "terminal/runtime/TerminalRuntimeTypes.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace terminal {

class SakuraTerminalInputAdapter;

struct TerminalInputBatchLimits final {
	std::size_t maximumActions{ 256 };
	std::size_t maximumUtf16CodeUnits{ 256u * 1024u };
	std::size_t maximumEncodedBytes{ 1024u * 1024u };
	std::uint16_t maximumRepeatCount{ 1000 };
};

enum class ETerminalInputBatchEncodeCode : std::uint8_t {
	Succeeded,
	InvalidInput,
	UnsupportedKey,
	ResourceExhausted,
};

struct TerminalInputBatchEncodingResult final {
	ETerminalInputBatchEncodeCode code{ ETerminalInputBatchEncodeCode::InvalidInput };
	std::vector<std::uint8_t> bytes;

	[[nodiscard]] bool Succeeded() const noexcept
	{
		return code == ETerminalInputBatchEncodeCode::Succeeded;
	}
};

//! Validates and encodes a complete input batch before any queue is touched.
class TerminalInputBatchEncoder final {
public:
	TerminalInputBatchEncoder() = default;
	explicit TerminalInputBatchEncoder(TerminalInputBatchLimits limits) noexcept;

	[[nodiscard]] TerminalInputBatchEncodingResult Encode(
		const TerminalInputBatch& batch,
		SakuraTerminalInputAdapter& adapter,
		bool bracketedPaste) const;

private:
	TerminalInputBatchLimits m_limits;
};

using TerminalInputCommitSink =
	std::function<TerminalQueueInputResult(std::span<const std::uint8_t>)>;

//! Performs the atomic commit boundary used by the runtime service.
class TerminalInputBatchCommitter final {
public:
	TerminalInputBatchCommitter() = default;
	explicit TerminalInputBatchCommitter(TerminalInputBatchLimits limits) noexcept;

	[[nodiscard]] TerminalInputResult EncodeAndCommit(
		const TerminalInputBatch& batch,
		SakuraTerminalInputAdapter& adapter,
		bool bracketedPaste,
		std::chrono::steady_clock::time_point now,
		const TerminalInputCommitSink& commit) const;

private:
	TerminalInputBatchEncoder m_encoder;
};

} // namespace terminal

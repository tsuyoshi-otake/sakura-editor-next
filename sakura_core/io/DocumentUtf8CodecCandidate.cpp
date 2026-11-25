/*! @file @brief C++ two-pass adapter for the Rust UTF-8/CESU-8 conversion candidate. */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "DocumentUtf8CodecCandidate.h"

#include <exception>
#include <utility>
#include <vector>

namespace document_codec::rust_candidate {
#if defined(SAKURA_UTF16_RUST_CANDIDATE)
namespace {

// What differs between the two directions: the element type of the output, how many output elements one input element
// can produce at most, the field names of the descriptors and the two exports. The two-pass protocol is written once.
struct DecodeDirection {
	using Request = SakuraDocumentCodecUtf8DecodeRequestV1;
	using Buffer = SakuraDocumentCodecUtf8DecodeBufferV1;
	using Output = std::uint16_t;

	//! A decode produces one unit per input byte at most (four bytes make two units, six bytes of CESU-8 make two).
	static constexpr std::uint64_t kMaxExpansion = 1;
	//! A decode keeps every byte, giving an unreadable one a binary unit, so it never reports loss.
	static constexpr std::uint32_t kMaxLossy = 0;

	static void SetInput(Request& request, const void* data, std::uint64_t length) noexcept
	{
		request.data = static_cast<const std::uint8_t*>(data);
		request.length_bytes = length;
	}

	static void SetOutput(Buffer& buffer, Output* data, std::uint64_t capacity) noexcept
	{
		buffer.units = data;
		buffer.capacity_units = capacity;
	}

	static SakuraDocumentCodecStatus Measure(const Request* request, SakuraDocumentCodecUtf8ResultV1* output) noexcept
	{
		return sakura_document_codec_utf8_decode_measure_v1(request, output);
	}

	static SakuraDocumentCodecStatus Write(
		const Request* request, const Buffer* buffer, SakuraDocumentCodecUtf8ResultV1* output) noexcept
	{
		return sakura_document_codec_utf8_decode_write_v1(request, buffer, output);
	}
};

struct EncodeDirection {
	using Request = SakuraDocumentCodecUtf8EncodeRequestV1;
	using Buffer = SakuraDocumentCodecUtf8EncodeBufferV1;
	using Output = std::uint8_t;

	//! An encode produces three bytes per input unit at most (a BMP unit; a pair makes four or six bytes for two units).
	static constexpr std::uint64_t kMaxExpansion = 3;
	//! An encode reports 1 when an unpaired surrogate outside U+DC00..U+DCFF became '?'.
	static constexpr std::uint32_t kMaxLossy = 1;

	static void SetInput(Request& request, const void* data, std::uint64_t length) noexcept
	{
		request.data = static_cast<const std::uint16_t*>(data);
		request.length_units = length;
	}

	static void SetOutput(Buffer& buffer, Output* data, std::uint64_t capacity) noexcept
	{
		buffer.bytes = data;
		buffer.capacity_bytes = capacity;
	}

	static SakuraDocumentCodecStatus Measure(const Request* request, SakuraDocumentCodecUtf8ResultV1* output) noexcept
	{
		return sakura_document_codec_utf8_encode_measure_v1(request, output);
	}

	static SakuraDocumentCodecStatus Write(
		const Request* request, const Buffer* buffer, SakuraDocumentCodecUtf8ResultV1* output) noexcept
	{
		return sakura_document_codec_utf8_encode_write_v1(request, buffer, output);
	}
};

template <class Direction>
struct Converted final {
	const std::vector<typename Direction::Output> elements;
	const bool lossy;
	const SakuraDocumentCodecStatus status;
};

template <class Direction>
Converted<Direction> Failure(SakuraDocumentCodecStatus status)
{
	return { {}, false, status };
}

// A valid header around length and loss fields that the exports ignore on entry.
SakuraDocumentCodecUtf8ResultV1 EmptyResult() noexcept
{
	SakuraDocumentCodecUtf8ResultV1 result{};
	result.struct_size = sizeof(result);
	result.abi_version = SAKURA_DOCUMENT_CODEC_ABI_VERSION_V1;
	return result;
}

// Checks what a successful export reported before it sizes an allocation: the header, the loss flag the direction
// can report, and a length that the input cannot exceed, which also keeps the bound below from overflowing.
template <class Direction>
bool IsValidResult(const SakuraDocumentCodecUtf8ResultV1& result, std::size_t inputLength) noexcept
{
	return result.struct_size == sizeof(result)
		&& result.abi_version == SAKURA_DOCUMENT_CODEC_ABI_VERSION_V1
		&& result.reserved0 == 0
		&& result.reserved[0] == 0
		&& result.lossy <= Direction::kMaxLossy
		&& inputLength <= SAKURA_DOCUMENT_CODEC_UTF8_MAX_INPUT_V1
		&& result.output_length <= static_cast<std::uint64_t>(inputLength) * Direction::kMaxExpansion;
}

template <class Direction>
Converted<Direction> Convert(ECodeType code, const void* data, std::size_t length)
{
	typename Direction::Request request{};
	request.struct_size = sizeof(request);
	request.abi_version = SAKURA_DOCUMENT_CODEC_ABI_VERSION_V1;
	request.variant = static_cast<std::uint32_t>(code);
	Direction::SetInput(request, data, static_cast<std::uint64_t>(length));

	SakuraDocumentCodecUtf8ResultV1 measure = EmptyResult();
	const auto measured = Direction::Measure(&request, &measure);
	if (measured != SakuraDocumentCodecStatus::Ok) return Failure<Direction>(measured);
	if (!IsValidResult<Direction>(measure, length)) return Failure<Direction>(SakuraDocumentCodecStatus::InternalError);

	std::vector<typename Direction::Output> elements(static_cast<std::size_t>(measure.output_length));
	typename Direction::Buffer buffer{};
	buffer.struct_size = sizeof(buffer);
	buffer.abi_version = SAKURA_DOCUMENT_CODEC_ABI_VERSION_V1;
	Direction::SetOutput(buffer, elements.empty() ? nullptr : elements.data(), static_cast<std::uint64_t>(elements.size()));

	SakuraDocumentCodecUtf8ResultV1 output = EmptyResult();
	const auto status = Direction::Write(&request, &buffer, &output);
	if (status != SakuraDocumentCodecStatus::Ok) return Failure<Direction>(status);
	if (!IsValidResult<Direction>(output, length)
		|| output.output_length != measure.output_length
		|| output.lossy != measure.lossy) {
		return Failure<Direction>(SakuraDocumentCodecStatus::InternalError);
	}
	return { std::move(elements), output.lossy != 0, SakuraDocumentCodecStatus::Ok };
}

} // namespace
#endif

Utf8DecodeResult DecodeUtf8(ECodeType code, const void* bytes, std::size_t lengthBytes) noexcept
{
#if defined(SAKURA_UTF16_RUST_CANDIDATE)
	try {
		const auto converted = Convert<DecodeDirection>(code, bytes, lengthBytes);
		if (converted.status != SakuraDocumentCodecStatus::Ok) return { std::nullopt, converted.status };
		return { std::wstring(converted.elements.begin(), converted.elements.end()), SakuraDocumentCodecStatus::Ok };
	} catch (const std::exception&) {
		return { std::nullopt, SakuraDocumentCodecStatus::InternalError };
	}
#else
	(void)code;
	(void)bytes;
	(void)lengthBytes;
	return { std::nullopt, SakuraDocumentCodecStatus::Unsupported };
#endif
}

Utf8EncodeResult EncodeUtf8(ECodeType code, const wchar_t* units, std::size_t lengthUnits) noexcept
{
#if defined(SAKURA_UTF16_RUST_CANDIDATE)
	try {
		const auto converted = Convert<EncodeDirection>(code, units, lengthUnits);
		if (converted.status != SakuraDocumentCodecStatus::Ok) return { std::nullopt, false, converted.status };
		return {
			std::string(converted.elements.begin(), converted.elements.end()),
			converted.lossy,
			SakuraDocumentCodecStatus::Ok,
		};
	} catch (const std::exception&) {
		return { std::nullopt, false, SakuraDocumentCodecStatus::InternalError };
	}
#else
	(void)code;
	(void)units;
	(void)lengthUnits;
	return { std::nullopt, false, SakuraDocumentCodecStatus::Unsupported };
#endif
}

} // namespace document_codec::rust_candidate

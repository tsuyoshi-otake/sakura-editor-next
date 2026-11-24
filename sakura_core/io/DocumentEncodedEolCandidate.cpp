/*! @file @brief C++ two-pass adapter for the Rust encoded-EOL candidate. */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "DocumentEncodedEolCandidate.h"

#include <exception>
#include <utility>

namespace document_codec::rust_candidate {
#if defined(SAKURA_UTF16_RUST_CANDIDATE)
namespace {

SakuraDocumentCodecEolMeasureV1 EmptyMeasure() noexcept
{
	SakuraDocumentCodecEolMeasureV1 result{};
	result.struct_size = sizeof(result);
	result.abi_version = SAKURA_DOCUMENT_CODEC_ABI_VERSION_V1;
	return result;
}

bool IsValidMeasure(const SakuraDocumentCodecEolMeasureV1& measure) noexcept
{
	return measure.struct_size == sizeof(measure)
		&& measure.abi_version == SAKURA_DOCUMENT_CODEC_ABI_VERSION_V1
		&& measure.reserved[0] == 0
		&& measure.reserved[1] == 0
		&& measure.record_count <= SAKURA_DOCUMENT_CODEC_ENCODED_EOL_MAX_RECORDS_V1;
}

std::optional<EEolType> ToEolType(std::uint32_t kind) noexcept
{
	switch (kind) {
	case static_cast<std::uint32_t>(EEolType::cr_and_lf): return EEolType::cr_and_lf;
	case static_cast<std::uint32_t>(EEolType::line_feed): return EEolType::line_feed;
	case static_cast<std::uint32_t>(EEolType::carriage_return): return EEolType::carriage_return;
	case static_cast<std::uint32_t>(EEolType::next_line): return EEolType::next_line;
	case static_cast<std::uint32_t>(EEolType::line_separator): return EEolType::line_separator;
	case static_cast<std::uint32_t>(EEolType::paragraph_separator): return EEolType::paragraph_separator;
	default: return std::nullopt;
	}
}

EolResult Failure(SakuraDocumentCodecStatus status) noexcept
{
	return { std::nullopt, status };
}

EolResult ScanImpl(
	EEncodingTrait format,
	bool extendedEol,
	const void* data,
	std::size_t lengthBytes,
	std::size_t startByteOffset)
{
	SakuraDocumentCodecEolRequestV1 request{};
	request.struct_size = sizeof(request);
	request.abi_version = SAKURA_DOCUMENT_CODEC_ABI_VERSION_V1;
	request.data = static_cast<const std::uint8_t*>(data);
	request.length_bytes = static_cast<std::uint64_t>(lengthBytes);
	request.start_byte_offset = static_cast<std::uint64_t>(startByteOffset);
	request.format = static_cast<std::uint32_t>(format);
	request.extended_eol = extendedEol ? 1u : 0u;

	SakuraDocumentCodecEolMeasureV1 measure = EmptyMeasure();
	const auto measured = sakura_document_codec_scan_eol_measure_v1(&request, &measure);
	if (measured != SakuraDocumentCodecStatus::Ok) return Failure(measured);
	if (!IsValidMeasure(measure) || measure.finish_byte_offset != request.length_bytes) {
		return Failure(SakuraDocumentCodecStatus::InternalError);
	}

	std::vector<SakuraDocumentCodecEolRecordV1> wire(static_cast<std::size_t>(measure.record_count));
	SakuraDocumentCodecEolBufferV1 buffer{};
	buffer.struct_size = sizeof(buffer);
	buffer.abi_version = SAKURA_DOCUMENT_CODEC_ABI_VERSION_V1;
	buffer.records = wire.empty() ? nullptr : wire.data();
	buffer.capacity_records = static_cast<std::uint64_t>(wire.size());

	SakuraDocumentCodecEolMeasureV1 output = EmptyMeasure();
	const auto status = sakura_document_codec_scan_eol_write_v1(&request, &buffer, &output);
	if (status != SakuraDocumentCodecStatus::Ok) return Failure(status);
	if (!IsValidMeasure(output)
		|| output.record_count != measure.record_count
		|| output.finish_byte_offset != measure.finish_byte_offset) {
		return Failure(SakuraDocumentCodecStatus::InternalError);
	}

	std::vector<EolRecord> records;
	records.reserve(wire.size());
	for (const auto& record : wire) {
		const auto kind = ToEolType(record.kind);
		if (!kind) return Failure(SakuraDocumentCodecStatus::InternalError);
		records.push_back({ record.delimiter_byte_offset, record.byte_width, *kind });
	}
	return { EolScan{ std::move(records), output.finish_byte_offset }, SakuraDocumentCodecStatus::Ok };
}

} // namespace
#endif

EolResult ScanEncodedEol(
	EEncodingTrait format,
	bool extendedEol,
	const void* data,
	std::size_t lengthBytes,
	std::size_t startByteOffset) noexcept
{
#if defined(SAKURA_UTF16_RUST_CANDIDATE)
	try {
		return ScanImpl(format, extendedEol, data, lengthBytes, startByteOffset);
	} catch (const std::exception&) {
		return { std::nullopt, SakuraDocumentCodecStatus::InternalError };
	}
#else
	(void)format;
	(void)extendedEol;
	(void)data;
	(void)lengthBytes;
	(void)startByteOffset;
	return { std::nullopt, SakuraDocumentCodecStatus::Unsupported };
#endif
}

} // namespace document_codec::rust_candidate

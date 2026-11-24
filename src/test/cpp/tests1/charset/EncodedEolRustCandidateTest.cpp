/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/

#include "pch.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "EncodedEolCorpus.h"
#include "EncodedEolTestSupport.h"
#include "io/DocumentEncodedEolCandidate.h"

// Differential suite of the Rust encoded-EOL candidate (#441). The production scan (CFileLoad, as exposed by
// CFileLoadScannerForTest) and the frozen corpus are the oracles; the candidate is only compared, never selected. The
// injected-panic paths of the exports are covered by the Rust unit tests only, because C++ cannot inject them.

namespace {

using namespace encoded_eol_corpus;
namespace rust_candidate = document_codec::rust_candidate;

#if defined(SAKURA_UTF16_RUST_CANDIDATE)

using CodecStatus = SakuraDocumentCodecStatus;

// Raw, unsized storage in which a test places descriptors, data and records at chosen offsets, so every overlap is
// deliberate. Objects are copied in as bytes and only the callee reads or writes them through the returned pointers.
struct alignas(16) Arena {
	std::array<unsigned char, 512> bytes{};
};

constexpr SakuraDocumentCodecEolRecordV1 kPoison{0x0123456789abcdefull, 0xa5a5a5a5u, 0x5a5a5a5au};

template <class T>
T* PlaceAt(Arena& arena, std::size_t offset, const T& value)
{
	std::memcpy(arena.bytes.data() + offset, &value, sizeof(T));
	return reinterpret_cast<T*>(arena.bytes.data() + offset);
}

bool SameArena(const Arena& left, const Arena& right) noexcept
{
	return std::memcmp(left.bytes.data(), right.bytes.data(), left.bytes.size()) == 0;
}

std::string Utf16Le(std::initializer_list<std::uint32_t> units)
{
	std::string bytes;
	for (const auto unit : units) {
		AppendUnit(bytes, Format::Utf16Le, unit, 2);
	}
	return bytes;
}

SakuraDocumentCodecEolRequestV1 RequestOver(
	const void* data, std::size_t length, bool extendedEol = false, EEncodingTrait trait = ENCODING_TRAIT_UTF16LE)
{
	SakuraDocumentCodecEolRequestV1 request{};
	request.struct_size = sizeof(request);
	request.data = static_cast<const std::uint8_t*>(data);
	request.length_bytes = static_cast<std::uint64_t>(length);
	request.format = static_cast<std::uint32_t>(trait);
	request.extended_eol = extendedEol ? 1u : 0u;
	return request;
}

SakuraDocumentCodecEolBufferV1 BufferFor(SakuraDocumentCodecEolRecordV1* records, std::uint64_t capacity)
{
	SakuraDocumentCodecEolBufferV1 buffer{};
	buffer.struct_size = sizeof(buffer);
	buffer.records = records;
	buffer.capacity_records = capacity;
	return buffer;
}

SakuraDocumentCodecEolBufferV1 BufferOver(std::vector<SakuraDocumentCodecEolRecordV1>& records)
{
	return BufferFor(records.empty() ? nullptr : records.data(), static_cast<std::uint64_t>(records.size()));
}

// A valid header around count and finish values that the exports ignore on entry, so that a call which leaves it
// byte-for-byte unchanged is distinguishable from one that wrote a result.
SakuraDocumentCodecEolMeasureV1 SentinelOutput()
{
	SakuraDocumentCodecEolMeasureV1 output{};
	output.struct_size = sizeof(output);
	output.record_count = 0x1111222233334444ull;
	output.finish_byte_offset = 0x5555666677778888ull;
	return output;
}

bool SameBytes(const SakuraDocumentCodecEolMeasureV1& left, const SakuraDocumentCodecEolMeasureV1& right) noexcept
{
	return std::memcmp(&left, &right, sizeof(left)) == 0;
}

bool IsPoison(const SakuraDocumentCodecEolRecordV1& record) noexcept
{
	return record.delimiter_byte_offset == kPoison.delimiter_byte_offset && record.byte_width == kPoison.byte_width
		&& record.kind == kPoison.kind;
}

std::vector<SakuraDocumentCodecEolRecordV1> PoisonedRecords(std::size_t count)
{
	return std::vector<SakuraDocumentCodecEolRecordV1>(count, kPoison);
}

CodecStatus CallMeasure(const SakuraDocumentCodecEolRequestV1& request, SakuraDocumentCodecEolMeasureV1& output)
{
	return sakura_document_codec_scan_eol_measure_v1(&request, &output);
}

CodecStatus CallWrite(
	const SakuraDocumentCodecEolRequestV1& request,
	const SakuraDocumentCodecEolBufferV1& buffer,
	SakuraDocumentCodecEolMeasureV1& output)
{
	return sakura_document_codec_scan_eol_write_v1(&request, &buffer, &output);
}

void ExpectOutput(const SakuraDocumentCodecEolMeasureV1& output, std::uint64_t count, std::uint64_t finish)
{
	EXPECT_EQ(sizeof(output), output.struct_size);
	EXPECT_EQ(SAKURA_DOCUMENT_CODEC_ABI_VERSION_V1, output.abi_version);
	EXPECT_EQ(count, output.record_count);
	EXPECT_EQ(finish, output.finish_byte_offset);
	EXPECT_EQ(0u, output.reserved[0]);
	EXPECT_EQ(0u, output.reserved[1]);
}

void ExpectRecord(const SakuraDocumentCodecEolRecordV1& actual, const SakuraDocumentCodecEolRecordV1& expected)
{
	EXPECT_EQ(expected.delimiter_byte_offset, actual.delimiter_byte_offset);
	EXPECT_EQ(expected.byte_width, actual.byte_width);
	EXPECT_EQ(expected.kind, actual.kind);
}

// Both exports refuse the request with `expected`; the output descriptor and the record array keep every byte.
void ExpectBothRejected(const SakuraDocumentCodecEolRequestV1& request, CodecStatus expected)
{
	auto records = PoisonedRecords(4);
	const auto buffer = BufferOver(records);

	auto output = SentinelOutput();
	EXPECT_EQ(expected, CallMeasure(request, output));
	EXPECT_TRUE(SameBytes(SentinelOutput(), output));

	output = SentinelOutput();
	EXPECT_EQ(expected, CallWrite(request, buffer, output));
	EXPECT_TRUE(SameBytes(SentinelOutput(), output));
	EXPECT_TRUE(std::ranges::all_of(records, IsPoison));
}

// Candidate records in the corpus vocabulary, so the production scan, the corpus and the candidate compare on one footing.
std::vector<Record> ToRecords(const rust_candidate::EolScan& scan)
{
	std::vector<Record> records;
	records.reserve(scan.records.size());
	for (const auto& record : scan.records) {
		records.push_back({static_cast<std::size_t>(record.byteOffset), static_cast<std::size_t>(record.byteWidth), record.kind});
	}
	return records;
}

// The candidate's records for [start, end) of data. The finish offset must be exactly `end`, trailing content included.
std::vector<Record> CandidateRange(Format format, bool extendedEol, const char* data, std::size_t start, std::size_t end)
{
	const auto result = rust_candidate::ScanEncodedEol(TraitOf(format), extendedEol, data, end, start);
	if (!result) {
		ADD_FAILURE() << "candidate status " << static_cast<std::uint32_t>(result.status);
		return {};
	}
	EXPECT_EQ(static_cast<std::uint64_t>(end), result.scan->finishByteOffset);
	return ToRecords(*result.scan);
}

// Pairs (begin, end) of the exhaustive sweep of one case: begin on a unit boundary in [0, completeUnits * width] and end
// any byte offset in [begin, size].
std::size_t SweepPairs(std::size_t size, std::size_t width)
{
	const std::size_t units = size / width;
	return (units + 1) * (size + 1) - width * (units * (units + 1) / 2);
}

// Starts the candidate must refuse in the start test: every byte start below the size that is not on a unit boundary,
// plus the width + 1 starts past the size that the test visits.
std::size_t RefusedStarts(std::size_t size, std::size_t width)
{
	const std::size_t boundaryStarts = (size + width - 1) / width;
	return (size - boundaryStarts) + (width + 1);
}

#endif

} // namespace

#if defined(SAKURA_UTF16_RUST_CANDIDATE)

TEST(EncodedEolRustCandidate, CorpusCasesMatchTheProductionScanAndTheFrozenRecords)
{
	const auto cases = LoadCases();
	ASSERT_FALSE(cases.empty());
	CFileLoadScannerForTest scanner;
	GuardedRegion region;
	for (const auto& c : cases) {
		SCOPED_TRACE(c.Describe());
		for (bool oddAddress : {false, true}) {
			SCOPED_TRACE(oddAddress ? "odd-address" : "page-end");
			const char* data = oddAddress ? region.PlaceAtOddAddress(c.bytes) : region.PlaceAtEnd(c.bytes);
			const auto expected = ToString(c.records);
			EXPECT_EQ(expected, ToString(ScanRange(scanner, c.format, c.extendedEol, data, c.start, c.bytes.size())));
			EXPECT_EQ(expected, ToString(CandidateRange(c.format, c.extendedEol, data, c.start, c.bytes.size())));
		}
	}
}

TEST(EncodedEolRustCandidate, EveryAlignedStartAndEveryByteEndMatchesTheProductionScan)
{
	const auto cases = LoadCases();
	ASSERT_FALSE(cases.empty());
	CFileLoadScannerForTest scanner;
	GuardedRegion region;
	std::size_t compared = 0;
	std::size_t expectedPairs = 0;
	for (const auto& c : cases) {
		SCOPED_TRACE(c.Describe());
		const std::size_t width = UnitWidth(c.format);
		expectedPairs += SweepPairs(c.bytes.size(), width);
		for (std::size_t end = 0; end <= c.bytes.size(); ++end) {
			// The prefix ends at the last readable byte before the guard page, so a scan that reads past `end` faults
			// instead of silently using bytes that the full image has there. The address parity follows `end`.
			const char* data = region.PlaceAtEnd(std::string_view(c.bytes).substr(0, end));
			for (std::size_t begin = 0; begin <= end; begin += width) {
				SCOPED_TRACE(testing::Message() << "range [" << begin << ", " << end << ")");
				ExpectSameRecords(
					ScanRange(scanner, c.format, c.extendedEol, data, begin, end),
					CandidateRange(c.format, c.extendedEol, data, begin, end));
				++compared;
			}
		}
	}
	EXPECT_EQ(expectedPairs, compared);
	RecordProperty("compared_ranges", static_cast<int>(compared));
}

TEST(EncodedEolRustCandidate, GeneratedImagesAtThePartitionThresholdMatchTheProductionScan)
{
	// The images of EncodedEolContract.GeneratedFilesAtThePartitionThresholdReplayTheSingleScan: 1 MiB, 1 MiB + 1 byte and
	// 1 MiB + 1 unit, with a CR LF planted across the seed of the second partition.
	constexpr std::size_t kMiB = 1024 * 1024;
	constexpr std::size_t kSeed = kMiB / 2;
	struct Fixture {
		const char* const name;
		const std::size_t extraUnits;
		const std::size_t strayBytes;
	};
	constexpr std::array fixtures{
		Fixture{"1MiB", 0, 0},
		Fixture{"1MiB+1byte", 0, 1},
		Fixture{"1MiB+1unit", 1, 0},
	};
	CFileLoadScannerForTest scanner;
	for (const Format format : {Format::Utf16Le, Format::Utf16Be, Format::Utf32Le, Format::Utf32Be}) {
		const std::size_t width = UnitWidth(format);
		for (const auto& fixture : fixtures) {
			const std::string image = BuildImage(format, kMiB / width + fixture.extraUnits, fixture.strayBytes, kSeed / width);
			const std::size_t size = image.size();
			for (const bool extendedEol : {false, true}) {
				SCOPED_TRACE(testing::Message() << Name(format) << " " << fixture.name << " extended=" << extendedEol);
				scanner.Attach(TraitOf(format), image.data(), size, extendedEol);
				const auto single = ScanRange(scanner, format, extendedEol, image.data(), 0, size);
				EXPECT_GT(single.size(), 100u);
				ExpectSameRecords(single, CandidateRange(format, extendedEol, image.data(), 0, size));

				// The ranges the production loader would plan for this file, scanned one by one and concatenated.
				for (const int threadCount : {1, 2, 3, 4, 8}) {
					SCOPED_TRACE(testing::Message() << "threads=" << threadCount);
					std::vector<Record> replay;
					for (const auto& range : PlanPartitions(scanner, size, threadCount)) {
						for (const auto& record : CandidateRange(format, extendedEol, image.data(), range.begin, range.end)) {
							replay.push_back(record);
						}
					}
					ExpectSameRecords(single, replay);
				}
			}
		}
	}
}

TEST(EncodedEolRustCandidate, StartAtTheEndIsEmptyAndEveryUnalignedOrLateStartIsRefused)
{
	const auto cases = LoadCases();
	ASSERT_FALSE(cases.empty());
	GuardedRegion region;
	std::size_t emptyAtEnd = 0;
	std::size_t refused = 0;
	std::size_t expectedRefused = 0;
	for (const auto& c : cases) {
		SCOPED_TRACE(c.Describe());
		const std::size_t size = c.bytes.size();
		const std::size_t width = UnitWidth(c.format);
		expectedRefused += RefusedStarts(size, width);
		const char* data = region.PlaceAtEnd(c.bytes);
		for (std::size_t start = 0; start <= size + width + 1; ++start) {
			if (start < size && start % width == 0) {
				continue; // A scannable start; the sweep above compares it with the production scan.
			}
			SCOPED_TRACE(testing::Message() << "start " << start);
			const auto result = rust_candidate::ScanEncodedEol(TraitOf(c.format), c.extendedEol, data, size, start);
			if (start == size) {
				// An empty range is valid whatever the alignment of the end of a file with a partial final unit.
				++emptyAtEnd;
				ASSERT_TRUE(result);
				EXPECT_TRUE(result.scan->records.empty());
				EXPECT_EQ(static_cast<std::uint64_t>(size), result.scan->finishByteOffset);
			} else {
				++refused;
				EXPECT_FALSE(result);
				EXPECT_EQ(CodecStatus::InvalidArgument, result.status);
			}
		}
	}
	EXPECT_EQ(cases.size(), emptyAtEnd);
	EXPECT_EQ(expectedRefused, refused);
	RecordProperty("refused_starts", static_cast<int>(refused));
}

TEST(EncodedEolRustCandidate, AdapterRefusesTraitsOutsideTheFourSupportedFormats)
{
	const std::string data = Utf16Le({0x41, 0x0a});
	for (const auto trait : {ENCODING_TRAIT_ERROR, ENCODING_TRAIT_ASCII, ENCODING_TRAIT_EBCDIC_CRLF, ENCODING_TRAIT_EBCDIC}) {
		SCOPED_TRACE(testing::Message() << "trait " << static_cast<int>(trait));
		const auto result = rust_candidate::ScanEncodedEol(trait, false, data.data(), data.size(), 0);
		EXPECT_FALSE(result);
		EXPECT_EQ(CodecStatus::InvalidArgument, result.status);
	}
	// A null pointer is valid only for an empty region.
	const auto missing = rust_candidate::ScanEncodedEol(ENCODING_TRAIT_UTF16LE, false, nullptr, data.size(), 0);
	EXPECT_FALSE(missing);
	EXPECT_EQ(CodecStatus::InvalidArgument, missing.status);
	const auto empty = rust_candidate::ScanEncodedEol(ENCODING_TRAIT_UTF16LE, false, nullptr, 0, 0);
	ASSERT_TRUE(empty);
	EXPECT_TRUE(empty.scan->records.empty());
	EXPECT_EQ(0u, empty.scan->finishByteOffset);
}

TEST(EncodedEolRustCandidate, MalformedRequestsFailClosedWithoutTouchingCallerBytes)
{
	const std::string data = Utf16Le({0x41, 0x0a, 0x42, 0x0a});
	const auto valid = RequestOver(data.data(), data.size());
	{
		auto records = PoisonedRecords(4);
		auto output = SentinelOutput();
		ASSERT_EQ(CodecStatus::Ok, CallMeasure(valid, output));
		ExpectOutput(output, 2, data.size());
		output = SentinelOutput();
		ASSERT_EQ(CodecStatus::Ok, CallWrite(valid, BufferOver(records), output));
		ExpectOutput(output, 2, data.size());
	}

	const auto with = [&valid](auto edit) {
		auto request = valid;
		edit(request);
		return request;
	};
	const std::vector<std::pair<const char*, SakuraDocumentCodecEolRequestV1>> damaged{
		{"struct_size 0", with([](auto& r) { r.struct_size = 0; })},
		{"struct_size 55", with([](auto& r) { r.struct_size = 55; })},
		{"struct_size 57", with([](auto& r) { r.struct_size = 57; })},
		{"abi_version 0", with([](auto& r) { r.abi_version = 0; })},
		{"abi_version 2", with([](auto& r) { r.abi_version = 2; })},
		{"reserved[0] set", with([](auto& r) { r.reserved[0] = 1; })},
		{"reserved[1] set", with([](auto& r) { r.reserved[1] = 1; })},
		{"format 0 (error)", with([](auto& r) { r.format = 0; })},
		{"format 1 (ascii)", with([](auto& r) { r.format = 1; })},
		{"format 6 (ebcdic)", with([](auto& r) { r.format = 6; })},
		{"format 7", with([](auto& r) { r.format = 7; })},
		{"format 0xFFFFFFFF", with([](auto& r) { r.format = 0xFFFFFFFFu; })},
		{"extended_eol 2", with([](auto& r) { r.extended_eol = 2; })},
		{"extended_eol 0xFFFFFFFF", with([](auto& r) { r.extended_eol = 0xFFFFFFFFu; })},
		{"null data with a length", with([](auto& r) { r.data = nullptr; })},
		{"length beyond the address space", with([](auto& r) { r.length_bytes = 0x8000000000000000ull; })},
		{"length 2^64-1", with([](auto& r) { r.length_bytes = std::numeric_limits<std::uint64_t>::max(); })},
		{"start past the length", with([](auto& r) { r.start_byte_offset = r.length_bytes + 1; })},
		{"aligned start past the length", with([](auto& r) { r.start_byte_offset = r.length_bytes + 2; })},
		{"start 2^64-1", with([](auto& r) { r.start_byte_offset = std::numeric_limits<std::uint64_t>::max(); })},
		{"unaligned start", with([](auto& r) { r.start_byte_offset = 1; })},
		{"unaligned start in the last unit", with([](auto& r) { r.start_byte_offset = r.length_bytes - 1; })},
	};
	for (const auto& [what, request] : damaged) {
		SCOPED_TRACE(what);
		ExpectBothRejected(request, CodecStatus::InvalidArgument);
	}
}

TEST(EncodedEolRustCandidate, UnusableDescriptorsAndBuffersFailClosedInContractOrder)
{
	const std::string data = Utf16Le({0x41, 0x0a, 0x42, 0x0a});
	const auto request = RequestOver(data.data(), data.size());
	auto records = PoisonedRecords(4);
	const auto buffer = BufferOver(records);
	auto output = SentinelOutput();

	// A null descriptor pointer is a caller error, not a crash.
	EXPECT_EQ(CodecStatus::InvalidArgument, sakura_document_codec_scan_eol_measure_v1(nullptr, &output));
	EXPECT_EQ(CodecStatus::InvalidArgument, sakura_document_codec_scan_eol_measure_v1(&request, nullptr));
	EXPECT_EQ(CodecStatus::InvalidArgument, sakura_document_codec_scan_eol_write_v1(nullptr, &buffer, &output));
	EXPECT_EQ(CodecStatus::InvalidArgument, sakura_document_codec_scan_eol_write_v1(&request, nullptr, &output));
	EXPECT_EQ(CodecStatus::InvalidArgument, sakura_document_codec_scan_eol_write_v1(&request, &buffer, nullptr));
	EXPECT_TRUE(SameBytes(SentinelOutput(), output));

	// A descriptor at a misaligned address is refused before any read.
	{
		Arena arena{};
		const auto* misalignedRequest = PlaceAt(arena, 1, request);
		const auto* misalignedBuffer = PlaceAt(arena, 129, buffer);
		auto* misalignedOutput = PlaceAt(arena, 257, SentinelOutput());
		const Arena before = arena;
		EXPECT_EQ(CodecStatus::InvalidArgument, sakura_document_codec_scan_eol_measure_v1(misalignedRequest, &output));
		EXPECT_EQ(CodecStatus::InvalidArgument, sakura_document_codec_scan_eol_measure_v1(&request, misalignedOutput));
		EXPECT_EQ(CodecStatus::InvalidArgument, sakura_document_codec_scan_eol_write_v1(&request, misalignedBuffer, &output));
		EXPECT_EQ(CodecStatus::InvalidArgument, sakura_document_codec_scan_eol_write_v1(&request, &buffer, misalignedOutput));
		EXPECT_TRUE(SameArena(before, arena));
		EXPECT_TRUE(SameBytes(SentinelOutput(), output));
		EXPECT_TRUE(std::ranges::all_of(records, IsPoison));
	}

	// The output descriptor needs a valid header as well: a failed call leaves every byte of it unchanged.
	const auto withOutput = [](auto edit) {
		auto damaged = SentinelOutput();
		edit(damaged);
		return damaged;
	};
	const std::vector<std::pair<const char*, SakuraDocumentCodecEolMeasureV1>> badOutputs{
		{"struct_size 0", withOutput([](auto& o) { o.struct_size = 0; })},
		{"struct_size 39", withOutput([](auto& o) { o.struct_size = 39; })},
		{"struct_size 41", withOutput([](auto& o) { o.struct_size = 41; })},
		{"abi_version 0", withOutput([](auto& o) { o.abi_version = 0; })},
		{"abi_version 2", withOutput([](auto& o) { o.abi_version = 2; })},
		{"reserved[0] set", withOutput([](auto& o) { o.reserved[0] = 1; })},
		{"reserved[1] set", withOutput([](auto& o) { o.reserved[1] = 1; })},
	};
	for (const auto& [what, damaged] : badOutputs) {
		SCOPED_TRACE(what);
		auto measureOutput = damaged;
		EXPECT_EQ(CodecStatus::InvalidArgument, CallMeasure(request, measureOutput));
		EXPECT_TRUE(SameBytes(damaged, measureOutput));
		auto writeOutput = damaged;
		EXPECT_EQ(CodecStatus::InvalidArgument, CallWrite(request, buffer, writeOutput));
		EXPECT_TRUE(SameBytes(damaged, writeOutput));
	}

	// Caller-contract failures of the buffer are InvalidArgument; size failures are InvalidCapacity.
	alignas(16) Arena scratch{};
	const auto withBuffer = [&buffer](auto edit) {
		auto damaged = buffer;
		edit(damaged);
		return damaged;
	};
	const std::vector<std::pair<const char*, SakuraDocumentCodecEolBufferV1>> badBuffers{
		{"struct_size 39", withBuffer([](auto& b) { b.struct_size = 39; })},
		{"struct_size 41", withBuffer([](auto& b) { b.struct_size = 41; })},
		{"abi_version 0", withBuffer([](auto& b) { b.abi_version = 0; })},
		{"abi_version 2", withBuffer([](auto& b) { b.abi_version = 2; })},
		{"reserved[0] set", withBuffer([](auto& b) { b.reserved[0] = 1; })},
		{"reserved[1] set", withBuffer([](auto& b) { b.reserved[1] = 1; })},
		{"null records with a capacity", withBuffer([](auto& b) { b.records = nullptr; })},
		{"records at an odd address", withBuffer([&scratch](auto& b) {
			b.records = reinterpret_cast<SakuraDocumentCodecEolRecordV1*>(scratch.bytes.data() + 1);
		})},
		{"capacity whose byte range wraps the address space", withBuffer([](auto& b) {
			b.capacity_records = (std::uint64_t{1} << 59) - 1;
		})},
	};
	const Arena scratchBefore = scratch;
	for (const auto& [what, damaged] : badBuffers) {
		SCOPED_TRACE(what);
		auto writeOutput = SentinelOutput();
		EXPECT_EQ(CodecStatus::InvalidArgument, CallWrite(request, damaged, writeOutput));
		EXPECT_TRUE(SameBytes(SentinelOutput(), writeOutput));
	}
	EXPECT_TRUE(SameArena(scratchBefore, scratch));

	// The capacity arithmetic is checked before the records pointer, so even a null pointer reports InvalidCapacity.
	const std::vector<std::pair<const char*, SakuraDocumentCodecEolBufferV1>> impossibleBuffers{
		{"capacity 2^64-1", withBuffer([](auto& b) { b.capacity_records = std::numeric_limits<std::uint64_t>::max(); })},
		{"capacity 2^64-1 with null records", withBuffer([](auto& b) {
			b.capacity_records = std::numeric_limits<std::uint64_t>::max();
			b.records = nullptr;
		})},
		{"capacity 2^59 (a byte size of 2^63)", withBuffer([](auto& b) { b.capacity_records = std::uint64_t{1} << 59; })},
	};
	for (const auto& [what, damaged] : impossibleBuffers) {
		SCOPED_TRACE(what);
		auto writeOutput = SentinelOutput();
		EXPECT_EQ(CodecStatus::InvalidCapacity, CallWrite(request, damaged, writeOutput));
		EXPECT_TRUE(SameBytes(SentinelOutput(), writeOutput));
	}

	// The request is validated before the buffer: a bad request with an impossible capacity reports the request error.
	auto badRequest = request;
	badRequest.format = 0;
	auto impossible = buffer;
	impossible.capacity_records = std::numeric_limits<std::uint64_t>::max();
	auto writeOutput = SentinelOutput();
	EXPECT_EQ(CodecStatus::InvalidArgument, CallWrite(badRequest, impossible, writeOutput));
	EXPECT_TRUE(SameBytes(SentinelOutput(), writeOutput));
	EXPECT_TRUE(std::ranges::all_of(records, IsPoison));
}

TEST(EncodedEolRustCandidate, WriteNeverPublishesPartialRecordsAndMeasureAndWriteAreIndependent)
{
	// A LF B CR LF C CR D NEL LS PS: six markers with the extended EOLs on, three without. Offsets and widths are bytes.
	const std::string data = Utf16Le({0x41, 0x0a, 0x42, 0x0d, 0x0a, 0x43, 0x0d, 0x44, 0x85, 0x2028, 0x2029});
	constexpr std::array<SakuraDocumentCodecEolRecordV1, 6> kExpected{{
		{2, 2, 2}, {6, 4, 1}, {12, 2, 3}, {16, 2, 4}, {18, 2, 5}, {20, 2, 6},
	}};
	const auto request = RequestOver(data.data(), data.size(), true);

	auto output = SentinelOutput();
	ASSERT_EQ(CodecStatus::Ok, CallMeasure(request, output));
	ExpectOutput(output, kExpected.size(), data.size());

	// Too little room: nothing is written, not even a prefix.
	for (const std::size_t capacity : {std::size_t{0}, std::size_t{5}}) {
		SCOPED_TRACE(testing::Message() << "capacity " << capacity);
		auto records = PoisonedRecords(capacity);
		output = SentinelOutput();
		EXPECT_EQ(CodecStatus::InvalidCapacity, CallWrite(request, BufferOver(records), output));
		EXPECT_TRUE(SameBytes(SentinelOutput(), output));
		EXPECT_TRUE(std::ranges::all_of(records, IsPoison));
	}

	// Exact room and spare room: the markers are published in order and the spare suffix stays intact.
	for (const std::size_t capacity : {std::size_t{6}, std::size_t{9}}) {
		SCOPED_TRACE(testing::Message() << "capacity " << capacity);
		auto records = PoisonedRecords(capacity);
		output = SentinelOutput();
		ASSERT_EQ(CodecStatus::Ok, CallWrite(request, BufferOver(records), output));
		ExpectOutput(output, kExpected.size(), data.size());
		for (std::size_t i = 0; i < records.size(); ++i) {
			if (i < kExpected.size()) {
				ExpectRecord(records[i], kExpected[i]);
			} else {
				EXPECT_TRUE(IsPoison(records[i])) << "record " << i;
			}
		}
	}

	// Measure and write share nothing: a buffer sized for one region serves another, and write needs no measure.
	const std::string other = Utf16Le({0x41, 0x0a, 0x42, 0x0a});
	{
		auto records = PoisonedRecords(kExpected.size());
		output = SentinelOutput();
		ASSERT_EQ(CodecStatus::Ok, CallWrite(RequestOver(other.data(), other.size(), true), BufferOver(records), output));
		ExpectOutput(output, 2, other.size());
		ExpectRecord(records[0], {2, 2, 2});
		ExpectRecord(records[1], {6, 2, 2});
		for (std::size_t i = 2; i < records.size(); ++i) {
			EXPECT_TRUE(IsPoison(records[i])) << "record " << i;
		}
	}

	// Without the extended EOLs only LF, CR LF and CR remain.
	output = SentinelOutput();
	ASSERT_EQ(CodecStatus::Ok, CallMeasure(RequestOver(data.data(), data.size(), false), output));
	ExpectOutput(output, 3, data.size());

	// No marker: an empty record array with a null pointer is valid, and a spare array stays untouched.
	const std::string plain = Utf16Le({0x41, 0x42, 0x43});
	{
		std::vector<SakuraDocumentCodecEolRecordV1> none;
		output = SentinelOutput();
		ASSERT_EQ(CodecStatus::Ok, CallWrite(RequestOver(plain.data(), plain.size()), BufferOver(none), output));
		ExpectOutput(output, 0, plain.size());
		auto spare = PoisonedRecords(2);
		output = SentinelOutput();
		ASSERT_EQ(CodecStatus::Ok, CallWrite(RequestOver(plain.data(), plain.size()), BufferOver(spare), output));
		ExpectOutput(output, 0, plain.size());
		EXPECT_TRUE(std::ranges::all_of(spare, IsPoison));
	}

	// The empty region needs no pointer at all.
	output = SentinelOutput();
	ASSERT_EQ(CodecStatus::Ok, CallMeasure(RequestOver(nullptr, 0), output));
	ExpectOutput(output, 0, 0);
}

TEST(EncodedEolRustCandidate, TheRecordCeilingIsExactlyOneMebiRecords)
{
	constexpr std::size_t kCeiling = static_cast<std::size_t>(SAKURA_DOCUMENT_CODEC_ENCODED_EOL_MAX_RECORDS_V1);
	const auto lineFeeds = [](std::size_t count) {
		std::string bytes;
		bytes.reserve(count * 2);
		for (std::size_t i = 0; i < count; ++i) {
			AppendUnit(bytes, Format::Utf16Le, 0x0a, 2);
		}
		return bytes;
	};

	{
		const std::string data = lineFeeds(kCeiling);
		const auto result = rust_candidate::ScanEncodedEol(ENCODING_TRAIT_UTF16LE, false, data.data(), data.size(), 0);
		ASSERT_TRUE(result);
		ASSERT_EQ(kCeiling, result.scan->records.size());
		EXPECT_EQ(static_cast<std::uint64_t>(data.size()), result.scan->finishByteOffset);
		EXPECT_EQ(0u, result.scan->records.front().byteOffset);
		EXPECT_EQ(2 * (kCeiling - 1), result.scan->records.back().byteOffset);
		EXPECT_EQ(2u, result.scan->records.back().byteWidth);
		EXPECT_EQ(EEolType::line_feed, result.scan->records.back().kind);
	}
	{
		const std::string data = lineFeeds(kCeiling + 1);
		const auto result = rust_candidate::ScanEncodedEol(ENCODING_TRAIT_UTF16LE, false, data.data(), data.size(), 0);
		EXPECT_FALSE(result);
		EXPECT_EQ(CodecStatus::InvalidCapacity, result.status);

		// The raw exports agree, and a buffer larger than the ceiling stays untouched by the refused write.
		const auto request = RequestOver(data.data(), data.size());
		auto records = PoisonedRecords(kCeiling + 1);
		auto output = SentinelOutput();
		EXPECT_EQ(CodecStatus::InvalidCapacity, CallMeasure(request, output));
		EXPECT_TRUE(SameBytes(SentinelOutput(), output));
		EXPECT_EQ(CodecStatus::InvalidCapacity, CallWrite(request, BufferOver(records), output));
		EXPECT_TRUE(SameBytes(SentinelOutput(), output));
		EXPECT_TRUE(std::ranges::all_of(records, IsPoison));
	}
}

TEST(EncodedEolRustCandidate, WritableOverlapsFailClosedAndReadOnlyOverlapsAreAccepted)
{
	// A LF B CR LF C D E: sixteen bytes with three markers, copied to the start of every arena.
	const std::string text = Utf16Le({0x41, 0x0a, 0x42, 0x0d, 0x0a, 0x43, 0x44, 0x45});
	ASSERT_EQ(16u, text.size());
	const auto freshArena = [&text]() {
		Arena arena{};
		std::memcpy(arena.bytes.data(), text.data(), text.size());
		return arena;
	};

	// The output descriptor inside the scanned region, for both exports.
	{
		Arena arena = freshArena();
		const auto request = RequestOver(arena.bytes.data(), text.size());
		auto* output = PlaceAt(arena, 8, SentinelOutput());
		auto records = PoisonedRecords(4);
		const Arena before = arena;
		EXPECT_EQ(CodecStatus::InvalidArgument, CallMeasure(request, *output));
		EXPECT_EQ(CodecStatus::InvalidArgument, CallWrite(request, BufferOver(records), *output));
		EXPECT_TRUE(SameArena(before, arena));
		EXPECT_TRUE(std::ranges::all_of(records, IsPoison));
	}

	// The record array over the scanned region, with room for every marker and with too little room: the overlap is
	// reported first.
	for (const std::uint64_t capacity : {std::uint64_t{4}, std::uint64_t{1}}) {
		SCOPED_TRACE(testing::Message() << "capacity " << capacity);
		Arena arena = freshArena();
		const auto request = RequestOver(arena.bytes.data(), text.size());
		auto* records = reinterpret_cast<SakuraDocumentCodecEolRecordV1*>(arena.bytes.data() + 8);
		auto output = SentinelOutput();
		const Arena before = arena;
		EXPECT_EQ(CodecStatus::InvalidArgument, CallWrite(request, BufferFor(records, capacity), output));
		EXPECT_TRUE(SameArena(before, arena));
		EXPECT_TRUE(SameBytes(SentinelOutput(), output));
	}

	// The request descriptor, the buffer descriptor and the output descriptor, each inside the record array.
	{
		Arena arena = freshArena();
		auto* records = reinterpret_cast<SakuraDocumentCodecEolRecordV1*>(arena.bytes.data() + 256);
		const auto* request = PlaceAt(arena, 288, RequestOver(arena.bytes.data(), text.size()));
		auto output = SentinelOutput();
		const Arena before = arena;
		EXPECT_EQ(CodecStatus::InvalidArgument, CallWrite(*request, BufferFor(records, 8), output));
		EXPECT_TRUE(SameArena(before, arena));
		EXPECT_TRUE(SameBytes(SentinelOutput(), output));
	}
	{
		Arena arena = freshArena();
		auto* records = reinterpret_cast<SakuraDocumentCodecEolRecordV1*>(arena.bytes.data() + 256);
		const auto request = RequestOver(arena.bytes.data(), text.size());
		const auto* buffer = PlaceAt(arena, 288, BufferFor(records, 8));
		auto output = SentinelOutput();
		const Arena before = arena;
		EXPECT_EQ(CodecStatus::InvalidArgument, CallWrite(request, *buffer, output));
		EXPECT_TRUE(SameArena(before, arena));
		EXPECT_TRUE(SameBytes(SentinelOutput(), output));
	}
	{
		Arena arena = freshArena();
		auto* records = reinterpret_cast<SakuraDocumentCodecEolRecordV1*>(arena.bytes.data() + 256);
		const auto request = RequestOver(arena.bytes.data(), text.size());
		auto* output = PlaceAt(arena, 288, SentinelOutput());
		const Arena before = arena;
		EXPECT_EQ(CodecStatus::InvalidArgument, CallWrite(request, BufferFor(records, 8), *output));
		EXPECT_TRUE(SameArena(before, arena));
	}

	// Read-only overlaps are allowed: a descriptor may lie inside the scanned region because the call only reads both.
	// The bytes of the descriptor are scanned like any others, so only the agreement of the two calls is compared.
	{
		Arena arena = freshArena();
		const auto request = RequestOver(arena.bytes.data(), 128);
		const auto* placed = PlaceAt(arena, 64, request);
		auto records = PoisonedRecords(65);
		auto measured = SentinelOutput();
		auto written = SentinelOutput();
		ASSERT_EQ(CodecStatus::Ok, CallMeasure(*placed, measured));
		ASSERT_EQ(CodecStatus::Ok, CallWrite(*placed, BufferOver(records), written));
		EXPECT_EQ(128u, measured.finish_byte_offset);
		EXPECT_EQ(128u, written.finish_byte_offset);
		EXPECT_EQ(measured.record_count, written.record_count);
		EXPECT_LE(written.record_count, 64u);
	}
	{
		Arena arena = freshArena();
		const auto request = RequestOver(arena.bytes.data(), 128);
		auto records = PoisonedRecords(65);
		const auto* placed = PlaceAt(arena, 64, BufferOver(records));
		auto measured = SentinelOutput();
		auto written = SentinelOutput();
		ASSERT_EQ(CodecStatus::Ok, CallMeasure(request, measured));
		ASSERT_EQ(CodecStatus::Ok, CallWrite(request, *placed, written));
		EXPECT_EQ(128u, written.finish_byte_offset);
		EXPECT_EQ(measured.record_count, written.record_count);
	}
}

#else

TEST(EncodedEolRustCandidate, IsExplicitlyUnavailableWithoutNativeRustArchive)
{
	const auto result = rust_candidate::ScanEncodedEol(ENCODING_TRAIT_UTF16LE, false, nullptr, 0, 0);
	EXPECT_FALSE(result);
	EXPECT_EQ(SakuraDocumentCodecStatus::Unsupported, result.status);
}

#endif

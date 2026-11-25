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
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "CodecGoldenCorpus.h"
#include "TestMemoryPlacement.h"
#include "Utf8CodecCorpus.h"
#include "Utf8CodecTestSupport.h"
#include "basis/CEol.h"
#include "charset/CCodeFactory.h"
#include "env/DLLSHAREDATA.h"
#include "env/ShareDataTestSuite.hpp"
#include "io/CFileLoad.h"
#include "io/CIoBridge.h"
#include "io/DocumentUtf8CodecCandidate.h"

// Differential suite of the Rust UTF-8/CESU-8 candidate (#443). The production converters (CUtf8 and CCesu8, run through
// the guarded raw loops of Utf8CodecTestSupport.h and through the real callers CFileLoad and CIoBridge) and the frozen
// corpus are the oracles; the candidate is only compared, never selected. The injected-panic paths and the acceptance of
// an input at the 2 GiB bound are covered by the Rust unit tests only, because C++ can neither inject a panic nor
// supply that much input.

namespace {

using namespace utf8_codec_corpus;
namespace rust_candidate = document_codec::rust_candidate;

#if defined(SAKURA_UTF16_RUST_CANDIDATE)

using CodecStatus = SakuraDocumentCodecStatus;
using ResultV1 = SakuraDocumentCodecUtf8ResultV1;

constexpr std::uint64_t kMaxInput = SAKURA_DOCUMENT_CODEC_UTF8_MAX_INPUT_V1;
constexpr std::uint32_t kUtf8Variant = static_cast<std::uint32_t>(CODE_UTF8);
constexpr std::uintptr_t kAddressMax = static_cast<std::uintptr_t>(std::numeric_limits<std::ptrdiff_t>::max());
constexpr std::uint64_t kAll = std::numeric_limits<std::uint64_t>::max();

ECodeType CodeOf(Mode mode)
{
	return IsCesu8(mode) ? CODE_CESU8 : CODE_UTF8;
}

std::uint32_t VariantOf(Mode mode)
{
	return static_cast<std::uint32_t>(CodeOf(mode));
}

// A valid header around length and loss fields that the exports ignore on entry, so that a call which leaves it byte for
// byte unchanged is distinguishable from one that wrote a result.
ResultV1 SentinelResult()
{
	ResultV1 result{};
	result.struct_size = sizeof(result);
	result.abi_version = SAKURA_DOCUMENT_CODEC_ABI_VERSION_V1;
	result.output_length = 0xA5A5A5A5A5A5A5A5ull;
	result.lossy = 0x5A5A5A5Au;
	return result;
}

template <class T>
std::array<unsigned char, sizeof(T)> BytesOfObject(const T& value) noexcept
{
	std::array<unsigned char, sizeof(T)> bytes{};
	std::memcpy(bytes.data(), &value, sizeof(T));
	return bytes;
}

template <class T>
bool SameBytes(const T& left, const T& right) noexcept
{
	return std::memcmp(&left, &right, sizeof(T)) == 0;
}

// One direction of the two-sided ABI. Everything that differs between decode and encode is named here, so that each ABI
// rule is written once and run for both: the descriptor types, the element types, the two exports, one conversion that
// both variants agree on, and the production conversion that serves as the reference.
struct DecodeSide {
	using Request = SakuraDocumentCodecUtf8DecodeRequestV1;
	using Buffer = SakuraDocumentCodecUtf8DecodeBufferV1;
	using Input = std::uint8_t;
	using Output = std::uint16_t;

	static constexpr const char* kName = "decode";
	static constexpr Output kSentinel = 0xA5A5;

	// "A" and U+3042 in UTF-8.
	static std::vector<Input> SampleInput() { return { 0x41, 0xE3, 0x81, 0x82 }; }
	static std::vector<Output> SampleOutput() { return { 0x0041, 0x3042 }; }

	static std::uint64_t& Length(Request& request) noexcept { return request.length_bytes; }
	static std::uint64_t& Capacity(Buffer& buffer) noexcept { return buffer.capacity_units; }
	static Output*& Array(Buffer& buffer) noexcept { return buffer.units; }

	static CodecStatus Measure(const Request* request, ResultV1* output) noexcept
	{
		return sakura_document_codec_utf8_decode_measure_v1(request, output);
	}

	static CodecStatus Write(const Request* request, const Buffer* buffer, ResultV1* output) noexcept
	{
		return sakura_document_codec_utf8_decode_write_v1(request, buffer, output);
	}

	// The production conversion of the bytes of an input region, as elements of the output type.
	static void Reference(
		RawLoops& loops, const unsigned char* region, std::size_t regionBytes, std::vector<Output>& output, bool& lossy)
	{
		const auto decoded = loops.Decode(std::string_view(reinterpret_cast<const char*>(region), regionBytes), Mode::Utf8);
		output.clear();
		for (const wchar_t unit : decoded.units) {
			output.push_back(static_cast<Output>(unit));
		}
		lossy = false;
	}
};

struct EncodeSide {
	using Request = SakuraDocumentCodecUtf8EncodeRequestV1;
	using Buffer = SakuraDocumentCodecUtf8EncodeBufferV1;
	using Input = std::uint16_t;
	using Output = std::uint8_t;

	static constexpr const char* kName = "encode";
	static constexpr Output kSentinel = 0xA5;

	static std::vector<Input> SampleInput() { return { 0x0041, 0x3042 }; }
	static std::vector<Output> SampleOutput() { return { 0x41, 0xE3, 0x81, 0x82 }; }

	static std::uint64_t& Length(Request& request) noexcept { return request.length_units; }
	static std::uint64_t& Capacity(Buffer& buffer) noexcept { return buffer.capacity_bytes; }
	static Output*& Array(Buffer& buffer) noexcept { return buffer.bytes; }

	static CodecStatus Measure(const Request* request, ResultV1* output) noexcept
	{
		return sakura_document_codec_utf8_encode_measure_v1(request, output);
	}

	static CodecStatus Write(const Request* request, const Buffer* buffer, ResultV1* output) noexcept
	{
		return sakura_document_codec_utf8_encode_write_v1(request, buffer, output);
	}

	static void Reference(
		RawLoops& loops, const unsigned char* region, std::size_t regionBytes, std::vector<Output>& output, bool& lossy)
	{
		std::wstring units(regionBytes / sizeof(wchar_t), L'\0');
		std::memcpy(units.data(), region, units.size() * sizeof(wchar_t));
		const auto encoded = loops.Encode(units, Mode::Utf8);
		output.clear();
		for (const char byte : encoded.bytes) {
			output.push_back(static_cast<Output>(byte));
		}
		lossy = encoded.lossy;
	}
};

template <class Side>
typename Side::Request RequestOver(const void* data, std::uint64_t length, std::uint32_t variant = kUtf8Variant)
{
	typename Side::Request request{};
	request.struct_size = sizeof(request);
	request.abi_version = SAKURA_DOCUMENT_CODEC_ABI_VERSION_V1;
	request.data = static_cast<const typename Side::Input*>(data);
	Side::Length(request) = length;
	request.variant = variant;
	return request;
}

template <class Side>
typename Side::Buffer BufferFor(typename Side::Output* array, std::uint64_t capacity)
{
	typename Side::Buffer buffer{};
	buffer.struct_size = sizeof(buffer);
	buffer.abi_version = SAKURA_DOCUMENT_CODEC_ABI_VERSION_V1;
	Side::Array(buffer) = array;
	Side::Capacity(buffer) = capacity;
	return buffer;
}

template <class Side>
std::vector<typename Side::Output> Poisoned(std::size_t count)
{
	return std::vector<typename Side::Output>(count, Side::kSentinel);
}

template <class Side>
bool AllPoison(const std::vector<typename Side::Output>& elements)
{
	return std::ranges::all_of(elements, [](typename Side::Output element) { return element == Side::kSentinel; });
}

template <class Side>
CodecStatus CallMeasure(const typename Side::Request& request, ResultV1& output)
{
	return Side::Measure(&request, &output);
}

template <class Side>
CodecStatus CallWrite(const typename Side::Request& request, const typename Side::Buffer& buffer, ResultV1& output)
{
	return Side::Write(&request, &buffer, &output);
}

void ExpectResult(const ResultV1& result, std::uint64_t length, bool lossy)
{
	EXPECT_EQ(sizeof(result), result.struct_size);
	EXPECT_EQ(SAKURA_DOCUMENT_CODEC_ABI_VERSION_V1, result.abi_version);
	EXPECT_EQ(length, result.output_length);
	EXPECT_EQ(lossy ? 1u : 0u, result.lossy);
	EXPECT_EQ(0u, result.reserved0);
	EXPECT_EQ(0u, result.reserved[0]);
}

// Both exports refuse the request with `expected`; the result descriptor, the request and the array keep every byte.
template <class Side>
void ExpectBothRejected(const typename Side::Request& request, CodecStatus expected)
{
	auto elements = Poisoned<Side>(4);
	const auto buffer = BufferFor<Side>(elements.data(), elements.size());
	const auto requestBefore = BytesOfObject(request);
	const auto bufferBefore = BytesOfObject(buffer);

	auto result = SentinelResult();
	EXPECT_EQ(expected, CallMeasure<Side>(request, result));
	EXPECT_TRUE(SameBytes(SentinelResult(), result));

	result = SentinelResult();
	EXPECT_EQ(expected, CallWrite<Side>(request, buffer, result));
	EXPECT_TRUE(SameBytes(SentinelResult(), result));
	EXPECT_TRUE(AllPoison<Side>(elements));
	EXPECT_EQ(requestBefore, BytesOfObject(request));
	EXPECT_EQ(bufferBefore, BytesOfObject(buffer));
}

template <class Side>
void ExpectMeasureFailure(const typename Side::Request& request, ResultV1 result, CodecStatus expected)
{
	const auto requestBefore = BytesOfObject(request);
	const auto resultBefore = BytesOfObject(result);
	EXPECT_EQ(expected, CallMeasure<Side>(request, result));
	EXPECT_EQ(resultBefore, BytesOfObject(result));
	EXPECT_EQ(requestBefore, BytesOfObject(request));
}

// A write over an array of `arraySize` poisoned elements, after `configure` has edited the buffer and the result
// descriptor, is refused with `expected`, and nothing the caller owns changes.
template <class Side, class Configure>
void ExpectWriteFailure(
	const typename Side::Request& request, std::size_t arraySize, Configure&& configure, CodecStatus expected)
{
	auto elements = Poisoned<Side>(arraySize);
	auto buffer = BufferFor<Side>(elements.data(), elements.size());
	auto result = SentinelResult();
	configure(buffer, result);
	const auto requestBefore = BytesOfObject(request);
	const auto bufferBefore = BytesOfObject(buffer);
	const auto resultBefore = BytesOfObject(result);
	EXPECT_EQ(expected, CallWrite<Side>(request, buffer, result));
	EXPECT_EQ(resultBefore, BytesOfObject(result));
	EXPECT_EQ(bufferBefore, BytesOfObject(buffer));
	EXPECT_EQ(requestBefore, BytesOfObject(request));
	EXPECT_TRUE(AllPoison<Side>(elements));
}

// Measure reports the expected output; a write with exactly that capacity and with spare capacity publishes it and the
// same result and leaves the spare elements alone; a write one element short changes nothing.
template <class Side>
void ExpectSuccess(const typename Side::Request& request, const std::vector<typename Side::Output>& expected, bool lossy)
{
	const auto requestBefore = BytesOfObject(request);
	auto measured = SentinelResult();
	ASSERT_EQ(CodecStatus::Ok, CallMeasure<Side>(request, measured));
	ExpectResult(measured, expected.size(), lossy);
	EXPECT_EQ(requestBefore, BytesOfObject(request));

	for (const std::size_t capacity : { expected.size(), expected.size() + 3 }) {
		SCOPED_TRACE(testing::Message() << "capacity " << capacity);
		auto elements = Poisoned<Side>(capacity);
		const auto buffer = BufferFor<Side>(elements.data(), elements.size());
		const auto bufferBefore = BytesOfObject(buffer);
		auto written = SentinelResult();
		ASSERT_EQ(CodecStatus::Ok, CallWrite<Side>(request, buffer, written));
		EXPECT_EQ(BytesOfObject(measured), BytesOfObject(written));
		EXPECT_EQ(requestBefore, BytesOfObject(request));
		EXPECT_EQ(bufferBefore, BytesOfObject(buffer));
		EXPECT_TRUE(std::equal(expected.begin(), expected.end(), elements.begin()));
		EXPECT_TRUE(std::all_of(elements.begin() + static_cast<std::ptrdiff_t>(expected.size()), elements.end(),
			[](typename Side::Output element) { return element == Side::kSentinel; }));
	}
	if (!expected.empty()) {
		ExpectWriteFailure<Side>(request, expected.size() - 1, [](auto&, auto&) {}, CodecStatus::InvalidCapacity);
	}
}

// The exports called over caller-owned storage that is reused between calls, the counterpart of RawLoops. A guard of
// sentinel elements follows the measured length, so a write beyond it is caught, and a call counts as `ok` only when both
// exports succeed, report a valid result and agree on it. The views a call returns stay valid until the next call of
// the same kind.
class RawCandidate {
public:
	struct Decoded {
		const std::wstring_view units;
		const bool ok;
	};

	struct Encoded {
		const std::string_view bytes;
		const bool lossy;
		const bool ok;
	};

	Decoded Decode(std::string_view input, Mode mode)
	{
		const auto request = RequestOver<DecodeSide>(input.data(), input.size(), VariantOf(mode));
		auto measured = SentinelResult();
		if (DecodeSide::Measure(&request, &measured) != CodecStatus::Ok || !IsValid(measured, input.size(), 0)) {
			return { std::wstring_view{}, false };
		}
		const auto length = static_cast<std::size_t>(measured.output_length);
		m_units.assign(length + kGuard, kUnitSentinel);
		const auto buffer = BufferFor<DecodeSide>(reinterpret_cast<std::uint16_t*>(m_units.data()), length);
		auto written = SentinelResult();
		const bool ok = DecodeSide::Write(&request, &buffer, &written) == CodecStatus::Ok && SameBytes(measured, written)
			&& GuardIntact(m_units, length, kUnitSentinel);
		return { std::wstring_view{ m_units.data(), ok ? length : 0 }, ok };
	}

	Encoded Encode(std::wstring_view input, Mode mode)
	{
		const auto request = RequestOver<EncodeSide>(input.data(), input.size(), VariantOf(mode));
		auto measured = SentinelResult();
		if (EncodeSide::Measure(&request, &measured) != CodecStatus::Ok || !IsValid(measured, input.size() * 3, 1)) {
			return { std::string_view{}, false, false };
		}
		const auto length = static_cast<std::size_t>(measured.output_length);
		m_bytes.assign(length + kGuard, kByteSentinel);
		const auto buffer = BufferFor<EncodeSide>(reinterpret_cast<std::uint8_t*>(m_bytes.data()), length);
		auto written = SentinelResult();
		const bool ok = EncodeSide::Write(&request, &buffer, &written) == CodecStatus::Ok && SameBytes(measured, written)
			&& GuardIntact(m_bytes, length, kByteSentinel);
		return { std::string_view{ m_bytes.data(), ok ? length : 0 }, ok && measured.lossy != 0, ok };
	}

private:
	static bool IsValid(const ResultV1& result, std::size_t maxLength, std::uint32_t maxLossy) noexcept
	{
		return result.struct_size == sizeof(result) && result.abi_version == SAKURA_DOCUMENT_CODEC_ABI_VERSION_V1
			&& result.reserved0 == 0 && result.reserved[0] == 0 && result.lossy <= maxLossy && result.output_length <= maxLength;
	}

	template <class Element>
	static bool GuardIntact(const std::vector<Element>& storage, std::size_t length, Element sentinel)
	{
		return std::all_of(storage.begin() + static_cast<std::ptrdiff_t>(length), storage.end(),
			[sentinel](Element element) { return element == sentinel; });
	}

	static constexpr std::size_t kGuard = 8;
	static constexpr wchar_t kUnitSentinel = static_cast<wchar_t>(0xA5A5);
	static constexpr char kByteSentinel = static_cast<char>(0xA5);

	std::vector<wchar_t> m_units;
	std::vector<char> m_bytes;
};

std::string Clip(std::string text)
{
	constexpr std::size_t kLimit = 200;
	if (text.size() > kLimit) {
		text.resize(kLimit);
		text += "...";
	}
	return text;
}

// Runs the candidate and the production loops over the same input and counts the pairs that disagree. A call returns
// the candidate's output, so a sweep can feed it into the opposite direction.
class Differential {
public:
	struct Encoded {
		const std::string_view bytes;
		const bool lossy;
	};

	std::wstring_view Decode(std::string_view input, Mode mode)
	{
		const auto theirs = m_candidate.Decode(input, mode);
		const auto ours = m_production.Decode(input, mode);
		++m_compared;
		if (!theirs.ok || !ours.contained || theirs.units != ours.units) {
			m_mismatches.Report(std::string(Name(mode)) + " decode " + Clip(FormatBytes(input)) + " -> candidate "
				+ Clip(FormatUnits(theirs.units)) + (theirs.ok ? "" : " (call failed)") + ", production "
				+ Clip(FormatUnits(ours.units)));
		}
		return theirs.units;
	}

	Encoded Encode(std::wstring_view input, Mode mode)
	{
		const auto theirs = m_candidate.Encode(input, mode);
		const auto ours = m_production.Encode(input, mode);
		++m_compared;
		if (!theirs.ok || !ours.contained || theirs.lossy != ours.lossy || theirs.bytes != ours.bytes) {
			m_mismatches.Report(std::string(Name(mode)) + " encode " + Clip(FormatUnits(input)) + " -> candidate "
				+ Clip(FormatBytes(theirs.bytes)) + (theirs.lossy ? " (lossy)" : "") + (theirs.ok ? "" : " (call failed)")
				+ ", production " + Clip(FormatBytes(ours.bytes)) + (ours.lossy ? " (lossy)" : ""));
		}
		return { theirs.bytes, theirs.lossy };
	}

	void Fail(const std::string& description)
	{
		m_mismatches.Report(description);
	}

	std::size_t Compared() const
	{
		return m_compared;
	}

	std::size_t Mismatched() const
	{
		return m_mismatches.Count();
	}

private:
	RawCandidate m_candidate;
	RawLoops m_production;
	Mismatches m_mismatches;
	std::size_t m_compared = 0;
};

// Every sequence of up to `maxLength` symbols over `alphabet`, shortest first: the alphabets and lengths of the Rust
// short-sequence sweeps, so that both languages cover the same inputs.
template <class Char, std::size_t N, class Visit>
std::size_t ForEachSequence(const std::array<unsigned, N>& alphabet, std::size_t maxLength, Visit&& visit)
{
	std::size_t visited = 0;
	std::basic_string<Char> sequence;
	for (std::size_t length = 0; length <= maxLength; ++length) {
		std::size_t total = 1;
		for (std::size_t i = 0; i < length; ++i) {
			total *= N;
		}
		for (std::size_t code = 0; code < total; ++code) {
			sequence.clear();
			std::size_t rest = code;
			for (std::size_t i = 0; i < length; ++i) {
				sequence.push_back(static_cast<Char>(alphabet[rest % N]));
				rest /= N;
			}
			visit(std::basic_string_view<Char>(sequence));
			++visited;
		}
	}
	return visited;
}

// The one-to-four byte form of a scalar, without any validity check, so that a sweep can also build forms that a
// decoder must refuse (a surrogate, a value above U+10FFFF).
std::string FormOf(unsigned scalar)
{
	if (scalar < 0x80) {
		return { static_cast<char>(scalar) };
	}
	if (scalar < 0x800) {
		return { static_cast<char>(0xC0 | (scalar >> 6)), static_cast<char>(0x80 | (scalar & 0x3F)) };
	}
	if (scalar < 0x10000) {
		return { static_cast<char>(0xE0 | (scalar >> 12)), static_cast<char>(0x80 | ((scalar >> 6) & 0x3F)),
			static_cast<char>(0x80 | (scalar & 0x3F)) };
	}
	return { static_cast<char>(0xF0 | (scalar >> 18)), static_cast<char>(0x80 | ((scalar >> 12) & 0x3F)),
		static_cast<char>(0x80 | ((scalar >> 6) & 0x3F)), static_cast<char>(0x80 | (scalar & 0x3F)) };
}

// A deterministic generator, so that a failing buffer can be reproduced from the seed alone.
class Lcg {
public:
	explicit Lcg(std::uint64_t seed) noexcept
		: m_state(seed)
	{
	}

	std::uint32_t Below(std::uint32_t bound) noexcept
	{
		m_state = m_state * 6364136223846793005ull + 1442695040888963407ull;
		return static_cast<std::uint32_t>(m_state >> 33) % bound;
	}

private:
	std::uint64_t m_state;
};

// One random piece of bytes: an ASCII run, a valid form, a surrogate form, a six-byte pair, a truncated form or a stray
// byte.
void AppendRandomBytes(std::string& bytes, Lcg& random)
{
	switch (random.Below(8)) {
	case 0:
		for (auto run = random.Below(24); run > 0; --run) {
			bytes.push_back(static_cast<char>(random.Below(0x80)));
		}
		break;
	case 1:
		bytes += FormOf(0x80 + random.Below(0x780));
		break;
	case 2:
		bytes += FormOf(random.Below(2) == 0 ? 0x800 + random.Below(0xD000) : 0xE000 + random.Below(0x2000));
		break;
	case 3:
		bytes += FormOf(0x10000 + random.Below(0x100000));
		break;
	case 4:
		bytes += FormOf(0xD800 + random.Below(0x800));
		break;
	case 5:
		bytes += FormOf(0xD800 + random.Below(0x400));
		bytes += FormOf(0xDC00 + random.Below(0x400));
		break;
	case 6: {
		const std::string form = FormOf(0x80 + random.Below(0x10FF80));
		bytes += form.substr(0, 1 + random.Below(static_cast<std::uint32_t>(form.size())));
		break;
	}
	default:
		bytes.push_back(static_cast<char>(random.Below(0x100)));
		break;
	}
}

// One random piece of units: an ASCII run, a unit of either BMP range, a valid pair, a lone high or low surrogate, a
// binary unit or any unit.
void AppendRandomUnits(std::wstring& units, Lcg& random)
{
	switch (random.Below(8)) {
	case 0:
		for (auto run = random.Below(24); run > 0; --run) {
			units.push_back(static_cast<wchar_t>(random.Below(0x80)));
		}
		break;
	case 1:
		units.push_back(static_cast<wchar_t>(0x80 + random.Below(0xD780)));
		break;
	case 2:
		units.push_back(static_cast<wchar_t>(0xE000 + random.Below(0x2000)));
		break;
	case 3:
		units.push_back(static_cast<wchar_t>(0xD800 + random.Below(0x400)));
		units.push_back(static_cast<wchar_t>(0xDC00 + random.Below(0x400)));
		break;
	case 4:
		units.push_back(static_cast<wchar_t>(0xD800 + random.Below(0x400)));
		break;
	case 5:
		units.push_back(static_cast<wchar_t>(0xDC00 + random.Below(0x400)));
		break;
	case 6:
		units.push_back(static_cast<wchar_t>(0xDC80 + random.Below(0x80)));
		break;
	default:
		units.push_back(static_cast<wchar_t>(random.Below(0x10000)));
		break;
	}
}

// ---- Golden summaries -----------------------------------------------------------------------------------------------

const codec_golden_corpus::Summary* RecordedSummary(
	std::string_view codec, std::string_view direction, std::string_view group)
{
	for (const auto& row : codec_golden_corpus::kExpectedRows) {
		if (row.codec == codec && row.direction == direction && row.group == group) {
			return &row.summary;
		}
	}
	return nullptr;
}

void ExpectSummary(const char* who, const codec_golden_corpus::Summary& actual, const codec_golden_corpus::Summary& expected)
{
	SCOPED_TRACE(who);
	EXPECT_EQ(expected.complete, actual.complete);
	EXPECT_EQ(expected.losesome, actual.losesome);
	EXPECT_EQ(expected.failure, actual.failure);
	EXPECT_EQ(expected.totalLength, actual.totalLength);
	EXPECT_EQ(expected.digest, actual.digest);
}

struct SummaryPair {
	const codec_golden_corpus::Summary candidate;
	const codec_golden_corpus::Summary production;
};

template <class Inputs>
SummaryPair DecodeSummaries(Mode mode, const Inputs& inputs)
{
	RawCandidate candidate;
	RawLoops production;
	codec_golden_corpus::SummaryBuilder fromCandidate;
	codec_golden_corpus::SummaryBuilder fromProduction;
	for (const auto& input : inputs) {
		const std::string_view bytes(input.data(), input.size());
		const auto theirs = candidate.Decode(bytes, mode);
		EXPECT_TRUE(theirs.ok);
		fromCandidate.Add(RESULT_COMPLETE, theirs.units);
		fromProduction.Add(RESULT_COMPLETE, production.Decode(bytes, mode).units);
	}
	return { fromCandidate.Build(), fromProduction.Build() };
}

template <class Inputs>
SummaryPair EncodeSummaries(Mode mode, const Inputs& inputs)
{
	RawCandidate candidate;
	RawLoops production;
	codec_golden_corpus::SummaryBuilder fromCandidate;
	codec_golden_corpus::SummaryBuilder fromProduction;
	for (const auto& input : inputs) {
		const std::wstring_view units(input.data(), input.size());
		const auto theirs = candidate.Encode(units, mode);
		EXPECT_TRUE(theirs.ok);
		fromCandidate.Add(theirs.lossy ? RESULT_LOSESOME : RESULT_COMPLETE, theirs.bytes);
		const auto ours = production.Encode(units, mode);
		fromProduction.Add(ours.lossy ? RESULT_LOSESOME : RESULT_COMPLETE, ours.bytes);
	}
	return { fromCandidate.Build(), fromProduction.Build() };
}

// ---- ABI rules, run for both directions -----------------------------------------------------------------------------

struct RequestHeader {
	const std::uint32_t size;
	const std::uint32_t version;
	const std::uint32_t reserved0;
	const std::array<std::uint64_t, 2> reserved;
};

struct ResultHeader {
	const std::uint32_t size;
	const std::uint32_t version;
	const std::uint32_t reserved0;
	const std::uint64_t reserved;
};

struct BufferHeader {
	const std::uint32_t size;
	const std::uint32_t version;
	const std::array<std::uint64_t, 2> reserved;
};

template <class Side>
void MalformedHeaders()
{
	SCOPED_TRACE(Side::kName);
	const auto input = Side::SampleInput();
	const auto valid = RequestOver<Side>(input.data(), input.size());
	ExpectSuccess<Side>(valid, Side::SampleOutput(), false);

	// Each row changes one header field of a 48-byte request: size, version, reserved0, reserved.
	const std::initializer_list<RequestHeader> requestHeaders{
		{ 47, 1, 0, { 0, 0 } }, { 49, 1, 0, { 0, 0 } }, { 0, 1, 0, { 0, 0 } },
		{ 48, 0, 0, { 0, 0 } }, { 48, 2, 0, { 0, 0 } }, { 48, 0xFFFFFFFFu, 0, { 0, 0 } },
		{ 48, 1, 1, { 0, 0 } }, { 48, 1, 0xFFFFFFFFu, { 0, 0 } },
		{ 48, 1, 0, { 1, 0 } }, { 48, 1, 0, { 0, 1 } }, { 48, 1, 0, { kAll, kAll } },
	};
	std::size_t row = 0;
	for (const auto& header : requestHeaders) {
		SCOPED_TRACE(testing::Message() << "request header row " << row++);
		auto request = valid;
		request.struct_size = header.size;
		request.abi_version = header.version;
		request.reserved0 = header.reserved0;
		request.reserved[0] = header.reserved[0];
		request.reserved[1] = header.reserved[1];
		ExpectBothRejected<Side>(request, CodecStatus::InvalidArgument);
	}

	// The same for the 32-byte result: size, version, reserved0, reserved.
	const std::initializer_list<ResultHeader> resultHeaders{
		{ 31, 1, 0, 0 }, { 33, 1, 0, 0 }, { 0, 1, 0, 0 }, { 32, 0, 0, 0 }, { 32, 2, 0, 0 }, { 32, 0xFFFFFFFFu, 0, 0 },
		{ 32, 1, 1, 0 }, { 32, 1, 0xFFFFFFFFu, 0 }, { 32, 1, 0, 1 }, { 32, 1, 0, kAll },
	};
	row = 0;
	for (const auto& header : resultHeaders) {
		SCOPED_TRACE(testing::Message() << "result header row " << row++);
		auto bad = SentinelResult();
		bad.struct_size = header.size;
		bad.abi_version = header.version;
		bad.reserved0 = header.reserved0;
		bad.reserved[0] = header.reserved;
		ExpectMeasureFailure<Side>(valid, bad, CodecStatus::InvalidArgument);
		ExpectWriteFailure<Side>(valid, 4, [&bad](auto&, ResultV1& result) { result = bad; }, CodecStatus::InvalidArgument);
	}

	// And for the 40-byte buffer: size, version, reserved.
	const std::initializer_list<BufferHeader> bufferHeaders{
		{ 39, 1, { 0, 0 } }, { 41, 1, { 0, 0 } }, { 0, 1, { 0, 0 } }, { 40, 0, { 0, 0 } }, { 40, 2, { 0, 0 } },
		{ 40, 0xFFFFFFFFu, { 0, 0 } }, { 40, 1, { 1, 0 } }, { 40, 1, { 0, 1 } }, { 40, 1, { kAll, kAll } },
	};
	row = 0;
	for (const auto& header : bufferHeaders) {
		SCOPED_TRACE(testing::Message() << "buffer header row " << row++);
		ExpectWriteFailure<Side>(valid, 4, [&header](typename Side::Buffer& buffer, auto&) {
			buffer.struct_size = header.size;
			buffer.abi_version = header.version;
			buffer.reserved[0] = header.reserved[0];
			buffer.reserved[1] = header.reserved[1];
		}, CodecStatus::InvalidArgument);
	}
}

template <class Side>
void VariantsAndLengthBound()
{
	SCOPED_TRACE(Side::kName);
	const auto input = Side::SampleInput();
	const auto valid = RequestOver<Side>(input.data(), input.size());
	for (const std::uint32_t variant : { 0u, 1u, 3u, 5u, 6u, 8u, 0x80000004u, 0xFFFFFFFFu }) {
		SCOPED_TRACE(testing::Message() << "variant " << variant);
		auto request = valid;
		request.variant = variant;
		ExpectBothRejected<Side>(request, CodecStatus::InvalidArgument);
	}
	for (const std::uint32_t variant : { 4u, 7u }) {
		SCOPED_TRACE(testing::Message() << "variant " << variant);
		auto request = valid;
		request.variant = variant;
		ExpectSuccess<Side>(request, Side::SampleOutput(), false);
	}

	// The bound is checked before the input is read, so a phantom address is never touched.
	auto phantom = valid;
	phantom.data = reinterpret_cast<const typename Side::Input*>(std::uintptr_t{ 2 });
	for (const std::uint64_t length : { kMaxInput + 1, std::uint64_t{ 1 } << 32, std::uint64_t{ 1 } << 63, kAll }) {
		SCOPED_TRACE(testing::Message() << "length " << length);
		Side::Length(phantom) = length;
		ExpectBothRejected<Side>(phantom, CodecStatus::InvalidArgument);
	}
}

template <class Side>
void BadPointers()
{
	SCOPED_TRACE(Side::kName);
	using Output = typename Side::Output;
	const auto input = Side::SampleInput();
	const auto valid = RequestOver<Side>(input.data(), input.size());
	const auto validBefore = BytesOfObject(valid);

	// Descriptors at null, misaligned or address-space-wrapping addresses are refused before any access.
	const std::array<std::uintptr_t, 5> descriptorAddresses{ 0, 1, 4, kAddressMax - 7, ~std::uintptr_t{ 0 } - 7 };
	for (const auto address : descriptorAddresses) {
		SCOPED_TRACE(testing::Message() << "descriptor address " << address);
		const auto* requestAddress = reinterpret_cast<const typename Side::Request*>(address);
		auto* resultAddress = reinterpret_cast<ResultV1*>(address);
		const auto* bufferAddress = reinterpret_cast<const typename Side::Buffer*>(address);
		auto elements = Poisoned<Side>(4);
		const auto buffer = BufferFor<Side>(elements.data(), elements.size());
		const auto bufferBefore = BytesOfObject(buffer);
		auto result = SentinelResult();
		const auto resultBefore = BytesOfObject(result);

		EXPECT_EQ(CodecStatus::InvalidArgument, Side::Measure(requestAddress, &result));
		EXPECT_EQ(CodecStatus::InvalidArgument, Side::Measure(&valid, resultAddress));
		EXPECT_EQ(CodecStatus::InvalidArgument, Side::Write(requestAddress, &buffer, &result));
		EXPECT_EQ(CodecStatus::InvalidArgument, Side::Write(&valid, bufferAddress, &result));
		EXPECT_EQ(CodecStatus::InvalidArgument, Side::Write(&valid, &buffer, resultAddress));
		EXPECT_EQ(resultBefore, BytesOfObject(result));
		EXPECT_EQ(bufferBefore, BytesOfObject(buffer));
		EXPECT_EQ(validBefore, BytesOfObject(valid));
		EXPECT_TRUE(AllPoison<Side>(elements));
	}

	// Real descriptors at odd addresses are refused as well, and the storage around them is not written.
	{
		auto elements = Poisoned<Side>(4);
		const auto buffer = BufferFor<Side>(elements.data(), elements.size());
		auto result = SentinelResult();
		Arena arena{};
		const auto* misalignedRequest = PlaceAt(arena, 1, valid);
		const auto* misalignedBuffer = PlaceAt(arena, 129, buffer);
		auto* misalignedResult = PlaceAt(arena, 257, SentinelResult());
		const Arena before = arena;
		EXPECT_EQ(CodecStatus::InvalidArgument, Side::Measure(misalignedRequest, &result));
		EXPECT_EQ(CodecStatus::InvalidArgument, Side::Measure(&valid, misalignedResult));
		EXPECT_EQ(CodecStatus::InvalidArgument, Side::Write(misalignedRequest, &buffer, &result));
		EXPECT_EQ(CodecStatus::InvalidArgument, Side::Write(&valid, misalignedBuffer, &result));
		EXPECT_EQ(CodecStatus::InvalidArgument, Side::Write(&valid, &buffer, misalignedResult));
		EXPECT_TRUE(SameArena(before, arena));
		EXPECT_TRUE(SameBytes(SentinelResult(), result));
		EXPECT_TRUE(AllPoison<Side>(elements));
	}

	// An input at null, at an odd address (two-byte units only) or whose range wraps is refused.
	std::vector<std::uintptr_t> badInputs{ 0, kAddressMax + 1 - sizeof(typename Side::Input) };
	if constexpr (sizeof(typename Side::Input) > 1) {
		badInputs.push_back(1);
	}
	for (const auto address : badInputs) {
		SCOPED_TRACE(testing::Message() << "input address " << address);
		auto request = valid;
		request.data = reinterpret_cast<const typename Side::Input*>(address);
		ExpectBothRejected<Side>(request, CodecStatus::InvalidArgument);
	}

	// The same for the array, which a write also refuses.
	std::vector<std::uintptr_t> badArrays{ 0, kAddressMax + 1 - sizeof(Output) };
	if constexpr (sizeof(Output) > 1) {
		badArrays.push_back(1);
	}
	for (const auto address : badArrays) {
		SCOPED_TRACE(testing::Message() << "array address " << address);
		ExpectWriteFailure<Side>(valid, 4, [address](typename Side::Buffer& buffer, auto&) {
			Side::Array(buffer) = reinterpret_cast<Output*>(address);
		}, CodecStatus::InvalidArgument);
	}
}

template <class Side>
void CapacityRules()
{
	SCOPED_TRACE(Side::kName);
	using Output = typename Side::Output;

	// An empty output ignores the address of the array.
	const auto emptyRequest = RequestOver<Side>(nullptr, 0);
	for (Output* const unused : { static_cast<Output*>(nullptr), reinterpret_cast<Output*>(~std::uintptr_t{ 0 }) }) {
		const auto buffer = BufferFor<Side>(unused, 0);
		const auto bufferBefore = BytesOfObject(buffer);
		auto result = SentinelResult();
		ASSERT_EQ(CodecStatus::Ok, CallWrite<Side>(emptyRequest, buffer, result));
		ExpectResult(result, 0, false);
		EXPECT_EQ(bufferBefore, BytesOfObject(buffer));
	}

	const auto input = Side::SampleInput();
	const auto sample = Side::SampleOutput();
	const auto request = RequestOver<Side>(input.data(), input.size());
	for (std::size_t capacity = 0; capacity < sample.size(); ++capacity) {
		SCOPED_TRACE(testing::Message() << "capacity " << capacity);
		ExpectWriteFailure<Side>(request, capacity, [](auto&, auto&) {}, CodecStatus::InvalidCapacity);
	}
	ExpectWriteFailure<Side>(request, 0, [](typename Side::Buffer& buffer, auto&) {
		Side::Array(buffer) = nullptr;
	}, CodecStatus::InvalidCapacity);
	ExpectSuccess<Side>(request, sample, false);

	// The byte count overflows (2^64-1), exceeds the address range, or wraps around to a small value ((2^63 + 1) two-byte
	// units make 2 bytes): the capacity is decided before the array address is read.
	const std::uint64_t outputSize = sizeof(Output);
	const std::uint64_t largest = static_cast<std::uint64_t>(std::numeric_limits<std::ptrdiff_t>::max());
	for (const std::uint64_t capacity : { kAll, largest / outputSize + 1, (std::uint64_t{ 1 } << 63) + 1 }) {
		SCOPED_TRACE(testing::Message() << "capacity " << capacity);
		ExpectWriteFailure<Side>(request, 2, [capacity](typename Side::Buffer& buffer, auto&) {
			Side::Capacity(buffer) = capacity;
			Side::Array(buffer) = nullptr;
		}, CodecStatus::InvalidCapacity);
	}
	// A byte count that fits the address range but cannot start at a real address is a range failure.
	ExpectWriteFailure<Side>(request, 4, [largest, outputSize](typename Side::Buffer& buffer, auto&) {
		Side::Capacity(buffer) = largest / outputSize;
	}, CodecStatus::InvalidArgument);
}

template <class Side>
void FirstInvalidCheckDecides()
{
	SCOPED_TRACE(Side::kName);
	using Output = typename Side::Output;
	const auto input = Side::SampleInput();
	const auto valid = RequestOver<Side>(input.data(), input.size());
	const auto overflow = [](typename Side::Buffer& buffer) {
		Side::Capacity(buffer) = kAll;
		Side::Array(buffer) = nullptr;
	};

	// The result header comes first.
	ExpectWriteFailure<Side>(valid, 1, [&overflow](typename Side::Buffer& buffer, ResultV1& result) {
		overflow(buffer);
		result.abi_version = 0;
	}, CodecStatus::InvalidArgument);

	// Then the request header, variant and length, before the buffer is examined.
	auto badVariant = valid;
	badVariant.variant = 5;
	auto badLength = valid;
	Side::Length(badLength) = kAll;
	auto badReserved = valid;
	badReserved.reserved0 = 1;
	for (const auto* bad : { &badVariant, &badLength, &badReserved }) {
		ExpectWriteFailure<Side>(*bad, 1, [&overflow](typename Side::Buffer& buffer, auto&) { overflow(buffer); },
			CodecStatus::InvalidArgument);
	}

	// The buffer header and the capacity arithmetic come before the input and array ranges.
	ExpectWriteFailure<Side>(valid, 1, [&overflow](typename Side::Buffer& buffer, auto&) {
		overflow(buffer);
		buffer.abi_version = 0;
	}, CodecStatus::InvalidArgument);
	auto nullInput = valid;
	nullInput.data = nullptr;
	ExpectWriteFailure<Side>(nullInput, 1, [&overflow](typename Side::Buffer& buffer, auto&) { overflow(buffer); },
		CodecStatus::InvalidCapacity);

	// The ranges and overlaps come before the capacity refusal, and an empty array ignores its address.
	ExpectWriteFailure<Side>(nullInput, 0, [](auto&, auto&) {}, CodecStatus::InvalidArgument);
	ExpectWriteFailure<Side>(valid, 0, [](typename Side::Buffer& buffer, auto&) {
		Side::Array(buffer) = reinterpret_cast<Output*>(std::uintptr_t{ 1 });
	}, CodecStatus::InvalidCapacity);
}

template <class Side>
void ZeroLengthInputIgnoresItsAddress()
{
	SCOPED_TRACE(Side::kName);
	for (const std::uint32_t variant : { 4u, 7u }) {
		for (const std::uintptr_t address : { std::uintptr_t{ 0 }, std::uintptr_t{ 1 }, std::uintptr_t{ 3 }, ~std::uintptr_t{ 0 } }) {
			SCOPED_TRACE(testing::Message() << "variant " << variant << " address " << address);
			auto request = RequestOver<Side>(nullptr, 0, variant);
			request.data = reinterpret_cast<const typename Side::Input*>(address);
			ExpectSuccess<Side>(request, std::vector<typename Side::Output>{}, false);
		}
	}
}

// ---- Overlaps -------------------------------------------------------------------------------------------------------

constexpr std::size_t kArenaSize = 1280;
constexpr unsigned char kArenaFill = 0xA5;

// Byte offsets inside one arena of everything a call touches. Every offset is a multiple of 8, so every placement is
// aligned for every element type. A placement is a value: each With member returns a copy that moves one object.
struct Placement {
	const std::size_t request;
	const std::size_t buffer;
	const std::size_t output;
	const std::size_t input;
	const std::size_t inputElements;
	const std::size_t array;
	const std::size_t arrayElements;

	constexpr Placement WithRequest(std::size_t at) const noexcept
	{
		return { at, buffer, output, input, inputElements, array, arrayElements };
	}

	constexpr Placement WithBuffer(std::size_t at) const noexcept
	{
		return { request, at, output, input, inputElements, array, arrayElements };
	}

	constexpr Placement WithOutput(std::size_t at) const noexcept
	{
		return { request, buffer, at, input, inputElements, array, arrayElements };
	}

	constexpr Placement WithInput(std::size_t at) const noexcept
	{
		return { request, buffer, output, at, inputElements, array, arrayElements };
	}

	constexpr Placement WithArray(std::size_t at) const noexcept
	{
		return { request, buffer, output, input, inputElements, at, arrayElements };
	}
};

// Nothing touches anything else. The array holds 48 elements, the most that 16 input elements can convert to (three bytes
// per UTF-16 unit when encoding).
constexpr Placement kApart{ 0, 256, 512, 768, 16, 1024, 48 };

struct Placed {
	const CodecStatus status;
	const std::vector<unsigned char> before;
	const std::vector<unsigned char> after;
};

// Runs one call over objects placed in a fresh arena whose input holds the letters A, B, ... repeating and whose other
// bytes are kArenaFill. Returns the status and the arena before and after the call.
template <class Side, class Tweak>
Placed RunPlaced(const Placement& placement, bool write, Tweak&& tweak)
{
	using Input = typename Side::Input;
	using Output = typename Side::Output;
	alignas(16) std::array<unsigned char, kArenaSize> arena;
	arena.fill(kArenaFill);
	unsigned char* const base = arena.data();
	for (std::size_t index = 0; index < placement.inputElements; ++index) {
		const Input letter = static_cast<Input>('A' + index % 26);
		std::memcpy(base + placement.input + index * sizeof(Input), &letter, sizeof(letter));
	}
	auto request = RequestOver<Side>(base + placement.input, placement.inputElements);
	auto buffer = BufferFor<Side>(reinterpret_cast<Output*>(base + placement.array), placement.arrayElements);
	tweak(request, buffer);

	// A descriptor written later wins where two descriptors share bytes.
	const auto result = SentinelResult();
	std::memcpy(base + placement.output, &result, sizeof(result));
	std::memcpy(base + placement.buffer, &buffer, sizeof(buffer));
	std::memcpy(base + placement.request, &request, sizeof(request));

	std::vector<unsigned char> before(base, base + kArenaSize);
	const auto* requestAt = reinterpret_cast<const typename Side::Request*>(base + placement.request);
	const auto* bufferAt = reinterpret_cast<const typename Side::Buffer*>(base + placement.buffer);
	auto* outputAt = reinterpret_cast<ResultV1*>(base + placement.output);
	const CodecStatus status = write ? Side::Write(requestAt, bufferAt, outputAt) : Side::Measure(requestAt, outputAt);
	return { status, std::move(before), std::vector<unsigned char>(base, base + kArenaSize) };
}

// A placement is refused with every arena byte unchanged, or accepted with exactly the result and the output that the
// production conversion gives for the input as the arena held it, and nothing else changed.
template <class Side, class Tweak>
void CheckPlaced(const Placement& placement, bool write, bool refused, Tweak&& tweak)
{
	SCOPED_TRACE(testing::Message() << (write ? "write" : "measure") << " request " << placement.request << " buffer "
		<< placement.buffer << " output " << placement.output << " input " << placement.input << " array " << placement.array);
	using Output = typename Side::Output;
	const auto placed = RunPlaced<Side>(placement, write, std::forward<Tweak>(tweak));
	if (refused) {
		EXPECT_EQ(CodecStatus::InvalidArgument, placed.status);
		EXPECT_TRUE(placed.before == placed.after) << "a refused call changed the arena";
		return;
	}
	ASSERT_EQ(CodecStatus::Ok, placed.status);

	RawLoops loops;
	std::vector<Output> expected;
	bool lossy = false;
	Side::Reference(loops, placed.before.data() + placement.input,
		placement.inputElements * sizeof(typename Side::Input), expected, lossy);
	auto wanted = placed.before;
	ResultV1 result{};
	result.struct_size = sizeof(result);
	result.abi_version = SAKURA_DOCUMENT_CODEC_ABI_VERSION_V1;
	result.output_length = expected.size();
	result.lossy = lossy ? 1u : 0u;
	std::memcpy(wanted.data() + placement.output, &result, sizeof(result));
	if (write && !expected.empty()) {
		std::memcpy(wanted.data() + placement.array, expected.data(), expected.size() * sizeof(Output));
	}
	EXPECT_TRUE(wanted == placed.after) << "the call changed a byte other than the result and the output";
}

template <class Side>
void OverlapRules()
{
	SCOPED_TRACE(Side::kName);
	constexpr std::size_t region = kApart.inputElements * sizeof(typename Side::Input);
	constexpr std::size_t array = kApart.arrayElements * sizeof(typename Side::Output);
	constexpr std::size_t kResultSize = sizeof(ResultV1);
	const auto none = [](auto&, auto&) {};
	const auto meets = [](std::size_t a, std::size_t aLength, std::size_t b, std::size_t bLength) {
		return (std::max)(a, b) < (std::min)(a + aLength, b + bLength);
	};
	// The offsets, in steps of 8, at which an object of `length` bytes abuts, overlaps or covers the fixed range.
	const auto around = [](std::size_t fixed, std::size_t fixedLength, std::size_t length) {
		std::vector<std::size_t> at{ fixed + fixedLength, fixed + fixedLength - 8, fixed, fixed + 8 };
		if (fixed >= length) {
			at.push_back(fixed - length);
		}
		if (fixed + 8 >= length) {
			at.push_back(fixed + 8 - length);
		}
		std::ranges::sort(at);
		at.erase(std::unique(at.begin(), at.end()), at.end());
		return at;
	};

	for (const bool write : { false, true }) {
		// The result slot is written, so it may share bytes with nothing that is read.
		for (const std::size_t at : around(kApart.input, region, kResultSize)) {
			CheckPlaced<Side>(kApart.WithOutput(at), write, meets(at, kResultSize, kApart.input, region), none);
		}
		// The input is read only, so it may share bytes with the descriptors that are read.
		for (const std::size_t input : { kApart.request, kApart.buffer - 8 }) {
			CheckPlaced<Side>(kApart.WithInput(input), write, false, none);
		}
	}

	// A request placed over the ignored fields of the result stays valid with no input, so the overlap itself is what is
	// refused.
	const auto noInput = [](auto& request) {
		request.data = nullptr;
		Side::Length(request) = 0;
	};
	const Placement overTheResult = kApart.WithOutput(0).WithRequest(8);
	CheckPlaced<Side>(overTheResult, false, true, [&noInput](auto& request, auto&) { noInput(request); });
	CheckPlaced<Side>(overTheResult, true, true, [&noInput](auto& request, auto&) { noInput(request); });
	// The same for a buffer descriptor with no array.
	const Placement bufferOverTheResult = kApart.WithOutput(0).WithBuffer(8).WithRequest(64);
	CheckPlaced<Side>(bufferOverTheResult, true, true, [&noInput](auto& request, auto& buffer) {
		noInput(request);
		Side::Array(buffer) = nullptr;
		Side::Capacity(buffer) = 0;
	});

	// The array is written, so it may share bytes with nothing else.
	for (const std::size_t at : around(kApart.array, array, kResultSize)) {
		CheckPlaced<Side>(kApart.WithOutput(at), true, meets(at, kResultSize, kApart.array, array), none);
	}
	const std::array<std::pair<std::size_t, std::size_t>, 3> fixedRanges{ {
		{ kApart.input, region }, { kApart.request, sizeof(typename Side::Request) },
		{ kApart.buffer, sizeof(typename Side::Buffer) } } };
	for (const auto& [fixed, fixedLength] : fixedRanges) {
		for (const std::size_t at : around(fixed, fixedLength, array)) {
			CheckPlaced<Side>(kApart.WithArray(at), true, meets(at, array, fixed, fixedLength), none);
		}
	}

	// An overlap is refused before a short capacity is, so the status stays InvalidArgument.
	CheckPlaced<Side>(kApart.WithOutput(kApart.array), true, true, [](auto&, auto& buffer) { Side::Capacity(buffer) = 1; });
}

// ---- Guard pages ----------------------------------------------------------------------------------------------------

// Converts one input that ends exactly at a guard page into an array that ends exactly at another. A read or a write one
// element beyond either faults. The array is then offered one element short, which must change nothing.
template <class Side>
void ConvertAtThePageEnds(
	GuardedRegion& inputRegion,
	GuardedRegion& arrayRegion,
	std::string_view inputBytes,
	std::size_t inputElements,
	Mode mode,
	const std::vector<typename Side::Output>& expected,
	bool lossy)
{
	using Output = typename Side::Output;
	const char* const input = inputRegion.PlaceAtEnd(inputBytes);
	const auto request = RequestOver<Side>(input, inputElements, VariantOf(mode));

	auto measured = SentinelResult();
	ASSERT_EQ(CodecStatus::Ok, CallMeasure<Side>(request, measured));
	ExpectResult(measured, expected.size(), lossy);

	const std::string filler(expected.size() * sizeof(Output), static_cast<char>(0xA5));
	Output* const array = reinterpret_cast<Output*>(const_cast<char*>(arrayRegion.PlaceAtEnd(filler)));
	const auto buffer = BufferFor<Side>(array, expected.size());
	auto written = SentinelResult();
	ASSERT_EQ(CodecStatus::Ok, CallWrite<Side>(request, buffer, written));
	EXPECT_EQ(BytesOfObject(measured), BytesOfObject(written));
	if (!expected.empty()) {
		EXPECT_EQ(0, std::memcmp(array, expected.data(), filler.size()));

		const std::string shorter(filler.size() - sizeof(Output), static_cast<char>(0xA5));
		Output* const shortArray = reinterpret_cast<Output*>(const_cast<char*>(arrayRegion.PlaceAtEnd(shorter)));
		const auto shortBuffer = BufferFor<Side>(shortArray, expected.size() - 1);
		auto refused = SentinelResult();
		EXPECT_EQ(CodecStatus::InvalidCapacity, CallWrite<Side>(request, shortBuffer, refused));
		EXPECT_TRUE(SameBytes(SentinelResult(), refused));
		EXPECT_TRUE(std::all_of(reinterpret_cast<const unsigned char*>(shortArray),
			reinterpret_cast<const unsigned char*>(shortArray) + shorter.size(),
			[](unsigned char byte) { return byte == 0xA5; }));
	}
}

// ---- Actual callers -------------------------------------------------------------------------------------------------

// CFileLoad reads the extended-EOL policy from the share data. The suite does not own any, so a test that drives CFileLoad
// creates it for its own duration, forces the policy off (the replays split lines at CR, LF and CR LF only) and restores
// whatever it found.
class ShareDataScope {
public:
	ShareDataScope()
		: m_ownsShareData(env::ShareDataTestSuite::pcShareData == nullptr)
	{
		if (m_ownsShareData) {
			env::ShareDataTestSuite::SetUpShareData();
		}
		if (env::ShareDataTestSuite::pcShareData && env::ShareDataTestSuite::pcShareData->GetDllShareDataPtr()) {
			m_policy = &env::ShareDataTestSuite::pcShareData->GetDllShareDataPtr()->m_Common.m_sEdit.m_bEnableExtEol;
			m_originalPolicy = *m_policy;
			*m_policy = false;
		}
	}

	~ShareDataScope()
	{
		if (m_policy != nullptr) {
			*m_policy = m_originalPolicy;
		}
		if (m_ownsShareData) {
			env::ShareDataTestSuite::TearDownShareData();
		}
	}

	ShareDataScope(const ShareDataScope&) = delete;
	ShareDataScope& operator=(const ShareDataScope&) = delete;

	bool IsReady() const
	{
		return m_policy != nullptr;
	}

private:
	bool m_ownsShareData;
	bool* m_policy = nullptr;
	bool m_originalPolicy = false;
};

class TempFile {
public:
	explicit TempFile(std::string_view bytes)
	{
		wchar_t directory[MAX_PATH]{};
		wchar_t name[MAX_PATH]{};
		if (::GetTempPathW(MAX_PATH, directory) == 0 || ::GetTempFileNameW(directory, L"u8c", 0, name) == 0) {
			return;
		}
		m_path = name;
		std::ofstream stream(m_path, std::ios::binary | std::ios::trunc);
		stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
		stream.close();
		m_ready = !stream.fail();
	}

	~TempFile()
	{
		if (!m_path.empty()) {
			::DeleteFileW(m_path.c_str());
		}
	}

	TempFile(const TempFile&) = delete;
	TempFile& operator=(const TempFile&) = delete;

	bool IsReady() const
	{
		return m_ready;
	}

	const std::filesystem::path& Path() const
	{
		return m_path;
	}

private:
	std::filesystem::path m_path;
	bool m_ready = false;
};

struct LineSegment {
	const std::string_view bytes;
	const EEolType eol;
};

// The lines of a document as CFileLoad splits them without the extended EOLs: each segment keeps its end-of-line bytes,
// and CR LF is one end of line. This is written independently of CFileLoad, which the replays compare it with.
std::vector<LineSegment> SplitLines(std::string_view document)
{
	std::vector<LineSegment> lines;
	std::size_t begin = 0;
	for (std::size_t i = 0; i < document.size(); ++i) {
		if (document[i] == '\n') {
			lines.push_back({ document.substr(begin, i + 1 - begin), EEolType::line_feed });
			begin = i + 1;
		} else if (document[i] == '\r') {
			const bool crlf = i + 1 < document.size() && document[i + 1] == '\n';
			const std::size_t end = i + (crlf ? 2 : 1);
			lines.push_back({ document.substr(begin, end - begin), crlf ? EEolType::cr_and_lf : EEolType::carriage_return });
			begin = end;
			i = end - 1;
		}
	}
	if (begin < document.size()) {
		lines.push_back({ document.substr(begin), EEolType::none });
	}
	return lines;
}

// A document of the corpus rows that apply to the mode, one per line with the three end-of-line forms in turn and the
// last line unterminated, followed by every byte value as its own line, alone and in front of an ASCII byte. The first
// line is ASCII so that no row can be read as a byte order mark.
std::string BuildDocument(const std::vector<DecodeCase>& rows, Mode mode)
{
	static constexpr std::array<std::string_view, 3> kEols{ "\r\n", "\n", "\r" };
	std::string document = "head";
	document += kEols[0];
	std::size_t index = 0;
	for (const auto& row : rows) {
		if (Applies(row.variants, mode)) {
			document += row.input;
			document += kEols[++index % kEols.size()];
		}
	}
	for (unsigned byte = 0; byte < 0x100; ++byte) {
		document.push_back(static_cast<char>(byte));
		document += kEols[++index % kEols.size()];
		document.push_back(static_cast<char>(byte));
		document.push_back('z');
		document += kEols[++index % kEols.size()];
	}
	document += "tail";
	return document;
}

struct LoadedLine {
	const std::wstring units;
	const EEolType eol;
};

// Reads a document through CFileLoad::ReadLine, the caller of the decode in production, with the code type of the mode.
std::vector<LoadedLine> LoadThroughFileLoad(const std::string& document, Mode mode, bool& ok)
{
	std::vector<LoadedLine> lines;
	ok = false;
	const TempFile file(document);
	if (!file.IsReady()) {
		return lines;
	}
	CFileLoad loader;
	if (loader.FileOpen(file.Path().c_str(), false, CodeOf(mode), 0) != CodeOf(mode)) {
		return lines;
	}
	CNativeW line;
	CEol eol;
	for (;;) {
		const EConvertResult result = loader.ReadLine(&line, &eol);
		if (result == RESULT_FAILURE) {
			break;
		}
		if (result != RESULT_COMPLETE) {
			return lines;
		}
		lines.push_back({ std::wstring(UnitsOf(line)), eol.GetType() });
	}
	loader.FileClose();
	ok = true;
	return lines;
}

#endif

} // namespace

#if defined(SAKURA_UTF16_RUST_CANDIDATE)

TEST(Utf8CodecRustCandidate, CorpusRowsMatchTheFrozenRowsAndProductionAtEveryPlacement)
{
	const auto corpus = LoadCorpus();
	RawLoops production;
	GuardedRegion region;
	const size_t decoded = ForEachApplicable(corpus.decode, [&](const DecodeCase& testCase, Mode mode) {
		const auto reference = production.Decode(testCase.input, mode);
		ASSERT_TRUE(reference.contained);
		// The bytes end at a guard page once and start at an odd address once, so a read past them faults and an
		// alignment assumption on the input is caught. An empty row puts its pointer on the guard page itself.
		for (const bool oddAddress : { false, true }) {
			SCOPED_TRACE(oddAddress ? "odd-address" : "page-end");
			const char* data = oddAddress ? region.PlaceAtOddAddress(testCase.input) : region.PlaceAtEnd(testCase.input);
			const auto result = rust_candidate::DecodeUtf8(CodeOf(mode), data, testCase.input.size());
			ASSERT_TRUE(result);
			EXPECT_EQ(FormatUnits(testCase.expected), FormatUnits(*result.units));
			EXPECT_EQ(FormatUnits(reference.units), FormatUnits(*result.units));
		}
	});
	const size_t encoded = ForEachApplicable(corpus.encode, [&](const EncodeCase& testCase, Mode mode) {
		const auto reference = production.Encode(testCase.input, mode);
		ASSERT_TRUE(reference.contained);
		const std::string_view unitBytes(
			reinterpret_cast<const char*>(testCase.input.data()), testCase.input.size() * sizeof(wchar_t));
		const auto* units = reinterpret_cast<const wchar_t*>(region.PlaceAtEnd(unitBytes));
		const auto result = rust_candidate::EncodeUtf8(CodeOf(mode), units, testCase.input.size());
		ASSERT_TRUE(result);
		EXPECT_EQ(testCase.lossy, result.lossy);
		EXPECT_EQ(FormatBytes(testCase.expected), FormatBytes(*result.bytes));
		EXPECT_EQ(reference.lossy, result.lossy);
		EXPECT_EQ(FormatBytes(reference.bytes), FormatBytes(*result.bytes));
	});
	EXPECT_EQ(ApplicablePairs(corpus.decode), decoded);
	EXPECT_EQ(ApplicablePairs(corpus.encode), encoded);
}

TEST(Utf8CodecRustCandidate, GoldenSummariesMatchProductionAndTheRecordedRows)
{
	size_t verified = 0;
	for (const Mode mode : kModes) {
		const std::string_view codec = IsCesu8(mode) ? "CESU8" : "UTF8";
		SCOPED_TRACE(codec);
		const auto check = [&](std::string_view direction, std::string_view group, const SummaryPair& pair) {
			SCOPED_TRACE(std::string(direction) + "/" + std::string(group));
			const auto* recorded = RecordedSummary(codec, direction, group);
			ASSERT_NE(nullptr, recorded) << "the row is not recorded";
			ExpectSummary("candidate", pair.candidate, *recorded);
			ExpectSummary("production", pair.production, *recorded);
			++verified;
		};
		check("decode", "SingleBytes", DecodeSummaries(mode, codec_golden_corpus::SingleBytes()));
		check("decode", "Utf8Forms", DecodeSummaries(mode, codec_golden_corpus::kUtf8Forms));
		check("encode", "BmpUnits", EncodeSummaries(mode, codec_golden_corpus::BmpUnits()));
		check("encode", "SurrogatePairs", EncodeSummaries(mode, codec_golden_corpus::kSurrogatePairs));
		check("encode", "TextSamples", EncodeSummaries(mode, codec_golden_corpus::kTextSamples));
	}
	EXPECT_EQ(10u, verified);
}

// A decode keeps every byte, so decoding what a candidate decoded and encoding it again gives the input back, without loss.
TEST(Utf8CodecRustCandidate, EveryByteAndBytePairMatchesProductionAndRoundTrips)
{
	Differential differential;
	Mismatches roundTrip;
	size_t examined = 0;
	std::string input;
	for (const Mode mode : kModes) {
		for (unsigned first = 0; first < 0x100; ++first) {
			// second == 0x100 stands for the single byte on its own.
			for (unsigned second = 0; second <= 0x100; ++second) {
				input.assign(1, static_cast<char>(first));
				if (second < 0x100) {
					input.push_back(static_cast<char>(second));
				}
				++examined;
				const auto units = differential.Decode(input, mode);
				const auto encoded = differential.Encode(units, mode);
				if (encoded.lossy || encoded.bytes != input) {
					roundTrip.Report(std::string(Name(mode)) + ": " + FormatBytes(input) + " -> " + FormatUnits(units)
						+ " -> " + FormatBytes(encoded.bytes));
				}
			}
		}
	}
	EXPECT_EQ(2u * 256u * 257u, examined);
	EXPECT_EQ(2 * examined, differential.Compared());
	EXPECT_EQ(0u, differential.Mismatched());
	EXPECT_EQ(0u, roundTrip.Count());
	RecordProperty("compared_conversions", static_cast<int>(differential.Compared()));
}

TEST(Utf8CodecRustCandidate, ShortSequencesAndTruncatedFormsMatchProduction)
{
	constexpr std::array<unsigned, 12> kBoundaryBytes{ 0x00, 0x41, 0x7F, 0x80, 0xBF, 0xC2, 0xDF, 0xE0, 0xED, 0xF0, 0xF4, 0xFF };
	constexpr std::array<unsigned, 6> kSurrogateFormBytes{ 0xED, 0xA0, 0xAF, 0xB0, 0xBF, 0x41 };
	constexpr std::array<unsigned, 12> kBoundaryUnits{
		0x0000, 0x0041, 0x07FF, 0x0800, 0xD7FF, 0xD800, 0xDBFF, 0xDC00, 0xDCFF, 0xDD00, 0xDFFF, 0xFFFF };
	constexpr std::array<unsigned, 5> kSurrogateUnits{ 0xD800, 0xDBFF, 0xDC00, 0xDFFF, 0x0041 };

	Differential differential;
	size_t decoded = 0;
	size_t encoded = 0;
	for (const Mode mode : kModes) {
		decoded += ForEachSequence<char>(kBoundaryBytes, 4, [&](std::string_view input) { differential.Decode(input, mode); });
		decoded += ForEachSequence<char>(kSurrogateFormBytes, 6, [&](std::string_view input) { differential.Decode(input, mode); });
		encoded += ForEachSequence<wchar_t>(kBoundaryUnits, 4, [&](std::wstring_view input) { differential.Encode(input, mode); });
		encoded += ForEachSequence<wchar_t>(kSurrogateUnits, 5, [&](std::wstring_view input) { differential.Encode(input, mode); });
	}
	EXPECT_EQ(2u * (22621u + 55987u), decoded);
	EXPECT_EQ(2u * (22621u + 3906u), encoded);

	// Every truncation of the four-byte forms of boundary scalars (including the first value above U+10FFFF and the lead
	// byte past F7) and of the six-byte forms of boundary surrogate pairs, bare and followed by a byte that continues
	// nothing or a complete two-byte form.
	std::vector<std::string> forms;
	for (const unsigned scalar : { 0x10000u, 0x10001u, 0x1D11Eu, 0x1F600u, 0x10FFFFu, 0x110000u, 0x200000u }) {
		forms.push_back(FormOf(scalar));
	}
	for (const unsigned high : { 0xD800u, 0xD801u, 0xDBFEu, 0xDBFFu }) {
		for (const unsigned low : { 0xDC00u, 0xDC01u, 0xDFFEu, 0xDFFFu }) {
			forms.push_back(FormOf(high) + FormOf(low));
		}
	}
	size_t truncated = 0;
	for (const Mode mode : kModes) {
		for (const auto& form : forms) {
			for (size_t length = 0; length <= form.size(); ++length) {
				const std::string prefix = form.substr(0, length);
				for (const std::string_view tail : { std::string_view{}, std::string_view{ "A" }, std::string_view{ "\x80" },
						std::string_view{ "\xED" }, std::string_view{ "\xC3\xA9" } }) {
					differential.Decode(prefix + std::string(tail), mode);
					++truncated;
				}
			}
		}
	}
	EXPECT_EQ(2u * (7u * 5u + 16u * 7u) * 5u, truncated);
	EXPECT_EQ(decoded + encoded + truncated, differential.Compared());
	EXPECT_EQ(0u, differential.Mismatched());
	RecordProperty("compared_conversions", static_cast<int>(differential.Compared()));
}

TEST(Utf8CodecRustCandidate, EveryBmpUnitBesideAsciiNeighboursMatchesProduction)
{
	Differential differential;
	size_t shortBuffers = 0;
	size_t longBuffers = 0;
	size_t units = 0;
	for (const Mode mode : kModes) {
		// All 65,536 units in three long buffers, each unit followed by an ASCII unit, preceded by one, and between two.
		std::wstring followed;
		std::wstring preceded;
		std::wstring between;
		for (unsigned unit = 0; unit < 0x10000; ++unit) {
			const auto wide = static_cast<wchar_t>(unit);
			followed.push_back(wide);
			followed.push_back(L'A');
			preceded.push_back(L'B');
			preceded.push_back(wide);
			between.push_back(L'C');
			between.push_back(wide);
			between.push_back(L'D');
		}
		differential.Encode(followed, mode);
		differential.Encode(preceded, mode);
		differential.Encode(between, mode);
		longBuffers += 3;

		// Short buffers, where the end of the buffer is next to the unit: every surrogate unit and a stride of the others,
		// alone and with an ASCII unit before it, after it, and on both sides. (The recorded golden rows already compare
		// every unit alone.)
		for (unsigned unit = 0; unit < 0x10000; ++unit) {
			const bool surrogate = unit >= 0xD800 && unit <= 0xDFFF;
			if (!surrogate && unit % 61 != 0 && unit != 0xFFFF) {
				continue;
			}
			const auto wide = static_cast<wchar_t>(unit);
			differential.Encode(std::wstring{ wide }, mode);
			differential.Encode(std::wstring{ L'A', wide }, mode);
			differential.Encode(std::wstring{ wide, L'A' }, mode);
			differential.Encode(std::wstring{ L'A', wide, L'A' }, mode);
			shortBuffers += 4;
			if (mode == Mode::Utf8) {
				++units;
			}
		}
	}
	EXPECT_EQ(3090u, units);
	EXPECT_EQ(2u * 4u * units, shortBuffers);
	EXPECT_EQ(shortBuffers + longBuffers, differential.Compared());
	EXPECT_EQ(0u, differential.Mismatched());
	RecordProperty("compared_conversions", static_cast<int>(differential.Compared()));
}

// A high surrogate followed by any low surrogate is a pair: UTF-8 writes the scalar as one four-byte form, CESU-8 the two
// units as two three-byte forms. Each high surrogate is packed with all 1,024 low ones into one buffer of 2,048 units, so
// the whole 1,048,576-pair grid is compared in 2,048 conversions, and every produced byte string is decoded again.
TEST(Utf8CodecRustCandidate, EveryHighAndLowSurrogateCombinationMatchesProductionAndRoundTrips)
{
	Differential differential;
	size_t buffers = 0;
	std::wstring pairs(2048, L'\0');
	for (unsigned high = 0xD800; high <= 0xDBFF; ++high) {
		for (unsigned low = 0xDC00; low <= 0xDFFF; ++low) {
			pairs[2 * (low - 0xDC00)] = static_cast<wchar_t>(high);
			pairs[2 * (low - 0xDC00) + 1] = static_cast<wchar_t>(low);
		}
		for (const Mode mode : kModes) {
			++buffers;
			const auto encoded = differential.Encode(pairs, mode);
			if (encoded.lossy || encoded.bytes.size() != 1024u * (IsCesu8(mode) ? 6u : 4u)) {
				differential.Fail(std::string(Name(mode)) + ": high " + FormatUnits(std::wstring_view(pairs).substr(0, 1))
					+ " encoded to " + std::to_string(encoded.bytes.size()) + " bytes" + (encoded.lossy ? " (lossy)" : ""));
			}
			if (differential.Decode(encoded.bytes, mode) != pairs) {
				differential.Fail(std::string(Name(mode)) + ": high " + FormatUnits(std::wstring_view(pairs).substr(0, 1))
					+ " did not round-trip");
			}
		}
	}
	EXPECT_EQ(2u * 1024u, buffers);
	EXPECT_EQ(2 * buffers, differential.Compared());
	EXPECT_EQ(0u, differential.Mismatched());
	RecordProperty("compared_conversions", static_cast<int>(differential.Compared()));
}

TEST(Utf8CodecRustCandidate, SeededRandomBuffersAndAsciiPrefixHandoffsMatchProduction)
{
	Differential differential;
	Lcg random(0x443);
	size_t expected = 0;
	std::string bytes;
	std::wstring units;
	for (const Mode mode : kModes) {
		// An ASCII prefix of every length up to 130 handing over to a mixed tail, which moves the first non-ASCII element
		// across every position of any block or lane that the converters process at once.
		for (size_t prefix = 0; prefix <= 130; ++prefix) {
			for (unsigned pieces = 1; pieces <= 6; ++pieces) {
				bytes.assign(prefix, 'a');
				units.assign(prefix, L'a');
				for (unsigned piece = 0; piece < pieces; ++piece) {
					AppendRandomBytes(bytes, random);
					AppendRandomUnits(units, random);
				}
				differential.Decode(bytes, mode);
				differential.Encode(units, mode);
				expected += 2;
			}
		}
		// Random buffers of up to 4,096 elements, with the largest length and the one before it always present.
		for (unsigned round = 0; round < 300; ++round) {
			const size_t target = round == 0 ? 4096 : round == 1 ? 4095 : 1 + random.Below(4096);
			bytes.clear();
			units.clear();
			while (bytes.size() < target) {
				AppendRandomBytes(bytes, random);
			}
			while (units.size() < target) {
				AppendRandomUnits(units, random);
			}
			bytes.resize(target);
			units.resize(target);
			differential.Decode(bytes, mode);
			differential.Encode(units, mode);
			expected += 2;
		}
	}
	EXPECT_EQ(2u * (131u * 6u + 300u) * 2u, expected);
	EXPECT_EQ(expected, differential.Compared());
	EXPECT_EQ(0u, differential.Mismatched());
	RecordProperty("compared_conversions", static_cast<int>(differential.Compared()));
}

TEST(Utf8CodecRustCandidate, AdapterRefusesUnsupportedCodesAndBadAddressesWithoutOutput)
{
	const std::string bytes = "A\xC3\xA9";
	const std::wstring units = L"A\x00E9";
	for (const ECodeType code : { CODE_SJIS, CODE_JIS, CODE_EUC, CODE_UNICODE, CODE_UTF7, CODE_UNICODEBE, CODE_LATIN1,
			CODE_UTF32LE, CODE_AUTODETECT, CODE_ERROR, static_cast<ECodeType>(0x80000004) }) {
		SCOPED_TRACE(testing::Message() << "code " << static_cast<int>(code));
		const auto decoded = rust_candidate::DecodeUtf8(code, bytes.data(), bytes.size());
		EXPECT_FALSE(decoded);
		EXPECT_EQ(CodecStatus::InvalidArgument, decoded.status);
		const auto encoded = rust_candidate::EncodeUtf8(code, units.data(), units.size());
		EXPECT_FALSE(encoded);
		EXPECT_FALSE(encoded.lossy);
		EXPECT_EQ(CodecStatus::InvalidArgument, encoded.status);
	}

	// A null pointer is valid only for an empty input, which converts to nothing without loss.
	for (const ECodeType code : { CODE_UTF8, CODE_CESU8 }) {
		SCOPED_TRACE(testing::Message() << "code " << static_cast<int>(code));
		const auto missingBytes = rust_candidate::DecodeUtf8(code, nullptr, bytes.size());
		EXPECT_FALSE(missingBytes);
		EXPECT_EQ(CodecStatus::InvalidArgument, missingBytes.status);
		const auto missingUnits = rust_candidate::EncodeUtf8(code, nullptr, units.size());
		EXPECT_FALSE(missingUnits);
		EXPECT_EQ(CodecStatus::InvalidArgument, missingUnits.status);

		const auto emptyBytes = rust_candidate::DecodeUtf8(code, nullptr, 0);
		ASSERT_TRUE(emptyBytes);
		EXPECT_TRUE(emptyBytes.units->empty());
		const auto emptyUnits = rust_candidate::EncodeUtf8(code, nullptr, 0);
		ASSERT_TRUE(emptyUnits);
		EXPECT_TRUE(emptyUnits.bytes->empty());
		EXPECT_FALSE(emptyUnits.lossy);

		// An input longer than the int length domain of the converters is refused before it is read.
		const auto tooLong = rust_candidate::DecodeUtf8(code, bytes.data(), static_cast<std::size_t>(kMaxInput) + 1);
		EXPECT_FALSE(tooLong);
		EXPECT_EQ(CodecStatus::InvalidArgument, tooLong.status);
		const auto tooManyUnits = rust_candidate::EncodeUtf8(code, units.data(), static_cast<std::size_t>(kMaxInput) + 1);
		EXPECT_FALSE(tooManyUnits);
		EXPECT_EQ(CodecStatus::InvalidArgument, tooManyUnits.status);
	}

	// UTF-16 units at an odd address are refused, where the bytes of a decode may start anywhere.
	GuardedRegion region;
	const std::string_view unitBytes(reinterpret_cast<const char*>(units.data()), units.size() * sizeof(wchar_t));
	const auto* odd = reinterpret_cast<const wchar_t*>(region.PlaceAtOddAddress(unitBytes));
	const auto misaligned = rust_candidate::EncodeUtf8(CODE_UTF8, odd, units.size());
	EXPECT_FALSE(misaligned);
	EXPECT_EQ(CodecStatus::InvalidArgument, misaligned.status);
	const auto anywhere = rust_candidate::DecodeUtf8(CODE_UTF8, region.PlaceAtOddAddress(bytes), bytes.size());
	ASSERT_TRUE(anywhere);
	EXPECT_EQ(FormatUnits(units), FormatUnits(*anywhere.units));
}

TEST(Utf8CodecRustCandidate, MalformedHeadersVariantsAndLengthsFailClosedInBothDirections)
{
	MalformedHeaders<DecodeSide>();
	MalformedHeaders<EncodeSide>();
	VariantsAndLengthBound<DecodeSide>();
	VariantsAndLengthBound<EncodeSide>();
}

TEST(Utf8CodecRustCandidate, BadPointersAreRefusedBeforeAnyAccessInBothDirections)
{
	BadPointers<DecodeSide>();
	BadPointers<EncodeSide>();
}

TEST(Utf8CodecRustCandidate, CapacityErrorsAreAtomicAndTheFirstInvalidCheckDecidesTheStatus)
{
	CapacityRules<DecodeSide>();
	CapacityRules<EncodeSide>();
	FirstInvalidCheckDecides<DecodeSide>();
	FirstInvalidCheckDecides<EncodeSide>();
	ZeroLengthInputIgnoresItsAddress<DecodeSide>();
	ZeroLengthInputIgnoresItsAddress<EncodeSide>();
}

TEST(Utf8CodecRustCandidate, WritableOverlapsFailClosedAndReadOnlyOverlapsAreAccepted)
{
	OverlapRules<DecodeSide>();
	OverlapRules<EncodeSide>();
}

TEST(Utf8CodecRustCandidate, InputsAndOutputArraysEndingAtAGuardPageAreNeverOverrun)
{
	const auto corpus = LoadCorpus();
	RawLoops production;
	GuardedRegion inputRegion;
	GuardedRegion arrayRegion;
	const size_t decoded = ForEachApplicable(corpus.decode, [&](const DecodeCase& testCase, Mode mode) {
		const auto reference = production.Decode(testCase.input, mode);
		ASSERT_TRUE(reference.contained);
		std::vector<std::uint16_t> expected;
		for (const wchar_t unit : reference.units) {
			expected.push_back(static_cast<std::uint16_t>(unit));
		}
		ConvertAtThePageEnds<DecodeSide>(inputRegion, arrayRegion, testCase.input, testCase.input.size(), mode, expected, false);
	});
	const size_t encoded = ForEachApplicable(corpus.encode, [&](const EncodeCase& testCase, Mode mode) {
		const auto reference = production.Encode(testCase.input, mode);
		ASSERT_TRUE(reference.contained);
		std::vector<std::uint8_t> expected;
		for (const char byte : reference.bytes) {
			expected.push_back(static_cast<std::uint8_t>(byte));
		}
		const std::string_view unitBytes(
			reinterpret_cast<const char*>(testCase.input.data()), testCase.input.size() * sizeof(wchar_t));
		ConvertAtThePageEnds<EncodeSide>(inputRegion, arrayRegion, unitBytes, testCase.input.size(), mode, expected, reference.lossy);
	});
	EXPECT_EQ(ApplicablePairs(corpus.decode), decoded);
	EXPECT_EQ(ApplicablePairs(corpus.encode), encoded);
}

// The decode path of a document load: CFileLoad splits a real file at CR LF, LF and CR and converts every line, end of line
// included, with the code type of the mode. The candidate decodes the same bytes of every line to the same units, and the
// line splitting that this test writes independently agrees with CFileLoad on every line and every end of line.
TEST(Utf8CodecRustCandidate, FileLoadReadLineReplayMatchesTheCandidateDecode)
{
	const auto corpus = LoadCorpus();
	const ShareDataScope shareData;
	ASSERT_TRUE(shareData.IsReady());
	size_t lines = 0;
	for (const Mode mode : kModes) {
		SCOPED_TRACE(Name(mode));
		const std::string document = BuildDocument(corpus.decode, mode);
		bool loaded = false;
		const auto produced = LoadThroughFileLoad(document, mode, loaded);
		ASSERT_TRUE(loaded) << "CFileLoad could not read the document";
		const auto segments = SplitLines(document);
		ASSERT_EQ(segments.size(), produced.size());
		std::set<EEolType> kinds;
		for (size_t index = 0; index < segments.size(); ++index) {
			SCOPED_TRACE(testing::Message() << "line " << index << " " << Clip(FormatBytes(segments[index].bytes)));
			EXPECT_EQ(segments[index].eol, produced[index].eol);
			kinds.insert(produced[index].eol);
			const auto candidate = rust_candidate::DecodeUtf8(CodeOf(mode), segments[index].bytes.data(), segments[index].bytes.size());
			ASSERT_TRUE(candidate);
			EXPECT_EQ(FormatUnits(produced[index].units), FormatUnits(*candidate.units));
			++lines;
		}
		EXPECT_EQ((std::set<EEolType>{ EEolType::none, EEolType::cr_and_lf, EEolType::line_feed, EEolType::carriage_return }), kinds);
	}
	EXPECT_GT(lines, 2u * 512u);
	RecordProperty("replayed_lines", static_cast<int>(lines));
}

// The encode path of a document save: CIoBridge::ImplToFile converts a document line with the codec of the mode. The
// candidate encodes the same units to the same bytes with the same loss flag, for every corpus line (the lossy ones
// included) and for every line that CFileLoad loaded, which production writes back to the bytes it read.
TEST(Utf8CodecRustCandidate, ImplToFileReplayMatchesTheCandidateEncodeAndTheLossFlag)
{
	const auto corpus = LoadCorpus();
	const ShareDataScope shareData;
	ASSERT_TRUE(shareData.IsReady());
	size_t lines = 0;
	size_t lossy = 0;
	for (const Mode mode : kModes) {
		SCOPED_TRACE(Name(mode));
		const auto codec = CCodeFactory::CreateCodeBase(CodeOf(mode));
		ASSERT_NE(nullptr, codec);
		const auto replay = [&](std::wstring_view units) {
			const CNativeW line(units.data(), units.size());
			CMemory saved;
			const EConvertResult result = CIoBridge::ImplToFile(line, &saved, codec.get());
			EXPECT_NE(RESULT_FAILURE, result);
			const auto candidate = rust_candidate::EncodeUtf8(CodeOf(mode), units.data(), units.size());
			ASSERT_TRUE(candidate);
			EXPECT_EQ(result == RESULT_LOSESOME, candidate.lossy);
			EXPECT_EQ(FormatBytes(BytesOf(saved)), FormatBytes(*candidate.bytes));
			++lines;
			if (candidate.lossy) {
				++lossy;
			}
		};
		for (const auto& row : corpus.encode) {
			if (Applies(row.variants, mode)) {
				SCOPED_TRACE(row.id);
				replay(row.input);
			}
		}

		const std::string document = BuildDocument(corpus.decode, mode);
		bool loaded = false;
		const auto produced = LoadThroughFileLoad(document, mode, loaded);
		ASSERT_TRUE(loaded) << "CFileLoad could not read the document";
		const auto segments = SplitLines(document);
		ASSERT_EQ(segments.size(), produced.size());
		for (size_t index = 0; index < produced.size(); ++index) {
			SCOPED_TRACE(testing::Message() << "line " << index << " " << Clip(FormatBytes(segments[index].bytes)));
			replay(produced[index].units);
			const CNativeW line(produced[index].units.data(), produced[index].units.size());
			CMemory saved;
			ASSERT_EQ(RESULT_COMPLETE, CIoBridge::ImplToFile(line, &saved, codec.get()));
			EXPECT_EQ(FormatBytes(segments[index].bytes), FormatBytes(BytesOf(saved)));
		}
	}
	EXPECT_GT(lossy, 0u);
	RecordProperty("replayed_lines", static_cast<int>(lines));
}

#else

TEST(Utf8CodecRustCandidate, IsExplicitlyUnavailableWithoutNativeRustArchive)
{
	const auto decoded = rust_candidate::DecodeUtf8(CODE_UTF8, nullptr, 0);
	EXPECT_FALSE(decoded);
	EXPECT_EQ(SakuraDocumentCodecStatus::Unsupported, decoded.status);
	const auto encoded = rust_candidate::EncodeUtf8(CODE_UTF8, nullptr, 0);
	EXPECT_FALSE(encoded);
	EXPECT_EQ(SakuraDocumentCodecStatus::Unsupported, encoded.status);
}

#endif

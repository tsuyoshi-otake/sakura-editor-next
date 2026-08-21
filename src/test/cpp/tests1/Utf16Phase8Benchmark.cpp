/*! @file @brief Reproducible Issue #239 UTF-16 benchmark-only harness. */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"
#include "Utf16Phase8Benchmark.h"

#include "agent/CSearchAgent.h"
#include "markdown/MarkdownCodeHighlighter.h"
#include "markdown/MarkdownParser.h"
#include "util/CpuDispatch.h"
#include "util/CpuDispatchInternal.h"
#include "util/Utf16BenchmarkTelemetry.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <limits>
#include <malloc.h>
#include <memory>
#include <numeric>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Utf16Phase8Benchmark
{
namespace
{
constexpr std::uint64_t kSeed = 0x239'2026'0821ULL;
constexpr std::size_t kStreamingBytes = 48U * 1024U * 1024U;
volatile std::uint64_t g_sink{};

enum class Operation {
	CrOrLf,
	Markdown,
	FindChar,
};

enum class CallerFamily {
	MarkdownParse,
	MarkdownHighlight,
	Search,
};

struct DirectImplementation {
	std::string name;
	std::string family;
	std::string isa;
	CpuDispatch::FindUtf16Function crOrLf;
	CpuDispatch::FindUtf16Function markdown;
	CpuDispatch::FindUtf16CharFunction findChar;
};

struct DirectCase {
	std::string group;
	std::string scenario;
	std::size_t length{};
	std::size_t alignment{};
	bool streaming{};
};

struct CallerCase {
	CallerFamily family{};
	std::string corpus;
	std::wstring text;
	std::wstring language;
	std::wstring pattern;
	int start{};
};

class JsonlWriter final {
public:
	explicit JsonlWriter(const std::filesystem::path& path)
		: stream_(path, std::ios::binary | std::ios::app)
	{
		if (!stream_) {
			throw std::runtime_error("cannot open benchmark JSONL output");
		}
	}

	void Write(std::string_view line)
	{
		stream_ << line << '\n';
		stream_.flush();
		if (!stream_) {
			throw std::runtime_error("cannot write benchmark JSONL output");
		}
	}

private:
	std::ofstream stream_;
};

class AlignedCorpus final {
public:
	AlignedCorpus(const DirectCase& testCase, Operation operation)
		: length_(testCase.length)
	{
		const std::size_t strideBytes = RoundUp(length_ * sizeof(wchar_t) + 64U, 64U);
		const std::size_t desiredSlices = testCase.streaming
			? std::max<std::size_t>(2U, kStreamingBytes / std::max<std::size_t>(strideBytes, 1U))
			: 1U;
		const std::size_t totalBytes = strideBytes * desiredSlices + 64U;
		allocation_.reset(static_cast<unsigned char*>(_aligned_malloc(totalBytes, 64U)));
		if (!allocation_) {
			throw std::bad_alloc{};
		}
		for (std::size_t index = 0; index < desiredSlices; ++index) {
			auto* const bytes = allocation_.get() + index * strideBytes + testCase.alignment;
			auto* const data = reinterpret_cast<wchar_t*>(bytes);
			if ((reinterpret_cast<std::uintptr_t>(data) & 63U) != testCase.alignment) {
				throw std::runtime_error("failed to construct requested alignment");
			}
			Fill(data, testCase, operation);
			slices_.push_back(data);
		}
	}

	const wchar_t* Slice(std::uint64_t iteration) const noexcept
	{
		return slices_[static_cast<std::size_t>(iteration % slices_.size())];
	}

	std::size_t SliceCount() const noexcept { return slices_.size(); }
	std::size_t Length() const noexcept { return length_; }
	std::size_t Expected() const noexcept { return expected_; }

private:
	struct FreeAligned {
		void operator()(unsigned char* value) const noexcept { _aligned_free(value); }
	};

	static std::size_t RoundUp(std::size_t value, std::size_t alignment) noexcept
	{
		return (value + alignment - 1U) & ~(alignment - 1U);
	}

	void Fill(wchar_t* data, const DirectCase& testCase, Operation operation)
	{
		const wchar_t fill = operation == Operation::Markdown ? L'a' : L'\u65e5';
		std::fill(data, data + length_, fill);
		const wchar_t primary = operation == Operation::CrOrLf ? L'\r'
			: operation == Operation::Markdown ? L'[' : L'q';
		if (testCase.scenario == "not_found" || length_ == 0U) {
			expected_ = length_;
			return;
		}

		std::size_t position = 0U;
		if (testCase.scenario == "first") {
			position = 0U;
		} else if (testCase.scenario == "middle" || testCase.scenario == "multiple") {
			position = length_ / 2U;
		} else if (testCase.scenario == "last") {
			position = length_ - 1U;
		} else if (testCase.scenario.rfind("boundary", 0U) == 0U) {
			position = static_cast<std::size_t>(std::stoull(testCase.scenario.substr(8U)));
			position = std::min(position, length_ - 1U);
		} else {
			throw std::runtime_error("unknown direct benchmark scenario");
		}
		data[position] = primary;
		if (testCase.scenario == "multiple" && position + 1U < length_) {
			const std::size_t second = std::min(length_ - 1U, position + std::max<std::size_t>(1U, length_ / 5U));
			data[second] = operation == Operation::CrOrLf ? L'\n'
				: operation == Operation::Markdown ? L'$' : primary;
		}
		expected_ = position;
	}

	std::unique_ptr<unsigned char, FreeAligned> allocation_;
	std::vector<wchar_t*> slices_;
	std::size_t length_{};
	std::size_t expected_{};
};

std::wstring GetEnvironmentWide(const wchar_t* name)
{
	const DWORD required = ::GetEnvironmentVariableW(name, nullptr, 0U);
	if (required == 0U) return {};
	std::wstring wide(required, L'\0');
	const DWORD copied = ::GetEnvironmentVariableW(name, wide.data(), required);
	if (copied == 0U || copied >= required) {
		throw std::runtime_error("cannot read benchmark environment value");
	}
	wide.resize(copied);
	return wide;
}

std::string GetEnvironment(const wchar_t* name)
{
	const std::wstring wide = GetEnvironmentWide(name);
	if (wide.empty()) return {};
	const int required = ::WideCharToMultiByte(
		CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), static_cast<int>(wide.size()),
		nullptr, 0, nullptr, nullptr);
	if (required <= 0) throw std::runtime_error("invalid UTF-16 benchmark environment value");
	std::string result(static_cast<std::size_t>(required), '\0');
	if (::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
		static_cast<int>(wide.size()), result.data(), required, nullptr, nullptr) != required) {
		throw std::runtime_error("cannot convert benchmark environment value");
	}
	return result;
}

std::filesystem::path GetEnvironmentPath(const wchar_t* name)
{
	const std::wstring value = GetEnvironmentWide(name);
	return value.empty() ? std::filesystem::path{} : std::filesystem::path(value);
}

std::uint64_t ParseUnsignedEnvironment(const wchar_t* name, std::uint64_t fallback)
{
	const std::string value = GetEnvironment(name);
	if (value.empty()) return fallback;
	std::size_t consumed = 0;
	const auto parsed = std::stoull(value, &consumed, 10);
	if (consumed != value.size()) throw std::runtime_error("invalid numeric benchmark environment value");
	return parsed;
}

double ParseDoubleEnvironment(const wchar_t* name, double fallback)
{
	const std::string value = GetEnvironment(name);
	if (value.empty()) return fallback;
	std::size_t consumed = 0;
	const double parsed = std::stod(value, &consumed);
	if (consumed != value.size() || !(parsed > 0.0)) {
		throw std::runtime_error("invalid floating-point benchmark environment value");
	}
	return parsed;
}

void PinCurrentProcess(std::uint64_t logicalCpu)
{
	const DWORD active = ::GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
	if (logicalCpu >= active || logicalCpu >= sizeof(DWORD_PTR) * 8U) {
		throw std::runtime_error("requested benchmark logical CPU is unavailable");
	}
	const DWORD_PTR mask = DWORD_PTR{1} << logicalCpu;
	if (::SetProcessAffinityMask(::GetCurrentProcess(), mask) == 0
		|| ::SetThreadAffinityMask(::GetCurrentThread(), mask) == 0) {
		throw std::runtime_error("cannot apply benchmark CPU affinity");
	}
	if (::SetPriorityClass(::GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS) == 0) {
		throw std::runtime_error("cannot set benchmark process priority");
	}
}

std::string OperationName(Operation operation)
{
	switch (operation) {
	case Operation::CrOrLf: return "crlf";
	case Operation::Markdown: return "markdown";
	default: return "find_char";
	}
}

std::string CallerFamilyName(CallerFamily family)
{
	switch (family) {
	case CallerFamily::MarkdownParse: return "markdown_parse";
	case CallerFamily::MarkdownHighlight: return "markdown_highlight";
	default: return "search";
	}
}

std::uint64_t Mix(std::uint64_t hash, std::uint64_t value) noexcept
{
	hash ^= value + 0x9e3779b97f4a7c15ULL + (hash << 6U) + (hash >> 2U);
	return hash;
}

__declspec(noinline) std::size_t ScalarCrOrLf(const wchar_t* data, std::size_t length) noexcept
{
	std::size_t offset = 0U;
#pragma loop(no_vector)
	while (offset < length && data[offset] != L'\r' && data[offset] != L'\n') ++offset;
	return offset;
}

bool IsMarkdownSpecial(wchar_t value) noexcept
{
	switch (value) {
	case L'\\': case L'`': case L'!': case L'[': case L'*':
	case L'_': case L'~': case L'<': case L'&': case L'$': return true;
	default: return false;
	}
}

__declspec(noinline) std::size_t ScalarMarkdown(const wchar_t* data, std::size_t length) noexcept
{
	std::size_t offset = 0U;
#pragma loop(no_vector)
	while (offset < length && !IsMarkdownSpecial(data[offset])) ++offset;
	return offset;
}

__declspec(noinline) std::size_t ScalarFindChar(
	const wchar_t* data, std::size_t length, wchar_t target) noexcept
{
	std::size_t offset = 0U;
#pragma loop(no_vector)
	while (offset < length && data[offset] != target) ++offset;
	return offset;
}

std::vector<DirectImplementation> GetDirectImplementations()
{
	std::vector<DirectImplementation> result{
		{"scalar", "scalar", "scalar", ScalarCrOrLf, ScalarMarkdown, ScalarFindChar},
	};
#if defined(SAKURA_UTF16_BACKEND_RUST)
	constexpr const char* backend = "rust";
	constexpr const char* prefix = "rust-";
#else
	constexpr const char* backend = "cpp";
	constexpr const char* prefix = "cpp-";
#endif
	for (const CpuDispatch::Isa isa : {
		CpuDispatch::Isa::Avx, CpuDispatch::Isa::Avx2, CpuDispatch::Isa::Avx512}) {
		const auto crlf = CpuDispatch::Testing::GetSupportedFindCrOrLfUtf16(isa);
		const auto markdown = CpuDispatch::Testing::GetSupportedFindMarkdownInlineSpecialUtf16(isa);
		const auto findChar = CpuDispatch::Testing::GetSupportedFindUtf16Char(isa);
		if (crlf == nullptr || markdown == nullptr || findChar == nullptr) continue;
		const char* const isaName = CpuDispatch::GetIsaName(isa);
		result.push_back({std::string(prefix) + isaName, backend, isaName,
			crlf, markdown, findChar});
	}
	return result;
}

std::vector<DirectCase> MakeDirectCases()
{
	std::vector<DirectCase> cases;
	constexpr std::array<std::size_t, 26> lengths{
		1, 2, 4, 5, 6, 7, 8, 9, 15, 16, 17, 31, 32, 33,
		63, 64, 65, 127, 128, 129, 255, 256, 257, 1024, 4096, 65536,
	};
	for (const std::size_t length : lengths) {
		for (const char* scenario : {"first", "middle", "last", "multiple", "not_found"}) {
			cases.push_back({"length", scenario, length, 0U, false});
		}
		for (const std::size_t width : {8U, 16U, 32U}) {
			if (length > width) {
				cases.push_back({"vector_boundary", "boundary" + std::to_string(width),
					length, 0U, false});
			}
		}
	}
	for (const std::size_t length : {8U, 16U, 32U, 257U, 4097U}) {
		for (std::size_t alignment = 0U; alignment <= 62U; alignment += 2U) {
			for (const char* scenario : {"last", "not_found"}) {
				cases.push_back({"alignment", scenario, length, alignment, false});
			}
		}
	}
	for (const std::size_t length : {4096U, 65536U, 1024U * 1024U}) {
		for (const std::size_t alignment : {0U, 62U}) {
			for (const char* scenario : {"middle", "last", "multiple", "not_found"}) {
				cases.push_back({"streaming", scenario, length, alignment, true});
			}
		}
	}
	return cases;
}

std::int64_t MeasureDirectTicks(
	Operation operation,
	const DirectImplementation& implementation,
	const AlignedCorpus& corpus,
	std::uint64_t iterations)
{
	LARGE_INTEGER begin{};
	LARGE_INTEGER end{};
	std::uint64_t accumulator = 0U;
	::QueryPerformanceCounter(&begin);
	switch (operation) {
	case Operation::CrOrLf:
		for (std::uint64_t index = 0; index < iterations; ++index) {
			accumulator = Mix(accumulator,
				implementation.crOrLf(corpus.Slice(index), corpus.Length()));
		}
		break;
	case Operation::Markdown:
		for (std::uint64_t index = 0; index < iterations; ++index) {
			accumulator = Mix(accumulator,
				implementation.markdown(corpus.Slice(index), corpus.Length()));
		}
		break;
	default:
		for (std::uint64_t index = 0; index < iterations; ++index) {
			accumulator = Mix(accumulator,
				implementation.findChar(corpus.Slice(index), corpus.Length(), L'q'));
		}
		break;
	}
	::QueryPerformanceCounter(&end);
	g_sink = Mix(static_cast<std::uint64_t>(g_sink), accumulator);
	return end.QuadPart - begin.QuadPart;
}

std::uint64_t CalibrateDirect(
	Operation operation,
	const DirectImplementation& implementation,
	const AlignedCorpus& corpus,
	std::int64_t targetTicks)
{
	std::uint64_t iterations = 1U;
	while (iterations < (std::numeric_limits<std::uint64_t>::max)() / 2U) {
		const std::int64_t ticks = MeasureDirectTicks(operation, implementation, corpus, iterations);
		if (ticks >= std::max<std::int64_t>(1, targetTicks / 4)) {
			const long double scale = static_cast<long double>(targetTicks)
				/ static_cast<long double>(std::max<std::int64_t>(ticks, 1));
			return std::max<std::uint64_t>(1U,
				static_cast<std::uint64_t>(static_cast<long double>(iterations) * scale));
		}
		iterations *= 2U;
	}
	return iterations;
}

void VerifyDirectResult(
	Operation operation,
	const DirectImplementation& implementation,
	const AlignedCorpus& corpus)
{
	std::size_t actual{};
	switch (operation) {
	case Operation::CrOrLf:
		actual = implementation.crOrLf(corpus.Slice(0), corpus.Length());
		break;
	case Operation::Markdown:
		actual = implementation.markdown(corpus.Slice(0), corpus.Length());
		break;
	default:
		actual = implementation.findChar(corpus.Slice(0), corpus.Length(), L'q');
		break;
	}
	if (actual != corpus.Expected()) {
		throw std::runtime_error("direct benchmark implementation disagrees with expected result");
	}
}

void WriteMetadata(JsonlWriter& writer, std::string_view mode, std::uint64_t runId,
	std::uint64_t affinity, const LARGE_INTEGER& frequency)
{
	const auto& dispatch = CpuDispatch::Get();
	std::ostringstream line;
	line << "{\"kind\":\"metadata\",\"mode\":\"" << mode
		<< "\",\"run_id\":" << runId
		<< ",\"pid\":" << ::GetCurrentProcessId()
		<< ",\"affinity_cpu\":" << affinity
		<< ",\"qpc_frequency\":" << frequency.QuadPart
		<< ",\"build_mode\":\"" << dispatch.utf16BuildMode
		<< "\",\"backend\":\"" << dispatch.utf16Backend
		<< "\",\"selected_isa\":\"" << CpuDispatch::GetIsaName(dispatch.isa)
		<< "\",\"threshold_crlf\":" << dispatch.utf16ScanPolicy.crOrLfMinimumLength
		<< ",\"threshold_markdown\":"
		<< dispatch.utf16ScanPolicy.markdownInlineSpecialMinimumLength
		<< ",\"threshold_find_char\":" << dispatch.utf16ScanPolicy.findCharMinimumLength
		<< ",\"abi_version\":" << dispatch.utf16AbiVersion << '}';
	writer.Write(line.str());
}

void RunDirect(JsonlWriter& writer, std::uint64_t runId, const LARGE_INTEGER& frequency)
{
	const double targetMilliseconds =
		ParseDoubleEnvironment(L"SAKURA_UTF16_BENCHMARK_TARGET_MS", 2.0);
	const auto targetTicks = static_cast<std::int64_t>(
		targetMilliseconds * static_cast<double>(frequency.QuadPart) / 1000.0);
	auto implementations = GetDirectImplementations();
	const auto capabilities = CpuDispatch::Get().capabilities;
	const std::size_t expectedImplementations =
		1U + (capabilities.avx ? 1U : 0U)
		+ (capabilities.avx2 ? 1U : 0U)
		+ (capabilities.avx512 ? 1U : 0U);
	if (implementations.size() != expectedImplementations) {
		throw std::runtime_error("all supported UTF-16 ISA implementations must be executable");
	}
	const auto cases = MakeDirectCases();
	std::uint64_t order = 0U;
	for (const Operation operation : {Operation::CrOrLf, Operation::Markdown, Operation::FindChar}) {
		for (std::size_t caseIndex = 0; caseIndex < cases.size(); ++caseIndex) {
			const auto& testCase = cases[caseIndex];
			AlignedCorpus corpus(testCase, operation);
			std::vector<std::size_t> implementationOrder(implementations.size());
			std::iota(implementationOrder.begin(), implementationOrder.end(), 0U);
			std::mt19937_64 engine(kSeed ^ runId ^ (caseIndex * 0x9e3779b9ULL)
				^ (static_cast<std::uint64_t>(operation) << 48U));
			std::shuffle(implementationOrder.begin(), implementationOrder.end(), engine);
			for (const std::size_t implementationIndex : implementationOrder) {
				const auto& implementation = implementations[implementationIndex];
				VerifyDirectResult(operation, implementation, corpus);
				for (int warmup = 0; warmup < 8; ++warmup) {
					MeasureDirectTicks(operation, implementation, corpus, 1U);
				}
				const std::uint64_t iterations = CalibrateDirect(
					operation, implementation, corpus, targetTicks);
				const std::int64_t ticks = MeasureDirectTicks(
					operation, implementation, corpus, iterations);
				const double nanoseconds = static_cast<double>(ticks) * 1e9
					/ static_cast<double>(frequency.QuadPart)
					/ static_cast<double>(iterations);
				std::ostringstream line;
				line << std::setprecision(17)
					<< "{\"kind\":\"direct_sample\",\"run_id\":" << runId
					<< ",\"sample_order\":" << order++
					<< ",\"operation\":\"" << OperationName(operation)
					<< "\",\"case_group\":\"" << testCase.group
					<< "\",\"scenario\":\"" << testCase.scenario
					<< "\",\"cache_mode\":\"" << (testCase.streaming ? "streaming" : "warm")
					<< "\",\"length\":" << testCase.length
					<< ",\"alignment_mod64\":" << testCase.alignment
					<< ",\"implementation\":\"" << implementation.name
					<< "\",\"implementation_family\":\"" << implementation.family
					<< "\",\"isa\":\"" << implementation.isa
					<< "\",\"iterations\":" << iterations
					<< ",\"duration_ticks\":" << ticks
					<< ",\"ns_per_call\":" << nanoseconds
					<< ",\"slice_count\":" << corpus.SliceCount()
					<< ",\"expected_position\":" << corpus.Expected()
					<< ",\"result_category\":\""
					<< (corpus.Expected() == corpus.Length() ? "not_found" : "found")
					<< "\"}";
				writer.Write(line.str());
			}
		}
	}
	std::printf("UTF16_PHASE8_DIRECT run=%llu cases=%zu implementations=%zu sink=%llu\n",
		static_cast<unsigned long long>(runId), cases.size(), implementations.size(),
		static_cast<unsigned long long>(g_sink));
}

std::wstring RepeatToLength(std::wstring_view seed, std::size_t length)
{
	std::wstring result;
	result.reserve(length);
	while (result.size() < length) result.append(seed);
	result.resize(length);
	return result;
}

std::vector<CallerCase> MakeCallerCases(bool histogramScale)
{
	const std::size_t longLength = histogramScale ? 64U * 1024U : 1024U * 1024U;
	const std::size_t lineCount = histogramScale ? 256U : 4096U;
	std::vector<CallerCase> cases;

	std::wstring shortLines;
	for (std::size_t index = 0; index < lineCount; ++index) {
		shortLines.append(index % 2U == 0U ? L"# short\n" : L"plain text\n");
	}
	cases.push_back({CallerFamily::MarkdownParse, "markdown_short_lines", std::move(shortLines)});

	std::wstring japanese;
	for (std::size_t line = 0; line < (histogramScale ? 32U : 256U); ++line) {
		japanese.append(L"## \u65e5\u672c\u8a9e ");
		japanese.append(RepeatToLength(L"\u691c\u7d22\u5bfe\u8c61\u6587\u7ae0\u8d70\u67fb", histogramScale ? 512U : 1024U));
		japanese.append(line % 8U == 0U ? L" **strong**\r\n" : L"\r\n");
	}
	cases.push_back({CallerFamily::MarkdownParse, "markdown_long_japanese", std::move(japanese)});

	std::wstring mixed;
	for (std::size_t line = 0; line < lineCount; ++line) {
		mixed.append(L"mixed line ");
		mixed.append(std::to_wstring(line));
		mixed.append(line % 3U == 0U ? L"\r\n" : line % 3U == 1U ? L"\n" : L"\r");
	}
	cases.push_back({CallerFamily::MarkdownParse, "markdown_mixed_eol", std::move(mixed)});
	cases.push_back({CallerFamily::MarkdownParse, "markdown_dense_specials",
		RepeatToLength(L"\\ ` ! [ * _ ~ < & $ ", longLength)});
	cases.push_back({CallerFamily::MarkdownParse, "markdown_sparse_specials",
		RepeatToLength(L"\u65e5\u672c\u8a9e plain text without punctuation ", longLength - 16U)
			+ L" [link](a.md)"});
	cases.push_back({CallerFamily::MarkdownParse, "markdown_no_specials",
		RepeatToLength(L"\u65e5\u672c\u8a9eplain", longLength)});
	cases.push_back({CallerFamily::MarkdownParse, "jsonc_minified",
		RepeatToLength(L"{\"key\":123,//comment\n\"items\":[true,false,null]}", longLength)});
	cases.push_back({CallerFamily::MarkdownParse, "plain_minified",
		RepeatToLength(L"abcdefghijklmnopqrstuvwxyz\u65e5\u672c\u8a9e", longLength)});
	cases.push_back({CallerFamily::MarkdownParse, "source_minified",
		RepeatToLength(L"int value=call(arg);if(value<10){value++;}//source\n", longLength)});

	const std::size_t parseCount = cases.size();
	for (std::size_t index = 0; index < parseCount; ++index) {
		const std::wstring language = cases[index].corpus == "jsonc_minified" ? L"json"
			: cases[index].corpus == "source_minified" ? L"cpp"
			: cases[index].corpus == "plain_minified" ? L"plain" : L"markdown";
		cases.push_back({CallerFamily::MarkdownHighlight,
			cases[index].corpus + "_highlight", cases[index].text, language});
	}

	auto addSearch = [&](std::string name, std::wstring text,
		std::wstring pattern, int start = 0) {
		cases.push_back({CallerFamily::Search, std::move(name), std::move(text), {},
			std::move(pattern), start});
	};
	addSearch("search_short_first", L"needle short text", L"needle");
	addSearch("search_short_not_found", L"short text without target", L"needle");
	std::wstring longPlain = RepeatToLength(L"abcdefghijklmnopqrstuvwxyz", longLength);
	longPlain.replace(longPlain.size() / 2U, 6U, L"needle");
	addSearch("search_long_middle", longPlain, L"needle");
	longPlain.replace(longPlain.size() / 2U, 6U, L"abcdef");
	longPlain.replace(longPlain.size() - 6U, 6U, L"needle");
	addSearch("search_long_last", longPlain, L"needle");
	addSearch("search_long_not_found", RepeatToLength(L"abcdefghijklm", longLength), L"needle");
	std::wstring longJapanese = RepeatToLength(L"\u691c\u7d22\u5bfe\u8c61\u6587\u7ae0\u8d70\u67fb", longLength);
	longJapanese.replace(longJapanese.size() - 4U, 4U, L"\u4e00\u81f4\u4f4d\u7f6e");
	addSearch("search_japanese_last", std::move(longJapanese), L"\u4e00\u81f4\u4f4d\u7f6e");
	std::wstring nulText = RepeatToLength(L"plain", longLength);
	nulText[nulText.size() / 2U] = L'\0';
	addSearch("search_nul_middle", std::move(nulText), std::wstring(1U, L'\0'));
	std::wstring surrogateText = RepeatToLength(L"plain", longLength);
	surrogateText.back() = static_cast<wchar_t>(0xd800);
	addSearch("search_surrogate_last", std::move(surrogateText),
		std::wstring(1U, static_cast<wchar_t>(0xd800)));
	std::wstring multiple = RepeatToLength(L"qaaaaaaaaaaaaaaa", longLength);
	multiple.replace(multiple.size() - 4U, 4U, L"qXYZ");
	addSearch("search_multiple_candidates", std::move(multiple), L"qXYZ");
	std::wstring jsonc = RepeatToLength(L"{\"item\":1,//note\n}", longLength);
	jsonc.replace(jsonc.size() / 2U, 6U, L"needle");
	addSearch("search_jsonc_minified", std::move(jsonc), L"needle");
	std::wstring source = RepeatToLength(L"int value=call(arg);", longLength);
	source.replace(source.size() - 11U, 11U, L"UniqueToken");
	addSearch("search_source_minified", std::move(source), L"UniqueToken");
	return cases;
}

std::uint64_t DigestMarkdown(const markdown::Document& document) noexcept
{
	std::uint64_t hash = Mix(0U, static_cast<std::uint64_t>(document.completion));
	hash = Mix(hash, document.blocks.size());
	for (const auto& block : document.blocks) {
		hash = Mix(hash, static_cast<std::uint64_t>(block.kind));
		hash = Mix(hash, block.text.size());
		hash = Mix(hash, block.inlineSpans.size());
		hash = Mix(hash, block.tableRows.size());
	}
	return hash;
}

std::uint64_t RunCallerOnce(const CallerCase& testCase, CSearchStringPattern* searchPattern)
{
	switch (testCase.family) {
	case CallerFamily::MarkdownParse:
		return DigestMarkdown(markdown::ParseMarkdown(testCase.text));
	case CallerFamily::MarkdownHighlight: {
		const auto result = markdown::HighlightMarkdownCode(
			testCase.language, testCase.text, 4096U);
		std::uint64_t hash = Mix(0U, result.tokens.size());
		hash = Mix(hash, result.scannedLength);
		hash = Mix(hash, result.workUnits);
		hash = Mix(hash, static_cast<std::uint64_t>(result.terminalState));
		return hash;
	}
	default: {
		const wchar_t* const result = CSearchAgent::SearchString(
			testCase.text.data(), static_cast<int>(testCase.text.size()),
			testCase.start, *searchPattern);
		return result == nullptr ? (std::numeric_limits<std::uint64_t>::max)()
			: static_cast<std::uint64_t>(result - testCase.text.data());
	}
	}
}

template <typename TBody>
std::int64_t MeasureCallerTicks(TBody&& body, std::uint64_t iterations)
{
	LARGE_INTEGER begin{};
	LARGE_INTEGER end{};
	std::uint64_t accumulator = 0U;
	::QueryPerformanceCounter(&begin);
	for (std::uint64_t iteration = 0; iteration < iterations; ++iteration) {
		accumulator = Mix(accumulator, body());
	}
	::QueryPerformanceCounter(&end);
	g_sink = Mix(static_cast<std::uint64_t>(g_sink), accumulator);
	return end.QuadPart - begin.QuadPart;
}

template <typename TBody>
std::uint64_t CalibrateCaller(TBody&& body, std::int64_t targetTicks)
{
	std::uint64_t iterations = 1U;
	while (iterations < (std::numeric_limits<std::uint64_t>::max)() / 2U) {
		const std::int64_t ticks = MeasureCallerTicks(body, iterations);
		if (ticks >= std::max<std::int64_t>(1, targetTicks / 4)) {
			const long double scale = static_cast<long double>(targetTicks)
				/ static_cast<long double>(std::max<std::int64_t>(ticks, 1));
			return std::max<std::uint64_t>(1U,
				static_cast<std::uint64_t>(static_cast<long double>(iterations) * scale));
		}
		iterations *= 2U;
	}
	return iterations;
}

void RunCaller(JsonlWriter& writer, std::uint64_t runId, const LARGE_INTEGER& frequency)
{
	const double targetMilliseconds =
		ParseDoubleEnvironment(L"SAKURA_UTF16_BENCHMARK_TARGET_MS", 25.0);
	const auto targetTicks = static_cast<std::int64_t>(
		targetMilliseconds * static_cast<double>(frequency.QuadPart) / 1000.0);
	auto cases = MakeCallerCases(false);
	std::vector<std::size_t> order(cases.size());
	std::iota(order.begin(), order.end(), 0U);
	std::mt19937_64 engine(kSeed ^ runId);
	std::shuffle(order.begin(), order.end(), engine);
	std::uint64_t sampleOrder = 0U;
	for (const std::size_t caseIndex : order) {
		const CallerCase& testCase = cases[caseIndex];
		SSearchOption option(false, true, false);
		CSearchStringPattern pattern;
		CSearchStringPattern* patternPointer = nullptr;
		if (testCase.family == CallerFamily::Search) {
			if (!pattern.SetPattern(nullptr, testCase.pattern.data(),
				testCase.pattern.size(), option, nullptr)) {
				throw std::runtime_error("cannot compile generated search pattern");
			}
			patternPointer = &pattern;
		}
		auto body = [&]() { return RunCallerOnce(testCase, patternPointer); };
		const std::uint64_t expectedDigest = body();
		for (int warmup = 0; warmup < 3; ++warmup) body();
		const std::uint64_t iterations = CalibrateCaller(body, targetTicks);
		const std::int64_t ticks = MeasureCallerTicks(body, iterations);
		const double nanoseconds = static_cast<double>(ticks) * 1e9
			/ static_cast<double>(frequency.QuadPart)
			/ static_cast<double>(iterations);
		const auto& dispatch = CpuDispatch::Get();
		std::ostringstream line;
		line << std::setprecision(17)
			<< "{\"kind\":\"caller_sample\",\"run_id\":" << runId
			<< ",\"sample_order\":" << sampleOrder++
			<< ",\"family\":\"" << CallerFamilyName(testCase.family)
			<< "\",\"corpus\":\"" << testCase.corpus
			<< "\",\"backend\":\"" << dispatch.utf16Backend
			<< "\",\"selected_isa\":\"" << CpuDispatch::GetIsaName(dispatch.isa)
			<< "\",\"length\":" << testCase.text.size()
			<< ",\"iterations\":" << iterations
			<< ",\"duration_ticks\":" << ticks
			<< ",\"ns_per_iteration\":" << nanoseconds
			<< ",\"result_digest\":" << expectedDigest << '}';
		writer.Write(line.str());
	}
	std::printf("UTF16_PHASE8_CALLER run=%llu backend=%s cases=%zu sink=%llu\n",
		static_cast<unsigned long long>(runId), CpuDispatch::Get().utf16Backend,
		cases.size(), static_cast<unsigned long long>(g_sink));
}

void RunHistogramCorpus()
{
	#if !defined(SAKURA_UTF16_BENCHMARK_TELEMETRY)
	throw std::runtime_error(
		"histogram mode requires SAKURA_UTF16_BENCHMARK_TELEMETRY");
	#else
	const auto cases = MakeCallerCases(true);
	for (const CallerCase& testCase : cases) {
		Utf16BenchmarkTelemetry::SetCorpusLabel(testCase.corpus);
		SSearchOption option(false, true, false);
		CSearchStringPattern pattern;
		CSearchStringPattern* patternPointer = nullptr;
		if (testCase.family == CallerFamily::Search) {
			if (!pattern.SetPattern(nullptr, testCase.pattern.data(),
				testCase.pattern.size(), option, nullptr)) {
				throw std::runtime_error("cannot compile generated histogram search pattern");
			}
			patternPointer = &pattern;
		}
		g_sink = Mix(static_cast<std::uint64_t>(g_sink),
			RunCallerOnce(testCase, patternPointer));
	}
	std::printf("UTF16_PHASE8_HISTOGRAM corpora=%zu sink=%llu\n", cases.size(),
		static_cast<unsigned long long>(g_sink));
	#endif
}
} // namespace

bool TryRunFromEnvironment()
{
	const std::string mode = GetEnvironment(L"SAKURA_UTF16_BENCHMARK_MODE");
	if (mode.empty()) return false;
	const std::uint64_t runId =
		ParseUnsignedEnvironment(L"SAKURA_UTF16_BENCHMARK_RUN_ID", 0U);
	const std::uint64_t affinity =
		ParseUnsignedEnvironment(L"SAKURA_UTF16_BENCHMARK_AFFINITY", 14U);
	PinCurrentProcess(affinity);
	LARGE_INTEGER frequency{};
	if (::QueryPerformanceFrequency(&frequency) == 0 || frequency.QuadPart <= 0) {
		throw std::runtime_error("QueryPerformanceFrequency failed");
	}
	const auto output = GetEnvironmentPath(L"SAKURA_UTF16_BENCHMARK_OUTPUT");
	if (output.empty()) throw std::runtime_error("benchmark output path is required");
	if (mode == "histogram") {
	#if defined(SAKURA_UTF16_BENCHMARK_TELEMETRY)
		Utf16BenchmarkTelemetry::Start(output, runId);
		struct TelemetryStop final {
			~TelemetryStop() noexcept { Utf16BenchmarkTelemetry::Stop(); }
		} stop;
	#endif
		RunHistogramCorpus();
		return true;
	}
	JsonlWriter writer(output);
	WriteMetadata(writer, mode, runId, affinity, frequency);
	if (mode == "direct") {
		RunDirect(writer, runId, frequency);
	} else if (mode == "caller") {
		RunCaller(writer, runId, frequency);
	} else {
		throw std::runtime_error("unknown SAKURA_UTF16_BENCHMARK_MODE");
	}
	return true;
}
}

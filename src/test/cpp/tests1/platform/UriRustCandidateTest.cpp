/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/

#include "pch.h"

#include "platform/uri/UriRustCandidate.h"
#include <sakura/uri/UriIdentity.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace platform::uri {
namespace {

SakuraUriCandidateStatus CandidateStatus(EUriParseError error) noexcept
{
	switch (error) {
	case EUriParseError::None: return SakuraUriCandidateStatus::Ok;
	case EUriParseError::EmptyInput: return SakuraUriCandidateStatus::EmptyInput;
	case EUriParseError::MissingScheme: return SakuraUriCandidateStatus::MissingScheme;
	case EUriParseError::InvalidScheme: return SakuraUriCandidateStatus::InvalidScheme;
	case EUriParseError::InvalidAuthority: return SakuraUriCandidateStatus::InvalidAuthority;
	case EUriParseError::InvalidPath: return SakuraUriCandidateStatus::InvalidPath;
	case EUriParseError::InvalidQuery: return SakuraUriCandidateStatus::InvalidQuery;
	case EUriParseError::InvalidFragment: return SakuraUriCandidateStatus::InvalidFragment;
	case EUriParseError::InvalidPercentEncoding: return SakuraUriCandidateStatus::InvalidPercentEncoding;
	case EUriParseError::InvalidUtf8: return SakuraUriCandidateStatus::InvalidUtf8;
	case EUriParseError::InvalidWindowsPath: return SakuraUriCandidateStatus::InvalidArgument;
	}
	return SakuraUriCandidateStatus::InternalError;
}

#if defined(SAKURA_UTF16_RUST_CANDIDATE)

void ExpectEquivalent(std::wstring_view text)
{
	const auto cpp = Uri::Parse(text);
	const auto rust = rust_candidate::Parse(text);
	ASSERT_EQ(static_cast<bool>(cpp), static_cast<bool>(rust));
	if (!cpp) {
		EXPECT_EQ(CandidateStatus(cpp.error), rust.status);
		return;
	}
	ASSERT_TRUE(rust.value);
	EXPECT_EQ(cpp.value->Scheme(), rust.value->scheme);
	EXPECT_EQ(cpp.value->Authority(), rust.value->authority);
	EXPECT_EQ(cpp.value->Path(), rust.value->path);
	EXPECT_EQ(cpp.value->Query(), rust.value->query);
	EXPECT_EQ(cpp.value->Fragment(), rust.value->fragment);
	EXPECT_EQ(cpp.value->HasAuthority(), rust.value->hasAuthority);
	EXPECT_EQ(cpp.value->ToString(), rust.value->serialized);
	EXPECT_EQ(SakuraUriCandidateStatus::Ok, rust.status);
}

void ExpectComponentsEquivalent(
	std::wstring scheme,
	std::wstring authority,
	std::wstring path,
	std::optional<std::wstring> query = std::nullopt,
	std::optional<std::wstring> fragment = std::nullopt,
	bool hasAuthority = false)
{
	const auto cpp = Uri::FromComponents(scheme, authority, path, query, fragment, hasAuthority);
	const auto rust = rust_candidate::FromComponents(
		std::move(scheme), std::move(authority), std::move(path),
		std::move(query), std::move(fragment), hasAuthority);
	ASSERT_EQ(static_cast<bool>(cpp), static_cast<bool>(rust));
	if (!cpp) {
		EXPECT_EQ(CandidateStatus(cpp.error), rust.status);
		return;
	}
	ASSERT_TRUE(rust.value);
	EXPECT_EQ(cpp.value->Scheme(), rust.value->scheme);
	EXPECT_EQ(cpp.value->Authority(), rust.value->authority);
	EXPECT_EQ(cpp.value->Path(), rust.value->path);
	EXPECT_EQ(cpp.value->Query(), rust.value->query);
	EXPECT_EQ(cpp.value->Fragment(), rust.value->fragment);
	EXPECT_EQ(cpp.value->HasAuthority(), rust.value->hasAuthority);
	EXPECT_EQ(cpp.value->ToString(), rust.value->serialized);
	EXPECT_EQ(SakuraUriCandidateStatus::Ok, rust.status);
}

SakuraUriCandidateSpanV1 Span(std::wstring_view value) noexcept
{
	SakuraUriCandidateSpanV1 span{};
	span.struct_size = sizeof(span);
	span.abi_version = SAKURA_URI_CANDIDATE_ABI_VERSION_V1;
	span.data = reinterpret_cast<const std::uint16_t*>(value.data());
	span.length = static_cast<std::uint64_t>(value.size());
	return span;
}

SakuraUriCandidateMeasureV1 Measure() noexcept
{
	SakuraUriCandidateMeasureV1 measure{};
	measure.struct_size = sizeof(measure);
	measure.abi_version = SAKURA_URI_CANDIDATE_ABI_VERSION_V1;
	return measure;
}

SakuraUriCandidateBufferV1 Buffer(std::vector<std::uint16_t>& storage) noexcept
{
	SakuraUriCandidateBufferV1 buffer{};
	buffer.struct_size = sizeof(buffer);
	buffer.abi_version = SAKURA_URI_CANDIDATE_ABI_VERSION_V1;
	buffer.data = storage.empty() ? nullptr : storage.data();
	buffer.capacity = static_cast<std::uint64_t>(storage.size());
	return buffer;
}

struct OutputStorage {
	std::vector<std::uint16_t> scheme;
	std::vector<std::uint16_t> authority;
	std::vector<std::uint16_t> path;
	std::vector<std::uint16_t> query;
	std::vector<std::uint16_t> fragment;
	std::vector<std::uint16_t> serialized;
};

OutputStorage FilledStorage(const SakuraUriCandidateMeasureV1& measure, std::uint16_t fill)
{
	return {
		std::vector<std::uint16_t>(static_cast<std::size_t>(measure.scheme_length), fill),
		std::vector<std::uint16_t>(static_cast<std::size_t>(measure.authority_length), fill),
		std::vector<std::uint16_t>(static_cast<std::size_t>(measure.path_length), fill),
		std::vector<std::uint16_t>(static_cast<std::size_t>(measure.query_length), fill),
		std::vector<std::uint16_t>(static_cast<std::size_t>(measure.fragment_length), fill),
		std::vector<std::uint16_t>(static_cast<std::size_t>(measure.serialized_length), fill),
	};
}

SakuraUriCandidateBuffersV1 Buffers(OutputStorage& storage) noexcept
{
	SakuraUriCandidateBuffersV1 buffers{};
	buffers.struct_size = sizeof(buffers);
	buffers.abi_version = SAKURA_URI_CANDIDATE_ABI_VERSION_V1;
	buffers.scheme = Buffer(storage.scheme);
	buffers.authority = Buffer(storage.authority);
	buffers.path = Buffer(storage.path);
	buffers.query = Buffer(storage.query);
	buffers.fragment = Buffer(storage.fragment);
	buffers.serialized = Buffer(storage.serialized);
	return buffers;
}

bool AllEqual(const std::vector<std::uint16_t>& values, std::uint16_t expected)
{
	return std::all_of(values.begin(), values.end(), [expected](std::uint16_t value) {
		return value == expected;
	});
}

TEST(UriRustCandidate, MatchesFrozenParseAndSerializationCorpus)
{
	constexpr std::array cases{
		L"file:///C:/Work%20Files/%E6%97%A5%E6%9C%AC.txt",
		L"DeMo://Example.Test/a%20b/%C3%A9?x=%2F#%F0%90%90%80",
		L"untitled:Untitled-1",
		L"vscode-extension://publisher.extension/repository/item",
		L"demo://host/path",
		L"demo://host/path?",
		L"demo://host/path#",
		L"demo://host/path?#",
		L"demo://host/path#fragment?still-fragment",
		L"demo://host/%41%42%43",
		L"demo://host/\u65e5\u672c\u8a9e",
		L"demo:path",
		L"demo:///path",
	};
	for (const auto* text : cases) {
		SCOPED_TRACE(::testing::Message() << "input length=" << std::wstring_view(text).size());
		ExpectEquivalent(text);
	}
}

TEST(UriRustCandidate, MatchesFrozenMalformedInputAndErrorPrecedence)
{
	const std::array<std::wstring, 18> cases{
		L"",
		L"missing/scheme",
		L"1demo:path",
		L"C:\\not-a-uri",
		L"demo://bad\\host/path",
		L"demo://host/raw space",
		L"demo://host/path?raw space",
		L"demo://host/path#raw space",
		L"demo://host/%C0%AF",
		L"demo://host/%E0%80%AF",
		L"demo://host/%ED%A0%80",
		L"demo://host/%ED%A0%BD%ED%B8%8A",
		L"demo://host/%F0%80%80%80",
		L"demo://host/%F4%90%80%80",
		L"demo://host/%80",
		L"demo://host/%E6%97",
		L"demo://host/%ZZ raw",
		L"demo://host/%2",
	};
	for (const auto& text : cases) {
		SCOPED_TRACE(::testing::Message() << "input length=" << text.size());
		ExpectEquivalent(text);
	}

	std::wstring invalidUtf16 = L"demo://host/";
	invalidUtf16.push_back(static_cast<wchar_t>(0xd800));
	ExpectEquivalent(invalidUtf16);
}

TEST(UriRustCandidate, MatchesFrozenFromComponentsContract)
{
	ExpectComponentsEquivalent(L"demo", L"", L"/path");
	ExpectComponentsEquivalent(L"demo", L"host", L"/path", L"", L"", true);
	ExpectComponentsEquivalent(L"demo", L"\u0130", L"/a \u00e9?#", L"q= ?", L"f# \u00e9", true);
	ExpectComponentsEquivalent(L"1demo", L"", L"/path");
	ExpectComponentsEquivalent(L"demo", L"host", L"/path");
	ExpectComponentsEquivalent(L"demo", L"host", L"path", std::nullopt, std::nullopt, true);

	std::wstring control(1, static_cast<wchar_t>(0x1f));
	ExpectComponentsEquivalent(L"demo", L"", L"/path", control, std::nullopt, true);
	std::wstring surrogate(1, static_cast<wchar_t>(0xd800));
	ExpectComponentsEquivalent(L"demo", L"", L"/path", std::nullopt, surrogate, true);
}

TEST(UriRustCandidate, CapacityFailureDoesNotPartiallyWriteAnyDestination)
{
	const std::wstring text = L"demo://host/path?query#fragment";
	const auto input = Span(text);
	auto measure = Measure();
	ASSERT_EQ(SakuraUriCandidateStatus::Ok,
		sakura_uri_candidate_parse_measure_v1(&input, &measure));
	ASSERT_GT(measure.serialized_length, 0u);

	constexpr std::uint16_t poison = 0x5a5a;
	auto storage = FilledStorage(measure, poison);
	storage.serialized.pop_back();
	const auto buffers = Buffers(storage);
	auto output = Measure();
	output.scheme_length = 77;
	EXPECT_EQ(SakuraUriCandidateStatus::InvalidCapacity,
		sakura_uri_candidate_parse_write_v1(&input, &buffers, &output));
	EXPECT_EQ(std::numeric_limits<std::uint64_t>::max(), output.scheme_length);
	EXPECT_EQ(std::numeric_limits<std::uint32_t>::max(), output.has_query);
	EXPECT_TRUE(AllEqual(storage.scheme, poison));
	EXPECT_TRUE(AllEqual(storage.authority, poison));
	EXPECT_TRUE(AllEqual(storage.path, poison));
	EXPECT_TRUE(AllEqual(storage.query, poison));
	EXPECT_TRUE(AllEqual(storage.fragment, poison));
	EXPECT_TRUE(AllEqual(storage.serialized, poison));
}

TEST(UriRustCandidate, InvalidAbiAndOverlappingOutputsFailClosed)
{
	const std::wstring text = L"demo://host/path";
	auto input = Span(text);
	auto output = Measure();
	input.reserved[0] = 1;
	EXPECT_EQ(SakuraUriCandidateStatus::InvalidArgument,
		sakura_uri_candidate_parse_measure_v1(&input, &output));
	EXPECT_EQ(std::numeric_limits<std::uint64_t>::max(), output.path_length);

	input = Span(text);
	auto measure = Measure();
	ASSERT_EQ(SakuraUriCandidateStatus::Ok,
		sakura_uri_candidate_parse_measure_v1(&input, &measure));
	auto storage = FilledStorage(measure, 0x4242);
	auto buffers = Buffers(storage);
	ASSERT_FALSE(storage.scheme.empty());
	buffers.authority.data = storage.scheme.data();
	buffers.authority.capacity = static_cast<std::uint64_t>(storage.scheme.size());
	output = Measure();
	EXPECT_EQ(SakuraUriCandidateStatus::InvalidArgument,
		sakura_uri_candidate_parse_write_v1(&input, &buffers, &output));
	EXPECT_TRUE(AllEqual(storage.scheme, 0x4242));
	EXPECT_TRUE(AllEqual(storage.authority, 0x4242));
	EXPECT_EQ(std::numeric_limits<std::uint64_t>::max(), output.serialized_length);
}

#else

TEST(UriRustCandidate, IsExplicitlyUnavailableWithoutNativeRustArchive)
{
	const auto result = rust_candidate::Parse(L"demo://host/path");
	EXPECT_FALSE(result);
	EXPECT_EQ(SakuraUriCandidateStatus::Unsupported, result.status);
}

#endif

} // namespace
} // namespace platform::uri

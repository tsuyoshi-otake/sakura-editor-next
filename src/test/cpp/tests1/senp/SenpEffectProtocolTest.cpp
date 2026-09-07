/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#include "pch.h"
#include <gtest/gtest.h>
#include "senp/SenpEffectProtocol.h"
#include <sakura/serialization/JsoncDocument.h>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace {
using namespace senp::effect;
using Json = platform::serialization::JsoncValue;

std::string Utf8(const std::wstring& text)
{
	if (text.empty()) return {};
	const auto size = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
	std::string result(static_cast<std::size_t>(size), '\0');
	if (size > 0) ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), size, nullptr, nullptr);
	return result;
}

std::filesystem::path FixturePath()
{
	// Both the repository-root entry point and configuration-directory CTest
	// runs consume the same checked-in fixture. Never substitute empty data.
	auto directory = std::filesystem::current_path();
	for (std::size_t level = 0; level < 8; ++level) {
		const auto path = directory / "rust/senp/fixtures/effect-protocol.jsonl";
		if (std::filesystem::is_regular_file(path)) return path;
		const auto parent = directory.parent_path();
		if (parent == directory) break;
		directory = parent;
	}
	return {};
}

TEST(SenpEffectProtocol, SharedFixturesAgreeOnGrammarBoundsAndTypedRoundTrips)
{
	std::ifstream input(FixturePath(), std::ios::binary);
	ASSERT_TRUE(input.good());
	std::ofstream output;
	if (const auto* path = std::getenv("SENP_PROTOCOL_OUTPUT")) { output.open(path, std::ios::binary); ASSERT_TRUE(output.good()); }
	std::string line;
	std::size_t cases = 0, accepted = 0;
	while (std::getline(input, line)) {
		const auto fixture = platform::serialization::CJsoncDocument::ParseStrict(line);
		ASSERT_TRUE(fixture.Succeeded());
		const auto& fields = std::get<Json::Object>(fixture.value->Value());
		SCOPED_TRACE(Utf8(std::get<std::wstring>(fields.at(L"name").Value())));
		auto wire = Utf8(std::get<std::wstring>(fields.at(L"input").Value()));
		if (const auto fill = fields.find(L"fill"); fill != fields.end()) {
			const auto count = std::get<std::int64_t>(fill->second.Value());
			ASSERT_GE(count, 0); ASSERT_LE(count, kMaximumFrameBytes);
			const auto position = wire.find("~fill~"); ASSERT_NE(std::string::npos, position);
			wire.replace(position, 6, static_cast<std::size_t>(count), 'x');
		}
		const bool valid = std::get<bool>(fields.at(L"valid").Value());
		const auto decoded = Decode(wire);
		ASSERT_EQ(valid, decoded.has_value());
		if (decoded) {
			const auto encoded = Encode(*decoded); ASSERT_TRUE(encoded);
			const auto again = Decode(*encoded); ASSERT_TRUE(again); EXPECT_EQ(*decoded, *again);
			if (output.is_open()) output << *encoded << '\n';
			++accepted;
		}
		++cases;
	}
	EXPECT_EQ(71U, cases); EXPECT_EQ(26U, accepted);
	if (output.is_open()) { output.flush(); EXPECT_TRUE(output.good()); }
}

TEST(SenpEffectProtocol, AcceptsPeerSerializedFixturesWhenRequested)
{
	const auto* path = std::getenv("SENP_PROTOCOL_PEER_FILE");
	if (!path) GTEST_SKIP() << "Set SENP_PROTOCOL_PEER_FILE for the cross-language exchange.";
	std::ifstream input(path, std::ios::binary); ASSERT_TRUE(input.good());
	std::string line; std::size_t count = 0;
	while (std::getline(input, line)) { SCOPED_TRACE(count); ASSERT_TRUE(Decode(line)); ++count; }
	EXPECT_EQ(26U, count);
}

TEST(SenpEffectProtocol, RejectsFrameLimitInvalidUnicodeAndInvalidOutgoingFields)
{
	EXPECT_FALSE(Decode(std::string(kMaximumFrameBytes + 1, ' ')));
	EXPECT_FALSE(Decode(std::string("\xff", 1)));
	Envelope message{ 2, 1, 1, Hello{ std::wstring(kAbi) } };
	EXPECT_TRUE(Encode(message));
	message.sequence = -1; EXPECT_FALSE(Encode(message));
	message.sequence = 1;
	PublishDocument document{ L"document", L"title", 1,
		{ MarkdownSection{ std::wstring(1, static_cast<wchar_t>(0xd800)) } } };
	message.body = EffectsMessage{ { L"op", 1, 0, 0, 1 }, { document } };
	EXPECT_FALSE(Encode(message));
	document.sections = std::vector<DocumentSection>(4, MarkdownSection{ std::wstring(262144, L'x') });
	message.body = EffectsMessage{ { L"op", 1, 0, 0, 1 }, { std::move(document) } };
	EXPECT_FALSE(Encode(message)); // Each section fits; the aggregate frame does not.
	message.body = Deactivate{ static_cast<StopReason>(255) }; EXPECT_FALSE(Encode(message));
}

TEST(SenpEffectProtocol, StrictJsonModeRetainsConfigurationJsoncCompatibility)
{
	for (const auto& text : { std::string("/* comment */{\"key\":[1,],}"), std::string("\xef\xbb\xbf{\"key\":1}") }) {
		EXPECT_TRUE(platform::serialization::CJsoncDocument::Parse(text).Succeeded());
		EXPECT_FALSE(platform::serialization::CJsoncDocument::ParseStrict(text).Succeeded());
	}
	EXPECT_FALSE(platform::serialization::CJsoncDocument::ParseStrict("{\"key\":1,\"key\":2}").Succeeded());
	EXPECT_TRUE(platform::serialization::CJsoncDocument::ParseStrict("{\"key\":[1,2]}").Succeeded());
}

TEST(SenpEffectProtocol, EnforcesAggregateNodeBudgetBeforePublishingEncodedDocument)
{
	TableSection table{ std::vector<std::wstring>(16, L"column"),
		std::vector<TableRow>(256, TableRow{ std::vector<std::wstring>(16) }) };
	PublishDocument document{ L"document", L"title", 1, std::vector<DocumentSection>(32, table) };
	Envelope envelope{ 2, 1, 1, EffectsMessage{ { L"op", 1, 0, 0, 1 }, { document } } };
	EXPECT_FALSE(Encode(envelope));
}
} // namespace

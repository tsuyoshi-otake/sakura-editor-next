/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#include "pch.h"

#include "platform/serialization/JsoncDocument.h"

#include <gtest/gtest.h>

#include <variant>

namespace {

using platform::serialization::CJsoncDocument;
using platform::serialization::EJsoncDiagnosticCode;
using platform::serialization::JsoncValue;

TEST(JsoncDocument, AcceptsCommentsTrailingCommasAndTypedValues)
{
	const auto result = CJsoncDocument::Parse(R"json({
		// JSONC line comment
		"enabled": true,
		"items": [1, "two", null,],
		/* JSONC block comment */
	})json");

	ASSERT_TRUE(result.Succeeded());
	const auto* object = std::get_if<JsoncValue::Object>(&result.value->Value());
	ASSERT_NE(nullptr, object);
	EXPECT_EQ(true, std::get<bool>(object->at(L"enabled").Value()));
	const auto& items = std::get<JsoncValue::Array>(object->at(L"items").Value());
	ASSERT_EQ(3U, items.size());
	EXPECT_EQ(1, std::get<std::int64_t>(items[0].Value()));
	EXPECT_EQ(L"two", std::get<std::wstring>(items[1].Value()));
	EXPECT_TRUE(std::holds_alternative<std::monostate>(items[2].Value()));
}

TEST(JsoncDocument, ReportsDiagnosticsAgainstOriginalBomPrefixedInput)
{
	const std::string input("\xef\xbb\xbf{\"key\": }");
	const auto result = CJsoncDocument::Parse(input);

	ASSERT_FALSE(result.Succeeded());
	ASSERT_TRUE(result.diagnostic.has_value());
	EXPECT_EQ(EJsoncDiagnosticCode::UnexpectedToken, result.diagnostic->code);
	EXPECT_EQ(input.find('}'), result.diagnostic->byteOffset);
}

TEST(JsoncDocument, RejectsDuplicateKeysWithoutReturningAPartialDocument)
{
	const auto result = CJsoncDocument::Parse(R"json({ "same": 1, "same": 2 })json");

	EXPECT_FALSE(result.value.has_value());
	ASSERT_TRUE(result.diagnostic.has_value());
	EXPECT_EQ(EJsoncDiagnosticCode::DuplicateKey, result.diagnostic->code);
}

TEST(JsoncDocument, RejectsInvalidUtf8AtTheOriginalByteOffset)
{
	const std::string validPrefix("{\"key\":\"");
	const std::string invalid = validPrefix + static_cast<char>(0xff) + "\"}";
	const auto result = CJsoncDocument::Parse(invalid);

	ASSERT_FALSE(result.Succeeded());
	ASSERT_TRUE(result.diagnostic.has_value());
	EXPECT_EQ(EJsoncDiagnosticCode::InvalidUtf8, result.diagnostic->code);
	EXPECT_EQ(validPrefix.size(), result.diagnostic->byteOffset);
}

} // namespace

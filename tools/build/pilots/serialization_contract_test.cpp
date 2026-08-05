/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#include <sakura/serialization/JsoncDocument.h>

#include <array>
#include <iostream>
#include <string>
#include <string_view>
#include <variant>

namespace {

using platform::serialization::CJsoncDocument;
using platform::serialization::EJsoncDiagnosticCode;
using platform::serialization::JsoncValue;

bool AcceptsCommentsTrailingCommasAndTypedValues()
{
	const auto result = CJsoncDocument::Parse(R"json({
		// JSONC line comment
		"enabled": true,
		"items": [1, "two", null,],
		/* JSONC block comment */
	})json");
	if (!result.Succeeded()) return false;
	const auto* object = std::get_if<JsoncValue::Object>(&result.value->Value());
	if (!object) return false;
	const auto enabled = object->find(L"enabled");
	const auto items = object->find(L"items");
	if (enabled == object->end() || items == object->end()) return false;
	const auto* enabledValue = std::get_if<bool>(&enabled->second.Value());
	const auto* itemValues = std::get_if<JsoncValue::Array>(&items->second.Value());
	return enabledValue && *enabledValue && itemValues && itemValues->size() == 3
		&& std::get<std::int64_t>((*itemValues)[0].Value()) == 1
		&& std::get<std::wstring>((*itemValues)[1].Value()) == L"two"
		&& std::holds_alternative<std::monostate>((*itemValues)[2].Value());
}

bool ReportsDiagnosticsAgainstOriginalBomPrefixedInput()
{
	const std::string input("\xef\xbb\xbf{\"key\": }");
	const auto result = CJsoncDocument::Parse(input);
	return !result.Succeeded() && result.diagnostic
		&& result.diagnostic->code == EJsoncDiagnosticCode::UnexpectedToken
		&& result.diagnostic->byteOffset == input.find('}');
}

bool RejectsDuplicateKeysWithoutReturningAPartialDocument()
{
	const auto result = CJsoncDocument::Parse(R"json({ "same": 1, "same": 2 })json");
	return !result.value && result.diagnostic
		&& result.diagnostic->code == EJsoncDiagnosticCode::DuplicateKey;
}

bool RejectsInvalidUtf8AtTheOriginalByteOffset()
{
	const std::string prefix("{\"key\":\"");
	const std::string invalid = prefix + static_cast<char>(0xff) + "\"}";
	const auto result = CJsoncDocument::Parse(invalid);
	return !result.Succeeded() && result.diagnostic
		&& result.diagnostic->code == EJsoncDiagnosticCode::InvalidUtf8
		&& result.diagnostic->byteOffset == prefix.size();
}

struct TestCase {
	std::string_view name;
	bool (*run)();
};

constexpr std::array kTests{
	TestCase{"AcceptsCommentsTrailingCommasAndTypedValues", AcceptsCommentsTrailingCommasAndTypedValues},
	TestCase{"ReportsDiagnosticsAgainstOriginalBomPrefixedInput", ReportsDiagnosticsAgainstOriginalBomPrefixedInput},
	TestCase{"RejectsDuplicateKeysWithoutReturningAPartialDocument", RejectsDuplicateKeysWithoutReturningAPartialDocument},
	TestCase{"RejectsInvalidUtf8AtTheOriginalByteOffset", RejectsInvalidUtf8AtTheOriginalByteOffset},
};

bool Matches(std::string_view fullName, std::string_view filter)
{
	if (filter.empty() || filter == "*") return true;
	const auto star = filter.find('*');
	if (star == std::string_view::npos) return fullName == filter;
	const auto prefix = filter.substr(0, star);
	const auto suffix = filter.substr(star + 1);
	return fullName.starts_with(prefix) && fullName.ends_with(suffix)
		&& fullName.size() >= prefix.size() + suffix.size();
}

} // namespace

int main(int argc, char** argv)
{
	std::string_view filter = "JsoncDocument.*";
	for (int index = 1; index < argc; ++index) {
		const std::string_view argument = argv[index];
		if (argument == "--gtest_list_tests") {
			std::cout << "JsoncDocument.\n";
			for (const auto& test : kTests) std::cout << "  " << test.name << '\n';
			return 0;
		}
		constexpr std::string_view prefix = "--gtest_filter=";
		if (argument.starts_with(prefix)) filter = argument.substr(prefix.size());
	}

	int selected = 0;
	int failed = 0;
	for (const auto& test : kTests) {
		const std::string fullName = "JsoncDocument." + std::string(test.name);
		if (!Matches(fullName, filter)) continue;
		++selected;
		const bool passed = test.run();
		std::cout << (passed ? "[       OK ] " : "[  FAILED  ] ") << fullName << '\n';
		if (!passed) ++failed;
	}
	std::cout << "[==========] " << selected << " tests ran; " << failed << " failed.\n";
	return failed == 0 && selected > 0 ? 0 : 1;
}

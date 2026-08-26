/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/

#include <sakura/uri/UriIdentity.h>

#if __has_include("UriIdentityInternal.h") || __has_include("platform/uri/UriIdentityInternal.h")
#error "sakura_uri_tests consumer can reach the provider private header"
#endif

#include <array>
#include <clocale>
#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <string>
#include <string_view>

namespace {

using namespace platform::uri;

std::wstring CodeUnits(std::initializer_list<std::uint32_t> values)
{
	std::wstring result;
	result.reserve(values.size());
	for (const auto value : values) result.push_back(static_cast<wchar_t>(value));
	return result;
}

constexpr wchar_t kComparisonKeySeparator = static_cast<wchar_t>(0x1f);

bool ConvertsDrivePathToCanonicalFileUriAndBackWithoutFilesystemResolution()
{
	auto uri = Uri::FromWindowsPath(L"C:\\Work Files\\\u65e5\u672c\u8a9e.txt");
	if (!uri || uri.value->ToString() != L"file:///C:/Work%20Files/%E6%97%A5%E6%9C%AC%E8%AA%9E.txt") return false;
	auto path = uri.value->ToWindowsPath();
	return path && *path.value == L"C:\\Work Files\\\u65e5\u672c\u8a9e.txt";
}

bool ConvertsUncPathToFileUriAndBack()
{
	auto uri = Uri::FromWindowsPath(L"\\\\server\\share\\folder\\note.txt");
	if (!uri || uri.value->ToString() != L"file://server/share/folder/note.txt") return false;
	auto path = uri.value->ToWindowsPath();
	return path && *path.value == L"\\\\server\\share\\folder\\note.txt";
}

bool ConvertsExtendedWindowsPathsToCanonicalFileUrisAndBack()
{
	auto driveUri = Uri::FromWindowsPath(L"\\\\?\\C:\\workspace\\file.txt");
	if (!driveUri || driveUri.value->ToString() != L"file:///C:/workspace/file.txt") return false;
	auto drivePath = driveUri.value->ToWindowsPath();
	if (!drivePath || *drivePath.value != L"C:\\workspace\\file.txt") return false;

	auto uncUri = Uri::FromWindowsPath(L"\\\\?\\UNC\\server\\share\\file.txt");
	if (!uncUri || uncUri.value->ToString() != L"file://server/share/file.txt") return false;
	auto uncPath = uncUri.value->ToWindowsPath();
	return uncPath && *uncPath.value == L"\\\\server\\share\\file.txt";
}

bool ParsesAndSerializesEncodedSpaceAndUnicode()
{
	auto uri = Uri::Parse(L"file:///C:/Work%20Files/%E6%97%A5%E6%9C%AC.txt");
	return uri && uri.value->Path() == L"/C:/Work Files/\u65e5\u672c.txt"
		&& uri.value->ToString() == L"file:///C:/Work%20Files/%E6%97%A5%E6%9C%AC.txt";
}

bool ParsesComponentsAndPreservesDecodedFields()
{
	const auto astral = CodeUnits({0xd801, 0xdc00});
	auto uri = Uri::Parse(L"DeMo://Example.Test/a%20b/%C3%A9?x=%2F#%F0%90%90%80");
	const auto expectedPath = std::wstring{L"/a b/"} + CodeUnits({0x00e9});
	if (!uri || uri.value->Path() != expectedPath || !uri.value->Fragment() || *uri.value->Fragment() != astral) return false;
	return uri && uri.value->Scheme() == L"demo"
		&& uri.value->Authority() == L"Example.Test"
		&& uri.value->HasAuthority()
		&& uri.value->Path() == expectedPath
		&& uri.value->Query() && *uri.value->Query() == L"x=/"
		&& uri.value->Fragment() && *uri.value->Fragment() == astral
		&& uri.value->ToString() == L"demo://Example.Test/a%20b/%C3%A9?x=/#%F0%90%90%80";
}

bool EncodesComponentsWithComponentSpecificPercentRules()
{
	const auto authority = CodeUnits({0x0130});
	const auto path = std::wstring{L"/a "} + CodeUnits({0x00e9}) + L"?#";
	const auto query = std::wstring{L"q= ?"};
	const auto fragment = std::wstring{L"f# "} + CodeUnits({0x00e9});
	auto uri = Uri::FromComponents(L"demo", authority, path, query, fragment, true);
	return uri && uri.value->ToString() == L"demo://%C4%B0/a%20%C3%A9%3F%23?q=%20?#f%23%20%C3%A9";
}

bool SupportsUntitledAndArbitrarySchemes()
{
	auto untitled = Uri::Parse(L"untitled:Untitled-1");
	auto extension = Uri::Parse(L"git+demo://Example.Test/repository/item");
	return untitled && untitled.value->Scheme() == L"untitled" && untitled.value->Path() == L"Untitled-1"
		&& untitled.value->ToString() == L"untitled:Untitled-1" && extension
		&& extension.value->Scheme() == L"git+demo" && extension.value->Authority() == L"Example.Test"
		&& extension.value->Path() == L"/repository/item";
}

bool RejectsMalformedUtf8AndCesu8PercentRuns()
{
	struct InvalidUri {
		const wchar_t* text;
		EUriParseError error;
	};
	const std::array cases{
		InvalidUri{L"demo://host/%C0%AF", EUriParseError::InvalidUtf8},
		InvalidUri{L"demo://host/%E0%80%AF", EUriParseError::InvalidUtf8},
		InvalidUri{L"demo://host/%ED%A0%80", EUriParseError::InvalidUtf8},
		InvalidUri{L"demo://host/%ED%A0%BD%ED%B8%8A", EUriParseError::InvalidUtf8},
		InvalidUri{L"demo://host/%F0%80%80%80", EUriParseError::InvalidUtf8},
		InvalidUri{L"demo://host/%F4%90%80%80", EUriParseError::InvalidUtf8},
		InvalidUri{L"demo://host/%80", EUriParseError::InvalidUtf8},
		InvalidUri{L"demo://host/%E6%97", EUriParseError::InvalidUtf8},
		InvalidUri{L"demo://host/%ZZ", EUriParseError::InvalidPercentEncoding},
		InvalidUri{L"demo://host/%2", EUriParseError::InvalidPercentEncoding},
		InvalidUri{L"demo://host/path?%C0%AF", EUriParseError::InvalidUtf8},
		InvalidUri{L"demo://host/path#%ED%A0%80", EUriParseError::InvalidUtf8},
	};
	for (const auto& item : cases) {
		auto uri = Uri::Parse(item.text);
		if (uri || uri.error != item.error) return false;
	}
	return true;
}

bool PreservesQueryAndFragmentAsDistinctComponents()
{
	auto uri = Uri::Parse(L"demo://host/path?line=3%20and%204#section%202");
	return uri && uri.value->Query() && uri.value->Fragment()
		&& *uri.value->Query() == L"line=3 and 4" && *uri.value->Fragment() == L"section 2"
		&& uri.value->ToString() == L"demo://host/path?line=3%20and%204#section%202";
}

bool PreservesAbsentAndEmptyQueryAndFragmentValues()
{
	auto absent = Uri::Parse(L"demo://host/path");
	auto emptyQuery = Uri::Parse(L"demo://host/path?");
	auto emptyFragment = Uri::Parse(L"demo://host/path#");
	auto emptyBoth = Uri::Parse(L"demo://host/path?#");
	return absent && emptyQuery && emptyFragment && emptyBoth
		&& !absent.value->Query() && !absent.value->Fragment()
		&& emptyQuery.value->Query() && emptyQuery.value->Query()->empty() && !emptyQuery.value->Fragment()
		&& !emptyFragment.value->Query() && emptyFragment.value->Fragment() && emptyFragment.value->Fragment()->empty()
		&& emptyBoth.value->Query() && emptyBoth.value->Query()->empty()
		&& emptyBoth.value->Fragment() && emptyBoth.value->Fragment()->empty()
		&& absent.value->ToString() == L"demo://host/path"
		&& emptyQuery.value->ToString() == L"demo://host/path?"
		&& emptyFragment.value->ToString() == L"demo://host/path#"
		&& emptyBoth.value->ToString() == L"demo://host/path?#";
}

bool FileIdentityIsCaseInsensitiveWithoutRealPathResolution()
{
	auto first = Uri::Parse(L"file:///C:/Workspace/ReadMe.md");
	auto second = Uri::Parse(L"FILE:///c:/workspace/readme.md");
	auto changedQuery = Uri::Parse(L"file:///c:/workspace/readme.md?view=Preview");
	return first && second && changedQuery && UriIdentityService::IsEqual(*first.value, *second.value)
		&& !UriIdentityService::IsEqual(*first.value, *changedQuery.value);
}

bool LocalhostAndAuthoritySyntaxShareLocalFileIdentity()
{
	auto canonical = Uri::Parse(L"file:///C:/Workspace/ReadMe.md");
	auto localhost = Uri::Parse(L"file://localhost/c:/workspace/readme.md");
	auto authorityLess = Uri::Parse(L"file:/c:/workspace/readme.md");
	return canonical && localhost && authorityLess
		&& UriIdentityService::IsEqual(*canonical.value, *localhost.value)
		&& UriIdentityService::IsEqual(*canonical.value, *authorityLess.value);
}

bool ComparisonKeyIsByteExactAndTracksComponentPresence()
{
	auto canonical = Uri::Parse(L"file:///C:/Workspace/ReadMe.md");
	auto alias = Uri::Parse(L"FILE://localhost/c:/workspace/readme.md");
	auto emptyQuery = Uri::Parse(L"file:///C:/Workspace/ReadMe.md?");
	if (!canonical || !alias || !emptyQuery) return false;
	std::wstring expected;
	expected.append(L"4:file");
	expected.push_back(kComparisonKeySeparator);
	expected.append(L"1");
	expected.push_back(kComparisonKeySeparator);
	expected.append(L"0:");
	expected.push_back(kComparisonKeySeparator);
	expected.append(L"23:/c:/workspace/readme.md");
	expected.push_back(kComparisonKeySeparator);
	expected.append(L"0");
	expected.push_back(kComparisonKeySeparator);
	expected.append(L"0:");
	expected.push_back(kComparisonKeySeparator);
	expected.append(L"0");
	expected.push_back(kComparisonKeySeparator);
	expected.append(L"0:");
	expected.push_back(kComparisonKeySeparator);
	return UriIdentityService::MakeComparisonKey(*canonical.value) == expected
		&& UriIdentityService::MakeComparisonKey(*alias.value) == expected
		&& UriIdentityService::IsEqual(*canonical.value, *alias.value)
		&& !UriIdentityService::IsEqual(*canonical.value, *emptyQuery.value);
}

bool NonFileCasePolicyIsExplicit()
{
	auto first = Uri::Parse(L"demo://Host/Path?Name=Value#Anchor");
	auto second = Uri::Parse(L"demo://host/path?name=value#anchor");
	return first && second && !UriIdentityService::IsEqual(*first.value, *second.value)
		&& UriIdentityService::IsEqual(*first.value, *second.value, ENonFileUriCasePolicy::CaseInsensitive);
}

bool FreezesJapaneseLocaleCaseSentinels()
{
	const char* previousLocale = std::setlocale(LC_ALL, nullptr);
	if (previousLocale == nullptr) return false;
	const std::string savedLocale(previousLocale);
	if (std::setlocale(LC_ALL, "Japanese") == nullptr) return false;

	const auto dottedCapital = CodeUnits({0x0130});
	const auto deseretCapital = CodeUnits({0xd801, 0xdc00});
	const auto deseretSmall = CodeUnits({0xd801, 0xdc28});
	auto dotted = Uri::FromComponents(L"demo", dottedCapital, L"/path", std::nullopt, std::nullopt, true);
	auto asciiI = Uri::FromComponents(L"demo", L"i", L"/path", std::nullopt, std::nullopt, true);
	auto deseretUpper = Uri::FromComponents(L"demo", L"Host", std::wstring{L"/"} + deseretCapital, std::nullopt, std::nullopt, true);
	auto deseretLower = Uri::FromComponents(L"demo", L"host", std::wstring{L"/"} + deseretSmall, std::nullopt, std::nullopt, true);

	bool passed = dotted && asciiI && deseretUpper && deseretLower
		&& !UriIdentityService::IsEqual(*dotted.value, *asciiI.value, ENonFileUriCasePolicy::CaseInsensitive)
		&& !UriIdentityService::IsEqual(*deseretUpper.value, *deseretLower.value, ENonFileUriCasePolicy::CaseInsensitive);
	if (passed) {
		std::wstring expectedDotted;
		expectedDotted.append(L"4:demo");
		expectedDotted.push_back(kComparisonKeySeparator);
		expectedDotted.append(L"1");
		expectedDotted.push_back(kComparisonKeySeparator);
		expectedDotted.append(L"1:");
		expectedDotted.append(dottedCapital);
		expectedDotted.push_back(kComparisonKeySeparator);
		expectedDotted.append(L"5:/path");
		expectedDotted.push_back(kComparisonKeySeparator);
		expectedDotted.append(L"0");
		expectedDotted.push_back(kComparisonKeySeparator);
		expectedDotted.append(L"0:");
		expectedDotted.push_back(kComparisonKeySeparator);
		expectedDotted.append(L"0");
		expectedDotted.push_back(kComparisonKeySeparator);
		expectedDotted.append(L"0:");
		expectedDotted.push_back(kComparisonKeySeparator);
		std::wstring expectedDeseret;
		expectedDeseret.append(L"4:demo");
		expectedDeseret.push_back(kComparisonKeySeparator);
		expectedDeseret.append(L"1");
		expectedDeseret.push_back(kComparisonKeySeparator);
		expectedDeseret.append(L"4:host");
		expectedDeseret.push_back(kComparisonKeySeparator);
		expectedDeseret.append(L"3:/");
		expectedDeseret.append(deseretCapital);
		expectedDeseret.push_back(kComparisonKeySeparator);
		expectedDeseret.append(L"0");
		expectedDeseret.push_back(kComparisonKeySeparator);
		expectedDeseret.append(L"0:");
		expectedDeseret.push_back(kComparisonKeySeparator);
		expectedDeseret.append(L"0");
		expectedDeseret.push_back(kComparisonKeySeparator);
		expectedDeseret.append(L"0:");
		expectedDeseret.push_back(kComparisonKeySeparator);
		passed = UriIdentityService::MakeComparisonKey(*dotted.value, ENonFileUriCasePolicy::CaseInsensitive) == expectedDotted
			&& UriIdentityService::MakeComparisonKey(*deseretUpper.value, ENonFileUriCasePolicy::CaseInsensitive) == expectedDeseret;
	}

	const bool restored = std::setlocale(LC_ALL, savedLocale.c_str()) != nullptr;
	return passed && restored;
}

bool RejectsInvalidComponentConstructionExplicitly()
{
	auto invalidScheme = Uri::FromComponents(L"1demo", L"", L"/path");
	auto authorityWithoutMarker = Uri::FromComponents(L"demo", L"host", L"/path");
	auto pathWithoutLeadingSlash = Uri::FromComponents(L"demo", L"host", L"path", std::nullopt, std::nullopt, true);
	auto invalidQuery = Uri::FromComponents(L"demo", L"", L"/path", CodeUnits({0x1f}), std::nullopt, true);
	auto invalidFragment = Uri::FromComponents(L"demo", L"", L"/path", std::nullopt, CodeUnits({0xd800}), true);
	return !invalidScheme && invalidScheme.error == EUriParseError::InvalidScheme
		&& !authorityWithoutMarker && authorityWithoutMarker.error == EUriParseError::InvalidAuthority
		&& !pathWithoutLeadingSlash && pathWithoutLeadingSlash.error == EUriParseError::InvalidPath
		&& !invalidQuery && invalidQuery.error == EUriParseError::InvalidQuery
		&& !invalidFragment && invalidFragment.error == EUriParseError::InvalidFragment;
}

bool RejectsInvalidUriAndWindowsPathInputsExplicitly()
{
	auto noScheme = Uri::Parse(L"C:\\not-a-uri");
	auto malformedPercent = Uri::Parse(L"file:///C:/bad%2");
	auto malformedUtf8 = Uri::Parse(L"file:///C:/bad%E6%97");
	auto relativePath = Uri::FromWindowsPath(L"relative\\file.txt");
	auto devicePath = Uri::FromWindowsPath(L"\\\\.\\pipe\\sakura");
	auto emptyUncShare = Uri::FromWindowsPath(L"\\\\server\\\\file.txt");
	return !noScheme && noScheme.error == EUriParseError::InvalidPath
		&& !malformedPercent && malformedPercent.error == EUriParseError::InvalidPercentEncoding
		&& !malformedUtf8 && malformedUtf8.error == EUriParseError::InvalidUtf8
		&& !relativePath && relativePath.error == EUriParseError::InvalidWindowsPath
		&& !devicePath && devicePath.error == EUriParseError::InvalidWindowsPath
		&& !emptyUncShare && emptyUncShare.error == EUriParseError::InvalidWindowsPath;
}

struct TestCase {
	std::string_view name;
	bool (*run)();
};

constexpr std::array kTests{
	TestCase{"ConvertsDrivePathToCanonicalFileUriAndBackWithoutFilesystemResolution", ConvertsDrivePathToCanonicalFileUriAndBackWithoutFilesystemResolution},
	TestCase{"ConvertsUncPathToFileUriAndBack", ConvertsUncPathToFileUriAndBack},
	TestCase{"ConvertsExtendedWindowsPathsToCanonicalFileUrisAndBack", ConvertsExtendedWindowsPathsToCanonicalFileUrisAndBack},
	TestCase{"ParsesAndSerializesEncodedSpaceAndUnicode", ParsesAndSerializesEncodedSpaceAndUnicode},
	TestCase{"ParsesComponentsAndPreservesDecodedFields", ParsesComponentsAndPreservesDecodedFields},
	TestCase{"EncodesComponentsWithComponentSpecificPercentRules", EncodesComponentsWithComponentSpecificPercentRules},
	TestCase{"SupportsUntitledAndArbitrarySchemes", SupportsUntitledAndArbitrarySchemes},
	TestCase{"PreservesQueryAndFragmentAsDistinctComponents", PreservesQueryAndFragmentAsDistinctComponents},
	TestCase{"PreservesAbsentAndEmptyQueryAndFragmentValues", PreservesAbsentAndEmptyQueryAndFragmentValues},
	TestCase{"RejectsMalformedUtf8AndCesu8PercentRuns", RejectsMalformedUtf8AndCesu8PercentRuns},
	TestCase{"FileIdentityIsCaseInsensitiveWithoutRealPathResolution", FileIdentityIsCaseInsensitiveWithoutRealPathResolution},
	TestCase{"LocalhostAndAuthoritySyntaxShareLocalFileIdentity", LocalhostAndAuthoritySyntaxShareLocalFileIdentity},
	TestCase{"ComparisonKeyIsByteExactAndTracksComponentPresence", ComparisonKeyIsByteExactAndTracksComponentPresence},
	TestCase{"NonFileCasePolicyIsExplicit", NonFileCasePolicyIsExplicit},
	TestCase{"FreezesJapaneseLocaleCaseSentinels", FreezesJapaneseLocaleCaseSentinels},
	TestCase{"RejectsInvalidComponentConstructionExplicitly", RejectsInvalidComponentConstructionExplicitly},
	TestCase{"RejectsInvalidUriAndWindowsPathInputsExplicitly", RejectsInvalidUriAndWindowsPathInputsExplicitly},
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
	std::string_view filter = "UriIdentity.*";
	for (int index = 1; index < argc; ++index) {
		const std::string_view argument = argv[index];
		if (argument == "--gtest_list_tests") {
			std::cout << "UriIdentity.\n";
			for (const auto& test : kTests) std::cout << "  " << test.name << '\n';
			return 0;
		}
		constexpr std::string_view prefix = "--gtest_filter=";
		if (argument.starts_with(prefix)) filter = argument.substr(prefix.size());
	}

	int selected = 0;
	int failed = 0;
	for (const auto& test : kTests) {
		const std::string fullName = "UriIdentity." + std::string(test.name);
		if (!Matches(fullName, filter)) continue;
		++selected;
		const bool passed = test.run();
		std::cout << (passed ? "[       OK ] " : "[  FAILED  ] ") << fullName << '\n';
		if (!passed) ++failed;
	}
	std::cout << "[==========] " << selected << " tests ran; " << failed << " failed.\n";
	return failed == 0 && selected > 0 ? 0 : 1;
}

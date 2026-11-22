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
#include <iomanip>
#include <initializer_list>
#include <iostream>
#include <sstream>
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

#if defined(_MSC_VER) || defined(_UCRT)
constexpr bool kUsesUcrt = true;
#else
constexpr bool kUsesUcrt = false;
#endif

class ScopedLocale final {
public:
	explicit ScopedLocale(const char* name)
	{
		if (const char* previous = std::setlocale(LC_ALL, nullptr)) {
			m_previous = previous;
			m_isSet = std::setlocale(LC_ALL, name) != nullptr;
		}
	}

	ScopedLocale(const ScopedLocale&) = delete;
	ScopedLocale& operator=(const ScopedLocale&) = delete;

	~ScopedLocale() { Restore(); }

	bool IsSet() const { return m_isSet; }

	bool Restore()
	{
		if (!m_isSet) return true;
		if (std::setlocale(LC_ALL, m_previous.c_str()) == nullptr) return false;
		m_isSet = false;
		return true;
	}

private:
	std::string m_previous;
	bool m_isSet = false;
};

std::wstring ExpectedKey(std::initializer_list<std::wstring_view> fields)
{
	std::wstring expected;
	for (const auto field : fields) {
		expected.append(field);
		expected.push_back(kComparisonKeySeparator);
	}
	return expected;
}

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
	ScopedLocale locale("Japanese");
	if (!locale.IsSet()) return false;

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

	const bool restored = locale.Restore();
	return passed && restored;
}

bool FreezesFileAndNonFileKeyFieldsUnderProductionLocale()
{
	ScopedLocale locale("Japanese");
	if (!locale.IsSet()) return false;

	const auto local = Uri::Parse(L"FILE://LocalHost/C:/ReadMe.TXT?Query#Frag");
	const auto unc = Uri::Parse(L"file://Server/Share/ReadMe.TXT");
	const auto nonFile = Uri::Parse(L"DeMo://Host/Path?Query#Frag");
	bool passed = local && unc && nonFile
		&& UriIdentityService::MakeComparisonKey(*local.value) == ExpectedKey({
			L"4:file", L"1", L"0:", L"14:/c:/readme.txt", L"1", L"5:Query", L"1", L"4:Frag"})
		&& UriIdentityService::MakeComparisonKey(*unc.value) == ExpectedKey({
			L"4:file", L"1", L"6:server", L"17:/share/readme.txt", L"0", L"0:", L"0", L"0:"})
		&& UriIdentityService::MakeComparisonKey(*nonFile.value, ENonFileUriCasePolicy::CaseSensitive) == ExpectedKey({
			L"4:demo", L"1", L"4:Host", L"5:/Path", L"1", L"5:Query", L"1", L"4:Frag"})
		&& UriIdentityService::MakeComparisonKey(*nonFile.value, ENonFileUriCasePolicy::CaseInsensitive) == ExpectedKey({
			L"4:demo", L"1", L"4:host", L"5:/path", L"1", L"5:query", L"1", L"4:frag"});

	if constexpr (kUsesUcrt) {
		const auto cyrillic = Uri::Parse(L"file:///C:/\u0414.txt");
		const auto latinCapital = Uri::Parse(L"file:///C:/\u00c9.txt");
		const auto latinSmall = Uri::Parse(L"file:///C:/\u00e9.txt");
		passed = passed && cyrillic && latinCapital && latinSmall
			&& UriIdentityService::MakeComparisonKey(*cyrillic.value) == ExpectedKey({
				L"4:file", L"1", L"0:", L"9:/c:/\u0434.txt", L"0", L"0:", L"0", L"0:"})
			&& UriIdentityService::MakeComparisonKey(*latinCapital.value) == ExpectedKey({
				L"4:file", L"1", L"0:", L"9:/c:/\u00c9.txt", L"0", L"0:", L"0", L"0:"})
			&& !UriIdentityService::IsEqual(*latinCapital.value, *latinSmall.value); // #436
	}

	const bool restored = locale.Restore();
	return passed && restored;
}

void PrintFoldProbe(const char* localeName, std::wstring_view input, std::wstring_view output)
{
	std::ostringstream line;
	line << "[  PROBE   ] crt=other locale=" << localeName << std::uppercase << std::hex << std::setfill('0');
	const auto appendUnits = [&line](std::wstring_view units) {
		if (units.size() == 2 && units[0] >= 0xd800 && units[0] <= 0xdbff
			&& units[1] >= 0xdc00 && units[1] <= 0xdfff) {
			const auto scalar = 0x10000u + (units[0] - 0xd800u) * 0x400u + (units[1] - 0xdc00u);
			line << " U+" << std::setw(4) << scalar;
			return;
		}
		for (const auto unit : units) line << " U+" << std::setw(4) << static_cast<std::uint32_t>(unit);
	};
	appendUnits(input);
	line << " ->";
	appendUnits(output);
	std::cout << line.str() << '\n';
}

bool CheckNonFilePathFold(const char* localeName, std::wstring_view input, std::wstring_view expectedFold)
{
	const auto uri = Uri::Parse(std::wstring{L"demo://host/"}.append(input));
	if (!uri) return false;
	const auto key = UriIdentityService::MakeComparisonKey(*uri.value, ENonFileUriCasePolicy::CaseInsensitive);
	const auto keyPrefix = ExpectedKey({L"4:demo", L"1", L"4:host"}) + (input.size() == 1 ? L"2:/" : L"3:/");
	const auto keySuffix = kComparisonKeySeparator + ExpectedKey({L"0", L"0:", L"0", L"0:"});
	if constexpr (kUsesUcrt) {
		return key == std::wstring{keyPrefix}.append(expectedFold).append(keySuffix);
	} else {
		// The exact length also catches expansion of U+0130.
		if (key.size() != keyPrefix.size() + input.size() + keySuffix.size()
			|| !key.starts_with(keyPrefix) || !key.ends_with(keySuffix)) return false;
		const auto folded = std::wstring_view{key}.substr(keyPrefix.size(), input.size());
		PrintFoldProbe(localeName, input, folded);
		if (input.front() <= 0x7f) return folded == expectedFold;
		if (input.size() == 2) return folded == input;
		return true;
	}
}

bool FreezesProductionLocaleCodeUnitFoldTable()
{
	constexpr char localeName[] = "Japanese";
	ScopedLocale locale(localeName);
	if (!locale.IsSet()) return false;

	struct Fold {
		const std::uint32_t input;
		const std::uint32_t expected;
	};
	constexpr std::array folds{
		Fold{0x0049, 0x0069},
		Fold{0x0178, 0x00ff},
		Fold{0x0391, 0x03b1},
		Fold{0x03a3, 0x03c3},
		Fold{0x0401, 0x0451},
		Fold{0x0414, 0x0434},
		Fold{0x2160, 0x2170},
		Fold{0x24b6, 0x24d0},
		Fold{0x10a0, 0x2d00},
		Fold{0xff21, 0xff41},
		Fold{0xff3a, 0xff5a},
		Fold{0x00c0, 0x00c0},
		Fold{0x00c9, 0x00c9},
		Fold{0x00d6, 0x00d6},
		Fold{0x00de, 0x00de},
		Fold{0x00df, 0x00df},
		Fold{0x0130, 0x0130},
		Fold{0x1e9e, 0x1e9e},
		Fold{0x2126, 0x2126},
		Fold{0x212a, 0x212a},
	};
	bool passed = true;
	for (const auto& fold : folds) {
		if (!CheckNonFilePathFold(localeName, CodeUnits({fold.input}), CodeUnits({fold.expected}))) passed = false;
	}
	const auto deseretCapital = CodeUnits({0xd801, 0xdc00});
	if (!CheckNonFilePathFold(localeName, deseretCapital, deseretCapital)) passed = false;

	const bool restored = locale.Restore();
	return passed && restored;
}

bool RecordsCLocaleFoldDifference()
{
	constexpr char localeName[] = "C";
	ScopedLocale locale(localeName);
	if (!locale.IsSet()) return false;

	bool passed = CheckNonFilePathFold(localeName, L"I", L"i");
	for (const auto unit : {0x0178u, 0x0414u, 0xff21u}) {
		const auto value = CodeUnits({unit});
		if (!CheckNonFilePathFold(localeName, value, value)) passed = false;
	}

	const bool restored = locale.Restore();
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
	TestCase{"FreezesFileAndNonFileKeyFieldsUnderProductionLocale", FreezesFileAndNonFileKeyFieldsUnderProductionLocale},
	TestCase{"FreezesProductionLocaleCodeUnitFoldTable", FreezesProductionLocaleCodeUnitFoldTable},
	TestCase{"RecordsCLocaleFoldDifference", RecordsCLocaleFoldDifference},
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

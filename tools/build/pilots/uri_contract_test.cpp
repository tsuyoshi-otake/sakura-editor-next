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

bool FreezesUnicodeLowercaseSentinels()
{
	ScopedLocale locale("Japanese");
	if (!locale.IsSet()) return false;

	const auto deseretCapital = CodeUnits({0xd801, 0xdc00});
	const auto deseretSmall = CodeUnits({0xd801, 0xdc28});
	const auto dotted = Uri::FromComponents(L"demo", L"\u0130", L"/path", std::nullopt, std::nullopt, true);
	const auto dottedLower = Uri::FromComponents(L"demo", L"i\u0307", L"/path", std::nullopt, std::nullopt, true);
	const auto asciiI = Uri::FromComponents(L"demo", L"i", L"/path", std::nullopt, std::nullopt, true);
	const auto deseretUpper = Uri::FromComponents(L"demo", L"Host", std::wstring{L"/"} + deseretCapital, std::nullopt, std::nullopt, true);
	const auto deseretLower = Uri::FromComponents(L"demo", L"host", std::wstring{L"/"} + deseretSmall, std::nullopt, std::nullopt, true);

	constexpr auto policy = ENonFileUriCasePolicy::CaseInsensitive;
	const bool passed = dotted && dottedLower && asciiI && deseretUpper && deseretLower
		&& UriIdentityService::IsEqual(*dotted.value, *dottedLower.value, policy)
		&& !UriIdentityService::IsEqual(*dotted.value, *asciiI.value, policy)
		&& UriIdentityService::IsEqual(*deseretUpper.value, *deseretLower.value, policy)
		&& UriIdentityService::MakeComparisonKey(*dotted.value, policy) == ExpectedKey({
			L"4:demo", L"1", L"2:i\u0307", L"5:/path", L"0", L"0:", L"0", L"0:"})
		&& UriIdentityService::MakeComparisonKey(*deseretUpper.value, policy) == ExpectedKey({
			L"4:demo", L"1", L"4:host", std::wstring{L"3:/"} + deseretSmall, L"0", L"0:", L"0", L"0:"});

	const bool restored = locale.Restore();
	return passed && restored;
}

bool FreezesFileAndNonFileKeyFields()
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

	const auto cyrillic = Uri::Parse(L"file:///C:/\u0414.txt");
	const auto latinCapital = Uri::Parse(L"file:///C:/\u00c9.txt");
	const auto latinSmall = Uri::Parse(L"file:///C:/\u00e9.txt");
	passed = passed && cyrillic && latinCapital && latinSmall
		&& UriIdentityService::MakeComparisonKey(*cyrillic.value) == ExpectedKey({
			L"4:file", L"1", L"0:", L"9:/c:/\u0434.txt", L"0", L"0:", L"0", L"0:"})
		&& UriIdentityService::MakeComparisonKey(*latinCapital.value) == ExpectedKey({
			L"4:file", L"1", L"0:", L"9:/c:/\u00e9.txt", L"0", L"0:", L"0", L"0:"})
		&& UriIdentityService::IsEqual(*latinCapital.value, *latinSmall.value);

	const bool restored = locale.Restore();
	return passed && restored;
}

bool CheckNonFilePathFold(std::wstring_view input, std::wstring_view expectedFold)
{
	const auto uri = Uri::Parse(std::wstring{L"demo://host/"}.append(input));
	if (!uri) return false;
	const auto key = UriIdentityService::MakeComparisonKey(*uri.value, ENonFileUriCasePolicy::CaseInsensitive);
	const auto pathField = (std::to_wstring(1 + expectedFold.size()) + L":/").append(expectedFold);
	return key == ExpectedKey({L"4:demo", L"1", L"4:host", pathField, L"0", L"0:", L"0", L"0:"});
}

using Fold = std::array<std::wstring_view, 2>;

bool FreezesUnicodeLowercaseFoldTable()
{
	ScopedLocale locale("Japanese");
	if (!locale.IsSet()) return false;

	const auto deseretCapital = CodeUnits({0xd801, 0xdc00});
	const auto deseretSmall = CodeUnits({0xd801, 0xdc28});
	const std::array folds{
		Fold{L"I", L"i"},
		Fold{L"\u0178", L"\u00ff"},
		Fold{L"\u0391", L"\u03b1"},
		Fold{L"\u03a3", L"\u03c3"}, // The preceding slash is not Cased.
		Fold{L"\u0401", L"\u0451"},
		Fold{L"\u0414", L"\u0434"},
		Fold{L"\u2160", L"\u2170"},
		Fold{L"\u24b6", L"\u24d0"},
		Fold{L"\u10a0", L"\u2d00"},
		Fold{L"\uff21", L"\uff41"},
		Fold{L"\uff3a", L"\uff5a"},
		Fold{L"\u00c0", L"\u00e0"},
		Fold{L"\u00c9", L"\u00e9"},
		Fold{L"\u00d6", L"\u00f6"},
		Fold{L"\u00de", L"\u00fe"},
		Fold{L"\u00df", L"\u00df"},
		Fold{L"\u0130", L"i\u0307"},
		Fold{L"\u1e9e", L"\u00df"},
		Fold{L"\u2126", L"\u03c9"},
		Fold{L"\u212a", L"k"},
		Fold{L"\u01c5", L"\u01c6"},
		Fold{L"\u13a0", L"\uab70"},
		Fold{L"\uab70", L"\uab70"},
		Fold{L"\u1c90", L"\u10d0"},
		Fold{deseretCapital, deseretSmall},
	};
	bool passed = true;
	for (const auto& [input, expected] : folds) {
		if (!CheckNonFilePathFold(input, expected)) passed = false;
	}

	const bool restored = locale.Restore();
	return passed && restored;
}

bool FoldIgnoresTheProcessLocale()
{
	constexpr std::array folds{
		Fold{L"I", L"i"},
		Fold{L"\u0178", L"\u00ff"},
		Fold{L"\u0414", L"\u0434"},
		Fold{L"\uff21", L"\uff41"},
		Fold{L"\u00c9", L"\u00e9"},
	};

	for (const auto* localeName : {"C", "Japanese"}) {
		ScopedLocale locale(localeName);
		if (!locale.IsSet()) return false;
		bool passed = true;
		for (const auto& [input, expected] : folds) {
			if (!CheckNonFilePathFold(input, expected)) passed = false;
		}
		const bool restored = locale.Restore();
		if (!passed || !restored) return false;
	}
	return true;
}

bool AppliesFinalSigmaContextLikeVsCode()
{
	constexpr std::array folds{
		Fold{L"/\u0391\u03a3", L"/\u03b1\u03c2"},
		Fold{L"/\u0391\u03a3.txt", L"/\u03b1\u03c3.txt"},
		Fold{L"/\u0391\u03a3/x", L"/\u03b1\u03c2/x"},
		Fold{L"/\u03a3", L"/\u03c3"},
		Fold{L"/\u0391\u03a3\u0391", L"/\u03b1\u03c3\u03b1"},
		Fold{L"/\u0391'\u03a3", L"/\u03b1'\u03c2"},
		Fold{L"/\u0391\u0301\u03a3", L"/\u03b1\u0301\u03c2"},
		Fold{L"/\u0391\u03a3\u0301\u0391", L"/\u03b1\u03c3\u0301\u03b1"},
		Fold{L"/\u0391\u03a3\u0301", L"/\u03b1\u03c2\u0301"},
	};
	for (const auto& [path, expected] : folds) {
		const auto uri = Uri::FromComponents(L"demo", L"host", std::wstring{path}, std::nullopt, std::nullopt, true);
		if (!uri) return false;
		const auto pathField = (std::to_wstring(expected.size()) + L":").append(expected);
		const auto key = UriIdentityService::MakeComparisonKey(*uri.value, ENonFileUriCasePolicy::CaseInsensitive);
		if (key != ExpectedKey({L"4:demo", L"1", L"4:host", pathField, L"0", L"0:", L"0", L"0:"})) return false;
	}

	const auto file = Uri::Parse(L"file:///C:/\u0391\u03a3.txt");
	return file && UriIdentityService::MakeComparisonKey(*file.value) == ExpectedKey({
		L"4:file", L"1", L"0:", L"10:/c:/\u03b1\u03c3.txt", L"0", L"0:", L"0", L"0:"});
}

bool FileIdentityFoldsLatin1AndSupplementaryCapitals()
{
	// VS Code ExtUri.getComparisonKey uses uri.path.toLowerCase() (microsoft/vscode 733a99842b0e).
	const auto parseFile = [](std::wstring_view name) {
		return Uri::Parse(std::wstring{L"file:///C:/"}.append(name).append(L".txt"));
	};
	const auto deseretCapital = CodeUnits({0xd801, 0xdc00});
	const auto deseretSmall = CodeUnits({0xd801, 0xdc28});
	const std::array folds{
		Fold{L"\u00c9", L"\u00e9"},
		Fold{L"\u212a", L"k"},
		Fold{L"\u2126", L"\u03c9"},
		Fold{deseretCapital, deseretSmall},
		Fold{L"\u0130", L"i\u0307"},
		Fold{L"\u1e9e", L"\u00df"},
	};
	for (const auto& [input, expected] : folds) {
		const auto upper = parseFile(input);
		const auto lower = parseFile(expected);
		if (!upper || !lower || !UriIdentityService::IsEqual(*upper.value, *lower.value)) return false;
	}

	const auto dotted = parseFile(L"\u0130");
	const auto asciiI = parseFile(L"i");
	const auto capitalSharpS = parseFile(L"\u1e9e");
	const auto doubleS = parseFile(L"ss");
	return dotted && asciiI && capitalSharpS && doubleS
		&& !UriIdentityService::IsEqual(*dotted.value, *asciiI.value)
		&& !UriIdentityService::IsEqual(*capitalSharpS.value, *doubleS.value);
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
	TestCase{"FreezesUnicodeLowercaseSentinels", FreezesUnicodeLowercaseSentinels},
	TestCase{"FreezesFileAndNonFileKeyFields", FreezesFileAndNonFileKeyFields},
	TestCase{"FreezesUnicodeLowercaseFoldTable", FreezesUnicodeLowercaseFoldTable},
	TestCase{"FoldIgnoresTheProcessLocale", FoldIgnoresTheProcessLocale},
	TestCase{"AppliesFinalSigmaContextLikeVsCode", AppliesFinalSigmaContextLikeVsCode},
	TestCase{"FileIdentityFoldsLatin1AndSupplementaryCapitals", FileIdentityFoldsLatin1AndSupplementaryCapitals},
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

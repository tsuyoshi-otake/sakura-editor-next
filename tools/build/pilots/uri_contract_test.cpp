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
#include <iostream>
#include <string>
#include <string_view>

namespace {

using namespace platform::uri;

bool ConvertsDrivePathToCanonicalFileUriAndBackWithoutFilesystemResolution()
{
	auto uri = Uri::FromWindowsPath(L"C:\\Work Files\\日本語.txt");
	if (!uri || uri.value->ToString() != L"file:///C:/Work%20Files/%E6%97%A5%E6%9C%AC%E8%AA%9E.txt") return false;
	auto path = uri.value->ToWindowsPath();
	return path && *path.value == L"C:\\Work Files\\日本語.txt";
}

bool ConvertsUncPathToFileUriAndBack()
{
	auto uri = Uri::FromWindowsPath(L"\\\\server\\share\\folder\\note.txt");
	if (!uri || uri.value->ToString() != L"file://server/share/folder/note.txt") return false;
	auto path = uri.value->ToWindowsPath();
	return path && *path.value == L"\\\\server\\share\\folder\\note.txt";
}

bool ParsesAndSerializesEncodedSpaceAndUnicode()
{
	auto uri = Uri::Parse(L"file:///C:/Work%20Files/%E6%97%A5%E6%9C%AC.txt");
	return uri && uri.value->Path() == L"/C:/Work Files/日本.txt"
		&& uri.value->ToString() == L"file:///C:/Work%20Files/%E6%97%A5%E6%9C%AC.txt";
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

bool PreservesQueryAndFragmentAsDistinctComponents()
{
	auto uri = Uri::Parse(L"demo://host/path?line=3%20and%204#section%202");
	return uri && uri.value->Query() && uri.value->Fragment()
		&& *uri.value->Query() == L"line=3 and 4" && *uri.value->Fragment() == L"section 2"
		&& uri.value->ToString() == L"demo://host/path?line=3%20and%204#section%202";
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

bool NonFileCasePolicyIsExplicit()
{
	auto first = Uri::Parse(L"demo://Host/Path?Name=Value#Anchor");
	auto second = Uri::Parse(L"demo://host/path?name=value#anchor");
	return first && second && !UriIdentityService::IsEqual(*first.value, *second.value)
		&& UriIdentityService::IsEqual(*first.value, *second.value, ENonFileUriCasePolicy::CaseInsensitive);
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
	TestCase{"ParsesAndSerializesEncodedSpaceAndUnicode", ParsesAndSerializesEncodedSpaceAndUnicode},
	TestCase{"SupportsUntitledAndArbitrarySchemes", SupportsUntitledAndArbitrarySchemes},
	TestCase{"PreservesQueryAndFragmentAsDistinctComponents", PreservesQueryAndFragmentAsDistinctComponents},
	TestCase{"FileIdentityIsCaseInsensitiveWithoutRealPathResolution", FileIdentityIsCaseInsensitiveWithoutRealPathResolution},
	TestCase{"LocalhostAndAuthoritySyntaxShareLocalFileIdentity", LocalhostAndAuthoritySyntaxShareLocalFileIdentity},
	TestCase{"NonFileCasePolicyIsExplicit", NonFileCasePolicyIsExplicit},
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

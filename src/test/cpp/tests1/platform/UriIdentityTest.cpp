/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/

#include "pch.h"

#include "platform/uri/UriIdentity.h"

using namespace platform::uri;

TEST(UriIdentity, ConvertsDrivePathToCanonicalFileUriAndBackWithoutFilesystemResolution)
{
	auto uri = Uri::FromWindowsPath(L"C:\\Work Files\\日本語.txt");
	ASSERT_TRUE(uri);
	EXPECT_EQ(L"file:///C:/Work%20Files/%E6%97%A5%E6%9C%AC%E8%AA%9E.txt", uri.value->ToString());

	auto path = uri.value->ToWindowsPath();
	ASSERT_TRUE(path);
	EXPECT_EQ(L"C:\\Work Files\\日本語.txt", *path.value);
}

TEST(UriIdentity, ConvertsUncPathToFileUriAndBack)
{
	auto uri = Uri::FromWindowsPath(L"\\\\server\\share\\folder\\note.txt");
	ASSERT_TRUE(uri);
	EXPECT_EQ(L"file://server/share/folder/note.txt", uri.value->ToString());

	auto path = uri.value->ToWindowsPath();
	ASSERT_TRUE(path);
	EXPECT_EQ(L"\\\\server\\share\\folder\\note.txt", *path.value);
}

TEST(UriIdentity, ParsesAndSerializesEncodedSpaceAndUnicode)
{
	auto uri = Uri::Parse(L"file:///C:/Work%20Files/%E6%97%A5%E6%9C%AC.txt");
	ASSERT_TRUE(uri);
	EXPECT_EQ(L"/C:/Work Files/日本.txt", uri.value->Path());
	EXPECT_EQ(L"file:///C:/Work%20Files/%E6%97%A5%E6%9C%AC.txt", uri.value->ToString());
}

TEST(UriIdentity, SupportsUntitledAndArbitrarySchemes)
{
	auto untitled = Uri::Parse(L"untitled:Untitled-1");
	ASSERT_TRUE(untitled);
	EXPECT_EQ(L"untitled", untitled.value->Scheme());
	EXPECT_EQ(L"Untitled-1", untitled.value->Path());
	EXPECT_EQ(L"untitled:Untitled-1", untitled.value->ToString());

	auto extension = Uri::Parse(L"git+demo://Example.Test/repository/item");
	ASSERT_TRUE(extension);
	EXPECT_EQ(L"git+demo", extension.value->Scheme());
	EXPECT_EQ(L"Example.Test", extension.value->Authority());
	EXPECT_EQ(L"/repository/item", extension.value->Path());
}

TEST(UriIdentity, PreservesQueryAndFragmentAsDistinctComponents)
{
	auto uri = Uri::Parse(L"demo://host/path?line=3%20and%204#section%202");
	ASSERT_TRUE(uri);
	ASSERT_TRUE(uri.value->Query());
	ASSERT_TRUE(uri.value->Fragment());
	EXPECT_EQ(L"line=3 and 4", *uri.value->Query());
	EXPECT_EQ(L"section 2", *uri.value->Fragment());
	EXPECT_EQ(L"demo://host/path?line=3%20and%204#section%202", uri.value->ToString());
}

TEST(UriIdentity, FileIdentityIsCaseInsensitiveWithoutRealPathResolution)
{
	auto first = Uri::Parse(L"file:///C:/Workspace/ReadMe.md");
	auto second = Uri::Parse(L"FILE:///c:/workspace/readme.md");
	auto changedQuery = Uri::Parse(L"file:///c:/workspace/readme.md?view=Preview");
	ASSERT_TRUE(first && second && changedQuery);
	EXPECT_TRUE(UriIdentityService::IsEqual(*first.value, *second.value));
	EXPECT_FALSE(UriIdentityService::IsEqual(*first.value, *changedQuery.value));
}

TEST(UriIdentity, LocalhostAndAuthoritySyntaxShareLocalFileIdentity)
{
	auto canonical = Uri::Parse(L"file:///C:/Workspace/ReadMe.md");
	auto localhost = Uri::Parse(L"file://localhost/c:/workspace/readme.md");
	auto authorityLess = Uri::Parse(L"file:/c:/workspace/readme.md");
	ASSERT_TRUE(canonical && localhost && authorityLess);
	EXPECT_TRUE(UriIdentityService::IsEqual(*canonical.value, *localhost.value));
	EXPECT_TRUE(UriIdentityService::IsEqual(*canonical.value, *authorityLess.value));
}

TEST(UriIdentity, NonFileCasePolicyIsExplicit)
{
	auto first = Uri::Parse(L"demo://Host/Path?Name=Value#Anchor");
	auto second = Uri::Parse(L"demo://host/path?name=value#anchor");
	ASSERT_TRUE(first && second);
	EXPECT_FALSE(UriIdentityService::IsEqual(*first.value, *second.value));
	EXPECT_TRUE(UriIdentityService::IsEqual(*first.value, *second.value, ENonFileUriCasePolicy::CaseInsensitive));
}

TEST(UriIdentity, RejectsInvalidUriAndWindowsPathInputsExplicitly)
{
	auto noScheme = Uri::Parse(L"C:\\not-a-uri");
	EXPECT_FALSE(noScheme);
	EXPECT_EQ(EUriParseError::InvalidPath, noScheme.error);

	auto malformedPercent = Uri::Parse(L"file:///C:/bad%2");
	EXPECT_FALSE(malformedPercent);
	EXPECT_EQ(EUriParseError::InvalidPercentEncoding, malformedPercent.error);

	auto malformedUtf8 = Uri::Parse(L"file:///C:/bad%E6%97");
	EXPECT_FALSE(malformedUtf8);
	EXPECT_EQ(EUriParseError::InvalidUtf8, malformedUtf8.error);

	auto relativePath = Uri::FromWindowsPath(L"relative\\file.txt");
	EXPECT_FALSE(relativePath);
	EXPECT_EQ(EUriParseError::InvalidWindowsPath, relativePath.error);

	auto devicePath = Uri::FromWindowsPath(L"\\\\.\\pipe\\sakura");
	EXPECT_FALSE(devicePath);
	EXPECT_EQ(EUriParseError::InvalidWindowsPath, devicePath.error);

	auto emptyUncShare = Uri::FromWindowsPath(L"\\\\server\\\\file.txt");
	EXPECT_FALSE(emptyUncShare);
	EXPECT_EQ(EUriParseError::InvalidWindowsPath, emptyUncShare.error);
}

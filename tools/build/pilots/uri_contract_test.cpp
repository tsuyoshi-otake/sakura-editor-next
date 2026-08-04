/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/

#include "platform/uri/UriIdentity.h"

namespace {

int Require(bool condition, int failureCode) noexcept
{
	return condition ? 0 : failureCode;
}

} // namespace

int main()
{
	using namespace platform::uri;

	auto uri = Uri::FromWindowsPath(L"C:\\Work Files\\日本語.txt");
	if (const int failure = Require(static_cast<bool>(uri), 1)) return failure;
	if (const int failure = Require(uri.value->ToString() == L"file:///C:/Work%20Files/%E6%97%A5%E6%9C%AC%E8%AA%9E.txt", 2)) return failure;

	auto path = uri.value->ToWindowsPath();
	if (const int failure = Require(static_cast<bool>(path), 3)) return failure;
	if (const int failure = Require(*path.value == L"C:\\Work Files\\日本語.txt", 4)) return failure;

	auto equivalent = Uri::Parse(L"FILE:///c:/work%20files/%E6%97%A5%E6%9C%AC%E8%AA%9E.txt");
	if (const int failure = Require(static_cast<bool>(equivalent), 5)) return failure;
	if (const int failure = Require(UriIdentityService::IsEqual(*uri.value, *equivalent.value), 6)) return failure;

	auto invalid = Uri::Parse(L"file:///C:/bad%2");
	if (const int failure = Require(!invalid && invalid.error == EUriParseError::InvalidPercentEncoding, 7)) return failure;

	return 0;
}

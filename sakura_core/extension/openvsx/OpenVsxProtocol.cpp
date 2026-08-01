/*! @file
 *
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#include "StdAfx.h"
#include "extension/openvsx/OpenVsxProtocol.h"

#include <picojson/picojson.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

#include "util/string_ex.h"

namespace {

std::wstring UrlEncode(std::wstring_view input)
{
	const std::string utf8 = wcstou8s(std::wstring(input));
	std::wstring encoded;
	encoded.reserve(utf8.size() * 3);

	for (const char character : utf8) {
		const auto byte = static_cast<unsigned char>(character);
		const bool unreserved =
			(byte >= 'A' && byte <= 'Z') ||
			(byte >= 'a' && byte <= 'z') ||
			(byte >= '0' && byte <= '9') ||
			byte == '-' || byte == '_' || byte == '.' || byte == '~';
		if (unreserved) {
			encoded.push_back(static_cast<wchar_t>(byte));
		}
		else {
			constexpr wchar_t hex[] = L"0123456789ABCDEF";
			encoded.push_back(L'%');
			encoded.push_back(hex[byte >> 4]);
			encoded.push_back(hex[byte & 0x0F]);
		}
	}
	return encoded;
}

std::wstring GetWString(const picojson::object& object, const char* key)
{
	const auto it = object.find(key);
	if (it == object.end() || !it->second.is<std::string>()) return {};
	return u8stowcs(it->second.get<std::string>());
}

double GetNumber(const picojson::object& object, const char* key, double defaultValue)
{
	const auto it = object.find(key);
	if (it == object.end() || !it->second.is<double>()) return defaultValue;
	return it->second.get<double>();
}

//! 非信頼 JSON の数値を、未定義の浮動小数点→整数変換を起こさず非負整数へ収める。
template<typename Integer>
Integer GetNonNegativeInteger(const picojson::object& object, const char* key)
{
	const double value = GetNumber(object, key, 0.0);
	if (!std::isfinite(value) || value <= 0.0) return 0;
	constexpr Integer upper = (std::numeric_limits<Integer>::max)();
	if (value >= static_cast<double>(upper)) return upper;
	return static_cast<Integer>(std::floor(value));
}

bool GetBool(const picojson::object& object, const char* key, bool defaultValue)
{
	const auto it = object.find(key);
	if (it == object.end() || !it->second.is<bool>()) return defaultValue;
	return it->second.get<bool>();
}

const picojson::object* GetObject(const picojson::object& object, const char* key)
{
	const auto it = object.find(key);
	if (it == object.end() || !it->second.is<picojson::object>()) return nullptr;
	return &it->second.get<picojson::object>();
}

} // namespace

namespace extension::openvsx {

std::wstring OpenVsxProtocol::NormalizeRegistryUrl(std::wstring registryUrl)
{
	while (!registryUrl.empty() && registryUrl.back() == L'/') registryUrl.pop_back();
	return registryUrl;
}

std::wstring OpenVsxProtocol::BuildSearchUrl(std::wstring_view registryUrl, std::wstring_view query, int offset, int pageSize)
{
	const int clampedOffset = (std::max)(0, offset);
	const int clampedPageSize = std::clamp(pageSize, 1, kMaxPageSize);
	std::wstring url = std::wstring(registryUrl) + L"/api/-/search?offset=" + std::to_wstring(clampedOffset)
		+ L"&size=" + std::to_wstring(clampedPageSize);
	if (query.empty()) {
		url += L"&sortBy=downloadCount&sortOrder=desc";
	}
	else {
		url += L"&query=" + UrlEncode(query);
	}
	return url;
}

bool OpenVsxProtocol::ParseSearchResponse(const std::string& json, SOpenVsxSearchResult& result, std::wstring& errorMsg)
{
	result = SOpenVsxSearchResult();
	errorMsg.clear();

	picojson::value root;
	if (const std::string parseError = picojson::parse(root, json); !parseError.empty()) {
		errorMsg = L"invalid JSON: " + u8stowcs(parseError);
		return false;
	}
	if (!root.is<picojson::object>()) {
		errorMsg = L"invalid JSON: root is not an object";
		return false;
	}

	const picojson::object& rootObject = root.get<picojson::object>();
	result.nOffset = GetNonNegativeInteger<int>(rootObject, "offset");
	result.nTotalSize = GetNonNegativeInteger<int>(rootObject, "totalSize");

	const auto extensionsIt = rootObject.find("extensions");
	if (extensionsIt == rootObject.end()) return true;
	if (!extensionsIt->second.is<picojson::array>()) {
		errorMsg = L"invalid JSON: 'extensions' is not an array";
		return false;
	}

	const picojson::array& extensions = extensionsIt->second.get<picojson::array>();
	if (extensions.size() > static_cast<size_t>(kMaxPageSize)) {
		errorMsg = L"invalid JSON: too many extensions in one response";
		return false;
	}
	result.extensions.reserve(extensions.size());

	for (const picojson::value& item : extensions) {
		if (!item.is<picojson::object>()) continue;
		const picojson::object& extensionObject = item.get<picojson::object>();

		SOpenVsxExtension extension;
		extension.sNamespace = GetWString(extensionObject, "namespace");
		extension.sName = GetWString(extensionObject, "name");
		extension.sVersion = GetWString(extensionObject, "version");
		extension.sDisplayName = GetWString(extensionObject, "displayName");
		extension.sDescription = GetWString(extensionObject, "description");
		extension.nDownloadCount = GetNonNegativeInteger<long long>(extensionObject, "downloadCount");
		const double averageRating = GetNumber(extensionObject, "averageRating", -1.0);
		extension.dAverageRating = std::isfinite(averageRating) ? averageRating : -1.0;
		extension.bVerified = GetBool(extensionObject, "verified", false);
		extension.bDeprecated = GetBool(extensionObject, "deprecated", false);

		if (const picojson::object* files = GetObject(extensionObject, "files"); files) {
			extension.sDownloadUrl = GetWString(*files, "download");
			extension.sSha256Url = GetWString(*files, "sha256");
			extension.sIconUrl = GetWString(*files, "icon");
			extension.sReadmeUrl = GetWString(*files, "readme");
			extension.sChangelogUrl = GetWString(*files, "changelog");
		}

		if (extension.sNamespace.empty() || extension.sName.empty() || extension.sVersion.empty() || extension.sDownloadUrl.empty()) continue;
		if (extension.sDisplayName.empty()) extension.sDisplayName = extension.sName;
		result.extensions.push_back(std::move(extension));
	}
	return true;
}

} // namespace extension::openvsx

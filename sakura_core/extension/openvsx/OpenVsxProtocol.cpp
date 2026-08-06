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

/*!
 * @brief URI の path segment としてそのまま置ける識別子か
 *
 * 拡張の名前空間名と拡張名は Open VSX 側の一意キーなので、encode して形を
 * 変えると別の拡張を要求することになる。encode せずに済む文字だけを許し、
 * それ以外は呼び出し元に拒否させる。
 */
bool IsSafePathSegment(std::wstring_view segment)
{
	if (segment.empty()) return false;
	if (segment == L"." || segment == L"..") return false;
	for (const wchar_t character : segment) {
		const bool allowed =
			(character >= L'A' && character <= L'Z') ||
			(character >= L'a' && character <= L'z') ||
			(character >= L'0' && character <= L'9') ||
			character == L'-' || character == L'_' || character == L'.' || character == L'~';
		if (!allowed) return false;
	}
	return true;
}

//! downloads に載り得る targetPlatform の数の上限。VS Code の語彙は十数種しかない。
constexpr size_t kMaxTargetPlatformCount = 64;

} // namespace

namespace extension::openvsx {

std::wstring OpenVsxProtocol::NormalizeRegistryUrl(std::wstring registryUrl)
{
	while (!registryUrl.empty() && registryUrl.back() == L'/') registryUrl.pop_back();
	return registryUrl;
}

std::wstring OpenVsxProtocol::BuildSearchUrl(
	std::wstring_view registryUrl,
	std::wstring_view query,
	int offset,
	int pageSize,
	std::wstring_view targetPlatform)
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
	if (!targetPlatform.empty()) {
		url += L"&targetPlatform=" + UrlEncode(targetPlatform);
	}
	return url;
}

std::wstring OpenVsxProtocol::BuildExtensionMetadataUrl(
	std::wstring_view registryUrl,
	std::wstring_view namespaceName,
	std::wstring_view extensionName)
{
	if (!IsSafePathSegment(namespaceName) || !IsSafePathSegment(extensionName)) return {};
	return std::wstring(registryUrl) + L"/api/" + std::wstring(namespaceName) + L"/" + std::wstring(extensionName);
}

bool OpenVsxProtocol::ParseExtensionMetadataResponse(
	const std::string& json,
	SOpenVsxExtensionAssets& assets,
	std::wstring& errorMsg)
{
	assets = SOpenVsxExtensionAssets();
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

	// レジストリは見つからない拡張にも 200 と error 本文を返し得るので、
	// HTTP status だけでは足りない。error があれば失敗として扱う。
	if (const std::wstring error = GetWString(rootObject, "error"); !error.empty()) {
		errorMsg = error;
		return false;
	}

	assets.sVersion = GetWString(rootObject, "version");
	assets.sTargetPlatform = GetWString(rootObject, "targetPlatform");

	if (const picojson::object* files = GetObject(rootObject, "files"); files) {
		assets.sDownloadUrl = GetWString(*files, "download");
		assets.sSha256Url = GetWString(*files, "sha256");
		assets.sIconUrl = GetWString(*files, "icon");
		assets.sReadmeUrl = GetWString(*files, "readme");
		assets.sChangelogUrl = GetWString(*files, "changelog");
	}

	if (const picojson::object* downloads = GetObject(rootObject, "downloads"); downloads) {
		if (downloads->size() > kMaxTargetPlatformCount) {
			errorMsg = L"invalid JSON: too many target platforms in one response";
			return false;
		}
		assets.downloads.reserve(downloads->size());
		for (const auto& [platform, url] : *downloads) {
			if (!url.is<std::string>()) continue;
			std::wstring downloadUrl = u8stowcs(url.get<std::string>());
			if (platform.empty() || downloadUrl.empty()) continue;
			assets.downloads.emplace_back(u8stowcs(platform), std::move(downloadUrl));
		}
	}

	if (assets.sVersion.empty() && assets.sDownloadUrl.empty() && assets.downloads.empty()) {
		errorMsg = L"invalid JSON: response carries no distribution";
		return false;
	}
	return true;
}

std::wstring OpenVsxProtocol::SelectPlatformDownloadUrl(
	const SOpenVsxExtensionAssets& assets,
	std::wstring_view targetPlatform)
{
	// downloads を持たない古い応答では、files が唯一の配布物を指している。
	// その targetPlatform が合っているときだけ採用する。
	if (assets.downloads.empty()) {
		if (assets.sDownloadUrl.empty()) return {};
		if (assets.sTargetPlatform.empty()
			|| assets.sTargetPlatform == targetPlatform
			|| assets.sTargetPlatform == kUniversalTargetPlatform) {
			return assets.sDownloadUrl;
		}
		return {};
	}

	for (const auto& [platform, url] : assets.downloads) {
		if (platform == targetPlatform) return url;
	}
	for (const auto& [platform, url] : assets.downloads) {
		if (platform == kUniversalTargetPlatform) return url;
	}
	return {};
}

std::wstring OpenVsxProtocol::DeriveSha256Url(std::wstring_view vsixUrl)
{
	constexpr std::wstring_view vsixSuffix = L".vsix";
	if (vsixUrl.size() <= vsixSuffix.size()) return {};
	if (vsixUrl.substr(vsixUrl.size() - vsixSuffix.size()) != vsixSuffix) return {};
	return std::wstring(vsixUrl.substr(0, vsixUrl.size() - vsixSuffix.size())) + L".sha256";
}

std::wstring OpenVsxProtocol::TargetPlatformFromVsixUrl(std::wstring_view vsixUrl)
{
	constexpr std::wstring_view vsixSuffix = L".vsix";
	if (vsixUrl.size() <= vsixSuffix.size()) return {};
	if (vsixUrl.substr(vsixUrl.size() - vsixSuffix.size()) != vsixSuffix) return {};
	vsixUrl.remove_suffix(vsixSuffix.size());

	// '@' はファイル名の中だけを見る。path や query 側の '@' を拾わない。
	const size_t nameBegin = vsixUrl.find_last_of(L"/?#");
	const std::wstring_view fileName =
		(nameBegin == std::wstring_view::npos) ? vsixUrl : vsixUrl.substr(nameBegin + 1);
	const size_t at = fileName.rfind(L'@');
	if (at == std::wstring_view::npos) return {};

	const std::wstring_view platform = fileName.substr(at + 1);
	if (platform.empty() || platform.size() > 32) return {};
	for (const wchar_t character : platform) {
		const bool allowed =
			(character >= L'a' && character <= L'z') ||
			(character >= L'0' && character <= L'9') ||
			character == L'-';
		if (!allowed) return {};
	}
	return std::wstring(platform);
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
		extension.sTargetPlatform = GetWString(extensionObject, "targetPlatform");

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

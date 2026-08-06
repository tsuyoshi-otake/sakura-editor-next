/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"

#include "update/UpdateFeed.h"

#include <sakura/serialization/JsoncDocument.h>

#include <variant>

namespace update {
namespace {

using platform::serialization::CJsoncDocument;
using platform::serialization::EJsoncDiagnosticCode;
using platform::serialization::JsoncValue;

constexpr std::wstring_view kGitHubHost = L"github.com";
constexpr std::size_t kMaximumOwnerLength = 128;
constexpr std::size_t kMaximumRepositoryLength = 128;
constexpr std::size_t kSha256HexLength = 64;

std::wstring ToLowerAscii(std::wstring_view value)
{
	std::wstring result(value);
	for (auto& character : result) {
		if (character >= L'A' && character <= L'Z') character = static_cast<wchar_t>(character - L'A' + L'a');
	}
	return result;
}

bool EqualsIgnoringAsciiCase(std::wstring_view left, std::wstring_view right)
{
	return left.size() == right.size() && ToLowerAscii(left) == ToLowerAscii(right);
}

bool StartsWithIgnoringAsciiCase(std::wstring_view value, std::wstring_view prefix)
{
	return value.size() >= prefix.size() && EqualsIgnoringAsciiCase(value.substr(0, prefix.size()), prefix);
}

//! A path segment usable as an owner or repository name. Deliberately narrow:
//! this string is spliced into a URL, so anything that could change the URL's
//! shape (a slash, a dot segment, a query, a control character) is refused.
bool IsSafePathSegment(std::wstring_view value, std::size_t maximumLength) noexcept
{
	if (value.empty() || value.size() > maximumLength) return false;
	if (value == L"." || value == L"..") return false;
	for (const auto character : value) {
		const bool allowed = (character >= L'A' && character <= L'Z')
			|| (character >= L'a' && character <= L'z')
			|| (character >= L'0' && character <= L'9')
			|| character == L'-' || character == L'_' || character == L'.';
		if (!allowed) return false;
	}
	return true;
}

std::wstring_view StripGitSuffix(std::wstring_view value)
{
	constexpr std::wstring_view suffix = L".git";
	if (value.size() > suffix.size() && EqualsIgnoringAsciiCase(value.substr(value.size() - suffix.size()), suffix)) {
		return value.substr(0, value.size() - suffix.size());
	}
	return value;
}

std::optional<GitHubRepositoryRef> SplitOwnerAndRepository(std::wstring_view path)
{
	while (!path.empty() && path.front() == L'/') path.remove_prefix(1);
	while (!path.empty() && path.back() == L'/') path.remove_suffix(1);
	const auto separator = path.find(L'/');
	if (separator == std::wstring_view::npos) return std::nullopt;
	const auto owner = path.substr(0, separator);
	const auto repository = StripGitSuffix(path.substr(separator + 1));
	if (repository.find(L'/') != std::wstring_view::npos) return std::nullopt;
	if (!IsSafePathSegment(owner, kMaximumOwnerLength)) return std::nullopt;
	if (!IsSafePathSegment(repository, kMaximumRepositoryLength)) return std::nullopt;
	return GitHubRepositoryRef{ std::wstring(owner), std::wstring(repository) };
}

const JsoncValue::Object* AsObject(const JsoncValue& value) noexcept
{
	return std::get_if<JsoncValue::Object>(&value.Value());
}

const JsoncValue::Array* AsArray(const JsoncValue& value) noexcept
{
	return std::get_if<JsoncValue::Array>(&value.Value());
}

const JsoncValue* Member(const JsoncValue::Object& object, std::wstring_view key) noexcept
{
	const auto found = object.find(key);
	return found == object.end() ? nullptr : &found->second;
}

std::wstring StringMember(const JsoncValue::Object& object, std::wstring_view key)
{
	const auto* value = Member(object, key);
	if (value == nullptr) return {};
	const auto* text = std::get_if<std::wstring>(&value->Value());
	return text == nullptr ? std::wstring{} : *text;
}

//! Absent and `null` both mean "not set". A member of the wrong type is a
//! malformed feed, which the caller turns into `InvalidResponse`.
std::optional<bool> BoolMember(const JsoncValue::Object& object, std::wstring_view key, bool& malformed) noexcept
{
	const auto* value = Member(object, key);
	if (value == nullptr || std::holds_alternative<std::monostate>(value->Value())) return std::nullopt;
	const auto* flag = std::get_if<bool>(&value->Value());
	if (flag == nullptr) { malformed = true; return std::nullopt; }
	return *flag;
}

std::optional<std::uint64_t> UnsignedMember(const JsoncValue::Object& object, std::wstring_view key, bool& malformed) noexcept
{
	const auto* value = Member(object, key);
	if (value == nullptr || std::holds_alternative<std::monostate>(value->Value())) return std::nullopt;
	const auto* number = std::get_if<std::int64_t>(&value->Value());
	if (number == nullptr || *number < 0) { malformed = true; return std::nullopt; }
	return static_cast<std::uint64_t>(*number);
}

//! `sha256:<64 lowercase hex>`. Any other algorithm, length, or casing yields
//! no digest at all rather than one the verifier would compare wrongly.
std::optional<std::wstring> ParseAssetDigest(std::wstring_view digest)
{
	constexpr std::wstring_view prefix = L"sha256:";
	if (!StartsWithIgnoringAsciiCase(digest, prefix)) return std::nullopt;
	const auto hex = ToLowerAscii(digest.substr(prefix.size()));
	if (hex.size() != kSha256HexLength) return std::nullopt;
	for (const auto character : hex) {
		const bool isHex = (character >= L'0' && character <= L'9') || (character >= L'a' && character <= L'f');
		if (!isHex) return std::nullopt;
	}
	return hex;
}

struct ReleaseCandidate final {
	UpdateVersion version;
	const JsoncValue::Object* release = nullptr;
};

//! One release object, screened against upstream's two exclusion flags and the
//! tag grammar. A release we cannot name a version for is skipped, not guessed.
std::optional<ReleaseCandidate> ScreenRelease(const JsoncValue& value, bool& malformed)
{
	const auto* release = AsObject(value);
	if (release == nullptr) { malformed = true; return std::nullopt; }
	if (BoolMember(*release, L"draft", malformed).value_or(false)) return std::nullopt;
	if (BoolMember(*release, L"prerelease", malformed).value_or(false)) return std::nullopt;
	if (malformed) return std::nullopt;
	const auto version = ParseReleaseTag(StringMember(*release, L"tag_name"));
	if (!version) return std::nullopt;
	return ReleaseCandidate{ *version, release };
}

//! Finds the asset whose name is exactly the installer this build's own
//! packaging would have produced for that version. An exact name is available
//! because the release tag already fixes every field of it, so no pattern
//! matching — and therefore no chance of selecting the wrong architecture's
//! payload — is involved.
UpdateFeedResult BuildUpdate(const ReleaseCandidate& candidate, const UpdateFeedRequest& request)
{
	const auto expectedName = BuildUpdateAssetName(candidate.version, request.architecture);
	const auto* assetsValue = Member(*candidate.release, L"assets");
	const JsoncValue::Array* assets = assetsValue == nullptr ? nullptr : AsArray(*assetsValue);
	if (assets == nullptr) {
		return { EUpdateFeedOutcome::NoEligibleAsset, std::nullopt,
			L"The release " + candidate.version.ToReleaseTag() + L" lists no assets." };
	}

	for (const auto& entry : *assets) {
		const auto* asset = AsObject(entry);
		if (asset == nullptr) continue;
		if (!EqualsIgnoringAsciiCase(StringMember(*asset, L"name"), expectedName)) continue;

		const auto state = StringMember(*asset, L"state");
		if (!state.empty() && !EqualsIgnoringAsciiCase(state, L"uploaded")) {
			return { EUpdateFeedOutcome::NoEligibleAsset, std::nullopt,
				L"The installer " + expectedName + L" has not finished uploading." };
		}

		const auto downloadUrl = StringMember(*asset, L"browser_download_url");
		if (!StartsWithIgnoringAsciiCase(downloadUrl, L"https://")) {
			return { EUpdateFeedOutcome::NoEligibleAsset, std::nullopt,
				L"The installer " + expectedName + L" has no HTTPS download address." };
		}

		bool malformed = false;
		const auto size = UnsignedMember(*asset, L"size", malformed);
		if (malformed || !size || *size == 0) {
			return { EUpdateFeedOutcome::NoEligibleAsset, std::nullopt,
				L"The installer " + expectedName + L" reports no size." };
		}

		Update update{};
		update.version = candidate.version;
		update.tagName = StringMember(*candidate.release, L"tag_name");
		update.assetName = expectedName;
		update.downloadUrl = downloadUrl;
		update.sizeBytes = *size;
		update.sha256 = ParseAssetDigest(StringMember(*asset, L"digest"));
		update.releaseUrl = StringMember(*candidate.release, L"html_url");
		update.releaseNotes = StringMember(*candidate.release, L"body");
		return { EUpdateFeedOutcome::UpdateAvailable, std::move(update), {} };
	}

	return { EUpdateFeedOutcome::NoEligibleAsset, std::nullopt,
		L"The release " + candidate.version.ToReleaseTag() + L" carries no " + expectedName + L"." };
}

} // namespace

std::optional<GitHubRepositoryRef> ParseGitHubRemoteUrl(std::wstring_view remoteUrl)
{
	while (!remoteUrl.empty() && (remoteUrl.front() == L' ' || remoteUrl.front() == L'\t')) remoteUrl.remove_prefix(1);
	while (!remoteUrl.empty() && (remoteUrl.back() == L' ' || remoteUrl.back() == L'\t'
		|| remoteUrl.back() == L'\r' || remoteUrl.back() == L'\n')) {
		remoteUrl.remove_suffix(1);
	}
	if (remoteUrl.empty()) return std::nullopt;

	// `git@github.com:owner/repo.git`
	constexpr std::wstring_view scpPrefix = L"git@";
	if (StartsWithIgnoringAsciiCase(remoteUrl, scpPrefix)) {
		const auto rest = remoteUrl.substr(scpPrefix.size());
		const auto colon = rest.find(L':');
		if (colon == std::wstring_view::npos) return std::nullopt;
		if (!EqualsIgnoringAsciiCase(rest.substr(0, colon), kGitHubHost)) return std::nullopt;
		return SplitOwnerAndRepository(rest.substr(colon + 1));
	}

	for (const std::wstring_view prefix : { std::wstring_view(L"https://"), std::wstring_view(L"ssh://git@") }) {
		if (!StartsWithIgnoringAsciiCase(remoteUrl, prefix)) continue;
		const auto rest = remoteUrl.substr(prefix.size());
		const auto slash = rest.find(L'/');
		if (slash == std::wstring_view::npos) return std::nullopt;
		if (!EqualsIgnoringAsciiCase(rest.substr(0, slash), kGitHubHost)) return std::nullopt;
		return SplitOwnerAndRepository(rest.substr(slash + 1));
	}
	return std::nullopt;
}

std::wstring BuildLatestReleaseUrl(const GitHubRepositoryRef& repository)
{
	if (!IsSafePathSegment(repository.owner, kMaximumOwnerLength)) return {};
	if (!IsSafePathSegment(repository.repository, kMaximumRepositoryLength)) return {};
	return L"https://api.github.com/repos/" + repository.owner + L"/" + repository.repository + L"/releases/latest";
}

std::wstring BuildUpdateAssetName(const UpdateVersion& version, std::wstring_view architecture)
{
	return L"sakura_install" + std::to_wstring(version.major) + L"-" + std::to_wstring(version.minor)
		+ L"-" + std::to_wstring(version.patch) + L"-" + std::to_wstring(version.revision)
		+ L"-" + std::wstring(architecture) + L".exe";
}

UpdateFeedResult ParseGitHubReleaseFeed(std::string_view utf8, const UpdateFeedRequest& request)
{
	if (utf8.size() > CJsoncDocument::kMaximumInputBytes) {
		return { EUpdateFeedOutcome::ResponseTooLarge, std::nullopt,
			L"The release listing was larger than this editor will parse." };
	}

	auto parsed = CJsoncDocument::Parse(utf8);
	if (!parsed.Succeeded()) {
		const bool tooLarge = parsed.diagnostic.has_value()
			&& (parsed.diagnostic->code == EJsoncDiagnosticCode::InputTooLarge
				|| parsed.diagnostic->code == EJsoncDiagnosticCode::MaximumNodesExceeded
				|| parsed.diagnostic->code == EJsoncDiagnosticCode::MaximumDepthExceeded
				|| parsed.diagnostic->code == EJsoncDiagnosticCode::MaximumStringLengthExceeded
				|| parsed.diagnostic->code == EJsoncDiagnosticCode::MaximumKeyLengthExceeded);
		return { tooLarge ? EUpdateFeedOutcome::ResponseTooLarge : EUpdateFeedOutcome::InvalidResponse,
			std::nullopt,
			tooLarge ? std::wstring(L"The release listing was larger than this editor will parse.")
			         : std::wstring(L"The release listing could not be read.") };
	}

	bool malformed = false;
	std::optional<ReleaseCandidate> best;
	const auto consider = [&best](std::optional<ReleaseCandidate> candidate) {
		if (!candidate) return;
		if (!best || candidate->version > best->version) best = candidate;
	};

	if (const auto* array = AsArray(*parsed.value)) {
		for (const auto& entry : *array) consider(ScreenRelease(entry, malformed));
	} else if (AsObject(*parsed.value) != nullptr) {
		consider(ScreenRelease(*parsed.value, malformed));
	} else {
		malformed = true;
	}

	if (malformed) {
		return { EUpdateFeedOutcome::InvalidResponse, std::nullopt,
			L"The release listing was not in the expected shape." };
	}
	if (!best) {
		return { EUpdateFeedOutcome::NoUpdateAvailable, std::nullopt,
			L"No stable release has been published yet." };
	}
	if (!IsNewerBuild(best->version, request.currentVersion)) {
		return { EUpdateFeedOutcome::NoUpdateAvailable, std::nullopt,
			L"This is the latest version of Sakura Editor NEXT." };
	}
	return BuildUpdate(*best, request);
}

} // namespace update

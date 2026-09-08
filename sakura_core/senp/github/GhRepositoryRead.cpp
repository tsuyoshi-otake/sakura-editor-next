/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#include "StdAfx.h"
#include "senp/github/GhRepositoryRead.h"

#include <sakura/serialization/JsoncDocument.h>

#include <algorithm>
#include <charconv>
#include <cctype>
#include <limits>
#include <string_view>

namespace senp::github {
namespace {

constexpr std::uint32_t kReadTimeoutMilliseconds = 30000;
constexpr std::size_t kReadOutputBytes = 4u * 1024u * 1024u;
constexpr std::size_t kReadErrorBytes = 64u * 1024u;
constexpr std::size_t kMaximumHeaderBytes = 64u * 1024u;

class Envelope final {
public:
	int Status() const noexcept { return status_; }
	void SetStatus(const int status) noexcept { status_ = status; }
	const std::vector<std::uint8_t>& Body() const noexcept { return body_; }
	std::vector<std::uint8_t> TakeBody() noexcept { return std::move(body_); }
	void SetBody(std::vector<std::uint8_t> body) { body_ = std::move(body); }
	const std::optional<std::string>& Etag() const noexcept { return etag_; }
	void SetEtag(std::string etag) { etag_ = std::move(etag); }
	const std::optional<std::uint32_t>& NextPage() const noexcept { return nextPage_; }
	void SetNextPage(const std::optional<std::uint32_t> page) noexcept { nextPage_ = page; }
	bool HasJsonContent() const noexcept { return jsonContent_; }
	void SetJsonContent(const bool jsonContent) noexcept { jsonContent_ = jsonContent; }
	bool ContentTypeSeen() const noexcept { return contentTypeSeen_; }
	void SetContentTypeSeen() noexcept { contentTypeSeen_ = true; }
	bool LinkSeen() const noexcept { return linkSeen_; }
	void SetLinkSeen() noexcept { linkSeen_ = true; }

private:
	int status_{};
	std::vector<std::uint8_t> body_;
	std::optional<std::string> etag_;
	std::optional<std::uint32_t> nextPage_;
	bool jsonContent_{};
	bool contentTypeSeen_{}, linkSeen_{};
};

bool EqualsInsensitive(const std::string_view left, const std::string_view right) noexcept
{
	return left.size() == right.size() && std::ranges::equal(left, right, [](const char a, const char b) {
		return static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(a)))
			== static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(b)));
	});
}

std::string_view Trim(std::string_view value) noexcept
{
	while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1);
	while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r')) value.remove_suffix(1);
	return value;
}

bool IsHeaderName(const std::string_view value) noexcept
{
	return !value.empty() && std::ranges::all_of(value, [](const unsigned char character) {
		return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z')
			|| (character >= '0' && character <= '9') || character == '-';
	});
}

bool IsEntityTag(const std::string_view value) noexcept
{
	const std::size_t begin = value.starts_with("W/\"") ? 3 : 1;
	if (value.size() <= begin || value.front() != (begin == 3 ? 'W' : '"') || value.back() != '"') return false;
	return std::ranges::all_of(value.substr(begin, value.size() - begin - 1), [](const unsigned char character) {
		return character >= 0x21 && character <= 0x7e && character != '"';
	});
}

std::optional<std::uint32_t> ParsePositive(const std::string_view value) noexcept
{
	std::uint32_t result{};
	const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
	return parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() && result > 0
		? std::optional<std::uint32_t>(result) : std::nullopt;
}

std::optional<std::uint32_t> ParseNextPage(const std::string_view link) noexcept
{
	std::size_t begin{};
	while (begin < link.size()) {
		const auto comma = link.find(',', begin);
		const auto part = link.substr(begin, (comma == std::string_view::npos ? link.size() : comma) - begin);
		if (part.find("rel=\"next\"") != std::string_view::npos) {
			auto marker = part.find("?page=");
			if (marker == std::string_view::npos) marker = part.find("&page=");
			if (marker == std::string_view::npos) return std::nullopt;
			const auto digits = part.substr(marker + 6, part.find_first_of("&>; ", marker + 6) - marker - 6);
			return ParsePositive(digits);
		}
		if (comma == std::string_view::npos) break;
		begin = comma + 1;
	}
	return std::nullopt;
}

std::optional<Envelope> ParseEnvelope(const std::vector<std::uint8_t>& bytes)
{
	if (bytes.empty()) return std::nullopt;
	const std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
	auto separator = text.find("\r\n\r\n");
	std::size_t separatorBytes = 4;
	if (separator == std::string_view::npos) {
		separator = text.find("\n\n");
		separatorBytes = 2;
	}
	if (separator == std::string_view::npos || separator > kMaximumHeaderBytes) return std::nullopt;
	const auto head = text.substr(0, separator);
	const auto firstLineEnd = head.find('\n');
	if (firstLineEnd == std::string_view::npos) return std::nullopt;
	const auto statusLine = Trim(head.substr(0, firstLineEnd));
	if (!statusLine.starts_with("HTTP/") || statusLine.size() < 12) return std::nullopt;
	const auto firstSpace = statusLine.find(' ');
	if (firstSpace == std::string_view::npos || firstSpace + 4 > statusLine.size()) return std::nullopt;
	const auto status = ParsePositive(statusLine.substr(firstSpace + 1, 3));
	if (!status || *status < 100 || *status > 599) return std::nullopt;
	Envelope envelope;
	envelope.SetStatus(static_cast<int>(*status));
	for (std::size_t begin = firstLineEnd + 1; begin < head.size();) {
		const auto end = head.find('\n', begin);
		const auto rawLine = head.substr(begin, (end == std::string_view::npos ? head.size() : end) - begin);
		if (rawLine.empty() || rawLine.front() == ' ' || rawLine.front() == '\t') return std::nullopt;
		const auto line = Trim(rawLine);
		const auto colon = line.find(':');
		if (colon == std::string_view::npos || !IsHeaderName(line.substr(0, colon))) return std::nullopt;
		const auto name = line.substr(0, colon);
		const auto value = Trim(line.substr(colon + 1));
		if (std::ranges::any_of(value, [](const unsigned char character) {
			return character < 0x20 || character == 0x7f;
		})) return std::nullopt;
		if (EqualsInsensitive(name, "content-type")) {
			if (envelope.ContentTypeSeen()) return std::nullopt;
			envelope.SetContentTypeSeen();
			envelope.SetJsonContent(value.starts_with("application/json")
				|| value.starts_with("application/vnd.github+json"));
		} else if (EqualsInsensitive(name, "etag")) {
			if (envelope.Etag() || value.size() > 256 || !IsEntityTag(value)) return std::nullopt;
			envelope.SetEtag(std::string(value));
		} else if (EqualsInsensitive(name, "link")) {
			if (envelope.LinkSeen()) return std::nullopt;
			envelope.SetLinkSeen();
			const auto next = ParseNextPage(value);
			if (value.find("rel=\"next\"") != std::string_view::npos && !next) return std::nullopt;
			envelope.SetNextPage(next);
		}
		if (end == std::string_view::npos) break;
		begin = end + 1;
	}
	const auto body = text.substr(separator + separatorBytes);
	envelope.SetBody(std::vector<std::uint8_t>(reinterpret_cast<const std::uint8_t*>(body.data()),
		reinterpret_cast<const std::uint8_t*>(body.data() + body.size())));
	return envelope;
}

std::uint32_t CurrentPage(const GhRepositoryReadRequest& request) noexcept
{
	for (const auto& [name, value] : request.Query()) {
		if (name != L"page") continue;
		std::uint32_t page{};
		for (const wchar_t character : value) {
			if (character < L'0' || character > L'9'
				|| page > (std::numeric_limits<std::uint32_t>::max() - (character - L'0')) / 10U) return 1;
			page = page * 10U + static_cast<std::uint32_t>(character - L'0');
		}
		if (page > 0) return page;
	}
	return 1;
}

GhRepositoryResponseStatus ProcessTerminal(const platform::process::EBoundedProcessStatus status) noexcept
{
	switch (status) {
	case platform::process::EBoundedProcessStatus::TimedOut: return GhRepositoryResponseStatus::TimedOut;
	case platform::process::EBoundedProcessStatus::Cancelled: return GhRepositoryResponseStatus::Cancelled;
	case platform::process::EBoundedProcessStatus::OutputLimitExceeded: return GhRepositoryResponseStatus::OutputLimitExceeded;
	default: return GhRepositoryResponseStatus::Failed;
	}
}

GhRepositoryResponse Terminal(const GhRepositoryResponseStatus status, const std::uint32_t page)
{
	return { status, 0, {}, std::nullopt, page, std::nullopt };
}

} // namespace

GhRepositoryResponse::GhRepositoryResponse(const GhRepositoryResponseStatus status, const int httpStatus,
	std::vector<std::uint8_t> body, std::optional<std::string> etag,
	const std::uint32_t currentPage, std::optional<std::uint32_t> nextPage) :
	m_status(status), m_httpStatus(httpStatus), m_body(std::move(body)), m_etag(std::move(etag)),
	m_currentPage(currentPage), m_nextPage(std::move(nextPage)) {}

GhRepositoryResponse CGhRepositoryReader::Read(GhAuthenticatedAccount& account,
	const GhToolProbe& probe, const GhRepositoryReadRequest& request, HANDLE stop) const
try {
	const auto page = CurrentPage(request);
	const auto prepared = m_policy.PrepareRepositoryRead(probe, request);
	if (prepared.Status() != GhRepositoryReadStatus::Succeeded) {
		if (prepared.Status() == GhRepositoryReadStatus::InvalidRequest) {
			return Terminal(GhRepositoryResponseStatus::InvalidRequest, page);
		}
		if (prepared.Status() == GhRepositoryReadStatus::UnsupportedVersion) {
			return Terminal(GhRepositoryResponseStatus::UnsupportedVersion, page);
		}
		return Terminal(GhRepositoryResponseStatus::ToolUnavailable, page);
	}
	const auto outcome = account.Credential().RunAuthenticated(prepared.Arguments(),
		kReadTimeoutMilliseconds, kReadOutputBytes, kReadErrorBytes, stop);
	auto envelope = ParseEnvelope(outcome.StandardOutput());
	if (!envelope) {
		return Terminal(outcome.Status() == platform::process::EBoundedProcessStatus::Succeeded
			|| !outcome.StandardOutput().empty() ? GhRepositoryResponseStatus::InvalidEnvelope
			: ProcessTerminal(outcome.Status()), page);
	}
	if (envelope->Status() == 304 && envelope->Body().empty()) {
		return { GhRepositoryResponseStatus::NotModified, 304, {}, envelope->Etag(), page, std::nullopt };
	}
	if (envelope->Status() == 401) return { GhRepositoryResponseStatus::Unauthorized, 401, {}, envelope->Etag(), page, std::nullopt };
	if (envelope->Status() == 403) return { GhRepositoryResponseStatus::Forbidden, 403, {}, envelope->Etag(), page, std::nullopt };
	if (envelope->Status() == 404) return { GhRepositoryResponseStatus::NotFound, 404, {}, envelope->Etag(), page, std::nullopt };
	if (envelope->Status() != 200 || outcome.Status() != platform::process::EBoundedProcessStatus::Succeeded) {
		return { GhRepositoryResponseStatus::Failed, envelope->Status(), {}, envelope->Etag(), page, std::nullopt };
	}
	if (!envelope->HasJsonContent()) {
		return { GhRepositoryResponseStatus::InvalidEnvelope, 200, {}, envelope->Etag(), page, std::nullopt };
	}
	const std::string body(reinterpret_cast<const char*>(envelope->Body().data()), envelope->Body().size());
	const auto parsed = platform::serialization::CJsoncDocument::ParseStrict(body);
	if (!parsed.Succeeded()) {
		return { GhRepositoryResponseStatus::InvalidJson, 200, {}, envelope->Etag(), page, std::nullopt };
	}
	const auto& value = parsed.value->Value();
	if (!std::holds_alternative<platform::serialization::JsoncValue::Object>(value)
		&& !std::holds_alternative<platform::serialization::JsoncValue::Array>(value)) {
		return { GhRepositoryResponseStatus::InvalidJson, 200, {}, envelope->Etag(), page, std::nullopt };
	}
	const auto etag = envelope->Etag();
	const auto nextPage = envelope->NextPage();
	return { GhRepositoryResponseStatus::Succeeded, 200, envelope->TakeBody(), etag, page, nextPage };
} catch (...) {
	return Terminal(GhRepositoryResponseStatus::Failed, CurrentPage(request));
}

} // namespace senp::github

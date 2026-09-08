/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#include "StdAfx.h"
#include "senp/github/GhLogResource.h"

#include <algorithm>
#include <span>

namespace senp::github {
namespace {

constexpr std::uint32_t kLogTimeoutMilliseconds = 120000;
constexpr std::size_t kErrorBytes = 64u * 1024u;

bool EqualsInsensitive(const std::wstring_view left, const std::wstring_view right) noexcept
{
	return left.size() == right.size() && ::CompareStringOrdinal(left.data(), static_cast<int>(left.size()),
		right.data(), static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

enum class SinkFailure : std::uint8_t { None, LimitExceeded, ResourceUnavailable };

class TextResourceObserver final : public platform::process::IBoundedProcessOutputObserver {
public:
	TextResourceObserver(SenpTextResourceStore& store, const TextResourceScope& scope,
		std::wstring handle) : m_store(store), m_scope(scope), m_handle(std::move(handle)) {}
	bool OnOutput(const platform::process::EBoundedProcessStream stream,
		const std::span<const std::uint8_t> bytes) override
	{
		if (stream == platform::process::EBoundedProcessStream::StandardError) return true;
		std::size_t consumed{};
		while (consumed < bytes.size()) {
			const auto count = (std::min)(bytes.size() - consumed, SenpTextResourceStore::kChunkBytes);
			const auto value = std::string_view(reinterpret_cast<const char*>(bytes.data() + consumed), count);
			const auto result = m_store.Append(m_scope, m_handle, m_offset, value);
			if (result != TextResourceResult::Accepted) {
				m_failure = result == TextResourceResult::LimitReached
					? SinkFailure::LimitExceeded : SinkFailure::ResourceUnavailable;
				return false;
			}
			m_offset += count;
			consumed += count;
		}
		return true;
	}
	[[nodiscard]] std::size_t AcceptedBytes() const noexcept { return m_offset; }
	[[nodiscard]] SinkFailure Failure() const noexcept { return m_failure; }
private:
	SenpTextResourceStore& m_store;
	TextResourceScope m_scope;
	std::wstring m_handle;
	std::size_t m_offset{};
	SinkFailure m_failure{ SinkFailure::None };
};

class ResourceCompletion final {
public:
	ResourceCompletion(SenpTextResourceStore& store, const TextResourceScope& scope,
		std::wstring_view handle) noexcept : m_store(store), m_scope(scope), m_handle(handle) {}
	~ResourceCompletion() { (void)m_store.Finish(m_scope, m_handle, TextResourceEnd::Failed); }
	ResourceCompletion(const ResourceCompletion&) = delete;
	ResourceCompletion& operator=(const ResourceCompletion&) = delete;
private:
	SenpTextResourceStore& m_store;
	const TextResourceScope& m_scope;
	std::wstring_view m_handle;
};

GhLogResourceStatus PreparedStatus(const GhRepositoryReadStatus status) noexcept
{
	switch (status) {
	case GhRepositoryReadStatus::UnsupportedVersion: return GhLogResourceStatus::UnsupportedVersion;
	case GhRepositoryReadStatus::ToolUnavailable: return GhLogResourceStatus::ToolUnavailable;
	default: return GhLogResourceStatus::InvalidRequest;
	}
}

GhLogResourceStatus ProcessStatus(const platform::process::EBoundedProcessStatus status) noexcept
{
	switch (status) {
	case platform::process::EBoundedProcessStatus::Succeeded: return GhLogResourceStatus::Succeeded;
	case platform::process::EBoundedProcessStatus::Failed: return GhLogResourceStatus::UnavailableOrNotFound;
	case platform::process::EBoundedProcessStatus::TimedOut: return GhLogResourceStatus::TimedOut;
	case platform::process::EBoundedProcessStatus::Cancelled: return GhLogResourceStatus::Cancelled;
	case platform::process::EBoundedProcessStatus::OutputLimitExceeded: return GhLogResourceStatus::LimitExceeded;
	default: return GhLogResourceStatus::Failed;
	}
}

TextResourceEnd ResourceEnd(const GhLogResourceStatus status) noexcept
{
	switch (status) {
	case GhLogResourceStatus::Succeeded: return TextResourceEnd::Complete;
	case GhLogResourceStatus::Cancelled: return TextResourceEnd::Cancelled;
	case GhLogResourceStatus::LimitExceeded: return TextResourceEnd::LimitExceeded;
	default: return TextResourceEnd::Failed;
	}
}

} // namespace

GhLogResourceResult::GhLogResourceResult(const GhLogResourceStatus status,
	std::wstring handle, const std::size_t acceptedBytes) noexcept :
	m_status(status), m_handle(std::move(handle)), m_acceptedBytes(acceptedBytes) {}

GhLogResourceResult CGhLogResource::Download(GhAuthenticatedAccount& account,
	const GhToolProbe& probe, const TextResourceScope& scope,
	const GhJobLogRequest& request, HANDLE stop) const
{
	std::wstring handle;
	try {
		if (scope.accountGeneration <= 0 || scope.accountGeneration != account.AccountGeneration()
			|| !EqualsInsensitive(account.Identity().Hostname(), request.Hostname())) {
			return { GhLogResourceStatus::InvalidRequest, {}, 0 };
		}
		const auto prepared = m_policy.PrepareJobLog(probe, request);
		if (prepared.Status() != GhRepositoryReadStatus::Succeeded) {
			return { PreparedStatus(prepared.Status()), {}, 0 };
		}
		const auto created = m_store.Create(scope);
		if (created.result != TextResourceResult::Accepted) {
			return { GhLogResourceStatus::ResourceUnavailable, {}, 0 };
		}
		handle = created.handle;
		const ResourceCompletion completion(m_store, scope, handle);
		auto observer = std::make_shared<TextResourceObserver>(m_store, scope, handle);
		const auto outcome = account.Credential().RunAuthenticatedStreaming(prepared.Arguments(),
			kLogTimeoutMilliseconds, SenpTextResourceStore::kResourceBytes, kErrorBytes,
			observer, stop);
		auto status = ProcessStatus(outcome.Status());
		if (observer->Failure() == SinkFailure::LimitExceeded) {
			status = GhLogResourceStatus::LimitExceeded;
		} else if (observer->Failure() == SinkFailure::ResourceUnavailable) {
			status = GhLogResourceStatus::ResourceUnavailable;
		}
		const auto finish = m_store.Finish(scope, handle, ResourceEnd(status));
		if (finish != TextResourceResult::Accepted && finish != TextResourceResult::Terminal) {
			status = GhLogResourceStatus::ResourceUnavailable;
		}
		return { status, handle, observer->AcceptedBytes() };
	} catch (...) {
		return { GhLogResourceStatus::Failed, std::move(handle), 0 };
	}
}

} // namespace senp::github

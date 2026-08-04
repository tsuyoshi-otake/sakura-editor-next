/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#pragma once

#include <cstdint>
#include <optional>

namespace markdown {

struct PreviewRenderKey final {
	std::uint64_t generation = 0;
	int revision = -1;
	[[nodiscard]] bool operator==(const PreviewRenderKey&) const noexcept = default;
};

enum class PreviewAsyncPhase : std::uint8_t {
	Idle,
	Queued,
	Running,
	ResultReady,
	Failed,
	Closed,
};

enum class PreviewQueueAction : std::uint8_t {
	Queued,
	ReplacedPending,
	RejectedClosed,
};

enum class PreviewCompletionAction : std::uint8_t {
	Publish,
	PublishFailure,
	DiscardStale,
	DiscardClosed,
};

//! Pure bounded scheduler for the native preview's one persistent worker.
//! Exactly one request can be active and exactly one latest request can wait.
class MarkdownPreviewAsyncState final {
public:
	[[nodiscard]] PreviewQueueAction Queue(PreviewRenderKey key) noexcept
	{
		if (m_phase == PreviewAsyncPhase::Closed) return PreviewQueueAction::RejectedClosed;
		const bool replaced = m_pending.has_value();
		m_latestRequested = key;
		m_pending = key;
		if (!m_inFlight) m_phase = PreviewAsyncPhase::Queued;
		return replaced ? PreviewQueueAction::ReplacedPending : PreviewQueueAction::Queued;
	}

	[[nodiscard]] std::optional<PreviewRenderKey> TakeNext() noexcept
	{
		if (m_phase == PreviewAsyncPhase::Closed || m_inFlight || !m_pending) return std::nullopt;
		m_inFlight = m_pending;
		m_pending.reset();
		m_phase = PreviewAsyncPhase::Running;
		return m_inFlight;
	}

	[[nodiscard]] PreviewCompletionAction Complete(PreviewRenderKey key, bool succeeded) noexcept
	{
		if (m_phase == PreviewAsyncPhase::Closed) return PreviewCompletionAction::DiscardClosed;
		if (!m_inFlight || *m_inFlight != key) return PreviewCompletionAction::DiscardStale;
		m_inFlight.reset();
		if (m_pending || !m_latestRequested || *m_latestRequested != key) {
			m_phase = m_pending ? PreviewAsyncPhase::Queued : PreviewAsyncPhase::Idle;
			return PreviewCompletionAction::DiscardStale;
		}
		m_completed = key;
		m_phase = succeeded ? PreviewAsyncPhase::ResultReady : PreviewAsyncPhase::Failed;
		return succeeded ? PreviewCompletionAction::Publish : PreviewCompletionAction::PublishFailure;
	}

	[[nodiscard]] bool IsCurrent(PreviewRenderKey key) const noexcept
	{
		return m_phase != PreviewAsyncPhase::Closed && m_latestRequested == key;
	}

	void MarkDelivered(PreviewRenderKey key) noexcept
	{
		if (m_completed != key || m_phase == PreviewAsyncPhase::Closed) return;
		m_completed.reset();
		m_phase = m_pending ? PreviewAsyncPhase::Queued
			: (m_inFlight ? PreviewAsyncPhase::Running : PreviewAsyncPhase::Idle);
	}

	void MarkDeliveryFailed(PreviewRenderKey key) noexcept
	{
		if (m_completed != key || m_phase == PreviewAsyncPhase::Closed) return;
		m_completed.reset();
		m_phase = PreviewAsyncPhase::Failed;
	}

	void Close() noexcept
	{
		m_pending.reset();
		m_inFlight.reset();
		m_completed.reset();
		m_phase = PreviewAsyncPhase::Closed;
	}

	[[nodiscard]] PreviewAsyncPhase Phase() const noexcept { return m_phase; }
	[[nodiscard]] const std::optional<PreviewRenderKey>& Pending() const noexcept { return m_pending; }
	[[nodiscard]] const std::optional<PreviewRenderKey>& InFlight() const noexcept { return m_inFlight; }
	[[nodiscard]] const std::optional<PreviewRenderKey>& LatestRequested() const noexcept { return m_latestRequested; }

private:
	PreviewAsyncPhase m_phase = PreviewAsyncPhase::Idle;
	std::optional<PreviewRenderKey> m_pending;
	std::optional<PreviewRenderKey> m_inFlight;
	std::optional<PreviewRenderKey> m_completed;
	std::optional<PreviewRenderKey> m_latestRequested;
};

} // namespace markdown

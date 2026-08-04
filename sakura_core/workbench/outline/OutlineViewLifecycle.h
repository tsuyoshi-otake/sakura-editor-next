/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <Windows.h>

#include <algorithm>
#include <cstdint>
#include <limits>

namespace workbench::outline {

//! Stable identity and monotonically increasing content version for one document.
struct OutlineDocumentVersion {
	std::uint64_t identity = 0;
	std::uint64_t version = 0;

	[[nodiscard]] bool IsValid() const noexcept { return identity != 0; }

	friend bool operator==( const OutlineDocumentVersion&, const OutlineDocumentVersion& ) noexcept = default;
};

enum class OutlineRefreshRequestStatus : std::uint8_t {
	Cached,
	Started,
	InFlight,
	HiddenPending,
	InvalidDocument,
	Closed,
	GenerationExhausted,
};

struct OutlineRefreshRequest {
	OutlineRefreshRequestStatus status = OutlineRefreshRequestStatus::InvalidDocument;
	std::uint64_t generation = 0;
};

enum class OutlineRefreshCompletion : std::uint8_t {
	Committed,
	Failed,
	Stale,
	NotInFlight,
	Closed,
};

struct OutlineRefreshSnapshot {
	bool visible = true;
	bool closed = false;
	bool hasCommittedModel = false;
	bool refreshInFlight = false;
	bool hiddenRefreshPending = false;
	OutlineDocumentVersion committedVersion{};
	OutlineDocumentVersion inFlightVersion{};
	OutlineDocumentVersion pendingVersion{};
	std::uint64_t activeGeneration = 0;
	std::uint64_t startedCount = 0;
	std::uint64_t committedCount = 0;
	std::uint64_t failedCount = 0;
	std::uint64_t staleCount = 0;
	std::uint64_t cachedCount = 0;
	std::uint64_t deduplicatedCount = 0;
};

//! O(1) refresh coordinator for the retained Outline View.
//!
//! Visibility never owns the committed model: collapsing the View only suppresses
//! new work.  A request for the current committed version is served synchronously
//! from cache, requests for the same active generation are deduplicated, and a
//! completion from a superseded generation is explicitly rejected as stale.
class OutlineRefreshCoordinator final {
public:
	[[nodiscard]] OutlineRefreshRequest Request( OutlineDocumentVersion documentVersion ) noexcept
	{
		if( m_closed ) return { OutlineRefreshRequestStatus::Closed, 0 };
		if( !documentVersion.IsValid() ) return { OutlineRefreshRequestStatus::InvalidDocument, 0 };
		if( m_hasCommittedModel && m_committedVersion == documentVersion ) {
			++m_cachedCount;
			return { OutlineRefreshRequestStatus::Cached, 0 };
		}
		if( m_refreshInFlight && m_inFlightVersion == documentVersion ) {
			++m_deduplicatedCount;
			return { OutlineRefreshRequestStatus::InFlight, m_activeGeneration };
		}
		if( !m_visible ) {
			m_hiddenRefreshPending = true;
			m_pendingVersion = documentVersion;
			return { OutlineRefreshRequestStatus::HiddenPending, 0 };
		}
		if( m_nextGeneration == std::numeric_limits<std::uint64_t>::max() ) {
			return { OutlineRefreshRequestStatus::GenerationExhausted, 0 };
		}

		m_hiddenRefreshPending = false;
		m_pendingVersion = {};
		m_refreshInFlight = true;
		m_inFlightVersion = documentVersion;
		m_activeGeneration = ++m_nextGeneration;
		++m_startedCount;
		return { OutlineRefreshRequestStatus::Started, m_activeGeneration };
	}

	[[nodiscard]] OutlineRefreshCompletion Complete(
		std::uint64_t generation,
		bool succeeded,
		OutlineDocumentVersion observedVersion = {} ) noexcept
	{
		if( m_closed ) return OutlineRefreshCompletion::Closed;
		if( !m_refreshInFlight ) return OutlineRefreshCompletion::NotInFlight;
		if( generation == 0 || generation != m_activeGeneration ) {
			++m_staleCount;
			return OutlineRefreshCompletion::Stale;
		}
		if( observedVersion.IsValid() && observedVersion != m_inFlightVersion ) {
			m_refreshInFlight = false;
			m_inFlightVersion = {};
			m_activeGeneration = 0;
			m_hiddenRefreshPending = true;
			m_pendingVersion = observedVersion;
			++m_staleCount;
			return OutlineRefreshCompletion::Stale;
		}

		const auto completedVersion = m_inFlightVersion;
		m_refreshInFlight = false;
		m_inFlightVersion = {};
		m_activeGeneration = 0;
		if( !succeeded ) {
			++m_failedCount;
			return OutlineRefreshCompletion::Failed;
		}

		m_hasCommittedModel = true;
		m_committedVersion = completedVersion;
		++m_committedCount;
		return OutlineRefreshCompletion::Committed;
	}

	//! Synchronizes an already-built dialog model without scheduling parse work.
	void AdoptCommitted( OutlineDocumentVersion documentVersion ) noexcept
	{
		if( m_closed || !documentVersion.IsValid() ) return;
		m_hasCommittedModel = true;
		m_committedVersion = documentVersion;
	}

	void SetVisible( bool visible ) noexcept
	{
		if( m_closed ) return;
		m_visible = visible;
	}

	void Close() noexcept
	{
		if( m_closed ) return;
		m_closed = true;
		m_refreshInFlight = false;
		m_hiddenRefreshPending = false;
		m_inFlightVersion = {};
		m_pendingVersion = {};
		m_activeGeneration = 0;
	}

	[[nodiscard]] OutlineRefreshSnapshot Snapshot() const noexcept
	{
		return {
			m_visible,
			m_closed,
			m_hasCommittedModel,
			m_refreshInFlight,
			m_hiddenRefreshPending,
			m_committedVersion,
			m_inFlightVersion,
			m_pendingVersion,
			m_activeGeneration,
			m_startedCount,
			m_committedCount,
			m_failedCount,
			m_staleCount,
			m_cachedCount,
			m_deduplicatedCount,
		};
	}

private:
	bool m_visible = true;
	bool m_closed = false;
	bool m_hasCommittedModel = false;
	bool m_refreshInFlight = false;
	bool m_hiddenRefreshPending = false;
	OutlineDocumentVersion m_committedVersion{};
	OutlineDocumentVersion m_inFlightVersion{};
	OutlineDocumentVersion m_pendingVersion{};
	std::uint64_t m_nextGeneration = 0;
	std::uint64_t m_activeGeneration = 0;
	std::uint64_t m_startedCount = 0;
	std::uint64_t m_committedCount = 0;
	std::uint64_t m_failedCount = 0;
	std::uint64_t m_staleCount = 0;
	std::uint64_t m_cachedCount = 0;
	std::uint64_t m_deduplicatedCount = 0;
};

struct OutlineChildLayout {
	RECT bounds{};
	bool usesWindowBorder = false;

	friend bool operator==( const OutlineChildLayout& left, const OutlineChildLayout& right ) noexcept
	{
		return left.bounds.left == right.bounds.left
			&& left.bounds.top == right.bounds.top
			&& left.bounds.right == right.bounds.right
			&& left.bounds.bottom == right.bounds.bottom
			&& left.usesWindowBorder == right.usesWindowBorder;
	}
};

//! Exact client fill used by the dialog's TreeView/ListView child.
[[nodiscard]] inline OutlineChildLayout MakeOutlineChildLayout( int width, int height ) noexcept
{
	return { RECT{ 0, 0, (std::max)(0, width), (std::max)(0, height) }, false };
}

} // namespace workbench::outline

/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <cstddef>
#include <optional>
#include <utility>

namespace workbench::rendering {

struct LatestOnlyPublishResult final {
	bool accepted{};
	bool replaced{};
	bool wakeRequired{};
};

//! A depth-one payload model for a coalesced, payload-free wakeup.
//!
//! This type performs no locking and invokes no callback. The owner protects it
//! only while transferring payload ownership, then posts or signals after the
//! operation returns. A producer can therefore never wait for painting.
template<class T>
class LatestOnlyMailbox final {
public:
	[[nodiscard]] LatestOnlyPublishResult Publish(T value)
	{
		if (m_closed) return {};
		const bool replaced = m_pending.has_value();
		m_pending = std::move(value);
		const bool wakeRequired = !m_wakePending;
		m_wakePending = true;
		return { true, replaced, wakeRequired };
	}

	[[nodiscard]] std::optional<T> Take()
	{
		m_wakePending = false;
		return std::exchange(m_pending, std::nullopt);
	}

	void CancelWakeAndDiscard() noexcept
	{
		m_pending.reset();
		m_wakePending = false;
	}

	void Close() noexcept
	{
		CancelWakeAndDiscard();
		m_closed = true;
	}

	void Open() noexcept
	{
		CancelWakeAndDiscard();
		m_closed = false;
	}

	[[nodiscard]] std::size_t Depth() const noexcept { return m_pending ? 1u : 0u; }
	[[nodiscard]] bool WakePending() const noexcept { return m_wakePending; }
	[[nodiscard]] bool Closed() const noexcept { return m_closed; }

private:
	std::optional<T> m_pending;
	bool m_wakePending{};
	bool m_closed{};
};

} // namespace workbench::rendering

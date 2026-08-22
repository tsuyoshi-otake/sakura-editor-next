/*!
    @file TerminalWorkerRetirementService.h
    @brief Admission-bounded ownership for terminal worker handles.
*/
#pragma once

#include <array>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

namespace terminal {

enum class TerminalWorkerRetirementStatus : std::uint8_t {
	Retired,
	NotJoinable,
	InvalidReservation,
};

//! A process-lifetime, fixed-capacity join owner for terminal worker handles.
//!
//! A reservation is acquired before the worker is created.  Retire only moves
//! the handle and a lifetime token into a fixed slot; it never waits, allocates,
//! detaches, or grows a queue.  A small reaper pool performs the only joins.
class TerminalWorkerRetirementService final {
public:
	static constexpr std::size_t kMaximumWorkers = 32;
	static constexpr std::size_t kReaperThreads = 4;

	class Reservation final {
	public:
		Reservation() noexcept = default;
		Reservation( const Reservation& ) = delete;
		Reservation& operator=( const Reservation& ) = delete;

		Reservation( Reservation&& other ) noexcept
		{
			MoveFrom(std::move(other));
		}

		Reservation& operator=( Reservation&& other ) noexcept
		{
			if( this != &other ) {
				Reset();
				MoveFrom(std::move(other));
			}
			return *this;
		}

		~Reservation() noexcept
		{
			Reset();
		}

		[[nodiscard]] explicit operator bool() const noexcept
		{
			return m_owner != nullptr;
		}

		void Reset() noexcept;

	private:
		friend class TerminalWorkerRetirementService;

		Reservation(
			TerminalWorkerRetirementService* owner,
			const std::size_t index,
			const std::uint64_t generation) noexcept
			: m_owner(owner)
			, m_index(index)
			, m_generation(generation)
		{
		}

		void Disarm() noexcept
		{
			m_owner = nullptr;
		}

		void MoveFrom( Reservation&& other ) noexcept
		{
			m_owner = other.m_owner;
			m_index = other.m_index;
			m_generation = other.m_generation;
			other.m_owner = nullptr;
		}

		TerminalWorkerRetirementService* m_owner = nullptr;
		std::size_t m_index = 0;
		std::uint64_t m_generation = 0;
	};

	static TerminalWorkerRetirementService& Instance() noexcept
	{
		// A terminal worker can remain in a third-party wait after its window has
		// gone away.  Deliberately leak the service so static destruction cannot
		// turn that external stall into a process-shutdown join.
		static TerminalWorkerRetirementService* const service = new TerminalWorkerRetirementService();
		return *service;
	}

	//! Reserves one join slot without waiting for an existing worker.
	[[nodiscard]] std::optional<Reservation> TryReserve() noexcept
	{
		std::lock_guard lock(m_mutex);
		if( m_reaperCount == 0 ) return std::nullopt;
		for( std::size_t index = 0; index < m_slots.size(); ++index ) {
			if( m_slots[index].state != ESlotState::Free ) continue;
			m_slots[index].state = ESlotState::Reserved;
			return Reservation(this, index, m_slots[index].generation);
		}
		return std::nullopt;
	}

	//! Transfers a joinable worker to the fixed reaper queue.  This is nonblocking.
	[[nodiscard]] TerminalWorkerRetirementStatus Retire(
		std::thread&& worker,
		Reservation&& reservation,
		std::shared_ptr<void> lifetime = {}) noexcept
	{
		if( !worker.joinable() ) return TerminalWorkerRetirementStatus::NotJoinable;
		std::lock_guard lock(m_mutex);
		if( !IsValidReservationLocked(reservation) ) {
			return TerminalWorkerRetirementStatus::InvalidReservation;
		}
		if( m_queuedCount >= m_queue.size() ) return TerminalWorkerRetirementStatus::InvalidReservation;
		Slot& slot = m_slots[reservation.m_index];
		slot.worker = std::move(worker);
		slot.task = {};
		slot.lifetime = std::move(lifetime);
		slot.state = ESlotState::Queued;
		m_queue[(m_queueHead + m_queuedCount) % m_queue.size()] = reservation.m_index;
		++m_queuedCount;
		reservation.Disarm();
		m_ready.notify_one();
		return TerminalWorkerRetirementStatus::Retired;
	}

	//! Executes a close fallback on a fixed reaper thread when a lifecycle
	//! worker cannot be constructed. The task is still admitted before the
	//! session starts, so this path never blocks the UI or grows a queue.
	[[nodiscard]] TerminalWorkerRetirementStatus RetireTask(
		Reservation&& reservation,
		std::function<void()>&& task,
		std::shared_ptr<void> lifetime = {}) noexcept
	{
		if( !task ) return TerminalWorkerRetirementStatus::NotJoinable;
		std::lock_guard lock(m_mutex);
		if( !IsValidReservationLocked(reservation) ) {
			return TerminalWorkerRetirementStatus::InvalidReservation;
		}
		if( m_queuedCount >= m_queue.size() ) return TerminalWorkerRetirementStatus::InvalidReservation;
		Slot& slot = m_slots[reservation.m_index];
		slot.worker = std::thread();
		slot.task = std::move(task);
		slot.lifetime = std::move(lifetime);
		slot.state = ESlotState::Queued;
		m_queue[(m_queueHead + m_queuedCount) % m_queue.size()] = reservation.m_index;
		++m_queuedCount;
		reservation.Disarm();
		m_ready.notify_one();
		return TerminalWorkerRetirementStatus::Retired;
	}

	[[nodiscard]] std::size_t ReservedOrPendingCount() const noexcept
	{
		std::lock_guard lock(m_mutex);
		std::size_t count = 0;
		for( const auto& slot : m_slots ) {
			if( slot.state != ESlotState::Free ) ++count;
		}
		return count;
	}

private:
	enum class ESlotState : std::uint8_t {
		Free,
		Reserved,
		Queued,
		Joining,
		Quarantined,
	};

	struct Slot final {
		ESlotState state{ ESlotState::Free };
		std::uint64_t generation{ 1 };
		std::thread worker;
		std::function<void()> task;
		std::shared_ptr<void> lifetime;
	};

	TerminalWorkerRetirementService() noexcept
	{
		for( auto& reaper : m_reapers ) {
			try {
				reaper = std::jthread([this]( std::stop_token stopToken ) { ReaperMain(stopToken); });
				++m_reaperCount;
			} catch( ... ) {
				break;
			}
		}
	}

	[[nodiscard]] bool IsValidReservationLocked( const Reservation& reservation ) const noexcept
	{
		return reservation.m_owner == this
			&& reservation.m_index < m_slots.size()
			&& m_slots[reservation.m_index].state == ESlotState::Reserved
			&& m_slots[reservation.m_index].generation == reservation.m_generation;
	}

	void ReleaseReservation( const std::size_t index, const std::uint64_t generation ) noexcept
	{
		std::lock_guard lock(m_mutex);
		if( index >= m_slots.size() ) return;
		Slot& slot = m_slots[index];
		if( slot.state != ESlotState::Reserved || slot.generation != generation ) return;
		ReleaseSlotLocked(slot);
	}

	void ReleaseSlotLocked( Slot& slot ) noexcept
	{
		// This is reached only after the reaper has joined the worker, or while a
		// reservation is still empty.  Never destroy a live joinable handle here.
		if( slot.worker.joinable() ) std::terminate();
		slot.task = {};
		slot.lifetime.reset();
		slot.state = ESlotState::Free;
		++slot.generation;
		if( slot.generation == 0 ) slot.generation = 1;
	}

	void ReaperMain( const std::stop_token stopToken ) noexcept
	{
		for( ;; ) {
			std::size_t index = m_slots.size();
			std::thread worker;
			std::function<void()> task;
			std::shared_ptr<void> lifetime;
			{
				std::unique_lock lock(m_mutex);
				m_ready.wait(lock, [this, stopToken] {
					return stopToken.stop_requested() || m_queuedCount != 0;
				});
				if( m_queuedCount == 0 && stopToken.stop_requested() ) return;
				index = m_queue[m_queueHead];
				m_queueHead = (m_queueHead + 1) % m_queue.size();
				--m_queuedCount;
				Slot& slot = m_slots[index];
				slot.state = ESlotState::Joining;
				worker = std::move(slot.worker);
				task = std::move(slot.task);
				lifetime = std::move(slot.lifetime);
			}

			bool completed = false;
			if( task ) {
				try {
					task();
					completed = true;
				} catch( ... ) {
					// Preserve a failed task in the bounded slot.  A task exception must
					// never escape this noexcept reaper or silently release its lifetime.
				}
			} else if( worker.joinable() && worker.get_id() != std::this_thread::get_id() ) {
				try {
					worker.join();
					completed = true;
				} catch( ... ) {
					// Preserve ownership in the bounded slot.  A failed join must
					// never escape this noexcept reaper or silently detach.
				}
			}

			std::lock_guard lock(m_mutex);
			Slot& slot = m_slots[index];
			if( completed || (!task && !worker.joinable()) ) {
				slot.worker = std::thread();
				slot.task = {};
				slot.lifetime.reset();
				slot.state = ESlotState::Free;
				++slot.generation;
				if( slot.generation == 0 ) slot.generation = 1;
			} else {
				slot.worker = std::move(worker);
				slot.task = std::move(task);
				slot.lifetime = std::move(lifetime);
				slot.state = ESlotState::Quarantined;
			}
		}
	}

	mutable std::mutex m_mutex;
	std::condition_variable m_ready;
	std::array<Slot, kMaximumWorkers> m_slots{};
	std::array<std::size_t, kMaximumWorkers> m_queue{};
	std::size_t m_queueHead = 0;
	std::size_t m_queuedCount = 0;
	std::array<std::jthread, kReaperThreads> m_reapers{};
	std::size_t m_reaperCount = 0;
};

inline void TerminalWorkerRetirementService::Reservation::Reset() noexcept
{
	if( m_owner != nullptr ) {
		m_owner->ReleaseReservation(m_index, m_generation);
		m_owner = nullptr;
	}
}

} // namespace terminal

/*!
    @file WorkerRetirementService.h
    @brief Admission-bounded process-wide retirement for cancellable workers.
*/
#pragma once

#include <array>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>

namespace workbench {

enum class WorkerRetirementStatus {
	Retired,
	NotJoinable,
	InvalidReservation,
};

//! One bounded join owner for UI-facing worker lifetimes.
//!
//! Capacity is reserved before a worker is created. Therefore every admitted
//! worker owns a preallocated retirement slot and Retire never allocates,
//! blocks for worker completion, detaches, or overflows. A fixed reaper pool
//! prevents one stalled worker from serializing all unrelated retirement while
//! admission still caps the live population at kMaximumWorkers.
class WorkerRetirementService final {
public:
	static constexpr std::size_t kMaximumWorkers = 32;
	static constexpr std::size_t kReaperThreads = 4;

	class Reservation final {
	public:
		Reservation() noexcept = default;
		~Reservation() { Reset(); }
		Reservation(const Reservation&) = delete;
		Reservation& operator=(const Reservation&) = delete;
		Reservation(Reservation&& other) noexcept { MoveFrom(std::move(other)); }
		Reservation& operator=(Reservation&& other) noexcept
		{
			if (this != &other) {
				Reset();
				MoveFrom(std::move(other));
			}
			return *this;
		}

		[[nodiscard]] explicit operator bool() const noexcept { return m_owner != nullptr; }

	private:
		friend class WorkerRetirementService;
		Reservation(WorkerRetirementService* owner, std::size_t index,
			std::uint64_t generation) noexcept
			: m_owner(owner), m_index(index), m_generation(generation)
		{
		}

		void Reset() noexcept
		{
			if (m_owner != nullptr) m_owner->ReleaseReservation(m_index, m_generation);
			m_owner = nullptr;
		}

		void Disarm() noexcept { m_owner = nullptr; }
		void MoveFrom(Reservation&& other) noexcept
		{
			m_owner = std::exchange(other.m_owner, nullptr);
			m_index = other.m_index;
			m_generation = other.m_generation;
		}

		WorkerRetirementService* m_owner = nullptr;
		std::size_t m_index = 0;
		std::uint64_t m_generation = 0;
	};

	static WorkerRetirementService& Instance()
	{
		// Process-lifetime service by design. A worker may be blocked in third-party
		// code after the UI has detached its lifetime. Running static destruction
		// would turn that external stall into a process-shutdown join.
		static WorkerRetirementService* const service = new WorkerRetirementService();
		return *service;
	}

	//! Reserves retirement capacity before a worker is started. This never waits.
	[[nodiscard]] std::optional<Reservation> TryReserve() noexcept
	{
		std::lock_guard lock(m_mutex);
		if (m_reaperCount == 0) return std::nullopt;
		for (std::size_t index = 0; index < m_slots.size(); ++index) {
			auto& slot = m_slots[index];
			if (slot.state != ESlotState::Free) continue;
			slot.state = ESlotState::Reserved;
			return Reservation(this, index, slot.generation);
		}
		return std::nullopt;
	}

	template <typename Lifetime>
	WorkerRetirementStatus Retire(std::thread&& worker, Reservation&& reservation,
		std::shared_ptr<Lifetime> lifetime) noexcept
	{
		return RetireImpl(std::move(worker), std::move(reservation), std::move(lifetime));
	}

	template <typename Lifetime>
	WorkerRetirementStatus Retire(std::jthread&& worker, Reservation&& reservation,
		std::shared_ptr<Lifetime> lifetime) noexcept
	{
		return RetireImpl(std::move(worker), std::move(reservation), std::move(lifetime));
	}

	[[nodiscard]] std::size_t ReservedOrPendingCount() const noexcept
	{
		std::lock_guard lock(m_mutex);
		std::size_t count = 0;
		for (const auto& slot : m_slots) {
			if (slot.state != ESlotState::Free) ++count;
		}
		return count;
	}

private:
	enum class ESlotState : std::uint8_t { Free, Reserved, Queued, Joining };
	using Worker = std::variant<std::monostate, std::thread, std::jthread>;
	struct Slot final {
		ESlotState state = ESlotState::Free;
		std::uint64_t generation = 1;
		Worker worker;
		std::shared_ptr<void> lifetime;
	};

	WorkerRetirementService() noexcept
	{
		// Keep this pool much smaller than the admission bound: these threads only
		// wait for worker termination and do not perform parser or rendering work.
		for (std::size_t index = 0; index < m_threads.size(); ++index) {
			try {
				m_threads[index] = std::jthread(
					[this](std::stop_token stopToken) { Run(stopToken); });
				++m_reaperCount;
			}
			catch (...) {
				break;
			}
		}
	}

	~WorkerRetirementService()
	{
		for (auto& thread : m_threads) thread.request_stop();
		m_ready.notify_all();
	}

	template <typename WorkerType, typename Lifetime>
	WorkerRetirementStatus RetireImpl(WorkerType&& worker, Reservation&& reservation,
		std::shared_ptr<Lifetime> lifetime) noexcept
	{
		if (!worker.joinable()) return WorkerRetirementStatus::NotJoinable;
		std::lock_guard lock(m_mutex);
		if (reservation.m_owner != this || reservation.m_index >= m_slots.size()) {
			return WorkerRetirementStatus::InvalidReservation;
		}
		auto& slot = m_slots[reservation.m_index];
		if (slot.state != ESlotState::Reserved
			|| slot.generation != reservation.m_generation
			|| m_queuedCount >= m_queue.size()) {
			return WorkerRetirementStatus::InvalidReservation;
		}
		slot.worker.template emplace<std::remove_cvref_t<WorkerType>>(std::move(worker));
		slot.lifetime = std::move(lifetime);
		slot.state = ESlotState::Queued;
		m_queue[(m_queueHead + m_queuedCount) % m_queue.size()] = reservation.m_index;
		++m_queuedCount;
		reservation.Disarm();
		m_ready.notify_one();
		return WorkerRetirementStatus::Retired;
	}

	void ReleaseReservation(std::size_t index, std::uint64_t generation) noexcept
	{
		std::lock_guard lock(m_mutex);
		if (index >= m_slots.size()) return;
		auto& slot = m_slots[index];
		if (slot.state != ESlotState::Reserved || slot.generation != generation) return;
		ReleaseSlot(slot);
	}

	static void Join(Worker& worker) noexcept
	{
		std::visit([](auto& value) noexcept {
			using T = std::remove_cvref_t<decltype(value)>;
			if constexpr (!std::is_same_v<T, std::monostate>) {
				if (value.joinable()) value.join();
			}
		}, worker);
	}

	static void ReleaseSlot(Slot& slot) noexcept
	{
		slot.worker.template emplace<std::monostate>();
		slot.lifetime.reset();
		slot.state = ESlotState::Free;
		++slot.generation;
		if (slot.generation == 0) slot.generation = 1;
	}

	void Run(std::stop_token stopToken) noexcept
	{
		for (;;) {
			std::size_t index = 0;
			Worker worker;
			std::shared_ptr<void> lifetime;
			{
				std::unique_lock lock(m_mutex);
				m_ready.wait(lock, [this, stopToken] {
					return stopToken.stop_requested() || m_queuedCount != 0;
				});
				if (m_queuedCount == 0) {
					if (stopToken.stop_requested()) return;
					continue;
				}
				index = m_queue[m_queueHead];
				m_queueHead = (m_queueHead + 1) % m_queue.size();
				--m_queuedCount;
				auto& slot = m_slots[index];
				slot.state = ESlotState::Joining;
				worker = std::move(slot.worker);
				lifetime = std::move(slot.lifetime);
			}
			Join(worker);
			worker.template emplace<std::monostate>();
			lifetime.reset();
			{
				std::lock_guard lock(m_mutex);
				ReleaseSlot(m_slots[index]);
			}
		}
	}

	mutable std::mutex m_mutex;
	std::condition_variable m_ready;
	std::array<Slot, kMaximumWorkers> m_slots{};
	std::array<std::size_t, kMaximumWorkers> m_queue{};
	std::size_t m_queueHead = 0;
	std::size_t m_queuedCount = 0;
	std::array<std::jthread, kReaperThreads> m_threads{};
	std::size_t m_reaperCount = 0;
};

} // namespace workbench

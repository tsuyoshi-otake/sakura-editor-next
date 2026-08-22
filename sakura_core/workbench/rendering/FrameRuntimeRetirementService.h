#pragma once

#include "FrameCoordinatorRuntime.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>

namespace workbench::rendering {

enum class EFrameRuntimeRetirementStatus : uint8_t {
	Pending,
	Retired,
	Quarantined,
	CapacityExhausted,
	InvalidRuntime,
	InvalidReservation,
	NotJoinable,
	Failed,
};

enum class EFrameRuntimeRetirementPhase : uint8_t {
	Reserved,
	Queued,
	Joining,
	Completed,
	Quarantined,
	Failed,
};

struct FrameRuntimeRetirementSnapshot final {
	EFrameRuntimeRetirementStatus status = EFrameRuntimeRetirementStatus::Pending;
	EFrameRuntimeRetirementPhase phase = EFrameRuntimeRetirementPhase::Reserved;
	EFrameCoordinatorRuntimeState runtimeState = EFrameCoordinatorRuntimeState::Created;
	EFrameCoordinatorRuntimeStatus beginStatus = EFrameCoordinatorRuntimeStatus::NotStarted;
	EFrameCoordinatorRuntimeStatus waitStatus = EFrameCoordinatorRuntimeStatus::NotStarted;
};

class FrameRuntimeRetirementObservation final {
public:
	FrameRuntimeRetirementSnapshot Snapshot() const noexcept
	{
		FrameRuntimeRetirementSnapshot snapshot;
		snapshot.status = m_status.load(std::memory_order_acquire);
		snapshot.phase = m_phase.load(std::memory_order_acquire);
		snapshot.runtimeState = m_runtimeState.load(std::memory_order_acquire);
		snapshot.beginStatus = m_beginStatus.load(std::memory_order_acquire);
		snapshot.waitStatus = m_waitStatus.load(std::memory_order_acquire);
		return snapshot;
	}

private:
	friend class FrameRuntimeRetirementService;

	void SetStatus(const EFrameRuntimeRetirementStatus status) noexcept
	{
		m_status.store(status, std::memory_order_release);
	}

	void SetPhase(const EFrameRuntimeRetirementPhase phase) noexcept
	{
		m_phase.store(phase, std::memory_order_release);
	}

	void SetRuntimeState(const EFrameCoordinatorRuntimeState state) noexcept
	{
		m_runtimeState.store(state, std::memory_order_release);
	}

	void SetBeginStatus(const EFrameCoordinatorRuntimeStatus status) noexcept
	{
		m_beginStatus.store(status, std::memory_order_release);
	}

	void SetWaitStatus(const EFrameCoordinatorRuntimeStatus status) noexcept
	{
		m_waitStatus.store(status, std::memory_order_release);
	}

	std::atomic<EFrameRuntimeRetirementStatus> m_status = EFrameRuntimeRetirementStatus::Pending;
	std::atomic<EFrameRuntimeRetirementPhase> m_phase = EFrameRuntimeRetirementPhase::Reserved;
	std::atomic<EFrameCoordinatorRuntimeState> m_runtimeState = EFrameCoordinatorRuntimeState::Created;
	std::atomic<EFrameCoordinatorRuntimeStatus> m_beginStatus = EFrameCoordinatorRuntimeStatus::NotStarted;
	std::atomic<EFrameCoordinatorRuntimeStatus> m_waitStatus = EFrameCoordinatorRuntimeStatus::NotStarted;
};

struct FrameRuntimeRetirementResult final {
	EFrameRuntimeRetirementStatus status = EFrameRuntimeRetirementStatus::Failed;
	std::shared_ptr<FrameRuntimeRetirementObservation> observation;

	bool Accepted() const noexcept
	{
		return status == EFrameRuntimeRetirementStatus::Retired;
	}
};

class FrameRuntimeRetirementService final {
public:
	static constexpr std::size_t kMaximumSlots = 32;
	static constexpr std::size_t kMaximumRuntimes = kMaximumSlots;
	static constexpr std::size_t kReaperThreads = 4;

	class Reservation final {
	public:
		Reservation() noexcept = default;
		Reservation(const Reservation&) = delete;
		Reservation& operator=(const Reservation&) = delete;

		Reservation(Reservation&& other) noexcept
		{
			MoveFrom(std::move(other));
		}

		Reservation& operator=(Reservation&& other) noexcept
		{
			if (this != &other) {
				Reset();
				MoveFrom(std::move(other));
			}
			return *this;
		}

		~Reservation()
		{
			Reset();
		}

		explicit operator bool() const noexcept
		{
			return m_owner != nullptr && m_observation != nullptr;
		}

		std::shared_ptr<FrameRuntimeRetirementObservation> Observation() const noexcept
		{
			return m_observation;
		}

		void Reset() noexcept;

	private:
		friend class FrameRuntimeRetirementService;

		Reservation(
			FrameRuntimeRetirementService* owner,
			const std::size_t index,
			const uint64_t generation,
			std::shared_ptr<FrameRuntimeRetirementObservation> observation) noexcept
			: m_owner(owner)
			, m_index(index)
			, m_generation(generation)
			, m_observation(std::move(observation))
		{
		}

		void Disarm() noexcept
		{
			m_owner = nullptr;
		}

		void MoveFrom(Reservation&& other) noexcept
		{
			m_owner = other.m_owner;
			m_index = other.m_index;
			m_generation = other.m_generation;
			m_observation = std::move(other.m_observation);
			other.m_owner = nullptr;
		}

		FrameRuntimeRetirementService* m_owner = nullptr;
		std::size_t m_index = 0;
		uint64_t m_generation = 0;
		std::shared_ptr<FrameRuntimeRetirementObservation> m_observation;
	};

	static FrameRuntimeRetirementService& Instance() noexcept
	{
		static FrameRuntimeRetirementService* service = new FrameRuntimeRetirementService();
		return *service;
	}

	std::optional<Reservation> TryReserve() noexcept
	{
		std::lock_guard lock(m_mutex);
		if (m_reaperCount != kReaperThreads) {
			return std::nullopt;
		}

		std::size_t index = kMaximumSlots;
		for (std::size_t i = 0; i < kMaximumSlots; ++i) {
			if (m_slots[i].state == ESlotState::Free) {
				index = i;
				break;
			}
		}
		if (index == kMaximumSlots) {
			return std::nullopt;
		}

		std::shared_ptr<FrameRuntimeRetirementObservation> observation;
		try {
			observation = std::make_shared<FrameRuntimeRetirementObservation>();
		} catch (...) {
			return std::nullopt;
		}

		Slot& slot = m_slots[index];
		slot.state = ESlotState::Reserved;
		slot.observation = observation;
		++m_reservedOrPendingCount;
		return Reservation(this, index, slot.generation, std::move(observation));
	}

	/*
	 * This is the UI handoff. The caller must reserve a slot before creating the
	 * runtime. BeginClose is non-blocking; Wait is exclusively owned by a
	 * reaper after this method returns.
	 */
	FrameRuntimeRetirementResult Retire(
		std::unique_ptr<FrameCoordinatorRuntime>& runtime,
		Reservation&& reservation) noexcept
	{
		const auto observation = reservation.m_observation;
		if (!runtime) {
			MarkRejected(observation, EFrameRuntimeRetirementStatus::InvalidRuntime);
			return {EFrameRuntimeRetirementStatus::InvalidRuntime, observation};
		}

		if (!IsValidReservation(reservation)) {
			MarkRejected(observation, EFrameRuntimeRetirementStatus::InvalidReservation);
			return {EFrameRuntimeRetirementStatus::InvalidReservation, observation};
		}

		const FrameCoordinatorRuntimeResult begin = runtime->BeginClose();
		if (observation) {
			observation->SetBeginStatus(begin.status);
			observation->SetRuntimeState(begin.state);
		}

		const FrameRuntimeRetirementResult result = Enqueue(
			reservation,
			observation,
			{},
			[&](Slot& slot) noexcept {
				slot.item.template emplace<std::unique_ptr<FrameCoordinatorRuntime>>(std::move(runtime));
			});
		return result;
	}

	/*
	 * A small worker-only seam keeps the liveness test deterministic without
	 * changing FrameCoordinatorRuntime. It uses the same reservation, queue,
	 * slot, and reaper ownership path as Retire; production frame adapters may
	 * also use it for a worker that already owns its stop protocol.
	 */
	FrameRuntimeRetirementResult RetireWorker(
		std::thread& worker,
		Reservation&& reservation,
		std::shared_ptr<void> lifetime = {}) noexcept
	{
		const auto observation = reservation.m_observation;
		if (!worker.joinable()) {
			MarkRejected(observation, EFrameRuntimeRetirementStatus::NotJoinable);
			return {EFrameRuntimeRetirementStatus::NotJoinable, observation};
		}
		if (!IsValidReservation(reservation)) {
			MarkRejected(observation, EFrameRuntimeRetirementStatus::InvalidReservation);
			return {EFrameRuntimeRetirementStatus::InvalidReservation, observation};
		}

		return Enqueue(
			reservation,
			observation,
			std::move(lifetime),
			[&](Slot& slot) noexcept {
				slot.item.template emplace<std::thread>(std::move(worker));
			});
	}

	std::size_t ReservedOrPendingCount() const noexcept
	{
		std::lock_guard lock(m_mutex);
		return m_reservedOrPendingCount;
	}

private:
	enum class ESlotState : uint8_t {
		Free,
		Reserved,
		Queued,
		Joining,
		Quarantined,
	};

	using RetiredItem = std::variant<
		std::monostate,
		std::unique_ptr<FrameCoordinatorRuntime>,
		std::thread>;

	struct Slot final {
		ESlotState state = ESlotState::Free;
		uint64_t generation = 1;
		RetiredItem item;
		std::shared_ptr<FrameRuntimeRetirementObservation> observation;
		std::shared_ptr<void> lifetime;
	};

	FrameRuntimeRetirementService() noexcept
	{
		for (std::size_t i = 0; i < kReaperThreads; ++i) {
			try {
				m_reapers[i] = std::jthread([this](std::stop_token stopToken) {
					ReaperMain(stopToken);
				});
				++m_reaperCount;
			} catch (...) {
				break;
			}
		}
	}

	~FrameRuntimeRetirementService() = default;

	bool IsValidReservation(const Reservation& reservation) const noexcept
	{
		std::lock_guard lock(m_mutex);
		return reservation.m_owner == this
			&& reservation.m_index < kMaximumSlots
			&& m_slots[reservation.m_index].state == ESlotState::Reserved
			&& m_slots[reservation.m_index].generation == reservation.m_generation
			&& reservation.m_observation != nullptr;
	}

	template <typename EnqueueItem>
	FrameRuntimeRetirementResult Enqueue(
		Reservation& reservation,
		const std::shared_ptr<FrameRuntimeRetirementObservation>& observation,
		std::shared_ptr<void> lifetime,
		EnqueueItem&& enqueueItem) noexcept
	{
		std::lock_guard lock(m_mutex);
		if (reservation.m_owner != this
			|| reservation.m_index >= kMaximumSlots
			|| m_slots[reservation.m_index].state != ESlotState::Reserved
			|| m_slots[reservation.m_index].generation != reservation.m_generation
			|| reservation.m_observation == nullptr
			|| m_queuedCount >= kMaximumSlots) {
			MarkRejected(observation, EFrameRuntimeRetirementStatus::InvalidReservation);
			return {EFrameRuntimeRetirementStatus::InvalidReservation, observation};
		}

		Slot& slot = m_slots[reservation.m_index];
		enqueueItem(slot);
		slot.lifetime = std::move(lifetime);
		slot.state = ESlotState::Queued;
		m_queue[(m_queueHead + m_queuedCount) % kMaximumSlots] = reservation.m_index;
		++m_queuedCount;
		if (observation) {
			observation->SetStatus(EFrameRuntimeRetirementStatus::Retired);
			observation->SetPhase(EFrameRuntimeRetirementPhase::Queued);
		}
		reservation.Disarm();
		m_ready.notify_one();
		return {EFrameRuntimeRetirementStatus::Retired, observation};
	}

	static void MarkRejected(
		const std::shared_ptr<FrameRuntimeRetirementObservation>& observation,
		const EFrameRuntimeRetirementStatus status) noexcept
	{
		if (observation) {
			observation->SetStatus(status);
			observation->SetPhase(EFrameRuntimeRetirementPhase::Failed);
		}
	}

	void ReleaseReservation(
		const std::size_t index,
		const uint64_t generation) noexcept
	{
		std::lock_guard lock(m_mutex);
		if (index >= kMaximumSlots) {
			return;
		}
		Slot& slot = m_slots[index];
		if (slot.state != ESlotState::Reserved || slot.generation != generation) {
			return;
		}
		if (slot.observation) {
			slot.observation->SetStatus(EFrameRuntimeRetirementStatus::Failed);
			slot.observation->SetPhase(EFrameRuntimeRetirementPhase::Failed);
		}
		ReleaseSlotLocked(slot);
	}

	void ReleaseSlotLocked(Slot& slot) noexcept
	{
		slot.item.template emplace<std::monostate>();
		slot.observation.reset();
		slot.lifetime.reset();
		slot.state = ESlotState::Free;
		++slot.generation;
		if (m_reservedOrPendingCount != 0) {
			--m_reservedOrPendingCount;
		}
	}

	void ReaperMain(const std::stop_token stopToken) noexcept
	{
		for (;;) {
			std::size_t index = kMaximumSlots;
			RetiredItem item;
			std::shared_ptr<FrameRuntimeRetirementObservation> observation;
			std::shared_ptr<void> lifetime;
			{
				std::unique_lock lock(m_mutex);
				m_ready.wait(lock, [this, stopToken] {
					return stopToken.stop_requested() || m_queuedCount != 0;
				});
				if (m_queuedCount == 0 && stopToken.stop_requested()) {
					return;
				}

				index = m_queue[m_queueHead];
				m_queueHead = (m_queueHead + 1) % kMaximumSlots;
				--m_queuedCount;
				Slot& slot = m_slots[index];
				slot.state = ESlotState::Joining;
				item = std::move(slot.item);
				observation = std::move(slot.observation);
				lifetime = std::move(slot.lifetime);
			}

			if (observation) {
				observation->SetPhase(EFrameRuntimeRetirementPhase::Joining);
			}

			bool completed = false;
			std::visit(
				[&](auto& retired) noexcept {
					using T = std::decay_t<decltype(retired)>;
					if constexpr (std::is_same_v<T, std::unique_ptr<FrameCoordinatorRuntime>>) {
						if (!retired) {
							return;
						}
						const FrameCoordinatorRuntimeResult begin = retired->BeginClose();
						const FrameCoordinatorRuntimeResult wait = retired->Wait();
						if (observation) {
							observation->SetBeginStatus(begin.status);
							observation->SetRuntimeState(wait.state);
							observation->SetWaitStatus(wait.status);
						}
						completed = wait.status == EFrameCoordinatorRuntimeStatus::Succeeded
							&& wait.state == EFrameCoordinatorRuntimeState::Stopped;
						if (completed) retired.reset();
					} else if constexpr (std::is_same_v<T, std::thread>) {
						if (!retired.joinable()) {
							return;
						}
						if (retired.get_id() == std::this_thread::get_id()) return;
						try {
							retired.join();
							completed = true;
						} catch (...) {
							// Keep ownership in the bounded quarantine slot. A failed
							// join must never escape this noexcept reaper or detach.
						}
					}
				},
				item);

			if (observation) {
				observation->SetStatus(
					completed ? EFrameRuntimeRetirementStatus::Retired
						: EFrameRuntimeRetirementStatus::Quarantined);
				observation->SetPhase(
					completed ? EFrameRuntimeRetirementPhase::Completed
						: EFrameRuntimeRetirementPhase::Quarantined);
			}

			{
				std::lock_guard lock(m_mutex);
				Slot& slot = m_slots[index];
				slot.item = std::move(item);
				slot.observation = std::move(observation);
				slot.lifetime = std::move(lifetime);
				if (completed) {
					ReleaseSlotLocked(slot);
				} else {
					slot.state = ESlotState::Quarantined;
				}
			}
		}
	}

	mutable std::mutex m_mutex;
	std::condition_variable m_ready;
	std::array<Slot, kMaximumSlots> m_slots;
	std::array<std::size_t, kMaximumSlots> m_queue{};
	std::array<std::jthread, kReaperThreads> m_reapers;
	std::size_t m_queueHead = 0;
	std::size_t m_queuedCount = 0;
	std::size_t m_reservedOrPendingCount = 0;
	std::size_t m_reaperCount = 0;
};

/*
 * Couples admission, runtime ownership, and non-blocking retirement. The
 * reservation exists before the runtime thread is created, and every scope
 * exit after creation transfers both objects to the bounded service.
 */
class FrameRuntimeRetirementLease final {
public:
	static std::unique_ptr<FrameRuntimeRetirementLease> TryCreate(
		const FrameCoordinatorRuntimeOptions& options = {}) noexcept
	{
		auto& service = FrameRuntimeRetirementService::Instance();
		auto reservation = service.TryReserve();
		if (!reservation.has_value()) return nullptr;
		try {
			auto lease = std::unique_ptr<FrameRuntimeRetirementLease>(
				new FrameRuntimeRetirementLease(service, std::move(*reservation)));
			lease->m_runtime = std::make_unique<FrameCoordinatorRuntime>(options);
			return lease;
		} catch (...) {
			return nullptr;
		}
	}

	FrameRuntimeRetirementLease(const FrameRuntimeRetirementLease&) = delete;
	FrameRuntimeRetirementLease& operator=(const FrameRuntimeRetirementLease&) = delete;

	~FrameRuntimeRetirementLease() noexcept
	{
		const auto result = RetireNow();
		if (m_runtime != nullptr || (result.status != EFrameRuntimeRetirementStatus::Retired
			&& result.status != EFrameRuntimeRetirementStatus::InvalidRuntime)) {
			// A pre-admitted lease must be accepted. Continuing would either
			// detach or synchronously wait on the destroying (possibly UI) thread.
			std::terminate();
		}
	}

	FrameCoordinatorRuntime* Runtime() const noexcept
	{
		return m_runtime.get();
	}

	FrameRuntimeRetirementResult RetireNow() noexcept
	{
		if (!m_runtime) {
			return {EFrameRuntimeRetirementStatus::InvalidRuntime, m_observation};
		}
		if (!m_reservation.has_value()) {
			return {EFrameRuntimeRetirementStatus::InvalidReservation, m_observation};
		}
		auto result = m_service.Retire(m_runtime, std::move(*m_reservation));
		if (result.Accepted()) {
			m_reservation.reset();
			m_observation = result.observation;
		}
		return result;
	}

	std::shared_ptr<FrameRuntimeRetirementObservation> Observation() const noexcept
	{
		return m_observation;
	}

private:
	FrameRuntimeRetirementLease(
		FrameRuntimeRetirementService& service,
		FrameRuntimeRetirementService::Reservation&& reservation) noexcept
		: m_service(service)
		, m_observation(reservation.Observation())
		, m_reservation(std::move(reservation))
	{
	}

	FrameRuntimeRetirementService& m_service;
	std::unique_ptr<FrameCoordinatorRuntime> m_runtime;
	std::shared_ptr<FrameRuntimeRetirementObservation> m_observation;
	std::optional<FrameRuntimeRetirementService::Reservation> m_reservation;
};

inline void FrameRuntimeRetirementService::Reservation::Reset() noexcept
{
	if (m_owner != nullptr) {
		m_owner->ReleaseReservation(m_index, m_generation);
		m_owner = nullptr;
	}
	m_observation.reset();
}

} // namespace workbench::rendering

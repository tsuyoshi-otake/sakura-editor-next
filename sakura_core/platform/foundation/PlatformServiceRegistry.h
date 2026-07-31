/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/

#pragma once

#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeindex>
#include <unordered_map>

namespace platform::foundation {

//! Stable, process-local name for a service contract.  IDs belong to the platform
//! contract, rather than to a particular Win32 or legacy implementation.
using ServiceId = std::string;

enum class ServiceRegistrationOutcome : unsigned char {
	Registered,
	InvalidServiceId,
	NullService,
	DuplicateServiceId,
};

enum class ServiceLookupOutcome : unsigned char {
	Found,
	InvalidServiceId,
	ServiceNotRegistered,
	ServiceTypeMismatch,
};

//! The outcome is always present; callers must not infer a missing service from a
//! null pointer alone.
template<class T>
struct ServiceLookupResult {
	ServiceLookupOutcome outcome = ServiceLookupOutcome::ServiceNotRegistered;
	T* service = nullptr;

	[[nodiscard]] constexpr bool Found() const noexcept
	{
		return outcome == ServiceLookupOutcome::Found;
	}
};

//! Type-erased, ownership-preserving registry for platform service contracts.
//!
//! Registration is deliberately append-only.  Replacing a service behind an ID
//! would make lifecycle ownership ambiguous, so it is rejected explicitly.
class PlatformServiceRegistry final {
public:
	PlatformServiceRegistry() = default;
	~PlatformServiceRegistry() = default;
	PlatformServiceRegistry(const PlatformServiceRegistry&) = delete;
	PlatformServiceRegistry& operator=(const PlatformServiceRegistry&) = delete;

	template<class T>
	[[nodiscard]] ServiceRegistrationOutcome Register(std::string_view serviceId, std::shared_ptr<T> service)
	{
		static_assert(!std::is_const_v<T>, "Services must be registered through their mutable contract type.");
		if (serviceId.empty()) {
			return ServiceRegistrationOutcome::InvalidServiceId;
		}
		if (!service) {
			return ServiceRegistrationOutcome::NullService;
		}

		std::lock_guard lock(m_mutex);
		const ServiceId stableId(serviceId);
		if (m_entries.contains(stableId)) {
			return ServiceRegistrationOutcome::DuplicateServiceId;
		}
		m_entries.emplace(stableId, Entry{ std::move(service), std::type_index(typeid(T)) });
		return ServiceRegistrationOutcome::Registered;
	}

	template<class T>
	[[nodiscard]] ServiceLookupResult<T> Lookup(std::string_view serviceId)
	{
		if (serviceId.empty()) {
			return { ServiceLookupOutcome::InvalidServiceId, nullptr };
		}

		std::lock_guard lock(m_mutex);
		const auto found = m_entries.find(ServiceId(serviceId));
		if (found == m_entries.end()) {
			return { ServiceLookupOutcome::ServiceNotRegistered, nullptr };
		}
		if (found->second.contractType != std::type_index(typeid(T))) {
			return { ServiceLookupOutcome::ServiceTypeMismatch, nullptr };
		}
		return { ServiceLookupOutcome::Found, static_cast<T*>(found->second.service.get()) };
	}

	template<class T>
	[[nodiscard]] ServiceLookupResult<const T> Lookup(std::string_view serviceId) const
	{
		if (serviceId.empty()) {
			return { ServiceLookupOutcome::InvalidServiceId, nullptr };
		}

		std::lock_guard lock(m_mutex);
		const auto found = m_entries.find(ServiceId(serviceId));
		if (found == m_entries.end()) {
			return { ServiceLookupOutcome::ServiceNotRegistered, nullptr };
		}
		if (found->second.contractType != std::type_index(typeid(T))) {
			return { ServiceLookupOutcome::ServiceTypeMismatch, nullptr };
		}
		return { ServiceLookupOutcome::Found, static_cast<const T*>(found->second.service.get()) };
	}

	[[nodiscard]] bool Contains(std::string_view serviceId) const
	{
		if (serviceId.empty()) {
			return false;
		}
		std::lock_guard lock(m_mutex);
		return m_entries.contains(ServiceId(serviceId));
	}

private:
	struct Entry {
		std::shared_ptr<void> service;
		std::type_index contractType = std::type_index(typeid(void));
	};

	mutable std::mutex m_mutex;
	std::unordered_map<ServiceId, Entry> m_entries;
};

} // namespace platform::foundation

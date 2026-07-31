/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "platform/storage/StorageTypes.h"

#include <memory>

namespace platform::storage {

/*! 
	@brief RAII registration for a storage change listener.

	Destroying the handle, or calling Unsubscribe(), prevents later queued
	deliveries for this listener. A callback that is already running is allowed
	to finish; callbacks may safely unsubscribe themselves or mutate storage.
	A handle may outlive the service and then becomes unsubscribed.
	The service itself must remain alive until its active Apply call and all
	synchronous callbacks it invokes have returned.
*/
class IStorageChangeSubscription {
public:
	virtual ~IStorageChangeSubscription() = default;
	virtual void Unsubscribe() noexcept = 0;
	[[nodiscard]] virtual bool IsSubscribed() const noexcept = 0;
};

class IStorageService {
public:
	virtual ~IStorageService() = default;

	[[nodiscard]] virtual StorageMutationResult Apply(const StorageMutationRequest& request) = 0;
	[[nodiscard]] virtual StorageSnapshot Snapshot() const = 0;

	/*! 
		@brief Registers for committed value changes.

		Callbacks run after the mutation lock is released, in registration order.
		A null callback is rejected by returning nullptr. Failed, conflicting,
		replayed, and no-effective-change requests never produce a callback.
	*/
	[[nodiscard]] virtual std::unique_ptr<IStorageChangeSubscription> Subscribe(
		StorageChangeCallback callback) = 0;
};

} // namespace platform::storage

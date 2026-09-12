/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/

#pragma once

#include "platform/foundation/NativeWorkerThreadAbi.h"

#include <cstdint>
#include <system_error>
#include <utility>

namespace platform::foundation {

namespace detail {

/*!
	Adapts one member function to the plain C entry point the ABI accepts.

	Access to `Method` is checked where the pointer-to-member is formed, and
	that is inside the owning class, so a worker body stays private to its
	owner.  Calling through the formed pointer performs no further check.
*/
template <class Owner, void (Owner::*Method)()>
void InvokeWorkerBody(void* context) noexcept
{
	(static_cast<Owner*>(context)->*Method)();
}

} // namespace detail

/*!
	@brief One worker thread whose lifetime is owned by Rust.

	An owner used to hold a `std::thread`, raise its own stopping flag, and then
	remember to join in teardown.  The join was a convention each owner restated
	by hand, and an owner that forgot it left a thread running against destroyed
	state.  Here the thread is owned across the C ABI: Rust holds the join handle
	and joins when the token is released, so this handle's destructor is the
	join rather than a step teardown has to remember.

	The handle is move-only.  Acquisition is the construction of one of these,
	release is its destruction, and there is no third spelling for either.

	Only the owner's own destructor may run last: the owner must still raise its
	stop and wake its worker before this handle is destroyed, exactly as it did
	before joining a `std::thread`.  A worker that never observes the stop blocks
	in the join instead of running on against freed state.
*/
class CNativeWorkerThread final {
public:
	CNativeWorkerThread() noexcept = default;
	~CNativeWorkerThread() { Join(); }

	CNativeWorkerThread(const CNativeWorkerThread&) = delete;
	CNativeWorkerThread& operator=(const CNativeWorkerThread&) = delete;

	CNativeWorkerThread(CNativeWorkerThread&& other) noexcept :
		m_token(std::exchange(other.m_token, 0))
	{
	}

	CNativeWorkerThread& operator=(CNativeWorkerThread&& other) noexcept
	{
		if (this != &other) {
			Join();
			m_token = std::exchange(other.m_token, 0);
		}
		return *this;
	}

	/*!
		Starts one worker that runs `Method` on `owner`.

		Throws `std::system_error` when the thread cannot be created, which is
		what `std::thread` did for every owner this replaced: a worker that
		cannot start is a failed construction, never a silently idle object.
	*/
	template <class Owner, void (Owner::*Method)()>
	[[nodiscard]] static CNativeWorkerThread Start(Owner* owner)
	{
		CNativeWorkerThread worker;
		const SakuraWorkerThreadStatus status = ::sakura_worker_thread_start_v1(
			&detail::InvokeWorkerBody<Owner, Method>, owner, &worker.m_token);
		if (status != SakuraWorkerThreadStatus::Ok || !worker.Started()) {
			throw std::system_error(
				std::make_error_code(std::errc::resource_unavailable_try_again),
				"sakura_worker_thread_start_v1");
		}
		return worker;
	}

	//! Whether this handle still owns a worker that has to be joined.
	[[nodiscard]] bool Started() const noexcept { return m_token != 0; }

	/*!
		Waits for the worker body to return and releases the handle.

		Idempotent: a handle that was never started, was moved from, or was
		already joined has nothing left to wait for.
	*/
	void Join() noexcept
	{
		if (m_token == 0) return;
		(void)::sakura_worker_thread_join_v1(&m_token);
		m_token = 0;
	}

private:
	std::uint64_t m_token = 0;
};

} // namespace platform::foundation

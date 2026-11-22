/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/

#pragma once

#include "platform/foundation/NativeWorkerThreadAbi.h"

#include <cstdint>
#include <exception>
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
SakuraWorkerBodyOutcome InvokeWorkerBody(void* context) noexcept
{
	try {
		(static_cast<Owner*>(context)->*Method)();
		return SakuraWorkerBodyOutcome::Completed;
	} catch (const std::exception&) {
		return SakuraWorkerBodyOutcome::ThrewException;
	} catch (...) {
		return SakuraWorkerBodyOutcome::ThrewUnknown;
	}
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

	The handle is move-only. The owner still signals its worker to stop before
	joining or destroying the handle from another thread. On the worker itself,
	releasing the handle detaches, and the body must manage any state it still
	uses through its return.
*/
class CNativeWorkerThread final {
public:
	//! The one terminal of a join. It is an immutable value.
	struct JoinResult {
		const SakuraWorkerThreadStatus status;
		const SakuraWorkerBodyOutcome outcome; //!< Valid only when status is Ok.
	};

	CNativeWorkerThread() noexcept = default;
	~CNativeWorkerThread() { (void)Join(); }

	CNativeWorkerThread(const CNativeWorkerThread&) = delete;
	CNativeWorkerThread& operator=(const CNativeWorkerThread&) = delete;

	CNativeWorkerThread(CNativeWorkerThread&& other) noexcept :
		m_token(std::exchange(other.m_token, 0))
	{
	}

	CNativeWorkerThread& operator=(CNativeWorkerThread&& other) noexcept
	{
		if (this != &other) {
			(void)Join();
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
		const SakuraWorkerThreadStatus status = ::sakura_worker_thread_start_v2(
			&detail::InvokeWorkerBody<Owner, Method>, owner, &worker.m_token);
		if (status != SakuraWorkerThreadStatus::Ok || !worker.Started()) {
			throw std::system_error(
				std::make_error_code(std::errc::resource_unavailable_try_again),
				"sakura_worker_thread_start_v2");
		}
		return worker;
	}

	//! Whether this handle still owns a worker that has to be joined.
	[[nodiscard]] bool Started() const noexcept { return m_token != 0; }

	/*!
		Releases the handle and reports the body's outcome after waiting. On the
		worker itself, releases the handle immediately with WouldDeadlock.

		An empty handle returns InvalidArgument. Callers must synchronize access
		to this object when sharing it across threads.
	*/
	JoinResult Join() noexcept
	{
		auto token = std::exchange(m_token, 0);
		auto outcome = SakuraWorkerBodyOutcome::Completed;
		const auto status = token == 0 ? SakuraWorkerThreadStatus::InvalidArgument
			: ::sakura_worker_thread_join_v2(&token, &outcome);
		return {status, outcome};
	}

private:
	std::uint64_t m_token = 0;
};

} // namespace platform::foundation

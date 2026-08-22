/*!
    @file TerminalNativeFrameBridge.h
    @brief Lifetime-fenced bridge from terminal viewports to the frame runtime.
*/
#pragma once

#include "workbench/rendering/FramePresentationOwner.h"

#include <functional>
#include <cstdint>
#include <memory>
#include <atomic>
#include <utility>

namespace terminal {

//! A small, lifetime-fenced adapter used by terminal viewports.
//!
//! CTerminalWnd can outlive a frame-runtime owner during window teardown and
//! its CPU publisher can finish on a reaper thread.  The callbacks therefore
//! never capture a raw runtime pointer directly: the composition root owns one
//! shared bridge, closes it before retiring the runtime, and every callback is
//! invoked under this bridge's close fence.  The callbacks themselves must be
//! nonblocking runtime mailbox operations; GPU work remains on the runtime's
//! presentation-owner thread.
class TerminalNativeFrameBridge final {
public:
	using RegistrationCallback = std::function<void(
		const workbench::rendering::FrameNativeSurfaceRegistration&)>;
	using UpdateCallback = RegistrationCallback;
	using CloseCallback = std::function<void(
		workbench::rendering::FrameSurfaceId, std::uint64_t)>;
	using SubmitCallback = std::function<void(
		std::shared_ptr<const workbench::rendering::FrameNativeSurfaceFrame>)>;

	TerminalNativeFrameBridge(
		RegistrationCallback registerSurface,
		UpdateCallback updateSurface,
		CloseCallback closeSurface,
		SubmitCallback submitFrame)
		: m_callbacks(std::make_shared<Callbacks>(
			std::move(registerSurface), std::move(updateSurface),
			std::move(closeSurface), std::move(submitFrame)))
	{
	}

	TerminalNativeFrameBridge(const TerminalNativeFrameBridge&) = delete;
	TerminalNativeFrameBridge& operator=(const TerminalNativeFrameBridge&) = delete;

	//! The cadence source owns this observation.  A bridge starts at epoch one
	//! so a terminal can publish before the first cadence callback, and the
	//! composition root advances it whenever FrameCadenceSource observes a new
	//! display epoch.  This is metadata only; it never schedules a frame.
	void SetDisplayEpoch(const std::uint64_t epoch) noexcept
	{
		if (epoch != 0) m_displayEpoch.store(epoch, std::memory_order_release);
	}

	[[nodiscard]] std::uint64_t DisplayEpoch() const noexcept
	{
		const auto epoch = m_displayEpoch.load(std::memory_order_acquire);
		return epoch == 0 ? 1 : epoch;
	}

	//! Fences all future callbacks. This is an atomic close operation and never
	//! waits on a terminal worker or a runtime mutex. Native-frame callbacks are
	//! delivered only by the terminal HWND's UI message path; the worker merely
	//! fills the shared immutable mailbox and posts that message.
	void Close() noexcept
	{
		if (m_callbacks != nullptr) {
			m_callbacks->closed.store(true, std::memory_order_release);
		}
	}

	void Register(const workbench::rendering::FrameNativeSurfaceRegistration& registration) noexcept
	{
		const auto callbacks = m_callbacks;
		if (callbacks == nullptr || callbacks->closed.load(std::memory_order_acquire)
			|| !callbacks->registerSurface) return;
		try {
			callbacks->registerSurface(registration);
		} catch (...) {
			// A presentation bridge is an optional native path. Never let an
			// exception cross a window message or terminal worker boundary.
		}
	}

	void Update(const workbench::rendering::FrameNativeSurfaceRegistration& registration) noexcept
	{
		const auto callbacks = m_callbacks;
		if (callbacks == nullptr || callbacks->closed.load(std::memory_order_acquire)
			|| !callbacks->updateSurface) return;
		try {
			callbacks->updateSurface(registration);
		} catch (...) {
		}
	}

	void CloseSurface(
		const workbench::rendering::FrameSurfaceId surfaceId,
		const std::uint64_t surfaceLifetimeEpoch) noexcept
	{
		const auto callbacks = m_callbacks;
		if (callbacks == nullptr || callbacks->closed.load(std::memory_order_acquire)
			|| !callbacks->closeSurface) return;
		try {
			callbacks->closeSurface(surfaceId, surfaceLifetimeEpoch);
		} catch (...) {
		}
	}

	void Submit(std::shared_ptr<const workbench::rendering::FrameNativeSurfaceFrame> frame) noexcept
	{
		const auto callbacks = m_callbacks;
		if (callbacks == nullptr || callbacks->closed.load(std::memory_order_acquire)
			|| !callbacks->submitFrame || !frame) return;
		try {
			callbacks->submitFrame(std::move(frame));
		} catch (...) {
		}
	}

private:
	struct Callbacks final {
		Callbacks(RegistrationCallback registerSurface,
			UpdateCallback updateSurface, CloseCallback closeSurface,
			SubmitCallback submitFrame) noexcept
			: registerSurface(std::move(registerSurface))
			, updateSurface(std::move(updateSurface))
			, closeSurface(std::move(closeSurface))
			, submitFrame(std::move(submitFrame))
		{
		}
		std::atomic_bool closed{ false };
		RegistrationCallback registerSurface;
		UpdateCallback updateSurface;
		CloseCallback closeSurface;
		SubmitCallback submitFrame;
	};
	std::shared_ptr<Callbacks> m_callbacks;
	std::atomic<std::uint64_t> m_displayEpoch{ 1 };
};

using TerminalNativeFrameBridgePtr = std::shared_ptr<TerminalNativeFrameBridge>;

} // namespace terminal

#include "StdAfx.h"
#include "workbench/rendering/FramePresentationOwner.h"

#include <d3d11.h>
#include <d2d1_1.h>
#include <dcomp.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <limits>
#include <new>
#include <unordered_map>
#include <utility>

namespace workbench::rendering {

namespace {

using Microsoft::WRL::ComPtr;

constexpr std::uint32_t kMinimumBufferCount = 2;
constexpr std::uint32_t kMaximumDimension = 16384;

[[nodiscard]] bool IsValidDimension(const std::uint32_t value) noexcept
{
	return value != 0 && value <= kMaximumDimension;
}

[[nodiscard]] HRESULT SimulatedFailure() noexcept
{
	return E_FAIL;
}

void UpdateMaximum(std::uint64_t& target, const std::uint64_t value) noexcept
{
	if (target < value) target = value;
}

} // namespace

struct FramePresentationOwner::GpuResources final {
	ComPtr<IDXGIFactory4> factory;
	ComPtr<IDXGIAdapter1> adapter;
	ComPtr<ID3D11Device> device;
	ComPtr<ID3D11DeviceContext> immediateContext;
	ComPtr<ID2D1Factory1> d2dFactory;
	ComPtr<ID2D1Device> d2dDevice;
	ComPtr<ID2D1DeviceContext> d2dContext;
	ComPtr<IDCompositionDevice> compositionDevice;
};

struct FramePresentationOwner::NativeTargetResources final {
	HWND targetWindow = nullptr;
	ComPtr<IDCompositionTarget> compositionTarget;
	ComPtr<IDCompositionVisual> rootVisual;
};

struct FramePresentationOwner::NativeSurfaceResources final {
	FrameNativeSurfaceRegistration registration{};
	ComPtr<IDXGISwapChain1> swapChain;
	ComPtr<IDXGISwapChain2> swapChain2;
	ComPtr<IDCompositionVisual> visual;
	ComPtr<IDCompositionVisual> parentVisual;
	std::uint64_t deviceEpoch = 0;
};

FramePresentationOwner::FramePresentationOwner(FramePresentationOwnerOptions options)
	: m_options(std::move(options))
	, m_gpu(std::make_unique<GpuResources>())
	, m_deviceDomain(1)
	, m_surfaceRegistry(m_options.maximumSurfaceCount)
	, m_width(m_options.width)
	, m_height(m_options.height)
{
	if (m_options.bufferCount < kMinimumBufferCount) {
		m_options.bufferCount = kMinimumBufferCount;
	}
	if (!IsValidDimension(m_width)) {
		m_width = 1;
	}
	if (!IsValidDimension(m_height)) {
		m_height = 1;
	}
}

FramePresentationOwner::~FramePresentationOwner() noexcept
{
	// ComPtr release is the only cleanup here. The owner contract requires an
	// explicit Close on the owner thread, but destruction never waits or joins.
	ReleaseGpuResources();
}

bool FramePresentationOwner::IsOwnerThread() const noexcept
{
	return m_ownerThreadId != std::thread::id{}
		&& m_ownerThreadId == std::this_thread::get_id();
}

bool FramePresentationOwner::ClaimOwnerThread() noexcept
{
	const auto current = std::this_thread::get_id();
	if (m_ownerThreadId == std::thread::id{}) {
		m_ownerThreadId = current;
		return true;
	}
	return m_ownerThreadId == current;
}

FramePresentationOwnerResult FramePresentationOwner::Result(
	const EFramePresentationOwnerStatus status, const HRESULT hresult) const noexcept
{
	return {
		.status = status,
		.state = m_state,
		.hresult = hresult,
		.deviceEpoch = m_deviceDomain.DeviceEpoch(),
	};
}

bool FramePresentationOwner::HasD2DResources() const noexcept
{
	return m_gpu && m_gpu->d2dFactory && m_gpu->d2dDevice && m_gpu->d2dContext;
}

bool FramePresentationOwner::NativeSurfaceResourceAvailable(
	const FrameSurfaceId surfaceId) const noexcept
{
	const auto iterator = m_nativeSurfaces.find(surfaceId);
	return iterator != m_nativeSurfaces.end()
		&& iterator->second != nullptr
		&& iterator->second->swapChain != nullptr
		&& iterator->second->visual != nullptr
		&& iterator->second->parentVisual != nullptr;
}

FramePresentationOwnerResult FramePresentationOwner::Initialize() noexcept
{
	if (!ClaimOwnerThread()) return Result(EFramePresentationOwnerStatus::WrongThread);
	if (m_state == EFramePresentationOwnerState::Closed
		|| m_state == EFramePresentationOwnerState::Closing) {
		return Result(EFramePresentationOwnerStatus::Closed);
	}
	if (m_state != EFramePresentationOwnerState::Created) {
		return Result(EFramePresentationOwnerStatus::AlreadyInitialized);
	}
	if (!IsValidDimension(m_width) || !IsValidDimension(m_height)
		|| m_options.bufferCount < kMinimumBufferCount) {
		m_lastHresult = E_INVALIDARG;
		return Result(EFramePresentationOwnerStatus::Invalid, m_lastHresult);
	}

	try {
		if (m_options.forceSoftware || !m_options.allowHardware) {
			const auto fallback = RecoverFromDeviceLoss(
				EFrameDeviceFailureBoundary::Present,
				static_cast<long>(SimulatedFailure()), 0);
			if (m_state == EFramePresentationOwnerState::HardwareReady
				|| m_state == EFramePresentationOwnerState::WarpReady
				|| m_state == EFramePresentationOwnerState::SoftwareOnly) {
				m_lastHresult = S_OK;
				return Result(EFramePresentationOwnerStatus::Succeeded, S_OK);
			}
			return fallback;
		}

		GpuResources resources;
		HRESULT failure = S_OK;
		++m_telemetry.hardwareCreationAttempts;
		if (CreateHardwareResources(resources)) {
			if (!AdoptGpuResources(std::move(resources))) {
				SetFailed(E_OUTOFMEMORY);
				return Result(EFramePresentationOwnerStatus::Failed, m_lastHresult);
			}
			m_state = EFramePresentationOwnerState::HardwareReady;
			m_lastHresult = S_OK;
			return Result(EFramePresentationOwnerStatus::Succeeded, S_OK);
		}
		++m_telemetry.hardwareCreationFailures;
		failure = m_lastHresult == S_OK ? E_FAIL : m_lastHresult;
		const auto fallback = RecoverFromDeviceLoss(
			EFrameDeviceFailureBoundary::Present,
			static_cast<long>(failure), 0);
		if (m_state == EFramePresentationOwnerState::HardwareReady
			|| m_state == EFramePresentationOwnerState::WarpReady
			|| m_state == EFramePresentationOwnerState::SoftwareOnly) {
			return Result(EFramePresentationOwnerStatus::Succeeded, S_OK);
		}
		return fallback;
	} catch (...) {
		SetFailed(E_OUTOFMEMORY);
		return Result(EFramePresentationOwnerStatus::Failed, m_lastHresult);
	}
}

FramePresentationOwnerResult FramePresentationOwner::Resize(
	const std::uint32_t width, const std::uint32_t height) noexcept
{
	if (!IsOwnerThread()) {
		return Result(m_ownerThreadId == std::thread::id{}
			? EFramePresentationOwnerStatus::WrongThread
			: EFramePresentationOwnerStatus::WrongThread);
	}
	if (m_state == EFramePresentationOwnerState::Closed
		|| m_state == EFramePresentationOwnerState::Closing) {
		return Result(EFramePresentationOwnerStatus::Closed);
	}
	if (!IsValidDimension(width) || !IsValidDimension(height)) {
		m_lastHresult = E_INVALIDARG;
		return Result(EFramePresentationOwnerStatus::Invalid, m_lastHresult);
	}
	++m_telemetry.resizeCalls;
	if (m_state == EFramePresentationOwnerState::Created) {
		return Result(EFramePresentationOwnerStatus::NotReady);
	}
	if (m_state == EFramePresentationOwnerState::SoftwareOnly) {
		m_width = width;
		m_height = height;
		m_lastHresult = S_OK;
		return Result(EFramePresentationOwnerStatus::Succeeded, S_OK);
	}
	// There is no global swapchain. Resize is retained as a compatibility
	// boundary for callers that resize the owner window; native surface sizes
	// are independently updated through UpdateNativeSurface on this thread.
	m_width = width;
	m_height = height;
	m_lastHresult = S_OK;
	return Result(EFramePresentationOwnerStatus::Succeeded, S_OK);
}

FramePresentationOwnerResult FramePresentationOwner::Present() noexcept
{
	if (!IsOwnerThread()) return Result(EFramePresentationOwnerStatus::WrongThread);
	if (m_state == EFramePresentationOwnerState::Closed
		|| m_state == EFramePresentationOwnerState::Closing) {
		return Result(EFramePresentationOwnerStatus::Closed);
	}
	if (m_state == EFramePresentationOwnerState::Created
		|| m_state == EFramePresentationOwnerState::SoftwareOnly) {
		++m_telemetry.presentNotReadySkips;
		return Result(EFramePresentationOwnerStatus::NotReady);
	}
	if (m_state != EFramePresentationOwnerState::HardwareReady
		&& m_state != EFramePresentationOwnerState::WarpReady) {
		++m_telemetry.presentNotReadySkips;
		return Result(EFramePresentationOwnerStatus::NotReady);
	}
	// Present() is the legacy global-target entry point. The owner deliberately
	// has no global swapchain; only PresentNativeSurface can present a registered
	// target-backed surface with an explicit payload.
	++m_telemetry.presentNotReadySkips;
	++m_telemetry.nativeUnavailableFallbacks;
	m_lastHresult = E_NOINTERFACE;
	m_telemetry.lastPresentResult = static_cast<std::int64_t>(m_lastHresult);
	return Result(EFramePresentationOwnerStatus::NotReady, m_lastHresult);
}

FramePresentationOwnerResult FramePresentationOwner::RegisterNativeSurface(
	const FrameNativeSurfaceRegistration& registration) noexcept
{
	if (!IsOwnerThread()) return Result(EFramePresentationOwnerStatus::WrongThread);
	if (m_state == EFramePresentationOwnerState::Closed
		|| m_state == EFramePresentationOwnerState::Closing) {
		return Result(EFramePresentationOwnerStatus::Closed);
	}
	if (!registration.IsValid()) {
		m_lastHresult = E_INVALIDARG;
		return Result(EFramePresentationOwnerStatus::Invalid, m_lastHresult);
	}

	try {
		const auto registered = m_surfaceRegistry.Register(registration.presentation);
		if (!registered.Accepted()) {
			const auto status = registered.status == EFramePresentationSurfaceStatus::Closed
				? EFramePresentationOwnerStatus::Closed
				: registered.status == EFramePresentationSurfaceStatus::Stale
					? EFramePresentationOwnerStatus::Invalid
					: EFramePresentationOwnerStatus::Failed;
			m_lastHresult = status == EFramePresentationOwnerStatus::Closed
				? S_OK : E_INVALIDARG;
			return Result(status, m_lastHresult);
		}
		m_nativeSurfaceRegistrations[registration.presentation.surfaceId] = registration;
		++m_telemetry.nativeSurfaceRegistrations;
		if (registration.targetWindow != nullptr
			&& (m_state == EFramePresentationOwnerState::HardwareReady
				|| m_state == EFramePresentationOwnerState::WarpReady)) {
			(void)EnsureNativeSurfaceResources(registration);
		}
		m_lastHresult = S_OK;
		return Result(EFramePresentationOwnerStatus::Succeeded, S_OK);
	} catch (...) {
		m_lastHresult = E_OUTOFMEMORY;
		return Result(EFramePresentationOwnerStatus::Failed, m_lastHresult);
	}
}

FramePresentationOwnerResult FramePresentationOwner::UpdateNativeSurface(
	const FrameNativeSurfaceRegistration& registration) noexcept
{
	if (!IsOwnerThread()) return Result(EFramePresentationOwnerStatus::WrongThread);
	if (m_state == EFramePresentationOwnerState::Closed
		|| m_state == EFramePresentationOwnerState::Closing) {
		return Result(EFramePresentationOwnerStatus::Closed);
	}
	if (!registration.IsValid()) {
		m_lastHresult = E_INVALIDARG;
		return Result(EFramePresentationOwnerStatus::Invalid, m_lastHresult);
	}
	const auto snapshot = m_surfaceRegistry.Snapshot(
		registration.presentation.surfaceId);
	if (!snapshot.has_value()
		|| snapshot->surfaceLifetimeEpoch
			!= registration.presentation.surfaceLifetimeEpoch) {
		return Result(EFramePresentationOwnerStatus::Invalid, E_INVALIDARG);
	}

	try {
		m_nativeSurfaceRegistrations[registration.presentation.surfaceId] = registration;
		if (registration.targetWindow == nullptr
			|| (m_state != EFramePresentationOwnerState::HardwareReady
				&& m_state != EFramePresentationOwnerState::WarpReady)) {
			const auto nativeSurface = m_nativeSurfaces.find(
				registration.presentation.surfaceId);
			if (nativeSurface != m_nativeSurfaces.end() && nativeSurface->second) {
				ReleaseNativeSurfaceVisual(*nativeSurface->second);
			}
			m_nativeSurfaces.erase(registration.presentation.surfaceId);
			RefreshNativePresentationAvailability();
			m_lastHresult = S_OK;
			return Result(EFramePresentationOwnerStatus::Succeeded, S_OK);
		}
		(void)EnsureNativeSurfaceResources(registration);
		m_lastHresult = S_OK;
		return Result(EFramePresentationOwnerStatus::Succeeded, S_OK);
	} catch (...) {
		m_lastHresult = E_OUTOFMEMORY;
		return Result(EFramePresentationOwnerStatus::Failed, m_lastHresult);
	}
}

FramePresentationOwnerResult FramePresentationOwner::CloseNativeSurface(
	const FrameSurfaceId surfaceId,
	const std::uint64_t surfaceLifetimeEpoch) noexcept
{
	if (!IsOwnerThread()) return Result(EFramePresentationOwnerStatus::WrongThread);
	if (m_state == EFramePresentationOwnerState::Closed) {
		return Result(EFramePresentationOwnerStatus::Closed);
	}
	if (surfaceId == 0 || surfaceLifetimeEpoch == 0) {
		m_lastHresult = E_INVALIDARG;
		return Result(EFramePresentationOwnerStatus::Invalid, m_lastHresult);
	}
	const auto snapshot = m_surfaceRegistry.Snapshot(surfaceId);
	if (!snapshot.has_value()) return Result(
		EFramePresentationOwnerStatus::NotReady, E_NOINTERFACE);
	if (snapshot->surfaceLifetimeEpoch != surfaceLifetimeEpoch) {
		return Result(EFramePresentationOwnerStatus::Invalid, E_INVALIDARG);
	}
	const auto closeResult = m_surfaceRegistry.Close(
		surfaceId, surfaceLifetimeEpoch);
	if (!closeResult.Accepted()
		&& closeResult.status != EFramePresentationSurfaceStatus::Closed) {
		return Result(EFramePresentationOwnerStatus::Invalid, E_INVALIDARG);
	}
	const auto nativeSurface = m_nativeSurfaces.find(surfaceId);
	if (nativeSurface != m_nativeSurfaces.end() && nativeSurface->second) {
		ReleaseNativeSurfaceVisual(*nativeSurface->second);
	}
	m_nativeSurfaces.erase(surfaceId);
	m_nativeSurfaceRegistrations.erase(surfaceId);
	RefreshNativePresentationAvailability();
	m_lastHresult = S_OK;
	return Result(EFramePresentationOwnerStatus::Succeeded, S_OK);
}

FramePresentationOwnerResult FramePresentationOwner::PresentNativeSurface(
	const FrameNativeSurfaceFrame& frame) noexcept
{
	if (!IsOwnerThread()) return Result(EFramePresentationOwnerStatus::WrongThread);
	if (m_state == EFramePresentationOwnerState::Closed
		|| m_state == EFramePresentationOwnerState::Closing) {
		return Result(EFramePresentationOwnerStatus::Closed);
	}
	if (!frame.IsValid()) {
		m_lastHresult = E_INVALIDARG;
		return Result(EFramePresentationOwnerStatus::Invalid, m_lastHresult);
	}
	const auto nativeBytes = frame.PayloadBytes();
	if (nativeBytes > m_options.maximumNativeSurfaceBytes) {
		m_lastHresult = E_INVALIDARG;
		return Result(EFramePresentationOwnerStatus::Invalid, m_lastHresult);
	}
	if (frame.deviceEpoch != m_deviceDomain.DeviceEpoch()) {
		m_lastHresult = E_INVALIDARG;
		return Result(EFramePresentationOwnerStatus::Invalid, m_lastHresult);
	}
	const auto registration = m_nativeSurfaceRegistrations.find(frame.surfaceId);
	if (registration == m_nativeSurfaceRegistrations.end()
		|| registration->second.presentation.surfaceLifetimeEpoch
			!= frame.surfaceLifetimeEpoch) {
		m_lastHresult = E_INVALIDARG;
		return Result(EFramePresentationOwnerStatus::Invalid, m_lastHresult);
	}
	if (frame.width != registration->second.presentation.width
		|| frame.height != registration->second.presentation.height) {
		m_lastHresult = E_INVALIDARG;
		return Result(EFramePresentationOwnerStatus::Invalid, m_lastHresult);
	}
	if (m_state != EFramePresentationOwnerState::HardwareReady
		&& m_state != EFramePresentationOwnerState::WarpReady) {
		++m_telemetry.presentNotReadySkips;
		++m_telemetry.nativeUnavailableFallbacks;
		m_lastHresult = E_NOINTERFACE;
		return Result(EFramePresentationOwnerStatus::NotReady, m_lastHresult);
	}
	if (!EnsureNativeSurfaceResources(registration->second)) {
		++m_telemetry.presentNotReadySkips;
		++m_telemetry.nativeUnavailableFallbacks;
		m_lastHresult = m_lastHresult == S_OK ? E_NOINTERFACE : m_lastHresult;
		return Result(EFramePresentationOwnerStatus::NotReady, m_lastHresult);
	}
	const auto resource = m_nativeSurfaces.find(frame.surfaceId);
	if (resource == m_nativeSurfaces.end() || !resource->second
		|| !m_gpu || !m_gpu->device || !m_gpu->immediateContext) {
		++m_telemetry.presentNotReadySkips;
		++m_telemetry.nativeUnavailableFallbacks;
		m_lastHresult = E_NOINTERFACE;
		return Result(EFramePresentationOwnerStatus::NotReady, m_lastHresult);
	}

	const auto startedMicroseconds = FrameRuntimeTelemetry::NowMicroseconds();
	HRESULT hr = S_OK;
	ComPtr<ID3D11Texture2D> backBuffer;
	hr = resource->second->swapChain->GetBuffer(
		0, IID_PPV_ARGS(&backBuffer));
	if (SUCCEEDED(hr)) {
		RECT dirty = frame.dirtyRect;
		const bool fullSurface = dirty.left == 0 && dirty.top == 0
			&& dirty.right == 0 && dirty.bottom == 0;
		if (fullSurface) {
			dirty = RECT{ 0, 0, static_cast<LONG>(frame.width),
				static_cast<LONG>(frame.height) };
		}
		D3D11_BOX box{
			static_cast<UINT>(dirty.left), static_cast<UINT>(dirty.top), 0,
			static_cast<UINT>(dirty.right), static_cast<UINT>(dirty.bottom), 1,
		};
		const auto sourceOffset = frame.compactDirtyPayload
			? 0u
			: static_cast<std::size_t>(dirty.top) * frame.pitch
				+ static_cast<std::size_t>(dirty.left) * 4u;
		m_gpu->immediateContext->UpdateSubresource(
			backBuffer.Get(), 0, &box, frame.pixels->data() + sourceOffset,
			frame.pitch, 0);
		++m_telemetry.nativeSurfaceUploadCalls;
		m_telemetry.nativeSurfaceUploadBytes += static_cast<std::uint64_t>(
			static_cast<std::size_t>(dirty.bottom - dirty.top) * frame.pitch);
		if (!fullSurface) ++m_telemetry.nativeSurfaceDirtyRectUpdates;
		DXGI_PRESENT_PARAMETERS parameters{};
		if (!fullSurface) {
			parameters.DirtyRectsCount = 1;
			parameters.pDirtyRects = &dirty;
		}
		++m_telemetry.presentCalls;
		hr = resource->second->swapChain->Present1(
			1, DXGI_PRESENT_DO_NOT_WAIT, &parameters);
		if (hr == DXGI_ERROR_INVALID_CALL && !fullSurface) {
			// Composition swap chains on older WDDM implementations reject the
			// optional dirty-rect hint even though the back-buffer upload is valid.
			// Keep the bounded dirty upload, then retry one non-waiting full present;
			// this is a compatibility fallback, never a wait or an unbounded retry.
			DXGI_PRESENT_PARAMETERS fullParameters{};
			++m_telemetry.presentCalls;
			hr = resource->second->swapChain->Present1(
				1, DXGI_PRESENT_DO_NOT_WAIT, &fullParameters);
		}
	}
	const auto finishedMicroseconds = FrameRuntimeTelemetry::NowMicroseconds();
	const auto durationMicroseconds = finishedMicroseconds >= startedMicroseconds
		? finishedMicroseconds - startedMicroseconds : 0;
	m_telemetry.lastPresentResult = static_cast<std::int64_t>(hr);
	m_telemetry.lastPresentDurationMicroseconds = durationMicroseconds;
	UpdateMaximum(m_telemetry.maximumPresentDurationMicroseconds,
		durationMicroseconds);
	m_lastHresult = hr;
	if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
		++m_telemetry.presentBackpressureSkips;
		return Result(EFramePresentationOwnerStatus::SkippedBackpressure, hr);
	}
	if (FAILED(hr)) {
		if (IsDeviceLoss(hr)) {
			++m_telemetry.presentDeviceLosses;
			return RecoverFromDeviceLoss(
				EFrameDeviceFailureBoundary::Present,
				static_cast<long>(hr), 0);
		}
		++m_telemetry.presentFailures;
		SetFailed(hr);
		return Result(EFramePresentationOwnerStatus::Failed, hr);
	}
	if (!m_gpu || !m_gpu->compositionDevice) {
		m_lastHresult = E_NOINTERFACE;
		return Result(EFramePresentationOwnerStatus::Failed, m_lastHresult);
	}
	hr = m_gpu->compositionDevice->Commit();
	if (FAILED(hr)) {
		m_lastHresult = hr;
		if (IsDeviceLoss(hr)) {
			return RecoverFromDeviceLoss(
				EFrameDeviceFailureBoundary::CompositionCommit,
				static_cast<long>(hr), 0);
		}
		++m_telemetry.presentFailures;
		SetFailed(hr);
		return Result(EFramePresentationOwnerStatus::Failed, hr);
	}
	++m_telemetry.presentSuccesses;
	++m_telemetry.compositionCommits;
	m_lastHresult = S_OK;
	return Result(EFramePresentationOwnerStatus::Succeeded, S_OK);
}

FramePresentationOwnerResult FramePresentationOwner::ObserveReadback(
	const FrameReadbackObservation& observation) noexcept
{
	if (!IsOwnerThread()) return Result(EFramePresentationOwnerStatus::WrongThread);
	if (m_state == EFramePresentationOwnerState::Closed
		|| m_state == EFramePresentationOwnerState::Closing) {
		return Result(EFramePresentationOwnerStatus::Closed);
	}
	if (observation.surfaceId == 0 || observation.surfaceLifetimeEpoch == 0
		|| observation.deviceEpoch == 0 || observation.displayEpoch == 0
		|| observation.layoutEpoch == 0 || observation.requestId == 0
		|| observation.width == 0 || observation.height == 0) {
		m_lastHresult = E_INVALIDARG;
		return Result(EFramePresentationOwnerStatus::Invalid, m_lastHresult);
	}
	const auto surface = m_surfaceRegistry.Snapshot(observation.surfaceId);
	if (!surface.has_value()
		|| surface->surfaceLifetimeEpoch != observation.surfaceLifetimeEpoch
		|| surface->deviceEpoch != observation.deviceEpoch
		|| observation.layoutEpoch < surface->layoutEpoch
		|| observation.requestId < surface->lastPresentedRequestId) {
		return Result(EFramePresentationOwnerStatus::Invalid, E_INVALIDARG);
	}
	++m_telemetry.readbackObservations;
	if (observation.completed) ++m_telemetry.readbackCompletions;
	m_telemetry.readbackBytes += observation.byteCount;
	UpdateMaximum(m_telemetry.lastReadbackRequestId, observation.requestId);
	return Result(EFramePresentationOwnerStatus::Succeeded, S_OK);
}

FramePresentationOwnerResult FramePresentationOwner::CommitComposition() noexcept
{
	if (!IsOwnerThread()) return Result(EFramePresentationOwnerStatus::WrongThread);
	if (m_state == EFramePresentationOwnerState::Closed
		|| m_state == EFramePresentationOwnerState::Closing) {
		return Result(EFramePresentationOwnerStatus::Closed);
	}
	if (m_state == EFramePresentationOwnerState::Created
		|| m_state == EFramePresentationOwnerState::SoftwareOnly) {
		return Result(EFramePresentationOwnerStatus::NotReady);
	}
	if (!m_gpu || !m_gpu->compositionDevice || m_nativeSurfaces.empty()) {
		return Result(EFramePresentationOwnerStatus::NotReady);
	}

	const HRESULT hr = m_gpu->compositionDevice->Commit();
	m_lastHresult = hr;
	if (SUCCEEDED(hr)) {
		++m_telemetry.compositionCommits;
		return Result(EFramePresentationOwnerStatus::Succeeded, hr);
	}
	if (IsDeviceLoss(hr)) {
		return RecoverFromDeviceLoss(
			EFrameDeviceFailureBoundary::CompositionCommit,
			static_cast<long>(hr), 0);
	}
	SetFailed(hr);
	return Result(EFramePresentationOwnerStatus::Failed, hr);
}

FramePresentationOwnerResult FramePresentationOwner::PublishSoftwareFrame(
	const std::uint32_t width, const std::uint32_t height, const std::uint32_t pitch,
	const std::span<const std::uint8_t> pixels) noexcept
{
	if (!IsOwnerThread()) return Result(EFramePresentationOwnerStatus::WrongThread);
	if (m_state == EFramePresentationOwnerState::Closed
		|| m_state == EFramePresentationOwnerState::Closing) {
		return Result(EFramePresentationOwnerStatus::Closed);
	}
	if (m_state != EFramePresentationOwnerState::SoftwareOnly) {
		return Result(EFramePresentationOwnerStatus::NotReady);
	}
	if (!IsValidDimension(width) || !IsValidDimension(height)
		|| width > (std::numeric_limits<std::uint32_t>::max)() / 4u
		|| pitch < width * 4u) {
		m_lastHresult = E_INVALIDARG;
		return Result(EFramePresentationOwnerStatus::Invalid, m_lastHresult);
	}
	const auto requiredBytes = static_cast<std::size_t>(pitch) * height;
	if (requiredBytes > m_options.maximumSoftwareBytes || requiredBytes > pixels.size()) {
		m_lastHresult = E_INVALIDARG;
		return Result(EFramePresentationOwnerStatus::Invalid, m_lastHresult);
	}

	try {
		FrameSoftwarePublication publication;
		publication.width = width;
		publication.height = height;
		publication.pitch = pitch;
		publication.deviceEpoch = m_deviceDomain.DeviceEpoch();
		publication.pixels.assign(pixels.begin(), pixels.begin() + requiredBytes);
		m_lastSoftwarePublication = publication;
		++m_telemetry.softwarePublications;
		// Publish only a depth-one immutable mailbox entry. Consumer code is
		// never called on the owner and can stall without blocking this path.
		const auto mailboxEntry = std::make_shared<FrameSoftwarePublication>(
			m_lastSoftwarePublication);
		m_softwareMailbox.store(
			std::shared_ptr<const FrameSoftwarePublication>(mailboxEntry),
			std::memory_order_release);
		m_lastHresult = S_OK;
		return Result(EFramePresentationOwnerStatus::Succeeded, S_OK);
	} catch (...) {
		SetFailed(E_OUTOFMEMORY);
		return Result(EFramePresentationOwnerStatus::Failed, m_lastHresult);
	}
}

FramePresentationOwnerResult FramePresentationOwner::Tick(
	const std::uint64_t nowMilliseconds) noexcept
{
	if (!IsOwnerThread()) return Result(EFramePresentationOwnerStatus::WrongThread);
	if (m_state == EFramePresentationOwnerState::Closed
		|| m_state == EFramePresentationOwnerState::Closing) {
		return Result(EFramePresentationOwnerStatus::Closed);
	}
	if (!m_options.allowHardwareReprobe
		|| (m_state != EFramePresentationOwnerState::WarpReady
			&& m_state != EFramePresentationOwnerState::SoftwareOnly)) {
		return Result(EFramePresentationOwnerStatus::Backoff);
	}
	return ProbeHardware(nowMilliseconds);
}

FramePresentationOwnerResult FramePresentationOwner::InjectDeviceLoss(
	const EFrameDeviceFailureBoundary boundary, const long failureCode) noexcept
{
	if (!IsOwnerThread()) return Result(EFramePresentationOwnerStatus::WrongThread);
	if (m_state == EFramePresentationOwnerState::Closed
		|| m_state == EFramePresentationOwnerState::Closing) {
		return Result(EFramePresentationOwnerStatus::Closed);
	}
	if (m_state == EFramePresentationOwnerState::Created
		|| m_state == EFramePresentationOwnerState::SoftwareOnly) {
		return Result(EFramePresentationOwnerStatus::NotReady);
	}
	return RecoverFromDeviceLoss(boundary, failureCode, 0);
}

std::shared_ptr<const FrameSoftwarePublication>
FramePresentationOwner::TakeSoftwarePublication() noexcept
{
	return m_softwareMailbox.exchange(
		std::shared_ptr<const FrameSoftwarePublication>{},
		std::memory_order_acq_rel);
}

FramePresentationOwnerResult FramePresentationOwner::BeginClose() noexcept
{
	return Close();
}

FramePresentationOwnerResult FramePresentationOwner::Close() noexcept
{
	if (!ClaimOwnerThread()) return Result(EFramePresentationOwnerStatus::WrongThread);
	if (m_state == EFramePresentationOwnerState::Closed) {
		return Result(EFramePresentationOwnerStatus::Closed);
	}
	m_state = EFramePresentationOwnerState::Closing;
	ReleaseGpuResources();
	m_nativeSurfaceRegistrations.clear();
	(void)m_deviceDomain.Close();
	(void)m_softwareMailbox.exchange(
		std::shared_ptr<const FrameSoftwarePublication>{},
		std::memory_order_acq_rel);
	m_state = EFramePresentationOwnerState::Closed;
	m_lastHresult = S_OK;
	return Result(EFramePresentationOwnerStatus::Succeeded, S_OK);
}

bool FramePresentationOwner::CreateHardwareResources(GpuResources& resources) noexcept
{
	if (m_options.failHardwareCreation) {
		m_lastHresult = SimulatedFailure();
		return false;
	}
	HRESULT failure = S_OK;
	if (!CreateGpuResources(resources, false, failure)) {
		m_lastHresult = failure;
		return false;
	}
	return true;
}

bool FramePresentationOwner::CreateWarpResources(GpuResources& resources) noexcept
{
	if (m_options.failWarpCreation) {
		m_lastHresult = SimulatedFailure();
		return false;
	}
	HRESULT failure = S_OK;
	if (!CreateGpuResources(resources, true, failure)) {
		m_lastHresult = failure;
		return false;
	}
	return true;
}

bool FramePresentationOwner::CreateGpuResources(
	GpuResources& resources, const bool warp, HRESULT& failure) noexcept
{
	failure = S_OK;
	HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&resources.factory));
	if (FAILED(hr)) {
		failure = hr;
		return false;
	}

	if (warp) {
		hr = resources.factory->EnumWarpAdapter(IID_PPV_ARGS(&resources.adapter));
		if (FAILED(hr)) {
			failure = hr;
			return false;
		}
	} else {
		for (UINT index = 0;; ++index) {
			ComPtr<IDXGIAdapter1> adapter;
			hr = resources.factory->EnumAdapters1(index, &adapter);
			if (hr == DXGI_ERROR_NOT_FOUND) break;
			if (FAILED(hr)) {
				failure = hr;
				return false;
			}
			DXGI_ADAPTER_DESC1 description{};
			if (FAILED(adapter->GetDesc1(&description))) continue;
			if ((description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) continue;
			resources.adapter = std::move(adapter);
			break;
		}
		if (!resources.adapter) {
			failure = DXGI_ERROR_NOT_FOUND;
			return false;
		}
	}

	constexpr D3D_FEATURE_LEVEL featureLevels[] = {
		D3D_FEATURE_LEVEL_11_1,
		D3D_FEATURE_LEVEL_11_0,
		D3D_FEATURE_LEVEL_10_1,
	};
	D3D_FEATURE_LEVEL selectedLevel = D3D_FEATURE_LEVEL_10_1;
	UINT deviceFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
	if (m_options.enableDebugLayer) deviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
	hr = D3D11CreateDevice(
		resources.adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
		deviceFlags, featureLevels,
		static_cast<UINT>(std::size(featureLevels)), D3D11_SDK_VERSION,
		&resources.device, &selectedLevel, &resources.immediateContext);
	if (FAILED(hr)) {
		failure = hr;
		return false;
	}
	if (!CreateD2DResources(resources, failure)) return false;

	// Swapchains are deliberately per-surface. Creating one here would leave a
	// global composition resource with no target/payload and could cover pixels
	// owned by the GDI path. The DComp device is enough for later owner-thread
	// RegisterNativeSurface calls to create a target-backed chain.
	if (!CreateCompositionResources(resources, failure)) return false;
	return true;
}

bool FramePresentationOwner::CreateD2DResources(
	GpuResources& resources, HRESULT& failure) noexcept
{
	D2D1_FACTORY_OPTIONS factoryOptions{};
	if (m_options.enableDebugLayer) {
		factoryOptions.debugLevel = D2D1_DEBUG_LEVEL_INFORMATION;
	}
	HRESULT hr = D2D1CreateFactory(
		D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1),
		&factoryOptions,
		reinterpret_cast<void**>(resources.d2dFactory.GetAddressOf()));
	if (FAILED(hr)) {
		failure = hr;
		return false;
	}
	ComPtr<IDXGIDevice> dxgiDevice;
	hr = resources.device.As(&dxgiDevice);
	if (FAILED(hr)) {
		failure = hr;
		return false;
	}
	hr = resources.d2dFactory->CreateDevice(dxgiDevice.Get(), &resources.d2dDevice);
	if (FAILED(hr)) {
		failure = hr;
		return false;
	}
	hr = resources.d2dDevice->CreateDeviceContext(
		D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &resources.d2dContext);
	if (FAILED(hr)) {
		failure = hr;
		return false;
	}
	return true;
}

bool FramePresentationOwner::CreateCompositionResources(
	GpuResources& resources, HRESULT& failure) noexcept
{
	if (m_options.failCompositionCreation) {
		failure = SimulatedFailure();
		return false;
	}
	ComPtr<IDXGIDevice> dxgiDevice;
	HRESULT hr = resources.device.As(&dxgiDevice);
	if (FAILED(hr)) {
		failure = hr;
		return false;
	}
	hr = DCompositionCreateDevice(
		dxgiDevice.Get(), IID_PPV_ARGS(&resources.compositionDevice));
	if (FAILED(hr)) {
		failure = hr;
		return false;
	}
	return true;
}

bool FramePresentationOwner::CreateNativeSurfaceResources(
	const FrameNativeSurfaceRegistration& registration,
	NativeSurfaceResources& resources,
	HRESULT& failure) noexcept
{
	failure = S_OK;
	if (!registration.targetWindow || !::IsWindow(registration.targetWindow)) {
		failure = E_INVALIDARG;
		return false;
	}
	if (!m_gpu || !m_gpu->factory || !m_gpu->device
		|| !m_gpu->compositionDevice) {
		failure = E_NOINTERFACE;
		return false;
	}

	DXGI_SWAP_CHAIN_DESC1 description{};
	description.Width = registration.presentation.width;
	description.Height = registration.presentation.height;
	description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	description.Stereo = FALSE;
	description.SampleDesc.Count = 1;
	description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	description.BufferCount = m_options.bufferCount;
	description.Scaling = DXGI_SCALING_STRETCH;
	description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
	description.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
	// DO_NOT_WAIT is supplied to Present1. No waitable object is created or
	// waited on, keeping resource admission bounded without a hidden wait.
	description.Flags = 0;
	HRESULT hr = m_gpu->factory->CreateSwapChainForComposition(
		m_gpu->device.Get(), &description, nullptr, &resources.swapChain);
	if (FAILED(hr)) {
		failure = hr;
		return false;
	}
	(void)resources.swapChain.As(&resources.swapChain2);
	if (resources.swapChain2) {
		(void)resources.swapChain2->SetMaximumFrameLatency(1);
	}
	NativeTargetResources* targetResources = nullptr;
	const auto targetIterator = m_nativeTargets.find(registration.targetWindow);
	if (targetIterator != m_nativeTargets.end() && targetIterator->second) {
		targetResources = targetIterator->second.get();
	} else {
		auto target = std::make_unique<NativeTargetResources>();
		target->targetWindow = registration.targetWindow;
		hr = m_gpu->compositionDevice->CreateTargetForHwnd(
			registration.targetWindow, FALSE, &target->compositionTarget);
		if (FAILED(hr)) {
			failure = hr;
			return false;
		}
		hr = m_gpu->compositionDevice->CreateVisual(&target->rootVisual);
		if (FAILED(hr)) {
			failure = hr;
			return false;
		}
		hr = target->compositionTarget->SetRoot(target->rootVisual.Get());
		if (FAILED(hr)) {
			failure = hr;
			return false;
		}
		targetResources = target.get();
		try {
			m_nativeTargets.emplace(registration.targetWindow, std::move(target));
		} catch (...) {
			failure = E_OUTOFMEMORY;
			return false;
		}
	}

	hr = m_gpu->compositionDevice->CreateVisual(&resources.visual);
	if (FAILED(hr)) {
		failure = hr;
		return false;
	}
	hr = resources.visual->SetContent(resources.swapChain.Get());
	if (FAILED(hr)) {
		failure = hr;
		return false;
	}
	hr = resources.visual->SetOffsetX(static_cast<float>(registration.x));
	if (FAILED(hr)) {
		failure = hr;
		return false;
	}
	hr = resources.visual->SetOffsetY(static_cast<float>(registration.y));
	if (FAILED(hr)) {
		failure = hr;
		return false;
	}
	hr = targetResources->rootVisual->AddVisual(resources.visual.Get(), TRUE, nullptr);
	if (FAILED(hr)) {
		failure = hr;
		return false;
	}
	resources.parentVisual = targetResources->rootVisual;
	resources.registration = registration;
	resources.deviceEpoch = m_deviceDomain.DeviceEpoch();
	return true;
}

bool FramePresentationOwner::EnsureNativeSurfaceResources(
	const FrameNativeSurfaceRegistration& registration) noexcept
{
	if (!registration.targetWindow
		|| (m_state != EFramePresentationOwnerState::HardwareReady
			&& m_state != EFramePresentationOwnerState::WarpReady)) {
		RefreshNativePresentationAvailability();
		return false;
	}
	const auto existing = m_nativeSurfaces.find(registration.presentation.surfaceId);
	if (existing != m_nativeSurfaces.end() && existing->second
		&& existing->second->registration.targetWindow == registration.targetWindow
		&& existing->second->registration.x == registration.x
		&& existing->second->registration.y == registration.y
		&& existing->second->registration.presentation.width
			== registration.presentation.width
		&& existing->second->registration.presentation.height
			== registration.presentation.height
		&& existing->second->deviceEpoch == m_deviceDomain.DeviceEpoch()) {
		return true;
	}

	const auto existingResource = m_nativeSurfaces.find(
		registration.presentation.surfaceId);
	if (existingResource != m_nativeSurfaces.end() && existingResource->second) {
		ReleaseNativeSurfaceVisual(*existingResource->second);
	}
	m_nativeSurfaces.erase(registration.presentation.surfaceId);
	try {
		auto resources = std::make_unique<NativeSurfaceResources>();
		HRESULT failure = S_OK;
		if (!CreateNativeSurfaceResources(registration, *resources, failure)) {
			m_lastHresult = failure;
			++m_telemetry.nativeSurfaceResourceFailures;
			RefreshNativePresentationAvailability();
			return false;
		}
		m_nativeSurfaces.emplace(registration.presentation.surfaceId,
			std::move(resources));
		++m_telemetry.nativeSurfaceResourceCreations;
		RefreshNativePresentationAvailability();
		m_lastHresult = S_OK;
		return true;
	} catch (...) {
		m_lastHresult = E_OUTOFMEMORY;
		++m_telemetry.nativeSurfaceResourceFailures;
		RefreshNativePresentationAvailability();
		return false;
	}
}

void FramePresentationOwner::RecreateNativeSurfaceResources() noexcept
{
	if (m_state != EFramePresentationOwnerState::HardwareReady
		&& m_state != EFramePresentationOwnerState::WarpReady) {
		RefreshNativePresentationAvailability();
		return;
	}
	for (auto& [surfaceId, registration] : m_nativeSurfaceRegistrations) {
		registration.presentation.deviceEpoch = m_deviceDomain.DeviceEpoch();
		(void)EnsureNativeSurfaceResources(registration);
		(void)surfaceId;
	}
	RefreshNativePresentationAvailability();
}

void FramePresentationOwner::ReleaseNativeSurfaceVisual(
	NativeSurfaceResources& resources) noexcept
{
	if (resources.parentVisual && resources.visual) {
		(void)resources.parentVisual->RemoveVisual(resources.visual.Get());
	}
	resources.visual.Reset();
	resources.parentVisual.Reset();
}

void FramePresentationOwner::ReleaseNativeSurfaceResources() noexcept
{
	for (auto& entry : m_nativeSurfaces) {
		if (entry.second) ReleaseNativeSurfaceVisual(*entry.second);
	}
	m_nativeSurfaces.clear();
	m_nativeTargets.clear();
	RefreshNativePresentationAvailability();
}

void FramePresentationOwner::RefreshNativePresentationAvailability() noexcept
{
	m_nativePresentationAvailable = !m_nativeSurfaces.empty();
}

bool FramePresentationOwner::IsDeviceLoss(const HRESULT failure) const noexcept
{
	return failure == DXGI_ERROR_DEVICE_REMOVED
		|| failure == DXGI_ERROR_DEVICE_RESET
		|| failure == DXGI_ERROR_DRIVER_INTERNAL_ERROR;
}

FramePresentationOwnerResult FramePresentationOwner::RecoverFromDeviceLoss(
	const EFrameDeviceFailureBoundary boundary, const long failureCode,
	const std::uint64_t nowMilliseconds) noexcept
{
	const auto observedEpoch = m_deviceDomain.DeviceEpoch();
	const auto loss = m_deviceDomain.NotifyDeviceLoss(
		observedEpoch, boundary, failureCode);
	if (!loss.Accepted()) {
		if (loss.status == EFrameDeviceTransitionStatus::Closed) {
			return Result(EFramePresentationOwnerStatus::Closed);
		}
		++m_telemetry.failedTransitions;
		return Result(EFramePresentationOwnerStatus::Failed, m_lastHresult);
	}
	m_state = EFramePresentationOwnerState::Recovering;
	if (!m_deviceDomain.BeginQuiesce().Accepted()
		|| !m_deviceDomain.BeginHardwareRecreation().Accepted()) {
		SetFailed(E_UNEXPECTED);
		return Result(EFramePresentationOwnerStatus::Failed, m_lastHresult);
	}
	ReleaseGpuResources();

	GpuResources hardwareResources;
	bool hardwareSucceeded = false;
	if (m_options.allowHardware && !m_options.forceSoftware) {
		++m_telemetry.hardwareCreationAttempts;
		hardwareSucceeded = CreateHardwareResources(hardwareResources);
		if (!hardwareSucceeded) ++m_telemetry.hardwareCreationFailures;
	}
	if (m_deviceDomain.CompleteHardwareRecreation(hardwareSucceeded).Accepted()
		&& hardwareSucceeded) {
		if (!AdoptGpuResources(std::move(hardwareResources))) {
			SetFailed(E_OUTOFMEMORY);
			return Result(EFramePresentationOwnerStatus::Failed, m_lastHresult);
		}
		m_state = EFramePresentationOwnerState::HardwareReady;
		RecreateNativeSurfaceResources();
		++m_telemetry.deviceLossRecoveries;
		return Result(EFramePresentationOwnerStatus::DeviceLost, m_lastHresult);
	}

	GpuResources warpResources;
	bool warpSucceeded = false;
	if (m_options.allowWarp) {
		++m_telemetry.warpCreationAttempts;
		warpSucceeded = CreateWarpResources(warpResources);
		if (!warpSucceeded) ++m_telemetry.warpCreationFailures;
	}
	const auto warpTransition = m_deviceDomain.CompleteWarpCreation(
		warpSucceeded, nowMilliseconds);
	if (!warpTransition.Accepted()) {
		SetFailed(E_UNEXPECTED);
		return Result(EFramePresentationOwnerStatus::Failed, m_lastHresult);
	}
	if (warpSucceeded) {
		if (!AdoptGpuResources(std::move(warpResources))) {
			SetFailed(E_OUTOFMEMORY);
			return Result(EFramePresentationOwnerStatus::Failed, m_lastHresult);
		}
		m_state = EFramePresentationOwnerState::WarpReady;
		RecreateNativeSurfaceResources();
	} else {
		m_state = EFramePresentationOwnerState::SoftwareOnly;
	}
	++m_telemetry.deviceLossRecoveries;
	return Result(EFramePresentationOwnerStatus::DeviceLost, m_lastHresult);
}

FramePresentationOwnerResult FramePresentationOwner::ProbeHardware(
	const std::uint64_t nowMilliseconds) noexcept
{
	const auto begin = m_deviceDomain.BeginHardwareProbe(nowMilliseconds);
	if (!begin.Accepted()) {
		return Result(EFramePresentationOwnerStatus::Backoff, m_lastHresult);
	}
	m_state = EFramePresentationOwnerState::Recovering;
	GpuResources hardwareResources;
	++m_telemetry.hardwareCreationAttempts;
	const bool succeeded = CreateHardwareResources(hardwareResources);
	if (!succeeded) ++m_telemetry.hardwareCreationFailures;
	const auto complete = m_deviceDomain.CompleteHardwareProbe(succeeded, nowMilliseconds);
	if (!complete.Accepted()) {
		SetFailed(E_UNEXPECTED);
		return Result(EFramePresentationOwnerStatus::Failed, m_lastHresult);
	}
	if (succeeded) {
		if (!AdoptGpuResources(std::move(hardwareResources))) {
			SetFailed(E_OUTOFMEMORY);
			return Result(EFramePresentationOwnerStatus::Failed, m_lastHresult);
		}
		m_state = EFramePresentationOwnerState::HardwareReady;
		RecreateNativeSurfaceResources();
		m_lastHresult = S_OK;
	} else {
		SetFallbackState();
	}
	return Result(EFramePresentationOwnerStatus::Succeeded, m_lastHresult);
}

void FramePresentationOwner::ReleaseGpuResources() noexcept
{
	ReleaseNativeSurfaceResources();
	if (m_gpu) {
		m_gpu->compositionDevice.Reset();
		m_gpu->d2dContext.Reset();
		m_gpu->d2dDevice.Reset();
		m_gpu->d2dFactory.Reset();
		m_gpu->immediateContext.Reset();
		m_gpu->device.Reset();
		m_gpu->adapter.Reset();
		m_gpu->factory.Reset();
	}
	m_nativePresentationAvailable = false;
}

bool FramePresentationOwner::AdoptGpuResources(GpuResources&& resources) noexcept
{
	std::unique_ptr<GpuResources> replacement(
		new (std::nothrow) GpuResources(std::move(resources)));
	if (!replacement) {
		m_lastHresult = E_OUTOFMEMORY;
		return false;
	}
	m_gpu = std::move(replacement);
	return true;
}

void FramePresentationOwner::SetFallbackState() noexcept
{
	m_nativePresentationAvailable = false;
	if (m_deviceDomain.State() == EFrameDeviceState::WarpReady) {
		m_state = EFramePresentationOwnerState::WarpReady;
	} else {
		m_state = EFramePresentationOwnerState::SoftwareOnly;
	}
}

void FramePresentationOwner::SetFailed(const HRESULT failure) noexcept
{
	m_lastHresult = failure;
	ReleaseGpuResources();
	m_state = EFramePresentationOwnerState::Failed;
	++m_telemetry.failedTransitions;
}

} // namespace workbench::rendering

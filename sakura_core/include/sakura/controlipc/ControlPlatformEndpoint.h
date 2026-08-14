/*! @file
	@brief Control-process platform endpoint の共有メタデータ
*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#ifndef SAKURA_CONTROLPLATFORMENDPOINT_1A9FDDB0_B4D6_4413_A97B_51D16C9936C6_H_
#define SAKURA_CONTROLPLATFORMENDPOINT_1A9FDDB0_B4D6_4413_A97B_51D16C9936C6_H_
#pragma once

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>

namespace platform::controlipc {

//! Lifecycle values are durable ABI values. Only Accepting is usable by an editor.
enum class ControlPlatformEndpointLifecycle : std::uint32_t {
	Starting = 1,
	Accepting = 2,
	Stopping = 3,
	Stopped = 4,
};

//! Typed terminal result of one endpoint discovery/read operation.  Callers
//! must make retry decisions from this value, never from diagnostic text.
enum class EControlPlatformEndpointDiscoveryDisposition : std::uint8_t {
	Discovered,
	NotPublished,
	NotAccepting,
	DeadOrStale,
	Busy,
	InvalidDescriptor,
	Closed,
	AccessDenied,
	SecurityRejected,
	UnsupportedOrMalformedAbi,
	ResourceOrIoFailure,
};

struct ControlPlatformEndpointSnapshot {
	std::uint32_t controlProcessId = 0;
	std::uint64_t generation = 0;
	ControlPlatformEndpointLifecycle lifecycle = ControlPlatformEndpointLifecycle::Stopped;
	std::wstring profileHash;
	std::wstring pipeName;
	//! Immutable, opaque authority identity. This is published solely by the
	//! control process so an editor never has to reopen profile metadata.
	//!
	//! Kept last so pre-ABI-v2 test seams that use aggregate construction retain
	//! source compatibility; they remain unusable until they populate it.
	std::string profileId;
};

//! Per-reader anti-rollback requirement. Set minimumGeneration after a reconnect or
//! a previously observed endpoint generation; zero accepts any live generation.
struct ControlPlatformEndpointReadRequirements {
	std::uint64_t minimumGeneration = 0;
	bool requireLiveControlProcess = true;
};

struct ControlPlatformEndpointDiscoveryResult {
	EControlPlatformEndpointDiscoveryDisposition disposition =
		EControlPlatformEndpointDiscoveryDisposition::ResourceOrIoFailure;
	std::optional<ControlPlatformEndpointSnapshot> snapshot;
	std::uint32_t errorCode = 0;
	std::wstring diagnostic;

	[[nodiscard]] bool IsDiscovered() const noexcept
	{
		return disposition == EControlPlatformEndpointDiscoveryDisposition::Discovered && snapshot.has_value();
	}
};

//! A dedicated shared-memory ABI. It is intentionally unrelated to DLLSHAREDATA and
//! platform-service endpoint state. One control process writes; editor processes read a
//! bounded seqlock snapshot.
class CControlPlatformEndpoint final {
public:
	CControlPlatformEndpoint() = default;
	~CControlPlatformEndpoint();
	CControlPlatformEndpoint(const CControlPlatformEndpoint&) = delete;
	CControlPlatformEndpoint& operator=(const CControlPlatformEndpoint&) = delete;

	bool CreateForControl(const std::filesystem::path& profileDirectory, std::wstring& diagnostic);
	bool OpenForEditor(const std::filesystem::path& profileDirectory, std::wstring& diagnostic);
	//! Typed counterpart of OpenForEditor.  The bool overload is retained for
	//! existing composition code that only needs a diagnostic convenience.
	[[nodiscard]] ControlPlatformEndpointDiscoveryResult OpenForEditorDetailed(
		const std::filesystem::path& profileDirectory);
	void Close() noexcept;

	bool Publish(const ControlPlatformEndpointSnapshot& snapshot, std::wstring& diagnostic);
	[[nodiscard]] std::optional<ControlPlatformEndpointSnapshot> Read(
		const ControlPlatformEndpointReadRequirements& requirements = {}) const;
	[[nodiscard]] ControlPlatformEndpointDiscoveryResult ReadDetailed(
		const ControlPlatformEndpointReadRequirements& requirements = {}) const;
	[[nodiscard]] const std::wstring& MappingName() const noexcept { return m_mappingName; }
	[[nodiscard]] const std::wstring& ProfileHash() const noexcept { return m_profileHash; }

	//! Validates an untrusted decoded endpoint snapshot without dereferencing shared memory.
	static bool IsSnapshotUsable(
		const ControlPlatformEndpointSnapshot& snapshot,
		std::wstring_view expectedProfileHash,
		const ControlPlatformEndpointReadRequirements& requirements = {}) noexcept;
	//! Classifies a decoded snapshot before it is used for a pipe connection.
	[[nodiscard]] static EControlPlatformEndpointDiscoveryDisposition ClassifySnapshot(
		const ControlPlatformEndpointSnapshot& snapshot,
		std::wstring_view expectedProfileHash,
		const ControlPlatformEndpointReadRequirements& requirements = {}) noexcept;

private:
	struct SharedBlock;

	//! Opaque native mapping handle. The public contract does not expose Win32 types.
	void* m_mapping = nullptr;
	SharedBlock* m_block = nullptr;
	bool m_writer = false;
	std::wstring m_mappingName;
	std::wstring m_profileHash;
	mutable std::shared_mutex m_publishMutex;
};

} // namespace platform::controlipc

#endif /* SAKURA_CONTROLPLATFORMENDPOINT_1A9FDDB0_B4D6_4413_A97B_51D16C9936C6_H_ */

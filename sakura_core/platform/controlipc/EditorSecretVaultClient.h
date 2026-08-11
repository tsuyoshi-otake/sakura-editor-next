/*! @file
	@brief Narrow editor-side Secret Vault client over the authenticated control IPC.
*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <sakura/controlipc/ControlIpcTransport.h>
#include "platform/controlipc/ControlPlatformEndpoint.h"
#include "platform/controlipc/ControlSecretVaultRpc.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace platform::controlipc {

class IControlPlatformEndpointReader;

//! A copied, untrusted endpoint snapshot reader.  Discovery itself owns no retry loop.
class IEditorSecretVaultEndpointReader {
public:
	virtual ~IEditorSecretVaultEndpointReader() = default;
	[[nodiscard]] virtual ControlPlatformEndpointDiscoveryResult Read(
		const ControlPlatformEndpointReadRequirements& requirements) = 0;
	virtual void Close() noexcept {}
};

//! Bridges the established endpoint discovery reader without coupling the vault client to storage cache state.
class CEditorSecretVaultEndpointReaderAdapter final : public IEditorSecretVaultEndpointReader {
public:
	explicit CEditorSecretVaultEndpointReaderAdapter(IControlPlatformEndpointReader& reader) noexcept : m_reader(reader) {}
	[[nodiscard]] ControlPlatformEndpointDiscoveryResult Read(
		const ControlPlatformEndpointReadRequirements& requirements) override;
	void Close() noexcept override;

private:
	IControlPlatformEndpointReader& m_reader;
};

//! One authenticated named-pipe connection.  Each client operation creates one instance.
class IEditorSecretVaultChannel {
public:
	virtual ~IEditorSecretVaultChannel() = default;
	[[nodiscard]] virtual ControlIpcTransportResult Connect(const ControlPlatformEndpointSnapshot& endpoint,
		std::chrono::milliseconds deadline) = 0;
	[[nodiscard]] virtual ControlIpcTransportResult Exchange(const ControlIpcFrame& request,
		std::vector<ControlIpcFrame>& responses, std::chrono::milliseconds deadline) = 0;
	virtual void Close() noexcept = 0;
};

//! Production named-pipe adapter. Tests inject IEditorSecretVaultChannel instead.
class CEditorSecretVaultNamedPipeChannel final : public IEditorSecretVaultChannel {
public:
	[[nodiscard]] ControlIpcTransportResult Connect(const ControlPlatformEndpointSnapshot& endpoint,
		std::chrono::milliseconds deadline) override;
	[[nodiscard]] ControlIpcTransportResult Exchange(const ControlIpcFrame& request,
		std::vector<ControlIpcFrame>& responses, std::chrono::milliseconds deadline) override;
	void Close() noexcept override;

private:
	CControlIpcNamedPipeClient m_client;
};

//! Caller-provided values must have been authenticated/validated by extension-host composition.
struct EditorSecretVaultCallerIdentity {
	std::string extensionHostSessionId;
	std::uint64_t hostGeneration = 0;
	//! Exact canonical lowercase extension identity; this client never canonicalizes a caller input.
	std::string canonicalExtensionId;
};

struct EditorSecretVaultGetRequest {
	EditorSecretVaultCallerIdentity caller;
	std::string key;
};

struct EditorSecretVaultApplyRequest {
	EditorSecretVaultCallerIdentity caller;
	//! Must carry the caller's exact canonical extensionId and durable operationId.
	secrets::SecretMutationRequest mutation;
};

enum class EEditorSecretVaultOutcome : std::uint8_t {
	Succeeded,
	NotFound,
	NotApplicable,
	Conflict,
	RetryWithSameOperationId,
	OperationInFlight,
	Stopped,
	Unavailable,
	Failed,
};

//! Results deliberately contain no free-form peer diagnostics. GetResult is the only value-bearing result.
struct EditorSecretVaultGetResult {
	EEditorSecretVaultOutcome outcome = EEditorSecretVaultOutcome::Failed;
	EControlIpcTerminalStatus terminalStatus = EControlIpcTerminalStatus::InternalError;
	EControlPlatformEndpointDiscoveryDisposition discoveryDisposition =
		EControlPlatformEndpointDiscoveryDisposition::ResourceOrIoFailure;
	EControlIpcTransportDisconnectReason transportReason = EControlIpcTransportDisconnectReason::None;
	secrets::SecretGetResult result;
};

struct EditorSecretVaultApplyResult {
	EEditorSecretVaultOutcome outcome = EEditorSecretVaultOutcome::Failed;
	EControlIpcTerminalStatus terminalStatus = EControlIpcTerminalStatus::InternalError;
	EControlPlatformEndpointDiscoveryDisposition discoveryDisposition =
		EControlPlatformEndpointDiscoveryDisposition::ResourceOrIoFailure;
	EControlIpcTransportDisconnectReason transportReason = EControlIpcTransportDisconnectReason::None;
	secrets::SecretMutationResult result;
};

struct EditorSecretVaultRevokeResult {
	EEditorSecretVaultOutcome outcome = EEditorSecretVaultOutcome::Failed;
	EControlIpcTerminalStatus terminalStatus = EControlIpcTerminalStatus::InternalError;
	EControlPlatformEndpointDiscoveryDisposition discoveryDisposition =
		EControlPlatformEndpointDiscoveryDisposition::ResourceOrIoFailure;
	EControlIpcTransportDisconnectReason transportReason = EControlIpcTransportDisconnectReason::None;
};

struct EditorSecretVaultClientOptions {
	//! Immutable authority identity supplied once by editor process composition.
	std::string profileId;
	std::wstring profileHash;
	//! The editor accepts only this already-trusted control authority generation.
	std::uint64_t pinnedControlGeneration = 0;
	std::chrono::milliseconds exchangeDeadline = std::chrono::seconds(5);
	//! Kept short even though the server permits longer capability lifetimes.
	std::chrono::milliseconds capabilityLifetime = std::chrono::seconds(30);
	std::function<std::unique_ptr<IEditorSecretVaultChannel>()> channelFactory;
};

/*! 
	@brief Stateful stop gate around otherwise fresh, synchronous Secret Vault exchanges.

	Get/Store/Delete never reuse a pipe or capability. Exactly one caller may own
	an exchange; Stop closes that one channel and is terminal. A lost transport
	after Apply has been sent is deliberately observable as RetryWithSameOperationId
	and never causes a hidden retry or replacement operation ID.
*/
class IEditorSecretVaultClient {
public:
	virtual ~IEditorSecretVaultClient() = default;
	[[nodiscard]] virtual EditorSecretVaultGetResult Get(const EditorSecretVaultGetRequest& request) = 0;
	[[nodiscard]] virtual EditorSecretVaultApplyResult Store(const EditorSecretVaultApplyRequest& request) = 0;
	[[nodiscard]] virtual EditorSecretVaultApplyResult Delete(const EditorSecretVaultApplyRequest& request) = 0;
	[[nodiscard]] virtual EditorSecretVaultRevokeResult RevokeSession() = 0;
	virtual void Stop() noexcept = 0;
	[[nodiscard]] virtual bool IsStopped() const noexcept = 0;
};

class CEditorSecretVaultClient final : public IEditorSecretVaultClient {
public:
	CEditorSecretVaultClient(EditorSecretVaultClientOptions options, IEditorSecretVaultEndpointReader& endpointReader);
	~CEditorSecretVaultClient();
	CEditorSecretVaultClient(const CEditorSecretVaultClient&) = delete;
	CEditorSecretVaultClient& operator=(const CEditorSecretVaultClient&) = delete;

	[[nodiscard]] EditorSecretVaultGetResult Get(const EditorSecretVaultGetRequest& request) override;
	[[nodiscard]] EditorSecretVaultApplyResult Store(const EditorSecretVaultApplyRequest& request) override;
	[[nodiscard]] EditorSecretVaultApplyResult Delete(const EditorSecretVaultApplyRequest& request) override;
	[[nodiscard]] EditorSecretVaultRevokeResult RevokeSession() override;
	void Stop() noexcept override;
	[[nodiscard]] bool IsStopped() const noexcept override;

private:
	[[nodiscard]] bool Begin(std::shared_ptr<IEditorSecretVaultChannel>& channel) noexcept;
	void Finish(const std::shared_ptr<IEditorSecretVaultChannel>& channel) noexcept;
	[[nodiscard]] bool IsOptionsValid() const noexcept;
	[[nodiscard]] bool IsCallerValid(const EditorSecretVaultCallerIdentity& caller) const noexcept;
	[[nodiscard]] bool IsMutationValid(const EditorSecretVaultApplyRequest& request,
		secrets::ESecretMutationKind expectedKind) const noexcept;
	[[nodiscard]] std::optional<ControlPlatformEndpointSnapshot> Discover(
		EControlPlatformEndpointDiscoveryDisposition& disposition) const;
	[[nodiscard]] std::optional<ControlIpcFrame> ExchangeTerminal(IEditorSecretVaultChannel& channel,
		const ControlIpcFrame& request, EControlIpcKind expectedKind, bool applyWasSent,
		EControlIpcTerminalStatus& terminalStatus, EControlIpcTransportDisconnectReason& transportReason) const;
	[[nodiscard]] std::optional<std::uint64_t> NextRequestId() noexcept;
	[[nodiscard]] EditorSecretVaultGetResult GetStoppedResult() const noexcept;
	[[nodiscard]] EditorSecretVaultApplyResult ApplyStoppedResult() const noexcept;
	[[nodiscard]] EditorSecretVaultApplyResult ApplyInternal(const EditorSecretVaultApplyRequest& request,
		secrets::ESecretMutationKind expectedKind);

	EditorSecretVaultClientOptions m_options;
	IEditorSecretVaultEndpointReader& m_endpointReader;
	mutable std::mutex m_mutex;
	bool m_stopped = false;
	bool m_operationInFlight = false;
	std::uint64_t m_nextRequestId = 1;
	std::shared_ptr<IEditorSecretVaultChannel> m_activeChannel;
};

} // namespace platform::controlipc

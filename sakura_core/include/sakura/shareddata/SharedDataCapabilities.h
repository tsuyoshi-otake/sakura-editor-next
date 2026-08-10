/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#pragma once

#include <cstdint>
#include <optional>

struct DLLSHAREDATA;

namespace legacy::shareddata {

//! A value copy of the fixed mapping header. This is the compatibility gate, not mutable settings state.
class SharedDataHeaderSnapshot final {
public:
	constexpr SharedDataHeaderSnapshot(std::uint32_t structureVersion, std::uint32_t mappingSize) noexcept
		: m_structureVersion(structureVersion), m_mappingSize(mappingSize) {}
	[[nodiscard]] constexpr std::uint32_t StructureVersion() const noexcept { return m_structureVersion; }
	[[nodiscard]] constexpr std::uint32_t MappingSize() const noexcept { return m_mappingSize; }
private:
	const std::uint32_t m_structureVersion;
	const std::uint32_t m_mappingSize;
};

//! A value copy of the macro-recording flags. Native HWND values are intentionally opaque integers here.
class SharedDataMacroSnapshot final {
public:
	constexpr SharedDataMacroSnapshot(bool editWindowChanging, bool recording, std::uintptr_t recordingWindow) noexcept
		: m_editWindowChanging(editWindowChanging), m_recording(recording), m_recordingWindow(recordingWindow) {}
	[[nodiscard]] constexpr bool IsEditWindowChanging() const noexcept { return m_editWindowChanging; }
	[[nodiscard]] constexpr bool IsRecording() const noexcept { return m_recording; }
	[[nodiscard]] constexpr std::uintptr_t RecordingWindow() const noexcept { return m_recordingWindow; }
private:
	const bool m_editWindowChanging;
	const bool m_recording;
	const std::uintptr_t m_recordingWindow;
};

//! A value copy of process-owned window endpoints. The facade does not assign ownership to these handles.
class SharedDataWindowEndpointSnapshot final {
public:
	constexpr SharedDataWindowEndpointSnapshot(std::uintptr_t trayWindow, std::uintptr_t debugWindow) noexcept
		: m_trayWindow(trayWindow), m_debugWindow(debugWindow) {}
	[[nodiscard]] constexpr std::uintptr_t TrayWindow() const noexcept { return m_trayWindow; }
	[[nodiscard]] constexpr std::uintptr_t DebugWindow() const noexcept { return m_debugWindow; }
private:
	const std::uintptr_t m_trayWindow;
	const std::uintptr_t m_debugWindow;
};

//! A value copy of the settings-mapping counters. Lock ownership remains CShareDataLockCounter's responsibility.
class SharedDataSettingsSnapshot final {
public:
	constexpr SharedDataSettingsSnapshot(int typeCount, int lockCount) noexcept
		: m_typeCount(typeCount), m_lockCount(lockCount) {}
	[[nodiscard]] constexpr int TypeCount() const noexcept { return m_typeCount; }
	[[nodiscard]] constexpr int LockCount() const noexcept { return m_lockCount; }
private:
	const int m_typeCount;
	const int m_lockCount;
};

//! A value copy of the search settings consumed by command-routing decisions.
class SharedDataSearchSettingsSnapshot final {
public:
	constexpr SharedDataSearchSettingsSnapshot(bool useCaretKeyword, bool tagJumpOnDoubleClick,
		bool tagJumpOnReturn, bool enableExtendedEol) noexcept
		: m_useCaretKeyword(useCaretKeyword), m_tagJumpOnDoubleClick(tagJumpOnDoubleClick),
		m_tagJumpOnReturn(tagJumpOnReturn), m_enableExtendedEol(enableExtendedEol) {}
	[[nodiscard]] constexpr bool UsesCaretKeyword() const noexcept { return m_useCaretKeyword; }
	[[nodiscard]] constexpr bool TagJumpOnDoubleClick() const noexcept { return m_tagJumpOnDoubleClick; }
	[[nodiscard]] constexpr bool TagJumpOnReturn() const noexcept { return m_tagJumpOnReturn; }
	[[nodiscard]] constexpr bool EnablesExtendedEol() const noexcept { return m_enableExtendedEol; }
private:
	const bool m_useCaretKeyword;
	const bool m_tagJumpOnDoubleClick;
	const bool m_tagJumpOnReturn;
	const bool m_enableExtendedEol;
};

//! A value copy of the active editor-window count.
class SharedDataWindowNodesSnapshot final {
public:
	constexpr explicit SharedDataWindowNodesSnapshot(int editWindowCount) noexcept : m_editWindowCount(editWindowCount) {}
	[[nodiscard]] constexpr int EditWindowCount() const noexcept { return m_editWindowCount; }
private:
	const int m_editWindowCount;
};

class SharedDataCapabilities;

//! Read-only capability for the immutable mapping header.
class SharedDataHeaderReader final {
public:
	[[nodiscard]] SharedDataHeaderSnapshot Snapshot() const noexcept;

private:
	explicit SharedDataHeaderReader(const DLLSHAREDATA* data) noexcept : m_data(data) {}
	const DLLSHAREDATA* m_data;
	friend class SharedDataCapabilities;
};

//! Read-only capability for macro-recording process state.
class SharedDataMacroReader final {
public:
	[[nodiscard]] SharedDataMacroSnapshot Snapshot() const noexcept;

private:
	explicit SharedDataMacroReader(const DLLSHAREDATA* data) noexcept : m_data(data) {}
	const DLLSHAREDATA* m_data;
	friend class SharedDataCapabilities;
};

//! Writer for macro-recording process state. Starting and stopping recording are explicit terminal transitions.
class SharedDataMacroWriter final {
public:
	void SetEditWindowChanging(bool changing) noexcept;
	void StartRecording(std::uintptr_t ownerWindow) noexcept;
	void StopRecording() noexcept;

private:
	explicit SharedDataMacroWriter(DLLSHAREDATA* data) noexcept : m_data(data) {}
	DLLSHAREDATA* m_data;
	friend class SharedDataCapabilities;
};

//! Read-only capability for process window endpoints.
class SharedDataWindowEndpointReader final {
public:
	[[nodiscard]] SharedDataWindowEndpointSnapshot Snapshot() const noexcept;

private:
	explicit SharedDataWindowEndpointReader(const DLLSHAREDATA* data) noexcept : m_data(data) {}
	const DLLSHAREDATA* m_data;
	friend class SharedDataCapabilities;
};

//! Writer for process window endpoints. It stores opaque endpoints and never owns, closes, or dispatches them.
class SharedDataWindowEndpointWriter final {
public:
	void SetTrayWindow(std::uintptr_t window) noexcept;
	void SetDebugWindow(std::uintptr_t window) noexcept;

private:
	explicit SharedDataWindowEndpointWriter(DLLSHAREDATA* data) noexcept : m_data(data) {}
	DLLSHAREDATA* m_data;
	friend class SharedDataCapabilities;
};

//! Read-only capability for mapping counters.
class SharedDataSettingsReader final {
public:
	[[nodiscard]] SharedDataSettingsSnapshot Snapshot() const noexcept;

private:
	explicit SharedDataSettingsReader(const DLLSHAREDATA* data) noexcept : m_data(data) {}
	const DLLSHAREDATA* m_data;
	friend class SharedDataCapabilities;
};

//! Writer for mapping counters. Arbitrary lock-count assignment is deliberately unavailable.
class SharedDataSettingsWriter final {
public:
	void SetTypeCount(int count) noexcept;
	[[nodiscard]] int IncrementLockCount() noexcept;
	[[nodiscard]] int DecrementLockCount() noexcept;

private:
	explicit SharedDataSettingsWriter(DLLSHAREDATA* data) noexcept : m_data(data) {}
	DLLSHAREDATA* m_data;
	friend class SharedDataCapabilities;
};

//! Read-only capability for search settings used by command dispatch.
class SharedDataSearchSettingsReader final {
public:
	[[nodiscard]] SharedDataSearchSettingsSnapshot Snapshot() const noexcept;
private:
	explicit SharedDataSearchSettingsReader(const DLLSHAREDATA* data) noexcept : m_data(data) {}
	const DLLSHAREDATA* m_data;
	friend class SharedDataCapabilities;
};

//! Writer for the explicitly mutable command search preference.
class SharedDataSearchSettingsWriter final {
public:
	void SetUseCaretKeyword(bool enabled) noexcept;
private:
	explicit SharedDataSearchSettingsWriter(DLLSHAREDATA* data) noexcept : m_data(data) {}
	DLLSHAREDATA* m_data;
	friend class SharedDataCapabilities;
};

//! Read-only capability for active editor-window nodes.
class SharedDataWindowNodesReader final {
public:
	[[nodiscard]] SharedDataWindowNodesSnapshot Snapshot() const noexcept;
private:
	explicit SharedDataWindowNodesReader(const DLLSHAREDATA* data) noexcept : m_data(data) {}
	const DLLSHAREDATA* m_data;
	friend class SharedDataCapabilities;
};

//! A short-lived, mapping-bound set of capability facades. This is not an application context or service locator.
class SharedDataCapabilities final {
public:
	[[nodiscard]] SharedDataHeaderReader Header() const noexcept { return SharedDataHeaderReader(m_data); }
	[[nodiscard]] SharedDataMacroReader Macro() const noexcept { return SharedDataMacroReader(m_data); }
	[[nodiscard]] SharedDataMacroWriter MacroWriter() const noexcept { return SharedDataMacroWriter(m_data); }
	[[nodiscard]] SharedDataWindowEndpointReader WindowEndpoints() const noexcept { return SharedDataWindowEndpointReader(m_data); }
	[[nodiscard]] SharedDataWindowEndpointWriter WindowEndpointWriter() const noexcept { return SharedDataWindowEndpointWriter(m_data); }
	[[nodiscard]] SharedDataSettingsReader Settings() const noexcept { return SharedDataSettingsReader(m_data); }
	[[nodiscard]] SharedDataSettingsWriter SettingsWriter() const noexcept { return SharedDataSettingsWriter(m_data); }
	[[nodiscard]] SharedDataSearchSettingsReader SearchSettings() const noexcept { return SharedDataSearchSettingsReader(m_data); }
	[[nodiscard]] SharedDataSearchSettingsWriter SearchSettingsWriter() const noexcept { return SharedDataSearchSettingsWriter(m_data); }
	[[nodiscard]] SharedDataWindowNodesReader WindowNodes() const noexcept { return SharedDataWindowNodesReader(m_data); }

private:
	explicit SharedDataCapabilities(DLLSHAREDATA* data) noexcept : m_data(data) {}
	DLLSHAREDATA* m_data;
	friend SharedDataCapabilities OpenSharedDataCapabilities(DLLSHAREDATA& data) noexcept;
	friend std::optional<SharedDataCapabilities> TryOpenSharedDataCapabilities() noexcept;
};

//! Binds narrow facades to a mapping whose lifetime is externally owned by CShareData.
[[nodiscard]] SharedDataCapabilities OpenSharedDataCapabilities(DLLSHAREDATA& data) noexcept;

//! Opens the legacy process mapping when initialized; absence is explicit instead of throwing from a global accessor.
[[nodiscard]] std::optional<SharedDataCapabilities> TryOpenSharedDataCapabilities() noexcept;

//! Opens the required mapping for legacy command paths; an unavailable mapping is an explicit terminal failure.
[[nodiscard]] SharedDataCapabilities RequireSharedDataCapabilities();

} // namespace legacy::shareddata

/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "terminal/window/TerminalTabManager.h"
#include "terminal/window/TerminalNativeFrameBridge.h"
#include "TerminalTabPresentation.h"
#include "terminal/window/TerminalAgentPresence.h"
#include "terminal/window/TerminalGroupLayout.h"
#include "workbench/IWorkbenchTool.h"
#include "theme/CThemeService.h"
#include "terminal/window/TerminalPaneLayout.h"
#include "terminal/input/TerminalShortcutPreset.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace terminal {

class CTerminalWnd;

//! Frame-owned actions exposed to the terminal chrome without coupling it to CEditWnd.
struct TerminalPanelActions {
	std::function<void()> closePanel;
	std::function<void()> toggleMaximize;
	std::function<bool()> isMaximized;
	//! When false, the physical Panel Part draws maximize/close in its own chrome.
	bool renderPanelActions = true;
	//! When false, the physical Panel Part supplies the one shared header row and
	//! this tool renders only the terminal content surface.
	bool renderHeader = true;
	//! Executes a stable workbench command ID (`workbench.action.tasks.*`) for the
	//! Terminal menu's task entries. Empty means the host has no command surface.
	std::function<void(std::string_view)> runCommand;
};

enum class TerminalWorkspaceResetOutcome : std::uint8_t {
	Cleared,
	Restarted,
	RestartFailed,
	Busy,
	Unavailable,
};

struct TerminalWorkspaceResetResult {
	TerminalWorkspaceResetOutcome outcome{ TerminalWorkspaceResetOutcome::Unavailable };
	std::size_t clearedTabCount{};
	bool closeDeadlineExceeded{};
	std::uint32_t errorCode{};
};

enum class TerminalWorkspaceSwitchOutcome : std::uint8_t {
	Unchanged,
	Detached,
	Attached,
	Created,
	StartFailed,
	AttachFailed,
	RestoreFailed,
	Unavailable,
};

struct TerminalWorkspaceSwitchRequest final {
	std::wstring oldWorkspaceIdentityKey;
	std::wstring newWorkspaceIdentityKey;
	std::wstring newWorkingDirectory;
	bool terminalVisible{};
};

struct TerminalWorkspaceSwitchResult final {
	TerminalWorkspaceSwitchOutcome outcome{ TerminalWorkspaceSwitchOutcome::Unavailable };
	std::size_t savedTabCount{};
	std::size_t restoredTabCount{};
	std::size_t startedTabCount{};
	bool oldProjectionRestored{};
	std::uint32_t errorCode{};

	[[nodiscard]] constexpr bool Succeeded() const noexcept
	{
		return outcome == TerminalWorkspaceSwitchOutcome::Unchanged
			|| outcome == TerminalWorkspaceSwitchOutcome::Detached
			|| outcome == TerminalWorkspaceSwitchOutcome::Attached
			|| outcome == TerminalWorkspaceSwitchOutcome::Created;
	}
};

//! One integrated terminal of this window that hosts a recognized AI agent CLI
//! (Issue #402). `instanceId` is the stable row identity; `tabId` is the
//! window-local handle SelectTerminal accepts. Neither is a process ID.
struct TerminalAgentPresenceRecord final {
	std::uint64_t tabId{};
	TerminalInstanceId instanceId;
	TerminalAgentKind kind{};
	//! TerminalAgentSessionTitle of the tab's OSC title, for the row's hover
	//! text; empty when the agent announced none.
	std::wstring sessionTitle;
	//! TerminalAgentPresenceTracker::StartedUnixSeconds of the tab: when the
	//! agent process started (or when its title-only presence first resolved).
	//! Zero when no start anchor exists.
	std::int64_t startedUnixSeconds{};
};

//! Where CTerminalTool::RunInFirstPromptTerminalOrAdd queued its input.
struct TerminalInputTarget final {
	std::uint64_t tabId{};
	//! True when an existing terminal at its first prompt received the input;
	//! false when a new terminal was opened for it.
	bool reused{};
};

//! The current projection's group layout as plain data (#415 Phase 3):
//! `groups` in panel order with tab-id trees, plus the focused/active
//! terminal's tab id (absent when no tab is part of a group).
struct TerminalGroupLayoutSnapshot final {
	std::vector<TerminalPaneGroup> groups;
	std::optional<std::uint64_t> activeTabId;
};

//! One row of the terminal list as presented (#415 Phase 4): the tab, its
//! split-group position, and the resolved texts. `label` already carries the
//! tree-glyph prefix that upstream terminalTabsList.ts puts inside the label
//! (U+250C/U+251C/U+2514 plus a space); `description` is the dimmed text.
struct TerminalListRow final {
	TerminalTabSnapshot tab;
	TerminalSplitPosition splitPosition{ TerminalSplitPosition::None };
	std::wstring label;
	std::wstring description;
};

//! One terminal a layout restore installs. `input` queued after the session
//! starts is what `ResumeAgentSessionInTerminal` builds (the resume command
//! plus CR); an empty `input` makes it a plain shell. `reuseFirstPrompt`
//! applies the Phase-1 rule: a terminal still at its first prompt in
//! `workingDirectory` with no agent runs `input` instead of a new terminal
//! being opened. `drop` leaves never get a terminal and never count as
//! failures; the caller drops and accounts for them itself.
struct TerminalRestoreLeaf final {
	std::wstring workingDirectory;
	std::string input;
	bool reuseFirstPrompt{};
	bool drop{};
};

//! One node of a group tree a restore installs: a leaf (`leaf` indexes the
//! group's `leaves`) or a split (`children` node indices with matching
//! `weights`, `orientation`).
struct TerminalRestoreNode final {
	std::optional<std::size_t> leaf;
	TerminalPaneOrientation orientation{ TerminalPaneOrientation::Horizontal };
	std::vector<std::size_t> children;
	std::vector<int> weights;
};

//! One group a restore installs, in panel order.
struct TerminalRestoreGroup final {
	std::vector<TerminalRestoreLeaf> leaves;
	std::vector<TerminalRestoreNode> nodes;
	std::size_t root{};
};

//! What RestoreTerminalGroupLayout did. `leafTabIds` is parallel to each
//! group's `leaves`: the placed tab id, or none when the leaf got no
//! terminal -- it was dropped, its terminal could not be created or refused
//! its input, or its group's tree was rejected and rolled back (a reused
//! first-prompt tab returns to a single-pane group, created ones are
//! deleted).
struct TerminalRestoreResult final {
	std::vector<std::vector<std::optional<std::uint64_t>>> leafTabIds;
};

//! Bottom-panel terminal tool with recursive split groups and a right-side
//! terminal instance list. Pane count is unbounded.
class CTerminalTool final : public workbench::IWorkbenchTool {
public:
	explicit CTerminalTool( TerminalTabManagerDependencies dependencies = {} );
	~CTerminalTool() override;
	CTerminalTool( const CTerminalTool& ) = delete;
	CTerminalTool& operator=( const CTerminalTool& ) = delete;

	bool Create( HWND parent ) override;
	void Layout( const RECT& contentRect, unsigned int dpi ) override;
	void Activate() override;
	void Deactivate() override;
	bool PreTranslateMessage( MSG& message ) override;
	void Close() override;
	//! Commits pane frame projections only after the parent has flushed its real
	//! post-paint GDI boundary. No paint, wait, or timer is performed here.
	void CommitGdiFrames() noexcept;
	//! Advances every pane to a recovered presentation device epoch. The call is
	//! UI-owned, bounded by the pane count, and never waits for a renderer.
	void NotifyFrameDeviceEpoch(std::uint64_t epoch) noexcept;
	//! Connects pane HWNDs to the owning frame runtime. The bridge is shared by
	//! all currently visible panes and by panes materialized later.
	void SetNativeFrameRuntimeBridge(TerminalNativeFrameBridgePtr bridge) noexcept;
	//! Explicitly withdraws every pane surface and fences the bridge before the
	//! frame runtime is retired. This operation never waits for a worker.
	void DetachNativeFrameRuntime() noexcept;

	void SetWorkingDirectory( std::wstring workingDirectory );
	//! Switches the terminal projection between process-local workspace slots
	//! keyed by WorkspaceContextSnapshot::workspaceIdentityKey. Live runtime
	//! instances are detached and reattached by typed coordinates; a failed
	//! target attach/start restores the old projection before returning.
	[[nodiscard]] TerminalWorkspaceSwitchResult SwitchWorkspace(
		TerminalWorkspaceSwitchRequest request );
	//! Destructive identity-less reset retained for explicit teardown callers.
	//! Normal workspace transitions call SwitchWorkspace with stable keys.
	[[nodiscard]] TerminalWorkspaceResetResult ResetForWorkspace(
		std::wstring workingDirectory, bool recreateSession );
	void SetPalette( const theme::ThemePalette& palette );
	void SetPanelActions( TerminalPanelActions actions );
	//! Sets the physical Panel Part that owns the terminal-specific header actions.
	//! A null host restores standalone-header invalidation.
	void SetPanelHeaderHost( HWND host );
	//! Paints the terminal-specific actions into a host-owned, single-row header.
	void PaintPanelHeader( HDC dc, const RECT& bounds, unsigned int dpi );
	//! Forwards mouse input from the physical Panel Part's header region.
	[[nodiscard]] bool HandlePanelHeaderMessage(
		UINT message, WPARAM wParam, LPARAM lParam, const RECT& bounds, unsigned int dpi );
	//! Materializes the renderer and starts exactly one initial session without
	//! moving keyboard focus. Used when a persisted-visible panel is restored.
	[[nodiscard]] bool EnsureSessionStarted();
	[[nodiscard]] std::optional<std::uint64_t> AddTerminal();
	//! VS Code's createTerminal({ cwd }) + sendText: opens a new terminal group
	//! started in `workingDirectory` (the default directory when empty) and
	//! queues `input` to its shell. nullopt when no terminal was created or its
	//! session did not accept the input; like AddTerminal, a tab whose session
	//! failed to start stays visible so the failure can be seen and restarted.
	[[nodiscard]] std::optional<std::uint64_t> AddTerminalWithInput(
		std::wstring_view workingDirectory, std::string_view input );
	//! #415: AddTerminalWithInput, except that a terminal of this projection
	//! still at its first prompt receives `input` instead of a new terminal
	//! being opened beside it. A terminal qualifies when IsTerminalAtFirstPrompt
	//! holds for `workingDirectory` (the default directory when empty) and it
	//! hosts no agent; the active terminal is tried first, then list order. A
	//! terminal that refuses the input is skipped. Selection and focus are left
	//! to the caller, as with AddTerminalWithInput.
	[[nodiscard]] std::optional<TerminalInputTarget> RunInFirstPromptTerminalOrAdd(
		std::wstring_view workingDirectory, std::string_view input );
	void RedetectPowerShell();
	//! Selects the multiplexer keybinding preset without persisting it. Used when
	//! the window applies the effective setting.
	void SetShortcutPreset( TerminalShortcutPreset preset );
	[[nodiscard]] TerminalShortcutPreset ShortcutPreset() const noexcept;
	//! Applies one coherent terminal.integrated.tabs.* projection. This is plain
	//! presentation data; it never reaches TerminalTabManager or restarts PTYs.
	void SetTabPresentationSettings( TerminalTabPresentationSettings settings );
	//! Applies terminal.integrated.scrollback to all existing and future models.
	//! Shrinking discards the oldest retained rows without restarting a PTY.
	void SetScrollbackLimit( std::size_t lines );
	[[nodiscard]] std::size_t ScrollbackLimit() const noexcept;
	//! Called when the user picks a preset from the terminal menu, so the window
	//! can persist the selection. Never called by SetShortcutPreset.
	void SetShortcutPresetSink( std::function<void(TerminalShortcutPreset)> sink );
	//! Runs one key through the active preset. Returns true when the preset owns
	//! the key, meaning it must not reach the shell. PreTranslateMessage uses this
	//! after reading the physical modifier state.
	[[nodiscard]] bool DispatchShortcutPresetKey( const TerminalPresetKey& key );
	[[nodiscard]] bool SelectTerminal( std::uint64_t tabId );
	[[nodiscard]] bool RestartTerminal( std::uint64_t tabId );
	[[nodiscard]] bool DeleteTerminal( std::uint64_t tabId );
	[[nodiscard]] bool SplitTerminalRight();
	[[nodiscard]] bool SplitTerminalDown();
	[[nodiscard]] bool CloseTerminalSplit();
	//! Moves a split terminal to a new last group, leaving its old group active.
	[[nodiscard]] bool UnsplitTerminal( std::optional<std::uint64_t> tabId = std::nullopt );
	//! Alone, non-active terminals in the terminal list's order.
	[[nodiscard]] std::vector<std::uint64_t> JoinCandidates() const;
	//! Joins the picked terminal and the active terminal, selecting the picked one.
	[[nodiscard]] bool JoinTerminals( std::uint64_t pickedTabId );
	[[nodiscard]] bool HasTerminalSplit() const noexcept;
	[[nodiscard]] TerminalPaneOrientation ActivePaneOrientation() const noexcept;
	[[nodiscard]] std::vector<TerminalTabSnapshot> Tabs() const;
	//! The terminal list's rows in display order (#415 Phase 4): groups in
	//! order, each group's terminals in leaf order (upstream's `instances`).
	//! A tab not yet installed in a group keeps its creation-order place at
	//! the end. Painting, hit testing, Up/Down navigation, scroll-into-view
	//! and the Sessions menu all read this same order.
	[[nodiscard]] std::vector<TerminalListRow> TerminalListRows() const;
	//! The current projection's groups, split trees, and active tab as plain
	//! data (#415 Phase 3): what PersistLiveAgentSessions records.
	[[nodiscard]] TerminalGroupLayoutSnapshot GroupLayoutSnapshot() const;
	//! Installs planned terminal groups in one transaction (#415 Phase 3):
	//! one terminal per leaf (a `reuseFirstPrompt` session leaf may take the
	//! untouched default shell, which then leaves its old group for its place
	//! in the restored tree), each group's tree appended after the
	//! projection's existing groups through TerminalGroupLayout::InstallGroup,
	//! renderers rebuilt once, and `activeLeaf` (group, leaf index) -- or the
	//! first placed leaf -- selected. A leaf that fails to create collapses
	//! out of its tree; the rest still installs.
	[[nodiscard]] TerminalRestoreResult RestoreTerminalGroupLayout(
		std::vector<TerminalRestoreGroup> groups,
		std::optional<std::pair<std::size_t, std::size_t>> activeLeaf );
	//! Terminals of the current projection that host an agent, in the
	//! terminal list's order. `refresh` re-observes every terminal's job
	//! first (bounded by the tab count) and does not call the sink for what
	//! it finds; false returns the event-maintained state without touching
	//! any process.
	[[nodiscard]] std::vector<TerminalAgentPresenceRecord> AgentPresence( bool refresh );
	//! Called on the UI thread after an event changed which terminals host an
	//! agent. The sink must not re-enter this tool synchronously except through
	//! AgentPresence(false). Cleared by Close().
	void SetAgentPresenceSink( std::function<void()> sink );
	[[nodiscard]] std::optional<std::uint64_t> ActiveTerminalId() const noexcept;
	//! Resolves the focused terminal's display title for shared panel chrome.
	//! Empty means that the caller should keep its stable "Terminal" label.
	[[nodiscard]] std::wstring ActiveTerminalTitle() const;
	[[nodiscard]] std::size_t TabCount() const noexcept;
	//! Number of native terminal viewports in the selected terminal group.
	[[nodiscard]] std::size_t VisiblePaneCount() const noexcept;
	//! The pane renderer bound to `tabId`, or nullptr when the tab has none
	//! (different group, projection not materialized).  Tests drive the real
	//! ResizeTab -> resize -> ApplyScrollbackChange chain through this seam.
	[[nodiscard]] CTerminalWnd* RendererForTab( std::uint64_t tabId ) const noexcept;
	//! The default VS Code singleTerminal policy hides this list for one instance.
	[[nodiscard]] bool HasTerminalTabsList() const noexcept;
	//! Empty when the tabs list is hidden or cannot fit beside an 80 DIP terminal.
	[[nodiscard]] RECT TerminalTabsBounds() const noexcept;
	[[nodiscard]] bool HasStartedAnySession() const noexcept;
	//! True after the native renderer HWND has been created.  Kept observable so
	//! the hidden-panel startup contract can be unit-tested without a desktop.
	[[nodiscard]] bool HasCreatedRenderer() const noexcept;
	[[nodiscard]] std::vector<TerminalTabProjectionRecord> TerminalProjectionSnapshot() const;
	[[nodiscard]] std::size_t WorkspaceRegistryEntryCount() const noexcept;
	[[nodiscard]] bool HasWorkspaceProjection( std::wstring_view workspaceIdentityKey ) const noexcept;
	//! Removes an inactive projection created only while preparing a workspace
	//! transition that never committed. Live sessions must not be present.
	void DiscardWorkspaceProjection( std::wstring_view workspaceIdentityKey ) noexcept;
	[[nodiscard]] HWND GetHwnd() const noexcept;
	//! True when `message` targets a terminal instance (a pane renderer) or one of
	//! its children: VS Code's `terminalFocus`, which the tabs list does not set.
	[[nodiscard]] bool IsTerminalInstanceMessage( const MSG& message ) const noexcept;

	static LRESULT CALLBACK WindowProc( HWND window, UINT message, WPARAM wParam, LPARAM lParam );

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};

} // namespace terminal

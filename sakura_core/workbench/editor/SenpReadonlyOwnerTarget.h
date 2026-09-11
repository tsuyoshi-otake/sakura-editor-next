/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#pragma once

#include "workbench/SenpOwnerProjection.h"
#include "workbench/editor/SenpReadonlyDocumentHost.h"
#include "workbench/editor/SenpReadonlyEditorController.h"
#include "markdown/CMarkdownPreviewWnd.h"
#include "theme/CThemeService.h"

#include <map>

namespace workbench::editor {

class CSenpControlToolReads;

using SenpOwnerCommandCompleted = std::function<bool(const senp::effect::OperationContext&,
	const senp::effect::CompleteCommand&)>;
/*!
	@brief The one line a completed extension command is told to the user as.

	The user invoked the command, so its outcome belongs where the user is
	looking, and it has to say who is speaking: `extensionId` leads the line so
	text an extension wrote can never be read as the editor's own. That name is
	the identity the control side committed, never a title the extension chose.

	`completion.message` is untrusted and only checked for well-formed UTF-16 and
	a byte budget on the wire, so it may carry NUL, newlines and any other
	control unit. Here it becomes one bounded line: whitespace controls collapse
	to single spaces, because a message written over several lines is still a
	message, and every other control unit becomes the visible replacement
	character, as a published document's text does. At most 200 units survive,
	and an ellipsis marks a message that was longer.

	Empty means say nothing: a command that succeeded without a message has
	already shown its result in the tree or document it changed, and repeating
	that on every click would leave the status line saying nothing worth reading.
	Every other status is always said, with or without a message, because a
	failure that shows nothing is indistinguishable from nothing happening.
*/
[[nodiscard]] std::wstring SenpCommandCompletionStatus(std::wstring_view extensionId,
	const senp::effect::CompleteCommand& completion);
using SenpOwnerResourceReleased = std::function<bool(std::wstring_view)>;
//! UI-thread-only observer. False means its owner has been revoked or destroyed.
using SenpReadonlyOwnerStyleSink = std::function<bool(const theme::ThemePalette&, const LOGFONT&, unsigned int)>;
/*!
	@brief One turn of the text pump for a single owner, driven by the window.

	A document names its text by a resource handle the control side holds, so
	something has to carry chunks from there to the surface, one turn at a time.
	The window owns the cadence because it owns the turn; what to ask for and
	where the answer belongs is the target's, because it holds the documents.

	False means its owner has been revoked or destroyed, which is how the window
	drops it, exactly as it drops a style sink.
*/
using SenpReadonlyOwnerTextPump = std::function<bool()>;

//! Editor-side view of the account a profile has adopted, as the control side
//! last answered it. It mirrors the wire vocabulary rather than reusing it, so
//! this header does not depend on the control IPC contract. Unknown means the
//! question has not been answered yet, which is not a signed-out account.
enum class SenpToolAccountState : std::uint8_t {
	Unknown,
	Checking,
	Disconnected,
	Connected,
	ReauthenticationRequired,
	Unavailable,
};

//! The account fence a window synchronizes its extensions under. A zero
//! generation carries no authority, and only `State()` says why it is zero.
class SenpToolAccount {
public:
	SenpToolAccount() = default;
	SenpToolAccount(std::int64_t generation, SenpToolAccountState state) noexcept
		: m_generation(generation), m_state(state) {
	}

	[[nodiscard]] std::int64_t Generation() const noexcept { return m_generation; }
	[[nodiscard]] SenpToolAccountState State() const noexcept { return m_state; }

private:
	std::int64_t m_generation = 0;
	SenpToolAccountState m_state = SenpToolAccountState::Unknown;
};

/*!
	@brief One settled answer to a text-resource read.

	`handle` and `offset` echo the read this answers, because the surface that
	asked needs to attribute an answer that names no resource of its own.

	`chunk` is engaged only when the control side answered. A disengaged one
	means no answer arrived - the connection was unavailable, lost or replaced -
	which is not a statement about the resource, and the seam does not invent one
	that would read as if the store had spoken. An engaged chunk whose `result`
	is Accepted is the store's own chunk, whole; any other `result` is the
	control side's refusal, and the rest of the chunk describes nothing.
*/
class SenpToolResourceAnswer {
public:
	SenpToolResourceAnswer() = default;
	SenpToolResourceAnswer(std::wstring handle, std::uint64_t offset,
		std::optional<senp::TextResourceChunk> chunk)
		: handle(std::move(handle)), offset(offset), chunk(std::move(chunk)) {
	}

	[[nodiscard]] const std::wstring& Handle() const noexcept { return handle; }
	[[nodiscard]] std::uint64_t Offset() const noexcept { return offset; }
	[[nodiscard]] const std::optional<senp::TextResourceChunk>& Chunk() const noexcept { return chunk; }

private:
	//! CSenpControlToolReads is the wire-level answer producer and constructs/
	//! mutates these fields directly, matching the friend idiom this file
	//! already uses for its own nested Document/Pending classes.
	friend class CSenpControlToolReads;
	std::wstring handle;
	std::uint64_t offset = 0;
	std::optional<senp::TextResourceChunk> chunk;
};

/*!
	@brief Nonblocking tool-read seam between one owner target and the broker.

	Every method runs on the UI thread inside the owner projection's Pump, so an
	implementation must return without waiting on a pipe, a child process or a
	lock that is held across I/O. Start only admits a read; its terminal arrives
	later through Take. The owner identity selects the scope on every call
	because one editor process brokers reads for several owners over a single
	authenticated connection.
*/
class ISenpOwnerToolReads {
public:
	virtual ~ISenpOwnerToolReads() = default;
	[[nodiscard]] virtual bool Start(const senp::ContributionOwnerIdentity& owner,
		const senp::effect::OperationContext& context,
		const senp::effect::StartToolRead& read) noexcept = 0;
	//! Drains at most one finished terminal for this owner. An empty result means
	//! nothing has finished; it is a normal answer, not a failure.
	[[nodiscard]] virtual std::optional<senp::effect::ToolCompleted> Take(
		const senp::ContributionOwnerIdentity& owner) noexcept = 0;
	//! Cancels every read of one request lineage. A terminal already drained by
	//! the transport may still surface afterwards and must be discarded.
	virtual void Cancel(const senp::ContributionOwnerIdentity& owner,
		const senp::effect::OperationContext& context) noexcept = 0;
	//! Revocation obligation: no completion of this owner may be routed after it
	//! returns.
	virtual void CancelAll(const senp::ContributionOwnerIdentity& owner) noexcept = 0;

	/*!
		@brief The account fence every read through this seam is admitted under.

		It is not an owner concern and an owner target never calls it. It lives
		here because this seam is the only handle the window holds on the
		control connection, and the adopted account generation decides whether
		any read over that connection can be authorized at all.

		The answer is whatever has already settled; like every other method here
		it must not wait on the connection. A seam that has learned nothing
		answers Unknown with generation zero, which is fail-closed.
	*/
	[[nodiscard]] virtual SenpToolAccount Account() const noexcept = 0;
	//! Publishes a refresh of that answer without waiting for it. The window
	//! calls it on its own cadence, so an implementation owns the rate limit.
	virtual void RefreshAccount() noexcept = 0;

	/*!
		@brief Publishes the workspace the window holding this seam is open on.

		It is here for the same reason Account is, and no owner target calls it
		either: the control side resolves the repository a tool read answers for
		from the folders the window declares, and this seam is the only handle
		the window has on the connection that declaration travels over.

		Folder identities only, in URI text. What they contain is read on the
		control side from the folders themselves, so this claims nothing about
		them. Like every other method here it must not wait on the connection,
		and the window may call it on every turn: the rate limit belongs to the
		implementation.
	*/
	virtual void DeclareWorkspace(std::uint64_t generation, std::uint64_t revision,
		std::vector<std::wstring> folders) noexcept = 0;

	/*!
		@brief Publishes a read of one text resource the control side holds.

		A tool read answers with a resource handle rather than with the body, so
		this is the only way the bytes behind a published document reach the
		editor. Like Start it admits only: the answer arrives later through
		TakeResource, and like every other method here it must not wait on the
		connection.

		At most one read per owner is outstanding, matching the single
		outstanding read a document surface keeps, and its answer has to be
		drained before the next is admitted. False therefore means the request
		was unusable or this owner already has one in flight - not that the read
		failed, which is an answer rather than a refusal to admit.
	*/
	[[nodiscard]] virtual bool ReadResource(const senp::ContributionOwnerIdentity& owner,
		std::wstring_view handle, std::uint64_t offset, std::uint32_t length) noexcept = 0;
	//! Drains that answer once it has settled. Every admitted read ends in
	//! exactly one answer - a refusal is an answer - so a surface that asked is
	//! never left holding a read that can no longer finish.
	[[nodiscard]] virtual std::optional<SenpToolResourceAnswer> TakeResource(
		const senp::ContributionOwnerIdentity& owner) noexcept = 0;
	//! States that this owner will not read the resource again. It produces no
	//! answer: whether anything else still holds the resource is the store's
	//! business, and a release the connection never carried is not a failure
	//! this side can act on.
	virtual void ReleaseResource(const senp::ContributionOwnerIdentity& owner,
		std::wstring_view handle) noexcept = 0;
};

enum class SenpReadonlyOwnerTargetState : std::uint8_t {
	Ready, Invalid, ModelBeginFailed, ModelApplyFailed, HostFailed, EditorOpenFailed, EditorStoreFailed,
	EditorShowFailed, Revoked,
};

//! Native readonly-document destination for one committed SENP owner. It owns
//! document models and HWND hosts while the editor controller owns their Core
//! registrations. Controller finalizers retain each host independently, so a
//! failed/reentrant removal cannot leave a borrowed HWND dangling.
class CSenpReadonlyOwnerTarget final : public ISenpOwnerProjectionTarget {
public:
	CSenpReadonlyOwnerTarget(senp::ContributionOwnerIdentity owner,
		SenpReadonlyEditorController& editors, HWND parent,
		rendering::FrameSurfaceId firstSurfaceId,
		const ISenpReadonlyTextResources* resources = nullptr,
		SenpTextResourceView::CopySink copy = {},
		SenpOwnerCommandCompleted commandCompleted = {},
		SenpOwnerResourceReleased resourceReleased = {},
		ISenpOwnerToolReads* toolReads = nullptr
	);
	~CSenpReadonlyOwnerTarget() override;
	CSenpReadonlyOwnerTarget(const CSenpReadonlyOwnerTarget&) = delete;
	CSenpReadonlyOwnerTarget& operator=(const CSenpReadonlyOwnerTarget&) = delete;

	[[nodiscard]] bool BeginDocument(std::wstring_view resourceId,
		const senp::effect::OperationContext& context) noexcept override;
	[[nodiscard]] bool PublishDocument(const senp::effect::OperationContext& context,
		senp::effect::PublishDocument document) noexcept override;
	[[nodiscard]] bool FailDocument(const senp::effect::OperationContext& context,
		senp::InvocationStatus status) noexcept override;
	[[nodiscard]] bool CompleteCommand(const senp::effect::OperationContext& context,
		senp::effect::CompleteCommand completion) noexcept override;
	[[nodiscard]] bool ReleaseResource(std::wstring_view handle) noexcept override;
	[[nodiscard]] bool StartToolRead(const senp::effect::OperationContext& context,
		senp::effect::StartToolRead read) noexcept override;
	[[nodiscard]] std::optional<SenpToolReadTerminal> TakeToolRead() noexcept override;
	void CancelToolReads(const senp::effect::OperationContext& context) noexcept override;
	void Revoke() noexcept override;

	void SetStyle(const theme::ThemePalette& palette, const LOGFONT& font, unsigned int dpi) noexcept;
	[[nodiscard]] SenpReadonlyOwnerStyleSink StyleSink() const;
	/*!
		@brief Moves this owner's text resources one step toward the screen.

		At most one chunk per turn, and at most one read outstanding across every
		document this owner holds: the seam admits one read per owner, and a
		second answer could only be attributed by remembering which document
		asked, which is what the target remembers instead.

		A turn settles an answer that has arrived and then admits the next read
		if one is wanted. It never waits: an answer that has not settled leaves
		the turn with nothing done, and the next turn asks again.
	*/
	void PumpText() noexcept;
	[[nodiscard]] SenpReadonlyOwnerTextPump TextPump() const;
	//! The resource whose read is outstanding over the seam, empty when none is.
	[[nodiscard]] std::wstring OutstandingTextResource() const;
	[[nodiscard]] std::optional<std::string> InputId(std::wstring_view resourceId) const;
	[[nodiscard]] SenpReadonlyDocumentHost* Host(std::wstring_view resourceId) const noexcept;
	[[nodiscard]] std::size_t DocumentCount() const noexcept;
	[[nodiscard]] std::size_t ToolReadCount() const noexcept { return m_toolReadContexts.size(); }
	[[nodiscard]] SenpReadonlyOwnerTargetState State() const noexcept { return m_state; }

private:
	class Document;
	class Pending;
	[[nodiscard]] bool Matches(const senp::effect::OperationContext& context) const noexcept;
	void ReapClosed() noexcept;
	//! Delivers the settled answer to the document that asked, whatever it says.
	void SettleText() noexcept;
	//! Admits the next read one of this owner's documents is waiting to make.
	void BeginText() noexcept;

	senp::ContributionOwnerIdentity m_owner;
	SenpReadonlyScope m_scope;
	SenpReadonlyEditorController& m_editors;
	HWND m_parent{};
	rendering::FrameSurfaceId m_firstSurfaceId{};
	const ISenpReadonlyTextResources* m_resources{};
	SenpTextResourceView::CopySink m_copy;
	SenpOwnerCommandCompleted m_commandCompleted;
	SenpOwnerResourceReleased m_resourceReleased;
	ISenpOwnerToolReads* m_toolReads{};
	//! readId -> the context that started it. A terminal whose readId is absent
	//! was cancelled or never admitted here and must never reach the projection.
	std::map<std::wstring, senp::effect::OperationContext, std::less<>> m_toolReadContexts;
	//! Cleared by Revoke, so every weak handle this target hands out - the
	//! style sink and the text pump - dies with the owner rather than with
	//! the object, which the window may still be holding a copy of.
	std::shared_ptr<CSenpReadonlyOwnerTarget*> m_lifetime;
	std::map<std::wstring, Pending, std::less<>> m_pending;
	std::map<std::wstring, std::shared_ptr<Document>, std::less<>> m_documents;
	//! The read this owner has in flight over the seam and the resource id of
	//! the document that made it. The seam answers with a handle and an offset,
	//! and two documents of one owner may hold the same handle, so the document
	//! is remembered here rather than guessed from the answer.
	std::optional<SenpDocumentTextRead> m_textRead;
	std::wstring m_textResource;
	std::size_t m_surfaceBlocks{};
	theme::ThemePalette m_palette{ theme::CThemeService::PaletteFor(theme::ThemeMode::Dark) };
	LOGFONT m_font{};
	unsigned int m_dpi{ 96 };
	bool m_revoked{};
	SenpReadonlyOwnerTargetState m_state{ SenpReadonlyOwnerTargetState::Ready };
};

} // namespace workbench::editor

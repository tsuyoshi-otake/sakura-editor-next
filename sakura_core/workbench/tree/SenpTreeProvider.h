/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#pragma once

#include "senp/SenpRuntimeSession.h"
#include "workbench/tree/TreeViewModel.h"

namespace workbench::tree {

struct SenpTreeScope final {
	std::int64_t ownerGeneration{}, workspaceRevision{}, accountGeneration{};
};
struct SenpTreeAdmission final {
	SenpTreeAdmission() = default;
	SenpTreeAdmission(senp::AdmissionStatus statusValue, senp::effect::OperationContext contextValue) noexcept
		: status(statusValue), m_context(std::move(contextValue)) {}
	senp::AdmissionStatus status{ senp::AdmissionStatus::Unavailable };
	[[nodiscard]] const senp::effect::OperationContext& Context() const noexcept { return m_context; }
	[[nodiscard]] senp::effect::OperationContext& MutableContext() noexcept { return m_context; }
private:
	senp::effect::OperationContext m_context;
};
//! A native, owner-scoped async port. Submit never waits for Wasm or a tool.
//! Accepted contexts use owner-wide monotonic request generations. Tool result
//! events preserve that request generation even when their operation ID changes.
//! Cancel ends this subscriber and every tool read derived from its request;
//! the broker keeps physical cleanup ownership. No method pumps/reenters UI.
class ISenpTreeRuntime {
public:
	virtual ~ISenpTreeRuntime() = default;
	[[nodiscard]] virtual bool IsCurrent() const noexcept = 0;
	//! False while the runtime is delivering results to this subscriber. Demand
	//! raised then (a refresh or a page) waits for the owner's next Pump instead
	//! of being refused, which the subscriber could not tell from a dead owner.
	[[nodiscard]] virtual bool CanSubmit() const noexcept = 0;
	[[nodiscard]] virtual SenpTreeAdmission Submit(senp::effect::TreeRequest request,
		senp::CSenpRuntimeSession::Time deadline) noexcept = 0;
	virtual void Cancel(const senp::effect::OperationContext& context) noexcept = 0;
	[[nodiscard]] virtual bool Execute(senp::effect::CommandInvoked command) noexcept = 0;
};
class ISenpTreeObserver {
public:
	virtual ~ISenpTreeObserver() = default;
	//! Coalesce native projection work; never synchronously reenter the provider.
	virtual void TreeChanged(std::wstring_view parentId) noexcept = 0;
};
//! One `view/item/context` inline action: a declared command drawn on every row
//! whose contextValue contains each `contains` token and equals each `equals`
//! value (VS Code's `viewItem =~ /token/` and `viewItem == value` clauses).
struct SenpTreeItemAction final {
	SenpTreeItemAction() = default;
	SenpTreeItemAction(std::wstring commandIdValue, std::wstring titleValue, std::wstring iconValue,
		std::vector<std::wstring> containsValue, std::vector<std::wstring> equalsValue)
		: m_commandId(std::move(commandIdValue)), m_title(std::move(titleValue)), m_icon(std::move(iconValue)),
		  m_contains(std::move(containsValue)), m_equals(std::move(equalsValue)) {}
	[[nodiscard]] const std::wstring& CommandId() const noexcept { return m_commandId; }
	[[nodiscard]] const std::wstring& Title() const noexcept { return m_title; }
	[[nodiscard]] const std::wstring& Icon() const noexcept { return m_icon; }
	[[nodiscard]] const std::vector<std::wstring>& Contains() const noexcept { return m_contains; }
	[[nodiscard]] const std::vector<std::wstring>& Equals() const noexcept { return m_equals; }
	void SetCommandId(std::wstring value) noexcept { m_commandId = std::move(value); }
	void SetIcon(std::wstring value) noexcept { m_icon = std::move(value); }
	void SetContains(std::vector<std::wstring> value) noexcept { m_contains = std::move(value); }
	void SetEquals(std::vector<std::wstring> value) noexcept { m_equals = std::move(value); }
private:
	std::wstring m_commandId, m_title, m_icon;
	std::vector<std::wstring> m_contains, m_equals;
};
struct SenpTreeProviderOptions final {
	std::wstring viewId;
	SenpTreeScope scope;
	std::vector<std::wstring> commands;
	std::shared_ptr<ISenpTreeRuntime> runtime;
	std::vector<SenpTreeItemAction> itemActions{};
};

//! One visible Tree View subscriber, serialized on its native composition thread.
//! Pure tree rules stay in TreeViewModel. This adapter owns async admission,
//! deadlines, cancellation and SENP scope/command validation, without OS handles.
class SenpTreeProvider final {
public:
	using Time = senp::CSenpRuntimeSession::Time;
	static constexpr auto kLoadLifetime = std::chrono::seconds(30);
	explicit SenpTreeProvider(SenpTreeProviderOptions options);
	~SenpTreeProvider();
	SenpTreeProvider(const SenpTreeProvider&) = delete;
	SenpTreeProvider& operator=(const SenpTreeProvider&) = delete;
	[[nodiscard]] bool Observe(ISenpTreeObserver& observer) noexcept;
	void Unobserve(ISenpTreeObserver& observer) noexcept;
	void SetVisible(bool visible, Time now);
	void Pump(Time now);
	[[nodiscard]] TreeResult SetExpanded(std::wstring_view id, bool expanded, Time now);
	[[nodiscard]] TreeResult LoadNext(std::wstring_view parentId, Time now);
	[[nodiscard]] TreeResult Retry(std::wstring_view parentId, Time now);
	void Refresh(Time now);
	[[nodiscard]] bool Select(std::wstring_view id);
	[[nodiscard]] bool Execute(std::wstring_view id);
	//! A declared command from the View's title bar. It carries no item and no
	//! arguments, so unlike Execute it needs neither a selected row nor a visible body.
	[[nodiscard]] bool ExecuteViewCommand(std::wstring_view commandId);
	//! The inline actions whose `viewItem` conditions match this row, in declaration order.
	[[nodiscard]] std::vector<SenpTreeItemAction> ItemActions(std::wstring_view id) const;
	//! Runs a matching inline action with the row's stable ID as its only argument,
	//! SENP's stand-in for the TreeItem element VS Code passes to the command.
	[[nodiscard]] bool ExecuteItemAction(std::wstring_view id, std::wstring_view commandId);
	[[nodiscard]] TreeResult Apply(const senp::effect::OperationContext& context,
		senp::effect::PublishTreePage page, Time now);
	//! Non-effects runtime terminals for the admitted request or a derived event.
	[[nodiscard]] TreeResult Failed(const senp::effect::OperationContext& context,
		senp::InvocationStatus status, Time now);
	void Close() noexcept;
	[[nodiscard]] const TreeViewModel& Model() const noexcept;
	[[nodiscard]] std::optional<Time> NextDeadline() const noexcept;
	[[nodiscard]] bool IsVisible() const noexcept;
	[[nodiscard]] std::wstring_view ViewId() const noexcept;
private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};

} // namespace workbench::tree

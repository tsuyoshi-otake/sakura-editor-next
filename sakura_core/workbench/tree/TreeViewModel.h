/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace workbench::tree {

inline constexpr std::size_t kMaximumTreeItems = 2000;
inline constexpr std::size_t kMaximumTreeDepth = 16;
inline constexpr std::size_t kMaximumTreePageItems = 256;
inline constexpr std::size_t kMaximumTreeBytes = 8U * 1024U * 1024U;
inline constexpr std::size_t kMaximumTreeLoads = 8;

enum class TreeItemCollapsibleState : std::uint8_t { None, Collapsed, Expanded };
enum class TreeChildrenState : std::uint8_t { Unrequested, Loading, Complete, Partial, Empty, Failed, Stale, Stopped };
enum class TreePageState : std::uint8_t { Complete, Partial, Empty, Failed };
enum class TreeLoadKind : std::uint8_t { Initial, Refresh, NextPage };
enum class TreeResult : std::uint8_t { Applied, Accepted, Unchanged, Busy, Invalid, Stale, LimitExceeded, Unavailable };

struct TreeItem final {
	std::wstring id, label, description, tooltip, icon, commandId;
	std::vector<std::wstring> arguments;
	TreeItemCollapsibleState collapsibleState{ TreeItemCollapsibleState::None };
	//! VS Code's TreeItem.contextValue, read by `viewItem` inline-action conditions.
	[[nodiscard]] const std::wstring& ContextValue() const noexcept { return m_contextValue; }
	void SetContextValue(std::wstring value) noexcept { m_contextValue = std::move(value); }
private:
	std::wstring m_contextValue;
};
struct TreeChildrenPage final {
	std::wstring parentId;
	std::vector<TreeItem> items;
	std::wstring nextCursor;
	std::uint64_t revision{};
	TreePageState state{ TreePageState::Complete };
	std::wstring message;
};
struct TreeLoadRequest final {
	std::uint64_t generation{}, ticket{};
	std::wstring parentId, cursor;
};
struct TreeLoadAdmission final {
	TreeResult result{ TreeResult::Invalid };
	std::optional<TreeLoadRequest> request;
};
struct TreeChange final {
	TreeChange() = default;
	TreeChange(TreeResult resultValue, std::vector<std::uint64_t> cancelledValue) noexcept
		: result(resultValue), m_cancelled(std::move(cancelledValue)) {}
	TreeResult result{ TreeResult::Invalid };
	[[nodiscard]] const std::vector<std::uint64_t>& Cancelled() const noexcept { return m_cancelled; }
	//! Transport ownership remains with the caller, which cancels these tickets.
	[[nodiscard]] std::vector<std::uint64_t>& MutableCancelled() noexcept { return m_cancelled; }
private:
	std::vector<std::uint64_t> m_cancelled;
};
struct TreeNodeSnapshot final {
	TreeItem item;
	std::wstring parentId;
	std::vector<std::wstring> children;
	TreeChildrenState state{ TreeChildrenState::Unrequested };
	TreeLoadKind retryKind{ TreeLoadKind::Initial };
	std::wstring nextCursor, message;
	std::uint64_t revision{};
	std::size_t depth{};
	bool expanded{}, hasSnapshot{};
};

//! Serialized, OS/transport-free tree state. Begin reserves at most eight loads;
//! its caller owns completion/deadline/cancellation and observes every terminal
//! through Apply, Fail, Cancel, Invalidate or Close. A rejected page changes no
//! item data. Stable IDs are unique across the whole retained View.
class TreeViewModel final {
public:
	TreeViewModel();
	~TreeViewModel();
	TreeViewModel(const TreeViewModel&) = delete;
	TreeViewModel& operator=(const TreeViewModel&) = delete;
	[[nodiscard]] TreeLoadAdmission Begin(std::wstring_view parentId, TreeLoadKind kind);
	[[nodiscard]] TreeChange Apply(const TreeLoadRequest& request, TreeChildrenPage page);
	[[nodiscard]] TreeResult Fail(const TreeLoadRequest& request, std::wstring message);
	[[nodiscard]] TreeResult Cancel(const TreeLoadRequest& request);
	[[nodiscard]] TreeChange SetExpanded(std::wstring_view id, bool expanded);
	[[nodiscard]] bool Select(std::wstring_view id);
	[[nodiscard]] std::wstring Selection() const;
	[[nodiscard]] std::optional<TreeNodeSnapshot> Node(std::wstring_view id) const;
	//! Borrow only until the next model mutation; never retain this pointer in UI.
	[[nodiscard]] const TreeNodeSnapshot* Inspect(std::wstring_view id) const noexcept;
	//! Parent IDs currently needing first load/refresh, in visible preorder.
	//! Pending, failed, leaf, collapsed and blocked-by-parent branches are omitted.
	[[nodiscard]] std::vector<std::wstring> Demand() const;
	[[nodiscard]] std::vector<std::uint64_t> Invalidate();
	[[nodiscard]] std::vector<std::uint64_t> CancelAll();
	[[nodiscard]] std::vector<std::uint64_t> Close();
	[[nodiscard]] std::size_t ItemCount() const noexcept;
	[[nodiscard]] std::size_t PendingCount() const noexcept;
	[[nodiscard]] std::size_t RetainedBytes() const noexcept;
	[[nodiscard]] bool IsClosed() const noexcept;
	[[nodiscard]] static bool ValidItem(const TreeItem& item) noexcept;
private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};

} // namespace workbench::tree

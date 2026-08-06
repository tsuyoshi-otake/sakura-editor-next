/*! @file
	@brief 拡張マニフェストの contribution points を保持する所有権付きモデル
*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "extension/CExtensionContributionRegistry.h"

#include <algorithm>
#include <cwctype>

namespace {

//! 大文字小文字を無視した比較。ファイル名と拡張子の照合に使う（Windows のファイル系は case-insensitive）。
bool EqualsIgnoreCase(std::wstring_view left, std::wstring_view right) noexcept
{
	if (left.size() != right.size()) return false;
	for (std::size_t index = 0; index < left.size(); ++index) {
		if (std::towlower(left[index]) != std::towlower(right[index])) return false;
	}
	return true;
}

/*
	メニューのグループ並び。VS Code は `navigation` を常に先頭に置き、それ以外は
	グループ名の辞書順で並べる。ここも同じ規則にしないと、同じマニフェストから
	VS Code と違う並びのメニューが出る。
*/
int GroupRank(const std::wstring& groupName) noexcept
{
	if (groupName == L"navigation") return 0;
	return groupName.empty() ? 2 : 1;
}

bool MenuItemLess(const SExtensionMenuItem& left, const SExtensionMenuItem& right) noexcept
{
	const int leftRank = GroupRank(left.groupName);
	const int rightRank = GroupRank(right.groupName);
	if (leftRank != rightRank) return leftRank < rightRank;
	if (left.groupName != right.groupName) return left.groupName < right.groupName;
	if (left.groupOrder != right.groupOrder) return left.groupOrder < right.groupOrder;
	// 同着を安定させる。ここを決めておかないと、拡張の登録順というユーザーから見えない
	// ものでメニューの並びが変わる。
	if (left.extensionId != right.extensionId) return left.extensionId < right.extensionId;
	return left.commandId < right.commandId;
}

} // namespace

bool CExtensionContributionRegistry::OwnedBy(
	const std::wstring& ownerExtensionId,
	std::uint64_t ownerGeneration,
	std::wstring_view extensionId,
	std::uint64_t generation) noexcept
{
	return ownerExtensionId == extensionId && ownerGeneration == generation;
}

void CExtensionContributionRegistry::Register(
	const SExtensionContributionOwner& owner, SExtensionContributions contributions)
{
	if (owner.extensionId.empty()) return;
	std::unique_lock lock(m_mutex);

	/*
		世代を問わず、その拡張の貢献はすべて捨ててから入れ直す。マニフェストの再送は差分では
		なく全置換であり、拡張ホストが再接続すると必ず新しい世代で送り直されてくる。ここを
		同一世代だけの入れ替えにすると、前の世代のメニュー項目・キーバインド・アクティビティ
		バーのアイコンが再起動まで残り続ける。
	*/
	const auto drop = [&](auto& container) {
		std::erase_if(container, [&](const auto& entry) { return entry.extensionId == owner.extensionId; });
	};
	drop(m_containerPresentations);
	drop(m_viewPresentations);
	drop(m_menuItems);
	drop(m_submenus);
	drop(m_keybindings);
	drop(m_languages);
	drop(m_snippets);
	std::erase_if(m_acknowledged, [&](const AcknowledgedRecord& record) {
		return record.extensionId == owner.extensionId;
	});

	const auto stamp = [&](auto& container) {
		for (auto& entry : container) {
			entry.extensionId = owner.extensionId;
			entry.generation = owner.generation;
		}
	};
	stamp(contributions.containerPresentations);
	stamp(contributions.viewPresentations);
	stamp(contributions.menuItems);
	stamp(contributions.submenus);
	stamp(contributions.keybindings);
	stamp(contributions.languages);
	stamp(contributions.snippets);

	m_containerPresentations.insert(m_containerPresentations.end(),
		std::make_move_iterator(contributions.containerPresentations.begin()),
		std::make_move_iterator(contributions.containerPresentations.end()));
	m_viewPresentations.insert(m_viewPresentations.end(),
		std::make_move_iterator(contributions.viewPresentations.begin()),
		std::make_move_iterator(contributions.viewPresentations.end()));
	m_menuItems.insert(m_menuItems.end(),
		std::make_move_iterator(contributions.menuItems.begin()),
		std::make_move_iterator(contributions.menuItems.end()));
	m_submenus.insert(m_submenus.end(),
		std::make_move_iterator(contributions.submenus.begin()),
		std::make_move_iterator(contributions.submenus.end()));
	m_keybindings.insert(m_keybindings.end(),
		std::make_move_iterator(contributions.keybindings.begin()),
		std::make_move_iterator(contributions.keybindings.end()));
	m_languages.insert(m_languages.end(),
		std::make_move_iterator(contributions.languages.begin()),
		std::make_move_iterator(contributions.languages.end()));
	m_snippets.insert(m_snippets.end(),
		std::make_move_iterator(contributions.snippets.begin()),
		std::make_move_iterator(contributions.snippets.end()));
	if (!contributions.acknowledged.empty()) {
		m_acknowledged.push_back({ owner.extensionId, owner.generation, std::move(contributions.acknowledged) });
	}
}

void CExtensionContributionRegistry::RemoveOwnedBy(std::wstring_view extensionId, std::uint64_t generation)
{
	std::unique_lock lock(m_mutex);
	const auto drop = [&](auto& container) {
		std::erase_if(container, [&](const auto& entry) {
			return OwnedBy(entry.extensionId, entry.generation, extensionId, generation);
		});
	};
	drop(m_containerPresentations);
	drop(m_viewPresentations);
	drop(m_menuItems);
	drop(m_submenus);
	drop(m_keybindings);
	drop(m_languages);
	drop(m_snippets);
	std::erase_if(m_acknowledged, [&](const AcknowledgedRecord& record) {
		return OwnedBy(record.extensionId, record.generation, extensionId, generation);
	});
}

void CExtensionContributionRegistry::Clear()
{
	std::unique_lock lock(m_mutex);
	m_containerPresentations.clear();
	m_viewPresentations.clear();
	m_menuItems.clear();
	m_submenus.clear();
	m_keybindings.clear();
	m_languages.clear();
	m_snippets.clear();
	m_acknowledged.clear();
}

std::vector<SExtensionMenuItem> CExtensionContributionRegistry::MenuItems(std::wstring_view location) const
{
	std::shared_lock lock(m_mutex);
	std::vector<SExtensionMenuItem> result;
	for (const auto& item : m_menuItems) {
		if (item.location == location) result.push_back(item);
	}
	std::stable_sort(result.begin(), result.end(), MenuItemLess);
	return result;
}

std::vector<std::wstring> CExtensionContributionRegistry::MenuLocations() const
{
	std::shared_lock lock(m_mutex);
	std::vector<std::wstring> result;
	for (const auto& item : m_menuItems) {
		if (std::find(result.begin(), result.end(), item.location) == result.end()) {
			result.push_back(item.location);
		}
	}
	std::sort(result.begin(), result.end());
	return result;
}

SExtensionViewPresentation CExtensionContributionRegistry::ContainerPresentation(
	std::wstring_view containerId) const
{
	std::shared_lock lock(m_mutex);
	for (const auto& presentation : m_containerPresentations) {
		if (presentation.id == containerId) return presentation;
	}
	return {};
}

SExtensionViewPresentation CExtensionContributionRegistry::ViewPresentation(std::wstring_view viewId) const
{
	std::shared_lock lock(m_mutex);
	for (const auto& presentation : m_viewPresentations) {
		if (presentation.id == viewId) return presentation;
	}
	return {};
}

std::vector<SExtensionViewPresentation> CExtensionContributionRegistry::ContainerPresentations() const
{
	std::shared_lock lock(m_mutex);
	return m_containerPresentations;
}

std::vector<SExtensionViewPresentation> CExtensionContributionRegistry::ViewPresentations() const
{
	std::shared_lock lock(m_mutex);
	return m_viewPresentations;
}

std::vector<SExtensionSubmenuDescriptor> CExtensionContributionRegistry::Submenus() const
{
	std::shared_lock lock(m_mutex);
	return m_submenus;
}

std::vector<SExtensionKeybinding> CExtensionContributionRegistry::Keybindings() const
{
	std::shared_lock lock(m_mutex);
	return m_keybindings;
}

std::vector<SExtensionLanguageDescriptor> CExtensionContributionRegistry::Languages() const
{
	std::shared_lock lock(m_mutex);
	return m_languages;
}

std::vector<SExtensionSnippetFile> CExtensionContributionRegistry::SnippetFiles(std::wstring_view languageId) const
{
	std::shared_lock lock(m_mutex);
	if (languageId.empty()) return m_snippets;
	std::vector<SExtensionSnippetFile> result;
	for (const auto& snippet : m_snippets) {
		if (snippet.languageId == languageId) result.push_back(snippet);
	}
	return result;
}

std::vector<std::wstring> CExtensionContributionRegistry::AcknowledgedContributions(
	std::wstring_view extensionId) const
{
	std::shared_lock lock(m_mutex);
	std::vector<std::wstring> result;
	for (const auto& record : m_acknowledged) {
		if (record.extensionId != extensionId) continue;
		result.insert(result.end(), record.names.begin(), record.names.end());
	}
	return result;
}

std::wstring CExtensionContributionRegistry::ResolveLanguageId(std::wstring_view fileName) const
{
	if (fileName.empty()) return {};
	// ディレクトリ区切りを落として、純粋なファイル名にしてから照合する。
	const std::size_t separator = fileName.find_last_of(L"/\\");
	const std::wstring_view leaf =
		separator == std::wstring_view::npos ? fileName : fileName.substr(separator + 1);
	if (leaf.empty()) return {};

	std::shared_lock lock(m_mutex);
	// VS Code と同じく、完全なファイル名の一致が拡張子より強い。
	// 例: `Dockerfile` は拡張子を持たないが言語を確定できる。
	for (const auto& language : m_languages) {
		for (const auto& candidate : language.filenames) {
			if (EqualsIgnoreCase(leaf, candidate)) return language.id;
		}
	}
	for (const auto& language : m_languages) {
		for (const auto& candidate : language.extensions) {
			if (candidate.empty() || candidate.size() >= leaf.size()) continue;
			if (EqualsIgnoreCase(leaf.substr(leaf.size() - candidate.size()), candidate)) return language.id;
		}
	}
	return {};
}

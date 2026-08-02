/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"

#include "workbench/icons/CFileIconThemeRegistry.h"

#include "platform/serialization/JsoncDocument.h"
#include "util/string_ex.h"

#include <algorithm>
#include <array>
#include <cwctype>
#include <fstream>
#include <iterator>
#include <set>
#include <system_error>

namespace workbench::icons {
namespace {

using JsoncValue = platform::serialization::JsoncValue;
using JsoncObject = JsoncValue::Object;
using JsoncArray = JsoncValue::Array;

constexpr std::size_t kMaximumManifestBytes = 1024U * 1024U;
constexpr std::size_t kMaximumThemeBytes = 1024U * 1024U;
constexpr std::size_t kMaximumFontBytes = 16U * 1024U * 1024U;
constexpr std::size_t kMaximumContributions = 256;
constexpr std::size_t kMaximumDefinitions = 65'536;
constexpr std::size_t kMaximumAssociations = 32'768;

const JsoncValue* Member(const JsoncObject& object, std::wstring_view key) noexcept
{
	const auto found = object.find(std::wstring(key));
	return found == object.end() ? nullptr : &found->second;
}

const JsoncObject* AsObject(const JsoncValue* value) noexcept
{
	return value == nullptr ? nullptr : std::get_if<JsoncObject>(&value->Value());
}

const JsoncArray* AsArray(const JsoncValue* value) noexcept
{
	return value == nullptr ? nullptr : std::get_if<JsoncArray>(&value->Value());
}

const std::wstring* AsString(const JsoncValue* value) noexcept
{
	return value == nullptr ? nullptr : std::get_if<std::wstring>(&value->Value());
}

std::optional<bool> AsBool(const JsoncValue* value) noexcept
{
	if (value == nullptr) return std::nullopt;
	if (const auto* boolean = std::get_if<bool>(&value->Value())) return *boolean;
	return std::nullopt;
}

std::wstring Lower(std::wstring_view value)
{
	std::wstring result(value);
	std::transform(result.begin(), result.end(), result.begin(), [](wchar_t character) {
		return static_cast<wchar_t>(std::towlower(character));
	});
	return result;
}

std::wstring NormalizeAssociationKey(std::wstring_view value, bool extension)
{
	std::wstring result = Lower(value);
	std::replace(result.begin(), result.end(), L'\\', L'/');
	while (result.size() > 1 && result.front() == L'/') result.erase(result.begin());
	if (extension) {
		const auto separator = result.find_last_of(L'/');
		const auto start = separator == std::wstring::npos ? 0 : separator + 1;
		if (start < result.size() && result[start] == L'.') result.erase(start, 1);
	}
	return result;
}

std::filesystem::path ComparisonPath(const std::filesystem::path& path)
{
	std::error_code error;
	const auto canonical = std::filesystem::weakly_canonical(path, error);
	if (!error) return canonical.lexically_normal();
	const auto absolute = std::filesystem::absolute(path, error);
	return error ? path.lexically_normal() : absolute.lexically_normal();
}

std::wstring NormalizedPathText(const std::filesystem::path& path)
{
	std::wstring text = Lower(ComparisonPath(path).wstring());
	std::replace(text.begin(), text.end(), L'/', L'\\');
	while (text.size() > 3 && !text.empty() && text.back() == L'\\') text.pop_back();
	return text;
}

bool IsWithin(const std::filesystem::path& root, const std::filesystem::path& candidate)
{
	const std::wstring rootText = NormalizedPathText(root);
	const std::wstring candidateText = NormalizedPathText(candidate);
	if (candidateText == rootText) return true;
	return candidateText.size() > rootText.size()
		&& candidateText.compare(0, rootText.size(), rootText) == 0
		&& candidateText[rootText.size()] == L'\\';
}

std::optional<std::filesystem::path> SafePath(
	const std::filesystem::path& root,
	const std::filesystem::path& base,
	std::wstring_view relative)
{
	if (relative.empty()) return std::nullopt;
	const std::filesystem::path relativePath{ std::wstring(relative) };
	if (relativePath.has_root_name() || relativePath.has_root_directory()) return std::nullopt;
	const auto candidate = (base / relativePath).lexically_normal();
	return IsWithin(root, candidate) ? std::optional(candidate) : std::nullopt;
}

std::optional<std::string> ReadUtf8File(
	const std::filesystem::path& path, std::size_t maximumBytes, std::wstring& diagnostic)
{
	std::error_code error;
	const auto size = std::filesystem::file_size(path, error);
	if (error || size > maximumBytes) {
		diagnostic = L"file icon theme file is missing or too large: " + path.wstring();
		return std::nullopt;
	}
	std::ifstream input(path, std::ios::binary);
	if (!input) {
		diagnostic = L"file icon theme file could not be opened: " + path.wstring();
		return std::nullopt;
	}
	std::string content(static_cast<std::size_t>(size), '\0');
	if (size != 0) input.read(content.data(), static_cast<std::streamsize>(size));
	if (!input && !input.eof()) {
		diagnostic = L"file icon theme file could not be read: " + path.wstring();
		return std::nullopt;
	}
	return content;
}

std::optional<JsoncValue> ReadJson(
	const std::filesystem::path& path, std::size_t maximumBytes, std::wstring& diagnostic)
{
	const auto content = ReadUtf8File(path, maximumBytes, diagnostic);
	if (!content) return std::nullopt;
	const auto parsed = platform::serialization::CJsoncDocument::Parse(*content);
	if (!parsed.Succeeded()) {
		diagnostic = L"file icon theme JSONC parse failed: " + path.wstring();
		if (parsed.diagnostic) diagnostic += L": " + u8stowcs(parsed.diagnostic->message);
		return std::nullopt;
	}
	return parsed.value;
}

std::optional<std::uint32_t> ParseColor(std::wstring_view value) noexcept
{
	const std::wstring text = Lower(value);
	if (text.size() != 4 && text.size() != 7) return std::nullopt;
	if (text.front() != L'#') return std::nullopt;
	const auto nibble = [](wchar_t character) -> int {
		if (character >= L'0' && character <= L'9') return character - L'0';
		if (character >= L'a' && character <= L'f') return character - L'a' + 10;
		return -1;
	};
	const auto byte = [&nibble](wchar_t high, wchar_t low) -> int {
		const int left = nibble(high);
		const int right = nibble(low);
		return left < 0 || right < 0 ? -1 : left * 16 + right;
	};
	int red = 0;
	int green = 0;
	int blue = 0;
	if (text.size() == 4) {
		red = nibble(text[1]) * 17;
		green = nibble(text[2]) * 17;
		blue = nibble(text[3]) * 17;
	} else {
		red = byte(text[1], text[2]);
		green = byte(text[3], text[4]);
		blue = byte(text[5], text[6]);
	}
	if (red < 0 || green < 0 || blue < 0) return std::nullopt;
	return static_cast<std::uint32_t>(red | (green << 8) | (blue << 16));
}

void ReadAssociationMap(
	const JsoncValue* value,
	bool extension, std::map<std::wstring, std::wstring, std::less<>>& output)
{
	const auto* object = AsObject(value);
	if (object == nullptr) return;
	for (const auto& [key, mapped] : *object) {
		if (output.size() >= kMaximumAssociations) break;
		const auto* id = AsString(&mapped);
		const auto normalizedKey = NormalizeAssociationKey(key, extension);
		if (id == nullptr || id->empty() || normalizedKey.empty()) continue;
		output[normalizedKey] = *id;
	}
}

void ReadAssociationScalar(const JsoncValue* value, std::optional<std::wstring>& output)
{
	if (const auto* string = AsString(value); string != nullptr && !string->empty()) output = *string;
}

void ParseAssociationSet(const JsoncObject& object, FileIconAssociationSet& output)
{
	ReadAssociationScalar(Member(object, L"file"), output.file);
	ReadAssociationScalar(Member(object, L"folder"), output.folder);
	ReadAssociationScalar(Member(object, L"folderExpanded"), output.folderExpanded);
	ReadAssociationScalar(Member(object, L"rootFolder"), output.rootFolder);
	ReadAssociationScalar(Member(object, L"rootFolderExpanded"), output.rootFolderExpanded);
	ReadAssociationMap(Member(object, L"fileNames"), false, output.fileNames);
	ReadAssociationMap(Member(object, L"fileExtensions"), true, output.fileExtensions);
	ReadAssociationMap(Member(object, L"folderNames"), false, output.folderNames);
	ReadAssociationMap(Member(object, L"folderNamesExpanded"), false, output.folderNamesExpanded);
	ReadAssociationMap(Member(object, L"rootFolderNames"), false, output.rootFolderNames);
	ReadAssociationMap(Member(object, L"rootFolderNamesExpanded"), false, output.rootFolderNamesExpanded);
}

std::wstring ParentSegment(std::wstring_view path)
{
	std::wstring normalized(path);
	std::replace(normalized.begin(), normalized.end(), L'/', L'\\');
	while (!normalized.empty() && normalized.back() == L'\\') normalized.pop_back();
	const auto last = normalized.find_last_of(L'\\');
	if (last == std::wstring::npos) return {};
	const auto parentEnd = last;
	const auto parentStart = normalized.find_last_of(L'\\', parentEnd == 0 ? 0 : parentEnd - 1);
	const auto start = parentStart == std::wstring::npos ? 0 : parentStart + 1;
	return start < parentEnd ? Lower(std::wstring_view(normalized).substr(start, parentEnd - start)) : L"";
}

std::vector<std::wstring> ExtensionCandidates(std::wstring_view name)
{
	std::vector<std::wstring> candidates;
	std::size_t dot = name.find(L'.');
	while (dot != std::wstring_view::npos && dot + 1 < name.size()) {
		candidates.push_back(NormalizeAssociationKey(name.substr(dot + 1), true));
		dot = name.find(L'.', dot + 1);
	}
	std::sort(candidates.begin(), candidates.end(), [](const std::wstring& left, const std::wstring& right) {
		return left.size() > right.size();
	});
	candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
	return candidates;
}

std::array<const FileIconAssociationSet*, 4> VariantSets(
	const FileIconThemeSnapshot& snapshot, FileIconThemeVariant variant) noexcept
{
	switch (variant) {
	case FileIconThemeVariant::Light:
		return { &snapshot.light, &snapshot.base, nullptr, nullptr };
	case FileIconThemeVariant::HighContrast:
		return { &snapshot.highContrast, &snapshot.base, nullptr, nullptr };
	case FileIconThemeVariant::HighContrastLight:
		return { &snapshot.highContrastLight, &snapshot.highContrast, &snapshot.light, &snapshot.base };
	case FileIconThemeVariant::Default:
	default:
		return { &snapshot.base, nullptr, nullptr, nullptr };
	}
}

const FileIconDefinition* FindDefinition(
	const FileIconThemeSnapshot& snapshot, std::wstring_view id) noexcept
{
	if (id.empty()) return nullptr;
	const auto found = snapshot.definitions.find(std::wstring(id));
	return found == snapshot.definitions.end() ? nullptr : &found->second;
}

template <typename Map>
const FileIconDefinition* FindMapValue(
	const FileIconThemeSnapshot& snapshot,
	const std::array<const FileIconAssociationSet*, 4>& sets,
	const Map FileIconAssociationSet::*member,
	std::wstring_view key) noexcept
{
	if (key.empty()) return nullptr;
	for (const auto* set : sets) {
		if (set == nullptr) continue;
		const auto& map = set->*member;
		const auto found = map.find(std::wstring(key));
		if (found != map.end()) {
			if (const auto* definition = FindDefinition(snapshot, found->second)) return definition;
		}
	}
	return nullptr;
}

const FileIconDefinition* FindScalarValue(
	const FileIconThemeSnapshot& snapshot,
	const std::array<const FileIconAssociationSet*, 4>& sets,
	std::optional<std::wstring> FileIconAssociationSet::*member) noexcept
{
	for (const auto* set : sets) {
		if (set == nullptr) continue;
		const auto& value = set->*member;
		if (value.has_value()) {
			if (const auto* definition = FindDefinition(snapshot, *value)) return definition;
		}
	}
	return nullptr;
}

std::optional<std::filesystem::path> FindFontPath(
	const JsoncObject& root, const std::filesystem::path& extensionRoot,
	const std::filesystem::path& themePath, std::wstring_view fontId)
{
	const auto* fonts = AsArray(Member(root, L"fonts"));
	if (fonts == nullptr) return std::nullopt;
	for (const auto& value : *fonts) {
		const auto* object = AsObject(&value);
		if (object == nullptr) continue;
		const auto* id = AsString(Member(*object, L"id"));
		if (id == nullptr || (!fontId.empty() && *id != fontId)) continue;
		const auto* sources = AsArray(Member(*object, L"src"));
		if (sources == nullptr) continue;
		for (const auto& source : *sources) {
			const auto* sourceObject = AsObject(&source);
			if (sourceObject == nullptr) continue;
			const auto* path = AsString(Member(*sourceObject, L"path"));
			if (path == nullptr) continue;
			const auto resolved = SafePath(extensionRoot, themePath.parent_path(), *path);
			if (resolved && std::filesystem::is_regular_file(*resolved)) return resolved;
		}
	}
	return std::nullopt;
}

std::shared_ptr<const FileIconFont> LoadFont(
	const std::filesystem::path& path,
	std::map<std::wstring, std::shared_ptr<const FileIconFont>, std::less<>>& cache)
{
	const std::wstring cacheKey = NormalizedPathText(path);
	if (const auto found = cache.find(cacheKey); found != cache.end()) return found->second;
	std::wstring ignored;
	const auto raw = ReadUtf8File(path, kMaximumFontBytes, ignored);
	if (!raw || raw->empty()) return {};
	std::vector<std::byte> sfnt(raw->size());
	std::transform(raw->begin(), raw->end(), sfnt.begin(), [](char byte) {
		return static_cast<std::byte>(static_cast<unsigned char>(byte));
	});
	const auto rawFontBytes = sfnt;
	if (detail::LoadFontAsSfnt(rawFontBytes, sfnt) != detail::EFontDecodeError::None) return {};
	(void)detail::EnsureUniqueFontIdentifier(sfnt);
	std::wstring faceName;
	if (detail::ExtractFamilyName(sfnt, faceName) != detail::EFamilyNameError::None || faceName.empty()) return {};
	try {
		auto resource = std::make_shared<detail::CRegisteredMemoryFont>(std::move(sfnt));
		if (!resource->IsValid()) return {};
		auto font = std::make_shared<FileIconFont>();
		font->resource = std::move(resource);
		font->faceName = std::move(faceName);
		cache.emplace(cacheKey, font);
		return font;
	}
	catch (...) {
		return {};
	}
}

void ParseFontDefinition(
	const JsoncObject& object,
	const JsoncObject& themeRoot,
	const std::filesystem::path& extensionRoot,
	const std::filesystem::path& themePath,
	std::map<std::wstring, FileIconDefinition, std::less<>>& definitions,
	std::map<std::wstring, std::shared_ptr<const FileIconFont>, std::less<>>& fontCache,
	std::wstring_view definitionId)
{
	FileIconDefinition definition;
	if (const auto* iconPath = AsString(Member(object, L"iconPath")); iconPath != nullptr) {
		const auto resolved = SafePath(extensionRoot, themePath.parent_path(), *iconPath);
		if (resolved && std::filesystem::is_regular_file(*resolved)) definition.iconPath = *resolved;
	}
	if (definition.iconPath.empty()) {
		const auto* fontCharacter = AsString(Member(object, L"fontCharacter"));
		if (fontCharacter == nullptr) return;
		std::wstring glyph;
		if (detail::ParseFontCharacter(wcstou8s(*fontCharacter), glyph) != detail::EFontCharacterError::None) return;
		std::wstring fontId;
		if (const auto* rawFontId = AsString(Member(object, L"fontId")); rawFontId != nullptr) fontId = *rawFontId;
		const auto fontPath = FindFontPath(themeRoot, extensionRoot, themePath, fontId);
		if (!fontPath) return;
		definition.font = LoadFont(*fontPath, fontCache);
		if (!definition.font) return;
		definition.glyph = std::move(glyph);
	}
	if (const auto* fontColor = AsString(Member(object, L"fontColor")); fontColor != nullptr) {
		definition.fontColor = ParseColor(*fontColor);
	}
	definitions[std::wstring(definitionId)] = std::move(definition);
}

bool ParseDefinitions(
	const JsoncObject& root,
	const std::filesystem::path& extensionRoot,
	const std::filesystem::path& themePath,
	FileIconThemeSnapshot& snapshot,
	std::map<std::wstring, std::shared_ptr<const FileIconFont>, std::less<>>& fontCache)
{
	const auto* definitions = AsObject(Member(root, L"iconDefinitions"));
	if (definitions == nullptr) return false;
	for (const auto& [id, value] : *definitions) {
		if (snapshot.definitions.size() >= kMaximumDefinitions) break;
		const auto* object = AsObject(&value);
		if (object == nullptr || id.empty()) continue;
		ParseFontDefinition(*object, root, extensionRoot, themePath, snapshot.definitions, fontCache, id);
	}
	return !snapshot.definitions.empty();
}

} // namespace

const FileIconDefinition* FileIconThemeSnapshot::Resolve(
	std::wstring_view name,
	std::wstring_view path,
	bool isDirectory,
	bool expanded,
	bool isWorkspaceRoot,
	FileIconThemeVariant variant) const noexcept
{
	try {
		const auto sets = VariantSets(*this, variant);
		const auto loweredName = Lower(name);
		const auto parent = ParentSegment(path);
		if (isDirectory) {
			if (isWorkspaceRoot) {
				const auto& member = expanded
					? &FileIconAssociationSet::rootFolderNamesExpanded
					: &FileIconAssociationSet::rootFolderNames;
				if (const auto* definition = FindMapValue(*this, sets, member, parent.empty() ? loweredName : parent + L"/" + loweredName)) return definition;
				if (const auto* definition = FindMapValue(*this, sets, member, loweredName)) return definition;
				if (const auto* definition = FindScalarValue(*this, sets,
					expanded ? &FileIconAssociationSet::rootFolderExpanded : &FileIconAssociationSet::rootFolder)) return definition;
			}
			const auto& member = expanded
				? &FileIconAssociationSet::folderNamesExpanded
				: &FileIconAssociationSet::folderNames;
			if (const auto* definition = FindMapValue(*this, sets, member, parent.empty() ? loweredName : parent + L"/" + loweredName)) return definition;
			if (const auto* definition = FindMapValue(*this, sets, member, loweredName)) return definition;
			return FindScalarValue(*this, sets, expanded ? &FileIconAssociationSet::folderExpanded : &FileIconAssociationSet::folder);
		}

		if (const auto* definition = FindMapValue(*this, sets, &FileIconAssociationSet::fileNames,
			parent.empty() ? loweredName : parent + L"/" + loweredName)) return definition;
		if (const auto* definition = FindMapValue(*this, sets, &FileIconAssociationSet::fileNames, loweredName)) return definition;
		for (const auto& extension : ExtensionCandidates(name)) {
			if (const auto* definition = FindMapValue(*this, sets, &FileIconAssociationSet::fileExtensions,
				parent.empty() ? extension : parent + L"/" + extension)) return definition;
			if (const auto* definition = FindMapValue(*this, sets, &FileIconAssociationSet::fileExtensions, extension)) return definition;
		}
		return FindScalarValue(*this, sets, &FileIconAssociationSet::file);
	}
	catch (...) {
		return nullptr;
	}
}

bool CFileIconThemeRegistry::RegisterExtension(
	std::wstring_view extensionId, const std::filesystem::path& extensionRoot)
{
	try {
		const std::wstring ownedExtensionId(extensionId);
		m_themes.erase(std::remove_if(m_themes.begin(), m_themes.end(),
			[&ownedExtensionId](const FileIconThemeInfo& theme) { return theme.extensionId == ownedExtensionId; }), m_themes.end());
		if (ownedExtensionId.empty()) return false;
		std::wstring diagnostic;
		const auto document = ReadJson(extensionRoot / L"package.json", kMaximumManifestBytes, diagnostic);
		if (!document) return false;
		const auto* root = AsObject(&*document);
		const auto* contributes = root == nullptr ? nullptr : AsObject(Member(*root, L"contributes"));
		const auto* iconThemes = contributes == nullptr ? nullptr : AsArray(Member(*contributes, L"iconThemes"));
		if (iconThemes == nullptr) return false;
		std::vector<FileIconThemeInfo> discovered;
		for (const auto& value : *iconThemes) {
			if (discovered.size() >= kMaximumContributions) break;
			const auto* object = AsObject(&value);
			if (object == nullptr) continue;
			const auto* label = AsString(Member(*object, L"label"));
			const auto* path = AsString(Member(*object, L"path"));
			if (label == nullptr || label->empty() || path == nullptr) continue;
			const auto themePath = SafePath(extensionRoot, extensionRoot, *path);
			if (!themePath || !std::filesystem::is_regular_file(*themePath)) continue;
			FileIconThemeInfo info;
			info.extensionId = ownedExtensionId;
			info.extensionRoot = extensionRoot;
			info.label = *label;
			if (const auto* id = AsString(Member(*object, L"id")); id != nullptr && !id->empty()) {
				info.id = *id;
			} else {
				info.id = ownedExtensionId + L":" + *label;
			}
			info.themePath = *themePath;
			if (std::any_of(discovered.begin(), discovered.end(), [&info](const FileIconThemeInfo& existing) {
				return existing.id == info.id;
			}) || std::any_of(m_themes.begin(), m_themes.end(), [&info](const FileIconThemeInfo& existing) {
				return existing.id == info.id;
			})) continue;
			discovered.push_back(std::move(info));
		}
		m_themes.insert(m_themes.end(), discovered.begin(), discovered.end());
		return !discovered.empty();
	}
	catch (...) {
		return false;
	}
}

void CFileIconThemeRegistry::Clear() noexcept
{
	m_themes.clear();
	m_fonts.clear();
}

std::vector<FileIconThemeInfo> CFileIconThemeRegistry::Themes() const
{
	std::vector<FileIconThemeInfo> result = m_themes;
	std::sort(result.begin(), result.end(), [](const FileIconThemeInfo& left, const FileIconThemeInfo& right) {
		if (left.label != right.label) return left.label < right.label;
		return left.id < right.id;
	});
	return result;
}

FileIconThemeLoadResult CFileIconThemeRegistry::Load(std::wstring_view idOrLabel) const
{
	FileIconThemeLoadResult result;
	try {
		const FileIconThemeInfo* selected = nullptr;
		for (const auto& theme : m_themes) {
			if (theme.id == idOrLabel) {
				selected = &theme;
				break;
			}
		}
		if (selected == nullptr) {
			const auto query = Lower(idOrLabel);
			for (const auto& theme : m_themes) {
				if (Lower(theme.label) == query) {
					selected = &theme;
					break;
				}
			}
		}
		if (selected == nullptr) {
			result.diagnostic = L"file icon theme was not found: " + std::wstring(idOrLabel);
			return result;
		}
		const auto document = ReadJson(selected->themePath, kMaximumThemeBytes, result.diagnostic);
		if (!document) return result;
		const auto* root = AsObject(&*document);
		if (root == nullptr) {
			result.diagnostic = L"file icon theme root is not an object: " + selected->themePath.wstring();
			return result;
		}
		auto snapshot = std::make_shared<FileIconThemeSnapshot>();
		snapshot->info = *selected;
		if (!ParseDefinitions(*root, selected->extensionRoot, selected->themePath, *snapshot, m_fonts)) {
			result.diagnostic = L"file icon theme contains no usable icon definitions: " + selected->themePath.wstring();
			return result;
		}
		ParseAssociationSet(*root, snapshot->base);
		if (const auto* light = AsObject(Member(*root, L"light")); light != nullptr) ParseAssociationSet(*light, snapshot->light);
		if (const auto* highContrast = AsObject(Member(*root, L"highContrast")); highContrast != nullptr) ParseAssociationSet(*highContrast, snapshot->highContrast);
		if (const auto* highContrastLight = AsObject(Member(*root, L"highContrastLight")); highContrastLight != nullptr) ParseAssociationSet(*highContrastLight, snapshot->highContrastLight);
		if (const auto hides = AsBool(Member(*root, L"hidesExplorerArrows")); hides.has_value()) snapshot->hidesExplorerArrows = *hides;
		result.theme = std::move(snapshot);
		return result;
	}
	catch (...) {
		result.theme.reset();
		result.diagnostic = L"file icon theme loading failed unexpectedly";
		return result;
	}
}

} // namespace workbench::icons

//! SENP package version dispatch, strict validation, and immutable installation.

use ed25519_dalek::{Signature, VerifyingKey};
use serde::de::{self, DeserializeOwned, MapAccess, SeqAccess, Visitor};
use serde::{Deserialize, Deserializer, Serialize};
use sha2::{Digest, Sha256};
use std::collections::{BTreeMap, BTreeSet};
use std::fmt;
use std::fs::{self, File, OpenOptions, TryLockError};
use std::io::{Cursor, Read, Seek, Write};
use std::path::{Component, Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};
use zip::read::ZipArchive;
use zip::write::SimpleFileOptions;
use zip::{CompressionMethod, DateTime, ZipWriter};

pub const FORMAT_VERSION: u32 = 1;
pub const ABI: &str = "sakura:senp/extension@1.0.0";
pub const ABI_V2: &str = "sakura:senp/extension@3.0.0";
pub const MANIFEST_PATH: &str = "senp.json";
pub const README_PATH: &str = "README.md";
pub const LICENSE_PATH: &str = "LICENSE";
pub const MODULE_PATH: &str = "module/extension.wasm";
pub const CHECKSUM_PATH: &str = "integrity/SHA256SUMS";
pub const SIGNATURE_PATH: &str = "signature/ed25519.sig";

const MAX_ARCHIVE_BYTES: u64 = 64 * 1024 * 1024;
const MAX_EXPANDED_BYTES: u64 = 64 * 1024 * 1024;
const MAX_ENTRY_BYTES: u64 = 32 * 1024 * 1024;
const MAX_DOCUMENT_BYTES: u64 = 4 * 1024 * 1024;
const MAX_ENTRIES: usize = 256;
const MAX_COMPRESSION_RATIO: u64 = 100;
const ARCHIVE_MARKER_PATH: &str = ".senp-archive-sha256";

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields, rename_all = "camelCase")]
pub struct EngineCompatibility {
    pub sakura: String,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields, rename_all = "camelCase")]
pub struct RuntimeContract {
    pub abi: String,
    pub module: String,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields, rename_all = "camelCase")]
pub struct EditorDecorationContribution {
    pub id: String,
    pub kind: String,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields, rename_all = "camelCase")]
pub struct LanguageContribution {
    pub id: String,
    #[serde(default)]
    pub aliases: Vec<String>,
    #[serde(default)]
    pub extensions: Vec<String>,
    #[serde(default)]
    pub filenames: Vec<String>,
    #[serde(default)]
    pub filename_patterns: Vec<String>,
    #[serde(default)]
    pub mimetypes: Vec<String>,
    #[serde(default)]
    pub first_line: Option<String>,
    #[serde(default)]
    pub configuration: Option<String>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields, rename_all = "camelCase")]
pub struct GrammarContribution {
    #[serde(default)]
    pub language: Option<String>,
    pub scope_name: String,
    pub path: String,
    #[serde(default)]
    pub inject_to: Vec<String>,
    #[serde(default)]
    pub embedded_languages: BTreeMap<String, String>,
    #[serde(default)]
    pub balanced_bracket_scopes: Vec<String>,
    #[serde(default)]
    pub unbalanced_bracket_scopes: Vec<String>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields, rename_all = "camelCase")]
pub struct ViewContainerContribution {
    pub id: String,
    pub title: String,
    pub icon: String,
    pub order: i32,
}

#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields, rename_all = "camelCase")]
pub struct ViewContainersContribution {
    #[serde(default)]
    pub activitybar: Vec<ViewContainerContribution>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields, rename_all = "camelCase")]
pub struct ViewContribution {
    pub id: String,
    pub name: String,
    pub provider: String,
    pub order: i32,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields, rename_all = "camelCase")]
pub struct CommandContribution {
    pub command: String,
    pub title: String,
    /// A `$(codicon)` ThemeIcon or, as VS Code allows, a package-relative image
    /// path. The host never reads that file: a path draws only as an inline
    /// Tree item action and only when it names an entry of the compiled-in
    /// vocabulary, and a `view/title` action must still use a codicon. The
    /// light/dark pair form is rejected rather than drawn as something else.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub icon: Option<String>,
}

/// One `menus["view/title"]` or `menus["view/item/context"]` item. A title item
/// exists natively only in the inline `navigation` group and names its Views by
/// a `view == <id>` disjunction, see `view_title_when_views`; an item action
/// exists only in the `inline` group, see `view_item_when`.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields, rename_all = "camelCase")]
pub struct MenuItemContribution {
    pub command: String,
    pub when: String,
    pub group: String,
}

#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct MenusContribution {
    #[serde(default, rename = "view/title", skip_serializing_if = "Vec::is_empty")]
    pub view_title: Vec<MenuItemContribution>,
    #[serde(
        default,
        rename = "view/item/context",
        skip_serializing_if = "Vec::is_empty"
    )]
    pub view_item_context: Vec<MenuItemContribution>,
}

impl MenusContribution {
    pub fn is_empty(&self) -> bool {
        self.view_title.is_empty() && self.view_item_context.is_empty()
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields, rename_all = "camelCase")]
pub struct Contributions {
    #[serde(default)]
    pub commands: Vec<CommandContribution>,
    #[serde(default, skip_serializing_if = "MenusContribution::is_empty")]
    pub menus: MenusContribution,
    #[serde(default)]
    pub editor_decorations: Vec<EditorDecorationContribution>,
    #[serde(default)]
    pub languages: Vec<LanguageContribution>,
    #[serde(default)]
    pub grammars: Vec<GrammarContribution>,
    #[serde(default)]
    pub views_containers: ViewContainersContribution,
    #[serde(default)]
    pub views: BTreeMap<String, Vec<ViewContribution>>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields, rename_all = "camelCase")]
pub struct Manifest {
    pub schema_version: u32,
    pub id: String,
    pub display_name: String,
    pub version: String,
    pub publisher: String,
    pub description: String,
    pub engines: EngineCompatibility,
    #[serde(default)]
    pub runtime: Option<RuntimeContract>,
    #[serde(default)]
    pub activation_events: Vec<String>,
    #[serde(default)]
    pub capabilities: Vec<String>,
    pub contributes: Contributions,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct VerifiedPackage {
    pub manifest: Manifest,
    pub archive_sha256: String,
    pub signed: bool,
    pub entry_count: usize,
    pub expanded_bytes: u64,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct InstalledExtension {
    pub manifest: Manifest,
    pub archive_sha256: String,
    pub enabled: bool,
    pub signed: bool,
    pub trust: String,
    pub extension_path: PathBuf,
    pub module_sha256: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub module_path: Option<PathBuf>,
    /// False when the package names a runtime ABI other than the one this build
    /// executes. It stays listed (so it can be updated or removed) but must
    /// never be started; the host could not instantiate it.
    pub compatible: bool,
}

/// One line of `sakura-senp-tool list` output (JSON Lines). A failure confined
/// to one profile state entry becomes an `Invalid` row instead of failing the
/// whole listing; the row's `code` is the `SenpError` code the entry produced.
/// `README.md` is deliberately not part of either row: it stays mandatory,
/// UTF-8, checksum-covered package validation, but no consumer shows it in the
/// list, so it is read on demand rather than embedded in every row.
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
#[serde(tag = "kind", rename_all = "camelCase")]
pub enum InstalledListRow {
    Installed(Box<InstalledExtension>),
    Invalid {
        /// The profile state's validated extension id, or empty when no state
        /// could be read far enough to prove one.
        id: String,
        /// The profile state's trust class, or empty when unknown.
        trust: String,
        code: ErrorCode,
        /// Bounded diagnostic detail; never a path longer than
        /// `INVALID_ROW_MESSAGE_BYTES`.
        message: String,
    },
}

/// `sakura-senp-tool list` line and total budgets. One line must fit the C++
/// decoder's per-document limits (1 MiB, 65,536 JSON nodes); the whole stdout
/// must stay within the 4 MiB tool-output budget.
pub const LIST_LINE_MAX_BYTES: usize = 1024 * 1024;
pub const LIST_LINE_MAX_NODES: usize = 65_536;
pub const LIST_OUTPUT_MAX_BYTES: usize = 4 * 1024 * 1024;
const INVALID_ROW_MESSAGE_BYTES: usize = 512;

#[derive(Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields, rename_all = "camelCase")]
struct ProfileState {
    schema_version: u32,
    id: String,
    archive_sha256: String,
    enabled: bool,
    signed: bool,
    trust: String,
}

#[derive(Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields, rename_all = "camelCase")]
struct UninstalledState {
    schema_version: u32,
    id: String,
    archive_sha256: String,
    trust: String,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum TrustPolicy {
    BuiltIn { expected_archive_sha256: String },
    PublisherKeys(BTreeMap<String, [u8; 32]>),
    DeveloperUnsigned,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
#[serde(rename_all = "camelCase")]
pub enum ErrorCode {
    Io,
    ArchiveTooLarge,
    InvalidZip,
    TooManyEntries,
    DuplicateEntry,
    UnsafePath,
    UnsupportedEntry,
    UnsupportedCompression,
    EntryTooLarge,
    ExpandedSizeLimit,
    CompressionRatioLimit,
    InvalidUtf8,
    MissingRequiredEntry,
    InvalidChecksumDocument,
    ChecksumMismatch,
    InvalidManifest,
    UnsupportedSchema,
    AbiMismatch,
    InvalidSignature,
    UnsignedPackage,
    UntrustedPublisher,
    ReparsePoint,
    PublicationFailed,
    OutputTooLarge,
    Busy,
}

#[derive(Debug)]
pub struct SenpError {
    pub code: ErrorCode,
    pub detail: String,
}

impl SenpError {
    fn new(code: ErrorCode, detail: impl Into<String>) -> Self {
        Self {
            code,
            detail: detail.into(),
        }
    }
}

impl fmt::Display for SenpError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{:?}: {}", self.code, self.detail)
    }
}

impl std::error::Error for SenpError {}

fn io_error(context: &str, error: impl fmt::Display) -> SenpError {
    SenpError::new(ErrorCode::Io, format!("{context}: {error}"))
}

fn hex(bytes: &[u8]) -> String {
    const DIGITS: &[u8; 16] = b"0123456789abcdef";
    let mut result = String::with_capacity(bytes.len() * 2);
    for byte in bytes {
        result.push(DIGITS[(byte >> 4) as usize] as char);
        result.push(DIGITS[(byte & 0x0f) as usize] as char);
    }
    result
}

pub fn decode_hex<const N: usize>(value: &str) -> Result<[u8; N], SenpError> {
    if value.len() != N * 2 {
        return Err(SenpError::new(
            ErrorCode::InvalidSignature,
            "wrong hex length",
        ));
    }
    let mut result = [0u8; N];
    for (index, pair) in value.as_bytes().chunks_exact(2).enumerate() {
        let digit = |value: u8| match value {
            b'0'..=b'9' => Some(value - b'0'),
            b'a'..=b'f' => Some(value - b'a' + 10),
            b'A'..=b'F' => Some(value - b'A' + 10),
            _ => None,
        };
        let high = digit(pair[0])
            .ok_or_else(|| SenpError::new(ErrorCode::InvalidSignature, "non-hex digit"))?;
        let low = digit(pair[1])
            .ok_or_else(|| SenpError::new(ErrorCode::InvalidSignature, "non-hex digit"))?;
        result[index] = (high << 4) | low;
    }
    Ok(result)
}

#[derive(Debug)]
struct ArchiveSnapshot {
    bytes: Vec<u8>,
    archive_sha256: String,
}

fn read_archive_snapshot(path: &Path) -> Result<ArchiveSnapshot, SenpError> {
    let mut file = File::open(path).map_err(|error| io_error("open package", error))?;
    let initial_len = file
        .metadata()
        .map_err(|error| io_error("read package metadata", error))?
        .len();
    if initial_len > MAX_ARCHIVE_BYTES {
        return Err(SenpError::new(
            ErrorCode::ArchiveTooLarge,
            "archive exceeds 64 MiB",
        ));
    }
    let mut bytes = Vec::with_capacity(initial_len as usize);
    Read::by_ref(&mut file)
        .take(MAX_ARCHIVE_BYTES + 1)
        .read_to_end(&mut bytes)
        .map_err(|error| io_error("read package", error))?;
    let final_len = file
        .metadata()
        .map_err(|error| io_error("read package metadata", error))?
        .len();
    if bytes.len() as u64 > MAX_ARCHIVE_BYTES {
        return Err(SenpError::new(
            ErrorCode::ArchiveTooLarge,
            "archive exceeds 64 MiB",
        ));
    }
    if bytes.len() as u64 != initial_len || final_len != initial_len {
        return Err(SenpError::new(
            ErrorCode::InvalidZip,
            "package changed while reading",
        ));
    }
    let archive_sha256 = archive_digest(&bytes);
    Ok(ArchiveSnapshot {
        bytes,
        archive_sha256,
    })
}

fn archive_digest(bytes: &[u8]) -> String {
    hex(&Sha256::digest(bytes))
}

fn normalized_entry_name(raw: &[u8]) -> Result<String, SenpError> {
    let value = std::str::from_utf8(raw)
        .map_err(|_| SenpError::new(ErrorCode::InvalidUtf8, "ZIP entry name is not UTF-8"))?;
    if value.is_empty()
        || value.starts_with('/')
        || value.starts_with('\\')
        || value.contains('\\')
        || value.contains('\0')
        || value.contains(':')
        || value.ends_with('/')
    {
        return Err(SenpError::new(ErrorCode::UnsafePath, value));
    }
    let path = Path::new(value);
    for component in path.components() {
        match component {
            Component::Normal(part)
                if !part.is_empty()
                    && !part.to_str().is_some_and(|segment| {
                        is_windows_reserved_name(segment) || segment.ends_with(['.', ' '])
                    }) => {}
            _ => return Err(SenpError::new(ErrorCode::UnsafePath, value)),
        }
    }
    Ok(value.to_owned())
}

/// True for a path segment (or package id) that Windows resolves to a device:
/// `CON PRN AUX NUL COM0-9 LPT0-9` (also the superscript digits Windows
/// reserves for COM/LPT), compared case-insensitively, with or without any
/// extension. Windows ignores everything from the first dot and trailing
/// spaces before it, so `nul.txt` and `com1.tar.gz` name the device too.
fn is_windows_reserved_name(segment: &str) -> bool {
    let stem = segment
        .split('.')
        .next()
        .unwrap_or_default()
        .trim_end_matches(' ')
        .to_ascii_uppercase();
    if matches!(stem.as_str(), "CON" | "PRN" | "AUX" | "NUL") {
        return true;
    }
    stem.strip_prefix("COM")
        .or_else(|| stem.strip_prefix("LPT"))
        .is_some_and(|digit| {
            let mut chars = digit.chars();
            matches!(
                (chars.next(), chars.next()),
                (Some('0'..='9' | '\u{b9}' | '\u{b2}' | '\u{b3}'), None)
            )
        })
}

fn case_fold_path(value: &str) -> String {
    value.chars().flat_map(char::to_lowercase).collect()
}

fn is_document(path: &str) -> bool {
    matches!(
        path,
        MANIFEST_PATH
            | README_PATH
            | LICENSE_PATH
            | "CHANGELOG.md"
            | CHECKSUM_PATH
            | SIGNATURE_PATH
    )
}

fn is_allowed_entry(path: &str) -> bool {
    matches!(
        path,
        MANIFEST_PATH
            | README_PATH
            | LICENSE_PATH
            | MODULE_PATH
            | "CHANGELOG.md"
            | CHECKSUM_PATH
            | SIGNATURE_PATH
    ) || path.starts_with("assets/")
}

fn read_source_payload(source: &Path, relative: &str) -> Result<Vec<u8>, SenpError> {
    let path = source.join(relative);
    let metadata = fs::symlink_metadata(&path).map_err(|error| io_error(relative, error))?;
    if metadata.file_type().is_symlink() {
        return Err(SenpError::new(
            ErrorCode::ReparsePoint,
            path.display().to_string(),
        ));
    }
    if !metadata.is_file() {
        return Err(SenpError::new(ErrorCode::UnsupportedEntry, relative));
    }
    if metadata.len() > MAX_ENTRY_BYTES
        || (is_document(relative) && metadata.len() > MAX_DOCUMENT_BYTES)
    {
        return Err(SenpError::new(ErrorCode::EntryTooLarge, relative));
    }
    let bytes = fs::read(&path).map_err(|error| io_error(relative, error))?;
    if matches!(
        relative,
        MANIFEST_PATH | README_PATH | LICENSE_PATH | "CHANGELOG.md"
    ) && std::str::from_utf8(&bytes).is_err()
    {
        return Err(SenpError::new(ErrorCode::InvalidUtf8, relative));
    }
    Ok(bytes)
}

fn collect_asset_payloads(
    directory: &Path,
    relative: &Path,
    payloads: &mut BTreeMap<String, Vec<u8>>,
    depth: usize,
) -> Result<(), SenpError> {
    if depth > 16 {
        return Err(SenpError::new(
            ErrorCode::UnsafePath,
            "asset directory nesting exceeds 16 levels",
        ));
    }
    let mut children = fs::read_dir(directory)
        .map_err(|error| io_error("read assets directory", error))?
        .collect::<Result<Vec<_>, _>>()
        .map_err(|error| io_error("enumerate assets directory", error))?;
    children.sort_by_key(|entry| entry.file_name());
    for child in children {
        let metadata = child
            .file_type()
            .map_err(|error| io_error("inspect asset", error))?;
        if metadata.is_symlink() {
            return Err(SenpError::new(
                ErrorCode::ReparsePoint,
                child.path().display().to_string(),
            ));
        }
        let child_relative = relative.join(child.file_name());
        if metadata.is_dir() {
            collect_asset_payloads(&child.path(), &child_relative, payloads, depth + 1)?;
            continue;
        }
        if !metadata.is_file() {
            return Err(SenpError::new(
                ErrorCode::UnsupportedEntry,
                child.path().display().to_string(),
            ));
        }
        let archive_path = child_relative
            .components()
            .map(|component| match component {
                Component::Normal(value) => value.to_str().ok_or_else(|| {
                    SenpError::new(ErrorCode::InvalidUtf8, child.path().display().to_string())
                }),
                _ => Err(SenpError::new(
                    ErrorCode::UnsafePath,
                    child.path().display().to_string(),
                )),
            })
            .collect::<Result<Vec<_>, _>>()?
            .join("/");
        let archive_path = format!("assets/{archive_path}");
        normalized_entry_name(archive_path.as_bytes())?;
        if payloads.len() >= MAX_ENTRIES - 2 {
            return Err(SenpError::new(
                ErrorCode::TooManyEntries,
                MAX_ENTRIES.to_string(),
            ));
        }
        let bytes = fs::read(child.path()).map_err(|error| io_error("read asset", error))?;
        if bytes.len() as u64 > MAX_ENTRY_BYTES {
            return Err(SenpError::new(ErrorCode::EntryTooLarge, archive_path));
        }
        payloads.insert(archive_path, bytes);
    }
    Ok(())
}

fn valid_identifier(value: &str) -> bool {
    !value.is_empty()
        && value.len() <= 128
        && value.bytes().all(|byte| {
            byte.is_ascii_lowercase() || byte.is_ascii_digit() || matches!(byte, b'.' | b'-')
        })
        && !value.starts_with(['.', '-'])
        && !value.ends_with(['.', '-'])
}

/// A package id also names `profiles/<id>.json`, so it must not be a device
/// name on Windows. Other identifiers (languages, publishers, ...) never become
/// file names and keep the plain `valid_identifier` rule.
fn valid_package_id(value: &str) -> bool {
    valid_identifier(value) && !is_windows_reserved_name(value)
}

fn valid_workbench_identifier(value: &str) -> bool {
    !value.is_empty()
        && value.len() <= 160
        && value
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'.' | b'-' | b'_'))
        && !value.starts_with(['.', '-', '_'])
        && !value.ends_with(['.', '-', '_'])
}

fn valid_effect_identifier(value: &str) -> bool {
    !value.is_empty()
        && value.len() <= 160
        && value
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'.' | b'-' | b'_' | b':'))
        && value
            .as_bytes()
            .first()
            .is_some_and(u8::is_ascii_alphanumeric)
        && value
            .as_bytes()
            .last()
            .is_some_and(u8::is_ascii_alphanumeric)
}

fn valid_theme_icon(value: &str) -> bool {
    value.len() >= 4
        && value.len() <= 132
        && value.starts_with("$(")
        && value.ends_with(')')
        && value[2..value.len() - 1]
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || byte == b'-')
}

/// A menu action draws a bundled codicon named by lower-case characters only
/// (`[a-z0-9-]`), the same rule the window composition applies to
/// `view/title` and `view/item/context` icons. A manifest `$(Name)` with
/// upper case names no bundled glyph, so `pack` and install refuse it here
/// rather than letting the installed package be excluded per window (#352).
fn valid_codicon(value: &str) -> bool {
    value.len() >= 4
        && value.len() <= 163
        && value.starts_with("$(")
        && value.ends_with(')')
        && value[2..value.len() - 1]
            .bytes()
            .all(|byte| byte.is_ascii_lowercase() || byte.is_ascii_digit() || byte == b'-')
}

/// An Activity Bar ViewContainer icon is either a `$(codicon)` ThemeIcon or,
/// as VS Code's `viewsContainers` allows, a package-relative image path. The
/// host never reads that file: a path draws only when it names an entry of the
/// host's compiled-in vocabulary, so the path is bounded here and nothing more.
/// VS Code evaluates a full context-key expression in `when`. A SENP title item
/// may only name its Views, as `view == <id>` clauses joined by `||`; any other
/// operator, key or grouping is refused instead of being evaluated partially.
pub fn view_title_when_views(when: &str) -> Option<Vec<&str>> {
    if when.len() > 1024 {
        return None;
    }
    let mut views = Vec::new();
    for clause in when.split("||") {
        let view = clause
            .trim()
            .strip_prefix("view")?
            .trim_start()
            .strip_prefix("==")?
            .trim();
        if !valid_effect_identifier(view) || views.contains(&view) {
            return None;
        }
        views.push(view);
    }
    Some(views)
}

/// The decoded `when` of one `view/item/context` item. `contains` holds the
/// tokens of `viewItem =~ /token/` clauses, `equals` the values of
/// `viewItem == value` clauses, and `view` the optional `view == <id>` clause;
/// without one the item applies to every View of its package.
#[derive(Debug, Default, PartialEq, Eq)]
pub struct ViewItemWhen<'a> {
    pub contains: Vec<&'a str>,
    pub equals: Vec<&'a str>,
    pub view: Option<&'a str>,
}

/// VS Code evaluates a full context-key expression in `when`. A SENP item action
/// may only test the row's `contextValue`, as `&&`-joined `viewItem =~ /token/`
/// substring and `viewItem == value` equality clauses plus at most one
/// `view == <id>`. Regular-expression syntax, `||`, `!` and any other key are
/// refused instead of being evaluated partially; at least one `viewItem` clause
/// is required so an action never lands on host rows or on every item blindly.
pub fn view_item_when(when: &str) -> Option<ViewItemWhen<'_>> {
    if when.len() > 1024 {
        return None;
    }
    let token = |value: &str| {
        !value.is_empty()
            && value.len() <= 256
            && value
                .bytes()
                .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'_' | b'-'))
    };
    let mut parsed = ViewItemWhen::default();
    for clause in when.split("&&") {
        let clause = clause.trim();
        if let Some(rest) = clause.strip_prefix("viewItem") {
            let rest = rest.trim_start();
            if let Some(pattern) = rest.strip_prefix("=~") {
                let value = pattern.trim().strip_prefix('/')?.strip_suffix('/')?;
                if !token(value) || parsed.contains.contains(&value) {
                    return None;
                }
                parsed.contains.push(value);
            } else {
                let value = rest.strip_prefix("==")?.trim();
                if !token(value) || parsed.equals.contains(&value) {
                    return None;
                }
                parsed.equals.push(value);
            }
        } else {
            let view = clause
                .strip_prefix("view")?
                .trim_start()
                .strip_prefix("==")?
                .trim();
            if !valid_effect_identifier(view) || parsed.view.replace(view).is_some() {
                return None;
            }
        }
    }
    if parsed.contains.is_empty() && parsed.equals.is_empty() {
        return None;
    }
    Some(parsed)
}

fn valid_container_icon(value: &str) -> bool {
    valid_theme_icon(value) || valid_package_image_path(value)
}

fn valid_package_image_path(value: &str) -> bool {
    value.len() <= 260
        && (value.ends_with(".svg") || value.ends_with(".png"))
        && value.split('/').all(|segment| {
            !segment.is_empty()
                && segment != "."
                && segment != ".."
                && segment
                    .bytes()
                    .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'.' | b'_' | b'-'))
        })
}

fn valid_sha256(value: &str) -> bool {
    value.len() == 64
        && value
            .bytes()
            .all(|byte| byte.is_ascii_digit() || (b'a'..=b'f').contains(&byte))
}

fn valid_scope_name(value: &str) -> bool {
    !value.is_empty()
        && value.len() <= 256
        && value
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'.' | b'-' | b'_' | b'+'))
        && !value.starts_with(['.', '-'])
        && !value.ends_with(['.', '-'])
}

fn valid_asset_path(value: &str) -> bool {
    value.starts_with("assets/")
        && value.len() <= 1024
        && normalized_entry_name(value.as_bytes()).is_ok()
}

fn unique_bounded_strings(values: &[String], maximum: usize) -> bool {
    values.len() <= maximum
        && values
            .iter()
            .all(|value| !value.is_empty() && value.len() <= 1024)
        && values.iter().collect::<BTreeSet<_>>().len() == values.len()
}

#[cfg(test)]
fn validate_manifest(manifest: &Manifest) -> Result<(), SenpError> {
    validate_manifest_as(manifest, AbiAdmission::Current).map(|_| ())
}

fn validate_manifest_as(manifest: &Manifest, admission: AbiAdmission) -> Result<bool, SenpError> {
    if !matches!(manifest.schema_version, FORMAT_VERSION | 2) {
        return Err(SenpError::new(
            ErrorCode::UnsupportedSchema,
            manifest.schema_version.to_string(),
        ));
    }
    let effect_runtime = manifest.schema_version == 2;
    let valid_view_id = if effect_runtime {
        valid_effect_identifier
    } else {
        valid_workbench_identifier
    };
    if !valid_package_id(&manifest.id) || !valid_identifier(&manifest.publisher) {
        return Err(SenpError::new(
            ErrorCode::InvalidManifest,
            "invalid id or publisher",
        ));
    }
    if manifest.display_name.trim().is_empty()
        || manifest.display_name.len() > 160
        || manifest.description.len() > 512
        || manifest.version.trim().is_empty()
        || manifest.engines.sakura.trim().is_empty()
    {
        return Err(SenpError::new(
            ErrorCode::InvalidManifest,
            "invalid display metadata",
        ));
    }
    let compatible = match &manifest.runtime {
        Some(runtime) => runtime_compatibility(manifest.schema_version, runtime, admission)?,
        None => true,
    };
    let activation_events: BTreeSet<_> = manifest
        .activation_events
        .iter()
        .map(String::as_str)
        .collect();
    if activation_events.len() != manifest.activation_events.len()
        || (!effect_runtime
            && manifest.runtime.is_some()
            && (activation_events.len() != 1 || !activation_events.contains("onStartupFinished")))
        || (manifest.runtime.is_none() && !activation_events.is_empty())
    {
        return Err(SenpError::new(
            ErrorCode::InvalidManifest,
            "unknown or duplicate activation event",
        ));
    }
    let capabilities: BTreeSet<_> = manifest.capabilities.iter().map(String::as_str).collect();
    if capabilities.len() != manifest.capabilities.len()
        || manifest.capabilities.iter().any(|value| {
            if effect_runtime {
                !matches!(
                    value.as_str(),
                    "workbench.views.tree"
                        | "workbench.documents.readonly"
                        | "workbench.commands"
                        | "workspace.repositories.read"
                        | "tools.github.repository.read"
                )
            } else {
                !matches!(value.as_str(), "editor.visibleText" | "editor.decorations")
            }
        })
    {
        return Err(SenpError::new(
            ErrorCode::InvalidManifest,
            "unknown or duplicate capability",
        ));
    }
    if manifest.runtime.is_none() && !capabilities.is_empty() {
        return Err(SenpError::new(
            ErrorCode::InvalidManifest,
            "declarative extension cannot request runtime capabilities",
        ));
    }
    if manifest
        .contributes
        .editor_decorations
        .iter()
        .any(|value| value.kind != "indent" || !valid_identifier(&value.id))
    {
        return Err(SenpError::new(
            ErrorCode::InvalidManifest,
            "invalid editor decoration contribution",
        ));
    }
    let contribution_ids: BTreeSet<_> = manifest
        .contributes
        .editor_decorations
        .iter()
        .map(|value| value.id.as_str())
        .collect();
    if contribution_ids.len() != manifest.contributes.editor_decorations.len()
        || (!manifest.contributes.editor_decorations.is_empty()
            && (manifest.runtime.is_none()
                || !capabilities.contains("editor.visibleText")
                || !capabilities.contains("editor.decorations")))
    {
        return Err(SenpError::new(
            ErrorCode::InvalidManifest,
            "decoration capabilities or contribution ids are invalid",
        ));
    }

    if manifest.contributes.languages.len() > 64 || manifest.contributes.grammars.len() > 128 {
        return Err(SenpError::new(
            ErrorCode::InvalidManifest,
            "too many language or grammar contributions",
        ));
    }
    let mut language_ids = BTreeSet::new();
    for language in &manifest.contributes.languages {
        let has_selector = !language.extensions.is_empty()
            || !language.filenames.is_empty()
            || !language.filename_patterns.is_empty()
            || language.first_line.is_some();
        if !valid_identifier(&language.id)
            || !language_ids.insert(language.id.as_str())
            || !has_selector
            || !unique_bounded_strings(&language.aliases, 32)
            || !unique_bounded_strings(&language.extensions, 128)
            || !unique_bounded_strings(&language.filenames, 128)
            || !unique_bounded_strings(&language.filename_patterns, 128)
            || !unique_bounded_strings(&language.mimetypes, 32)
            || language
                .extensions
                .iter()
                .any(|value| !value.starts_with('.') || value.contains(['/', '\\', ':', '\0']))
            || language
                .filenames
                .iter()
                .any(|value| value.contains(['/', '\\', ':', '\0']))
            || language
                .first_line
                .as_ref()
                .is_some_and(|value| value.is_empty() || value.len() > 4096 || value.contains('\0'))
            || language
                .configuration
                .as_ref()
                .is_some_and(|value| !valid_asset_path(value))
        {
            return Err(SenpError::new(
                ErrorCode::InvalidManifest,
                "invalid language contribution",
            ));
        }
    }

    let mut grammar_scopes = BTreeSet::new();
    for grammar in &manifest.contributes.grammars {
        if !valid_scope_name(&grammar.scope_name)
            || !grammar_scopes.insert(grammar.scope_name.as_str())
            || !valid_asset_path(&grammar.path)
            || grammar
                .language
                .as_ref()
                .is_some_and(|value| !language_ids.contains(value.as_str()))
            || !unique_bounded_strings(&grammar.inject_to, 64)
            || grammar
                .inject_to
                .iter()
                .any(|value| !valid_scope_name(value))
            || grammar
                .embedded_languages
                .iter()
                .any(|(scope, language)| !valid_scope_name(scope) || !valid_identifier(language))
            || !unique_bounded_strings(&grammar.balanced_bracket_scopes, 64)
            || !unique_bounded_strings(&grammar.unbalanced_bracket_scopes, 64)
        {
            return Err(SenpError::new(
                ErrorCode::InvalidManifest,
                "invalid grammar contribution",
            ));
        }
    }
    if manifest.contributes.languages.is_empty() != manifest.contributes.grammars.is_empty() {
        return Err(SenpError::new(
            ErrorCode::InvalidManifest,
            "language and grammar contributions must be paired",
        ));
    }

    let containers = &manifest.contributes.views_containers.activitybar;
    if containers.len() > 32 || manifest.contributes.views.len() > 32 {
        return Err(SenpError::new(
            ErrorCode::InvalidManifest,
            "too many workbench view contributions",
        ));
    }
    let mut container_ids = BTreeSet::new();
    for container in containers {
        if !valid_view_id(&container.id)
            || !container_ids.insert(container.id.as_str())
            || container.title.trim().is_empty()
            || container.title.len() > 160
            || !valid_container_icon(&container.icon)
            || !(-10_000..=10_000).contains(&container.order)
        {
            return Err(SenpError::new(
                ErrorCode::InvalidManifest,
                "invalid activity bar ViewContainer contribution",
            ));
        }
    }
    let mut view_ids = BTreeSet::new();
    let mut view_count = 0usize;
    for (container_id, views) in &manifest.contributes.views {
        view_count = view_count.saturating_add(views.len());
        if !container_ids.contains(container_id.as_str()) || views.is_empty() || views.len() > 32 {
            return Err(SenpError::new(
                ErrorCode::InvalidManifest,
                "View contribution references an unknown or empty ViewContainer",
            ));
        }
        for view in views {
            if !valid_view_id(&view.id)
                || !view_ids.insert(view.id.as_str())
                || view.name.trim().is_empty()
                || view.name.len() > 160
                || !valid_workbench_identifier(&view.provider)
                || !(-10_000..=10_000).contains(&view.order)
            {
                return Err(SenpError::new(
                    ErrorCode::InvalidManifest,
                    "invalid workbench View contribution",
                ));
            }
        }
    }
    if view_count > 128
        || containers.is_empty() != manifest.contributes.views.is_empty()
        || container_ids
            .iter()
            .any(|container_id| !manifest.contributes.views.contains_key(*container_id))
    {
        return Err(SenpError::new(
            ErrorCode::InvalidManifest,
            "ViewContainer and View contributions must form a complete bounded hierarchy",
        ));
    }
    if effect_runtime {
        validate_effect_contributions(manifest, &view_ids, &capabilities)?;
    } else if !manifest.contributes.commands.is_empty() || !manifest.contributes.menus.is_empty() {
        return Err(SenpError::new(
            ErrorCode::InvalidManifest,
            "commands require schema 2",
        ));
    }
    if manifest.contributes.editor_decorations.is_empty()
        && manifest.contributes.languages.is_empty()
        && containers.is_empty()
    {
        return Err(SenpError::new(
            ErrorCode::InvalidManifest,
            "extension contributes no supported capability",
        ));
    }
    Ok(compatible)
}

/// Whether a runtime contract names the ABI this build executes. Packing,
/// verification and installation admit only that ABI. An installed schema-2
/// package built for another runtime ABI -- older after a WIT change, or newer
/// from a later product sharing the profile -- is still listed so the user can
/// see, update or remove it, but it is reported incompatible and never started.
fn runtime_compatibility(
    schema_version: u32,
    runtime: &RuntimeContract,
    admission: AbiAdmission,
) -> Result<bool, SenpError> {
    let current = if schema_version == 2 { ABI_V2 } else { ABI };
    if runtime.module != MODULE_PATH {
        return Err(SenpError::new(ErrorCode::AbiMismatch, runtime.abi.clone()));
    }
    if runtime.abi == current {
        return Ok(true);
    }
    if admission == AbiAdmission::Installed
        && schema_version == 2
        && is_extension_abi(&runtime.abi)
        && runtime.abi != ABI
    {
        return Ok(false);
    }
    Err(SenpError::new(ErrorCode::AbiMismatch, runtime.abi.clone()))
}

/// `sakura:senp/extension@<major>.<minor>.<patch>` with bounded decimal parts.
fn is_extension_abi(value: &str) -> bool {
    value
        .strip_prefix("sakura:senp/extension@")
        .map(|version| version.split('.').collect::<Vec<_>>())
        .is_some_and(|parts| {
            parts.len() == 3
                && parts.iter().all(|part| {
                    !part.is_empty() && part.len() <= 5 && part.bytes().all(|b| b.is_ascii_digit())
                })
        })
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum AbiAdmission {
    /// Packing, archive verification and installation: only the current ABI.
    Current,
    /// Reading already-installed content: another runtime ABI is listed as incompatible.
    Installed,
}

fn validate_effect_contributions(
    manifest: &Manifest,
    views: &BTreeSet<&str>,
    capabilities: &BTreeSet<&str>,
) -> Result<(), SenpError> {
    let invalid = || {
        SenpError::new(
            ErrorCode::InvalidManifest,
            "invalid event/effect contributions",
        )
    };
    if manifest.runtime.is_none()
        || !manifest.contributes.editor_decorations.is_empty()
        || !manifest.contributes.languages.is_empty()
        || !manifest.contributes.grammars.is_empty()
        || views.is_empty()
        || !capabilities.contains("workbench.views.tree")
        || manifest
            .contributes
            .views
            .values()
            .flatten()
            .any(|view| view.provider != "senp.tree")
        || manifest.activation_events.is_empty()
        || manifest.activation_events.len() > 128
        || manifest.activation_events.iter().any(|event| {
            !event
                .strip_prefix("onView:")
                .is_some_and(|view| views.contains(view))
        })
        || manifest.contributes.commands.len() > 128
    {
        return Err(invalid());
    }
    let mut commands = BTreeSet::new();
    for command in &manifest.contributes.commands {
        if !valid_effect_identifier(&command.command)
            || !commands.insert(command.command.as_str())
            || command.title.trim().is_empty()
            || command.title.len() > 160
            || command.title.chars().any(char::is_control)
            || command
                .icon
                .as_deref()
                .is_some_and(|icon| !valid_theme_icon(icon) && !valid_package_image_path(icon))
        {
            return Err(invalid());
        }
    }
    let menus = &manifest.contributes.menus.view_title;
    if menus.len() > 64 {
        return Err(invalid());
    }
    let mut placed = BTreeSet::new();
    let mut per_view = BTreeMap::<&str, usize>::new();
    for item in menus {
        let icon = manifest
            .contributes
            .commands
            .iter()
            .find(|command| command.command == item.command)
            .and_then(|command| command.icon.as_deref());
        let Some(targets) = view_title_when_views(&item.when) else {
            return Err(invalid());
        };
        // A title item is a codicon button: without one it would need VS Code's
        // text rendering, and any group but `navigation` its overflow menu.
        if menus.len() > 64 || !icon.is_some_and(valid_codicon) || item.group != "navigation" {
            return Err(invalid());
        }
        for view in targets {
            let count = per_view.entry(view).or_default();
            *count += 1;
            if !views.contains(view) || !placed.insert((view, item.command.as_str())) || *count > 8
            {
                return Err(invalid());
            }
        }
    }
    let items = &manifest.contributes.menus.view_item_context;
    if items.len() > 64 {
        return Err(invalid());
    }
    let mut placed = BTreeSet::new();
    let mut per_view = BTreeMap::<&str, usize>::new();
    for item in items {
        let icon = manifest
            .contributes
            .commands
            .iter()
            .find(|command| command.command == item.command)
            .and_then(|command| command.icon.as_deref());
        let Some(when) = view_item_when(&item.when) else {
            return Err(invalid());
        };
        // An inline action is an icon on the row; other groups are VS Code's
        // context menu, which a native Tree row does not have. The icon is a
        // lower-case codicon or a package-relative image path.
        if !icon.is_some_and(|icon| valid_codicon(icon) || valid_package_image_path(icon))
            || item.group != "inline"
        {
            return Err(invalid());
        }
        let targets: Vec<&str> = match when.view {
            Some(view) if views.contains(view) => vec![view],
            Some(_) => return Err(invalid()),
            None => views.iter().copied().collect(),
        };
        for view in targets {
            let count = per_view.entry(view).or_default();
            *count += 1;
            if !placed.insert((view, item.command.as_str())) || *count > 8 {
                return Err(invalid());
            }
        }
    }
    if (!commands.is_empty() && !capabilities.contains("workbench.commands"))
        || (capabilities
            .iter()
            .any(|capability| capability.starts_with("tools.github."))
            && !capabilities.contains("workspace.repositories.read"))
    {
        return Err(invalid());
    }
    Ok(())
}

fn validate_manifest_entries(
    manifest: &Manifest,
    entries: &BTreeMap<String, Vec<u8>>,
) -> Result<(), SenpError> {
    let mut declared_assets = manifest
        .contributes
        .grammars
        .iter()
        .map(|grammar| grammar.path.as_str())
        .collect::<BTreeSet<_>>();
    declared_assets.extend(
        manifest
            .contributes
            .languages
            .iter()
            .filter_map(|language| language.configuration.as_deref()),
    );
    if let Some(missing) = declared_assets
        .into_iter()
        .find(|path| !entries.contains_key(*path))
    {
        return Err(SenpError::new(ErrorCode::MissingRequiredEntry, missing));
    }
    if manifest.runtime.is_some() && !entries.contains_key(MODULE_PATH) {
        return Err(SenpError::new(ErrorCode::MissingRequiredEntry, MODULE_PATH));
    }
    if manifest.runtime.is_none() && entries.contains_key(MODULE_PATH) {
        return Err(SenpError::new(
            ErrorCode::InvalidManifest,
            "declarative extension unexpectedly contains a runtime module",
        ));
    }
    Ok(())
}

#[derive(Debug)]
struct NoDuplicateJson(serde_json::Value);

impl<'de> Deserialize<'de> for NoDuplicateJson {
    fn deserialize<D>(deserializer: D) -> Result<Self, D::Error>
    where
        D: Deserializer<'de>,
    {
        struct JsonVisitor;
        impl<'de> Visitor<'de> for JsonVisitor {
            type Value = NoDuplicateJson;
            fn expecting(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
                formatter.write_str("JSON value")
            }
            fn visit_bool<E: de::Error>(self, value: bool) -> Result<Self::Value, E> {
                Ok(NoDuplicateJson(value.into()))
            }
            fn visit_i64<E: de::Error>(self, value: i64) -> Result<Self::Value, E> {
                Ok(NoDuplicateJson(value.into()))
            }
            fn visit_u64<E: de::Error>(self, value: u64) -> Result<Self::Value, E> {
                Ok(NoDuplicateJson(value.into()))
            }
            fn visit_f64<E: de::Error>(self, value: f64) -> Result<Self::Value, E> {
                serde_json::Number::from_f64(value)
                    .map(|value| NoDuplicateJson(value.into()))
                    .ok_or_else(|| E::custom("non-finite number"))
            }
            fn visit_str<E: de::Error>(self, value: &str) -> Result<Self::Value, E> {
                Ok(NoDuplicateJson(value.into()))
            }
            fn visit_string<E: de::Error>(self, value: String) -> Result<Self::Value, E> {
                Ok(NoDuplicateJson(value.into()))
            }
            fn visit_none<E: de::Error>(self) -> Result<Self::Value, E> {
                Ok(NoDuplicateJson(serde_json::Value::Null))
            }
            fn visit_unit<E: de::Error>(self) -> Result<Self::Value, E> {
                self.visit_none()
            }
            fn visit_seq<A: SeqAccess<'de>>(
                self,
                mut sequence: A,
            ) -> Result<Self::Value, A::Error> {
                let mut values = Vec::new();
                while let Some(value) = sequence.next_element::<NoDuplicateJson>()? {
                    values.push(value.0);
                }
                Ok(NoDuplicateJson(values.into()))
            }
            fn visit_map<A: MapAccess<'de>>(self, mut map: A) -> Result<Self::Value, A::Error> {
                let mut values = serde_json::Map::new();
                while let Some((key, value)) = map.next_entry::<String, NoDuplicateJson>()? {
                    if values.insert(key.clone(), value.0).is_some() {
                        return Err(de::Error::custom(format!("duplicate member: {key}")));
                    }
                }
                Ok(NoDuplicateJson(values.into()))
            }
        }
        deserializer.deserialize_any(JsonVisitor)
    }
}

fn strict_json<T: DeserializeOwned>(bytes: &[u8]) -> Result<T, SenpError> {
    let mut deserializer = serde_json::Deserializer::from_slice(bytes);
    let value = NoDuplicateJson::deserialize(&mut deserializer)
        .map_err(|error| SenpError::new(ErrorCode::InvalidManifest, error.to_string()))?;
    deserializer
        .end()
        .map_err(|error| SenpError::new(ErrorCode::InvalidManifest, error.to_string()))?;
    serde_json::from_value(value.0)
        .map_err(|error| SenpError::new(ErrorCode::InvalidManifest, error.to_string()))
}

fn parse_manifest(bytes: &[u8]) -> Result<Manifest, SenpError> {
    parse_manifest_as(bytes, AbiAdmission::Current).map(|(manifest, _)| manifest)
}

/// Parses a manifest and reports whether its runtime ABI is the one this build
/// executes. Only `AbiAdmission::Installed` can return `false`.
fn parse_manifest_as(bytes: &[u8], admission: AbiAdmission) -> Result<(Manifest, bool), SenpError> {
    // Select the version before deserializing its fields, but only after strict
    // duplicate-member and trailing-input checks have consumed the entire JSON.
    let value: serde_json::Value = strict_json(bytes)?;
    let schema = value
        .get("schemaVersion")
        .and_then(serde_json::Value::as_u64)
        .ok_or_else(|| SenpError::new(ErrorCode::InvalidManifest, "invalid schemaVersion"))?;
    match schema {
        // A v2 package must name the runtime it targets before anything else is
        // read. Its contributions mean nothing without the effect host, and a
        // manifest that omits `runtime` would otherwise validate as a
        // declarative extension and be installed as one. Never fall back to v1.
        1 | 2 => {
            if schema == 2 {
                let runtime: RuntimeContract =
                    serde_json::from_value(value.get("runtime").cloned().unwrap_or_default())
                        .map_err(|error| {
                            SenpError::new(ErrorCode::InvalidManifest, error.to_string())
                        })?;
                runtime_compatibility(2, &runtime, admission)?;
            }
            let manifest: Manifest = serde_json::from_value(value)
                .map_err(|error| SenpError::new(ErrorCode::InvalidManifest, error.to_string()))?;
            let compatible = validate_manifest_as(&manifest, admission)?;
            Ok((manifest, compatible))
        }
        _ => Err(SenpError::new(
            ErrorCode::UnsupportedSchema,
            schema.to_string(),
        )),
    }
}

/// zip 5.1.1 folds central-directory entries with one exact name into a single
/// index, so `read_zip_entries` never sees the duplicate. The end-of-central-
/// directory record states how many entries the archive really holds; any
/// difference from what the reader indexed is a duplicate name. ZIP64 is not
/// part of the format (64 MiB and 256 entries at most), so an archive that
/// defers its count to a ZIP64 record is rejected rather than trusted.
fn reject_collapsed_duplicate_entries(bytes: &[u8], parsed: usize) -> Result<(), SenpError> {
    const EOCD_LEN: usize = 22;
    const EOCD_SIGNATURE: [u8; 4] = [0x50, 0x4b, 0x05, 0x06];
    const ZIP64_LOCATOR_SIGNATURE: [u8; 4] = [0x50, 0x4b, 0x06, 0x07];
    let invalid = |detail: &str| SenpError::new(ErrorCode::InvalidZip, detail);
    let comment_ends_the_file = |at: usize| {
        let comment = usize::from(u16::from_le_bytes([bytes[at + 20], bytes[at + 21]]));
        at + EOCD_LEN + comment == bytes.len()
    };
    let record = (0..=bytes.len().saturating_sub(EOCD_LEN))
        .rev()
        .find(|&at| {
            bytes.len() >= EOCD_LEN
                && bytes[at..at + 4] == EOCD_SIGNATURE
                && comment_ends_the_file(at)
        })
        .ok_or_else(|| invalid("end of central directory record not found"))?;
    let declared = u16::from_le_bytes([bytes[record + 10], bytes[record + 11]]);
    if declared == u16::MAX
        || (record >= 20 && bytes[record - 20..record - 16] == ZIP64_LOCATOR_SIGNATURE)
    {
        return Err(invalid("ZIP64 archives are not supported"));
    }
    if usize::from(declared) != parsed {
        return Err(SenpError::new(
            ErrorCode::DuplicateEntry,
            format!("central directory declares {declared} entries, {parsed} are distinct"),
        ));
    }
    Ok(())
}

fn read_zip_entries<R: Read + Seek>(
    archive: &mut ZipArchive<R>,
) -> Result<BTreeMap<String, Vec<u8>>, SenpError> {
    if archive.len() > MAX_ENTRIES {
        return Err(SenpError::new(
            ErrorCode::TooManyEntries,
            archive.len().to_string(),
        ));
    }
    let mut entries = BTreeMap::new();
    let mut folded = BTreeSet::new();
    let mut expanded = 0u64;
    for index in 0..archive.len() {
        let mut entry = archive
            .by_index(index)
            .map_err(|error| SenpError::new(ErrorCode::InvalidZip, error.to_string()))?;
        let name = normalized_entry_name(entry.name_raw())?;
        if !is_allowed_entry(&name) {
            return Err(SenpError::new(ErrorCode::UnsupportedEntry, name));
        }
        if entry.is_dir()
            || entry
                .unix_mode()
                .is_some_and(|mode| mode & 0o170000 != 0 && mode & 0o170000 != 0o100000)
        {
            return Err(SenpError::new(ErrorCode::UnsupportedEntry, name));
        }
        if !matches!(
            entry.compression(),
            CompressionMethod::Stored | CompressionMethod::Deflated
        ) {
            return Err(SenpError::new(ErrorCode::UnsupportedCompression, name));
        }
        if entry.size() > MAX_ENTRY_BYTES
            || (is_document(&name) && entry.size() > MAX_DOCUMENT_BYTES)
        {
            return Err(SenpError::new(ErrorCode::EntryTooLarge, name));
        }
        expanded = expanded
            .checked_add(entry.size())
            .ok_or_else(|| SenpError::new(ErrorCode::ExpandedSizeLimit, name.clone()))?;
        if expanded > MAX_EXPANDED_BYTES {
            return Err(SenpError::new(
                ErrorCode::ExpandedSizeLimit,
                expanded.to_string(),
            ));
        }
        if entry.size() > 0
            && (entry.compressed_size() == 0
                || entry.size() / entry.compressed_size().max(1) > MAX_COMPRESSION_RATIO)
        {
            return Err(SenpError::new(ErrorCode::CompressionRatioLimit, name));
        }
        if !folded.insert(case_fold_path(&name)) || entries.contains_key(&name) {
            return Err(SenpError::new(ErrorCode::DuplicateEntry, name));
        }
        // The exact read makes the declared-size budget above a bound on the
        // bytes that really arrive.
        let declared = entry.size();
        let bytes = read_entry_exact(&mut entry, declared)?;
        entries.insert(name, bytes);
    }
    Ok(entries)
}

/// Reads one entry through a bound of `declared + 1` bytes, so a stream that
/// inflates past its declared size is stopped after one byte of excess instead
/// of being drained into memory. Fewer bytes than declared are rejected too.
fn read_entry_exact(reader: &mut impl Read, declared: u64) -> Result<Vec<u8>, SenpError> {
    let mismatch = || SenpError::new(ErrorCode::InvalidZip, "uncompressed size mismatch");
    let bound = declared.saturating_add(1);
    let mut bytes = Vec::with_capacity(usize::try_from(bound).map_err(|_| mismatch())?);
    reader
        .take(bound)
        .read_to_end(&mut bytes)
        .map_err(|error| SenpError::new(ErrorCode::InvalidZip, error.to_string()))?;
    if bytes.len() as u64 != declared {
        return Err(mismatch());
    }
    Ok(bytes)
}

fn parse_checksums(bytes: &[u8]) -> Result<BTreeMap<String, String>, SenpError> {
    let text = std::str::from_utf8(bytes)
        .map_err(|_| SenpError::new(ErrorCode::InvalidUtf8, CHECKSUM_PATH))?;
    if text.is_empty() || !text.ends_with('\n') || text.contains('\r') {
        return Err(SenpError::new(
            ErrorCode::InvalidChecksumDocument,
            "non-canonical line endings",
        ));
    }
    let mut checksums = BTreeMap::new();
    let mut previous: Option<&str> = None;
    for line in text.lines() {
        let (digest, path) = line
            .split_once("  ")
            .ok_or_else(|| SenpError::new(ErrorCode::InvalidChecksumDocument, line))?;
        if digest.len() != 64
            || !digest
                .bytes()
                .all(|byte| byte.is_ascii_digit() || (b'a'..=b'f').contains(&byte))
        {
            return Err(SenpError::new(
                ErrorCode::InvalidChecksumDocument,
                "invalid SHA-256",
            ));
        }
        normalized_entry_name(path.as_bytes())?;
        if previous.is_some_and(|value| value >= path)
            || checksums
                .insert(path.to_owned(), digest.to_owned())
                .is_some()
        {
            return Err(SenpError::new(
                ErrorCode::InvalidChecksumDocument,
                "entries are not unique and sorted",
            ));
        }
        previous = Some(path);
    }
    Ok(checksums)
}

fn parse_signature(bytes: &[u8]) -> Result<Signature, SenpError> {
    let text = std::str::from_utf8(bytes)
        .map_err(|_| SenpError::new(ErrorCode::InvalidSignature, "signature is not UTF-8"))?;
    let encoded = text
        .strip_suffix('\n')
        .filter(|value| !value.contains(['\r', '\n']))
        .ok_or_else(|| {
            SenpError::new(
                ErrorCode::InvalidSignature,
                "signature is not a canonical line",
            )
        })?;
    Ok(Signature::from_bytes(&decode_hex::<64>(encoded)?))
}

#[derive(Debug)]
struct VerifiedArchive {
    package: VerifiedPackage,
    entries: BTreeMap<String, Vec<u8>>,
}

fn verify_archive_snapshot(
    snapshot: &ArchiveSnapshot,
    policy: &TrustPolicy,
) -> Result<VerifiedArchive, SenpError> {
    let archive_sha256 = &snapshot.archive_sha256;
    if let TrustPolicy::BuiltIn {
        expected_archive_sha256,
    } = policy
    {
        if expected_archive_sha256 != archive_sha256 {
            return Err(SenpError::new(
                ErrorCode::ChecksumMismatch,
                "built-in package pin mismatch",
            ));
        }
    }
    let mut archive = ZipArchive::new(Cursor::new(snapshot.bytes.as_slice()))
        .map_err(|error| SenpError::new(ErrorCode::InvalidZip, error.to_string()))?;
    reject_collapsed_duplicate_entries(&snapshot.bytes, archive.len())?;
    let entries = read_zip_entries(&mut archive)?;
    for required in [MANIFEST_PATH, README_PATH, LICENSE_PATH, CHECKSUM_PATH] {
        if !entries.contains_key(required) {
            return Err(SenpError::new(ErrorCode::MissingRequiredEntry, required));
        }
    }
    for path in [MANIFEST_PATH, README_PATH, LICENSE_PATH] {
        std::str::from_utf8(&entries[path])
            .map_err(|_| SenpError::new(ErrorCode::InvalidUtf8, path))?;
    }
    let checksums = parse_checksums(&entries[CHECKSUM_PATH])?;
    let payload_paths: BTreeSet<_> = entries
        .keys()
        .filter(|path| path.as_str() != CHECKSUM_PATH && path.as_str() != SIGNATURE_PATH)
        .cloned()
        .collect();
    if payload_paths != checksums.keys().cloned().collect() {
        return Err(SenpError::new(
            ErrorCode::InvalidChecksumDocument,
            "checksum coverage mismatch",
        ));
    }
    for (path, expected) in &checksums {
        if hex(&Sha256::digest(&entries[path])) != *expected {
            return Err(SenpError::new(ErrorCode::ChecksumMismatch, path));
        }
    }
    let manifest = parse_manifest(&entries[MANIFEST_PATH])?;
    validate_manifest_entries(&manifest, &entries)?;

    let signed = if let Some(signature_bytes) = entries.get(SIGNATURE_PATH) {
        let signature = parse_signature(signature_bytes)?;
        let key = match policy {
            TrustPolicy::PublisherKeys(keys) => keys.get(&manifest.publisher).ok_or_else(|| {
                SenpError::new(ErrorCode::UntrustedPublisher, &manifest.publisher)
            })?,
            TrustPolicy::BuiltIn { .. } | TrustPolicy::DeveloperUnsigned => {
                return Err(SenpError::new(
                    ErrorCode::UntrustedPublisher,
                    "no publisher key supplied",
                ));
            }
        };
        let verifying_key = VerifyingKey::from_bytes(key)
            .map_err(|error| SenpError::new(ErrorCode::InvalidSignature, error.to_string()))?;
        verifying_key
            .verify_strict(&entries[CHECKSUM_PATH], &signature)
            .map_err(|error| SenpError::new(ErrorCode::InvalidSignature, error.to_string()))?;
        true
    } else {
        if matches!(policy, TrustPolicy::PublisherKeys(_)) {
            return Err(SenpError::new(
                ErrorCode::UnsignedPackage,
                "publisher package has no signature",
            ));
        }
        false
    };
    Ok(VerifiedArchive {
        package: VerifiedPackage {
            manifest,
            archive_sha256: archive_sha256.clone(),
            signed,
            entry_count: entries.len(),
            expanded_bytes: entries.values().map(|value| value.len() as u64).sum(),
        },
        entries,
    })
}

pub fn verify_package(path: &Path, policy: &TrustPolicy) -> Result<VerifiedPackage, SenpError> {
    let snapshot = read_archive_snapshot(path)?;
    Ok(verify_archive_snapshot(&snapshot, policy)?.package)
}

pub fn pack_directory(
    source: &Path,
    destination: &Path,
    signing_key: Option<[u8; 32]>,
) -> Result<String, SenpError> {
    let mut payloads = BTreeMap::new();
    for required in [MANIFEST_PATH, README_PATH, LICENSE_PATH] {
        payloads.insert(required.to_owned(), read_source_payload(source, required)?);
    }
    let manifest = parse_manifest(&payloads[MANIFEST_PATH])?;
    if manifest.runtime.is_some() {
        payloads.insert(
            MODULE_PATH.to_owned(),
            read_source_payload(source, MODULE_PATH)?,
        );
    } else if source.join(MODULE_PATH).exists() {
        return Err(SenpError::new(
            ErrorCode::InvalidManifest,
            "declarative extension unexpectedly contains a runtime module",
        ));
    }
    let changelog = "CHANGELOG.md";
    if source.join(changelog).exists() {
        payloads.insert(
            changelog.to_owned(),
            read_source_payload(source, changelog)?,
        );
    }
    let assets = source.join("assets");
    if assets.exists() {
        let metadata = fs::symlink_metadata(&assets)
            .map_err(|error| io_error("inspect assets directory", error))?;
        if metadata.file_type().is_symlink() {
            return Err(SenpError::new(
                ErrorCode::ReparsePoint,
                assets.display().to_string(),
            ));
        }
        if !metadata.is_dir() {
            return Err(SenpError::new(ErrorCode::UnsupportedEntry, "assets"));
        }
        collect_asset_payloads(&assets, Path::new(""), &mut payloads, 0)?;
    }
    if payloads.len() + 2 > MAX_ENTRIES {
        return Err(SenpError::new(
            ErrorCode::TooManyEntries,
            payloads.len().to_string(),
        ));
    }
    let expanded_bytes = payloads.iter().try_fold(0u64, |total, (path, bytes)| {
        total
            .checked_add(bytes.len() as u64)
            .filter(|value| *value <= MAX_EXPANDED_BYTES)
            .ok_or_else(|| SenpError::new(ErrorCode::ExpandedSizeLimit, path))
    })?;
    if expanded_bytes > MAX_EXPANDED_BYTES {
        return Err(SenpError::new(
            ErrorCode::ExpandedSizeLimit,
            expanded_bytes.to_string(),
        ));
    }
    validate_manifest_entries(&manifest, &payloads)?;
    let mut checksum_document = String::new();
    for (path, bytes) in &payloads {
        checksum_document.push_str(&hex(&Sha256::digest(bytes)));
        checksum_document.push_str("  ");
        checksum_document.push_str(path);
        checksum_document.push('\n');
    }
    let signature = signing_key.map(|key| {
        use ed25519_dalek::{Signer, SigningKey};
        let signing_key = SigningKey::from_bytes(&key);
        format!(
            "{}\n",
            hex(&signing_key.sign(checksum_document.as_bytes()).to_bytes())
        )
    });
    if let Some(parent) = destination.parent() {
        fs::create_dir_all(parent).map_err(|error| io_error("create package directory", error))?;
    }
    let file = File::create(destination).map_err(|error| io_error("create package", error))?;
    let mut writer = ZipWriter::new(file);
    let options = SimpleFileOptions::default()
        .compression_method(CompressionMethod::Stored)
        .last_modified_time(DateTime::default())
        .unix_permissions(0o100644);
    for (path, bytes) in &payloads {
        writer
            .start_file(path, options)
            .map_err(|error| SenpError::new(ErrorCode::InvalidZip, error.to_string()))?;
        writer
            .write_all(bytes)
            .map_err(|error| io_error("write package", error))?;
    }
    writer
        .start_file(CHECKSUM_PATH, options)
        .map_err(|error| SenpError::new(ErrorCode::InvalidZip, error.to_string()))?;
    writer
        .write_all(checksum_document.as_bytes())
        .map_err(|error| io_error("write checksums", error))?;
    if let Some(signature) = signature {
        writer
            .start_file(SIGNATURE_PATH, options)
            .map_err(|error| SenpError::new(ErrorCode::InvalidZip, error.to_string()))?;
        writer
            .write_all(signature.as_bytes())
            .map_err(|error| io_error("write signature", error))?;
    }
    writer
        .finish()
        .map_err(|error| SenpError::new(ErrorCode::InvalidZip, error.to_string()))?;
    Ok(read_archive_snapshot(destination)?.archive_sha256)
}

fn reject_reparse_points(path: &Path) -> Result<(), SenpError> {
    let mut current = PathBuf::new();
    for component in path.components() {
        current.push(component.as_os_str());
        match fs::symlink_metadata(&current) {
            Ok(metadata) => {
                if metadata_is_reparse_point(&metadata) {
                    return Err(SenpError::new(
                        ErrorCode::ReparsePoint,
                        current.display().to_string(),
                    ));
                }
            }
            Err(error) if error.kind() == std::io::ErrorKind::NotFound => {}
            Err(error) => return Err(io_error("inspect install path", error)),
        }
    }
    Ok(())
}

fn publish_atomic(temporary: &Path, target: &Path) -> Result<(), SenpError> {
    #[cfg(windows)]
    {
        use std::os::windows::ffi::OsStrExt;
        const MOVEFILE_REPLACE_EXISTING: u32 = 0x1;
        const MOVEFILE_WRITE_THROUGH: u32 = 0x8;
        #[link(name = "kernel32")]
        unsafe extern "system" {
            fn MoveFileExW(existing: *const u16, replacement: *const u16, flags: u32) -> i32;
        }
        let from: Vec<u16> = temporary.as_os_str().encode_wide().chain(Some(0)).collect();
        let to: Vec<u16> = target.as_os_str().encode_wide().chain(Some(0)).collect();
        if unsafe {
            MoveFileExW(
                from.as_ptr(),
                to.as_ptr(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH,
            )
        } == 0
        {
            return Err(SenpError::new(
                ErrorCode::PublicationFailed,
                std::io::Error::last_os_error().to_string(),
            ));
        }
    }
    #[cfg(not(windows))]
    {
        if target.exists() {
            fs::remove_file(target).map_err(|error| io_error("replace profile state", error))?;
        }
        fs::rename(temporary, target).map_err(|error| io_error("publish profile state", error))?;
    }
    Ok(())
}

/// Distinguishes temporary names created within one clock tick: the Windows
/// system clock advances in ~15.6 ms steps, so nanoseconds alone collide.
static TEMPORARY_COUNTER: AtomicU64 = AtomicU64::new(0);

fn temporary_suffix() -> String {
    temporary_suffix_at(
        SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .map_or(0, |duration| duration.as_nanos()),
    )
}

fn temporary_suffix_at(nanos: u128) -> String {
    let sequence = TEMPORARY_COUNTER.fetch_add(1, Ordering::Relaxed);
    format!("{nanos}-{sequence}")
}

/// Lock file inside the install root that serializes every mutating command
/// (install, uninstall, set-enabled) across processes. Readers never take it.
const ROOT_LOCK_FILE: &str = ".senp-root.lock";
/// Longest a mutator waits for the lock before failing with `Busy`. Well under
/// the native 30 s tool timeout, so the caller gets a typed answer first.
const ROOT_LOCK_WAIT: Duration = Duration::from_secs(10);
const ROOT_LOCK_POLL_FIRST: Duration = Duration::from_millis(5);
const ROOT_LOCK_POLL_MAX: Duration = Duration::from_millis(200);

#[cfg(test)]
thread_local! {
    static ROOT_LOCK_WAIT_OVERRIDE: std::cell::Cell<Option<Duration>> =
        const { std::cell::Cell::new(None) };
}

fn root_lock_wait() -> Duration {
    #[cfg(test)]
    if let Some(wait) = ROOT_LOCK_WAIT_OVERRIDE.with(std::cell::Cell::get) {
        return wait;
    }
    ROOT_LOCK_WAIT
}

/// Exclusive hold on one install root. The OS releases it when the handle
/// closes, including when the process dies, so a crash never strands the root.
#[derive(Debug)]
struct RootLock {
    _file: File,
}

/// Polls `try_lock` with exponential backoff until `wait` has elapsed, then
/// fails with a typed `Busy`.
fn acquire_root_lock_within(root: &Path, wait: Duration) -> Result<RootLock, SenpError> {
    let path = root.join(ROOT_LOCK_FILE);
    reject_reparse_points(&path)?;
    let file = OpenOptions::new()
        .create(true)
        .truncate(false)
        .write(true)
        .open(&path)
        .map_err(|error| io_error("open install lock", error))?;
    let deadline = Instant::now() + wait;
    let mut delay = ROOT_LOCK_POLL_FIRST;
    loop {
        match file.try_lock() {
            Ok(()) => return Ok(RootLock { _file: file }),
            Err(TryLockError::WouldBlock) => {
                let now = Instant::now();
                if now >= deadline {
                    return Err(SenpError::new(
                        ErrorCode::Busy,
                        format!(
                            "another SENP package operation holds the install root; gave up after {} ms",
                            wait.as_millis()
                        ),
                    ));
                }
                std::thread::sleep(delay.min(deadline - now));
                delay = (delay * 2).min(ROOT_LOCK_POLL_MAX);
            }
            Err(TryLockError::Error(error)) => return Err(io_error("lock install root", error)),
        }
    }
}

/// Lock for a mutator that must not create the root: an absent root has
/// nothing to protect and the operation reports its own natural error.
fn lock_existing_root(root: &Path) -> Result<Option<RootLock>, SenpError> {
    match fs::metadata(root) {
        Ok(_) => acquire_root_lock_within(root, root_lock_wait()).map(Some),
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => Ok(None),
        Err(error) => Err(io_error("inspect install root", error)),
    }
}

fn write_profile_state(root: &Path, state: &ProfileState) -> Result<(), SenpError> {
    let state_root = root.join("profiles");
    fs::create_dir_all(&state_root).map_err(|error| io_error("create state root", error))?;
    let state_bytes = serde_json::to_vec_pretty(state)
        .map_err(|error| SenpError::new(ErrorCode::PublicationFailed, error.to_string()))?;
    let target = state_root.join(format!("{}.json", state.id));
    let temporary = state_root.join(format!(
        ".{}.{}.{}.tmp",
        state.id,
        std::process::id(),
        temporary_suffix()
    ));
    let mut file = OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(&temporary)
        .map_err(|error| io_error("create profile state", error))?;
    let result = (|| {
        file.write_all(&state_bytes)
            .map_err(|error| io_error("write profile state", error))?;
        file.sync_all()
            .map_err(|error| io_error("flush profile state", error))?;
        drop(file);
        publish_atomic(&temporary, &target)
    })();
    if result.is_err() {
        let _ = fs::remove_file(&temporary);
    }
    result
}

fn write_uninstalled_state(root: &Path, state: &UninstalledState) -> Result<(), SenpError> {
    let state_root = root.join("uninstalled");
    fs::create_dir_all(&state_root)
        .map_err(|error| io_error("create uninstalled state root", error))?;
    let state_bytes = serde_json::to_vec_pretty(state)
        .map_err(|error| SenpError::new(ErrorCode::PublicationFailed, error.to_string()))?;
    let target = state_root.join(format!("{}.json", state.id));
    let temporary = state_root.join(format!(
        ".{}.{}.{}.tmp",
        state.id,
        std::process::id(),
        temporary_suffix()
    ));
    let mut file = OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(&temporary)
        .map_err(|error| io_error("create uninstalled state", error))?;
    let result = (|| {
        file.write_all(&state_bytes)
            .map_err(|error| io_error("write uninstalled state", error))?;
        file.sync_all()
            .map_err(|error| io_error("flush uninstalled state", error))?;
        drop(file);
        publish_atomic(&temporary, &target)
    })();
    if result.is_err() {
        let _ = fs::remove_file(&temporary);
    }
    result
}

fn read_uninstalled_states(root: &Path) -> Result<BTreeMap<String, UninstalledState>, SenpError> {
    let state_root = root.join("uninstalled");
    if !state_root.exists() {
        return Ok(BTreeMap::new());
    }
    reject_reparse_points(&state_root)?;
    let mut entries = fs::read_dir(&state_root)
        .map_err(|error| io_error("read uninstalled state root", error))?
        .collect::<Result<Vec<_>, _>>()
        .map_err(|error| io_error("enumerate uninstalled state root", error))?;
    entries.sort_by_key(|entry| entry.file_name());
    if entries.len() > MAX_ENTRIES {
        return Err(SenpError::new(
            ErrorCode::TooManyEntries,
            entries.len().to_string(),
        ));
    }
    let mut states = BTreeMap::new();
    for entry in entries {
        let file_type = entry
            .file_type()
            .map_err(|error| io_error("inspect uninstalled state", error))?;
        if file_type.is_symlink() {
            return Err(SenpError::new(
                ErrorCode::ReparsePoint,
                entry.path().display().to_string(),
            ));
        }
        if !file_type.is_file()
            || entry.path().extension().and_then(|value| value.to_str()) != Some("json")
            || entry.file_name().to_string_lossy().starts_with('.')
        {
            continue;
        }
        let bytes =
            fs::read(entry.path()).map_err(|error| io_error("read uninstalled state", error))?;
        if bytes.len() as u64 > MAX_DOCUMENT_BYTES {
            return Err(SenpError::new(
                ErrorCode::EntryTooLarge,
                entry.path().display().to_string(),
            ));
        }
        let state: UninstalledState = strict_json(&bytes)?;
        let entry_path = entry.path();
        let filename = entry_path.file_stem().and_then(|value| value.to_str());
        if state.schema_version != 1
            || !valid_package_id(&state.id)
            || filename != Some(state.id.as_str())
            || !valid_sha256(&state.archive_sha256)
            || state.trust != "builtin"
        {
            return Err(SenpError::new(
                ErrorCode::InvalidManifest,
                "invalid uninstalled extension state",
            ));
        }
        if states.insert(state.id.clone(), state).is_some() {
            return Err(SenpError::new(
                ErrorCode::InvalidManifest,
                "duplicate uninstalled extension state",
            ));
        }
    }
    Ok(states)
}

fn previous_enabled_state(root: &Path, id: &str, trust: &str) -> Result<Option<bool>, SenpError> {
    let path = root.join("profiles").join(format!("{id}.json"));
    let bytes = match fs::read(&path) {
        Ok(bytes) => bytes,
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => return Ok(None),
        Err(error) => return Err(io_error("read previous profile state", error)),
    };
    if bytes.len() as u64 > MAX_DOCUMENT_BYTES {
        return Err(SenpError::new(
            ErrorCode::EntryTooLarge,
            path.display().to_string(),
        ));
    }
    let state: ProfileState = strict_json(&bytes)?;
    if state.schema_version != 1 || state.id != id || !valid_package_id(&state.id) {
        return Err(SenpError::new(
            ErrorCode::InvalidManifest,
            "previous profile state does not match the extension",
        ));
    }
    Ok((state.trust == trust).then_some(state.enabled))
}

#[derive(Debug)]
struct InstalledContentSnapshot {
    entries: BTreeMap<String, Vec<u8>>,
    manifest: Manifest,
    signed: bool,
    module_sha256: Option<String>,
    compatible: bool,
}

fn metadata_is_reparse_point(metadata: &fs::Metadata) -> bool {
    if metadata.file_type().is_symlink() {
        return true;
    }
    #[cfg(windows)]
    {
        use std::os::windows::fs::MetadataExt;
        if metadata.file_attributes() & 0x400 != 0 {
            return true;
        }
    }
    false
}

fn installed_entry_name(path: &Path) -> Result<String, SenpError> {
    path.components()
        .map(|component| match component {
            Component::Normal(value) => value
                .to_str()
                .map(str::to_owned)
                .ok_or_else(|| SenpError::new(ErrorCode::InvalidUtf8, path.display().to_string())),
            _ => Err(SenpError::new(
                ErrorCode::UnsafePath,
                path.display().to_string(),
            )),
        })
        .collect::<Result<Vec<_>, _>>()
        .map(|components| components.join("/"))
        .and_then(|value| normalized_entry_name(value.as_bytes()).map(|_| value))
}

fn read_installed_file(path: &Path, logical_path: &str) -> Result<Vec<u8>, SenpError> {
    let link_metadata =
        fs::symlink_metadata(path).map_err(|error| io_error(logical_path, error))?;
    if metadata_is_reparse_point(&link_metadata) {
        return Err(SenpError::new(
            ErrorCode::ReparsePoint,
            path.display().to_string(),
        ));
    }
    if !link_metadata.is_file() {
        return Err(SenpError::new(ErrorCode::UnsupportedEntry, logical_path));
    }
    let mut file = File::open(path).map_err(|error| io_error(logical_path, error))?;
    let metadata = file
        .metadata()
        .map_err(|error| io_error(logical_path, error))?;
    if metadata_is_reparse_point(&metadata) {
        return Err(SenpError::new(
            ErrorCode::ReparsePoint,
            path.display().to_string(),
        ));
    }
    if !metadata.is_file() {
        return Err(SenpError::new(ErrorCode::UnsupportedEntry, logical_path));
    }
    if metadata.len() > MAX_ENTRY_BYTES
        || (is_document(logical_path) && metadata.len() > MAX_DOCUMENT_BYTES)
    {
        return Err(SenpError::new(ErrorCode::EntryTooLarge, logical_path));
    }
    let expected_len = metadata.len();
    let mut bytes = Vec::with_capacity(expected_len as usize);
    Read::by_ref(&mut file)
        .take(MAX_ENTRY_BYTES + 1)
        .read_to_end(&mut bytes)
        .map_err(|error| io_error(logical_path, error))?;
    if bytes.len() as u64 != expected_len {
        return Err(SenpError::new(
            ErrorCode::ChecksumMismatch,
            format!("installed entry changed while reading: {logical_path}"),
        ));
    }
    Ok(bytes)
}

fn collect_installed_files(
    directory: &Path,
    relative: &Path,
    files: &mut BTreeMap<String, Vec<u8>>,
    directories: &mut BTreeSet<String>,
    expanded: &mut u64,
    depth: usize,
) -> Result<(), SenpError> {
    if depth > MAX_ENTRIES {
        return Err(SenpError::new(
            ErrorCode::UnsafePath,
            "installed content nesting exceeds entry bound",
        ));
    }
    let mut children = Vec::new();
    for child in
        fs::read_dir(directory).map_err(|error| io_error("read installed content", error))?
    {
        if children.len() >= MAX_ENTRIES {
            return Err(SenpError::new(
                ErrorCode::TooManyEntries,
                MAX_ENTRIES.to_string(),
            ));
        }
        children.push(child.map_err(|error| io_error("enumerate installed content", error))?);
    }
    children.sort_by_key(|entry| entry.file_name());
    for child in children {
        let metadata = fs::symlink_metadata(child.path())
            .map_err(|error| io_error("inspect installed content", error))?;
        if metadata_is_reparse_point(&metadata) {
            return Err(SenpError::new(
                ErrorCode::ReparsePoint,
                child.path().display().to_string(),
            ));
        }
        let name = child
            .file_name()
            .to_str()
            .map(str::to_owned)
            .ok_or_else(|| {
                SenpError::new(ErrorCode::InvalidUtf8, child.path().display().to_string())
            })?;
        let child_relative = relative.join(&name);
        let archive_path = installed_entry_name(&child_relative)?;
        if metadata.is_dir() {
            directories.insert(archive_path.clone());
            collect_installed_files(
                &child.path(),
                &child_relative,
                files,
                directories,
                expanded,
                depth + 1,
            )?;
            continue;
        }
        if !metadata.is_file() {
            return Err(SenpError::new(
                ErrorCode::UnsupportedEntry,
                child.path().display().to_string(),
            ));
        }
        if relative.as_os_str().is_empty() && name == ARCHIVE_MARKER_PATH {
            continue;
        }
        if !is_allowed_entry(&archive_path) {
            return Err(SenpError::new(ErrorCode::UnsupportedEntry, archive_path));
        }
        if files.len() >= MAX_ENTRIES {
            return Err(SenpError::new(
                ErrorCode::TooManyEntries,
                MAX_ENTRIES.to_string(),
            ));
        }
        let bytes = read_installed_file(&child.path(), &archive_path)?;
        *expanded = expanded
            .checked_add(bytes.len() as u64)
            .ok_or_else(|| SenpError::new(ErrorCode::ExpandedSizeLimit, archive_path.clone()))?;
        if *expanded > MAX_EXPANDED_BYTES {
            return Err(SenpError::new(
                ErrorCode::ExpandedSizeLimit,
                expanded.to_string(),
            ));
        }
        if files.insert(archive_path.clone(), bytes).is_some() {
            return Err(SenpError::new(ErrorCode::DuplicateEntry, archive_path));
        }
    }
    Ok(())
}

fn expected_installed_directories(files: &BTreeMap<String, Vec<u8>>) -> BTreeSet<String> {
    let mut directories = BTreeSet::new();
    for path in files.keys() {
        let components = path.split('/').collect::<Vec<_>>();
        for count in 1..components.len() {
            directories.insert(components[..count].join("/"));
        }
    }
    directories
}

fn read_installed_content(
    content: &Path,
    expected_archive_sha256: &str,
) -> Result<InstalledContentSnapshot, SenpError> {
    reject_reparse_points(content)?;
    let marker = read_installed_file(&content.join(ARCHIVE_MARKER_PATH), ARCHIVE_MARKER_PATH)?;
    if marker != format!("{expected_archive_sha256}\n").as_bytes() {
        return Err(SenpError::new(
            ErrorCode::ChecksumMismatch,
            "content marker mismatch",
        ));
    }

    let mut entries = BTreeMap::new();
    let mut directories = BTreeSet::new();
    let mut expanded = 0u64;
    collect_installed_files(
        content,
        Path::new(""),
        &mut entries,
        &mut directories,
        &mut expanded,
        0,
    )?;
    if directories != expected_installed_directories(&entries) {
        return Err(SenpError::new(
            ErrorCode::UnsupportedEntry,
            "installed directory layout does not match payload coverage",
        ));
    }
    for required in [MANIFEST_PATH, README_PATH, LICENSE_PATH, CHECKSUM_PATH] {
        if !entries.contains_key(required) {
            return Err(SenpError::new(ErrorCode::MissingRequiredEntry, required));
        }
    }
    for path in [MANIFEST_PATH, README_PATH, LICENSE_PATH] {
        std::str::from_utf8(&entries[path])
            .map_err(|_| SenpError::new(ErrorCode::InvalidUtf8, path))?;
    }
    let checksums = parse_checksums(&entries[CHECKSUM_PATH])?;
    let payload_paths: BTreeSet<_> = entries
        .keys()
        .filter(|path| path.as_str() != CHECKSUM_PATH && path.as_str() != SIGNATURE_PATH)
        .cloned()
        .collect();
    if payload_paths != checksums.keys().cloned().collect() {
        return Err(SenpError::new(
            ErrorCode::InvalidChecksumDocument,
            "checksum coverage mismatch",
        ));
    }
    for (path, expected) in &checksums {
        if hex(&Sha256::digest(&entries[path])) != *expected {
            return Err(SenpError::new(ErrorCode::ChecksumMismatch, path));
        }
    }
    let (manifest, compatible) =
        parse_manifest_as(&entries[MANIFEST_PATH], AbiAdmission::Installed)?;
    validate_manifest_entries(&manifest, &entries)?;
    let module_sha256 = manifest
        .runtime
        .as_ref()
        .map(|_| checksums[MODULE_PATH].clone());
    let signed = if let Some(signature) = entries.get(SIGNATURE_PATH) {
        parse_signature(signature)?;
        true
    } else {
        false
    };
    Ok(InstalledContentSnapshot {
        entries,
        manifest,
        signed,
        module_sha256,
        compatible,
    })
}

fn install_verified_archive(
    root: &Path,
    policy: &TrustPolicy,
    verified_archive: VerifiedArchive,
) -> Result<VerifiedPackage, SenpError> {
    let VerifiedArchive {
        package: verified,
        entries,
    } = verified_archive;
    reject_reparse_points(root)?;
    fs::create_dir_all(root).map_err(|error| io_error("create install root", error))?;
    reject_reparse_points(root)?;
    let _lock = acquire_root_lock_within(root, root_lock_wait())?;
    let content_root = root.join("content");
    let state_root = root.join("profiles");
    fs::create_dir_all(&content_root).map_err(|error| io_error("create content root", error))?;
    fs::create_dir_all(&state_root).map_err(|error| io_error("create state root", error))?;
    let destination = content_root.join(&verified.archive_sha256);
    reject_reparse_points(&destination)?;
    let intact = destination.exists()
        && read_installed_content(&destination, &verified.archive_sha256)
            .is_ok_and(|installed| installed.entries == entries);
    if !intact {
        let staging = content_root.join(format!(
            ".{}.{}.{}.tmp",
            verified.archive_sha256,
            std::process::id(),
            temporary_suffix()
        ));
        fs::create_dir(&staging).map_err(|error| io_error("create staging directory", error))?;
        let install_result = (|| {
            for (entry_path, bytes) in &entries {
                let output = staging.join(entry_path);
                if let Some(parent) = output.parent() {
                    fs::create_dir_all(parent)
                        .map_err(|error| io_error("create package directory", error))?;
                }
                let mut output_file = OpenOptions::new()
                    .write(true)
                    .create_new(true)
                    .open(&output)
                    .map_err(|error| io_error("create package entry", error))?;
                output_file
                    .write_all(bytes)
                    .map_err(|error| io_error("write package entry", error))?;
                output_file
                    .sync_all()
                    .map_err(|error| io_error("flush package entry", error))?;
            }
            fs::write(
                staging.join(ARCHIVE_MARKER_PATH),
                format!("{}\n", verified.archive_sha256),
            )
            .map_err(|error| io_error("write package marker", error))?;
            let staged = read_installed_content(&staging, &verified.archive_sha256)?;
            if staged.entries != entries {
                return Err(SenpError::new(
                    ErrorCode::ChecksumMismatch,
                    "staged content does not match the verified archive",
                ));
            }
            // The root lock means nobody else publishes this digest, so
            // whatever sits at `destination` failed verification above. Move
            // it aside first; a rename that cannot happen (files in use) is an
            // explicit error, never a silent success.
            let quarantine = if destination.exists() {
                let aside = content_root.join(format!(
                    ".{}.corrupt.{}",
                    verified.archive_sha256,
                    temporary_suffix()
                ));
                fs::rename(&destination, &aside).map_err(|error| {
                    SenpError::new(
                        ErrorCode::PublicationFailed,
                        format!("move corrupt content aside: {error}"),
                    )
                })?;
                Some(aside)
            } else {
                None
            };
            if let Err(error) = fs::rename(&staging, &destination) {
                if let Some(aside) = &quarantine {
                    // Put the previous state back rather than leave a hole.
                    let _ = fs::rename(aside, &destination);
                }
                return Err(SenpError::new(
                    ErrorCode::PublicationFailed,
                    error.to_string(),
                ));
            }
            if let Some(aside) = quarantine {
                // Best effort: the repaired content is already published.
                let _ = fs::remove_dir_all(&aside).or_else(|_| fs::remove_file(&aside));
            }
            Ok::<(), SenpError>(())
        })();
        if install_result.is_err() {
            let _ = fs::remove_dir_all(&staging);
            install_result?;
        }
    }
    let developer_unsigned = matches!(policy, TrustPolicy::DeveloperUnsigned) && !verified.signed;
    let trust = match policy {
        TrustPolicy::BuiltIn { .. } => "builtin",
        TrustPolicy::PublisherKeys(_) => "publisher",
        TrustPolicy::DeveloperUnsigned => "developer",
    };
    let enabled = if developer_unsigned {
        false
    } else {
        previous_enabled_state(root, &verified.manifest.id, trust)?.unwrap_or(true)
    };
    write_profile_state(
        root,
        &ProfileState {
            schema_version: 1,
            id: verified.manifest.id.clone(),
            archive_sha256: verified.archive_sha256.clone(),
            enabled,
            signed: verified.signed,
            trust: trust.to_owned(),
        },
    )?;
    let uninstalled_path = root
        .join("uninstalled")
        .join(format!("{}.json", verified.manifest.id));
    match fs::remove_file(&uninstalled_path) {
        Ok(()) => {}
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => {}
        Err(error) => return Err(io_error("clear uninstalled state", error)),
    }
    Ok(verified)
}

pub fn install_package(
    path: &Path,
    root: &Path,
    policy: &TrustPolicy,
) -> Result<VerifiedPackage, SenpError> {
    let snapshot = read_archive_snapshot(path)?;
    let verified = verify_archive_snapshot(&snapshot, policy)?;
    install_verified_archive(root, policy, verified)
}

/// One profile state entry that could not be listed. Sorting keeps the state
/// file name so an entry with no provable id still has a deterministic slot.
struct InvalidStateRow {
    sort_name: String,
    id: String,
    trust: String,
    code: ErrorCode,
    message: String,
}

fn bounded_invalid_message(detail: &str) -> String {
    if detail.len() <= INVALID_ROW_MESSAGE_BYTES {
        return detail.to_owned();
    }
    let mut end = INVALID_ROW_MESSAGE_BYTES;
    while !detail.is_char_boundary(end) {
        end -= 1;
    }
    detail[..end].to_owned()
}

/// Reads one `profiles/` entry. `Ok(None)` is a non-state file or a tombstoned
/// package; `Err` is a failure confined to this entry, which the caller lists
/// as an invalid row instead of failing the whole command.
fn list_state_entry(
    root: &Path,
    entry: &fs::DirEntry,
    uninstalled: &BTreeMap<String, UninstalledState>,
) -> Result<Option<InstalledExtension>, InvalidStateRow> {
    let sort_name = entry.file_name().to_string_lossy().into_owned();
    let invalid = |id: String, trust: String, error: SenpError| InvalidStateRow {
        sort_name: sort_name.clone(),
        id,
        trust,
        code: error.code,
        message: bounded_invalid_message(&error.detail),
    };
    let stem_id = || {
        entry
            .path()
            .file_stem()
            .and_then(|value| value.to_str())
            .filter(|value| valid_package_id(value))
            .unwrap_or("")
            .to_owned()
    };
    let file_type = entry.file_type().map_err(|error| {
        invalid(
            String::new(),
            String::new(),
            io_error("inspect state", error),
        )
    })?;
    if file_type.is_symlink() {
        return Err(invalid(
            stem_id(),
            String::new(),
            SenpError::new(ErrorCode::ReparsePoint, entry.path().display().to_string()),
        ));
    }
    if !file_type.is_file()
        || entry.path().extension().and_then(|value| value.to_str()) != Some("json")
        || entry.file_name().to_string_lossy().starts_with('.')
    {
        return Ok(None);
    }
    let state_bytes = fs::read(entry.path())
        .map_err(|error| invalid(String::new(), String::new(), io_error("read state", error)))?;
    if state_bytes.len() as u64 > MAX_DOCUMENT_BYTES {
        return Err(invalid(
            stem_id(),
            String::new(),
            SenpError::new(ErrorCode::EntryTooLarge, entry.path().display().to_string()),
        ));
    }
    let state: ProfileState =
        strict_json(&state_bytes).map_err(|error| invalid(String::new(), String::new(), error))?;
    if state.schema_version != 1
        || !valid_package_id(&state.id)
        || !valid_sha256(&state.archive_sha256)
        || !matches!(state.trust.as_str(), "builtin" | "publisher" | "developer")
    {
        let id = if valid_package_id(&state.id) {
            state.id.clone()
        } else {
            String::new()
        };
        return Err(invalid(
            id,
            state.trust.clone(),
            SenpError::new(
                ErrorCode::InvalidManifest,
                "invalid profile extension state",
            ),
        ));
    }
    if (state.trust == "publisher") != state.signed {
        return Err(invalid(
            state.id,
            state.trust,
            SenpError::new(
                ErrorCode::InvalidSignature,
                "profile trust and signature state differ",
            ),
        ));
    }
    if uninstalled
        .get(&state.id)
        .is_some_and(|removed| removed.trust == state.trust)
    {
        return Ok(None);
    }
    let content = root.join("content").join(&state.archive_sha256);
    let content_snapshot = read_installed_content(&content, &state.archive_sha256)
        .map_err(|error| invalid(state.id.clone(), state.trust.clone(), error))?;
    if content_snapshot.manifest.id != state.id {
        return Err(invalid(
            state.id,
            state.trust,
            SenpError::new(ErrorCode::InvalidManifest, "state and manifest ids differ"),
        ));
    }
    if content_snapshot.signed != state.signed {
        return Err(invalid(
            state.id,
            state.trust,
            SenpError::new(
                ErrorCode::InvalidSignature,
                "profile and installed signature state differ",
            ),
        ));
    }
    Ok(Some(InstalledExtension {
        module_path: content_snapshot
            .manifest
            .runtime
            .as_ref()
            .map(|_| content.join(MODULE_PATH)),
        module_sha256: content_snapshot.module_sha256,
        manifest: content_snapshot.manifest,
        archive_sha256: state.archive_sha256,
        enabled: state.enabled,
        signed: state.signed,
        trust: state.trust,
        extension_path: content,
        compatible: content_snapshot.compatible,
    }))
}

/// Lists every installed package in the profile. A failure confined to one
/// `profiles/` state entry yields an `Invalid` row; failures of the whole
/// install root (reparse point, unreadable `profiles/`, more than
/// `MAX_ENTRIES`, uninstalled-state read failure) still fail the command.
/// Rows sort by extension id; invalid rows without a provable id sort last,
/// by state file name.
pub fn list_installed(root: &Path) -> Result<Vec<InstalledListRow>, SenpError> {
    reject_reparse_points(root)?;
    let uninstalled = read_uninstalled_states(root)?;
    let state_root = root.join("profiles");
    // A reparse-point `profiles/` (dangling or not) is a root-level refusal,
    // like a reparse-point install root: the state directory's own boundary is
    // untrusted. Only a truly absent `profiles/` is an empty listing.
    match fs::symlink_metadata(&state_root) {
        Ok(metadata) if metadata_is_reparse_point(&metadata) => {
            return Err(SenpError::new(
                ErrorCode::ReparsePoint,
                state_root.display().to_string(),
            ));
        }
        Ok(_) => {}
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => return Ok(Vec::new()),
        Err(error) => return Err(io_error("inspect state root", error)),
    }
    let mut states = fs::read_dir(&state_root)
        .map_err(|error| io_error("read state root", error))?
        .collect::<Result<Vec<_>, _>>()
        .map_err(|error| io_error("enumerate state root", error))?;
    states.sort_by_key(|entry| entry.file_name());
    if states.len() > MAX_ENTRIES {
        return Err(SenpError::new(
            ErrorCode::TooManyEntries,
            states.len().to_string(),
        ));
    }
    let mut rows: Vec<(String, String, InstalledListRow)> = Vec::new();
    for entry in states {
        match list_state_entry(root, &entry, &uninstalled) {
            Ok(Some(extension)) => {
                let id = extension.manifest.id.clone();
                rows.push((
                    id,
                    String::new(),
                    InstalledListRow::Installed(Box::new(extension)),
                ));
            }
            Ok(None) => {}
            Err(invalid) => rows.push((
                invalid.id.clone(),
                invalid.sort_name,
                InstalledListRow::Invalid {
                    id: invalid.id,
                    trust: invalid.trust,
                    code: invalid.code,
                    message: invalid.message,
                },
            )),
        }
    }
    rows.sort_by(|left, right| {
        (left.0.is_empty(), left.0.as_str(), left.1.as_str()).cmp(&(
            right.0.is_empty(),
            right.0.as_str(),
            right.1.as_str(),
        ))
    });
    Ok(rows.into_iter().map(|(_, _, row)| row).collect())
}

fn json_node_count(value: &serde_json::Value) -> usize {
    match value {
        serde_json::Value::Array(items) => 1 + items.iter().map(json_node_count).sum::<usize>(),
        serde_json::Value::Object(members) => {
            1 + members.values().map(json_node_count).sum::<usize>()
        }
        _ => 1,
    }
}

fn serialize_listing_line(
    row: &InstalledListRow,
) -> Result<(serde_json::Value, String), SenpError> {
    let value = serde_json::to_value(row)
        .map_err(|error| SenpError::new(ErrorCode::PublicationFailed, error.to_string()))?;
    let line = serde_json::to_string(&value)
        .map_err(|error| SenpError::new(ErrorCode::PublicationFailed, error.to_string()))?;
    Ok((value, line))
}

fn serialize_installed_listing_bounded(
    rows: &[InstalledListRow],
    line_budget: usize,
    output_budget: usize,
) -> Result<String, SenpError> {
    let mut output = String::new();
    for row in rows {
        let (mut value, mut line) = serialize_listing_line(row)?;
        if line.len() > line_budget || json_node_count(&value) > LIST_LINE_MAX_NODES {
            // An over-budget installed row cannot be trusted by the decoder;
            // emit the typed invalid row in its place.
            let (id, trust) = match row {
                InstalledListRow::Installed(extension) => {
                    (extension.manifest.id.clone(), extension.trust.clone())
                }
                InstalledListRow::Invalid { id, trust, .. } => (id.clone(), trust.clone()),
            };
            let replacement = InstalledListRow::Invalid {
                id,
                trust,
                code: ErrorCode::OutputTooLarge,
                message: "listing row exceeded the per-package output budget".to_owned(),
            };
            let pair = serialize_listing_line(&replacement)?;
            value = pair.0;
            line = pair.1;
            if line.len() > line_budget || json_node_count(&value) > LIST_LINE_MAX_NODES {
                return Err(SenpError::new(
                    ErrorCode::OutputTooLarge,
                    "invalid listing row exceeded the per-package output budget",
                ));
            }
        }
        if output.len() + line.len() + 1 > output_budget {
            // No partial listing is trusted: stop and fail with a typed code.
            return Err(SenpError::new(
                ErrorCode::OutputTooLarge,
                "installed listing exceeded the tool output budget",
            ));
        }
        output.push_str(&line);
        output.push('\n');
    }
    Ok(output)
}

/// Serializes `list` output as JSON Lines: one complete JSON object per listed
/// package, each within `LIST_LINE_MAX_BYTES`/`LIST_LINE_MAX_NODES`, the whole
/// document within `LIST_OUTPUT_MAX_BYTES`. An over-budget package line is
/// replaced by a `kind:"invalid"` row; exceeding the total budget fails the
/// command with `OutputTooLarge` before anything is written.
pub fn serialize_installed_listing(rows: &[InstalledListRow]) -> Result<String, SenpError> {
    serialize_installed_listing_bounded(rows, LIST_LINE_MAX_BYTES, LIST_OUTPUT_MAX_BYTES)
}

pub fn list_uninstalled(root: &Path) -> Result<Vec<String>, SenpError> {
    reject_reparse_points(root)?;
    Ok(read_uninstalled_states(root)?.into_keys().collect())
}

pub fn uninstall_built_in(
    root: &Path,
    id: &str,
    expected_archive_sha256: &str,
) -> Result<(), SenpError> {
    if !valid_package_id(id) || !valid_sha256(expected_archive_sha256) {
        return Err(SenpError::new(
            ErrorCode::InvalidManifest,
            "invalid built-in uninstall request",
        ));
    }
    reject_reparse_points(root)?;
    let _lock = lock_existing_root(root)?;
    let state_path = root.join("profiles").join(format!("{id}.json"));
    reject_reparse_points(&state_path)?;
    let bytes = match fs::read(&state_path) {
        Ok(bytes) => bytes,
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => {
            let uninstalled = read_uninstalled_states(root)?;
            return if uninstalled.contains_key(id) {
                Ok(())
            } else {
                Err(io_error("read profile state", error))
            };
        }
        Err(error) => return Err(io_error("read profile state", error)),
    };
    if bytes.len() as u64 > MAX_DOCUMENT_BYTES {
        return Err(SenpError::new(
            ErrorCode::EntryTooLarge,
            state_path.display().to_string(),
        ));
    }
    let state: ProfileState = strict_json(&bytes)?;
    if state.schema_version != 1
        || state.id != id
        || state.trust != "builtin"
        || state.archive_sha256 != expected_archive_sha256
    {
        return Err(SenpError::new(
            ErrorCode::InvalidManifest,
            "installed state does not match the built-in uninstall request",
        ));
    }
    write_uninstalled_state(
        root,
        &UninstalledState {
            schema_version: 1,
            id: id.to_owned(),
            archive_sha256: expected_archive_sha256.to_owned(),
            trust: "builtin".to_owned(),
        },
    )?;
    match fs::remove_file(&state_path) {
        Ok(()) => Ok(()),
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => Ok(()),
        // The published tombstone is authoritative even if stale profile
        // cleanup is delayed. list_installed continues to fail closed.
        Err(_) => Ok(()),
    }
}

pub fn set_extension_enabled(root: &Path, id: &str, enabled: bool) -> Result<(), SenpError> {
    if !valid_package_id(id) {
        return Err(SenpError::new(
            ErrorCode::InvalidManifest,
            "invalid extension id",
        ));
    }
    let _lock = lock_existing_root(root)?;
    let state_path = root.join("profiles").join(format!("{id}.json"));
    let bytes = fs::read(&state_path).map_err(|error| io_error("read profile state", error))?;
    let mut state: ProfileState = strict_json(&bytes)?;
    if state.id != id || state.schema_version != 1 {
        return Err(SenpError::new(
            ErrorCode::InvalidManifest,
            "profile state id mismatch",
        ));
    }
    if read_uninstalled_states(root)?.contains_key(id) {
        return Err(SenpError::new(
            ErrorCode::InvalidManifest,
            "uninstalled extension cannot be enabled",
        ));
    }
    state.enabled = enabled;
    write_profile_state(root, &state)
}

#[cfg(test)]
mod tests {
    use super::*;
    use tempfile::TempDir;

    /// The `kind:"installed"` rows of a listing, in order.
    fn installed_rows(rows: &[InstalledListRow]) -> Vec<&InstalledExtension> {
        rows.iter()
            .filter_map(|row| match row {
                InstalledListRow::Installed(extension) => Some(extension.as_ref()),
                InstalledListRow::Invalid { .. } => None,
            })
            .collect()
    }

    /// One `kind:"invalid"` row's `(id, code)`, asserting the shape exists.
    fn invalid(rows: &[InstalledListRow]) -> Vec<(&str, ErrorCode)> {
        rows.iter()
            .filter_map(|row| match row {
                InstalledListRow::Invalid { id, code, .. } => Some((id.as_str(), *code)),
                InstalledListRow::Installed(_) => None,
            })
            .collect()
    }

    fn fixture(root: &Path) {
        fs::create_dir_all(root.join("module")).unwrap();
        fs::create_dir_all(root.join("assets/icons")).unwrap();
        fs::write(
            root.join(MANIFEST_PATH),
            br#"{
  "schemaVersion": 1,
  "id": "sample-indent",
  "displayName": "Sample Indent",
  "version": "0.1.0",
  "publisher": "sakura.builtin",
  "description": "Fixture",
  "engines": { "sakura": ">=0.0.0" },
  "runtime": { "abi": "sakura:senp/extension@1.0.0", "module": "module/extension.wasm" },
  "activationEvents": ["onStartupFinished"],
  "capabilities": ["editor.visibleText", "editor.decorations"],
  "contributes": { "editorDecorations": [{ "id": "sample.indent", "kind": "indent" }] }
}"#,
        )
        .unwrap();
        fs::write(root.join(README_PATH), "# Sample\n").unwrap();
        fs::write(root.join(LICENSE_PATH), "Zlib\n").unwrap();
        fs::write(root.join(MODULE_PATH), b"wasm").unwrap();
        fs::write(root.join("assets/icons/sample.txt"), b"asset").unwrap();
    }

    fn grammar_fixture(root: &Path) {
        fs::create_dir_all(root.join("assets/syntaxes")).unwrap();
        fs::write(
            root.join(MANIFEST_PATH),
            br#"{
  "schemaVersion": 1,
  "id": "sample-shell-language-basics",
  "displayName": "Shell Language Basics",
  "version": "0.1.0",
  "publisher": "sakura.builtin",
  "description": "Fixture",
  "engines": { "sakura": ">=0.0.0" },
  "contributes": {
    "languages": [{
      "id": "shellscript",
      "aliases": ["Shell Script", "shellscript"],
      "extensions": [".sh", ".bash", ".zsh"],
      "filenames": [".bashrc", ".zshrc"],
      "firstLine": "^#!.*\\b(?:ba|z)?sh\\b"
    }],
    "grammars": [{
      "language": "shellscript",
      "scopeName": "source.shell",
      "path": "assets/syntaxes/shell-unix-bash.tmLanguage.json"
    }]
  }
}"#,
        )
        .unwrap();
        fs::write(root.join(README_PATH), "# Shell Language Basics\n").unwrap();
        fs::write(root.join(LICENSE_PATH), "MIT\n").unwrap();
        fs::write(
            root.join("assets/syntaxes/shell-unix-bash.tmLanguage.json"),
            br#"{"scopeName":"source.shell","patterns":[]}"#,
        )
        .unwrap();
    }

    fn workbench_view_fixture(root: &Path) {
        fs::create_dir_all(root).unwrap();
        fs::write(
            root.join(MANIFEST_PATH),
            br#"{
  "schemaVersion": 1,
  "id": "sample-projects",
  "displayName": "Projects",
  "version": "0.1.0",
  "publisher": "sakura.builtin",
  "description": "Fixture",
  "engines": { "sakura": ">=0.0.0" },
  "contributes": {
    "viewsContainers": {
      "activitybar": [{
        "id": "sakura.view.projects",
        "title": "Projects",
        "icon": "$(project)",
        "order": 45
      }]
    },
    "views": {
      "sakura.view.projects": [{
        "id": "sakura.projects",
        "name": "Projects",
        "provider": "sakura.projects",
        "order": 10
      }]
    }
  }
}"#,
        )
        .unwrap();
        fs::write(root.join(README_PATH), "# Projects\n").unwrap();
        fs::write(root.join(LICENSE_PATH), "Zlib\n").unwrap();
    }

    #[test]
    fn deterministic_package_round_trips_and_installs_immutably() {
        let temp = TempDir::new().unwrap();
        let source = temp.path().join("source");
        fixture(&source);
        let first = temp.path().join("first.senp");
        let second = temp.path().join("second.senp");
        let first_hash = pack_directory(&source, &first, None).unwrap();
        let second_hash = pack_directory(&source, &second, None).unwrap();
        assert_eq!(first_hash, second_hash);
        assert_eq!(fs::read(&first).unwrap(), fs::read(&second).unwrap());
        let verified = verify_package(
            &first,
            &TrustPolicy::BuiltIn {
                expected_archive_sha256: first_hash.clone(),
            },
        )
        .unwrap();
        assert_eq!(verified.manifest.id, "sample-indent");
        assert!(!verified.signed);
        let installed = install_package(
            &first,
            &temp.path().join("installed"),
            &TrustPolicy::BuiltIn {
                expected_archive_sha256: first_hash,
            },
        )
        .unwrap();
        assert!(temp
            .path()
            .join("installed/content")
            .join(&installed.archive_sha256)
            .join(MODULE_PATH)
            .is_file());
        let install_root = temp.path().join("installed");
        let listed = list_installed(&install_root).unwrap();
        let rows = installed_rows(&listed);
        assert_eq!(rows.len(), 1);
        assert_eq!(rows[0].manifest.id, "sample-indent");
        assert!(serde_json::to_value(rows[0])
            .unwrap()
            .get("readme")
            .is_none());
        assert_eq!(rows[0].trust, "builtin");
        assert!(rows[0].enabled);
        assert_eq!(rows[0].module_sha256, Some(hex(&Sha256::digest(b"wasm"))));
        assert_eq!(
            serde_json::to_value(&listed[0]).unwrap()["moduleSha256"],
            hex(&Sha256::digest(b"wasm"))
        );
        assert_eq!(
            serde_json::to_value(&listed[0]).unwrap()["kind"],
            "installed"
        );
        assert!(rows[0]
            .module_path
            .as_ref()
            .unwrap()
            .parent()
            .unwrap()
            .parent()
            .unwrap()
            .join("assets/icons/sample.txt")
            .is_file());
        set_extension_enabled(&install_root, "sample-indent", false).unwrap();
        assert!(!installed_rows(&list_installed(&install_root).unwrap())[0].enabled);
        install_package(
            &first,
            &install_root,
            &TrustPolicy::BuiltIn {
                expected_archive_sha256: installed.archive_sha256.clone(),
            },
        )
        .unwrap();
        assert!(!installed_rows(&list_installed(&install_root).unwrap())[0].enabled);
    }

    #[test]
    fn built_in_uninstall_is_persistent_and_reinstallable() {
        let temp = TempDir::new().unwrap();
        let source = temp.path().join("source");
        fixture(&source);
        let package = temp.path().join("sample.senp");
        let hash = pack_directory(&source, &package, None).unwrap();
        let install_root = temp.path().join("installed");
        install_package(
            &package,
            &install_root,
            &TrustPolicy::BuiltIn {
                expected_archive_sha256: hash.clone(),
            },
        )
        .unwrap();

        uninstall_built_in(&install_root, "sample-indent", &hash).unwrap();
        assert!(list_installed(&install_root).unwrap().is_empty());
        assert_eq!(list_uninstalled(&install_root).unwrap(), ["sample-indent"]);
        assert!(install_root.join("content").join(&hash).is_dir());
        uninstall_built_in(&install_root, "sample-indent", &hash).unwrap();

        // A tombstone wins if cleanup of the old active state is delayed.
        write_profile_state(
            &install_root,
            &ProfileState {
                schema_version: 1,
                id: "sample-indent".to_owned(),
                archive_sha256: hash.clone(),
                enabled: true,
                signed: false,
                trust: "builtin".to_owned(),
            },
        )
        .unwrap();
        assert!(list_installed(&install_root).unwrap().is_empty());

        install_package(
            &package,
            &install_root,
            &TrustPolicy::BuiltIn {
                expected_archive_sha256: hash,
            },
        )
        .unwrap();
        assert_eq!(list_installed(&install_root).unwrap().len(), 1);
        assert!(list_uninstalled(&install_root).unwrap().is_empty());
    }

    #[test]
    fn built_in_uninstall_rejects_a_mismatched_archive() {
        let temp = TempDir::new().unwrap();
        let source = temp.path().join("source");
        fixture(&source);
        let package = temp.path().join("sample.senp");
        let hash = pack_directory(&source, &package, None).unwrap();
        let install_root = temp.path().join("installed");
        install_package(
            &package,
            &install_root,
            &TrustPolicy::BuiltIn {
                expected_archive_sha256: hash,
            },
        )
        .unwrap();

        let error =
            uninstall_built_in(&install_root, "sample-indent", &"0".repeat(64)).unwrap_err();
        assert_eq!(error.code, ErrorCode::InvalidManifest);
        assert_eq!(list_installed(&install_root).unwrap().len(), 1);
        assert!(list_uninstalled(&install_root).unwrap().is_empty());
    }

    #[test]
    fn declarative_grammar_package_has_no_runtime_module() {
        let temp = TempDir::new().unwrap();
        let source = temp.path().join("source");
        grammar_fixture(&source);
        let package = temp.path().join("shell.senp");
        let hash = pack_directory(&source, &package, None).unwrap();
        let verified = verify_package(
            &package,
            &TrustPolicy::BuiltIn {
                expected_archive_sha256: hash.clone(),
            },
        )
        .unwrap();
        assert!(verified.manifest.runtime.is_none());
        assert_eq!(verified.manifest.contributes.languages[0].id, "shellscript");

        let install_root = temp.path().join("installed");
        install_package(
            &package,
            &install_root,
            &TrustPolicy::BuiltIn {
                expected_archive_sha256: hash,
            },
        )
        .unwrap();
        let listed = list_installed(&install_root).unwrap();
        let rows = installed_rows(&listed);
        assert_eq!(rows.len(), 1);
        assert!(rows[0].module_path.is_none());
        assert_eq!(rows[0].module_sha256, None);
        assert!(serde_json::to_value(&listed[0]).unwrap()["moduleSha256"].is_null());
        assert!(rows[0]
            .extension_path
            .join("assets/syntaxes/shell-unix-bash.tmLanguage.json")
            .is_file());
    }

    #[test]
    fn declarative_grammar_package_rejects_missing_declared_asset() {
        let temp = TempDir::new().unwrap();
        let source = temp.path().join("source");
        grammar_fixture(&source);
        fs::remove_file(source.join("assets/syntaxes/shell-unix-bash.tmLanguage.json")).unwrap();
        let error = pack_directory(&source, &temp.path().join("shell.senp"), None).unwrap_err();
        assert_eq!(error.code, ErrorCode::MissingRequiredEntry);
    }

    #[test]
    fn declarative_workbench_view_package_round_trips_without_runtime_authority() {
        let temp = TempDir::new().unwrap();
        let source = temp.path().join("source");
        workbench_view_fixture(&source);
        let package = temp.path().join("projects.senp");
        let hash = pack_directory(&source, &package, None).unwrap();
        let verified = verify_package(
            &package,
            &TrustPolicy::BuiltIn {
                expected_archive_sha256: hash,
            },
        )
        .unwrap();

        assert!(verified.manifest.runtime.is_none());
        assert!(verified.manifest.capabilities.is_empty());
        assert_eq!(
            verified.manifest.contributes.views_containers.activitybar[0].id,
            "sakura.view.projects"
        );
        assert_eq!(
            verified.manifest.contributes.views["sakura.view.projects"][0].provider,
            "sakura.projects"
        );
    }

    #[test]
    fn duplicate_manifest_member_is_rejected() {
        let value = br#"{"schemaVersion":1,"schemaVersion":1}"#;
        let result = strict_json::<Manifest>(value).unwrap_err();
        assert_eq!(result.code, ErrorCode::InvalidManifest);
        assert!(result.detail.contains("duplicate member"));
    }

    fn versioned_manifest(schema: u32, abi: &str) -> Vec<u8> {
        serde_json::to_vec(&serde_json::json!({
            "schemaVersion": schema,
            "id": "sample-github", "publisher": "sakura.builtin",
            "displayName": "GitHub", "version": "0.1.0", "description": "Fixture",
            "engines": { "sakura": ">=0.0.0" },
            "runtime": { "abi": abi, "module": MODULE_PATH },
            "activationEvents": ["onView:pr:github"],
            "capabilities": ["workbench.views.tree"],
            "contributes": {
                "viewsContainers": { "activitybar": [{
                    "id": "github-pull-requests", "title": "GitHub", "icon": "$(github)", "order": 6
                }] },
                "views": { "github-pull-requests": [{
                    "id": "pr:github", "name": "Pull Requests", "provider": "senp.tree", "order": 10
                }] }
            }
        }))
        .unwrap()
    }

    #[test]
    fn container_icons_accept_theme_icons_and_bounded_package_image_paths() {
        let with_icon = |icon: &str| {
            let mut value: serde_json::Value =
                serde_json::from_slice(&versioned_manifest(2, ABI_V2)).unwrap();
            value["contributes"]["viewsContainers"]["activitybar"][0]["icon"] = icon.into();
            parse_manifest(&serde_json::to_vec(&value).unwrap())
        };
        for icon in [
            "$(github)",
            "resources/icons/light/explorer.svg",
            "media/icon.png",
        ] {
            assert!(with_icon(icon).is_ok(), "{icon}");
        }
        for icon in [
            "",
            "$()",
            "$(a b)",
            "/resources/icon.svg",
            "./icon.svg",
            "../icon.svg",
            "resources/../icon.svg",
            "resources//icon.svg",
            "resources\\icon.svg",
            "resources/icon.svgz",
            "C:/icon.svg",
            "https://example.com/icon.svg",
        ] {
            assert_eq!(
                with_icon(icon).unwrap_err().code,
                ErrorCode::InvalidManifest,
                "{icon}"
            );
        }
    }

    #[test]
    fn manifest_version_dispatch_preserves_v1_and_admits_v2_only_with_its_own_runtime() {
        let temp = TempDir::new().unwrap();
        fixture(temp.path());
        let bytes = fs::read(temp.path().join(MANIFEST_PATH)).unwrap();
        assert_eq!(parse_manifest(&bytes).unwrap().runtime.unwrap().abi, ABI);
        let crossed = String::from_utf8(bytes).unwrap().replace(ABI, ABI_V2);
        assert_eq!(
            parse_manifest(crossed.as_bytes()).unwrap_err().code,
            ErrorCode::AbiMismatch
        );
        assert_eq!(
            parse_manifest(&versioned_manifest(2, ABI_V2))
                .unwrap()
                .runtime
                .unwrap()
                .abi,
            ABI_V2
        );
        // A v2 manifest without a runtime is not a declarative extension that
        // happens to carry a newer version number: its View and command
        // contributions exist only for the effect host to execute.
        let mut bare: serde_json::Value =
            serde_json::from_slice(&versioned_manifest(2, ABI_V2)).unwrap();
        bare.as_object_mut().unwrap().remove("runtime");
        assert_eq!(
            parse_manifest(&serde_json::to_vec(&bare).unwrap())
                .unwrap_err()
                .code,
            ErrorCode::InvalidManifest
        );
        for abi in [
            ABI,
            "sakura:senp/extension@2.0.0",
            "sakura:senp/extension@4.0.0",
        ] {
            assert_eq!(
                parse_manifest(&versioned_manifest(2, abi))
                    .unwrap_err()
                    .code,
                ErrorCode::AbiMismatch
            );
        }
        for schema in [0, 3, u32::MAX] {
            assert_eq!(
                parse_manifest(&versioned_manifest(schema, ABI_V2))
                    .unwrap_err()
                    .code,
                ErrorCode::UnsupportedSchema
            );
        }
        assert_eq!(
            parse_manifest(&versioned_manifest(1, ABI))
                .unwrap_err()
                .code,
            ErrorCode::InvalidManifest
        );
    }

    #[test]
    fn real_effect_packages_parse_with_the_commands_they_declare() {
        for source in [
            include_bytes!("../../extensions/sakura_senp_sample/senp.json").as_slice(),
            include_bytes!("../../extensions/sakura_github_pull_requests/senp.json").as_slice(),
            include_bytes!("../../extensions/sakura_github_actions/senp.json").as_slice(),
        ] {
            let manifest = parse_manifest(source).unwrap();
            assert_eq!(manifest.schema_version, 2);
            assert_eq!(manifest.runtime.as_ref().unwrap().abi, ABI_V2);
            // Every command a tree item may carry has to be declared here: the
            // View registers exactly this list, and an item naming anything else
            // rejects the page that carries it.
            assert!(!manifest.contributes.commands.is_empty());
        }
    }

    #[test]
    fn effect_manifest_rejects_undeclared_activation_commands_and_capabilities() {
        let base: serde_json::Value = serde_json::from_slice(include_bytes!(
            "../../extensions/sakura_senp_sample/senp.json"
        ))
        .unwrap();
        let cases = [
            ("/activationEvents", serde_json::json!(["onView:foreign"])),
            (
                "/activationEvents",
                serde_json::json!(["onStartupFinished"]),
            ),
            ("/activationEvents", serde_json::json!([])),
            (
                "/activationEvents",
                serde_json::json!(["onView:sample.projects", "onView:sample.projects"]),
            ),
            (
                "/capabilities",
                serde_json::json!(["workbench.views.tree", "unknown"]),
            ),
            ("/capabilities", serde_json::json!(["workbench.views.tree"])),
            (
                "/capabilities",
                serde_json::json!([
                    "workbench.views.tree",
                    "workbench.commands",
                    "tools.github.repository.read"
                ]),
            ),
            (
                "/contributes/commands",
                serde_json::json!([{"command":"bad id", "title":"Open"}]),
            ),
            (
                "/contributes/commands",
                serde_json::json!([{"command":"sample.open", "title":" "}]),
            ),
            (
                "/contributes/commands",
                serde_json::json!([{"command":"sample.open", "title":"Line\nbreak"}]),
            ),
            (
                "/contributes/commands",
                serde_json::json!([{"command":"sample.open", "title":"Open"}, {"command":"sample.open", "title":"Duplicate"}]),
            ),
            (
                "/contributes/commands",
                serde_json::json!([{"command":"sample.open", "title":"Open", "script":"run"}]),
            ),
            (
                "/contributes/views/sample.senp/0/provider",
                serde_json::json!("legacy.provider"),
            ),
            ("/runtime", serde_json::Value::Null),
        ];
        for (pointer, replacement) in cases {
            let mut value = base.clone();
            *value.pointer_mut(pointer).unwrap() = replacement;
            let bytes = serde_json::to_vec(&value).unwrap();
            assert_eq!(
                parse_manifest(&bytes).unwrap_err().code,
                ErrorCode::InvalidManifest,
                "{pointer}: {value}"
            );
        }
        let mut manifest: Manifest = serde_json::from_value(base).unwrap();
        manifest.contributes.commands = (0..129)
            .map(|index| CommandContribution {
                command: format!("sample.open{index}"),
                title: "Open".into(),
                icon: None,
            })
            .collect();
        assert_eq!(
            validate_manifest(&manifest).unwrap_err().code,
            ErrorCode::InvalidManifest
        );
    }

    fn with_view_title(
        commands: serde_json::Value,
        view_title: serde_json::Value,
    ) -> Result<Manifest, SenpError> {
        let mut value: serde_json::Value = serde_json::from_slice(include_bytes!(
            "../../extensions/sakura_senp_sample/senp.json"
        ))
        .unwrap();
        value["contributes"]["commands"] = commands;
        value["contributes"]["menus"] = serde_json::json!({ "view/title": view_title });
        parse_manifest(&serde_json::to_vec(&value).unwrap())
    }

    #[test]
    fn view_title_menus_accept_icon_commands_placed_by_view_equality() {
        let commands = serde_json::json!([
            {"command":"sample.refresh", "title":"Refresh", "icon":"$(refresh)"},
            {"command":"sample.collapse", "title":"Collapse", "icon":"$(collapse-all)"}
        ]);
        let manifest = with_view_title(
            commands,
            serde_json::json!([
                {"command":"sample.refresh", "when":"view == sample.projects || view==sample.states", "group":"navigation"},
                {"command":"sample.collapse", "when":"  view ==  sample.states ", "group":"navigation"}
            ]),
        )
        .unwrap();
        let items = &manifest.contributes.menus.view_title;
        assert_eq!(items.len(), 2);
        assert_eq!(
            view_title_when_views(&items[0].when).unwrap(),
            ["sample.projects", "sample.states"]
        );
        assert_eq!(
            manifest.contributes.commands[0].icon.as_deref(),
            Some("$(refresh)")
        );
        // A manifest without menus still serializes without the key, so the
        // C++ decoder of a package installed before menus existed is unchanged.
        let plain = parse_manifest(include_bytes!(
            "../../extensions/sakura_senp_sample/senp.json"
        ))
        .unwrap();
        let serialized = serde_json::to_value(&plain).unwrap();
        assert!(serialized["contributes"].get("menus").is_none());
        assert!(serialized["contributes"]["commands"][0]
            .get("icon")
            .is_none());
    }

    #[test]
    fn view_title_menus_reject_what_a_title_button_cannot_show() {
        let icon = serde_json::json!([
            {"command":"sample.refresh", "title":"Refresh", "icon":"$(refresh)"},
            {"command":"sample.plain", "title":"Plain"}
        ]);
        let item = |command: &str, when: &str, group: &str| serde_json::json!({"command": command, "when": when, "group": group});
        let cases = [
            // Undeclared command, a command without an icon, overflow group.
            serde_json::json!([item(
                "sample.other",
                "view == sample.projects",
                "navigation"
            )]),
            serde_json::json!([item(
                "sample.plain",
                "view == sample.projects",
                "navigation"
            )]),
            serde_json::json!([item("sample.refresh", "view == sample.projects", "inline")]),
            serde_json::json!([item(
                "sample.refresh",
                "view == sample.projects",
                "navigation@1"
            )]),
            // Anything but a `view == <declared id>` disjunction.
            serde_json::json!([item("sample.refresh", "view == foreign.view", "navigation")]),
            serde_json::json!([item(
                "sample.refresh",
                "view != sample.projects",
                "navigation"
            )]),
            serde_json::json!([item(
                "sample.refresh",
                "view == sample.projects && isWeb",
                "navigation"
            )]),
            serde_json::json!([item(
                "sample.refresh",
                "viewItem == sample.projects",
                "navigation"
            )]),
            serde_json::json!([item("sample.refresh", "", "navigation")]),
            serde_json::json!([item(
                "sample.refresh",
                "view == sample.projects ||",
                "navigation"
            )]),
            serde_json::json!([item(
                "sample.refresh",
                "view == sample.projects || view == sample.projects",
                "navigation"
            )]),
            // The same command twice on one View.
            serde_json::json!([
                item("sample.refresh", "view == sample.projects", "navigation"),
                item("sample.refresh", "view == sample.projects", "navigation")
            ]),
            // Unknown item and menu keys.
            serde_json::json!([{"command":"sample.refresh", "when":"view == sample.projects", "group":"navigation", "alt":"sample.plain"}]),
        ];
        for view_title in cases {
            assert_eq!(
                with_view_title(icon.clone(), view_title.clone())
                    .unwrap_err()
                    .code,
                ErrorCode::InvalidManifest,
                "{view_title}"
            );
        }
        // A package image path is a valid command icon, but only a row action can
        // draw one; a title button needs a codicon.
        assert_eq!(
            with_view_title(
                serde_json::json!([{"command":"sample.refresh", "title":"Refresh", "icon":"resources/refresh.svg"}]),
                serde_json::json!([item("sample.refresh", "view == sample.projects", "navigation")])
            )
            .unwrap_err()
            .code,
            ErrorCode::InvalidManifest
        );
        // A title button draws a bundled codicon, whose names are lower case.
        // `$(Refresh)` is a well-formed ThemeIcon yet names no glyph, so pack
        // refuses it instead of shipping a package every window would exclude.
        for icon in ["$(Refresh)", "$(REFRESH)", "$(reFresh-all)"] {
            assert_eq!(
                with_view_title(
                    serde_json::json!([{"command":"sample.refresh", "title":"Refresh", "icon": icon}]),
                    serde_json::json!([item("sample.refresh", "view == sample.projects", "navigation")])
                )
                .unwrap_err()
                .code,
                ErrorCode::InvalidManifest,
                "{icon}"
            );
        }
        // The same command without a menu placement still parses: nothing tries
        // to draw that icon.
        assert!(with_view_title(
            serde_json::json!([{"command":"sample.refresh", "title":"Refresh", "icon":"$(Refresh)"}]),
            serde_json::json!([])
        )
        .is_ok());
        for bad_icon in [
            "refresh",
            "$(refresh",
            "$()",
            "../refresh.svg",
            "resources/refresh.gif",
            "$(re fresh)",
        ] {
            assert_eq!(
                with_view_title(
                    serde_json::json!([{"command":"sample.refresh", "title":"Refresh", "icon": bad_icon}]),
                    serde_json::json!([])
                )
                .unwrap_err()
                .code,
                ErrorCode::InvalidManifest,
                "{bad_icon}"
            );
        }
        // At most eight title actions fit one View.
        let nine: Vec<_> = (0..9)
            .map(|index| serde_json::json!({"command": format!("sample.act{index}"), "title":"Act", "icon":"$(refresh)"}))
            .collect();
        let placed: Vec<_> = (0..9)
            .map(|index| {
                item(
                    &format!("sample.act{index}"),
                    "view == sample.projects",
                    "navigation",
                )
            })
            .collect();
        assert_eq!(
            with_view_title(serde_json::json!(nine), serde_json::json!(placed[..8]))
                .unwrap()
                .contributes
                .menus
                .view_title
                .len(),
            8
        );
        assert_eq!(
            with_view_title(serde_json::json!(nine), serde_json::json!(placed))
                .unwrap_err()
                .code,
            ErrorCode::InvalidManifest
        );
        let mut value: serde_json::Value = serde_json::from_slice(include_bytes!(
            "../../extensions/sakura_senp_sample/senp.json"
        ))
        .unwrap();
        value["contributes"]["menus"] = serde_json::json!({"editor/title": []});
        assert_eq!(
            parse_manifest(&serde_json::to_vec(&value).unwrap())
                .unwrap_err()
                .code,
            ErrorCode::InvalidManifest
        );
    }

    fn with_view_item(
        commands: serde_json::Value,
        view_item: serde_json::Value,
    ) -> Result<Manifest, SenpError> {
        let mut value: serde_json::Value = serde_json::from_slice(include_bytes!(
            "../../extensions/sakura_senp_sample/senp.json"
        ))
        .unwrap();
        value["contributes"]["commands"] = commands;
        value["contributes"]["menus"] = serde_json::json!({ "view/item/context": view_item });
        parse_manifest(&serde_json::to_vec(&value).unwrap())
    }

    #[test]
    fn view_item_menus_accept_inline_actions_matched_by_context_value() {
        let commands = serde_json::json!([
            {"command":"sample.logs", "title":"View logs", "icon":"resources/icons/light/logs.svg"},
            {"command":"sample.open", "title":"Open", "icon":"$(go-to-file)"}
        ]);
        let manifest = with_view_item(
            commands,
            serde_json::json!([
                {"command":"sample.logs", "when":"viewItem =~ /job/ && viewItem =~ /completed/", "group":"inline"},
                {"command":"sample.open", "when":" view == sample.states&&viewItem == step ", "group":"inline"}
            ]),
        )
        .unwrap();
        let items = &manifest.contributes.menus.view_item_context;
        assert_eq!(items.len(), 2);
        let first = view_item_when(&items[0].when).unwrap();
        assert_eq!(first.contains, ["job", "completed"]);
        assert!(first.equals.is_empty());
        assert_eq!(first.view, None);
        let second = view_item_when(&items[1].when).unwrap();
        assert!(second.contains.is_empty());
        assert_eq!(second.equals, ["step"]);
        assert_eq!(second.view, Some("sample.states"));
        let serialized = serde_json::to_value(&manifest).unwrap();
        assert_eq!(
            serialized["contributes"]["menus"]["view/item/context"]
                .as_array()
                .unwrap()
                .len(),
            2
        );
        assert!(serialized["contributes"]["menus"]
            .get("view/title")
            .is_none());
    }

    #[test]
    fn view_item_menus_reject_what_a_row_action_cannot_show() {
        let commands = serde_json::json!([
            {"command":"sample.logs", "title":"View logs", "icon":"resources/icons/light/logs.svg"},
            {"command":"sample.plain", "title":"Plain"},
            {"command":"sample.upper", "title":"Upper", "icon":"$(Output)"}
        ]);
        let item = |command: &str, when: &str, group: &str| serde_json::json!({"command": command, "when": when, "group": group});
        let cases = [
            // Undeclared command, a command without an icon, a context-menu group.
            serde_json::json!([item("sample.other", "viewItem =~ /job/", "inline")]),
            serde_json::json!([item("sample.plain", "viewItem =~ /job/", "inline")]),
            serde_json::json!([item("sample.logs", "viewItem =~ /job/", "navigation")]),
            serde_json::json!([item("sample.logs", "viewItem =~ /job/", "")]),
            // Only `viewItem` matches and one `view ==` joined by `&&`.
            serde_json::json!([item(
                "sample.logs",
                "viewItem =~ /job/ || viewItem =~ /step/",
                "inline"
            )]),
            serde_json::json!([item("sample.logs", "viewItem != job", "inline")]),
            serde_json::json!([item("sample.logs", "!viewItem", "inline")]),
            serde_json::json!([item("sample.logs", "viewItem =~ /jo.b/", "inline")]),
            serde_json::json!([item("sample.logs", "viewItem =~ /job/i", "inline")]),
            serde_json::json!([item("sample.logs", "viewItem =~ //", "inline")]),
            serde_json::json!([item("sample.logs", "view == sample.projects", "inline")]),
            serde_json::json!([item("sample.logs", "viewItem =~ /job/ && isWeb", "inline")]),
            // A row action icon is a lower-case bundled codicon or a package
            // image path; an upper-case ThemeIcon names no bundled glyph.
            serde_json::json!([item("sample.upper", "viewItem =~ /job/", "inline")]),
            serde_json::json!([item(
                "sample.logs",
                "view == foreign.view && viewItem == job",
                "inline"
            )]),
            serde_json::json!([item(
                "sample.logs",
                "view == sample.projects && view == sample.states && viewItem == job",
                "inline"
            )]),
            serde_json::json!([item(
                "sample.logs",
                "viewItem == job && viewItem == job",
                "inline"
            )]),
            serde_json::json!([item("sample.logs", "viewItem =~ /job/ &&", "inline")]),
            serde_json::json!([item("sample.logs", "", "inline")]),
            // The same command twice on one View, directly or through "all Views".
            serde_json::json!([
                item("sample.logs", "viewItem == job", "inline"),
                item("sample.logs", "viewItem == step", "inline")
            ]),
            serde_json::json!([
                item("sample.logs", "viewItem == job", "inline"),
                item(
                    "sample.logs",
                    "view == sample.projects && viewItem == step",
                    "inline"
                )
            ]),
            serde_json::json!([{"command":"sample.logs", "when":"viewItem == job", "group":"inline", "alt":"sample.plain"}]),
        ];
        for view_item in cases {
            assert_eq!(
                with_view_item(commands.clone(), view_item.clone())
                    .unwrap_err()
                    .code,
                ErrorCode::InvalidManifest,
                "{view_item}"
            );
        }
        // At most eight inline actions fit one row.
        let nine: Vec<_> = (0..9)
            .map(|index| serde_json::json!({"command": format!("sample.act{index}"), "title":"Act", "icon":"$(output)"}))
            .collect();
        let placed: Vec<_> = (0..9)
            .map(|index| {
                item(
                    &format!("sample.act{index}"),
                    "view == sample.projects && viewItem == job",
                    "inline",
                )
            })
            .collect();
        assert_eq!(
            with_view_item(serde_json::json!(nine), serde_json::json!(placed[..8]))
                .unwrap()
                .contributes
                .menus
                .view_item_context
                .len(),
            8
        );
        assert_eq!(
            with_view_item(serde_json::json!(nine), serde_json::json!(placed))
                .unwrap_err()
                .code,
            ErrorCode::InvalidManifest
        );
    }

    #[test]
    fn manifest_version_dispatch_never_ignores_malformed_json() {
        for input in [
            r#"{"schemaVersion":2,"schemaVersion":1}"#,
            r#"{"schemaVersion":2,"runtime":{"abi":"one","abi":"two"}}"#,
            r#"{"schemaVersion":2} {}"#,
            r#"{"schemaVersion":"2"}"#,
            r#"{"schemaVersion":2.0}"#,
            r#"{"schemaVersion":-1}"#,
            r#"{"schemaVersion":2,"runtime":null}"#,
            r#"{"schemaVersion":2}"#,
            r#"[]"#,
        ] {
            assert_eq!(
                parse_manifest(input.as_bytes()).unwrap_err().code,
                ErrorCode::InvalidManifest,
                "{input}"
            );
        }
    }

    #[test]
    fn v2_runtime_packs_installs_and_lists_alongside_an_untouched_v1_extension() {
        let temp = TempDir::new().unwrap();
        let declarative = temp.path().join("v1");
        fixture(&declarative);
        let v1 = temp.path().join("v1.senp");
        let v1_hash = pack_directory(&declarative, &v1, None).unwrap();
        let root = temp.path().join("installed");
        install_package(
            &v1,
            &root,
            &TrustPolicy::BuiltIn {
                expected_archive_sha256: v1_hash,
            },
        )
        .unwrap();

        // The same payload layout under a v2 manifest, and under its own id, so
        // both live in one install root and neither may displace the other.
        let effect = temp.path().join("v2");
        fixture(&effect);
        fs::write(effect.join(MANIFEST_PATH), versioned_manifest(2, ABI_V2)).unwrap();
        let v2 = temp.path().join("v2.senp");
        let v2_hash = pack_directory(&effect, &v2, None).unwrap();
        let verified = verify_package(&v2, &TrustPolicy::DeveloperUnsigned).unwrap();
        assert_eq!(verified.manifest.schema_version, 2);
        assert_eq!(verified.archive_sha256, v2_hash);
        install_package(
            &v2,
            &root,
            &TrustPolicy::BuiltIn {
                expected_archive_sha256: v2_hash,
            },
        )
        .unwrap();

        let listed = list_installed(&root).unwrap();
        let rows = installed_rows(&listed);
        assert_eq!(rows.len(), 2);
        let effect_entry = rows
            .iter()
            .find(|entry| entry.manifest.id == "sample-github")
            .unwrap();
        assert_eq!(effect_entry.manifest.schema_version, 2);
        assert_eq!(effect_entry.manifest.runtime.as_ref().unwrap().abi, ABI_V2);
        // The module is what the effect host will load, so listing has to name
        // its digest; without one the package would be admitted as executable
        // while nothing could prove which bytes were installed.
        assert_eq!(
            effect_entry.module_sha256,
            Some(hex(&Sha256::digest(b"wasm")))
        );

        // Disabling one package leaves the other alone. The v1 extension shares
        // the install root, and releasing this gate must not couple lifecycles.
        set_extension_enabled(&root, "sample-github", false).unwrap();
        let listed = list_installed(&root).unwrap();
        let rows = installed_rows(&listed);
        for entry in &rows {
            assert_eq!(
                entry.enabled,
                entry.manifest.id == "sample-indent",
                "{}",
                entry.manifest.id
            );
        }
        assert_eq!(
            rows.iter()
                .find(|entry| entry.manifest.id == "sample-indent")
                .unwrap()
                .manifest
                .schema_version,
            1
        );

        // What was released is the runtime's gate, not the version number's. A
        // v2 manifest naming the v1 ABI is still a package for a host that
        // cannot run it, and packing must refuse it rather than publish it.
        fs::write(effect.join(MANIFEST_PATH), versioned_manifest(2, ABI)).unwrap();
        let crossed = temp.path().join("crossed.senp");
        assert_eq!(
            pack_directory(&effect, &crossed, None).unwrap_err().code,
            ErrorCode::AbiMismatch
        );
        assert!(!crossed.exists());
    }

    #[test]
    fn the_v2_runtime_abi_is_the_wit_package_version() {
        // The host pins the WIT digest to this version (sakura_senp_host
        // tests/wit_pin.rs); the manifest gate must name the same world.
        let wit = include_str!("../../wit/v2/senp-extension.wit");
        let version = wit
            .lines()
            .find_map(|line| line.trim().strip_prefix("package sakura:senp@"))
            .and_then(|rest| rest.strip_suffix(';'))
            .unwrap();
        assert_eq!(ABI_V2, format!("sakura:senp/extension@{version}"));
        for manifest in [
            include_str!("../../extensions/sakura_github_actions/senp.json"),
            include_str!("../../extensions/sakura_github_pull_requests/senp.json"),
            include_str!("../../extensions/sakura_senp_sample/senp.json"),
        ] {
            let manifest = parse_manifest(manifest.as_bytes()).unwrap();
            assert_eq!(manifest.runtime.unwrap().abi, ABI_V2, "{}", manifest.id);
        }
    }

    /// Replaces an installed package's manifest and re-signs its checksum line,
    /// as an older build's install of the same id would have left it.
    fn rewrite_installed_manifest(content: &Path, manifest: &[u8]) {
        fs::write(content.join(MANIFEST_PATH), manifest).unwrap();
        let checksums = fs::read_to_string(content.join(CHECKSUM_PATH)).unwrap();
        let digest = hex(&Sha256::digest(manifest));
        let rewritten: String = checksums
            .lines()
            .map(|line| match line.split_once("  ") {
                Some((_, path)) if path == MANIFEST_PATH => format!("{digest}  {path}\n"),
                _ => format!("{line}\n"),
            })
            .collect();
        fs::write(content.join(CHECKSUM_PATH), rewritten).unwrap();
    }

    #[test]
    fn installed_package_for_another_runtime_abi_is_listed_as_incompatible() {
        let temp = TempDir::new().unwrap();
        let declarative = temp.path().join("v1");
        fixture(&declarative);
        let v1 = temp.path().join("v1.senp");
        let v1_hash = pack_directory(&declarative, &v1, None).unwrap();
        let root = temp.path().join("installed");
        install_package(
            &v1,
            &root,
            &TrustPolicy::BuiltIn {
                expected_archive_sha256: v1_hash,
            },
        )
        .unwrap();
        let effect = temp.path().join("v2");
        fixture(&effect);
        fs::write(effect.join(MANIFEST_PATH), versioned_manifest(2, ABI_V2)).unwrap();
        let v2 = temp.path().join("v2.senp");
        let v2_hash = pack_directory(&effect, &v2, None).unwrap();
        install_package(
            &v2,
            &root,
            &TrustPolicy::BuiltIn {
                expected_archive_sha256: v2_hash.clone(),
            },
        )
        .unwrap();
        assert!(installed_rows(&list_installed(&root).unwrap())
            .iter()
            .all(|entry| entry.compatible));

        // A package an earlier build installed for its own WIT world. The host
        // here could not instantiate it, but one such package must neither hide
        // the others nor disappear itself: it is listed so it can be refreshed.
        let content = root.join("content").join(&v2_hash);
        let stale = "sakura:senp/extension@2.0.1";
        assert_ne!(stale, ABI_V2);
        rewrite_installed_manifest(&content, &versioned_manifest(2, stale));
        let listed = list_installed(&root).unwrap();
        let rows = installed_rows(&listed);
        assert_eq!(rows.len(), 2);
        for entry in &rows {
            assert_eq!(
                entry.compatible,
                entry.manifest.id == "sample-indent",
                "{}",
                entry.manifest.id
            );
        }
        let stale_entry = rows
            .iter()
            .find(|entry| entry.manifest.id == "sample-github")
            .unwrap();
        assert_eq!(stale_entry.manifest.runtime.as_ref().unwrap().abi, stale);

        // Only installed content is admitted this way. The same manifest can
        // still not be packed or verified by this build.
        fs::write(effect.join(MANIFEST_PATH), versioned_manifest(2, stale)).unwrap();
        assert_eq!(
            pack_directory(&effect, &temp.path().join("stale.senp"), None)
                .unwrap_err()
                .code,
            ErrorCode::AbiMismatch
        );

        // What is not an extension ABI at all, or the v1 ABI under a v2 manifest,
        // is malformed installed state: its one entry degrades to an invalid
        // row while the sibling stays listed, instead of failing the command.
        for abi in [ABI, "sakura:senp/extension@x.0.0", "other:world@3.0.0"] {
            rewrite_installed_manifest(&content, &versioned_manifest(2, abi));
            let listed = list_installed(&root).unwrap();
            assert_eq!(
                invalid(&listed),
                [("sample-github", ErrorCode::AbiMismatch)],
                "{abi}"
            );
            assert_eq!(installed_rows(&listed).len(), 1, "{abi}");
        }
    }

    #[test]
    fn publisher_policy_rejects_unsigned_package() {
        let temp = TempDir::new().unwrap();
        let source = temp.path().join("source");
        fixture(&source);
        let package = temp.path().join("sample.senp");
        pack_directory(&source, &package, None).unwrap();
        let error =
            verify_package(&package, &TrustPolicy::PublisherKeys(BTreeMap::new())).unwrap_err();
        assert_eq!(error.code, ErrorCode::UnsignedPackage);
    }

    #[test]
    fn developer_package_installs_disabled_until_explicitly_enabled() {
        let temp = TempDir::new().unwrap();
        let source = temp.path().join("source");
        fixture(&source);
        let package = temp.path().join("sample.senp");
        pack_directory(&source, &package, None).unwrap();
        let install_root = temp.path().join("installed");
        install_package(&package, &install_root, &TrustPolicy::DeveloperUnsigned).unwrap();

        let listed = list_installed(&install_root).unwrap();
        let rows = installed_rows(&listed);
        assert_eq!(rows.len(), 1);
        assert_eq!(rows[0].trust, "developer");
        assert!(!rows[0].enabled);
    }

    #[test]
    fn installed_payload_hash_mismatch_fails_closed() {
        let temp = TempDir::new().unwrap();
        let source = temp.path().join("source");
        fixture(&source);
        let package = temp.path().join("sample.senp");
        let hash = pack_directory(&source, &package, None).unwrap();
        let install_root = temp.path().join("installed");
        install_package(
            &package,
            &install_root,
            &TrustPolicy::BuiltIn {
                expected_archive_sha256: hash.clone(),
            },
        )
        .unwrap();

        fs::write(
            install_root.join("content").join(&hash).join(MODULE_PATH),
            b"tampered",
        )
        .unwrap();
        // One tampered content directory degrades to an invalid row rather
        // than failing the whole listing.
        let listed = list_installed(&install_root).unwrap();
        assert_eq!(
            invalid(&listed),
            [("sample-indent", ErrorCode::ChecksumMismatch)]
        );
        assert!(installed_rows(&listed).is_empty());
    }

    #[test]
    fn installed_checksum_coverage_rejects_unlisted_payload() {
        let temp = TempDir::new().unwrap();
        let source = temp.path().join("source");
        fixture(&source);
        let package = temp.path().join("sample.senp");
        let hash = pack_directory(&source, &package, None).unwrap();
        let install_root = temp.path().join("installed");
        install_package(
            &package,
            &install_root,
            &TrustPolicy::BuiltIn {
                expected_archive_sha256: hash.clone(),
            },
        )
        .unwrap();

        fs::write(
            install_root
                .join("content")
                .join(hash)
                .join("assets/icons/unlisted.txt"),
            b"unlisted",
        )
        .unwrap();
        let listed = list_installed(&install_root).unwrap();
        assert_eq!(
            invalid(&listed),
            [("sample-indent", ErrorCode::InvalidChecksumDocument)]
        );
        assert!(installed_rows(&listed).is_empty());
    }

    #[test]
    fn archive_snapshot_survives_source_replacement_before_install() {
        let temp = TempDir::new().unwrap();
        let source_a = temp.path().join("source-a");
        let source_b = temp.path().join("source-b");
        fixture(&source_a);
        fixture(&source_b);
        fs::write(source_b.join(README_PATH), "replacement\n").unwrap();
        let package_a = temp.path().join("a.senp");
        let package_b = temp.path().join("b.senp");
        let hash_a = pack_directory(&source_a, &package_a, None).unwrap();
        pack_directory(&source_b, &package_b, None).unwrap();

        let snapshot = read_archive_snapshot(&package_a).unwrap();
        fs::copy(&package_b, &package_a).unwrap();
        let verified = verify_archive_snapshot(
            &snapshot,
            &TrustPolicy::BuiltIn {
                expected_archive_sha256: hash_a.clone(),
            },
        )
        .unwrap();
        let installed = install_verified_archive(
            &temp.path().join("installed"),
            &TrustPolicy::BuiltIn {
                expected_archive_sha256: hash_a.clone(),
            },
            verified,
        )
        .unwrap();

        assert_eq!(installed.archive_sha256, hash_a);
        let listed = list_installed(&temp.path().join("installed")).unwrap();
        assert_eq!(installed_rows(&listed)[0].manifest.id, "sample-indent");
        assert!(serde_json::to_value(&listed[0])
            .unwrap()
            .get("readme")
            .is_none());
    }

    #[test]
    fn pack_rejects_non_utf8_readme_before_publication() {
        let temp = TempDir::new().unwrap();
        let source = temp.path().join("source");
        fixture(&source);
        fs::write(source.join(README_PATH), [0xff, 0xfe]).unwrap();
        let error = pack_directory(&source, &temp.path().join("sample.senp"), None).unwrap_err();
        assert_eq!(error.code, ErrorCode::InvalidUtf8);
    }

    // ---- B3a: per-package listing isolation and one list budget (#356, #362) ----

    fn fixture_with_id(root: &Path, id: &str) {
        fixture(root);
        let manifest = fs::read_to_string(root.join(MANIFEST_PATH))
            .unwrap()
            .replace("\"id\": \"sample-indent\"", &format!("\"id\": \"{id}\""));
        fs::write(root.join(MANIFEST_PATH), manifest).unwrap();
    }

    fn install_built_in(source: &Path, install_root: &Path) -> String {
        let package = source.with_extension("senp");
        let hash = pack_directory(source, &package, None).unwrap();
        install_package(
            &package,
            install_root,
            &TrustPolicy::BuiltIn {
                expected_archive_sha256: hash.clone(),
            },
        )
        .unwrap();
        hash
    }

    /// Three installed packages; `corrupt` names the one whose entry is
    /// damaged. Returns `(install root, content dir of the damaged package)`.
    fn three_installed(temp: &TempDir, corrupt: usize) -> (PathBuf, PathBuf) {
        let root = temp.path().join("installed");
        let mut damaged = PathBuf::new();
        for (index, id) in ["pkg-alpha", "pkg-beta", "pkg-gamma"].iter().enumerate() {
            let source = temp.path().join(format!("source-{index}"));
            fixture_with_id(&source, id);
            let hash = install_built_in(&source, &root);
            if index == corrupt {
                damaged = root.join("content").join(&hash);
            }
        }
        (root, damaged)
    }

    #[test]
    fn list_reports_one_bad_package_as_an_invalid_row() {
        let temp = TempDir::new().unwrap();
        let (root, damaged) = three_installed(&temp, 1);
        fs::write(damaged.join(MODULE_PATH), b"tampered").unwrap();
        let rows = list_installed(&root).unwrap();
        assert_eq!(rows.len(), 3);
        assert_eq!(
            installed_rows(&rows)
                .iter()
                .map(|entry| entry.manifest.id.as_str())
                .collect::<Vec<_>>(),
            ["pkg-alpha", "pkg-gamma"]
        );
        assert_eq!(invalid(&rows), [("pkg-beta", ErrorCode::ChecksumMismatch)]);
    }

    #[test]
    fn list_reports_unreadable_state_as_an_invalid_row() {
        let temp = TempDir::new().unwrap();
        let (root, _) = three_installed(&temp, 1);
        // State JSON that fails strict parsing. The id is not provable, so the
        // invalid row carries an empty id and sorts last, by file name.
        fs::write(root.join("profiles").join("pkg-beta.json"), b"{").unwrap();
        let rows = list_installed(&root).unwrap();
        assert_eq!(rows.len(), 3);
        assert_eq!(invalid(&rows), [("", ErrorCode::InvalidManifest)]);
        assert!(matches!(rows.last(), Some(InstalledListRow::Invalid { id, .. }) if id.is_empty()));
        // An oversized state document is the same per-entry failure.
        fs::write(
            root.join("profiles").join("pkg-alpha.json"),
            vec![b' '; MAX_DOCUMENT_BYTES as usize + 1],
        )
        .unwrap();
        let rows = list_installed(&root).unwrap();
        assert_eq!(rows.len(), 3);
        assert_eq!(installed_rows(&rows).len(), 1);
        assert_eq!(
            invalid(&rows),
            [
                ("pkg-alpha", ErrorCode::EntryTooLarge),
                ("", ErrorCode::InvalidManifest)
            ]
        );
    }

    #[test]
    fn list_reports_state_manifest_id_mismatch_as_an_invalid_row() {
        let temp = TempDir::new().unwrap();
        let (root, damaged) = three_installed(&temp, 1);
        // Repoint the middle package's profile state at a different id. The
        // content directory itself is intact; only the state/manifest pair is
        // inconsistent.
        let archive = damaged.file_name().unwrap().to_str().unwrap().to_owned();
        write_profile_state(
            &root,
            &ProfileState {
                schema_version: 1,
                id: "pkg-beta".to_owned(),
                archive_sha256: archive.clone(),
                enabled: true,
                signed: false,
                trust: "builtin".to_owned(),
            },
        )
        .unwrap();
        // Now claim the same content under a different profile id as well:
        // the content's manifest id differs from the state's id.
        write_profile_state(
            &root,
            &ProfileState {
                schema_version: 1,
                id: "pkg-delta".to_owned(),
                archive_sha256: archive,
                enabled: true,
                signed: false,
                trust: "builtin".to_owned(),
            },
        )
        .unwrap();
        let rows = list_installed(&root).unwrap();
        assert_eq!(installed_rows(&rows).len(), 3);
        assert_eq!(invalid(&rows), [("pkg-delta", ErrorCode::InvalidManifest)]);
    }

    #[test]
    fn list_reports_trust_signature_mismatch_as_an_invalid_row() {
        let temp = TempDir::new().unwrap();
        let (root, damaged) = three_installed(&temp, 1);
        let archive = damaged.file_name().unwrap().to_str().unwrap().to_owned();
        // signed=true with trust "developer" contradicts the profile rule
        // (trust == "publisher") == signed.
        write_profile_state(
            &root,
            &ProfileState {
                schema_version: 1,
                id: "pkg-beta".to_owned(),
                archive_sha256: archive,
                enabled: true,
                signed: true,
                trust: "developer".to_owned(),
            },
        )
        .unwrap();
        let rows = list_installed(&root).unwrap();
        assert_eq!(installed_rows(&rows).len(), 2);
        assert_eq!(invalid(&rows), [("pkg-beta", ErrorCode::InvalidSignature)]);
    }

    #[test]
    fn list_reports_a_symlink_state_entry_as_an_invalid_row() {
        let temp = TempDir::new().unwrap();
        let (root, _) = three_installed(&temp, 1);
        let outside = temp.path().join("outside.json");
        fs::write(&outside, b"{}").unwrap();
        #[cfg(unix)]
        std::os::unix::fs::symlink(&outside, root.join("profiles").join("pkg-link.json")).unwrap();
        #[cfg(windows)]
        std::os::windows::fs::symlink_file(&outside, root.join("profiles").join("pkg-link.json"))
            .unwrap();
        let rows = list_installed(&root).unwrap();
        // A symlink state file is a per-entry failure (invalid row), not a
        // root failure: the other packages stay listed.
        assert_eq!(installed_rows(&rows).len(), 3);
        assert_eq!(invalid(&rows), [("pkg-link", ErrorCode::ReparsePoint)]);
    }

    #[test]
    fn list_still_fails_when_the_install_root_is_a_reparse_point() {
        // Whole-root failures stay command failures: a reparse point at the
        // install root (or at `profiles/`) means the profile's own boundary is
        // violated, so no listing is produced at all.
        let temp = TempDir::new().unwrap();
        let target = temp.path().join("real-root");
        fs::create_dir_all(&target).unwrap();
        let link = temp.path().join("root-link");
        #[cfg(unix)]
        std::os::unix::fs::symlink(&target, &link).unwrap();
        #[cfg(windows)]
        std::os::windows::fs::symlink_dir(&target, &link).unwrap();
        assert_eq!(
            list_installed(&link).unwrap_err().code,
            ErrorCode::ReparsePoint
        );

        // A symlinked `profiles/` inside a real root is the same root-level
        // refusal.
        let (root, _) = three_installed(&temp, 0);
        let moved = temp.path().join("moved-profiles");
        fs::rename(root.join("profiles"), &moved).unwrap();
        #[cfg(unix)]
        std::os::unix::fs::symlink(&moved, root.join("profiles")).unwrap();
        #[cfg(windows)]
        std::os::windows::fs::symlink_dir(&moved, root.join("profiles")).unwrap();
        assert_eq!(
            list_installed(&root).unwrap_err().code,
            ErrorCode::ReparsePoint
        );

        // A dangling `profiles/` link is the same refusal, never an empty
        // profile: following the link would report "not found".
        fs::remove_dir_all(&moved).unwrap();
        assert_eq!(
            list_installed(&root).unwrap_err().code,
            ErrorCode::ReparsePoint
        );
    }

    #[test]
    fn installed_rows_have_no_readme_and_a_large_readme_still_lists() {
        let temp = TempDir::new().unwrap();
        let source = temp.path().join("source");
        fixture(&source);
        // README.md stays mandatory and checksum-covered; it only leaves the
        // listing. A 2 MiB one must list fine and keep its line small.
        fs::write(source.join(README_PATH), vec![b'x'; 2 * 1024 * 1024]).unwrap();
        let root = temp.path().join("installed");
        install_built_in(&source, &root);
        let rows = list_installed(&root).unwrap();
        assert_eq!(rows.len(), 1);
        let value = serde_json::to_value(&rows[0]).unwrap();
        assert!(value.get("readme").is_none());
        assert_eq!(value["kind"], "installed");
        let output = serialize_installed_listing(&rows).unwrap();
        assert!(output.len() < 64 * 1024, "{}", output.len());
        assert!(output.ends_with('\n'));
    }

    #[test]
    fn an_over_budget_line_becomes_an_invalid_row_and_the_total_budget_fails() {
        let temp = TempDir::new().unwrap();
        let (root, _) = three_installed(&temp, 1);
        let rows = list_installed(&root).unwrap();
        assert_eq!(rows.len(), 3);
        // A row that cannot fit the decoder's per-document budget is replaced
        // by the typed invalid row, never emitted over-budget.
        let output =
            serialize_installed_listing_bounded(&rows, 256, LIST_OUTPUT_MAX_BYTES).unwrap();
        let lines: Vec<serde_json::Value> = output
            .lines()
            .map(|line| serde_json::from_str(line).unwrap())
            .collect();
        assert_eq!(lines.len(), 3);
        assert!(lines
            .iter()
            .all(|line| line["kind"] == "invalid" && line["code"] == "outputTooLarge"));
        // Exceeding the whole 4 MiB stdout budget is a typed command failure:
        // nothing partial is produced for the caller to trust.
        let error = serialize_installed_listing_bounded(&rows, LIST_LINE_MAX_BYTES, 4).unwrap_err();
        assert_eq!(error.code, ErrorCode::OutputTooLarge);
        // The production constants are the documented ones.
        assert_eq!(LIST_LINE_MAX_BYTES, 1024 * 1024);
        assert_eq!(LIST_LINE_MAX_NODES, 65_536);
        assert_eq!(LIST_OUTPUT_MAX_BYTES, 4 * 1024 * 1024);
    }

    // ---- B4a: archive and package-store hygiene -------------------------

    /// A reader that serves `total` zero bytes and counts what it handed out.
    struct CountingReader {
        remaining: u64,
        served: u64,
    }

    impl CountingReader {
        fn new(total: u64) -> Self {
            Self {
                remaining: total,
                served: 0,
            }
        }
    }

    impl Read for CountingReader {
        fn read(&mut self, buffer: &mut [u8]) -> std::io::Result<usize> {
            let count = (buffer.len() as u64).min(self.remaining) as usize;
            buffer[..count].fill(0);
            self.remaining -= count as u64;
            self.served += count as u64;
            Ok(count)
        }
    }

    /// A Deflate ZIP of `(name, payload)` entries.
    fn deflated_zip(entries: &[(&str, &[u8])]) -> Vec<u8> {
        let mut writer = ZipWriter::new(Cursor::new(Vec::new()));
        let options = SimpleFileOptions::default().compression_method(CompressionMethod::Deflated);
        for (name, payload) in entries {
            writer.start_file(*name, options).unwrap();
            writer.write_all(payload).unwrap();
        }
        writer.finish().unwrap().into_inner()
    }

    /// Deterministic, poorly compressible bytes.
    fn noise(length: usize) -> Vec<u8> {
        let mut state = 0x2545_f491_u32;
        (0..length)
            .map(|_| {
                state = state.wrapping_mul(1_664_525).wrapping_add(1_013_904_223);
                (state >> 24) as u8
            })
            .collect()
    }

    /// Rewrites the declared uncompressed size of `name` in its local and
    /// central headers, leaving the compressed bytes untouched.
    fn declare_size(zip: &mut [u8], name: &str, declared: u32) {
        let mut patched = 0;
        for index in 0..zip.len().saturating_sub(46) {
            let (name_length_at, size_at, name_at) = match zip[index..index + 4] {
                [0x50, 0x4b, 0x03, 0x04] => (index + 26, index + 22, index + 30),
                [0x50, 0x4b, 0x01, 0x02] => (index + 28, index + 24, index + 46),
                _ => continue,
            };
            let length = u16::from_le_bytes([zip[name_length_at], zip[name_length_at + 1]]);
            if &zip[name_at..name_at + usize::from(length)] == name.as_bytes() {
                zip[size_at..size_at + 4].copy_from_slice(&declared.to_le_bytes());
                patched += 1;
            }
        }
        assert_eq!(patched, 2, "local and central header of {name}");
    }

    fn zip_entries(bytes: &[u8]) -> Result<BTreeMap<String, Vec<u8>>, SenpError> {
        let mut archive = ZipArchive::new(Cursor::new(bytes)).unwrap();
        read_zip_entries(&mut archive)
    }

    #[test]
    fn entry_read_stops_one_byte_past_the_declared_size() {
        // Declared 0 but the stream carries megabytes: the reader must not be
        // drained, and the entry is rejected after at most 1 byte.
        let mut reader = CountingReader::new(8 * 1024 * 1024);
        let error = read_entry_exact(&mut reader, 0).unwrap_err();
        assert_eq!(error.code, ErrorCode::InvalidZip);
        assert!(reader.served <= 1, "served {} bytes", reader.served);

        let mut reader = CountingReader::new(8 * 1024 * 1024);
        let error = read_entry_exact(&mut reader, 16).unwrap_err();
        assert_eq!(error.code, ErrorCode::InvalidZip);
        assert!(reader.served <= 17, "served {} bytes", reader.served);
    }

    #[test]
    fn entry_read_requires_exactly_the_declared_size() {
        let error = read_entry_exact(&mut CountingReader::new(10), 16).unwrap_err();
        assert_eq!(error.code, ErrorCode::InvalidZip);
        let bytes = read_entry_exact(&mut CountingReader::new(16), 16).unwrap();
        assert_eq!(bytes, vec![0u8; 16]);
        assert!(read_entry_exact(&mut CountingReader::new(0), 0)
            .unwrap()
            .is_empty());
    }

    #[test]
    fn deflate_entry_declaring_zero_bytes_is_rejected() {
        // 65 MiB of zeros inflate from a tiny Deflate stream.
        let payload = vec![0u8; 65 * 1024 * 1024];
        let mut zip = deflated_zip(&[("assets/bomb.bin", payload.as_slice())]);
        declare_size(&mut zip, "assets/bomb.bin", 0);
        let error = zip_entries(&zip).unwrap_err();
        assert_eq!(error.code, ErrorCode::InvalidZip);
    }

    #[test]
    fn declared_entry_size_must_match_the_real_size() {
        let payload = noise(1000);
        let honest = deflated_zip(&[("assets/data.bin", payload.as_slice())]);
        assert_eq!(zip_entries(&honest).unwrap()["assets/data.bin"], payload);

        let mut understated = honest.clone();
        declare_size(&mut understated, "assets/data.bin", 10);
        assert_eq!(
            zip_entries(&understated).unwrap_err().code,
            ErrorCode::InvalidZip
        );

        let mut overstated = honest;
        declare_size(&mut overstated, "assets/data.bin", 2000);
        assert_eq!(
            zip_entries(&overstated).unwrap_err().code,
            ErrorCode::InvalidZip
        );
    }

    #[test]
    fn two_central_directory_entries_with_one_exact_name_are_a_duplicate() {
        let mut zip = deflated_zip(&[("assets/a.txt", b"first"), ("assets/b.txt", b"second")]);
        // Rename the second entry in its local and central headers.
        let mut renamed = 0;
        for index in 0..zip.len() - 30 {
            let name_at = match zip[index..index + 4] {
                [0x50, 0x4b, 0x03, 0x04] => index + 30,
                [0x50, 0x4b, 0x01, 0x02] => index + 46,
                _ => continue,
            };
            if &zip[name_at..name_at + 12] == b"assets/b.txt" {
                zip[name_at..name_at + 12].copy_from_slice(b"assets/a.txt");
                renamed += 1;
            }
        }
        assert_eq!(renamed, 2);
        let snapshot = ArchiveSnapshot {
            archive_sha256: archive_digest(&zip),
            bytes: zip,
        };
        let error =
            verify_archive_snapshot(&snapshot, &TrustPolicy::DeveloperUnsigned).unwrap_err();
        assert_eq!(error.code, ErrorCode::DuplicateEntry);
    }

    #[test]
    fn central_directory_count_check_accepts_honest_archives_and_rejects_zip64() {
        let zip = deflated_zip(&[("assets/a.txt", b"first"), ("assets/b.txt", b"second")]);
        assert!(reject_collapsed_duplicate_entries(&zip, 2).is_ok());
        assert_eq!(
            reject_collapsed_duplicate_entries(&zip, 1)
                .unwrap_err()
                .code,
            ErrorCode::DuplicateEntry
        );
        // An EOCD record that defers to ZIP64 is not part of the format.
        let mut zip64 = zip.clone();
        let eocd = zip64.len() - 22;
        zip64[eocd + 10..eocd + 12].copy_from_slice(&0xffff_u16.to_le_bytes());
        assert_eq!(
            reject_collapsed_duplicate_entries(&zip64, 2)
                .unwrap_err()
                .code,
            ErrorCode::InvalidZip
        );
        assert_eq!(
            reject_collapsed_duplicate_entries(b"not a zip", 0)
                .unwrap_err()
                .code,
            ErrorCode::InvalidZip
        );
    }

    #[test]
    fn windows_reserved_names_are_rejected_in_entry_paths() {
        for rejected in [
            "assets/NUL.txt",
            "assets/con",
            "assets/Com1.json",
            "assets/lpt9",
            "assets/COM0.x.y",
            "assets/aux/readme.txt",
            "assets/prn .txt",
            "assets/com\u{b9}.json",
            "assets/LPT\u{b2}",
            "assets/COM\u{b3}.txt",
            "Com1.json",
        ] {
            let error = normalized_entry_name(rejected.as_bytes()).unwrap_err();
            assert_eq!(error.code, ErrorCode::UnsafePath, "{rejected}");
            assert!(!valid_asset_path(rejected), "{rejected}");
        }
        for accepted in [
            "assets/console",
            "assets/nullable.txt",
            "assets/com10",
            "assets/com10.json",
            "assets/lpt",
            "assets/auxiliary/x.txt",
            "assets/.con",
            "assets/icon.com1",
        ] {
            assert!(
                normalized_entry_name(accepted.as_bytes()).is_ok(),
                "{accepted}"
            );
        }
    }

    #[test]
    fn trailing_dot_or_space_is_rejected_in_entry_paths() {
        for rejected in [
            "assets/name.",
            "assets/name ",
            "assets/dir./x.txt",
            "assets/ ",
        ] {
            let error = normalized_entry_name(rejected.as_bytes()).unwrap_err();
            assert_eq!(error.code, ErrorCode::UnsafePath, "{rejected}");
        }
        assert!(normalized_entry_name(b"assets/name.txt").is_ok());
    }

    #[test]
    fn windows_reserved_names_are_rejected_as_package_ids() {
        for rejected in [
            "nul", "con", "aux", "prn", "com1", "lpt1", "com0", "nul.ext",
        ] {
            assert!(!valid_package_id(rejected), "{rejected}");
        }
        for accepted in [
            "console",
            "nullable",
            "com10",
            "lpt",
            "my-nul",
            "sample-indent",
        ] {
            assert!(valid_package_id(accepted), "{accepted}");
        }
        let temp = TempDir::new().unwrap();
        let source = temp.path().join("source");
        fixture_with_id(&source, "nul");
        let error = pack_directory(&source, &temp.path().join("nul.senp"), None).unwrap_err();
        assert_eq!(error.code, ErrorCode::InvalidManifest);
        fixture_with_id(&source, "lpt1");
        let error = pack_directory(&source, &temp.path().join("lpt1.senp"), None).unwrap_err();
        assert_eq!(error.code, ErrorCode::InvalidManifest);
        fixture_with_id(&source, "com10");
        pack_directory(&source, &temp.path().join("com10.senp"), None).unwrap();
    }

    #[test]
    fn installed_listing_turns_a_reserved_id_into_an_invalid_row() {
        let temp = TempDir::new().unwrap();
        let (root, _) = three_installed(&temp, 1);
        // A state file (named so the file system can hold it) that claims a
        // device name as its package id.
        let state = fs::read_to_string(root.join("profiles/pkg-beta.json"))
            .unwrap()
            .replace("pkg-beta", "nul");
        fs::write(root.join("profiles/pkg-beta.json"), state).unwrap();
        let rows = list_installed(&root).unwrap();
        assert_eq!(rows.len(), 3);
        assert_eq!(installed_rows(&rows).len(), 2);
        assert_eq!(invalid(&rows), [("", ErrorCode::InvalidManifest)]);
    }

    #[test]
    fn temporary_suffixes_differ_even_with_a_frozen_clock() {
        let first = temporary_suffix_at(42);
        let second = temporary_suffix_at(42);
        assert_ne!(first, second);
        assert_ne!(temporary_suffix(), temporary_suffix());
    }

    /// One package installed at `<temp>/installed`; returns
    /// `(package file, install root, content dir, policy)`.
    fn installed_sample(temp: &TempDir) -> (PathBuf, PathBuf, PathBuf, TrustPolicy) {
        let source = temp.path().join("source");
        fixture(&source);
        let package = temp.path().join("sample.senp");
        let hash = pack_directory(&source, &package, None).unwrap();
        let root = temp.path().join("installed");
        let policy = TrustPolicy::BuiltIn {
            expected_archive_sha256: hash.clone(),
        };
        install_package(&package, &root, &policy).unwrap();
        let content = root.join("content").join(&hash);
        (package, root, content, policy)
    }

    fn quarantine_leftovers(root: &Path) -> Vec<String> {
        fs::read_dir(root.join("content"))
            .unwrap()
            .map(|entry| entry.unwrap().file_name().to_string_lossy().into_owned())
            .filter(|name| name.contains(".corrupt."))
            .collect()
    }

    #[test]
    fn install_repairs_corrupt_content_of_the_same_digest() {
        let temp = TempDir::new().unwrap();
        let (package, root, content, policy) = installed_sample(&temp);
        fs::write(content.join(README_PATH), b"tampered").unwrap();
        let rows = list_installed(&root).unwrap();
        assert_eq!(
            invalid(&rows),
            [("sample-indent", ErrorCode::ChecksumMismatch)]
        );

        install_package(&package, &root, &policy).unwrap();

        let rows = list_installed(&root).unwrap();
        assert!(invalid(&rows).is_empty());
        assert_eq!(installed_rows(&rows).len(), 1);
        assert_eq!(fs::read(content.join(README_PATH)).unwrap(), b"# Sample\n");
        assert!(quarantine_leftovers(&root).is_empty());
    }

    #[test]
    fn install_repairs_content_that_is_not_even_a_directory() {
        let temp = TempDir::new().unwrap();
        let (package, root, content, policy) = installed_sample(&temp);
        fs::remove_dir_all(&content).unwrap();
        fs::write(&content, b"not a directory").unwrap();
        install_package(&package, &root, &policy).unwrap();
        assert!(content.join(MODULE_PATH).is_file());
        assert!(invalid(&list_installed(&root).unwrap()).is_empty());
        assert!(quarantine_leftovers(&root).is_empty());
    }

    #[cfg(windows)]
    #[test]
    fn install_reports_an_error_when_the_corrupt_content_cannot_be_moved_aside() {
        let temp = TempDir::new().unwrap();
        let (package, root, content, policy) = installed_sample(&temp);
        fs::write(content.join(README_PATH), b"tampered").unwrap();
        // A held handle inside the directory makes the rename fail.
        let held = File::open(content.join(MODULE_PATH)).unwrap();
        let error = install_package(&package, &root, &policy).unwrap_err();
        assert_eq!(error.code, ErrorCode::PublicationFailed);
        assert_eq!(fs::read(content.join(README_PATH)).unwrap(), b"tampered");
        assert!(quarantine_leftovers(&root).is_empty());
        drop(held);
        install_package(&package, &root, &policy).unwrap();
        assert!(invalid(&list_installed(&root).unwrap()).is_empty());
    }

    /// Runs `body` with a short mutator lock wait so contention tests stay fast.
    fn with_lock_wait(wait: Duration, body: impl FnOnce()) {
        ROOT_LOCK_WAIT_OVERRIDE.with(|cell| cell.set(Some(wait)));
        body();
        ROOT_LOCK_WAIT_OVERRIDE.with(|cell| cell.set(None));
    }

    #[test]
    fn a_held_root_lock_makes_every_mutator_fail_with_busy_but_not_readers() {
        let temp = TempDir::new().unwrap();
        let (package, root, _, policy) = installed_sample(&temp);
        let lock = acquire_root_lock_within(&root, Duration::from_secs(1)).unwrap();
        with_lock_wait(Duration::from_millis(300), || {
            let started = Instant::now();
            let error = set_extension_enabled(&root, "sample-indent", false).unwrap_err();
            assert_eq!(error.code, ErrorCode::Busy);
            let waited = started.elapsed();
            assert!(
                waited >= Duration::from_millis(250),
                "gave up after {waited:?}"
            );
            assert!(waited < Duration::from_secs(5), "waited {waited:?}");
            assert_eq!(
                install_package(&package, &root, &policy).unwrap_err().code,
                ErrorCode::Busy
            );
            let hash = "0".repeat(64);
            assert_eq!(
                uninstall_built_in(&root, "sample-indent", &hash)
                    .unwrap_err()
                    .code,
                ErrorCode::Busy
            );
        });
        // Readers never block on the lock.
        assert_eq!(installed_rows(&list_installed(&root).unwrap()).len(), 1);
        assert!(list_uninstalled(&root).unwrap().is_empty());
        // A rejected mutator changed nothing.
        assert!(installed_rows(&list_installed(&root).unwrap())[0].enabled);

        drop(lock);
        set_extension_enabled(&root, "sample-indent", false).unwrap();
        assert!(!installed_rows(&list_installed(&root).unwrap())[0].enabled);
        install_package(&package, &root, &policy).unwrap();
    }

    #[test]
    fn a_mutator_waits_for_a_lock_released_within_the_bound() {
        let temp = TempDir::new().unwrap();
        let (_, root, _, _) = installed_sample(&temp);
        let lock = acquire_root_lock_within(&root, Duration::from_secs(1)).unwrap();
        let releaser = std::thread::spawn(move || {
            std::thread::sleep(Duration::from_millis(150));
            drop(lock);
        });
        with_lock_wait(Duration::from_secs(5), || {
            set_extension_enabled(&root, "sample-indent", false).unwrap();
        });
        releaser.join().unwrap();
        assert!(!installed_rows(&list_installed(&root).unwrap())[0].enabled);
    }

    #[test]
    fn the_mutator_lock_wait_stays_under_the_native_tool_timeout() {
        assert!(ROOT_LOCK_WAIT <= Duration::from_secs(10));
        assert_eq!(root_lock_wait(), ROOT_LOCK_WAIT);
    }

    #[test]
    fn mutators_on_a_missing_root_keep_their_natural_error_and_create_nothing() {
        let temp = TempDir::new().unwrap();
        let root = temp.path().join("absent");
        let error = set_extension_enabled(&root, "sample-indent", true).unwrap_err();
        assert_eq!(error.code, ErrorCode::Io);
        assert!(!root.exists());
    }
}

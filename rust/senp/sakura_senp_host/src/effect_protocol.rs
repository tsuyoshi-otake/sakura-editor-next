//! Strict, bounded v2 transport DTOs. The v1 host never guesses this protocol.
use serde::{Deserialize, Serialize};

pub const MAX_FRAME_BYTES: usize = 1024 * 1024;
pub const MAX_EFFECTS: usize = 64;
pub const MAX_ITEMS: usize = 256;
pub const MAX_COUNTER: u64 = i64::MAX as u64;
pub const ABI: &str = "sakura:senp/extension@2.0.0";

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub enum CollapsibleState {
    Leaf,
    Collapsed,
    Expanded,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub enum CompletionStatus {
    Succeeded,
    Cancelled,
    Failed,
    TimedOut,
    HostUnavailable,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub enum PageStatus {
    Complete,
    Partial,
    Empty,
    Failed,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub enum TextStatus {
    Loading,
    Complete,
    Partial,
    Expired,
    Failed,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub enum StopReason {
    Disabled,
    Updated,
    Shutdown,
    ProtocolError,
    HostUnavailable,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub enum RejectionCode {
    Busy,
    Conflict,
    Stale,
    InvalidRequest,
    Unsupported,
    HostUnavailable,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct OperationContext {
    pub operation_id: String,
    pub owner_generation: u64,
    pub workspace_revision: u64,
    pub account_generation: u64,
    pub request_generation: u64,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct Field {
    pub name: String,
    pub value: String,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct Remote {
    pub name: String,
    pub url: String,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct Repository {
    pub root_id: String,
    pub branch: String,
    pub remotes: Vec<Remote>,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct TreeItem {
    pub id: String,
    pub label: String,
    pub description: String,
    pub tooltip: String,
    pub icon: String,
    pub collapsible_state: CollapsibleState,
    pub command_id: String,
    pub arguments: Vec<String>,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct TreeRequest {
    pub view_id: String,
    pub parent_id: String,
    pub cursor: String,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct DocumentRequest {
    pub resource_id: String,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct CommandInvoked {
    pub command_id: String,
    pub arguments: Vec<String>,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct ToolCompleted {
    pub read_id: String,
    pub status: CompletionStatus,
    pub data: String,
    pub message: String,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct WorkspaceChanged {
    pub repositories: Vec<Repository>,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct Cancel {
    pub operation_id: String,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct VisibilityChanged {
    pub view_id: String,
    pub visible: bool,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct StartToolRead {
    pub read_id: String,
    pub tool_id: String,
    pub operation: String,
    pub arguments: Vec<Field>,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct PublishTreePage {
    pub view_id: String,
    pub parent_id: String,
    pub items: Vec<TreeItem>,
    pub next_cursor: String,
    pub revision: u64,
    pub status: PageStatus,
    pub message: String,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct MarkdownSection {
    pub text: String,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct MetadataSection {
    pub fields: Vec<Field>,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct TableRow {
    pub cells: Vec<String>,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct TableSection {
    pub columns: Vec<String>,
    pub rows: Vec<TableRow>,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct TextResourceSection {
    pub handle: String,
    pub length: u64,
    pub status: TextStatus,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct CompleteCommand {
    pub status: CompletionStatus,
    pub message: String,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct InvalidateTree {
    pub view_id: String,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct OpenDocument {
    pub resource_id: String,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct ReleaseResource {
    pub handle: String,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct Hello {
    pub abi: String,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct Activate {
    pub context: OperationContext,
    pub extension_id: String,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct Deactivate {
    pub reason: StopReason,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct Ack {
    pub ack_sequence: u64,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct Rejected {
    pub request_sequence: u64,
    pub code: RejectionCode,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct Stopped {
    pub reason: StopReason,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(
    tag = "type",
    content = "data",
    rename_all = "camelCase",
    deny_unknown_fields
)]
pub enum DocumentSection {
    Markdown(MarkdownSection),
    Metadata(MetadataSection),
    Table(TableSection),
    TextResource(TextResourceSection),
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(
    tag = "type",
    content = "data",
    rename_all = "camelCase",
    deny_unknown_fields
)]
pub enum Event {
    TreeRequest(TreeRequest),
    DocumentRequest(DocumentRequest),
    CommandInvoked(CommandInvoked),
    ToolCompleted(ToolCompleted),
    WorkspaceChanged(WorkspaceChanged),
    Cancel(Cancel),
    VisibilityChanged(VisibilityChanged),
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct PublishDocument {
    pub resource_id: String,
    pub title: String,
    pub revision: u64,
    pub sections: Vec<DocumentSection>,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct EventMessage {
    pub context: OperationContext,
    pub event: Event,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(
    tag = "type",
    content = "data",
    rename_all = "camelCase",
    deny_unknown_fields
)]
pub enum Effect {
    StartToolRead(StartToolRead),
    PublishTreePage(PublishTreePage),
    PublishDocument(PublishDocument),
    CompleteCommand(CompleteCommand),
    InvalidateTree(InvalidateTree),
    OpenDocument(OpenDocument),
    ReleaseResource(ReleaseResource),
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct EffectsMessage {
    pub context: OperationContext,
    pub effects: Vec<Effect>,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(
    tag = "type",
    content = "data",
    rename_all = "camelCase",
    deny_unknown_fields
)]
pub enum Message {
    Hello(Hello),
    Activate(Activate),
    Event(EventMessage),
    Effects(EffectsMessage),
    Deactivate(Deactivate),
    Ack(Ack),
    Rejected(Rejected),
    Stopped(Stopped),
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct Envelope {
    pub protocol: u64,
    pub sequence: u64,
    pub session_generation: u64,
    pub body: Message,
}

fn id(value: &str, empty: bool) -> bool {
    (empty || !value.is_empty())
        && value.len() <= 256
        && value
            .bytes()
            .all(|c| c.is_ascii_alphanumeric() || b".-_:/".contains(&c))
}
fn unique<'a>(values: impl Iterator<Item = &'a String>) -> bool {
    let mut seen = std::collections::HashSet::new();
    values.into_iter().all(|x| seen.insert(x))
}
#[derive(Default)]
struct Budget {
    nodes: usize,
}
impl Budget {
    fn node(&mut self) -> bool {
        self.nodes += 1;
        self.nodes <= 65536
    }
}

trait Check {
    fn check(&self, budget: &mut Budget) -> bool;
}
impl Check for String {
    fn check(&self, budget: &mut Budget) -> bool {
        budget.node() && self.len() <= 262144
    }
}
impl Check for u64 {
    fn check(&self, budget: &mut Budget) -> bool {
        budget.node() && *self <= MAX_COUNTER
    }
}
impl Check for bool {
    fn check(&self, budget: &mut Budget) -> bool {
        budget.node()
    }
}
impl<T: Check> Check for Vec<T> {
    fn check(&self, budget: &mut Budget) -> bool {
        budget.node() && self.len() <= MAX_ITEMS && self.iter().all(|item| item.check(budget))
    }
}

fn validate(envelope: &Envelope) -> Result<(), &'static str> {
    let mut budget = Budget::default();
    if envelope.check(&mut budget) {
        return Ok(());
    }
    Err(if budget.nodes > 65536 {
        "nodeLimit"
    } else {
        "invalidEnvelope"
    })
}

/// No partially validated value crosses the extension boundary.
pub fn decode(bytes: &[u8]) -> Result<Envelope, &'static str> {
    if bytes.is_empty() || bytes.len() > MAX_FRAME_BYTES {
        return Err("frameLimit");
    }
    let envelope: Envelope = serde_json::from_slice(bytes).map_err(|_| "invalidJson")?;
    validate(&envelope)?;
    Ok(envelope)
}

#[derive(Default)]
struct BoundedOutput {
    bytes: Vec<u8>,
}
impl std::io::Write for BoundedOutput {
    fn write(&mut self, bytes: &[u8]) -> std::io::Result<usize> {
        if bytes.len() > MAX_FRAME_BYTES - self.bytes.len() {
            return Err(std::io::Error::other("frameLimit"));
        }
        self.bytes.extend_from_slice(bytes);
        Ok(bytes.len())
    }
    fn flush(&mut self) -> std::io::Result<()> {
        Ok(())
    }
}

pub fn encode(envelope: &Envelope) -> Result<Vec<u8>, &'static str> {
    validate(envelope)?;
    let mut output = BoundedOutput::default();
    serde_json::to_writer(&mut output, envelope).map_err(|_| "frameLimit")?;
    Ok(output.bytes)
}
impl Check for CollapsibleState {
    fn check(&self, budget: &mut Budget) -> bool {
        if !budget.node() {
            return false;
        }
        true
    }
}
impl Check for CompletionStatus {
    fn check(&self, budget: &mut Budget) -> bool {
        if !budget.node() {
            return false;
        }
        true
    }
}
impl Check for PageStatus {
    fn check(&self, budget: &mut Budget) -> bool {
        if !budget.node() {
            return false;
        }
        true
    }
}
impl Check for TextStatus {
    fn check(&self, budget: &mut Budget) -> bool {
        if !budget.node() {
            return false;
        }
        true
    }
}
impl Check for StopReason {
    fn check(&self, budget: &mut Budget) -> bool {
        if !budget.node() {
            return false;
        }
        true
    }
}
impl Check for RejectionCode {
    fn check(&self, budget: &mut Budget) -> bool {
        if !budget.node() {
            return false;
        }
        true
    }
}
impl Check for OperationContext {
    fn check(&self, budget: &mut Budget) -> bool {
        if !budget.node() {
            return false;
        }
        self.operation_id.check(budget)
            && self.owner_generation.check(budget)
            && self.workspace_revision.check(budget)
            && self.account_generation.check(budget)
            && self.request_generation.check(budget)
            && (id(&self.operation_id, false)
                && self.operation_id.len() <= 96
                && self.owner_generation > 0)
    }
}
impl Check for Field {
    fn check(&self, budget: &mut Budget) -> bool {
        if !budget.node() {
            return false;
        }
        self.name.check(budget)
            && self.value.check(budget)
            && (self.name.len() <= 1024 && self.value.len() <= 4096)
    }
}
impl Check for Remote {
    fn check(&self, budget: &mut Budget) -> bool {
        if !budget.node() {
            return false;
        }
        self.name.check(budget)
            && self.url.check(budget)
            && (self.name.len() <= 256 && self.url.len() <= 4096)
    }
}
impl Check for Repository {
    fn check(&self, budget: &mut Budget) -> bool {
        if !budget.node() {
            return false;
        }
        self.root_id.check(budget)
            && self.branch.check(budget)
            && self.remotes.check(budget)
            && (id(&self.root_id, false) && self.branch.len() <= 1024 && self.remotes.len() <= 16)
    }
}
impl Check for TreeItem {
    fn check(&self, budget: &mut Budget) -> bool {
        if !budget.node() {
            return false;
        }
        self.id.check(budget)
            && self.label.check(budget)
            && self.description.check(budget)
            && self.tooltip.check(budget)
            && self.icon.check(budget)
            && self.collapsible_state.check(budget)
            && self.command_id.check(budget)
            && self.arguments.check(budget)
            && (id(&self.id, false)
                && self.label.len() <= 1024
                && self.description.len() <= 1024
                && self.tooltip.len() <= 4096
                && id(&self.icon, true)
                && id(&self.command_id, true)
                && self.arguments.len() <= 16
                && self.arguments.iter().all(|x| x.len() <= 4096))
    }
}
impl Check for TreeRequest {
    fn check(&self, budget: &mut Budget) -> bool {
        if !budget.node() {
            return false;
        }
        self.view_id.check(budget)
            && self.parent_id.check(budget)
            && self.cursor.check(budget)
            && (id(&self.view_id, false) && id(&self.parent_id, true) && self.cursor.len() <= 2048)
    }
}
impl Check for DocumentRequest {
    fn check(&self, budget: &mut Budget) -> bool {
        if !budget.node() {
            return false;
        }
        self.resource_id.check(budget) && (id(&self.resource_id, false))
    }
}
impl Check for CommandInvoked {
    fn check(&self, budget: &mut Budget) -> bool {
        if !budget.node() {
            return false;
        }
        self.command_id.check(budget)
            && self.arguments.check(budget)
            && (id(&self.command_id, false)
                && self.arguments.len() <= 16
                && self.arguments.iter().all(|x| x.len() <= 4096))
    }
}
impl Check for ToolCompleted {
    fn check(&self, budget: &mut Budget) -> bool {
        if !budget.node() {
            return false;
        }
        self.read_id.check(budget)
            && self.status.check(budget)
            && self.data.check(budget)
            && self.message.check(budget)
            && (id(&self.read_id, false)
                && self.data.len() <= 65536
                && self.message.len() <= 4096
                && (self.status == CompletionStatus::Succeeded || self.data.is_empty()))
    }
}
impl Check for WorkspaceChanged {
    fn check(&self, budget: &mut Budget) -> bool {
        if !budget.node() {
            return false;
        }
        self.repositories.check(budget)
            && (self.repositories.len() <= 32
                && unique(self.repositories.iter().map(|x| &x.root_id)))
    }
}
impl Check for Cancel {
    fn check(&self, budget: &mut Budget) -> bool {
        if !budget.node() {
            return false;
        }
        self.operation_id.check(budget)
            && (id(&self.operation_id, false) && self.operation_id.len() <= 96)
    }
}
impl Check for VisibilityChanged {
    fn check(&self, budget: &mut Budget) -> bool {
        if !budget.node() {
            return false;
        }
        self.view_id.check(budget) && self.visible.check(budget) && (id(&self.view_id, false))
    }
}
impl Check for StartToolRead {
    fn check(&self, budget: &mut Budget) -> bool {
        if !budget.node() {
            return false;
        }
        self.read_id.check(budget)
            && self.tool_id.check(budget)
            && self.operation.check(budget)
            && self.arguments.check(budget)
            && (id(&self.read_id, false)
                && id(&self.tool_id, false)
                && id(&self.operation, false)
                && self.arguments.len() <= 16
                && self.arguments.iter().all(|x| id(&x.name, false))
                && unique(self.arguments.iter().map(|x| &x.name)))
    }
}
impl Check for PublishTreePage {
    fn check(&self, budget: &mut Budget) -> bool {
        if !budget.node() {
            return false;
        }
        self.view_id.check(budget)
            && self.parent_id.check(budget)
            && self.items.check(budget)
            && self.next_cursor.check(budget)
            && self.revision.check(budget)
            && self.status.check(budget)
            && self.message.check(budget)
            && (id(&self.view_id, false)
                && id(&self.parent_id, true)
                && self.next_cursor.len() <= 2048
                && self.message.len() <= 4096
                && ((self.status == PageStatus::Partial) == !self.next_cursor.is_empty())
                && (!matches!(self.status, PageStatus::Empty | PageStatus::Failed)
                    || self.items.is_empty())
                && self.items.iter().all(|x| x.id != self.parent_id)
                && unique(self.items.iter().map(|x| &x.id)))
    }
}
impl Check for MarkdownSection {
    fn check(&self, budget: &mut Budget) -> bool {
        if !budget.node() {
            return false;
        }
        self.text.check(budget)
    }
}
impl Check for MetadataSection {
    fn check(&self, budget: &mut Budget) -> bool {
        if !budget.node() {
            return false;
        }
        self.fields.check(budget) && (self.fields.len() <= 64)
    }
}
impl Check for TableRow {
    fn check(&self, budget: &mut Budget) -> bool {
        if !budget.node() {
            return false;
        }
        self.cells.check(budget)
            && (self.cells.len() <= 16 && self.cells.iter().all(|x| x.len() <= 4096))
    }
}
impl Check for TableSection {
    fn check(&self, budget: &mut Budget) -> bool {
        if !budget.node() {
            return false;
        }
        self.columns.check(budget)
            && self.rows.check(budget)
            && (!self.columns.is_empty()
                && self.columns.len() <= 16
                && self.columns.iter().all(|x| x.len() <= 1024)
                && self
                    .rows
                    .iter()
                    .all(|x| x.cells.len() == self.columns.len()))
    }
}
impl Check for TextResourceSection {
    fn check(&self, budget: &mut Budget) -> bool {
        if !budget.node() {
            return false;
        }
        self.handle.check(budget)
            && self.length.check(budget)
            && self.status.check(budget)
            && (id(&self.handle, false) && self.length <= 32 * 1024 * 1024)
    }
}
impl Check for PublishDocument {
    fn check(&self, budget: &mut Budget) -> bool {
        if !budget.node() {
            return false;
        }
        self.resource_id.check(budget)
            && self.title.check(budget)
            && self.revision.check(budget)
            && self.sections.check(budget)
            && (id(&self.resource_id, false)
                && self.title.len() <= 1024
                && self.sections.len() <= 32)
    }
}
impl Check for CompleteCommand {
    fn check(&self, budget: &mut Budget) -> bool {
        if !budget.node() {
            return false;
        }
        self.status.check(budget) && self.message.check(budget) && (self.message.len() <= 4096)
    }
}
impl Check for InvalidateTree {
    fn check(&self, budget: &mut Budget) -> bool {
        if !budget.node() {
            return false;
        }
        self.view_id.check(budget) && (id(&self.view_id, false))
    }
}
impl Check for OpenDocument {
    fn check(&self, budget: &mut Budget) -> bool {
        if !budget.node() {
            return false;
        }
        self.resource_id.check(budget) && (id(&self.resource_id, false))
    }
}
impl Check for ReleaseResource {
    fn check(&self, budget: &mut Budget) -> bool {
        if !budget.node() {
            return false;
        }
        self.handle.check(budget) && (id(&self.handle, false))
    }
}
impl Check for Hello {
    fn check(&self, budget: &mut Budget) -> bool {
        if !budget.node() {
            return false;
        }
        self.abi.check(budget) && (self.abi == ABI)
    }
}
impl Check for Activate {
    fn check(&self, budget: &mut Budget) -> bool {
        if !budget.node() {
            return false;
        }
        self.context.check(budget)
            && self.extension_id.check(budget)
            && (id(&self.extension_id, false))
    }
}
impl Check for EventMessage {
    fn check(&self, budget: &mut Budget) -> bool {
        if !budget.node() {
            return false;
        }
        self.context.check(budget) && self.event.check(budget)
    }
}
impl Check for EffectsMessage {
    fn check(&self, budget: &mut Budget) -> bool {
        if !budget.node() {
            return false;
        }
        self.context.check(budget)
            && self.effects.check(budget)
            && (self.effects.len() <= MAX_EFFECTS
                && unique(self.effects.iter().filter_map(|x| {
                    if let Effect::StartToolRead(read) = x {
                        Some(&read.read_id)
                    } else {
                        None
                    }
                }))
                && self
                    .effects
                    .iter()
                    .filter(|x| matches!(x, Effect::CompleteCommand(_)))
                    .count()
                    <= 1)
    }
}
impl Check for Deactivate {
    fn check(&self, budget: &mut Budget) -> bool {
        if !budget.node() {
            return false;
        }
        self.reason.check(budget)
    }
}
impl Check for Ack {
    fn check(&self, budget: &mut Budget) -> bool {
        if !budget.node() {
            return false;
        }
        self.ack_sequence.check(budget) && (self.ack_sequence > 0)
    }
}
impl Check for Rejected {
    fn check(&self, budget: &mut Budget) -> bool {
        if !budget.node() {
            return false;
        }
        self.request_sequence.check(budget)
            && self.code.check(budget)
            && (self.request_sequence > 0)
    }
}
impl Check for Stopped {
    fn check(&self, budget: &mut Budget) -> bool {
        if !budget.node() {
            return false;
        }
        self.reason.check(budget)
    }
}
impl Check for Envelope {
    fn check(&self, budget: &mut Budget) -> bool {
        if !budget.node() {
            return false;
        }
        self.protocol.check(budget)
            && self.sequence.check(budget)
            && self.session_generation.check(budget)
            && self.body.check(budget)
            && (self.protocol == 2 && self.sequence > 0 && self.session_generation > 0)
    }
}
impl Check for DocumentSection {
    fn check(&self, budget: &mut Budget) -> bool {
        if !budget.node() {
            return false;
        }
        if !budget.node() {
            return false;
        }
        match self {
            Self::Markdown(v) => v.check(budget),
            Self::Metadata(v) => v.check(budget),
            Self::Table(v) => v.check(budget),
            Self::TextResource(v) => v.check(budget),
        }
    }
}
impl Check for Event {
    fn check(&self, budget: &mut Budget) -> bool {
        if !budget.node() {
            return false;
        }
        if !budget.node() {
            return false;
        }
        match self {
            Self::TreeRequest(v) => v.check(budget),
            Self::DocumentRequest(v) => v.check(budget),
            Self::CommandInvoked(v) => v.check(budget),
            Self::ToolCompleted(v) => v.check(budget),
            Self::WorkspaceChanged(v) => v.check(budget),
            Self::Cancel(v) => v.check(budget),
            Self::VisibilityChanged(v) => v.check(budget),
        }
    }
}
impl Check for Effect {
    fn check(&self, budget: &mut Budget) -> bool {
        if !budget.node() {
            return false;
        }
        if !budget.node() {
            return false;
        }
        match self {
            Self::StartToolRead(v) => v.check(budget),
            Self::PublishTreePage(v) => v.check(budget),
            Self::PublishDocument(v) => v.check(budget),
            Self::CompleteCommand(v) => v.check(budget),
            Self::InvalidateTree(v) => v.check(budget),
            Self::OpenDocument(v) => v.check(budget),
            Self::ReleaseResource(v) => v.check(budget),
        }
    }
}
impl Check for Message {
    fn check(&self, budget: &mut Budget) -> bool {
        if !budget.node() {
            return false;
        }
        if !budget.node() {
            return false;
        }
        match self {
            Self::Hello(v) => v.check(budget),
            Self::Activate(v) => v.check(budget),
            Self::Event(v) => v.check(budget),
            Self::Effects(v) => v.check(budget),
            Self::Deactivate(v) => v.check(budget),
            Self::Ack(v) => v.check(budget),
            Self::Rejected(v) => v.check(budget),
            Self::Stopped(v) => v.check(budget),
        }
    }
}

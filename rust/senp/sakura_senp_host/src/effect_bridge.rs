//! Exhaustive conversions at the WIT/IPC boundary. Keep semantic validation in
//! effect_protocol; the session validates before dispatch and after each call.
use crate::bindings_v2::exports::sakura::senp::event_effects as wit;
use crate::effect_protocol as wire;

impl From<&wire::CollapsibleState> for wit::CollapsibleState {
    fn from(value: &wire::CollapsibleState) -> Self {
        match value {
            wire::CollapsibleState::Leaf => Self::Leaf,
            wire::CollapsibleState::Collapsed => Self::Collapsed,
            wire::CollapsibleState::Expanded => Self::Expanded,
        }
    }
}

impl From<&wire::CompletionStatus> for wit::CompletionStatus {
    fn from(value: &wire::CompletionStatus) -> Self {
        match value {
            wire::CompletionStatus::Succeeded => Self::Succeeded,
            wire::CompletionStatus::Cancelled => Self::Cancelled,
            wire::CompletionStatus::Failed => Self::Failed,
            wire::CompletionStatus::TimedOut => Self::TimedOut,
            wire::CompletionStatus::HostUnavailable => Self::HostUnavailable,
        }
    }
}

impl From<&wire::PageStatus> for wit::PageStatus {
    fn from(value: &wire::PageStatus) -> Self {
        match value {
            wire::PageStatus::Complete => Self::Complete,
            wire::PageStatus::Partial => Self::Partial,
            wire::PageStatus::Empty => Self::Empty,
            wire::PageStatus::Failed => Self::Failed,
        }
    }
}

impl From<&wire::TextStatus> for wit::TextStatus {
    fn from(value: &wire::TextStatus) -> Self {
        match value {
            wire::TextStatus::Loading => Self::Loading,
            wire::TextStatus::Complete => Self::Complete,
            wire::TextStatus::Partial => Self::Partial,
            wire::TextStatus::Expired => Self::Expired,
            wire::TextStatus::Failed => Self::Failed,
        }
    }
}

impl From<&wire::StopReason> for wit::StopReason {
    fn from(value: &wire::StopReason) -> Self {
        match value {
            wire::StopReason::Disabled => Self::Disabled,
            wire::StopReason::Updated => Self::Updated,
            wire::StopReason::Shutdown => Self::Shutdown,
            wire::StopReason::ProtocolError => Self::ProtocolError,
            wire::StopReason::HostUnavailable => Self::HostUnavailable,
        }
    }
}

impl From<&wire::OperationContext> for wit::OperationContext {
    fn from(value: &wire::OperationContext) -> Self {
        Self {
            operation_id: value.operation_id.clone(),
            owner_generation: value.owner_generation,
            workspace_revision: value.workspace_revision,
            account_generation: value.account_generation,
            request_generation: value.request_generation,
        }
    }
}

impl From<&wire::Field> for wit::Field {
    fn from(value: &wire::Field) -> Self {
        Self {
            name: value.name.clone(),
            value: value.value.clone(),
        }
    }
}

impl From<&wire::Remote> for wit::Remote {
    fn from(value: &wire::Remote) -> Self {
        Self {
            name: value.name.clone(),
            url: value.url.clone(),
        }
    }
}

impl From<&wire::Repository> for wit::Repository {
    fn from(value: &wire::Repository) -> Self {
        Self {
            root_id: value.root_id.clone(),
            branch: value.branch.clone(),
            // `Check` bounds the wire value to u32 before any conversion.
            ahead: u32::try_from(value.ahead).unwrap_or(u32::MAX),
            remotes: value.remotes.iter().map(Into::into).collect(),
        }
    }
}

impl From<&wire::TreeItem> for wit::TreeItem {
    fn from(value: &wire::TreeItem) -> Self {
        Self {
            id: value.id.clone(),
            label: value.label.clone(),
            description: value.description.clone(),
            tooltip: value.tooltip.clone(),
            icon: value.icon.clone(),
            collapsible_state: (&value.collapsible_state).into(),
            command_id: value.command_id.clone(),
            arguments: value.arguments.clone(),
            context_value: value.context_value.clone(),
        }
    }
}

impl From<&wire::TreeRequest> for wit::TreeRequest {
    fn from(value: &wire::TreeRequest) -> Self {
        Self {
            view_id: value.view_id.clone(),
            parent_id: value.parent_id.clone(),
            cursor: value.cursor.clone(),
        }
    }
}

impl From<&wire::DocumentRequest> for wit::DocumentRequest {
    fn from(value: &wire::DocumentRequest) -> Self {
        Self {
            resource_id: value.resource_id.clone(),
        }
    }
}

impl From<&wire::CommandInvoked> for wit::CommandInvoked {
    fn from(value: &wire::CommandInvoked) -> Self {
        Self {
            command_id: value.command_id.clone(),
            arguments: value.arguments.clone(),
        }
    }
}

impl From<&wire::ToolCompleted> for wit::ToolCompleted {
    fn from(value: &wire::ToolCompleted) -> Self {
        Self {
            read_id: value.read_id.clone(),
            status: (&value.status).into(),
            data: value.data.clone(),
            message: value.message.clone(),
        }
    }
}

impl From<&wire::WorkspaceChanged> for wit::WorkspaceChanged {
    fn from(value: &wire::WorkspaceChanged) -> Self {
        Self {
            repositories: value.repositories.iter().map(Into::into).collect(),
        }
    }
}

impl From<&wire::Cancel> for wit::Cancel {
    fn from(value: &wire::Cancel) -> Self {
        Self {
            operation_id: value.operation_id.clone(),
        }
    }
}

impl From<&wire::VisibilityChanged> for wit::VisibilityChanged {
    fn from(value: &wire::VisibilityChanged) -> Self {
        Self {
            view_id: value.view_id.clone(),
            visible: value.visible,
        }
    }
}

impl From<&wire::StartToolRead> for wit::StartToolRead {
    fn from(value: &wire::StartToolRead) -> Self {
        Self {
            read_id: value.read_id.clone(),
            tool_id: value.tool_id.clone(),
            operation: value.operation.clone(),
            arguments: value.arguments.iter().map(Into::into).collect(),
        }
    }
}

impl From<&wire::PublishTreePage> for wit::PublishTreePage {
    fn from(value: &wire::PublishTreePage) -> Self {
        Self {
            view_id: value.view_id.clone(),
            parent_id: value.parent_id.clone(),
            items: value.items.iter().map(Into::into).collect(),
            next_cursor: value.next_cursor.clone(),
            revision: value.revision,
            status: (&value.status).into(),
            message: value.message.clone(),
        }
    }
}

impl From<&wire::MarkdownSection> for wit::MarkdownSection {
    fn from(value: &wire::MarkdownSection) -> Self {
        Self {
            text: value.text.clone(),
        }
    }
}

impl From<&wire::MetadataSection> for wit::MetadataSection {
    fn from(value: &wire::MetadataSection) -> Self {
        Self {
            fields: value.fields.iter().map(Into::into).collect(),
        }
    }
}

impl From<&wire::TableRow> for wit::TableRow {
    fn from(value: &wire::TableRow) -> Self {
        Self {
            cells: value.cells.clone(),
        }
    }
}

impl From<&wire::TableSection> for wit::TableSection {
    fn from(value: &wire::TableSection) -> Self {
        Self {
            columns: value.columns.clone(),
            rows: value.rows.iter().map(Into::into).collect(),
        }
    }
}

impl From<&wire::TextResourceSection> for wit::TextResourceSection {
    fn from(value: &wire::TextResourceSection) -> Self {
        Self {
            handle: value.handle.clone(),
            length: value.length,
            status: (&value.status).into(),
        }
    }
}

impl From<&wire::PublishDocument> for wit::PublishDocument {
    fn from(value: &wire::PublishDocument) -> Self {
        Self {
            resource_id: value.resource_id.clone(),
            title: value.title.clone(),
            revision: value.revision,
            sections: value.sections.iter().map(Into::into).collect(),
        }
    }
}

impl From<&wire::CompleteCommand> for wit::CompleteCommand {
    fn from(value: &wire::CompleteCommand) -> Self {
        Self {
            status: (&value.status).into(),
            message: value.message.clone(),
        }
    }
}

impl From<&wire::InvalidateTree> for wit::InvalidateTree {
    fn from(value: &wire::InvalidateTree) -> Self {
        Self {
            view_id: value.view_id.clone(),
        }
    }
}

impl From<&wire::OpenDocument> for wit::OpenDocument {
    fn from(value: &wire::OpenDocument) -> Self {
        Self {
            resource_id: value.resource_id.clone(),
        }
    }
}

impl From<&wire::ReleaseResource> for wit::ReleaseResource {
    fn from(value: &wire::ReleaseResource) -> Self {
        Self {
            handle: value.handle.clone(),
        }
    }
}

impl From<&wire::Activate> for wit::Activation {
    fn from(value: &wire::Activate) -> Self {
        Self {
            context: (&value.context).into(),
            extension_id: value.extension_id.clone(),
        }
    }
}

impl From<&wire::DocumentSection> for wit::DocumentSection {
    fn from(value: &wire::DocumentSection) -> Self {
        match value {
            wire::DocumentSection::Markdown(value) => Self::Markdown(value.into()),
            wire::DocumentSection::Metadata(value) => Self::Metadata(value.into()),
            wire::DocumentSection::Table(value) => Self::Table(value.into()),
            wire::DocumentSection::TextResource(value) => Self::TextResource(value.into()),
        }
    }
}

impl From<&wire::Event> for wit::Event {
    fn from(value: &wire::Event) -> Self {
        match value {
            wire::Event::TreeRequest(value) => Self::TreeRequest(value.into()),
            wire::Event::DocumentRequest(value) => Self::DocumentRequest(value.into()),
            wire::Event::CommandInvoked(value) => Self::CommandInvoked(value.into()),
            wire::Event::ToolCompleted(value) => Self::ToolCompleted(value.into()),
            wire::Event::WorkspaceChanged(value) => Self::WorkspaceChanged(value.into()),
            wire::Event::Cancel(value) => Self::Cancel(value.into()),
            wire::Event::VisibilityChanged(value) => Self::VisibilityChanged(value.into()),
        }
    }
}

impl From<&wire::Effect> for wit::Effect {
    fn from(value: &wire::Effect) -> Self {
        match value {
            wire::Effect::StartToolRead(value) => Self::StartToolRead(value.into()),
            wire::Effect::PublishTreePage(value) => Self::PublishTreePage(value.into()),
            wire::Effect::PublishDocument(value) => Self::PublishDocument(value.into()),
            wire::Effect::CompleteCommand(value) => Self::CompleteCommand(value.into()),
            wire::Effect::InvalidateTree(value) => Self::InvalidateTree(value.into()),
            wire::Effect::OpenDocument(value) => Self::OpenDocument(value.into()),
            wire::Effect::ReleaseResource(value) => Self::ReleaseResource(value.into()),
        }
    }
}

impl From<wit::CollapsibleState> for wire::CollapsibleState {
    fn from(value: wit::CollapsibleState) -> Self {
        match value {
            wit::CollapsibleState::Leaf => Self::Leaf,
            wit::CollapsibleState::Collapsed => Self::Collapsed,
            wit::CollapsibleState::Expanded => Self::Expanded,
        }
    }
}

impl From<wit::CompletionStatus> for wire::CompletionStatus {
    fn from(value: wit::CompletionStatus) -> Self {
        match value {
            wit::CompletionStatus::Succeeded => Self::Succeeded,
            wit::CompletionStatus::Cancelled => Self::Cancelled,
            wit::CompletionStatus::Failed => Self::Failed,
            wit::CompletionStatus::TimedOut => Self::TimedOut,
            wit::CompletionStatus::HostUnavailable => Self::HostUnavailable,
        }
    }
}

impl From<wit::PageStatus> for wire::PageStatus {
    fn from(value: wit::PageStatus) -> Self {
        match value {
            wit::PageStatus::Complete => Self::Complete,
            wit::PageStatus::Partial => Self::Partial,
            wit::PageStatus::Empty => Self::Empty,
            wit::PageStatus::Failed => Self::Failed,
        }
    }
}

impl From<wit::TextStatus> for wire::TextStatus {
    fn from(value: wit::TextStatus) -> Self {
        match value {
            wit::TextStatus::Loading => Self::Loading,
            wit::TextStatus::Complete => Self::Complete,
            wit::TextStatus::Partial => Self::Partial,
            wit::TextStatus::Expired => Self::Expired,
            wit::TextStatus::Failed => Self::Failed,
        }
    }
}

impl From<wit::StopReason> for wire::StopReason {
    fn from(value: wit::StopReason) -> Self {
        match value {
            wit::StopReason::Disabled => Self::Disabled,
            wit::StopReason::Updated => Self::Updated,
            wit::StopReason::Shutdown => Self::Shutdown,
            wit::StopReason::ProtocolError => Self::ProtocolError,
            wit::StopReason::HostUnavailable => Self::HostUnavailable,
        }
    }
}

impl From<wit::OperationContext> for wire::OperationContext {
    fn from(value: wit::OperationContext) -> Self {
        Self {
            operation_id: value.operation_id,
            owner_generation: value.owner_generation,
            workspace_revision: value.workspace_revision,
            account_generation: value.account_generation,
            request_generation: value.request_generation,
        }
    }
}

impl From<wit::Field> for wire::Field {
    fn from(value: wit::Field) -> Self {
        Self {
            name: value.name,
            value: value.value,
        }
    }
}

impl From<wit::Remote> for wire::Remote {
    fn from(value: wit::Remote) -> Self {
        Self {
            name: value.name,
            url: value.url,
        }
    }
}

impl From<wit::Repository> for wire::Repository {
    fn from(value: wit::Repository) -> Self {
        Self {
            root_id: value.root_id,
            branch: value.branch,
            ahead: value.ahead.into(),
            remotes: value.remotes.into_iter().map(Into::into).collect(),
        }
    }
}

impl From<wit::TreeItem> for wire::TreeItem {
    fn from(value: wit::TreeItem) -> Self {
        Self {
            id: value.id,
            label: value.label,
            description: value.description,
            tooltip: value.tooltip,
            icon: value.icon,
            collapsible_state: value.collapsible_state.into(),
            command_id: value.command_id,
            arguments: value.arguments,
            context_value: value.context_value,
        }
    }
}

impl From<wit::TreeRequest> for wire::TreeRequest {
    fn from(value: wit::TreeRequest) -> Self {
        Self {
            view_id: value.view_id,
            parent_id: value.parent_id,
            cursor: value.cursor,
        }
    }
}

impl From<wit::DocumentRequest> for wire::DocumentRequest {
    fn from(value: wit::DocumentRequest) -> Self {
        Self {
            resource_id: value.resource_id,
        }
    }
}

impl From<wit::CommandInvoked> for wire::CommandInvoked {
    fn from(value: wit::CommandInvoked) -> Self {
        Self {
            command_id: value.command_id,
            arguments: value.arguments,
        }
    }
}

impl From<wit::ToolCompleted> for wire::ToolCompleted {
    fn from(value: wit::ToolCompleted) -> Self {
        Self {
            read_id: value.read_id,
            status: value.status.into(),
            data: value.data,
            message: value.message,
        }
    }
}

impl From<wit::WorkspaceChanged> for wire::WorkspaceChanged {
    fn from(value: wit::WorkspaceChanged) -> Self {
        Self {
            repositories: value.repositories.into_iter().map(Into::into).collect(),
        }
    }
}

impl From<wit::Cancel> for wire::Cancel {
    fn from(value: wit::Cancel) -> Self {
        Self {
            operation_id: value.operation_id,
        }
    }
}

impl From<wit::VisibilityChanged> for wire::VisibilityChanged {
    fn from(value: wit::VisibilityChanged) -> Self {
        Self {
            view_id: value.view_id,
            visible: value.visible,
        }
    }
}

impl From<wit::StartToolRead> for wire::StartToolRead {
    fn from(value: wit::StartToolRead) -> Self {
        Self {
            read_id: value.read_id,
            tool_id: value.tool_id,
            operation: value.operation,
            arguments: value.arguments.into_iter().map(Into::into).collect(),
        }
    }
}

impl From<wit::PublishTreePage> for wire::PublishTreePage {
    fn from(value: wit::PublishTreePage) -> Self {
        Self {
            view_id: value.view_id,
            parent_id: value.parent_id,
            items: value.items.into_iter().map(Into::into).collect(),
            next_cursor: value.next_cursor,
            revision: value.revision,
            status: value.status.into(),
            message: value.message,
        }
    }
}

impl From<wit::MarkdownSection> for wire::MarkdownSection {
    fn from(value: wit::MarkdownSection) -> Self {
        Self { text: value.text }
    }
}

impl From<wit::MetadataSection> for wire::MetadataSection {
    fn from(value: wit::MetadataSection) -> Self {
        Self {
            fields: value.fields.into_iter().map(Into::into).collect(),
        }
    }
}

impl From<wit::TableRow> for wire::TableRow {
    fn from(value: wit::TableRow) -> Self {
        Self { cells: value.cells }
    }
}

impl From<wit::TableSection> for wire::TableSection {
    fn from(value: wit::TableSection) -> Self {
        Self {
            columns: value.columns,
            rows: value.rows.into_iter().map(Into::into).collect(),
        }
    }
}

impl From<wit::TextResourceSection> for wire::TextResourceSection {
    fn from(value: wit::TextResourceSection) -> Self {
        Self {
            handle: value.handle,
            length: value.length,
            status: value.status.into(),
        }
    }
}

impl From<wit::PublishDocument> for wire::PublishDocument {
    fn from(value: wit::PublishDocument) -> Self {
        Self {
            resource_id: value.resource_id,
            title: value.title,
            revision: value.revision,
            sections: value.sections.into_iter().map(Into::into).collect(),
        }
    }
}

impl From<wit::CompleteCommand> for wire::CompleteCommand {
    fn from(value: wit::CompleteCommand) -> Self {
        Self {
            status: value.status.into(),
            message: value.message,
        }
    }
}

impl From<wit::InvalidateTree> for wire::InvalidateTree {
    fn from(value: wit::InvalidateTree) -> Self {
        Self {
            view_id: value.view_id,
        }
    }
}

impl From<wit::OpenDocument> for wire::OpenDocument {
    fn from(value: wit::OpenDocument) -> Self {
        Self {
            resource_id: value.resource_id,
        }
    }
}

impl From<wit::ReleaseResource> for wire::ReleaseResource {
    fn from(value: wit::ReleaseResource) -> Self {
        Self {
            handle: value.handle,
        }
    }
}

impl From<wit::Activation> for wire::Activate {
    fn from(value: wit::Activation) -> Self {
        Self {
            context: value.context.into(),
            extension_id: value.extension_id,
        }
    }
}

impl From<wit::DocumentSection> for wire::DocumentSection {
    fn from(value: wit::DocumentSection) -> Self {
        match value {
            wit::DocumentSection::Markdown(value) => Self::Markdown(value.into()),
            wit::DocumentSection::Metadata(value) => Self::Metadata(value.into()),
            wit::DocumentSection::Table(value) => Self::Table(value.into()),
            wit::DocumentSection::TextResource(value) => Self::TextResource(value.into()),
        }
    }
}

impl From<wit::Event> for wire::Event {
    fn from(value: wit::Event) -> Self {
        match value {
            wit::Event::TreeRequest(value) => Self::TreeRequest(value.into()),
            wit::Event::DocumentRequest(value) => Self::DocumentRequest(value.into()),
            wit::Event::CommandInvoked(value) => Self::CommandInvoked(value.into()),
            wit::Event::ToolCompleted(value) => Self::ToolCompleted(value.into()),
            wit::Event::WorkspaceChanged(value) => Self::WorkspaceChanged(value.into()),
            wit::Event::Cancel(value) => Self::Cancel(value.into()),
            wit::Event::VisibilityChanged(value) => Self::VisibilityChanged(value.into()),
        }
    }
}

impl From<wit::Effect> for wire::Effect {
    fn from(value: wit::Effect) -> Self {
        match value {
            wit::Effect::StartToolRead(value) => Self::StartToolRead(value.into()),
            wit::Effect::PublishTreePage(value) => Self::PublishTreePage(value.into()),
            wit::Effect::PublishDocument(value) => Self::PublishDocument(value.into()),
            wit::Effect::CompleteCommand(value) => Self::CompleteCommand(value.into()),
            wit::Effect::InvalidateTree(value) => Self::InvalidateTree(value.into()),
            wit::Effect::OpenDocument(value) => Self::OpenDocument(value.into()),
            wit::Effect::ReleaseResource(value) => Self::ReleaseResource(value.into()),
        }
    }
}

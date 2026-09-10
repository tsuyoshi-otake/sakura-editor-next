//! Read-only GitHub Actions workflow, run and attempt projection.

use sakura_senp_github_client::actions::{parse_run, parse_runs, parse_workflows, Run, Workflow};
use std::cell::RefCell;

mod jobs;
mod status;

wit_bindgen::generate!({ path: "../../wit/v2/senp-extension.wit", world: "extension" });
use exports::sakura::senp::event_effects::*;

const WORKFLOWS: &str = "github-actions.workflows";
const BRANCH: &str = "github-actions.current-branch";
const OPEN_RUN: &str = "github-actions.workflow.run.open";
/// Upstream's `view/title` refresh commands. They are the only way to re-read a
/// tree that nothing else invalidated; there is no polling behind them.
const REFRESH: &str = "github-actions.explorer.refresh";
const REFRESH_BRANCH: &str = "github-actions.explorer.current-branch.refresh";
const PAGE_SIZE: u32 = 20;

struct GithubActions;

thread_local! {
    static STATE: RefCell<State> = RefCell::new(State::default());
}

impl Guest for GithubActions {
    fn activate(_: Activation) -> Vec<Effect> {
        STATE.with(|state| *state.borrow_mut() = State::default());
        invalidations()
    }

    fn on_event(context: OperationContext, event: Event) -> Vec<Effect> {
        STATE.with(|state| state.borrow_mut().dispatch(context, event))
    }

    fn deactivate(_: StopReason) {
        STATE.with(|state| *state.borrow_mut() = State::default());
    }
}

#[derive(Default, PartialEq)]
enum BranchState {
    #[default]
    Unavailable,
    Detached,
    Selected(String),
}

/// The part of a workspace snapshot that decides which repository both views
/// read: each root and its remotes. The branch is tracked apart from it.
type RepositoryIdentity = Vec<(String, Vec<(String, String)>)>;

#[derive(Default)]
struct State {
    scope: Option<(u64, u64, u64)>,
    repositories: Option<RepositoryIdentity>,
    branch: BranchState,
    /// The one repository's commits ahead of its upstream, as last observed.
    ahead: u32,
}

impl State {
    fn dispatch(&mut self, context: OperationContext, event: Event) -> Vec<Effect> {
        let scope = (
            context.owner_generation,
            context.workspace_revision,
            context.account_generation,
        );
        if self.scope != Some(scope) {
            self.scope = Some(scope);
            self.repositories = None;
            self.branch = BranchState::Unavailable;
            self.ahead = 0;
        }
        match event {
            Event::WorkspaceChanged(change) => {
                // Upstream refreshes the current-branch tree when the Git
                // extension reports a new HEAD, and rebuilds everything only when
                // the repository itself changes. Mirror that split instead of
                // re-reading the workflows on every checkout.
                let repositories: RepositoryIdentity = change
                    .repositories
                    .iter()
                    .map(|repository| {
                        (
                            repository.root_id.clone(),
                            repository
                                .remotes
                                .iter()
                                .map(|remote| (remote.name.clone(), remote.url.clone()))
                                .collect(),
                        )
                    })
                    .collect();
                let branch = match change.repositories.as_slice() {
                    [repository] if repository.branch.is_empty() => BranchState::Detached,
                    [repository]
                        if repository.branch.len() <= 1024 && !repository.branch.contains('\0') =>
                    {
                        BranchState::Selected(repository.branch.clone())
                    }
                    _ => BranchState::Unavailable,
                };
                // Upstream reads a drop in HEAD's ahead count as a push. It records
                // the count only when it refreshes, so the usual commit-then-push
                // (0 -> 1 -> 0) never compares lower; every observation counts here.
                let ahead = match change.repositories.as_slice() {
                    [repository] => repository.ahead,
                    _ => 0,
                };
                let pushed = ahead < self.ahead;
                self.ahead = ahead;
                if self.repositories.as_ref() != Some(&repositories) {
                    self.repositories = Some(repositories);
                    self.branch = branch;
                    invalidations()
                } else if self.branch != branch || pushed {
                    self.branch = branch;
                    vec![invalidate(BRANCH)]
                } else {
                    Vec::new()
                }
            }
            Event::CommandInvoked(command)
                if command.command_id == REFRESH || command.command_id == REFRESH_BRANCH =>
            {
                if !command.arguments.is_empty() {
                    return vec![Effect::CompleteCommand(CompleteCommand {
                        status: CompletionStatus::Failed,
                        message: "Refresh takes no arguments".into(),
                    })];
                }
                let view = if command.command_id == REFRESH { WORKFLOWS } else { BRANCH };
                vec![
                    invalidate(view),
                    Effect::CompleteCommand(CompleteCommand {
                        status: CompletionStatus::Succeeded,
                        message: String::new(),
                    }),
                ]
            }
            Event::TreeRequest(request) if view_code(&request.view_id).is_some() => {
                vec![self.tree_request(request)]
            }
            Event::ToolCompleted(read) => vec![self.complete(read)],
            Event::CommandInvoked(command)
                if command.command_id == OPEN_RUN
                    || command.command_id == jobs::OPEN_JOB
                    || command.command_id == jobs::VIEW_LOGS =>
            {
                if let [argument] = command.arguments.as_slice() {
                    let resource = if command.command_id == OPEN_RUN {
                        document_identity(argument).map(|_| argument.clone())
                    } else {
                        jobs::opened(&command.command_id, argument)
                    };
                    if let Some(resource_id) = resource {
                        return vec![
                            Effect::OpenDocument(OpenDocument { resource_id }),
                            Effect::CompleteCommand(CompleteCommand {
                                status: CompletionStatus::Succeeded,
                                message: String::new(),
                            }),
                        ];
                    }
                }
                vec![Effect::CompleteCommand(CompleteCommand {
                    status: CompletionStatus::Failed,
                    message: "Invalid workflow run identity".into(),
                })]
            }
            Event::DocumentRequest(request)
                if jobs::document_identity(&request.resource_id).is_some() =>
            {
                let (identity, kind) = jobs::document_identity(&request.resource_id).unwrap();
                vec![jobs::document_request(identity, kind)]
            }
            Event::DocumentRequest(request) => match document_identity(&request.resource_id) {
                Some((run, attempt)) => vec![read(
                    format!("detail:{run}:{attempt}"),
                    Resource::attempt("runAttempt", run, attempt),
                    Vec::new(),
                )],
                None => vec![failed_document(
                    request.resource_id,
                    "Invalid workflow run identity",
                )],
            },
            _ => Vec::new(),
        }
    }

    fn tree_request(&self, request: TreeRequest) -> Effect {
        if let Some(effect) = jobs::tree_request(&request) {
            return effect;
        }
        let view = request.view_id.as_str();
        let parent = request.parent_id.as_str();
        let fail = |message| failed_page(view, parent, 1, message);
        if parent.is_empty() {
            let Some(page) = page_cursor(&request.cursor) else {
                return fail("Invalid page cursor");
            };
            if view == WORKFLOWS {
                return read(
                    format!("workflows:{page}"),
                    Resource::collection("workflows"),
                    paging(page),
                );
            }
            return match &self.branch {
                BranchState::Selected(branch) => {
                    let mut fields = paging(page);
                    fields.push(field("branch", branch.clone()));
                    read(
                        format!("runs:b:0:{page}"),
                        Resource::collection("runs"),
                        fields,
                    )
                }
                BranchState::Detached => page_effect(
                    view,
                    parent,
                    1,
                    Vec::new(),
                    String::new(),
                    "Detached HEAD; use Workflows to browse runs".into(),
                ),
                BranchState::Unavailable => {
                    fail("Current Branch requires one verified selected repository")
                }
            };
        }
        if view == WORKFLOWS {
            if let Some(workflow) = parent.strip_prefix("workflow:").and_then(positive) {
                let Some(page) = page_cursor(&request.cursor) else {
                    return fail("Invalid workflow run page cursor");
                };
                return read(
                    format!("runs:w:{workflow}:{page}"),
                    Resource::item("workflowRuns", workflow),
                    paging(page),
                );
            }
        }
        fail("Unsupported Actions tree parent")
    }

    fn complete(&self, completion: ToolCompleted) -> Effect {
        if let Some(effect) = jobs::complete(&completion) {
            return effect;
        }
        let parts: Vec<_> = completion.read_id.split(':').collect();
        match parts.as_slice() {
            ["detail", run, attempt] => {
                let (Some(run), Some(attempt)) = (positive(run), positive32(attempt)) else {
                    return failed_page(WORKFLOWS, "", 1, "Invalid workflow detail completion");
                };
                let resource = document_id(run, attempt);
                if completion.status != CompletionStatus::Succeeded {
                    return failed_document(resource, completion_message(&completion));
                }
                match parse_run(&completion.data) {
                    Ok(value) if value.id == run && value.run_attempt == attempt => {
                        run_document(value)
                    }
                    Ok(_) => failed_document(resource, "Mismatched workflow run or attempt"),
                    Err(error) => failed_document(resource, error.to_string()),
                }
            }
            ["workflows", page] => {
                let Some(page) = positive32(page) else {
                    return failed_page(WORKFLOWS, "", 1, "Invalid workflow page completion");
                };
                if completion.status != CompletionStatus::Succeeded {
                    return failed_page(
                        WORKFLOWS,
                        "",
                        u64::from(page),
                        completion_message(&completion),
                    );
                }
                match parse_workflows(&completion.data) {
                    Ok(result) if forward_page(result.next_page, page) => page_effect(
                        WORKFLOWS,
                        "",
                        u64::from(page),
                        result.items.into_iter().map(workflow_item).collect(),
                        next_cursor(result.next_page),
                        String::new(),
                    ),
                    Ok(_) => {
                        failed_page(WORKFLOWS, "", u64::from(page), "Invalid workflow next page")
                    }
                    Err(error) => failed_page(WORKFLOWS, "", u64::from(page), error.to_string()),
                }
            }
            ["runs", code, workflow, page] => {
                let (Some(view), Some(page)) = (code_view(code), positive32(page)) else {
                    return failed_page(WORKFLOWS, "", 1, "Invalid run page completion");
                };
                let workflow = if *code == "b" && *workflow == "0" {
                    0
                } else {
                    let Some(workflow) = positive(workflow) else {
                        return failed_page(view, "", 1, "Invalid workflow identity");
                    };
                    workflow
                };
                let parent = if workflow == 0 {
                    String::new()
                } else {
                    format!("workflow:{workflow}")
                };
                if (*code == "w") != (workflow != 0) {
                    return failed_page(
                        view,
                        &parent,
                        u64::from(page),
                        "Mismatched run page parent",
                    );
                }
                if completion.status != CompletionStatus::Succeeded {
                    return failed_page(
                        view,
                        &parent,
                        u64::from(page),
                        completion_message(&completion),
                    );
                }
                match parse_runs(&completion.data) {
                    Ok(result) if forward_page(result.next_page, page)
                        && (workflow != 0 || matches!(self.branch, BranchState::Selected(_)))
                        && result.items.iter().all(|run| {
                        if workflow != 0 { run.workflow_id == workflow } else {
                            matches!(&self.branch, BranchState::Selected(branch) if run.head_branch.as_ref() == Some(branch))
                        }
                    }) => page_effect(view, &parent, u64::from(page), result.items.into_iter().map(|run| run_item(view, run)).collect(), next_cursor(result.next_page), String::new()),
                    Ok(_) => failed_page(view, &parent, u64::from(page), "Mismatched run filter or next page"),
                    Err(error) => failed_page(view, &parent, u64::from(page), error.to_string()),
                }
            }
            _ => failed_page(WORKFLOWS, "", 1, "Unknown Actions read completion"),
        }
    }
}

fn positive(value: &str) -> Option<u64> {
    if value.is_empty() || !value.bytes().all(|value| value.is_ascii_digit()) {
        return None;
    }
    value.parse().ok().filter(|value| *value > 0)
}

fn positive32(value: &str) -> Option<u32> {
    positive(value).and_then(|value| value.try_into().ok())
}
fn view_code(view: &str) -> Option<&str> {
    match view {
        WORKFLOWS => Some("w"),
        BRANCH => Some("b"),
        _ => None,
    }
}
fn code_view(code: &str) -> Option<&str> {
    match code {
        "w" => Some(WORKFLOWS),
        "b" => Some(BRANCH),
        _ => None,
    }
}
fn field(name: &str, value: String) -> Field {
    Field {
        name: name.into(),
        value,
    }
}
fn paging(page: u32) -> Vec<Field> {
    vec![
        field("per_page", PAGE_SIZE.to_string()),
        field("page", page.to_string()),
    ]
}
fn page_cursor(cursor: &str) -> Option<u32> {
    if cursor.is_empty() {
        Some(1)
    } else {
        cursor.strip_prefix("page:").and_then(positive32)
    }
}
fn forward_page(next: Option<u32>, current: u32) -> bool {
    next.is_none_or(|next| next > current)
}
fn next_cursor(next: Option<u32>) -> String {
    next.map(|next| format!("page:{next}")).unwrap_or_default()
}

fn document_id(run: u64, attempt: u32) -> String {
    format!("github-actions-run:{run}:{attempt}")
}
fn document_identity(resource: &str) -> Option<(u64, u32)> {
    let mut parts = resource.split(':');
    match (parts.next(), parts.next(), parts.next(), parts.next()) {
        (Some("github-actions-run"), Some(run), Some(attempt), None) => {
            Some((positive(run)?, positive32(attempt)?))
        }
        _ => None,
    }
}

fn invalidate(view: &str) -> Effect {
    Effect::InvalidateTree(InvalidateTree {
        view_id: view.into(),
    })
}

fn invalidations() -> Vec<Effect> {
    [BRANCH, WORKFLOWS].into_iter().map(invalidate).collect()
}

/// One shape of the tool boundary's closed set together with the ids that shape
/// is keyed by. The boundary owns every path segment: it refuses an argument
/// list that names a path of its own, so such a read never reaches GitHub.
pub struct Resource {
    shape: &'static str,
    id: Option<u64>,
    attempt: Option<u32>,
}

impl Resource {
    /// A collection under the repository. It carries no id of its own.
    pub fn collection(shape: &'static str) -> Self {
        Self {
            shape,
            id: None,
            attempt: None,
        }
    }
    pub fn item(shape: &'static str, id: u64) -> Self {
        Self {
            shape,
            id: Some(id),
            attempt: None,
        }
    }
    /// A run attempt is named by the run and the attempt number together;
    /// neither number identifies it alone, so both are sent.
    pub fn attempt(shape: &'static str, run: u64, attempt: u32) -> Self {
        Self {
            shape,
            id: Some(run),
            attempt: Some(attempt),
        }
    }

    fn fields(self) -> Vec<Field> {
        let mut fields = vec![field("shape", self.shape.into())];
        if let Some(id) = self.id {
            fields.push(field("id", id.to_string()));
        }
        if let Some(attempt) = self.attempt {
            fields.push(field("attempt", attempt.to_string()));
        }
        fields
    }
}

fn read(read_id: String, resource: Resource, mut arguments: Vec<Field>) -> Effect {
    let mut fields = resource.fields();
    fields.append(&mut arguments);
    Effect::StartToolRead(StartToolRead {
        read_id,
        tool_id: "github".into(),
        operation: "repositoryRead".into(),
        arguments: fields,
    })
}

/// A log is downloaded by its own operation rather than by a repository path.
/// The tool writes it into the editor's text resource store and answers with a
/// handle, so nothing here ever holds the bytes.
fn log_read(read_id: String, job: u64) -> Effect {
    Effect::StartToolRead(StartToolRead {
        read_id,
        tool_id: "github".into(),
        operation: "jobLog".into(),
        arguments: vec![field("id", job.to_string())],
    })
}

fn completion_message(completion: &ToolCompleted) -> &str {
    if completion.message.is_empty() {
        "GitHub Actions read failed"
    } else {
        &completion.message
    }
}

fn failed_page(view: &str, parent: &str, revision: u64, message: impl Into<String>) -> Effect {
    Effect::PublishTreePage(PublishTreePage {
        view_id: view.into(),
        parent_id: parent.into(),
        items: Vec::new(),
        next_cursor: String::new(),
        revision,
        status: PageStatus::Failed,
        message: message.into(),
    })
}

fn page_effect(
    view: &str,
    parent: &str,
    revision: u64,
    items: Vec<TreeItem>,
    next_cursor: String,
    message: String,
) -> Effect {
    let status = if !next_cursor.is_empty() {
        PageStatus::Partial
    } else if items.is_empty() {
        PageStatus::Empty
    } else {
        PageStatus::Complete
    };
    Effect::PublishTreePage(PublishTreePage {
        view_id: view.into(),
        parent_id: parent.into(),
        items,
        next_cursor,
        revision,
        status,
        message,
    })
}

// Upstream's WorkflowNode is its name alone: no icon and no description. Its
// contextValue also carries pin and dispatch tokens for menus this extension
// does not contribute, so only the node kind is sent.
fn workflow_item(workflow: Workflow) -> TreeItem {
    TreeItem {
        id: format!("workflow:{}", workflow.id),
        label: workflow.name,
        description: String::new(),
        tooltip: format!("{}\n{}", workflow.path, workflow.html_url),
        icon: String::new(),
        collapsible_state: CollapsibleState::Collapsed,
        command_id: String::new(),
        arguments: Vec::new(),
        context_value: "workflow".into(),
    }
}

// Upstream's WorkflowRunNode: under a workflow the workflow is already named,
// so a run is its number; on the current branch it is the workflow and number.
// State is the icon and the tooltip rather than a text description.
fn run_item(view: &str, run: Run) -> TreeItem {
    let label = match &run.name {
        Some(name) if view != WORKFLOWS => format!("{name} #{}", run.run_number),
        _ => format!("#{}", run.run_number),
    };
    let attempt = if run.run_attempt > 1 {
        format!("Attempt #{} ", run.run_attempt)
    } else {
        String::new()
    };
    let state = status::status_text(
        &run.status,
        run.conclusion.as_deref(),
        run.run_started_at.as_deref(),
        Some(&run.updated_at),
    );
    // The row names the attempt it was read at: its children are that attempt's
    // jobs, so a rerun has to arrive as a new row rather than re-point this one.
    // Upstream's contextValue adds rerun and cancel permission tokens for menus
    // this extension does not contribute; the kind and "completed" are kept.
    let context_value = if run.status == "completed" {
        "run completed"
    } else {
        "run"
    };
    TreeItem {
        id: format!("run:{}:{}", run.id, run.run_attempt),
        label,
        description: String::new(),
        tooltip: format!(
            "{attempt}{state}\n\n{}",
            status::event_text(&run.event, run.run_attempt)
        ),
        icon: status::run_icon(&run.status, run.conclusion.as_deref()),
        collapsible_state: CollapsibleState::Collapsed,
        command_id: OPEN_RUN.into(),
        arguments: vec![document_id(run.id, run.run_attempt)],
        context_value: context_value.into(),
    }
}

fn run_document(run: Run) -> Effect {
    let fields = vec![
        field(
            "Workflow",
            run.name.clone().unwrap_or_else(|| "unavailable".into()),
        ),
        field("Workflow ID", run.workflow_id.to_string()),
        field("Run ID", run.id.to_string()),
        field("Run number", run.run_number.to_string()),
        field("Attempt", run.run_attempt.to_string()),
        field("State", run.summary()),
        field(
            "Branch",
            run.head_branch.unwrap_or_else(|| "unavailable".into()),
        ),
        field("Commit", run.head_sha),
        field("Event", run.event),
        field("Created", run.created_at),
        field(
            "Started",
            run.run_started_at.unwrap_or_else(|| "not started".into()),
        ),
        field("Updated", run.updated_at),
        field("URL", run.html_url),
    ];
    Effect::PublishDocument(PublishDocument {
        resource_id: document_id(run.id, run.run_attempt),
        title: format!(
            "#{} {} / Attempt #{}",
            run.run_number, run.display_title, run.run_attempt
        ),
        revision: 1,
        sections: vec![DocumentSection::Metadata(MetadataSection { fields })],
    })
}

fn failed_document(resource_id: String, message: impl Into<String>) -> Effect {
    Effect::PublishDocument(PublishDocument {
        resource_id,
        title: "GitHub Actions read failed".into(),
        revision: 1,
        sections: vec![DocumentSection::Metadata(MetadataSection {
            fields: vec![
                field("State", "Failed".into()),
                field("Message", message.into()),
            ],
        })],
    })
}

export!(GithubActions);

#[cfg(test)]
mod tests {
    use super::*;
    use sakura_senp_github_client::item_completion;

    const RUN: &str = r#"{"id":51,"workflow_id":31,"run_number":8,"run_attempt":2,"name":"Build","display_title":"Build changes","event":"push","head_branch":"main","head_sha":"abcd","status":"in_progress","conclusion":null,"created_at":"2026-09-01T00:00:00Z","updated_at":"2026-09-02T00:00:00Z","run_started_at":null,"html_url":"https://github.com/o/r/actions/runs/51"}"#;

    fn context(revision: u64) -> OperationContext {
        OperationContext {
            operation_id: "test".into(),
            owner_generation: 1,
            workspace_revision: revision,
            account_generation: 1,
            request_generation: 1,
        }
    }

    fn request(view: &str, parent: &str, cursor: &str) -> Event {
        Event::TreeRequest(TreeRequest {
            view_id: view.into(),
            parent_id: parent.into(),
            cursor: cursor.into(),
        })
    }

    fn completed(id: &str, data: String) -> ToolCompleted {
        ToolCompleted {
            read_id: id.into(),
            status: CompletionStatus::Succeeded,
            data,
            message: String::new(),
        }
    }

    fn workspace(branches: &[&str]) -> Event {
        Event::WorkspaceChanged(WorkspaceChanged {
            repositories: branches
                .iter()
                .enumerate()
                .map(|(index, branch)| Repository {
                    root_id: format!("root:{index}"),
                    branch: (*branch).into(),
                    ahead: 0,
                    remotes: Vec::new(),
                })
                .collect(),
        })
    }

    fn page(effect: &Effect) -> &PublishTreePage {
        let Effect::PublishTreePage(page) = effect else {
            panic!("expected tree page")
        };
        page
    }

    #[test]
    fn workflows_and_filtered_runs_use_distinct_fixed_shapes_and_keep_paging() {
        let mut state = State::default();
        for (parent, shape, id) in [
            ("", "workflows", None),
            ("workflow:31", "workflowRuns", Some("31")),
        ] {
            let effects = state.dispatch(context(1), request(WORKFLOWS, parent, "page:2"));
            let Effect::StartToolRead(read) = &effects[0] else {
                panic!()
            };
            assert_eq!(read.arguments[0].name, "shape");
            assert_eq!(read.arguments[0].value, shape);
            assert_eq!(
                read.arguments
                    .iter()
                    .find(|field| field.name == "id")
                    .map(|field| field.value.as_str()),
                id
            );
            assert!(read
                .arguments
                .iter()
                .any(|field| field.name == "page" && field.value == "2"));
        }
        let data =
            format!(r#"{{"body":{{"total_count":1,"workflow_runs":[{RUN}]}},"nextPage":2}}"#);
        let effect = state.complete(completed("runs:w:31:1", data));
        let page = page(&effect);
        assert_eq!(page.parent_id, "workflow:31");
        assert_eq!(page.items[0].id, "run:51:2");
        assert_eq!(page.items[0].context_value, "run");
        assert_eq!(page.items[0].label, "#8");
        assert_eq!(
            page.items[0].icon,
            "resources/icons/workflowruns/wr_inprogress.svg"
        );
        assert_eq!(page.items[0].description, "");
        assert_eq!(page.items[0].tooltip, "Attempt #2 In progress\n\nRe-run");
        assert_eq!(page.items[0].arguments, ["github-actions-run:51:2"]);
        assert_eq!(page.status, PageStatus::Partial);
        assert_eq!(page.next_cursor, "page:2");
    }

    #[test]
    fn runs_follow_upstream_labels_icons_and_tooltips() {
        let run = parse_run(&item_completion(RUN)).unwrap();
        let item = run_item(BRANCH, run);
        assert_eq!(item.label, "Build #8");
        let finished = RUN
            .replace("\"run_attempt\":2", "\"run_attempt\":1")
            .replace("\"status\":\"in_progress\"", "\"status\":\"completed\"")
            .replace("\"conclusion\":null", "\"conclusion\":\"failure\"")
            .replace(
                "\"run_started_at\":null",
                "\"run_started_at\":\"2026-09-01T23:58:30Z\"",
            )
            .replace("\"event\":\"push\"", "\"event\":\"pull_request\"");
        let item = run_item(WORKFLOWS, parse_run(&item_completion(&finished)).unwrap());
        assert_eq!(item.label, "#8");
        assert_eq!(item.icon, "resources/icons/workflowruns/wr_failure.svg");
        assert_eq!(
            item.tooltip,
            "Failed in 1m 30s\n\nTriggered via pull request"
        );
        assert_eq!(item.id, "run:51:1");
        assert_eq!(item.context_value, "run completed");
        let unnamed = RUN.replace("\"name\":\"Build\"", "\"name\":null");
        assert_eq!(
            run_item(BRANCH, parse_run(&item_completion(&unnamed)).unwrap()).label,
            "#8"
        );
        let item = workflow_item(Workflow {
            id: 31,
            name: "CI".into(),
            path: ".github/workflows/ci.yml".into(),
            state: "active".into(),
            html_url: "https://github.com/o/r/actions/workflows/ci.yml".into(),
        });
        assert_eq!(
            (
                item.label.as_str(),
                item.icon.as_str(),
                item.description.as_str()
            ),
            ("CI", "", "")
        );
        assert_eq!(item.context_value, "workflow");
    }

    #[test]
    fn branch_requires_one_current_repository_and_scope_changes_clear_it() {
        let mut state = State::default();
        assert_eq!(
            page(&state.dispatch(context(1), request(BRANCH, "", ""))[0]).status,
            PageStatus::Failed
        );
        state.dispatch(context(1), workspace(&["main", "main"]));
        assert_eq!(
            page(&state.dispatch(context(1), request(BRANCH, "", ""))[0]).status,
            PageStatus::Failed
        );
        state.dispatch(context(1), workspace(&[""]));
        let effects = state.dispatch(context(1), request(BRANCH, "", ""));
        assert_eq!(page(&effects[0]).status, PageStatus::Empty);
        assert!(page(&effects[0]).message.contains("Detached HEAD"));
        state.dispatch(context(1), workspace(&["main"]));
        let effects = state.dispatch(context(1), request(BRANCH, "", ""));
        let Effect::StartToolRead(read) = &effects[0] else {
            panic!()
        };
        assert_eq!(read.read_id, "runs:b:0:1");
        assert!(read
            .arguments
            .iter()
            .any(|field| field.name == "branch" && field.value == "main"));
        assert_eq!(
            page(&state.dispatch(context(2), request(BRANCH, "", ""))[0]).status,
            PageStatus::Failed
        );
    }

    fn invalidated(effects: &[Effect]) -> Vec<&str> {
        effects
            .iter()
            .filter_map(|effect| match effect {
                Effect::InvalidateTree(value) => Some(value.view_id.as_str()),
                _ => None,
            })
            .collect()
    }

    #[test]
    fn a_branch_change_refreshes_only_the_current_branch_view() {
        let mut state = State::default();
        assert_eq!(
            invalidated(&state.dispatch(context(1), workspace(&["main"]))),
            [BRANCH, WORKFLOWS]
        );
        // The same snapshot again changes nothing a view shows.
        assert!(state.dispatch(context(1), workspace(&["main"])).is_empty());
        assert_eq!(
            invalidated(&state.dispatch(context(1), workspace(&["topic"]))),
            [BRANCH]
        );
        assert_eq!(
            invalidated(&state.dispatch(context(1), workspace(&[""]))),
            [BRANCH]
        );
        // A different remote is a different repository for both views.
        let Event::WorkspaceChanged(mut moved) = workspace(&[""]) else {
            panic!()
        };
        moved.repositories[0].remotes.push(Remote {
            name: "origin".into(),
            url: "https://github.com/o/r.git".into(),
        });
        assert_eq!(
            invalidated(&state.dispatch(context(1), Event::WorkspaceChanged(moved))),
            [BRANCH, WORKFLOWS]
        );
        assert_eq!(
            invalidated(&state.dispatch(context(1), workspace(&["main", "main"]))),
            [BRANCH, WORKFLOWS]
        );
        // A new scope forgets what was announced before it.
        assert_eq!(
            invalidated(&state.dispatch(context(2), workspace(&["main", "main"]))),
            [BRANCH, WORKFLOWS]
        );
    }

    fn ahead_of(branch: &str, ahead: u32) -> Event {
        let Event::WorkspaceChanged(mut change) = workspace(&[branch]) else {
            panic!()
        };
        change.repositories[0].ahead = ahead;
        Event::WorkspaceChanged(change)
    }

    #[test]
    fn a_push_refreshes_only_the_current_branch_view() {
        let mut state = State::default();
        assert_eq!(
            invalidated(&state.dispatch(context(1), ahead_of("main", 0))),
            [BRANCH, WORKFLOWS]
        );
        // Commits raise the count; nothing on GitHub has changed yet.
        assert!(state.dispatch(context(1), ahead_of("main", 1)).is_empty());
        assert!(state.dispatch(context(1), ahead_of("main", 2)).is_empty());
        // The push upstream misses when the count was zero at its last refresh.
        assert_eq!(
            invalidated(&state.dispatch(context(1), ahead_of("main", 0))),
            [BRANCH]
        );
        assert!(state.dispatch(context(1), ahead_of("main", 0)).is_empty());
        // A partial push lowers the count too.
        let _ = state.dispatch(context(1), ahead_of("main", 3));
        assert_eq!(
            invalidated(&state.dispatch(context(1), ahead_of("main", 1))),
            [BRANCH]
        );
        // A checkout onto a branch with a lower count is one refresh, not two.
        assert_eq!(
            invalidated(&state.dispatch(context(1), ahead_of("topic", 0))),
            [BRANCH]
        );
        // A new scope forgets the count along with everything else.
        let _ = state.dispatch(context(1), ahead_of("topic", 5));
        assert_eq!(
            invalidated(&state.dispatch(context(2), ahead_of("topic", 1))),
            [BRANCH, WORKFLOWS]
        );
    }

    #[test]
    fn title_refresh_commands_invalidate_their_own_view_and_complete() {
        let mut state = State::default();
        for (command, view) in [(REFRESH, WORKFLOWS), (REFRESH_BRANCH, BRANCH)] {
            let effects = state.dispatch(
                context(1),
                Event::CommandInvoked(CommandInvoked {
                    command_id: command.into(),
                    arguments: Vec::new(),
                }),
            );
            assert_eq!(invalidated(&effects), [view]);
            assert!(
                matches!(&effects[1], Effect::CompleteCommand(value) if value.status == CompletionStatus::Succeeded)
            );
            let effects = state.dispatch(
                context(1),
                Event::CommandInvoked(CommandInvoked {
                    command_id: command.into(),
                    arguments: vec!["run:1".into()],
                }),
            );
            assert_eq!(effects.len(), 1);
            assert!(
                matches!(&effects[0], Effect::CompleteCommand(value) if value.status == CompletionStatus::Failed)
            );
        }
    }

    #[test]
    fn the_inline_log_action_opens_the_log_of_the_job_row_it_was_drawn_on() {
        let mut state = State::default();
        let invoke = |state: &mut State, argument: &str| {
            state.dispatch(
                context(1),
                Event::CommandInvoked(CommandInvoked {
                    command_id: jobs::VIEW_LOGS.into(),
                    arguments: vec![argument.into()],
                }),
            )
        };
        let effects = invoke(&mut state, "job:51:2:71");
        assert!(
            matches!(&effects[0], Effect::OpenDocument(value) if value.resource_id == "github-actions-job-log:51:2:71")
        );
        assert!(
            matches!(&effects[1], Effect::CompleteCommand(value) if value.status == CompletionStatus::Succeeded)
        );
        for argument in ["github-actions-job-log:51:2:71", "job:51:2", "run:51:2"] {
            let effects = invoke(&mut state, argument);
            assert_eq!(effects.len(), 1, "{argument}");
            assert!(
                matches!(&effects[0], Effect::CompleteCommand(value) if value.status == CompletionStatus::Failed)
            );
        }
    }

    #[test]
    fn opening_an_attempt_reads_that_attempt_and_rejects_different_results() {
        let mut state = State::default();
        let resource = "github-actions-run:51:1";
        let effects = state.dispatch(
            context(1),
            Event::CommandInvoked(CommandInvoked {
                command_id: OPEN_RUN.into(),
                arguments: vec![resource.into()],
            }),
        );
        assert!(
            matches!(&effects[0], Effect::OpenDocument(value) if value.resource_id == resource)
        );
        assert!(
            matches!(&effects[1], Effect::CompleteCommand(value) if value.status == CompletionStatus::Succeeded)
        );
        let effects = state.dispatch(
            context(1),
            Event::DocumentRequest(DocumentRequest {
                resource_id: resource.into(),
            }),
        );
        let Effect::StartToolRead(read) = &effects[0] else {
            panic!()
        };
        assert_eq!(read.arguments.len(), 3);
        assert_eq!(read.arguments[0].value, "runAttempt");
        assert_eq!(read.arguments[1].value, "51");
        assert_eq!(read.arguments[2].name, "attempt");
        assert_eq!(read.arguments[2].value, "1");
        let effect = state.complete(completed("detail:51:1", item_completion(RUN)));
        assert!(
            matches!(effect, Effect::PublishDocument(value) if value.title == "GitHub Actions read failed")
        );
        let effect = state.complete(completed(
            "detail:51:1",
            item_completion(&RUN.replace("\"run_attempt\":2", "\"run_attempt\":1")),
        ));
        let Effect::PublishDocument(document) = effect else {
            panic!()
        };
        assert_eq!(document.resource_id, resource);
        let DocumentSection::Metadata(metadata) = &document.sections[0] else {
            panic!()
        };
        assert!(metadata
            .fields
            .iter()
            .any(|field| field.name == "Attempt" && field.value == "1"));
        assert!(metadata
            .fields
            .iter()
            .any(|field| field.name == "State" && field.value == "in_progress"));
    }

    #[test]
    fn invalid_filters_empty_pages_and_failed_reads_have_terminal_states() {
        let mut state = State::default();
        for (view, parent, cursor) in [
            (WORKFLOWS, "workflow:0", ""),
            (BRANCH, "workflow:31", ""),
            (WORKFLOWS, "run:51", ""),
            (WORKFLOWS, "run:51:0", ""),
            (WORKFLOWS, "", "page:0"),
        ] {
            assert_eq!(
                page(&state.dispatch(context(1), request(view, parent, cursor))[0]).status,
                PageStatus::Failed
            );
        }
        let data = format!(r#"{{"body":{{"total_count":1,"workflow_runs":[{RUN}]}}}}"#);
        assert_eq!(
            page(&state.complete(completed("runs:w:99:1", data))).status,
            PageStatus::Failed
        );
        let effect = state.complete(completed(
            "runs:w:31:1",
            r#"{"body":{"total_count":0,"workflow_runs":[]}}"#.into(),
        ));
        assert_eq!(page(&effect).status, PageStatus::Empty);
        assert_eq!(
            page(&state.complete(completed(
                "runs:b:0:1",
                r#"{"body":{"total_count":0,"workflow_runs":[]}}"#.into()
            )))
            .status,
            PageStatus::Failed
        );
        for id in ["workflows:1", "runs:w:31:1", "runjobs:w:51:2:1"] {
            let mut completion = completed(id, String::new());
            completion.status = CompletionStatus::TimedOut;
            assert_eq!(page(&state.complete(completion)).status, PageStatus::Failed);
        }
        let effects = state.dispatch(
            context(1),
            Event::CommandInvoked(CommandInvoked {
                command_id: OPEN_RUN.into(),
                arguments: vec!["github-actions-run:../repo:1".into()],
            }),
        );
        assert!(
            matches!(&effects[0], Effect::CompleteCommand(value) if value.status == CompletionStatus::Failed)
        );
    }
}

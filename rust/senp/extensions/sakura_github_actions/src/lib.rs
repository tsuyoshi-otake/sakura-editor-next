//! Read-only GitHub Actions workflow, run and attempt projection.

use sakura_senp_github_client::actions::{parse_run, parse_runs, parse_workflows, Run, Workflow};
use std::cell::RefCell;

mod jobs;

wit_bindgen::generate!({ path: "../../wit/v2/senp-extension.wit", world: "extension" });
use exports::sakura::senp::event_effects::*;

const WORKFLOWS: &str = "github-actions.workflows";
const BRANCH: &str = "github-actions.current-branch";
const OPEN_RUN: &str = "github-actions.workflow.run.open";
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

#[derive(Default)]
enum BranchState {
    #[default]
    Unavailable,
    Detached,
    Selected(String),
}

#[derive(Default)]
struct State {
    scope: Option<(u64, u64, u64)>,
    branch: BranchState,
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
            self.branch = BranchState::Unavailable;
        }
        match event {
            Event::WorkspaceChanged(change) => {
                self.branch = match change.repositories.as_slice() {
                    [repository] if repository.branch.is_empty() => BranchState::Detached,
                    [repository]
                        if repository.branch.len() <= 1024 && !repository.branch.contains('\0') =>
                    {
                        BranchState::Selected(repository.branch.clone())
                    }
                    _ => BranchState::Unavailable,
                };
                invalidations()
            }
            Event::TreeRequest(request) if view_code(&request.view_id).is_some() => {
                vec![self.tree_request(request)]
            }
            Event::ToolCompleted(read) => vec![self.complete(read)],
            Event::CommandInvoked(command)
                if command.command_id == OPEN_RUN
                    || command.command_id == jobs::OPEN_JOB
                    || command.command_id == jobs::OPEN_LOG =>
            {
                if let [resource] = command.arguments.as_slice() {
                    if (command.command_id == OPEN_RUN && document_identity(resource).is_some())
                        || jobs::opens(&command.command_id, resource)
                    {
                        return vec![
                            Effect::OpenDocument(OpenDocument {
                                resource_id: resource.clone(),
                            }),
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
                    format!("actions/runs/{run}/attempts/{attempt}"),
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
        let code = view_code(view).unwrap();
        let fail = |message| failed_page(view, parent, 1, message);
        if parent.is_empty() {
            let Some(page) = page_cursor(&request.cursor) else {
                return fail("Invalid page cursor");
            };
            if view == WORKFLOWS {
                return read(
                    format!("workflows:{page}"),
                    "actions/workflows".into(),
                    paging(page),
                );
            }
            return match &self.branch {
                BranchState::Selected(branch) => {
                    let mut fields = paging(page);
                    fields.push(field("branch", branch.clone()));
                    read(format!("runs:b:0:{page}"), "actions/runs".into(), fields)
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
                    format!("actions/workflows/{workflow}/runs"),
                    paging(page),
                );
            }
        }
        if let Some(run) = parent.strip_prefix("run:").and_then(positive) {
            let (total, page) = if request.cursor.is_empty() {
                (0, 1)
            } else {
                let parts: Vec<_> = request.cursor.split(':').collect();
                let ["attempts", total, page] = parts.as_slice() else {
                    return fail("Invalid attempt page cursor");
                };
                let (Some(total), Some(page)) = (positive32(total), positive32(page)) else {
                    return fail("Invalid attempt page cursor");
                };
                (total, page)
            };
            return read(
                format!("attempts:{code}:{run}:{total}:{page}"),
                format!("actions/runs/{run}"),
                Vec::new(),
            );
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
                    }) => page_effect(view, &parent, u64::from(page), result.items.into_iter().map(run_item).collect(), next_cursor(result.next_page), String::new()),
                    Ok(_) => failed_page(view, &parent, u64::from(page), "Mismatched run filter or next page"),
                    Err(error) => failed_page(view, &parent, u64::from(page), error.to_string()),
                }
            }
            ["attempts", code, run, total, page] => {
                let (Some(view), Some(run), Ok(total), Some(page)) = (
                    code_view(code),
                    positive(run),
                    total.parse::<u32>(),
                    positive32(page),
                ) else {
                    return failed_page(WORKFLOWS, "", 1, "Invalid attempt page completion");
                };
                let parent = format!("run:{run}");
                if completion.status != CompletionStatus::Succeeded {
                    return failed_page(
                        view,
                        &parent,
                        u64::from(page),
                        completion_message(&completion),
                    );
                }
                match parse_run(&completion.data) {
                    Ok(value)
                        if value.id == run
                            && (total != 0 || page == 1)
                            && (total == 0 || total <= value.run_attempt) =>
                    {
                        attempt_page(
                            view,
                            run,
                            if total == 0 { value.run_attempt } else { total },
                            page,
                        )
                    }
                    Ok(_) => failed_page(
                        view,
                        &parent,
                        u64::from(page),
                        "Mismatched run attempt count",
                    ),
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

fn invalidations() -> Vec<Effect> {
    [BRANCH, WORKFLOWS]
        .into_iter()
        .map(|view| {
            Effect::InvalidateTree(InvalidateTree {
                view_id: view.into(),
            })
        })
        .collect()
}

fn read(read_id: String, path: String, mut arguments: Vec<Field>) -> Effect {
    arguments.insert(0, field("path", path));
    Effect::StartToolRead(StartToolRead {
        read_id,
        tool_id: "github".into(),
        operation: "repositoryRead".into(),
        arguments,
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

fn workflow_item(workflow: Workflow) -> TreeItem {
    TreeItem {
        id: format!("workflow:{}", workflow.id),
        label: workflow.name,
        description: workflow.state,
        tooltip: format!("{}\n{}", workflow.path, workflow.html_url),
        icon: "workflow".into(),
        collapsible_state: CollapsibleState::Collapsed,
        command_id: String::new(),
        arguments: Vec::new(),
    }
}

fn run_item(run: Run) -> TreeItem {
    TreeItem {
        id: format!("run:{}", run.id),
        label: format!("#{} {}", run.run_number, run.display_title),
        description: run.summary(),
        tooltip: run.html_url,
        icon: "play-circle".into(),
        collapsible_state: CollapsibleState::Collapsed,
        command_id: OPEN_RUN.into(),
        arguments: vec![document_id(run.id, run.run_attempt)],
    }
}

fn attempt_page(view: &str, run: u64, total: u32, page: u32) -> Effect {
    let parent = format!("run:{run}");
    let Some(offset) = page
        .checked_sub(1)
        .and_then(|value| value.checked_mul(PAGE_SIZE))
        .filter(|value| *value < total)
    else {
        return failed_page(
            view,
            &parent,
            u64::from(page),
            "Attempt page is outside the selected snapshot",
        );
    };
    let count = PAGE_SIZE.min(total - offset);
    let items = (0..count)
        .map(|index| {
            let attempt = total - offset - index;
            TreeItem {
                id: format!("attempt:{run}:{attempt}"),
                label: format!("Attempt #{attempt}"),
                description: "Select to read this attempt".into(),
                tooltip: format!("Run {run}, attempt {attempt}"),
                icon: "history".into(),
                collapsible_state: CollapsibleState::Collapsed,
                command_id: OPEN_RUN.into(),
                arguments: vec![document_id(run, attempt)],
            }
        })
        .collect();
    let cursor = if offset + count < total {
        format!("attempts:{total}:{}", page + 1)
    } else {
        String::new()
    };
    page_effect(view, &parent, u64::from(page), items, cursor, String::new())
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
    fn workflows_and_filtered_runs_use_distinct_fixed_paths_and_keep_paging() {
        let mut state = State::default();
        for (parent, expected) in [
            ("", "actions/workflows"),
            ("workflow:31", "actions/workflows/31/runs"),
        ] {
            let effects = state.dispatch(context(1), request(WORKFLOWS, parent, "page:2"));
            let Effect::StartToolRead(read) = &effects[0] else {
                panic!()
            };
            assert_eq!(read.arguments[0].value, expected);
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
        assert_eq!(page.items[0].id, "run:51");
        assert_eq!(page.items[0].description, "in_progress");
        assert_eq!(page.items[0].arguments, ["github-actions-run:51:2"]);
        assert_eq!(page.status, PageStatus::Partial);
        assert_eq!(page.next_cursor, "page:2");
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

    #[test]
    fn attempt_pages_are_bounded_and_pin_the_observed_attempt_count() {
        let state = State::default();
        let effect = state.complete(completed(
            "attempts:w:51:0:1",
            RUN.replace("\"run_attempt\":2", "\"run_attempt\":21"),
        ));
        let first = page(&effect);
        assert_eq!(first.items.len(), 20);
        assert_eq!(first.items[0].id, "attempt:51:21");
        assert_eq!(first.items[19].id, "attempt:51:2");
        assert_eq!(first.next_cursor, "attempts:21:2");
        let effect = state.complete(completed(
            "attempts:w:51:21:2",
            RUN.replace("\"run_attempt\":2", "\"run_attempt\":22"),
        ));
        let second = page(&effect);
        assert_eq!(second.items.len(), 1);
        assert_eq!(second.items[0].id, "attempt:51:1");
        assert_eq!(second.status, PageStatus::Complete);
        assert_eq!(
            page(&attempt_page(WORKFLOWS, 51, 2, u32::MAX)).status,
            PageStatus::Failed
        );
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
        assert_eq!(read.arguments.len(), 1);
        assert_eq!(read.arguments[0].value, "actions/runs/51/attempts/1");
        let effect = state.complete(completed("detail:51:1", RUN.into()));
        assert!(
            matches!(effect, Effect::PublishDocument(value) if value.title == "GitHub Actions read failed")
        );
        let effect = state.complete(completed(
            "detail:51:1",
            RUN.replace("\"run_attempt\":2", "\"run_attempt\":1"),
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
            (WORKFLOWS, "run:51", "attempts:0:2"),
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
        assert_eq!(
            page(&state.complete(completed("attempts:w:51:0:2", RUN.into()))).status,
            PageStatus::Failed
        );
        for id in ["workflows:1", "runs:w:31:1", "attempts:w:51:0:1"] {
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

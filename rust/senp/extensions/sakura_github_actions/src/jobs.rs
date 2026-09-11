//! Run and attempt job trees, step trees and read-only summaries.

use super::*;
use sakura_senp_github_client::actions::jobs::{parse_job, parse_jobs, Job, Step};
use sakura_senp_github_client::{parse_log_resource, LogResource};

// Upstream has a log command, but no native read-only job-summary command.
pub const OPEN_JOB: &str = "sakura.githubActions.openJobDetails";
/// Upstream's inline "View job logs" row action. It is drawn on a completed
/// job's row and receives that row's ID, the stand-in for the job node VS Code
/// passes. It is a separate command from the job because it is a separate
/// download: opening a job must not pull a log nobody asked to read.
pub const VIEW_LOGS: &str = "github-actions.workflow.logs";

/// Which of a job's two documents a resource identity names.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Kind {
    Detail,
    Log,
}

#[derive(Clone, Copy)]
pub struct Identity {
    run: u64,
    attempt: u32,
    job: u64,
}

impl Identity {
    fn matches(self, job: &Job) -> bool {
        (self.run, self.attempt, self.job) == (job.run_id, job.run_attempt, job.id)
    }
    fn item(self) -> String {
        format!("job:{}:{}:{}", self.run, self.attempt, self.job)
    }
    fn resource(self) -> String {
        format!(
            "github-actions-job:{}:{}:{}",
            self.run, self.attempt, self.job
        )
    }
    fn log_resource(self) -> String {
        format!(
            "github-actions-job-log:{}:{}:{}",
            self.run, self.attempt, self.job
        )
    }
    fn log_item(self) -> String {
        format!("joblog:{}:{}:{}", self.run, self.attempt, self.job)
    }
    fn parts(run: &str, attempt: &str, job: &str) -> Option<Self> {
        Some(Self {
            run: positive(run)?,
            attempt: positive32(attempt)?,
            job: positive(job)?,
        })
    }
}

pub fn document_identity(resource: &str) -> Option<(Identity, Kind)> {
    let parts: Vec<_> = resource.split(':').collect();
    match parts.as_slice() {
        ["github-actions-job", run, attempt, job] => {
            Some((Identity::parts(run, attempt, job)?, Kind::Detail))
        }
        ["github-actions-job-log", run, attempt, job] => {
            Some((Identity::parts(run, attempt, job)?, Kind::Log))
        }
        _ => None,
    }
}

/// The document a job command opens for its one argument, or None when the
/// argument is not that command's. The summary command takes the summary's
/// resource. The log action takes the job row's ID, as upstream's takes the job
/// node, and opens the log's own resource; a summary resource handed to it is
/// refused rather than opened as a document whose read would never answer.
pub fn opened(command_id: &str, argument: &str) -> Option<String> {
    match command_id {
        OPEN_JOB => matches!(document_identity(argument), Some((_, Kind::Detail)))
            .then(|| argument.to_string()),
        VIEW_LOGS => match argument.split(':').collect::<Vec<_>>().as_slice() {
            ["job", run, attempt, job] => Some(Identity::parts(run, attempt, job)?.log_resource()),
            _ => None,
        },
        _ => None,
    }
}

pub fn document_request(identity: Identity, kind: Kind) -> Effect {
    match kind {
        Kind::Detail => read(
            format!(
                "jobdetail:{}:{}:{}",
                identity.run, identity.attempt, identity.job
            ),
            Resource::item("job", identity.job),
            Vec::new(),
        ),
        // A log is its own operation rather than a path under the repository
        // read: the tool downloads it into the editor's text resource store and
        // answers with a handle, because an extension has no way to read bytes
        // itself and a log is far larger than a completion may carry.
        Kind::Log => log_read(identity.log_item(), identity.job),
    }
}

pub fn tree_request(request: &TreeRequest) -> Option<Effect> {
    let code = view_code(&request.view_id)?;
    let parts: Vec<_> = request.parent_id.split(':').collect();
    let fail = || {
        failed_page(
            &request.view_id,
            &request.parent_id,
            1,
            "Invalid run, attempt, job or step page identity",
        )
    };
    match parts.as_slice() {
        // Upstream's WorkflowRunNode lists the jobs of the run's latest attempt.
        // The run row's ID carries that attempt, so a rerun is a new row rather
        // than the old row's children silently switching attempts.
        [kind @ ("run" | "attempt"), run, attempt] => {
            let (Some(run), Some(attempt), Some(page)) = (
                positive(run),
                positive32(attempt),
                page_cursor(&request.cursor),
            ) else {
                return Some(fail());
            };
            let shape = if *kind == "run" { "runjobs" } else { "jobs" };
            Some(read(
                format!("{shape}:{code}:{run}:{attempt}:{page}"),
                Resource::attempt("runAttemptJobs", run, attempt),
                paging(page),
            ))
        }
        ["previous", run, attempt] => {
            let (Some(run), Some(attempt), Some(page)) = (
                positive(run),
                positive32(attempt),
                page_cursor(&request.cursor),
            ) else {
                return Some(fail());
            };
            Some(previous_page(&request.view_id, run, attempt, page))
        }
        ["job", run, attempt, job] => {
            let (Some(identity), Some(page)) = (
                Identity::parts(run, attempt, job),
                page_cursor(&request.cursor),
            ) else {
                return Some(fail());
            };
            Some(read(
                format!(
                    "steps:{code}:{}:{}:{}:{page}",
                    identity.run, identity.attempt, identity.job
                ),
                Resource::item("job", identity.job),
                Vec::new(),
            ))
        }
        _ => None,
    }
}

pub fn complete(completion: &ToolCompleted) -> Option<Effect> {
    let parts: Vec<_> = completion.read_id.split(':').collect();
    match parts.as_slice() {
        ["joblog", run, attempt, job] => {
            let Some(identity) = Identity::parts(run, attempt, job) else {
                return Some(failed_page(WORKFLOWS, "", 1, "Invalid job log completion"));
            };
            Some(if completion.status != CompletionStatus::Succeeded {
                failed_document(identity.log_resource(), completion_message(completion))
            } else {
                match parse_log_resource(&completion.data) {
                    Ok(log) => log_document(identity, log),
                    Err(error) => failed_document(identity.log_resource(), error.to_string()),
                }
            })
        }
        ["jobdetail", run, attempt, job] => {
            let Some(identity) = Identity::parts(run, attempt, job) else {
                return Some(failed_page(WORKFLOWS, "", 1, "Invalid job completion"));
            };
            Some(if completion.status != CompletionStatus::Succeeded {
                failed_document(identity.resource(), completion_message(completion))
            } else {
                match parse_job(&completion.data) {
                    Ok(job) if identity.matches(&job) => job_document(identity, job),
                    Ok(_) => failed_document(identity.resource(), "Mismatched job, run or attempt"),
                    Err(error) => failed_document(identity.resource(), error.to_string()),
                }
            })
        }
        [shape @ ("runjobs" | "jobs"), code, run, attempt, page] => {
            let (Some(view), Some(run), Some(attempt), Some(page)) = (
                code_view(code),
                positive(run),
                positive32(attempt),
                positive32(page),
            ) else {
                return Some(failed_page(WORKFLOWS, "", 1, "Invalid job page completion"));
            };
            let latest = *shape == "runjobs";
            let parent = if latest {
                format!("run:{run}:{attempt}")
            } else {
                format!("attempt:{run}:{attempt}")
            };
            let fail = |message: String| failed_page(view, &parent, u64::from(page), message);
            Some(if completion.status != CompletionStatus::Succeeded {
                fail(completion_message(completion).into())
            } else {
                match parse_jobs(&completion.data) {
                    Ok(result)
                        if forward_page(result.next_page, page)
                            && result
                                .items
                                .iter()
                                .all(|job| job.run_id == run && job.run_attempt == attempt) =>
                    {
                        let mut items: Vec<_> = result.items.into_iter().map(job_item).collect();
                        // Upstream's run row ends with "Previous attempts" when the
                        // run was rerun. It follows the last page of jobs, so it
                        // appears once and after every job.
                        if latest && attempt > 1 && result.next_page.is_none() {
                            items.push(previous_item(run, attempt));
                        }
                        page_effect(
                            view,
                            &parent,
                            u64::from(page),
                            items,
                            next_cursor(result.next_page),
                            String::new(),
                        )
                    }
                    Ok(_) => fail("Mismatched job run, attempt or page".into()),
                    Err(error) => fail(error.to_string()),
                }
            })
        }
        ["steps", code, run, attempt, job, page] => {
            let (Some(view), Some(identity), Some(page)) = (
                code_view(code),
                Identity::parts(run, attempt, job),
                positive32(page),
            ) else {
                return Some(failed_page(
                    WORKFLOWS,
                    "",
                    1,
                    "Invalid step page completion",
                ));
            };
            let fail =
                |message: String| failed_page(view, &identity.item(), u64::from(page), message);
            Some(if completion.status != CompletionStatus::Succeeded {
                fail(completion_message(completion).into())
            } else {
                match parse_job(&completion.data) {
                    Ok(job) if identity.matches(&job) => step_page(view, identity, job.steps, page),
                    Ok(_) => fail("Mismatched step job, run or attempt".into()),
                    Err(error) => fail(error.to_string()),
                }
            })
        }
        _ => None,
    }
}

// Upstream's PreviousAttemptsNode: a label and nothing else until expanded.
fn previous_item(run: u64, latest: u32) -> TreeItem {
    TreeItem {
        id: format!("previous:{run}:{latest}"),
        label: "Previous attempts".into(),
        description: String::new(),
        tooltip: String::new(),
        icon: String::new(),
        collapsible_state: CollapsibleState::Collapsed,
        command_id: String::new(),
        arguments: Vec::new(),
        context_value: String::new(),
    }
}

/// Every attempt before the latest, oldest first as upstream lists them. The
/// latest attempt is part of the parent's ID, so the list is fixed for that row
/// and needs no read.
fn previous_page(view: &str, run: u64, latest: u32, page: u32) -> Effect {
    let parent = format!("previous:{run}:{latest}");
    let total = latest - 1;
    let Some(offset) = page
        .checked_sub(1)
        .and_then(|value| value.checked_mul(PAGE_SIZE))
        .filter(|value| *value < total)
    else {
        return failed_page(
            view,
            &parent,
            u64::from(page),
            "Attempt page is outside this run",
        );
    };
    let count = PAGE_SIZE.min(total - offset);
    let items = (offset + 1..=offset + count)
        .map(|attempt| attempt_item(run, attempt))
        .collect();
    let cursor = if offset + count < total {
        format!("page:{}", page + 1)
    } else {
        String::new()
    };
    page_effect(view, &parent, u64::from(page), items, cursor, String::new())
}

// Upstream's AttemptNode. Its icon is that attempt's own status, which would
// cost one read per attempt before the list could show, so the history codicon
// stands in for it.
fn attempt_item(run: u64, attempt: u32) -> TreeItem {
    TreeItem {
        id: format!("attempt:{run}:{attempt}"),
        label: format!("Attempt #{attempt}"),
        description: String::new(),
        tooltip: format!("Run {run}, attempt {attempt}"),
        icon: "history".into(),
        collapsible_state: CollapsibleState::Collapsed,
        command_id: OPEN_RUN.into(),
        arguments: vec![document_id(run, attempt)],
        context_value: String::new(),
    }
}

fn job_item(job: Job) -> TreeItem {
    let identity = Identity {
        run: job.run_id,
        attempt: job.run_attempt,
        job: job.id,
    };
    let state = status::status_text(
        &job.status,
        job.conclusion.as_deref(),
        job.started_at.as_deref(),
        job.completed_at.as_deref(),
    );
    // Upstream's WorkflowJobNode shows state only as its icon and is a leaf when
    // it has no steps; the status string is the tooltip so the state can still
    // be read as text. Its contextValue gains "completed" once the job has,
    // which is what draws the inline log action on the row.
    let collapsible_state = if job.steps.is_empty() {
        CollapsibleState::Leaf
    } else {
        CollapsibleState::Collapsed
    };
    let context_value = if job.status == "completed" {
        "job completed"
    } else {
        "job"
    };
    TreeItem {
        id: identity.item(),
        label: job.name,
        description: String::new(),
        tooltip: state,
        icon: status::run_icon(&job.status, job.conclusion.as_deref()),
        collapsible_state,
        command_id: OPEN_JOB.into(),
        arguments: vec![identity.resource()],
        context_value: context_value.into(),
    }
}

fn step_page(view: &str, identity: Identity, steps: Vec<Step>, page: u32) -> Effect {
    let Some(offset) = page.checked_sub(1).and_then(|p| p.checked_mul(PAGE_SIZE)) else {
        return failed_page(
            view,
            &identity.item(),
            u64::from(page),
            "Invalid step page offset",
        );
    };
    if offset as usize >= steps.len() && page != 1 {
        return failed_page(
            view,
            &identity.item(),
            u64::from(page),
            "Step page is outside this job",
        );
    }
    let next = if (offset as usize + PAGE_SIZE as usize) < steps.len() {
        Some(page + 1)
    } else {
        None
    };
    let items: Vec<TreeItem> = steps
        .into_iter()
        .skip(offset as usize)
        .take(PAGE_SIZE as usize)
        // Upstream's WorkflowStepNode: the step name, its state as the icon, and
        // no description or tooltip. The number stays in the stable item ID.
        .map(|step| TreeItem {
            id: format!(
                "step:{}:{}:{}:{}",
                identity.run, identity.attempt, identity.job, step.number
            ),
            icon: status::step_icon(&step.status, step.conclusion.as_deref()),
            context_value: if step.status == "completed" {
                "step completed"
            } else {
                "step"
            }
            .into(),
            label: step.name,
            description: String::new(),
            tooltip: String::new(),
            collapsible_state: CollapsibleState::Leaf,
            command_id: OPEN_JOB.into(),
            arguments: vec![identity.resource()],
        })
        .collect();
    page_effect(
        view,
        &identity.item(),
        u64::from(page),
        items,
        next_cursor(next),
        String::new(),
    )
}

fn log_document(identity: Identity, log: LogResource) -> Effect {
    // The handle is named only now that the tool has answered with a real one.
    // The editor reads a text resource section as soon as it is published and
    // does not wait on its status, so a placeholder handle would be published as
    // a read that immediately fails rather than as a log still arriving.
    Effect::PublishDocument(PublishDocument {
        resource_id: identity.log_resource(),
        title: format!("Job {} log / Attempt #{}", identity.job, identity.attempt),
        revision: 1,
        sections: vec![
            DocumentSection::Metadata(MetadataSection {
                fields: vec![
                    field("Job ID", identity.job.to_string()),
                    field("Run ID", identity.run.to_string()),
                    field("Attempt", identity.attempt.to_string()),
                    field("Bytes", log.bytes.to_string()),
                ],
            }),
            DocumentSection::TextResource(TextResourceSection {
                handle: log.handle,
                length: log.bytes,
                status: TextStatus::Complete,
            }),
        ],
    })
}

fn optional(value: Option<String>, absent: &str) -> String {
    value.unwrap_or_else(|| absent.into())
}

fn job_document(identity: Identity, job: Job) -> Effect {
    let fields = vec![
        field("Job ID", job.id.to_string()),
        field("Run ID", job.run_id.to_string()),
        field("Attempt", job.run_attempt.to_string()),
        field("State", job.summary()),
        field("Started", optional(job.started_at, "not started")),
        field("Completed", optional(job.completed_at, "not completed")),
        field("Runner", optional(job.runner_name, "unassigned")),
        field(
            "Runner ID",
            job.runner_id
                .map(|v| v.to_string())
                .unwrap_or_else(|| "unassigned".into()),
        ),
        field(
            "Runner group",
            optional(job.runner_group_name, "unassigned"),
        ),
        field(
            "Runner group ID",
            job.runner_group_id
                .map(|v| v.to_string())
                .unwrap_or_else(|| "unassigned".into()),
        ),
        field("Labels", job.labels.join(", ")),
        field("Commit", job.head_sha),
        field("URL", job.html_url),
    ];
    let steps = if job.steps.is_empty() {
        DocumentSection::Markdown(MarkdownSection {
            text: "No steps were reported for this job.".into(),
        })
    } else {
        DocumentSection::Table(TableSection {
            columns: vec![
                "Step".into(),
                "Name".into(),
                "State".into(),
                "Started".into(),
                "Completed".into(),
            ],
            rows: job
                .steps
                .into_iter()
                .map(|step| {
                    let summary = step.summary();
                    TableRow {
                        cells: vec![
                            step.number.to_string(),
                            step.name,
                            summary,
                            optional(step.started_at, "not started"),
                            optional(step.completed_at, "not completed"),
                        ],
                    }
                })
                .collect(),
        })
    };
    Effect::PublishDocument(PublishDocument {
        resource_id: identity.resource(),
        title: job.name,
        revision: 1,
        sections: vec![DocumentSection::Metadata(MetadataSection { fields }), steps],
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use sakura_senp_github_client::item_completion;

    const JOB: &str = r#"{"id":71,"run_id":51,"run_attempt":2,"name":"Build (Windows)","status":"in_progress","conclusion":null,"started_at":null,"completed_at":null,"html_url":"https://github.com/o/r/actions/runs/51/job/71","head_sha":"abcd","runner_id":null,"runner_name":null,"runner_group_id":null,"runner_group_name":null,"labels":[],"steps":[{"number":7,"name":"Compile","status":"queued","conclusion":null,"started_at":null,"completed_at":null}]}"#;

    fn completion(id: &str, data: String) -> ToolCompleted {
        ToolCompleted {
            read_id: id.into(),
            status: CompletionStatus::Succeeded,
            data,
            message: String::new(),
        }
    }
    fn request(parent: &str, cursor: &str) -> TreeRequest {
        TreeRequest {
            view_id: WORKFLOWS.into(),
            parent_id: parent.into(),
            cursor: cursor.into(),
        }
    }
    fn page(effect: &Effect) -> &PublishTreePage {
        let Effect::PublishTreePage(page) = effect else {
            panic!("expected tree terminal")
        };
        page
    }

    #[test]
    fn jobs_are_scoped_to_the_selected_attempt_and_keep_matrix_ids_distinct() {
        let Effect::StartToolRead(read) = tree_request(&request("attempt:51:2", "page:2")).unwrap()
        else {
            panic!()
        };
        assert_eq!(read.read_id, "jobs:w:51:2:2");
        assert_eq!(read.arguments[0].value, "runAttemptJobs");
        assert_eq!(read.arguments[1].value, "51");
        assert_eq!(read.arguments[2].value, "2");
        let second = JOB.replace("\"id\":71", "\"id\":72");
        let data =
            format!(r#"{{"body":{{"total_count":2,"jobs":[{JOB},{second}]}},"nextPage":2}}"#);
        let effect = complete(&completion("jobs:w:51:2:1", data)).unwrap();
        let page = page(&effect);
        assert_eq!(page.parent_id, "attempt:51:2");
        assert_eq!(page.items[0].label, page.items[1].label);
        assert_ne!(page.items[0].id, page.items[1].id);
        assert_eq!(page.next_cursor, "page:2");
        assert_eq!(page.items[0].arguments, ["github-actions-job:51:2:71"]);
    }

    #[test]
    fn a_run_lists_its_latest_attempts_jobs_then_its_previous_attempts() {
        let Effect::StartToolRead(read) = tree_request(&request("run:51:2", "")).unwrap() else {
            panic!()
        };
        assert_eq!(read.read_id, "runjobs:w:51:2:1");
        assert_eq!(read.arguments[0].value, "runAttemptJobs");
        assert_eq!(read.arguments[1].value, "51");
        assert_eq!(read.arguments[2].value, "2");
        let data = format!(r#"{{"body":{{"total_count":2,"jobs":[{JOB}]}},"nextPage":2}}"#);
        let effect = complete(&completion("runjobs:w:51:2:1", data)).unwrap();
        let first = page(&effect);
        assert_eq!(first.parent_id, "run:51:2");
        // It follows the last page of jobs, not every page.
        assert_eq!(first.items.len(), 1);
        let second = JOB.replace("\"id\":71", "\"id\":72");
        let data = format!(r#"{{"body":{{"total_count":2,"jobs":[{second}]}}}}"#);
        let effect = complete(&completion("runjobs:w:51:2:2", data)).unwrap();
        let last = page(&effect);
        assert_eq!(last.items.len(), 2);
        assert_eq!(last.items[0].id, "job:51:2:72");
        assert_eq!(last.items[1].id, "previous:51:2");
        assert_eq!(last.items[1].label, "Previous attempts");
        assert_eq!(last.items[1].collapsible_state, CollapsibleState::Collapsed);
        assert_eq!(last.items[1].command_id, "");
        // A run on its first attempt has nothing before it.
        let first_attempt = JOB.replace("\"run_attempt\":2", "\"run_attempt\":1");
        let data = format!(r#"{{"body":{{"total_count":1,"jobs":[{first_attempt}]}}}}"#);
        let effect = complete(&completion("runjobs:w:51:1:1", data)).unwrap();
        assert_eq!(page(&effect).items.len(), 1);
        // A latest attempt with no jobs still leads to the earlier ones.
        let empty = r#"{"body":{"total_count":0,"jobs":[]}}"#.to_string();
        let effect = complete(&completion("runjobs:w:51:2:1", empty)).unwrap();
        assert_eq!(page(&effect).items[0].id, "previous:51:2");
        assert_eq!(page(&effect).status, PageStatus::Complete);
        // Jobs of another attempt never appear under this run row.
        let data = format!(r#"{{"body":{{"total_count":1,"jobs":[{first_attempt}]}}}}"#);
        let effect = complete(&completion("runjobs:w:51:2:1", data)).unwrap();
        assert_eq!(page(&effect).status, PageStatus::Failed);
    }

    #[test]
    fn previous_attempts_list_every_earlier_attempt_oldest_first() {
        let effect = tree_request(&request("previous:51:22", "")).unwrap();
        let first = page(&effect);
        assert_eq!(first.parent_id, "previous:51:22");
        assert_eq!(first.items.len(), 20);
        assert_eq!(first.items[0].id, "attempt:51:1");
        assert_eq!(first.items[0].label, "Attempt #1");
        assert_eq!(first.items[0].command_id, OPEN_RUN);
        assert_eq!(first.items[0].arguments, ["github-actions-run:51:1"]);
        assert_eq!(
            first.items[0].collapsible_state,
            CollapsibleState::Collapsed
        );
        assert_eq!(first.items[19].id, "attempt:51:20");
        assert_eq!(first.next_cursor, "page:2");
        let effect = tree_request(&request("previous:51:22", "page:2")).unwrap();
        assert_eq!(page(&effect).items.len(), 1);
        assert_eq!(page(&effect).items[0].id, "attempt:51:21");
        assert_eq!(page(&effect).status, PageStatus::Complete);
        for (parent, cursor) in [
            ("previous:51:1", ""),
            ("previous:51:22", "page:3"),
            ("previous:51:0", ""),
            ("previous:0:2", ""),
            ("run:51:0", ""),
        ] {
            assert_eq!(
                page(&tree_request(&request(parent, cursor)).unwrap()).status,
                PageStatus::Failed
            );
        }
    }

    #[test]
    fn job_details_and_steps_preserve_null_state_and_reject_other_attempts() {
        let resource = "github-actions-job:51:2:71";
        let (identity, kind) = document_identity(resource).unwrap();
        assert_eq!(kind, Kind::Detail);
        let Effect::StartToolRead(read) = document_request(identity, kind) else {
            panic!()
        };
        assert_eq!(read.arguments[0].value, "job");
        assert_eq!(read.arguments[1].value, "71");
        let effect = complete(&completion("jobdetail:51:2:71", item_completion(JOB))).unwrap();
        let Effect::PublishDocument(document) = effect else {
            panic!()
        };
        assert_eq!(document.resource_id, resource);
        let DocumentSection::Table(table) = &document.sections[1] else {
            panic!()
        };
        assert_eq!(
            table.rows[0].cells,
            ["7", "Compile", "queued", "not started", "not completed"]
        );
        let effect = complete(&completion("steps:w:51:2:71:1", item_completion(JOB))).unwrap();
        // The log is the job row's inline action, not a row of its own.
        assert_eq!(page(&effect).items.len(), 1);
        assert_eq!(page(&effect).items[0].id, "step:51:2:71:7");
        assert_eq!(page(&effect).items[0].label, "Compile");
        assert_eq!(
            page(&effect).items[0].icon,
            "resources/icons/steps/step_queued.svg"
        );
        assert_eq!(page(&effect).items[0].description, "");
        assert_eq!(page(&effect).items[0].context_value, "step");
        let done = JOB.replace("\"status\":\"queued\"", "\"status\":\"completed\"");
        let effect = complete(&completion("steps:w:51:2:71:1", item_completion(&done))).unwrap();
        assert_eq!(page(&effect).items[0].context_value, "step completed");
        let effect = complete(&completion("jobdetail:51:1:71", item_completion(JOB))).unwrap();
        assert!(
            matches!(effect,Effect::PublishDocument(doc) if doc.title=="GitHub Actions read failed")
        );
    }

    #[test]
    fn empty_failed_and_stale_job_results_have_distinct_terminals() {
        let empty = r#"{"body":{"total_count":0,"jobs":[]}}"#.to_string();
        assert_eq!(
            page(&complete(&completion("jobs:w:51:2:1", empty)).unwrap()).status,
            PageStatus::Empty
        );
        let data = format!(r#"{{"body":{{"total_count":1,"jobs":[{JOB}]}}}}"#);
        assert_eq!(
            page(&complete(&completion("jobs:w:51:1:1", data)).unwrap()).status,
            PageStatus::Failed
        );
        for id in ["jobs:w:51:2:1", "runjobs:w:51:2:1", "steps:w:51:2:71:1"] {
            let mut result = completion(id, String::new());
            result.status = CompletionStatus::TimedOut;
            assert_eq!(page(&complete(&result).unwrap()).status, PageStatus::Failed);
        }
        assert_eq!(
            page(&tree_request(&request("attempt:51:0", "")).unwrap()).status,
            PageStatus::Failed
        );
        assert!(document_identity("github-actions-job:51:2:../72").is_none());
        let job = parse_job(&item_completion(&JOB.replace("in_progress", "new_state"))).unwrap();
        let item = job_item(job);
        assert_eq!(item.icon, "");
        assert_eq!(item.tooltip, "New state");
        assert_eq!(item.context_value, "job");
    }

    #[test]
    fn jobs_show_upstream_state_icons_with_the_status_string_as_tooltip() {
        let job = parse_job(&item_completion(JOB)).unwrap();
        let item = job_item(job);
        assert_eq!(item.label, "Build (Windows)");
        assert_eq!(item.icon, "resources/icons/workflowruns/wr_inprogress.svg");
        assert_eq!(item.description, "");
        assert_eq!(item.tooltip, "In progress");
        assert_eq!(item.collapsible_state, CollapsibleState::Collapsed);
        // Only a completed job's contextValue shows the inline log action.
        assert_eq!(item.context_value, "job");
        let done = JOB
            .replacen("\"status\":\"in_progress\"", "\"status\":\"completed\"", 1)
            .replacen("\"conclusion\":null", "\"conclusion\":\"success\"", 1)
            .replacen(
                "\"started_at\":null",
                "\"started_at\":\"2026-09-01T00:00:00Z\"",
                1,
            )
            .replacen(
                "\"completed_at\":null",
                "\"completed_at\":\"2026-09-01T01:02:03Z\"",
                1,
            );
        let item = job_item(parse_job(&item_completion(&done)).unwrap());
        assert_eq!(item.icon, "resources/icons/workflowruns/wr_success.svg");
        assert_eq!(item.tooltip, "Succeeded in 1h 2m 3s");
        assert_eq!(item.context_value, "job completed");
        // Upstream's job node is a leaf when it has no steps.
        let stepless = done.split(",\"steps\":").next().unwrap().to_string() + ",\"steps\":[]}";
        let item = job_item(parse_job(&item_completion(&stepless)).unwrap());
        assert_eq!(item.collapsible_state, CollapsibleState::Leaf);
    }

    #[test]
    fn a_job_log_is_its_own_document_named_only_once_the_tool_answers() {
        let (identity, kind) = document_identity("github-actions-job-log:51:2:71").unwrap();
        assert_eq!(kind, Kind::Log);
        let Effect::StartToolRead(read) = document_request(identity, kind) else {
            panic!()
        };
        // A log is one endpoint the tool owns, so it is named by job alone.
        assert_eq!(read.read_id, "joblog:51:2:71");
        assert_eq!(read.operation, "jobLog");
        assert_eq!(read.arguments.len(), 1);
        assert_eq!(read.arguments[0].name, "id");
        assert_eq!(read.arguments[0].value, "71");

        let answer = r#"{"resource":"text:3:1","bytes":8192,"log":true}"#;
        let effect = complete(&completion("joblog:51:2:71", answer.into())).unwrap();
        let Effect::PublishDocument(document) = effect else {
            panic!()
        };
        assert_eq!(document.resource_id, "github-actions-job-log:51:2:71");
        let DocumentSection::TextResource(section) = &document.sections[1] else {
            panic!()
        };
        assert_eq!(section.handle, "text:3:1");
        assert_eq!(section.length, 8192);
        assert_eq!(section.status, TextStatus::Complete);
    }

    #[test]
    fn a_log_that_did_not_arrive_publishes_no_resource_to_read() {
        // The editor reads a text resource section as soon as it is published, so
        // a failure has to be a document with no section rather than a section
        // naming a handle nothing can answer.
        let mut result = completion("joblog:51:2:71", String::new());
        result.status = CompletionStatus::HostUnavailable;
        result.message = "unauthorized".into();
        let Effect::PublishDocument(document) = complete(&result).unwrap() else {
            panic!()
        };
        assert_eq!(document.resource_id, "github-actions-job-log:51:2:71");
        assert_eq!(document.sections.len(), 1);
        assert!(matches!(document.sections[0], DocumentSection::Metadata(_)));
        // A page answer names a resource too; read as a log it would show JSON
        // where log text belongs.
        let page_answer = r#"{"bytes":4,"httpStatus":200,"page":1,"resource":"text:3:1"}"#;
        let Effect::PublishDocument(document) =
            complete(&completion("joblog:51:2:71", page_answer.into())).unwrap()
        else {
            panic!()
        };
        assert_eq!(document.sections.len(), 1);
        assert_eq!(document.title, "GitHub Actions read failed");
    }

    #[test]
    fn each_job_command_opens_only_its_own_kind_of_document() {
        assert_eq!(
            opened(OPEN_JOB, "github-actions-job:51:2:71").as_deref(),
            Some("github-actions-job:51:2:71")
        );
        // The log action takes the job row itself, as upstream's takes the node.
        assert_eq!(
            opened(VIEW_LOGS, "job:51:2:71").as_deref(),
            Some("github-actions-job-log:51:2:71")
        );
        for (command, argument) in [
            (OPEN_JOB, "github-actions-job-log:51:2:71"),
            (OPEN_JOB, "job:51:2:71"),
            (VIEW_LOGS, "github-actions-job-log:51:2:71"),
            (VIEW_LOGS, "github-actions-job:51:2:71"),
            (VIEW_LOGS, "job:51:0:71"),
            (VIEW_LOGS, "step:51:2:71:7"),
            ("sakura.githubActions.other", "github-actions-job:51:2:71"),
        ] {
            assert!(opened(command, argument).is_none(), "{command} {argument}");
        }
    }

    #[test]
    fn step_pages_are_bounded_and_use_upstream_step_names() {
        let (identity, _) = document_identity("github-actions-job:51:2:71").unwrap();
        let step = parse_job(&item_completion(JOB)).unwrap().steps.remove(0);
        let steps: Vec<_> = (1..=21)
            .map(|number| Step {
                number,
                ..step.clone()
            })
            .collect();
        let effect = step_page(WORKFLOWS, identity, steps.clone(), 1);
        assert_eq!(page(&effect).items.len(), 20);
        assert_eq!(page(&effect).items[0].id, "step:51:2:71:1");
        assert_eq!(page(&effect).next_cursor, "page:2");
        let effect = step_page(WORKFLOWS, identity, steps.clone(), 2);
        assert_eq!(page(&effect).items.len(), 1);
        assert_eq!(page(&effect).items[0].id, "step:51:2:71:21");
        assert_eq!(
            page(&step_page(WORKFLOWS, identity, steps, u32::MAX)).status,
            PageStatus::Failed
        );
    }
}

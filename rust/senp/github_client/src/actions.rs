//! Workflow and run identities and bounded Actions REST responses.

pub mod jobs;

use super::{
    bounded_text, item_body, strict_json, ParseError, MAXIMUM_PAGE_ITEMS, MAXIMUM_RESPONSE_BYTES,
};
use serde::Deserialize;
use std::collections::BTreeSet;

#[derive(Clone, Debug, Deserialize, Eq, PartialEq)]
pub struct Workflow {
    pub id: u64,
    pub name: String,
    pub path: String,
    pub state: String,
    pub html_url: String,
}

#[derive(Clone, Debug, Deserialize, Eq, PartialEq)]
pub struct Run {
    pub id: u64,
    pub workflow_id: u64,
    pub run_number: u64,
    pub run_attempt: u32,
    pub name: Option<String>,
    pub display_title: String,
    pub event: String,
    pub head_branch: Option<String>,
    pub head_sha: String,
    pub status: String,
    pub conclusion: Option<String>,
    pub created_at: String,
    pub updated_at: String,
    pub run_started_at: Option<String>,
    pub html_url: String,
}

impl Run {
    pub fn summary(&self) -> String {
        state_summary(&self.status, self.conclusion.as_deref())
    }
}

pub fn state_summary(status: &str, conclusion: Option<&str>) -> String {
    let state = match status {
        "queued" | "in_progress" | "completed" | "waiting" | "requested" | "pending" => {
            status.to_owned()
        }
        _ => format!("unknown ({status})"),
    };
    match conclusion {
        Some(value) => format!("{state} / {value}"),
        None if status == "completed" => format!("{state} / conclusion unavailable"),
        None => state,
    }
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Page<T> {
    pub items: Vec<T>,
    pub next_page: Option<u32>,
}

#[derive(Deserialize)]
#[serde(rename_all = "camelCase")]
struct Envelope<T> {
    body: T,
    next_page: Option<u32>,
}

#[derive(Deserialize)]
struct Workflows {
    total_count: u64,
    workflows: Vec<Workflow>,
}

#[derive(Deserialize)]
struct Runs {
    total_count: u64,
    workflow_runs: Vec<Run>,
}

fn valid_url(value: &str) -> bool {
    bounded_text(value, 2048) && value.starts_with("https://")
}

fn valid_optional(value: &Option<String>, maximum: usize) -> bool {
    value
        .as_deref()
        .is_none_or(|value| bounded_text(value, maximum))
}

fn valid_run(value: &Run) -> bool {
    value.id > 0
        && value.workflow_id > 0
        && value.run_number > 0
        && value.run_attempt > 0
        && valid_optional(&value.name, 1024)
        && bounded_text(&value.display_title, 1024)
        && bounded_text(&value.event, 128)
        && valid_optional(&value.head_branch, 1024)
        && bounded_text(&value.head_sha, 128)
        && bounded_text(&value.status, 128)
        && valid_optional(&value.conclusion, 128)
        && bounded_text(&value.created_at, 64)
        && bounded_text(&value.updated_at, 64)
        && valid_optional(&value.run_started_at, 64)
        && valid_url(&value.html_url)
}

fn check_page(count: usize, total: u64, next: Option<u32>) -> Result<(), ParseError> {
    if count > MAXIMUM_PAGE_ITEMS || total < count as u64 || next == Some(0) {
        Err(ParseError::InvalidEnvelope)
    } else {
        Ok(())
    }
}

pub fn parse_workflows(data: &str) -> Result<Page<Workflow>, ParseError> {
    if data.len() > MAXIMUM_RESPONSE_BYTES {
        return Err(ParseError::LimitExceeded);
    }
    let envelope: Envelope<Workflows> = strict_json(data)?;
    check_page(
        envelope.body.workflows.len(),
        envelope.body.total_count,
        envelope.next_page,
    )?;
    let mut ids = BTreeSet::new();
    for value in &envelope.body.workflows {
        if value.id == 0
            || !ids.insert(value.id)
            || !bounded_text(&value.name, 1024)
            || !bounded_text(&value.path, 2048)
            || !bounded_text(&value.state, 128)
            || !valid_url(&value.html_url)
        {
            return Err(ParseError::InvalidItem);
        }
    }
    Ok(Page {
        items: envelope.body.workflows,
        next_page: envelope.next_page,
    })
}

pub fn parse_runs(data: &str) -> Result<Page<Run>, ParseError> {
    if data.len() > MAXIMUM_RESPONSE_BYTES {
        return Err(ParseError::LimitExceeded);
    }
    let envelope: Envelope<Runs> = strict_json(data)?;
    check_page(
        envelope.body.workflow_runs.len(),
        envelope.body.total_count,
        envelope.next_page,
    )?;
    let mut ids = BTreeSet::new();
    if envelope
        .body
        .workflow_runs
        .iter()
        .any(|value| !ids.insert(value.id) || !valid_run(value))
    {
        return Err(ParseError::InvalidItem);
    }
    Ok(Page {
        items: envelope.body.workflow_runs,
        next_page: envelope.next_page,
    })
}

pub fn parse_run(data: &str) -> Result<Run, ParseError> {
    if data.len() > MAXIMUM_RESPONSE_BYTES {
        return Err(ParseError::LimitExceeded);
    }
    let value: Run = item_body(data)?;
    if !valid_run(&value) {
        return Err(ParseError::InvalidItem);
    }
    Ok(value)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::item_completion;

    const WORKFLOW: &str = r#"{"id":31,"name":"Build","path":".github/workflows/build.yml","state":"disabled_manually","html_url":"https://github.com/o/r/actions/workflows/build.yml"}"#;
    const RUN: &str = r#"{"id":51,"workflow_id":31,"run_number":8,"run_attempt":2,"name":"Build","display_title":"Build changes","event":"push","head_branch":"main","head_sha":"abcd","status":"in_progress","conclusion":null,"created_at":"2026-09-01T00:00:00Z","updated_at":"2026-09-02T00:00:00Z","run_started_at":null,"html_url":"https://github.com/o/r/actions/runs/51"}"#;

    #[test]
    fn workflows_exist_without_runs_and_preserve_disabled_state() {
        let data =
            format!(r#"{{"body":{{"total_count":1,"workflows":[{WORKFLOW}]}},"nextPage":2}}"#);
        let page = parse_workflows(&data).unwrap();
        assert_eq!(page.items[0].state, "disabled_manually");
        assert_eq!(page.items[0].id, 31);
        assert_eq!(page.next_page, Some(2));
        assert!(
            parse_workflows(r#"{"body":{"total_count":0,"workflows":[]}}"#)
                .unwrap()
                .items
                .is_empty()
        );
    }

    #[test]
    fn run_attempt_and_null_or_unknown_status_are_not_conflated() {
        let run = parse_run(&item_completion(RUN)).unwrap();
        assert_eq!(
            (run.id, run.workflow_id, run.run_number, run.run_attempt),
            (51, 31, 8, 2)
        );
        assert_eq!(run.summary(), "in_progress");
        let changed = RUN
            .replace("in_progress", "brand_new_state")
            .replace("\"head_branch\":\"main\"", "\"head_branch\":null");
        let run = parse_run(&item_completion(&changed)).unwrap();
        assert_eq!(run.summary(), "unknown (brand_new_state)");
        assert_eq!(run.head_branch, None);
        assert_eq!(
            parse_run(&item_completion(&RUN.replace("in_progress", "completed")))
                .unwrap()
                .summary(),
            "completed / conclusion unavailable"
        );
    }

    #[test]
    fn malformed_pages_and_oversized_or_duplicate_items_fail_closed() {
        let page =
            |items: &str| format!(r#"{{"body":{{"total_count":2,"workflow_runs":[{items}]}}}}"#);
        assert_eq!(parse_runs(&page(RUN)).unwrap().items.len(), 1);
        assert!(parse_runs(&page(&format!("{RUN},{RUN}"))).is_err());
        for (from, to) in [
            ("\"run_attempt\":2", "\"run_attempt\":0"),
            ("\"workflow_id\":31", "\"workflow_id\":0"),
            ("https://", "file://"),
            ("\"id\":51", "\"id\":51,\"id\":52"),
        ] {
            assert!(parse_run(&item_completion(&RUN.replace(from, to))).is_err());
        }
        assert!(parse_runs(r#"{"body":[],"nextPage":2}"#).is_err());
        assert!(parse_runs(&page(RUN).replace("\"total_count\":2", "\"total_count\":0")).is_err());
        assert!(parse_run(&(item_completion(RUN) + " trailing")).is_err());
        assert_eq!(
            parse_run(&"x".repeat(MAXIMUM_RESPONSE_BYTES + 1)),
            Err(ParseError::LimitExceeded)
        );
    }
}

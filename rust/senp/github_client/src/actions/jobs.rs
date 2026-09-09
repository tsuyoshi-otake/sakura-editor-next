//! Jobs have database identities; steps have numbers scoped to one job.

use super::{
    bounded_text, check_page, item_body, state_summary, strict_json, valid_optional, valid_url,
    Envelope, Page, ParseError, MAXIMUM_PAGE_ITEMS, MAXIMUM_RESPONSE_BYTES,
};
use serde::Deserialize;
use std::collections::BTreeSet;

#[derive(Clone, Debug, Deserialize, Eq, PartialEq)]
pub struct Step {
    pub number: u32,
    pub name: String,
    pub status: String,
    pub conclusion: Option<String>,
    pub started_at: Option<String>,
    pub completed_at: Option<String>,
}

impl Step {
    pub fn summary(&self) -> String {
        state_summary(&self.status, self.conclusion.as_deref())
    }
}

#[derive(Clone, Debug, Deserialize, Eq, PartialEq)]
pub struct Job {
    pub id: u64,
    pub run_id: u64,
    pub run_attempt: u32,
    pub name: String,
    pub status: String,
    pub conclusion: Option<String>,
    pub started_at: Option<String>,
    pub completed_at: Option<String>,
    pub html_url: String,
    pub head_sha: String,
    pub runner_id: Option<u64>,
    pub runner_name: Option<String>,
    pub runner_group_id: Option<u64>,
    pub runner_group_name: Option<String>,
    pub labels: Vec<String>,
    pub steps: Vec<Step>,
}

impl Job {
    pub fn summary(&self) -> String {
        state_summary(&self.status, self.conclusion.as_deref())
    }
}

#[derive(Deserialize)]
struct Jobs {
    total_count: u64,
    jobs: Vec<Job>,
}

fn valid_job(value: &Job) -> bool {
    let mut numbers = BTreeSet::new();
    value.id > 0
        && value.run_id > 0
        && value.run_attempt > 0
        && bounded_text(&value.name, 1024)
        && bounded_text(&value.status, 128)
        && valid_optional(&value.conclusion, 128)
        && valid_optional(&value.started_at, 64)
        && valid_optional(&value.completed_at, 64)
        && valid_url(&value.html_url)
        && bounded_text(&value.head_sha, 128)
        && valid_optional(&value.runner_name, 1024)
        && valid_optional(&value.runner_group_name, 1024)
        && value.labels.len() <= 32
        && value.labels.iter().all(|v| bounded_text(v, 256))
        && value.steps.len() <= MAXIMUM_PAGE_ITEMS
        && value.steps.iter().all(|step| {
            step.number > 0
                && numbers.insert(step.number)
                && bounded_text(&step.name, 1024)
                && bounded_text(&step.status, 128)
                && valid_optional(&step.conclusion, 128)
                && valid_optional(&step.started_at, 64)
                && valid_optional(&step.completed_at, 64)
        })
}

pub fn parse_jobs(data: &str) -> Result<Page<Job>, ParseError> {
    if data.len() > MAXIMUM_RESPONSE_BYTES {
        return Err(ParseError::LimitExceeded);
    }
    let envelope: Envelope<Jobs> = strict_json(data)?;
    check_page(
        envelope.body.jobs.len(),
        envelope.body.total_count,
        envelope.next_page,
    )?;
    let mut ids = BTreeSet::new();
    if envelope
        .body
        .jobs
        .iter()
        .any(|job| !ids.insert(job.id) || !valid_job(job))
    {
        return Err(ParseError::InvalidItem);
    }
    Ok(Page {
        items: envelope.body.jobs,
        next_page: envelope.next_page,
    })
}

pub fn parse_job(data: &str) -> Result<Job, ParseError> {
    if data.len() > MAXIMUM_RESPONSE_BYTES {
        return Err(ParseError::LimitExceeded);
    }
    let mut job: Job = item_body(data)?;
    if !valid_job(&job) {
        return Err(ParseError::InvalidItem);
    }
    job.steps.sort_by_key(|step| step.number);
    Ok(job)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::item_completion;

    const JOB: &str = r#"{"id":71,"run_id":51,"run_attempt":2,"name":"Build (Windows)","status":"in_progress","conclusion":null,"started_at":null,"completed_at":null,"html_url":"https://github.com/o/r/actions/runs/51/job/71","head_sha":"abcd","runner_id":null,"runner_name":null,"runner_group_id":null,"runner_group_name":null,"labels":[],"steps":[{"number":7,"name":"Compile","status":"queued","conclusion":null,"started_at":null,"completed_at":null}]}"#;

    #[test]
    fn matrix_names_are_not_identities_and_nulls_remain_explicit() {
        let second = JOB.replace("\"id\":71", "\"id\":72");
        let data =
            format!(r#"{{"body":{{"total_count":2,"jobs":[{JOB},{second}]}},"nextPage":2}}"#);
        let page = parse_jobs(&data).unwrap();
        assert_eq!(page.items[0].name, page.items[1].name);
        assert_ne!(page.items[0].id, page.items[1].id);
        assert_eq!(page.next_page, Some(2));
        let job = parse_job(&item_completion(JOB)).unwrap();
        assert_eq!(job.summary(), "in_progress");
        assert_eq!(job.runner_id, None);
        assert_eq!(job.steps[0].number, 7);
        assert_eq!(job.steps[0].summary(), "queued");
        assert_eq!(job.steps[0].started_at, None);
        assert_eq!(
            parse_job(&item_completion(&JOB.replace("in_progress", "completed")))
                .unwrap()
                .summary(),
            "completed / conclusion unavailable"
        );
        assert_eq!(
            parse_job(&item_completion(&JOB.replace("queued", "new_state")))
                .unwrap()
                .steps[0]
                .summary(),
            "unknown (new_state)"
        );
    }

    #[test]
    fn skipped_empty_jobs_are_valid_but_bad_or_duplicate_identities_fail() {
        let mut value: serde_json::Value = serde_json::from_str(JOB).unwrap();
        value["steps"] = serde_json::json!([]);
        value["status"] = serde_json::json!("completed");
        value["conclusion"] = serde_json::json!("skipped");
        assert_eq!(
            parse_job(&item_completion(&value.to_string()))
                .unwrap()
                .summary(),
            "completed / skipped"
        );
        assert!(parse_jobs(r#"{"body":{"total_count":0,"jobs":[]}}"#)
            .unwrap()
            .items
            .is_empty());
        assert!(parse_jobs(&format!(
            r#"{{"body":{{"total_count":2,"jobs":[{JOB},{JOB}]}}}}"#
        ))
        .is_err());
        for (from, to) in [
            ("\"id\":71", "\"id\":0"),
            ("\"run_attempt\":2", "\"run_attempt\":0"),
            ("\"number\":7", "\"number\":0"),
            ("https://", "file://"),
        ] {
            assert!(parse_job(&item_completion(&JOB.replace(from, to))).is_err());
        }
        let mut value: serde_json::Value = serde_json::from_str(JOB).unwrap();
        let step = value["steps"][0].clone();
        value["steps"] = serde_json::json!([step, step]);
        assert!(parse_job(&item_completion(&value.to_string())).is_err());
        assert_eq!(
            parse_job(&"x".repeat(MAXIMUM_RESPONSE_BYTES + 1)),
            Err(ParseError::LimitExceeded)
        );
    }
}

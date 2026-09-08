//! GitHub Issues and Pull Requests SENP extension.

use sakura_senp_github_client::{parse_issue_page, Issue, IssueState};
wit_bindgen::generate!({ path: "../../wit/v2/senp-extension.wit", world: "extension" });
use exports::sakura::senp::event_effects::*;

const ISSUES_VIEW: &str = "issues:github";
const PULL_REQUESTS_VIEW: &str = "pr:github";
const TOOL_ID: &str = "github";
const REPOSITORY_READ: &str = "repositoryRead";

struct GithubPullRequests;

impl Guest for GithubPullRequests {
    fn activate(_: Activation) -> Vec<Effect> {
        vec![invalidate(ISSUES_VIEW), invalidate(PULL_REQUESTS_VIEW)]
    }

    fn on_event(_: OperationContext, event: Event) -> Vec<Effect> {
        dispatch(event)
    }

    fn deactivate(_: StopReason) {}
}

fn invalidate(view_id: &str) -> Effect {
    Effect::InvalidateTree(InvalidateTree {
        view_id: view_id.into(),
    })
}

fn request_parts(cursor: &str) -> Option<(IssueState, u32)> {
    if cursor.is_empty() {
        return Some((IssueState::Open, 1));
    }
    let mut parts = cursor.split(':');
    match (parts.next(), parts.next(), parts.next(), parts.next()) {
        (Some("issues"), Some(state), Some(page), None) => Some((
            IssueState::parse(state)?,
            page.parse::<u32>().ok().filter(|value| *value > 0)?,
        )),
        (Some("filter"), Some(state), None, None) => Some((IssueState::parse(state)?, 1)),
        _ => None,
    }
}

fn read_id(state: IssueState, page: u32) -> String {
    format!("issues:{}:{page}", state.as_str())
}

fn start_read(state: IssueState, page: u32) -> Effect {
    Effect::StartToolRead(StartToolRead {
        read_id: read_id(state, page),
        tool_id: TOOL_ID.into(),
        operation: REPOSITORY_READ.into(),
        arguments: vec![
            Field {
                name: "path".into(),
                value: "issues".into(),
            },
            Field {
                name: "state".into(),
                value: state.as_str().into(),
            },
            Field {
                name: "sort".into(),
                value: "updated".into(),
            },
            Field {
                name: "direction".into(),
                value: "desc".into(),
            },
            Field {
                name: "per_page".into(),
                value: "50".into(),
            },
            Field {
                name: "page".into(),
                value: page.to_string(),
            },
        ],
    })
}

fn item(issue: Issue) -> TreeItem {
    let labels = issue.labels.join(", ");
    TreeItem {
        id: format!("issue:{}", issue.id),
        label: format!("#{} {}", issue.number, issue.title),
        description: format!("{} · @{}", issue.state.as_str(), issue.author),
        tooltip: if labels.is_empty() {
            issue.html_url
        } else {
            format!("{}\nLabels: {labels}", issue.html_url)
        },
        icon: "issues".into(),
        collapsible_state: CollapsibleState::Leaf,
        command_id: String::new(),
        arguments: Vec::new(),
    }
}

fn failed(message: impl Into<String>) -> Effect {
    Effect::PublishTreePage(PublishTreePage {
        view_id: ISSUES_VIEW.into(),
        parent_id: String::new(),
        items: Vec::new(),
        next_cursor: String::new(),
        revision: 1,
        status: PageStatus::Failed,
        message: message.into(),
    })
}

fn complete_tool(read: ToolCompleted) -> Effect {
    let Some(rest) = read.read_id.strip_prefix("issues:") else {
        return failed("Unknown GitHub read completion");
    };
    let Some((state, page)) = rest.split_once(':') else {
        return failed("Invalid GitHub read identity");
    };
    let Some(state) = IssueState::parse(state) else {
        return failed("Invalid GitHub state filter");
    };
    let Ok(page) = page.parse::<u32>() else {
        return failed("Invalid GitHub page identity");
    };
    if read.status != CompletionStatus::Succeeded {
        return failed(if read.message.is_empty() {
            "GitHub read failed".into()
        } else {
            read.message
        });
    }
    let page_result = match parse_issue_page(&read.data, state) {
        Ok(value) => value,
        Err(error) => return failed(error.to_string()),
    };
    if page_result.next_page.is_some_and(|next| next <= page) {
        return failed("Invalid GitHub next page");
    }
    let items: Vec<_> = page_result.issues.into_iter().map(item).collect();
    let next_cursor = page_result
        .next_page
        .map(|next| format!("issues:{}:{next}", state.as_str()))
        .unwrap_or_default();
    let status = if !next_cursor.is_empty() {
        PageStatus::Partial
    } else if items.is_empty() {
        PageStatus::Empty
    } else {
        PageStatus::Complete
    };
    Effect::PublishTreePage(PublishTreePage {
        view_id: ISSUES_VIEW.into(),
        parent_id: String::new(),
        items,
        next_cursor,
        revision: u64::from(page),
        status,
        message: String::new(),
    })
}

fn dispatch(event: Event) -> Vec<Effect> {
    match event {
        Event::TreeRequest(request)
            if request.view_id == ISSUES_VIEW && request.parent_id.is_empty() =>
        {
            match request_parts(&request.cursor) {
                Some((state, page)) => vec![start_read(state, page)],
                None => vec![failed("Invalid Issues page cursor")],
            }
        }
        Event::ToolCompleted(read) if read.read_id.starts_with("issues:") => {
            vec![complete_tool(read)]
        }
        Event::WorkspaceChanged(_) => vec![invalidate(ISSUES_VIEW), invalidate(PULL_REQUESTS_VIEW)],
        _ => Vec::new(),
    }
}

export!(GithubPullRequests);

#[cfg(test)]
mod tests {
    use super::*;

    const MIXED: &str = r#"{"body":[{"id":11,"number":7,"title":"Visible issue","state":"open","user":{"login":"octocat"},"labels":[{"name":"bug"}],"html_url":"https://github.com/o/r/issues/7"},{"id":12,"number":8,"title":"Filtered PR","state":"open","user":{"login":"hubot"},"labels":[],"html_url":"https://github.com/o/r/pull/8","pull_request":{}}],"nextPage":2}"#;

    #[test]
    fn starts_a_fixed_open_issue_read_and_supports_closed_filter() {
        for (cursor, expected_state) in [("", "open"), ("filter:closed", "closed")] {
            let effects = dispatch(Event::TreeRequest(TreeRequest {
                view_id: ISSUES_VIEW.into(),
                parent_id: String::new(),
                cursor: cursor.into(),
            }));
            let Effect::StartToolRead(read) = &effects[0] else {
                panic!()
            };
            assert_eq!(read.tool_id, TOOL_ID);
            assert_eq!(read.operation, REPOSITORY_READ);
            assert!(read
                .arguments
                .iter()
                .any(|field| field.name == "state" && field.value == expected_state));
            assert!(read
                .arguments
                .iter()
                .any(|field| field.name == "per_page" && field.value == "50"));
        }
    }

    #[test]
    fn publishes_only_issues_and_preserves_next_page_after_filtering() {
        let effects = dispatch(Event::ToolCompleted(ToolCompleted {
            read_id: "issues:open:1".into(),
            status: CompletionStatus::Succeeded,
            data: MIXED.into(),
            message: String::new(),
        }));
        let Effect::PublishTreePage(page) = &effects[0] else {
            panic!()
        };
        assert_eq!(page.view_id, ISSUES_VIEW);
        assert_eq!(page.items.len(), 1);
        assert_eq!(page.items[0].id, "issue:11");
        assert_eq!(page.items[0].label, "#7 Visible issue");
        assert_eq!(page.next_cursor, "issues:open:2");
        assert_eq!(page.status, PageStatus::Partial);
    }

    #[test]
    fn every_invalid_or_failed_completion_publishes_a_terminal_failed_page() {
        for completion in [
            ToolCompleted {
                read_id: "issues:open:1".into(),
                status: CompletionStatus::Failed,
                data: String::new(),
                message: "denied".into(),
            },
            ToolCompleted {
                read_id: "issues:open:1".into(),
                status: CompletionStatus::Succeeded,
                data: "{}".into(),
                message: String::new(),
            },
            ToolCompleted {
                read_id: "issues:future:1".into(),
                status: CompletionStatus::Succeeded,
                data: MIXED.into(),
                message: String::new(),
            },
        ] {
            let effects = dispatch(Event::ToolCompleted(completion));
            assert!(
                matches!(&effects[0], Effect::PublishTreePage(page) if page.status == PageStatus::Failed)
            );
        }
    }
}

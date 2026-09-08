//! GitHub Issues and Pull Requests SENP extension.

use sakura_senp_github_client::{
    parse_comment_detail, parse_comment_page, parse_issue_detail, parse_issue_page, Comment, Issue,
    IssueDetail, IssueState,
};
wit_bindgen::generate!({ path: "../../wit/v2/senp-extension.wit", world: "extension" });
use exports::sakura::senp::event_effects::*;

const ISSUES_VIEW: &str = "issues:github";
const PULL_REQUESTS_VIEW: &str = "pr:github";
const TOOL_ID: &str = "github";
const REPOSITORY_READ: &str = "repositoryRead";
const OPEN_ISSUE: &str = "github.openIssue";
const OPEN_COMMENT: &str = "github.openIssueComment";

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

fn repository_read(read_id: String, path: String, query: Vec<Field>) -> Effect {
    let mut arguments = vec![Field {
        name: "path".into(),
        value: path,
    }];
    arguments.extend(query);
    Effect::StartToolRead(StartToolRead {
        read_id,
        tool_id: TOOL_ID.into(),
        operation: REPOSITORY_READ.into(),
        arguments,
    })
}

fn positive(value: &str) -> Option<u64> {
    value.parse::<u64>().ok().filter(|number| *number > 0)
}

fn issue_identity(value: &str) -> Option<(u64, u64)> {
    let mut parts = value.split(':');
    match (parts.next(), parts.next(), parts.next(), parts.next()) {
        (Some("issue"), Some(id), Some(number), None) => Some((positive(id)?, positive(number)?)),
        _ => None,
    }
}

fn comments_request(parent_id: &str, cursor: &str) -> Option<Effect> {
    let (id, number) = issue_identity(parent_id)?;
    let page = if cursor.is_empty() {
        1
    } else {
        cursor
            .strip_prefix("comments:")?
            .parse::<u32>()
            .ok()
            .filter(|value| *value > 0)?
    };
    Some(repository_read(
        format!("comments:{id}:{number}:{page}"),
        format!("issues/{number}/comments"),
        vec![
            Field {
                name: "per_page".into(),
                value: "20".into(),
            },
            Field {
                name: "page".into(),
                value: page.to_string(),
            },
        ],
    ))
}

fn item(issue: Issue) -> TreeItem {
    let labels = issue.labels.join(", ");
    TreeItem {
        id: format!("issue:{}:{}", issue.id, issue.number),
        label: format!("#{} {}", issue.number, issue.title),
        description: format!("{} · @{}", issue.state.as_str(), issue.author),
        tooltip: if labels.is_empty() {
            issue.html_url
        } else {
            format!("{}\nLabels: {labels}", issue.html_url)
        },
        icon: "issues".into(),
        collapsible_state: if issue.comments == 0 {
            CollapsibleState::Leaf
        } else {
            CollapsibleState::Collapsed
        },
        command_id: OPEN_ISSUE.into(),
        arguments: vec![format!("github-issue:{}", issue.number)],
    }
}

fn comment_item(comment: Comment) -> TreeItem {
    let preview = comment.body.lines().next().unwrap_or_default();
    let preview = if preview.is_empty() {
        "No comment text"
    } else {
        preview
    };
    TreeItem {
        id: format!("comment:{}", comment.id),
        label: format!("@{}", comment.author),
        description: comment.created_at,
        tooltip: preview.chars().take(512).collect(),
        icon: "comment".into(),
        collapsible_state: CollapsibleState::Leaf,
        command_id: OPEN_COMMENT.into(),
        arguments: vec![format!("github-issue-comment:{}", comment.id)],
    }
}

fn failed_page(parent_id: String, revision: u64, message: impl Into<String>) -> Effect {
    Effect::PublishTreePage(PublishTreePage {
        view_id: ISSUES_VIEW.into(),
        parent_id,
        items: Vec::new(),
        next_cursor: String::new(),
        revision,
        status: PageStatus::Failed,
        message: message.into(),
    })
}

fn failed(message: impl Into<String>) -> Effect {
    failed_page(String::new(), 1, message)
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

fn complete_comments(read: ToolCompleted) -> Effect {
    let mut parts = read.read_id.split(':');
    let (Some("comments"), Some(id), Some(number), Some(page), None) = (
        parts.next(),
        parts.next(),
        parts.next(),
        parts.next(),
        parts.next(),
    ) else {
        return failed("Invalid GitHub comments read identity");
    };
    let (Some(id), Some(number), Some(page)) = (
        positive(id),
        positive(number),
        page.parse::<u32>().ok().filter(|value| *value > 0),
    ) else {
        return failed("Invalid GitHub comments read identity");
    };
    let parent_id = format!("issue:{id}:{number}");
    if read.status != CompletionStatus::Succeeded {
        return failed_page(
            parent_id,
            u64::from(page),
            if read.message.is_empty() {
                "GitHub comments read failed".into()
            } else {
                read.message
            },
        );
    }
    let result = match parse_comment_page(&read.data) {
        Ok(value) => value,
        Err(error) => return failed_page(parent_id, u64::from(page), error.to_string()),
    };
    if result.next_page.is_some_and(|next| next <= page) {
        return failed_page(
            parent_id,
            u64::from(page),
            "Invalid GitHub comments next page",
        );
    }
    let items: Vec<_> = result.comments.into_iter().map(comment_item).collect();
    let next_cursor = result
        .next_page
        .map(|next| format!("comments:{next}"))
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
        parent_id,
        items,
        next_cursor,
        revision: u64::from(page),
        status,
        message: String::new(),
    })
}

fn issue_document(detail: IssueDetail) -> Effect {
    let body = detail
        .body
        .filter(|value| !value.is_empty())
        .unwrap_or_else(|| "_No description provided._".into());
    let issue = detail.issue;
    Effect::PublishDocument(PublishDocument {
        resource_id: format!("github-issue:{}", issue.number),
        title: format!("#{} {}", issue.number, issue.title),
        revision: 1,
        sections: vec![
            DocumentSection::Metadata(MetadataSection {
                fields: vec![
                    Field {
                        name: "State".into(),
                        value: issue.state.as_str().into(),
                    },
                    Field {
                        name: "Author".into(),
                        value: format!("@{}", issue.author),
                    },
                    Field {
                        name: "Created".into(),
                        value: detail.created_at,
                    },
                    Field {
                        name: "Updated".into(),
                        value: detail.updated_at,
                    },
                    Field {
                        name: "Comments".into(),
                        value: issue.comments.to_string(),
                    },
                    Field {
                        name: "URL".into(),
                        value: issue.html_url,
                    },
                ],
            }),
            DocumentSection::Markdown(MarkdownSection { text: body }),
        ],
    })
}

fn comment_document(comment: Comment) -> Effect {
    let body = if comment.body.is_empty() {
        "_Empty comment._".into()
    } else {
        comment.body
    };
    Effect::PublishDocument(PublishDocument {
        resource_id: format!("github-issue-comment:{}", comment.id),
        title: format!("Comment by @{}", comment.author),
        revision: 1,
        sections: vec![
            DocumentSection::Metadata(MetadataSection {
                fields: vec![
                    Field {
                        name: "Author".into(),
                        value: format!("@{}", comment.author),
                    },
                    Field {
                        name: "Created".into(),
                        value: comment.created_at,
                    },
                    Field {
                        name: "Updated".into(),
                        value: comment.updated_at,
                    },
                    Field {
                        name: "URL".into(),
                        value: comment.html_url,
                    },
                ],
            }),
            DocumentSection::Markdown(MarkdownSection { text: body }),
        ],
    })
}

fn failed_document(resource_id: String, message: String) -> Effect {
    Effect::PublishDocument(PublishDocument {
        resource_id,
        title: "GitHub content unavailable".into(),
        revision: 1,
        sections: vec![
            DocumentSection::Metadata(MetadataSection {
                fields: vec![Field {
                    name: "Status".into(),
                    value: "Failed".into(),
                }],
            }),
            DocumentSection::Markdown(MarkdownSection {
                text: if message.is_empty() {
                    "Unable to load GitHub content.".into()
                } else {
                    message
                },
            }),
        ],
    })
}

fn complete_detail(read: ToolCompleted) -> Effect {
    if let Some(value) = read.read_id.strip_prefix("issue-detail:") {
        let Some(number) = positive(value) else {
            return failed("Invalid Issue detail identity");
        };
        let resource = format!("github-issue:{number}");
        if read.status != CompletionStatus::Succeeded {
            return failed_document(resource, read.message);
        }
        return match parse_issue_detail(&read.data) {
            Ok(detail) if detail.issue.number == number => issue_document(detail),
            Ok(_) => failed_document(resource, "Mismatched Issue detail".into()),
            Err(error) => failed_document(resource, error.to_string()),
        };
    }
    if let Some(value) = read.read_id.strip_prefix("comment-detail:") {
        let Some(id) = positive(value) else {
            return failed("Invalid comment detail identity");
        };
        let resource = format!("github-issue-comment:{id}");
        if read.status != CompletionStatus::Succeeded {
            return failed_document(resource, read.message);
        }
        return match parse_comment_detail(&read.data) {
            Ok(comment) if comment.id == id => comment_document(comment),
            Ok(_) => failed_document(resource, "Mismatched comment detail".into()),
            Err(error) => failed_document(resource, error.to_string()),
        };
    }
    failed("Unknown GitHub detail completion")
}

fn open_document(command: CommandInvoked) -> Vec<Effect> {
    let valid = command.arguments.len() == 1
        && command
            .arguments
            .first()
            .is_some_and(|argument| match command.command_id.as_str() {
                OPEN_ISSUE => argument
                    .strip_prefix("github-issue:")
                    .and_then(positive)
                    .is_some(),
                OPEN_COMMENT => argument
                    .strip_prefix("github-issue-comment:")
                    .and_then(positive)
                    .is_some(),
                _ => false,
            });
    if !valid {
        return vec![Effect::CompleteCommand(CompleteCommand {
            status: CompletionStatus::Failed,
            message: "Invalid GitHub document command".into(),
        })];
    }
    vec![
        Effect::OpenDocument(OpenDocument {
            resource_id: command.arguments[0].clone(),
        }),
        Effect::CompleteCommand(CompleteCommand {
            status: CompletionStatus::Succeeded,
            message: String::new(),
        }),
    ]
}

fn request_document(resource: &str) -> Option<Effect> {
    if let Some(value) = resource.strip_prefix("github-issue:") {
        let number = positive(value)?;
        return Some(repository_read(
            format!("issue-detail:{number}"),
            format!("issues/{number}"),
            Vec::new(),
        ));
    }
    if let Some(value) = resource.strip_prefix("github-issue-comment:") {
        let id = positive(value)?;
        return Some(repository_read(
            format!("comment-detail:{id}"),
            format!("issues/comments/{id}"),
            Vec::new(),
        ));
    }
    None
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
        Event::TreeRequest(request) if request.view_id == ISSUES_VIEW => {
            match comments_request(&request.parent_id, &request.cursor) {
                Some(effect) => vec![effect],
                None => vec![failed_page(
                    request.parent_id,
                    1,
                    "Invalid comments page request",
                )],
            }
        }
        Event::ToolCompleted(read) if read.read_id.starts_with("issues:") => {
            vec![complete_tool(read)]
        }
        Event::ToolCompleted(read) if read.read_id.starts_with("comments:") => {
            vec![complete_comments(read)]
        }
        Event::ToolCompleted(read) => vec![complete_detail(read)],
        Event::CommandInvoked(command)
            if command.command_id == OPEN_ISSUE || command.command_id == OPEN_COMMENT =>
        {
            open_document(command)
        }
        Event::DocumentRequest(request) => request_document(&request.resource_id)
            .map(|effect| vec![effect])
            .unwrap_or_default(),
        Event::WorkspaceChanged(_) => vec![invalidate(ISSUES_VIEW), invalidate(PULL_REQUESTS_VIEW)],
        _ => Vec::new(),
    }
}

export!(GithubPullRequests);

#[cfg(test)]
mod tests {
    use super::*;

    const MIXED: &str = r#"{"body":[{"id":11,"number":7,"title":"Visible issue","state":"open","user":{"login":"octocat"},"labels":[{"name":"bug"}],"html_url":"https://github.com/o/r/issues/7","comments":2},{"id":12,"number":8,"title":"Filtered PR","state":"open","user":{"login":"hubot"},"labels":[],"html_url":"https://github.com/o/r/pull/8","comments":0,"pull_request":{}}],"nextPage":2}"#;

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
        assert_eq!(page.items[0].id, "issue:11:7");
        assert_eq!(page.items[0].label, "#7 Visible issue");
        assert_eq!(page.items[0].collapsible_state, CollapsibleState::Collapsed);
        assert_eq!(page.items[0].command_id, OPEN_ISSUE);
        assert_eq!(page.next_cursor, "issues:open:2");
        assert_eq!(page.status, PageStatus::Partial);
    }

    #[test]
    fn opens_issue_and_comment_documents_with_fixed_repository_reads() {
        for (command_id, resource_id, expected_read, expected_path) in [
            (OPEN_ISSUE, "github-issue:7", "issue-detail:7", "issues/7"),
            (
                OPEN_COMMENT,
                "github-issue-comment:91",
                "comment-detail:91",
                "issues/comments/91",
            ),
        ] {
            let effects = dispatch(Event::CommandInvoked(CommandInvoked {
                command_id: command_id.into(),
                arguments: vec![resource_id.into()],
            }));
            assert!(matches!(
                &effects[..],
                [Effect::OpenDocument(open), Effect::CompleteCommand(done)]
                    if open.resource_id == resource_id
                        && done.status == CompletionStatus::Succeeded
            ));

            let effects = dispatch(Event::DocumentRequest(DocumentRequest {
                resource_id: resource_id.into(),
            }));
            let Effect::StartToolRead(read) = &effects[0] else {
                panic!()
            };
            assert_eq!(read.read_id, expected_read);
            assert_eq!(read.tool_id, TOOL_ID);
            assert_eq!(read.operation, REPOSITORY_READ);
            assert_eq!(
                read.arguments
                    .iter()
                    .find(|field| field.name == "path")
                    .map(|field| field.value.as_str()),
                Some(expected_path)
            );
        }
    }

    #[test]
    fn publishes_explicit_empty_issue_body_and_comment_body_documents() {
        let detail = r#"{"id":11,"number":7,"title":"Visible issue","state":"open","user":{"login":"octocat"},"labels":[],"html_url":"https://github.com/o/r/issues/7","comments":1,"body":null,"created_at":"2026-09-01T00:00:00Z","updated_at":"2026-09-02T00:00:00Z"}"#;
        let effects = dispatch(Event::ToolCompleted(ToolCompleted {
            read_id: "issue-detail:7".into(),
            status: CompletionStatus::Succeeded,
            data: detail.into(),
            message: String::new(),
        }));
        let Effect::PublishDocument(document) = &effects[0] else {
            panic!()
        };
        assert_eq!(document.resource_id, "github-issue:7");
        assert!(matches!(
            &document.sections[1],
            DocumentSection::Markdown(section) if section.text == "_No description provided._"
        ));

        let comment = r#"{"id":91,"user":{"login":"hubot"},"body":"","html_url":"https://github.com/o/r/issues/7#issuecomment-91","created_at":"2026-09-02T01:00:00Z","updated_at":"2026-09-02T01:00:00Z"}"#;
        let effects = dispatch(Event::ToolCompleted(ToolCompleted {
            read_id: "comment-detail:91".into(),
            status: CompletionStatus::Succeeded,
            data: comment.into(),
            message: String::new(),
        }));
        let Effect::PublishDocument(document) = &effects[0] else {
            panic!()
        };
        assert_eq!(document.resource_id, "github-issue-comment:91");
        assert!(matches!(
            &document.sections[1],
            DocumentSection::Markdown(section) if section.text == "_Empty comment._"
        ));
    }

    #[test]
    fn publishes_paged_comments_and_terminal_empty_or_failed_children() {
        let page = r#"{"body":[{"id":91,"user":{"login":"hubot"},"body":"First line\nMore","html_url":"https://github.com/o/r/issues/7#issuecomment-91","created_at":"2026-09-02T01:00:00Z","updated_at":"2026-09-02T01:00:00Z"}],"nextPage":2}"#;
        let effects = dispatch(Event::ToolCompleted(ToolCompleted {
            read_id: "comments:11:7:1".into(),
            status: CompletionStatus::Succeeded,
            data: page.into(),
            message: String::new(),
        }));
        let Effect::PublishTreePage(result) = &effects[0] else {
            panic!()
        };
        assert_eq!(result.parent_id, "issue:11:7");
        assert_eq!(result.items[0].id, "comment:91");
        assert_eq!(result.items[0].tooltip, "First line");
        assert_eq!(result.next_cursor, "comments:2");
        assert_eq!(result.status, PageStatus::Partial);

        let effects = dispatch(Event::ToolCompleted(ToolCompleted {
            read_id: "comments:11:7:2".into(),
            status: CompletionStatus::Succeeded,
            data: r#"{"body":[]}"#.into(),
            message: String::new(),
        }));
        assert!(matches!(
            &effects[0],
            Effect::PublishTreePage(result)
                if result.parent_id == "issue:11:7" && result.status == PageStatus::Empty
        ));

        let effects = dispatch(Event::ToolCompleted(ToolCompleted {
            read_id: "comments:11:7:1".into(),
            status: CompletionStatus::Failed,
            data: String::new(),
            message: "denied".into(),
        }));
        assert!(matches!(
            &effects[0],
            Effect::PublishTreePage(result)
                if result.parent_id == "issue:11:7"
                    && result.status == PageStatus::Failed
                    && result.message == "denied"
        ));
    }

    #[test]
    fn invalid_document_commands_and_detail_reads_finish_explicitly() {
        let effects = dispatch(Event::CommandInvoked(CommandInvoked {
            command_id: OPEN_ISSUE.into(),
            arguments: vec!["github-issue:0".into()],
        }));
        assert!(matches!(
            &effects[0],
            Effect::CompleteCommand(done) if done.status == CompletionStatus::Failed
        ));

        let effects = dispatch(Event::ToolCompleted(ToolCompleted {
            read_id: "issue-detail:7".into(),
            status: CompletionStatus::Failed,
            data: String::new(),
            message: "not found".into(),
        }));
        assert!(matches!(
            &effects[0],
            Effect::PublishDocument(document)
                if document.resource_id == "github-issue:7"
                    && document.title == "GitHub content unavailable"
        ));
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

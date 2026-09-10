//! Deterministic, GitHub-free SENP v2 sample used by the native vertical slice.

wit_bindgen::generate!({ path: "../../wit/v2/senp-extension.wit", world: "extension" });
use exports::sakura::senp::event_effects::*;

const PROJECTS_VIEW: &str = "sample.projects";
const STATES_VIEW: &str = "sample.states";
const OPEN_DETAILS: &str = "sample.openDetails";
const DETAILS_RESOURCE: &str = "sample:details/alpha";

struct Sample;

impl Guest for Sample {
    fn activate(_: Activation) -> Vec<Effect> {
        vec![invalidate(PROJECTS_VIEW), invalidate(STATES_VIEW)]
    }

    fn on_event(_: OperationContext, event: Event) -> Vec<Effect> {
        dispatch(event)
    }

    fn deactivate(_: StopReason) {}
}

fn dispatch(event: Event) -> Vec<Effect> {
    match event {
        Event::TreeRequest(request) => vec![Effect::PublishTreePage(tree_page(request))],
        Event::CommandInvoked(command) if command.command_id == OPEN_DETAILS => vec![
            Effect::OpenDocument(OpenDocument {
                resource_id: DETAILS_RESOURCE.into(),
            }),
            Effect::CompleteCommand(CompleteCommand {
                status: CompletionStatus::Succeeded,
                message: String::new(),
            }),
        ],
        Event::DocumentRequest(request) if request.resource_id == DETAILS_RESOURCE => {
            vec![Effect::PublishDocument(details_document())]
        }
        Event::WorkspaceChanged(_) => vec![invalidate(PROJECTS_VIEW)],
        Event::VisibilityChanged(_)
        | Event::Cancel(_)
        | Event::ToolCompleted(_)
        | Event::DocumentRequest(_)
        | Event::CommandInvoked(_) => Vec::new(),
    }
}

fn invalidate(view_id: &str) -> Effect {
    Effect::InvalidateTree(InvalidateTree {
        view_id: view_id.into(),
    })
}

fn item(id: &str, label: &str, state: CollapsibleState) -> TreeItem {
    TreeItem {
        id: id.into(),
        label: label.into(),
        description: String::new(),
        tooltip: label.into(),
        icon: String::new(),
        collapsible_state: state,
        command_id: String::new(),
        arguments: Vec::new(),
        context_value: String::new(),
    }
}

fn tree_page(request: TreeRequest) -> PublishTreePage {
    let mut page = PublishTreePage {
        view_id: request.view_id.clone(),
        parent_id: request.parent_id.clone(),
        items: Vec::new(),
        next_cursor: String::new(),
        revision: 1,
        status: PageStatus::Complete,
        message: String::new(),
    };
    match (
        request.view_id.as_str(),
        request.parent_id.as_str(),
        request.cursor.as_str(),
    ) {
        (PROJECTS_VIEW, "", "") => {
            let mut alpha = item("project:alpha", "Alpha", CollapsibleState::Leaf);
            alpha.description = "Ready".into();
            alpha.command_id = OPEN_DETAILS.into();
            alpha.arguments = vec![DETAILS_RESOURCE.into()];
            page.items = vec![alpha, item("project:beta", "Beta", CollapsibleState::Leaf)];
        }
        (STATES_VIEW, "", "") => {
            page.items = vec![
                item(
                    "state:loading",
                    "Loading and paging",
                    CollapsibleState::Collapsed,
                ),
                item("state:empty", "Empty", CollapsibleState::Collapsed),
                item("state:error", "Error", CollapsibleState::Collapsed),
            ];
        }
        (STATES_VIEW, "state:loading", "") => {
            page.status = PageStatus::Partial;
            page.items = vec![item(
                "state:loading/first",
                "First page",
                CollapsibleState::Leaf,
            )];
            page.next_cursor = "page:2".into();
        }
        (STATES_VIEW, "state:loading", "page:2") => {
            page.items = vec![item(
                "state:loading/second",
                "Second page",
                CollapsibleState::Leaf,
            )];
            page.revision = 2;
        }
        (STATES_VIEW, "state:empty", "") => page.status = PageStatus::Empty,
        (STATES_VIEW, "state:error", "") => {
            page.status = PageStatus::Failed;
            page.message = "Sample failure".into();
        }
        _ => {
            page.status = PageStatus::Failed;
            page.message = "Unknown sample request".into();
        }
    }
    page
}

fn details_document() -> PublishDocument {
    PublishDocument {
        resource_id: DETAILS_RESOURCE.into(),
        title: "Alpha details".into(),
        revision: 1,
        sections: vec![
            DocumentSection::Markdown(MarkdownSection {
                text: "# Alpha\n\nA deterministic SENP v2 sample document.".into(),
            }),
            DocumentSection::Metadata(MetadataSection {
                fields: vec![
                    Field {
                        name: "State".into(),
                        value: "Ready".into(),
                    },
                    Field {
                        name: "Source".into(),
                        value: "Built-in sample".into(),
                    },
                ],
            }),
            DocumentSection::Table(TableSection {
                columns: vec!["Check".into(), "Result".into()],
                rows: vec![
                    TableRow {
                        cells: vec!["Activation".into(), "Passed".into()],
                    },
                    TableRow {
                        cells: vec!["Native projection".into(), "Ready".into()],
                    },
                ],
            }),
            DocumentSection::TextResource(TextResourceSection {
                handle: "sample:log/alpha".into(),
                length: 0,
                status: TextStatus::Loading,
            }),
        ],
    }
}

export!(Sample);

#[cfg(test)]
mod tests {
    use super::*;

    fn context() -> OperationContext {
        OperationContext {
            operation_id: "test".into(),
            owner_generation: 1,
            workspace_revision: 1,
            account_generation: 1,
            request_generation: 1,
        }
    }

    #[test]
    fn exposes_two_independent_tree_views_and_all_terminal_states() {
        let Event::TreeRequest(_) = Event::TreeRequest(TreeRequest {
            view_id: PROJECTS_VIEW.into(),
            parent_id: String::new(),
            cursor: String::new(),
        }) else {
            unreachable!()
        };
        let projects = dispatch(Event::TreeRequest(TreeRequest {
            view_id: PROJECTS_VIEW.into(),
            parent_id: String::new(),
            cursor: String::new(),
        }));
        let Effect::PublishTreePage(projects) = &projects[0] else {
            panic!()
        };
        assert_eq!(projects.items.len(), 2);
        assert_eq!(projects.items[0].command_id, OPEN_DETAILS);

        for (parent, status) in [
            ("state:loading", PageStatus::Partial),
            ("state:empty", PageStatus::Empty),
            ("state:error", PageStatus::Failed),
        ] {
            let result = dispatch(Event::TreeRequest(TreeRequest {
                view_id: STATES_VIEW.into(),
                parent_id: parent.into(),
                cursor: String::new(),
            }));
            let Effect::PublishTreePage(page) = &result[0] else {
                panic!()
            };
            assert_eq!(page.status, status);
        }
    }

    #[test]
    fn opens_structured_details_with_a_loading_text_resource() {
        let command = dispatch(Event::CommandInvoked(CommandInvoked {
            command_id: OPEN_DETAILS.into(),
            arguments: vec![DETAILS_RESOURCE.into()],
        }));
        assert!(matches!(
            command.as_slice(),
            [Effect::OpenDocument(_), Effect::CompleteCommand(_)]
        ));
        let result = Sample::on_event(
            context(),
            Event::DocumentRequest(DocumentRequest {
                resource_id: DETAILS_RESOURCE.into(),
            }),
        );
        let Effect::PublishDocument(document) = &result[0] else {
            panic!()
        };
        assert_eq!(document.sections.len(), 4);
        assert!(
            matches!(document.sections.last(), Some(DocumentSection::TextResource(section))
            if section.status == TextStatus::Loading)
        );
    }
}

//! Bounded GitHub REST DTO conversion shared by the built-in SENP extensions.

use serde::de::{self, DeserializeOwned, Deserializer, MapAccess, SeqAccess, Visitor};
use serde::Deserialize;
use std::collections::BTreeSet;
use std::fmt;

pub const MAXIMUM_PAGE_ITEMS: usize = 100;
pub const MAXIMUM_RESPONSE_BYTES: usize = 64 * 1024;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum IssueState {
    Open,
    Closed,
}

impl IssueState {
    pub fn as_str(self) -> &'static str {
        match self {
            Self::Open => "open",
            Self::Closed => "closed",
        }
    }

    pub fn parse(value: &str) -> Option<Self> {
        match value {
            "open" => Some(Self::Open),
            "closed" => Some(Self::Closed),
            _ => None,
        }
    }
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Issue {
    pub id: u64,
    pub number: u64,
    pub title: String,
    pub state: IssueState,
    pub author: String,
    pub labels: Vec<String>,
    pub html_url: String,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct IssuePage {
    pub issues: Vec<Issue>,
    pub next_page: Option<u32>,
    pub excluded_pull_requests: usize,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub enum ParseError {
    InvalidJson,
    InvalidEnvelope,
    InvalidItem,
    LimitExceeded,
}

impl fmt::Display for ParseError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(match self {
            Self::InvalidJson => "invalid GitHub response JSON",
            Self::InvalidEnvelope => "invalid repository response envelope",
            Self::InvalidItem => "invalid Issue response item",
            Self::LimitExceeded => "GitHub response exceeds the extension budget",
        })
    }
}

#[derive(Deserialize)]
#[serde(rename_all = "camelCase")]
struct Envelope {
    body: Vec<ApiIssue>,
    #[serde(default)]
    next_page: Option<u32>,
}

#[derive(Deserialize)]
struct ApiIssue {
    id: u64,
    number: u64,
    title: String,
    state: String,
    user: ApiUser,
    #[serde(default)]
    labels: Vec<ApiLabel>,
    html_url: String,
    #[serde(default)]
    pull_request: PullRequestMarker,
}

#[derive(Deserialize)]
struct ApiUser {
    login: String,
}

#[derive(Deserialize)]
struct ApiLabel {
    name: String,
}

#[derive(Default)]
enum PullRequestMarker {
    #[default]
    Absent,
    Present,
}

impl<'de> Deserialize<'de> for PullRequestMarker {
    fn deserialize<D>(deserializer: D) -> Result<Self, D::Error>
    where
        D: Deserializer<'de>,
    {
        let _ = serde_json::Value::deserialize(deserializer)?;
        Ok(Self::Present)
    }
}

struct NoDuplicateJson(serde_json::Value);

impl<'de> Deserialize<'de> for NoDuplicateJson {
    fn deserialize<D>(deserializer: D) -> Result<Self, D::Error>
    where
        D: Deserializer<'de>,
    {
        struct JsonVisitor;
        impl<'de> Visitor<'de> for JsonVisitor {
            type Value = NoDuplicateJson;
            fn expecting(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
                formatter.write_str("JSON value")
            }
            fn visit_bool<E: de::Error>(self, value: bool) -> Result<Self::Value, E> {
                Ok(NoDuplicateJson(value.into()))
            }
            fn visit_i64<E: de::Error>(self, value: i64) -> Result<Self::Value, E> {
                Ok(NoDuplicateJson(value.into()))
            }
            fn visit_u64<E: de::Error>(self, value: u64) -> Result<Self::Value, E> {
                Ok(NoDuplicateJson(value.into()))
            }
            fn visit_f64<E: de::Error>(self, value: f64) -> Result<Self::Value, E> {
                serde_json::Number::from_f64(value)
                    .map(|number| NoDuplicateJson(number.into()))
                    .ok_or_else(|| E::custom("non-finite number"))
            }
            fn visit_str<E: de::Error>(self, value: &str) -> Result<Self::Value, E> {
                Ok(NoDuplicateJson(value.into()))
            }
            fn visit_string<E: de::Error>(self, value: String) -> Result<Self::Value, E> {
                Ok(NoDuplicateJson(value.into()))
            }
            fn visit_none<E: de::Error>(self) -> Result<Self::Value, E> {
                Ok(NoDuplicateJson(serde_json::Value::Null))
            }
            fn visit_unit<E: de::Error>(self) -> Result<Self::Value, E> {
                self.visit_none()
            }
            fn visit_seq<A: SeqAccess<'de>>(
                self,
                mut sequence: A,
            ) -> Result<Self::Value, A::Error> {
                let mut values = Vec::new();
                while let Some(value) = sequence.next_element::<NoDuplicateJson>()? {
                    values.push(value.0);
                }
                Ok(NoDuplicateJson(values.into()))
            }
            fn visit_map<A: MapAccess<'de>>(self, mut map: A) -> Result<Self::Value, A::Error> {
                let mut values = serde_json::Map::new();
                while let Some((key, value)) = map.next_entry::<String, NoDuplicateJson>()? {
                    if values.insert(key.clone(), value.0).is_some() {
                        return Err(de::Error::custom(format!("duplicate member: {key}")));
                    }
                }
                Ok(NoDuplicateJson(values.into()))
            }
        }
        deserializer.deserialize_any(JsonVisitor)
    }
}

fn strict_json<T: DeserializeOwned>(data: &str) -> Result<T, ParseError> {
    let mut deserializer = serde_json::Deserializer::from_str(data);
    let value =
        NoDuplicateJson::deserialize(&mut deserializer).map_err(|_| ParseError::InvalidJson)?;
    deserializer.end().map_err(|_| ParseError::InvalidJson)?;
    serde_json::from_value(value.0).map_err(|_| ParseError::InvalidJson)
}

fn bounded_text(value: &str, maximum: usize) -> bool {
    !value.is_empty() && value.len() <= maximum && !value.contains('\0')
}

pub fn parse_issue_page(data: &str, filter: IssueState) -> Result<IssuePage, ParseError> {
    if data.len() > MAXIMUM_RESPONSE_BYTES {
        return Err(ParseError::LimitExceeded);
    }
    let envelope: Envelope = strict_json(data)?;
    if envelope.body.len() > MAXIMUM_PAGE_ITEMS || envelope.next_page.is_some_and(|page| page == 0)
    {
        return Err(ParseError::InvalidEnvelope);
    }
    let mut ids = BTreeSet::new();
    let mut issues = Vec::new();
    let mut excluded_pull_requests = 0usize;
    for value in envelope.body {
        if matches!(value.pull_request, PullRequestMarker::Present) {
            excluded_pull_requests += 1;
            continue;
        }
        let state = IssueState::parse(&value.state).ok_or(ParseError::InvalidItem)?;
        if value.id == 0
            || value.number == 0
            || !ids.insert(value.id)
            || !bounded_text(&value.title, 1024)
            || !bounded_text(&value.user.login, 128)
            || !bounded_text(&value.html_url, 2048)
            || !value.html_url.starts_with("https://")
            || value.labels.len() > 32
            || value
                .labels
                .iter()
                .any(|label| !bounded_text(&label.name, 128))
        {
            return Err(ParseError::InvalidItem);
        }
        if state == filter {
            issues.push(Issue {
                id: value.id,
                number: value.number,
                title: value.title,
                state,
                author: value.user.login,
                labels: value.labels.into_iter().map(|label| label.name).collect(),
                html_url: value.html_url,
            });
        }
    }
    Ok(IssuePage {
        issues,
        next_page: envelope.next_page,
        excluded_pull_requests,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    const MIXED: &str = r#"{"body":[{"id":11,"number":7,"title":"First issue","state":"open","user":{"login":"octocat"},"labels":[{"name":"bug"}],"html_url":"https://github.com/o/r/issues/7"},{"id":12,"number":8,"title":"A pull request","state":"open","user":{"login":"hubot"},"labels":[],"html_url":"https://github.com/o/r/pull/8","pull_request":{"url":"https://api.github.com/repos/o/r/pulls/8"}}],"nextPage":2,"ignored":"allowed"}"#;

    #[test]
    fn excludes_pull_requests_without_consuming_the_next_page() {
        let page = parse_issue_page(MIXED, IssueState::Open).unwrap();
        assert_eq!(page.issues.len(), 1);
        assert_eq!(page.issues[0].number, 7);
        assert_eq!(page.issues[0].labels, ["bug"]);
        assert_eq!(page.excluded_pull_requests, 1);
        assert_eq!(page.next_page, Some(2));
    }

    #[test]
    fn applies_the_requested_state_filter_and_keeps_pagination() {
        let page = parse_issue_page(MIXED, IssueState::Closed).unwrap();
        assert!(page.issues.is_empty());
        assert_eq!(page.next_page, Some(2));
    }

    #[test]
    fn accepts_unknown_fields_but_rejects_bad_required_fields_and_bounds() {
        let unknown = MIXED.replace(
            "\"title\":\"First issue\"",
            "\"title\":\"First issue\",\"future\":true",
        );
        assert!(parse_issue_page(&unknown, IssueState::Open).is_ok());
        for invalid in [
            MIXED.replace("\"id\":11", "\"id\":0"),
            MIXED.replace("\"state\":\"open\"", "\"state\":\"future\""),
            MIXED.replace("https://github.com/o/r/issues/7", "http://example.test/7"),
            "{}".into(),
        ] {
            assert!(parse_issue_page(&invalid, IssueState::Open).is_err());
        }
        assert_eq!(
            parse_issue_page(&"x".repeat(MAXIMUM_RESPONSE_BYTES + 1), IssueState::Open),
            Err(ParseError::LimitExceeded)
        );
        assert_eq!(
            parse_issue_page(
                &MIXED.replace("\"id\":11", "\"id\":11,\"id\":13"),
                IssueState::Open
            ),
            Err(ParseError::InvalidJson)
        );
        let null_marker = MIXED.replace(
            "\"pull_request\":{\"url\":\"https://api.github.com/repos/o/r/pulls/8\"}",
            "\"pull_request\":null",
        );
        assert_eq!(
            parse_issue_page(&null_marker, IssueState::Open)
                .unwrap()
                .excluded_pull_requests,
            1
        );
    }
}

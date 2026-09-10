//! Upstream's status icons, status strings and trigger strings.
//!
//! These follow github/vscode-github-actions `src/treeViews/icons.ts` and
//! `src/treeViews/shared/runTooltipHelper.ts`. Icons are named by the path
//! upstream resolves inside its package; the host draws that bundled
//! vocabulary and nothing else. Tooltips are plain text: there is no Markdown
//! tooltip contract, and the actor and relative time need a clock and a
//! timezone this extension does not have.

const RUN_ICONS: &str = "resources/icons/workflowruns/wr_";
const STEP_ICONS: &str = "resources/icons/steps/step_";

/// The icon upstream shows for a run or a job, or none for a state it does not
/// name.
pub fn run_icon(status: &str, conclusion: Option<&str>) -> String {
    let name = if status == "completed" {
        match conclusion {
            Some("success") => "success",
            Some("failure" | "startup_failure") => "failure",
            Some("skipped") => "skipped",
            Some("cancelled") => "cancelled",
            _ => return String::new(),
        }
    } else {
        match status {
            "pending" => "pending",
            "requested" | "queued" => "queued",
            "waiting" => "waiting",
            "inprogress" | "in_progress" => "inprogress",
            _ => return String::new(),
        }
    };
    format!("{RUN_ICONS}{name}.svg")
}

/// The icon upstream shows for a step, or none for a state it does not name.
pub fn step_icon(status: &str, conclusion: Option<&str>) -> String {
    let name = if status == "completed" {
        match conclusion {
            Some(name @ ("success" | "failure" | "skipped" | "cancelled")) => name,
            _ => return String::new(),
        }
    } else {
        match status {
            "queued" => "queued",
            "inprogress" | "in_progress" => "inprogress",
            _ => return String::new(),
        }
    };
    format!("{STEP_ICONS}{name}.svg")
}

/// Upstream's capitalized status string: the conclusion when there is one,
/// otherwise the status, followed by how long a concluded, unskipped piece of
/// work took when both of its timestamps are known.
pub fn status_text(
    status: &str,
    conclusion: Option<&str>,
    started: Option<&str>,
    ended: Option<&str>,
) -> String {
    let conclusion = conclusion.filter(|value| !value.is_empty());
    let text = match conclusion.unwrap_or(status) {
        "success" => "succeeded",
        "failure" => "failed",
        other => other,
    };
    // Upstream replaces only the first underscore.
    let text = text.replacen('_', " ", 1);
    let mut chars = text.chars();
    let mut result: String = match chars.next() {
        Some(first) => first.to_uppercase().chain(chars).collect(),
        None => String::new(),
    };
    if conclusion.is_some_and(|value| value != "skipped") {
        if let Some(duration) = started
            .zip(ended)
            .and_then(|(start, end)| duration(start, end))
        {
            result.push_str(" in ");
            result.push_str(&duration);
        }
    }
    result
}

/// Upstream's trigger line, without its actor and time suffix.
pub fn event_text(event: &str, attempt: u32) -> String {
    if attempt > 1 {
        return "Re-run".into();
    }
    match event {
        "workflow_dispatch" => "Manually triggered".into(),
        "dynamic" => "Triggered".into(),
        _ => format!("Triggered via {}", event.replacen('_', " ", 1)),
    }
}

/// `Dd Hh Mm Ss` with leading zero units dropped, as upstream prints it.
fn duration(start: &str, end: &str) -> Option<String> {
    let seconds = timestamp(end)?.checked_sub(timestamp(start)?)?;
    if seconds < 0 {
        return None;
    }
    let text = format!(
        "{}d {}h {}m {}s",
        seconds / 86_400,
        seconds / 3_600 % 24,
        seconds / 60 % 60,
        seconds % 60
    );
    let mut rest = text.as_str();
    for zero in ["0d ", "0h ", "0m "] {
        match rest.strip_prefix(zero) {
            Some(stripped) => rest = stripped,
            None => break,
        }
    }
    Some(rest.to_owned())
}

/// Seconds since the Unix epoch for GitHub's `YYYY-MM-DDTHH:MM:SS[.fff]Z`.
fn timestamp(value: &str) -> Option<i64> {
    let value = value.strip_suffix('Z')?;
    let (date, time) = value.split_once('T')?;
    let time = time.split_once('.').map_or(time, |(whole, fraction)| {
        if fraction.bytes().all(|byte| byte.is_ascii_digit()) {
            whole
        } else {
            ""
        }
    });
    let date: Vec<&str> = date.split('-').collect();
    let time: Vec<&str> = time.split(':').collect();
    let [year, month, day] = date[..] else {
        return None;
    };
    let [hour, minute, second] = time[..] else {
        return None;
    };
    let field = |text: &str, width: usize, max: i64| -> Option<i64> {
        if text.len() != width || !text.bytes().all(|byte| byte.is_ascii_digit()) {
            return None;
        }
        text.parse().ok().filter(|number| *number <= max)
    };
    let (year, month, day) = (
        field(year, 4, 9999)?,
        field(month, 2, 12)?,
        field(day, 2, 31)?,
    );
    let (hour, minute, second) = (
        field(hour, 2, 23)?,
        field(minute, 2, 59)?,
        field(second, 2, 60)?,
    );
    if month == 0 || day == 0 {
        return None;
    }
    Some(days_from_civil(year, month, day) * 86_400 + hour * 3_600 + minute * 60 + second)
}

/// Days since 1970-01-01 in the proleptic Gregorian calendar.
fn days_from_civil(year: i64, month: i64, day: i64) -> i64 {
    let year = if month <= 2 { year - 1 } else { year };
    let era = year.div_euclid(400);
    let year_of_era = year - era * 400;
    let month_index = (month + 9) % 12;
    let day_of_year = (153 * month_index + 2) / 5 + day - 1;
    let day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    era * 146_097 + day_of_era - 719_468
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn run_and_step_icons_follow_the_upstream_state_tables() {
        for (status, conclusion, icon) in [
            ("completed", Some("success"), "wr_success"),
            ("completed", Some("failure"), "wr_failure"),
            ("completed", Some("startup_failure"), "wr_failure"),
            ("completed", Some("skipped"), "wr_skipped"),
            ("completed", Some("cancelled"), "wr_cancelled"),
            ("pending", None, "wr_pending"),
            ("requested", None, "wr_queued"),
            ("queued", None, "wr_queued"),
            ("waiting", None, "wr_waiting"),
            ("in_progress", None, "wr_inprogress"),
        ] {
            assert_eq!(
                run_icon(status, conclusion),
                format!("resources/icons/workflowruns/{icon}.svg")
            );
        }
        assert_eq!(run_icon("completed", Some("neutral")), "");
        assert_eq!(run_icon("completed", None), "");
        assert_eq!(run_icon("new_state", None), "");

        for (status, conclusion, icon) in [
            ("completed", Some("success"), "step_success"),
            ("completed", Some("failure"), "step_failure"),
            ("completed", Some("skipped"), "step_skipped"),
            ("completed", Some("cancelled"), "step_cancelled"),
            ("queued", None, "step_queued"),
            ("in_progress", None, "step_inprogress"),
        ] {
            assert_eq!(
                step_icon(status, conclusion),
                format!("resources/icons/steps/{icon}.svg")
            );
        }
        // Upstream names no step icon for these.
        assert_eq!(step_icon("completed", Some("startup_failure")), "");
        assert_eq!(step_icon("pending", None), "");
        assert_eq!(step_icon("waiting", None), "");
    }

    #[test]
    fn status_text_matches_upstream_wording_and_duration() {
        let start = Some("2026-09-01T00:00:00Z");
        assert_eq!(status_text("in_progress", None, start, None), "In progress");
        assert_eq!(
            status_text(
                "completed",
                Some("success"),
                start,
                Some("2026-09-01T00:01:05Z")
            ),
            "Succeeded in 1m 5s"
        );
        assert_eq!(
            status_text(
                "completed",
                Some("failure"),
                start,
                Some("2026-09-01T00:00:09.500Z")
            ),
            "Failed in 9s"
        );
        assert_eq!(
            status_text(
                "completed",
                Some("timed_out"),
                start,
                Some("2026-09-02T01:00:00Z")
            ),
            "Timed out in 1d 1h 0m 0s"
        );
        // A skipped conclusion never carries a duration, and neither does one
        // whose timestamps are missing, malformed or reversed.
        assert_eq!(
            status_text(
                "completed",
                Some("skipped"),
                start,
                Some("2026-09-01T00:01:00Z")
            ),
            "Skipped"
        );
        assert_eq!(
            status_text("completed", Some("cancelled"), None, start),
            "Cancelled"
        );
        assert_eq!(
            status_text(
                "completed",
                Some("success"),
                Some("2026-09-01 00:00:00"),
                start
            ),
            "Succeeded"
        );
        assert_eq!(
            status_text(
                "completed",
                Some("success"),
                Some("2026-09-01T00:00:10Z"),
                start
            ),
            "Succeeded"
        );
        // Upstream replaces only the first underscore.
        assert_eq!(
            status_text("some_new_state", None, None, None),
            "Some new_state"
        );
        assert_eq!(status_text("", None, None, None), "");
    }

    #[test]
    fn event_text_matches_upstream_trigger_wording() {
        assert_eq!(event_text("push", 1), "Triggered via push");
        assert_eq!(event_text("pull_request", 1), "Triggered via pull request");
        assert_eq!(
            event_text("pull_request_target", 1),
            "Triggered via pull request_target"
        );
        assert_eq!(event_text("workflow_dispatch", 1), "Manually triggered");
        assert_eq!(event_text("dynamic", 1), "Triggered");
        assert_eq!(event_text("push", 2), "Re-run");
    }

    #[test]
    fn timestamps_are_utc_seconds_since_the_epoch() {
        assert_eq!(timestamp("1970-01-01T00:00:00Z"), Some(0));
        assert_eq!(timestamp("2000-03-01T00:00:00Z"), Some(951_868_800));
        assert_eq!(timestamp("2026-09-11T12:34:56.789Z"), Some(1_789_130_096));
        for invalid in [
            "2026-09-11T12:34:56",
            "2026-09-11T12:34:56+09:00",
            "2026-13-01T00:00:00Z",
            "2026-00-01T00:00:00Z",
            "26-09-11T12:34:56Z",
            "2026-09-11T12:34:56.x9Z",
            "",
        ] {
            assert_eq!(timestamp(invalid), None, "{invalid}");
        }
    }
}

wit_bindgen::generate!({
    path: "../../wit/senp-extension.wit",
    world: "extension",
});

use exports::sakura::senp::editor_decorations::{
    DecorationRequest, DecorationSlot, ExtensionError, Guest,
};

struct IndentRainbow;

impl Guest for IndentRainbow {
    fn decorate(request: DecorationRequest) -> Result<Vec<DecorationSlot>, ExtensionError> {
        const MAX_LINES: usize = 2_000;
        const MAX_INPUT_BYTES: usize = 4 * 1024 * 1024;
        const MAX_OUTPUTS: usize = 10_000;
        if !(1..=32).contains(&request.tab_size) {
            return Err(ExtensionError::InvalidRequest(
                "tab-size must be between 1 and 32".into(),
            ));
        }
        if request.lines.len() > MAX_LINES {
            return Err(ExtensionError::LimitExceeded("visible line count".into()));
        }
        let input_bytes = request
            .lines
            .iter()
            .try_fold(0usize, |total, line| total.checked_add(line.text.len()))
            .ok_or_else(|| ExtensionError::LimitExceeded("visible text bytes".into()))?;
        if input_bytes > MAX_INPUT_BYTES {
            return Err(ExtensionError::LimitExceeded("visible text bytes".into()));
        }
        let mut result = Vec::new();
        for line in request.lines {
            let mut indentation_width = 0u32;
            for character in line.text.chars() {
                let width = match character {
                    ' ' => 1,
                    '\t' => request.tab_size - (indentation_width % request.tab_size),
                    _ => break,
                };
                indentation_width = indentation_width
                    .checked_add(width)
                    .ok_or_else(|| ExtensionError::LimitExceeded("indentation width".into()))?;
            }
            let mut visual_start = 0u32;
            while visual_start < indentation_width {
                if result.len() >= MAX_OUTPUTS {
                    return Err(ExtensionError::LimitExceeded("decoration count".into()));
                }
                let visual_length = (indentation_width - visual_start).min(request.tab_size);
                result.push(DecorationSlot {
                    line: line.line,
                    visual_start,
                    visual_length,
                    depth: visual_start / request.tab_size,
                });
                visual_start += visual_length;
            }
        }
        Ok(result)
    }
}

export!(IndentRainbow);

#[cfg(test)]
mod tests {
    use super::*;
    use crate::exports::sakura::senp::editor_decorations::VisibleLine;

    fn decorate(text: &str, tab_size: u32) -> Result<Vec<DecorationSlot>, ExtensionError> {
        IndentRainbow::decorate(DecorationRequest {
            revision: 7,
            tab_size,
            lines: vec![VisibleLine {
                line: 11,
                text: text.to_owned(),
            }],
        })
    }

    #[test]
    fn empty_and_unindented_lines_produce_no_slots() {
        assert!(decorate("", 4).unwrap().is_empty());
        assert!(decorate("text", 4).unwrap().is_empty());
    }

    #[test]
    fn spaces_tabs_and_mixed_indentation_use_visual_columns() {
        let slots = decorate("  \t x", 4).unwrap();
        let projected: Vec<_> = slots
            .iter()
            .map(|slot| (slot.visual_start, slot.visual_length, slot.depth))
            .collect();
        assert_eq!(projected, vec![(0, 4, 0), (4, 1, 1)]);
        assert!(slots.iter().all(|slot| slot.line == 11));
    }

    #[test]
    fn invalid_tab_size_and_oversized_input_are_terminal_errors() {
        assert!(matches!(
            decorate("  x", 0),
            Err(ExtensionError::InvalidRequest(_))
        ));
        let request = DecorationRequest {
            revision: 1,
            tab_size: 4,
            lines: (0..2_001)
                .map(|line| VisibleLine {
                    line,
                    text: String::new(),
                })
                .collect(),
        };
        assert!(matches!(
            IndentRainbow::decorate(request),
            Err(ExtensionError::LimitExceeded(_))
        ));
    }
}

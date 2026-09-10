use sakura_senp_host::effect_protocol::{decode, encode, MAX_FRAME_BYTES};
use serde::Deserialize;
use std::io::{BufRead, Write};

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct Fixture {
    name: String,
    valid: bool,
    input: String,
    fill: Option<usize>,
}

#[test]
fn shared_fixtures_agree_on_grammar_bounds_and_typed_round_trips() {
    let mut output =
        std::env::var_os("SENP_PROTOCOL_OUTPUT").map(|path| std::fs::File::create(path).unwrap());
    let mut count = 0;
    let mut accepted = 0;
    for line in include_str!("../../fixtures/effect-protocol.jsonl").lines() {
        let fixture: Fixture = serde_json::from_str(line).unwrap();
        let wire = if let Some(fill) = fixture.fill {
            assert!(fill <= MAX_FRAME_BYTES && fixture.input.contains("~fill~"));
            fixture.input.replacen("~fill~", &"x".repeat(fill), 1)
        } else {
            fixture.input
        };
        let decoded = decode(wire.as_bytes());
        assert_eq!(
            fixture.valid,
            decoded.is_ok(),
            "{}: {:?}",
            fixture.name,
            decoded
        );
        if let Ok(envelope) = decoded {
            verify_wit_round_trip(&envelope);
            let encoded = encode(&envelope).unwrap();
            assert_eq!(envelope, decode(&encoded).unwrap(), "{}", fixture.name);
            if let Some(output) = &mut output {
                output.write_all(&encoded).unwrap();
                output.write_all(b"\n").unwrap();
            }
            accepted += 1;
        }
        count += 1;
    }
    assert_eq!(count, 72);
    assert_eq!(accepted, 26);
}

fn verify_wit_round_trip(envelope: &sakura_senp_host::effect_protocol::Envelope) {
    use sakura_senp_host::bindings_v2::exports::sakura::senp::event_effects as wit;
    use sakura_senp_host::effect_protocol as wire;
    match &envelope.body {
        wire::Message::Activate(value) => {
            assert_eq!(*value, wire::Activate::from(wit::Activation::from(value)));
        }
        wire::Message::Event(value) => {
            assert_eq!(
                value.context,
                wire::OperationContext::from(wit::OperationContext::from(&value.context))
            );
            assert_eq!(
                value.event,
                wire::Event::from(wit::Event::from(&value.event))
            );
        }
        wire::Message::Effects(value) => {
            assert_eq!(
                value.context,
                wire::OperationContext::from(wit::OperationContext::from(&value.context))
            );
            for effect in &value.effects {
                assert_eq!(*effect, wire::Effect::from(wit::Effect::from(effect)));
            }
        }
        wire::Message::Deactivate(value) => {
            assert_eq!(
                value.reason,
                wire::StopReason::from(wit::StopReason::from(&value.reason))
            );
        }
        _ => {} // Transport hello/ack/rejection/stopped are not guest exports.
    }
}

#[test]
fn accepts_peer_serialized_fixtures_when_requested() {
    let Some(path) = std::env::var_os("SENP_PROTOCOL_PEER_FILE") else {
        return;
    };
    let input = std::io::BufReader::new(std::fs::File::open(path).unwrap());
    let mut count = 0;
    for line in input.lines() {
        assert!(decode(line.unwrap().as_bytes()).is_ok());
        count += 1;
    }
    assert_eq!(count, 26);
}

#[test]
fn rejects_frame_limit_and_invalid_utf8() {
    use sakura_senp_host::effect_protocol::*;
    assert!(decode(&vec![b' '; MAX_FRAME_BYTES + 1]).is_err());
    assert!(decode(&[0xff]).is_err());
    let envelope = Envelope {
        protocol: 2,
        sequence: 1,
        session_generation: 1,
        body: Message::Effects(EffectsMessage {
            context: OperationContext {
                operation_id: "op".into(),
                owner_generation: 1,
                workspace_revision: 0,
                account_generation: 0,
                request_generation: 1,
            },
            effects: vec![Effect::PublishDocument(PublishDocument {
                resource_id: "document".into(),
                title: "title".into(),
                revision: 1,
                sections: vec![
                    DocumentSection::Markdown(MarkdownSection {
                        text: "x".repeat(262144)
                    });
                    4
                ],
            })],
        }),
    };
    assert_eq!(encode(&envelope).unwrap_err(), "frameLimit");
}

#[test]
fn enforces_aggregate_node_budget_before_publishing_encoded_document() {
    use sakura_senp_host::effect_protocol::*;
    let table = DocumentSection::Table(TableSection {
        columns: vec!["column".into(); 16],
        rows: vec![
            TableRow {
                cells: vec![String::new(); 16]
            };
            256
        ],
    });
    let envelope = Envelope {
        protocol: 2,
        sequence: 1,
        session_generation: 1,
        body: Message::Effects(EffectsMessage {
            context: OperationContext {
                operation_id: "op".into(),
                owner_generation: 1,
                workspace_revision: 0,
                account_generation: 0,
                request_generation: 1,
            },
            effects: vec![Effect::PublishDocument(PublishDocument {
                resource_id: "document".into(),
                title: "title".into(),
                revision: 1,
                sections: vec![table; 32],
            })],
        }),
    };
    assert_eq!(encode(&envelope).unwrap_err(), "nodeLimit");
}

use sakura_senp_host::effect_protocol as wire;
use sakura_senp_host::effect_session::{Guest, GuestFailed, Session, MAX_UNACKNOWLEDGED};
use std::cell::RefCell;
use std::rc::Rc;

#[derive(Default)]
struct Calls {
    activate: usize,
    events: usize,
    deactivate: usize,
    fail: bool,
    invalid_output: bool,
}

struct FakeGuest(Rc<RefCell<Calls>>);
impl Guest for FakeGuest {
    fn activate(&mut self, _: &wire::Activate) -> Result<Vec<wire::Effect>, GuestFailed> {
        self.0.borrow_mut().activate += 1;
        Ok(vec![])
    }
    fn on_event(&mut self, _: &wire::EventMessage) -> Result<Vec<wire::Effect>, GuestFailed> {
        let mut calls = self.0.borrow_mut();
        calls.events += 1;
        if calls.fail {
            return Err(GuestFailed);
        }
        if calls.invalid_output {
            return Ok(vec![wire::Effect::InvalidateTree(wire::InvalidateTree {
                view_id: "".into(),
            })]);
        }
        Ok(vec![wire::Effect::StartToolRead(wire::StartToolRead {
            read_id: "read:1".into(),
            tool_id: "github".into(),
            operation: "issues".into(),
            arguments: vec![],
        })])
    }
    fn deactivate(&mut self, _: &wire::StopReason) -> Result<(), GuestFailed> {
        let mut calls = self.0.borrow_mut();
        calls.deactivate += 1;
        if calls.fail {
            Err(GuestFailed)
        } else {
            Ok(())
        }
    }
}

fn context(sequence: u64) -> wire::OperationContext {
    wire::OperationContext {
        operation_id: format!("s9:o{sequence}"),
        owner_generation: 4,
        workspace_revision: 2,
        account_generation: 0,
        request_generation: 1,
    }
}
fn envelope(sequence: u64, body: wire::Message) -> wire::Envelope {
    wire::Envelope {
        protocol: 2,
        sequence,
        session_generation: 9,
        body,
    }
}
fn event(sequence: u64) -> wire::Envelope {
    envelope(
        sequence,
        wire::Message::Event(wire::EventMessage {
            context: context(sequence),
            event: wire::Event::TreeRequest(wire::TreeRequest {
                view_id: "issues".into(),
                parent_id: "".into(),
                cursor: "".into(),
            }),
        }),
    )
}
fn send(session: &mut Session<FakeGuest>, request: &wire::Envelope) -> wire::Envelope {
    wire::decode(
        &session
            .receive(&wire::encode(request).unwrap())
            .unwrap()
            .unwrap(),
    )
    .unwrap()
}
fn activate() -> (Session<FakeGuest>, Rc<RefCell<Calls>>) {
    let calls = Rc::new(RefCell::new(Calls::default()));
    let mut session = Session::new(FakeGuest(calls.clone()));
    let hello = send(
        &mut session,
        &envelope(
            1,
            wire::Message::Hello(wire::Hello {
                abi: wire::ABI.into(),
            }),
        ),
    );
    assert_eq!(hello.sequence, 1);
    let activated = send(
        &mut session,
        &envelope(
            2,
            wire::Message::Activate(wire::Activate {
                context: context(2),
                extension_id: "test.extension".into(),
            }),
        ),
    );
    assert!(matches!(activated.body, wire::Message::Effects(_)));
    (session, calls)
}

#[test]
fn identical_retry_replays_exact_bytes_and_changed_payload_conflicts() {
    let (mut session, calls) = activate();
    let request = wire::encode(&event(3)).unwrap();
    let first = session.receive(&request).unwrap();
    assert_eq!(first, session.receive(&request).unwrap());
    assert_eq!(calls.borrow().events, 1);
    let mut changed = event(3);
    if let wire::Message::Event(value) = &mut changed.body {
        value.context.request_generation = 2;
    }
    assert!(matches!(
        send(&mut session, &changed).body,
        wire::Message::Rejected(wire::Rejected {
            code: wire::RejectionCode::Conflict,
            ..
        })
    ));
    assert_eq!(first, session.receive(&request).unwrap());
    assert_eq!(calls.borrow().events, 1);
}

#[test]
fn ack_releases_only_its_response_and_old_ids_never_redispatch() {
    let (mut session, calls) = activate();
    let reply = send(&mut session, &event(3));
    let ack = envelope(
        4,
        wire::Message::Ack(wire::Ack {
            ack_sequence: reply.sequence,
        }),
    );
    assert!(session
        .receive(&wire::encode(&ack).unwrap())
        .unwrap()
        .is_none());
    assert_eq!(session.unacknowledged(), 2);
    assert!(matches!(
        send(&mut session, &event(3)).body,
        wire::Message::Rejected(wire::Rejected {
            code: wire::RejectionCode::Stale,
            ..
        })
    ));
    let altered_old_ack = envelope(4, wire::Message::Ack(wire::Ack { ack_sequence: 1 }));
    assert!(session
        .receive(&wire::encode(&altered_old_ack).unwrap())
        .unwrap()
        .is_none());
    assert_eq!(session.unacknowledged(), 2);
    let mut reused = event(5);
    if let wire::Message::Event(value) = &mut reused.body {
        value.context.operation_id = context(3).operation_id;
    }
    assert!(matches!(
        send(&mut session, &reused).body,
        wire::Message::Rejected(wire::Rejected {
            code: wire::RejectionCode::Conflict,
            ..
        })
    ));
    assert_eq!(calls.borrow().events, 1);
}

#[test]
fn full_cache_reserves_completion_capacity_before_invocation_and_recovers_after_ack() {
    let (mut session, calls) = activate();
    for sequence in 3..=MAX_UNACKNOWLEDGED as u64 {
        send(&mut session, &event(sequence));
    }
    assert_eq!(session.unacknowledged(), MAX_UNACKNOWLEDGED);
    assert!(matches!(
        send(&mut session, &event(17)).body,
        wire::Message::Rejected(wire::Rejected {
            code: wire::RejectionCode::Busy,
            ..
        })
    ));
    assert_eq!(calls.borrow().events, MAX_UNACKNOWLEDGED - 2);
    let ack = envelope(18, wire::Message::Ack(wire::Ack { ack_sequence: 1 }));
    session.receive(&wire::encode(&ack).unwrap()).unwrap();
    assert!(matches!(
        send(&mut session, &event(19)).body,
        wire::Message::Effects(_)
    ));
    assert_eq!(session.unacknowledged(), MAX_UNACKNOWLEDGED);
}

#[test]
fn owner_and_operation_identity_fail_closed_without_invoking_guest() {
    let (mut session, calls) = activate();
    let mut stale = event(3);
    if let wire::Message::Event(value) = &mut stale.body {
        value.context.owner_generation = 3;
    }
    assert!(matches!(
        send(&mut session, &stale).body,
        wire::Message::Rejected(wire::Rejected {
            code: wire::RejectionCode::Stale,
            ..
        })
    ));
    assert_eq!(calls.borrow().events, 0);
    assert!(matches!(
        send(&mut session, &event(4)).body,
        wire::Message::Effects(_)
    ));
}

#[test]
fn bad_handshake_generation_gap_unknown_direction_and_invalid_json_are_terminal() {
    for kind in 0..5 {
        let (mut session, calls) = activate();
        let mut request = event(3);
        match kind {
            0 => request.session_generation = 10,
            1 => request.sequence = 4,
            2 => request.body = wire::Message::Ack(wire::Ack { ack_sequence: 999 }),
            3 => request.protocol = 1,
            _ => {}
        }
        let bytes = if kind == 3 || kind == 4 {
            b"{}".to_vec()
        } else {
            wire::encode(&request).unwrap()
        };
        assert!(session.receive(&bytes).is_err());
        assert!(session.is_stopped());
        assert_eq!(session.unacknowledged(), 0);
        assert!(session.receive(&wire::encode(&event(3)).unwrap()).is_err());
        assert_eq!(calls.borrow().events, 0);
    }
    let mut session = Session::new(FakeGuest(Rc::new(RefCell::new(Calls::default()))));
    assert!(session.receive(&wire::encode(&event(1)).unwrap()).is_err());
}

#[test]
fn shutdown_trap_and_invalid_guest_output_clear_receipts_and_stop_once() {
    for kind in 0..4 {
        let (mut session, calls) = activate();
        let expected = match kind {
            0 => wire::StopReason::Disabled,
            1 | 3 => {
                calls.borrow_mut().fail = true;
                wire::StopReason::HostUnavailable
            }
            _ => {
                calls.borrow_mut().invalid_output = true;
                wire::StopReason::ProtocolError
            }
        };
        let request = if kind == 0 || kind == 3 {
            envelope(
                3,
                wire::Message::Deactivate(wire::Deactivate {
                    reason: wire::StopReason::Disabled,
                }),
            )
        } else {
            event(3)
        };
        let reply = send(&mut session, &request);
        assert_eq!(
            reply.body,
            wire::Message::Stopped(wire::Stopped { reason: expected })
        );
        assert!(session.is_stopped());
        assert_eq!(session.unacknowledged(), 0);
        assert!(session.receive(&wire::encode(&request).unwrap()).is_err());
        assert_eq!(
            calls.borrow().deactivate,
            usize::from(kind == 0 || kind == 3)
        );
    }
}

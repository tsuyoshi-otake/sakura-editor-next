//! Single-owner v2 dispatcher. A completed invocation is retained until ack;
//! it does not mean that tool reads or user commands have completed.
use crate::effect_protocol as wire;
use sha2::{Digest, Sha256};
use std::collections::BTreeMap;

pub const MAX_UNACKNOWLEDGED: usize = 16;

/// No ambient host capabilities are supplied to a guest by this boundary.
pub trait Guest {
    fn activate(&mut self, value: &wire::Activate) -> Result<Vec<wire::Effect>, GuestFailed>;
    fn on_event(&mut self, value: &wire::EventMessage) -> Result<Vec<wire::Effect>, GuestFailed>;
    fn deactivate(&mut self, reason: &wire::StopReason) -> Result<(), GuestFailed>;
}

#[derive(Debug)]
pub struct GuestFailed;

#[derive(Debug)]
pub struct ProtocolFailed;

enum Phase {
    Hello,
    Activation,
    Active { owner: u64 },
    Stopped,
}

struct Receipt {
    fingerprint: [u8; 32],
    response_sequence: u64,
    response: Vec<u8>,
}

pub struct Session<G> {
    guest: G,
    phase: Phase,
    generation: u64,
    input_sequence: u64,
    output_sequence: u64,
    operation_ticket: u64,
    receipts: BTreeMap<u64, Receipt>,
}

impl<G: Guest> Session<G> {
    pub fn new(guest: G) -> Self {
        Self {
            guest,
            phase: Phase::Hello,
            generation: 0,
            input_sequence: 0,
            output_sequence: 0,
            operation_ticket: 0,
            receipts: BTreeMap::new(),
        }
    }

    pub fn is_stopped(&self) -> bool {
        matches!(self.phase, Phase::Stopped)
    }

    pub fn unacknowledged(&self) -> usize {
        self.receipts.len()
    }

    /// The process/pipe owner terminates on Err; a protocol fault is never
    /// converted into an invitation to continue reading arbitrary requests.
    pub fn receive(&mut self, bytes: &[u8]) -> Result<Option<Vec<u8>>, ProtocolFailed> {
        let result = self.receive_inner(bytes);
        if result.is_err() {
            self.phase = Phase::Stopped;
            self.receipts.clear();
        }
        result
    }

    fn receive_inner(&mut self, bytes: &[u8]) -> Result<Option<Vec<u8>>, ProtocolFailed> {
        if self.is_stopped() {
            return Err(ProtocolFailed);
        }
        let request = wire::decode(bytes).map_err(|_| ProtocolFailed)?;
        if self.generation == 0 {
            if request.sequence != 1 || !matches!(request.body, wire::Message::Hello(_)) {
                return Err(ProtocolFailed);
            }
            self.generation = request.session_generation;
        }
        if request.session_generation != self.generation {
            return Err(ProtocolFailed);
        }
        let canonical = wire::encode(&request).map_err(|_| ProtocolFailed)?;
        let fingerprint: [u8; 32] = Sha256::digest(&canonical).into();
        if request.sequence <= self.input_sequence {
            if let Some(receipt) = self.receipts.get(&request.sequence) {
                if receipt.fingerprint == fingerprint {
                    return Ok(Some(receipt.response.clone()));
                }
                return self.reject(request.sequence, wire::RejectionCode::Conflict);
            }
            // Old ack frames have no side effect, even if their payload changed.
            if matches!(request.body, wire::Message::Ack(_)) {
                return Ok(None);
            }
            return self.reject(request.sequence, wire::RejectionCode::Stale);
        }
        if request.sequence != self.input_sequence + 1 {
            return Err(ProtocolFailed);
        }
        self.input_sequence = request.sequence;
        if let wire::Message::Ack(ack) = &request.body {
            if ack.ack_sequence > self.output_sequence {
                return Err(ProtocolFailed);
            }
            self.receipts
                .retain(|_, entry| entry.response_sequence != ack.ack_sequence);
            return Ok(None);
        }
        if let wire::Message::Deactivate(value) = &request.body {
            if matches!(self.phase, Phase::Hello) {
                return Err(ProtocolFailed);
            }
            let reason = if matches!(self.phase, Phase::Active { .. })
                && self.guest.deactivate(&value.reason).is_err()
            {
                wire::StopReason::HostUnavailable
            } else {
                value.reason.clone()
            };
            return self.stop(reason);
        }
        // Reserve a full frame slot BEFORE calling the guest. Output cache
        // cannot exceed 16 MiB even if every result consumes its entire budget.
        if self.receipts.len() >= MAX_UNACKNOWLEDGED {
            return self.reject(request.sequence, wire::RejectionCode::Busy);
        }
        let response = match (&self.phase, &request.body) {
            (Phase::Hello, wire::Message::Hello(_)) => {
                self.phase = Phase::Activation;
                wire::Message::Hello(wire::Hello {
                    abi: wire::ABI.into(),
                })
            }
            (Phase::Activation, wire::Message::Activate(value)) => {
                if !self.adopt_operation(&value.context) {
                    return self.reject(request.sequence, wire::RejectionCode::Conflict);
                }
                let effects = match self.guest.activate(value) {
                    Ok(effects) => effects,
                    Err(_) => return self.stop(wire::StopReason::HostUnavailable),
                };
                self.phase = Phase::Active {
                    owner: value.context.owner_generation,
                };
                wire::Message::Effects(wire::EffectsMessage {
                    context: value.context.clone(),
                    effects,
                })
            }
            (Phase::Active { owner }, wire::Message::Event(value)) => {
                if value.context.owner_generation != *owner {
                    return self.reject(request.sequence, wire::RejectionCode::Stale);
                }
                if !self.adopt_operation(&value.context) {
                    return self.reject(request.sequence, wire::RejectionCode::Conflict);
                }
                let effects = match self.guest.on_event(value) {
                    Ok(effects) => effects,
                    Err(_) => return self.stop(wire::StopReason::HostUnavailable),
                };
                wire::Message::Effects(wire::EffectsMessage {
                    context: value.context.clone(),
                    effects,
                })
            }
            _ => return self.reject(request.sequence, wire::RejectionCode::InvalidRequest),
        };
        let response = match self.respond(response) {
            Ok(response) => response,
            // Guest-created values must satisfy the SAME aggregate codec
            // contract. Do not retain or publish partial invalid output.
            Err(_) => return self.stop(wire::StopReason::ProtocolError),
        };
        self.receipts.insert(
            request.sequence,
            Receipt {
                fingerprint,
                response_sequence: self.output_sequence,
                response: response.clone(),
            },
        );
        Ok(Some(response))
    }

    fn adopt_operation(&mut self, context: &wire::OperationContext) -> bool {
        let prefix = format!("s{}:o", self.generation);
        let Some(text) = context.operation_id.strip_prefix(&prefix) else {
            return false;
        };
        let Ok(ticket) = text.parse::<u64>() else {
            return false;
        };
        if ticket <= self.operation_ticket
            || ticket > wire::MAX_COUNTER
            || ticket.to_string() != text
        {
            return false;
        }
        self.operation_ticket = ticket;
        true
    }

    fn reject(
        &mut self,
        sequence: u64,
        code: wire::RejectionCode,
    ) -> Result<Option<Vec<u8>>, ProtocolFailed> {
        self.respond(wire::Message::Rejected(wire::Rejected {
            request_sequence: sequence,
            code,
        }))
        .map(Some)
    }

    fn stop(&mut self, reason: wire::StopReason) -> Result<Option<Vec<u8>>, ProtocolFailed> {
        self.phase = Phase::Stopped;
        self.receipts.clear();
        self.respond(wire::Message::Stopped(wire::Stopped { reason }))
            .map(Some)
    }

    fn respond(&mut self, body: wire::Message) -> Result<Vec<u8>, ProtocolFailed> {
        let sequence = self
            .output_sequence
            .checked_add(1)
            .filter(|sequence| *sequence <= wire::MAX_COUNTER)
            .ok_or(ProtocolFailed)?;
        let bytes = wire::encode(&wire::Envelope {
            protocol: 2,
            sequence,
            session_generation: self.generation,
            body,
        })
        .map_err(|_| ProtocolFailed)?;
        self.output_sequence = sequence;
        Ok(bytes)
    }
}

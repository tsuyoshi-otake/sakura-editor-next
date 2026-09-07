//! Actual v2 Wasm component used by the native runtime acceptance test.
wit_bindgen::generate!({ path: "../../wit/v2/senp-extension.wit", world: "extension" });
use exports::sakura::senp::event_effects::*;

struct Fixture;
impl Guest for Fixture {
    fn activate(_: Activation) -> Vec<Effect> {
        vec![Effect::InvalidateTree(InvalidateTree {
            view_id: "test.issues".into(),
        })]
    }
    fn on_event(_: OperationContext, event: Event) -> Vec<Effect> {
        if let Event::CommandInvoked(command) = &event {
            if command.command_id == "test.spin" {
                loop {
                    std::hint::spin_loop();
                }
            }
        }
        vec![Effect::OpenDocument(OpenDocument {
            resource_id: "test:issue/1".into(),
        })]
    }
    fn deactivate(_: StopReason) {}
}
export!(Fixture);

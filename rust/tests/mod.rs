// SPDX-License-Identifier: MIT
//! The tests, for this computer. The Makefile builds the crate with them in (`rustc --test`) once for each set of
//! `--cfg` names that an image or a simulated board has, and each set has the tests that need it: `make test`.
//! They run one at a time (`--test-threads=1`), as the simulated chip is one.

// Nothing of the hardware: the plain build (`host`).
#[cfg(host)]
mod contract;
#[cfg(host)]
mod formats;
#[cfg(host)]
mod loader;

// The MIDI update's protocol, with the test as its host (`mu_test_app_ram`, and `smk` for the SMK-37).
#[cfg(all(mu_test_app_ram, not(boot_sim)))]
mod midi_update;

// The drivers, on registers that the test stands in for (`platform_sim`), each with what it stands in for besides:
// `platform_test`; `import_early panel_test`; `import_early sdboot_test`, and `smk`.
#[cfg(panel_test)]
mod panel_board;
#[cfg(platform_test)]
mod platform;
#[cfg(sdboot_test)]
mod sdboot;

// The bootloader on the simulated chip (`boot_sim platform_sim`): the flash loader's life on it (`lifecycle_test`),
// the boots of each board (`smk`; `test_requests`), and MIDI mode in them (`midi_mode mu_test_app_ram`).
#[cfg(all(boot_sim, not(lifecycle_test)))]
mod boot;
#[cfg(midi_mode)]
mod boot_midi;
#[cfg(lifecycle_test)]
mod loader_lifecycle;
#[cfg(boot_sim)]
mod nor;
#[cfg(boot_sim)]
mod sim;

#[cfg(any(boot_sim, mu_test_app_ram))]
mod escape;

// What the crate takes from the tests in those builds, in place of hardware: `crate::tests::...` in rust/src/.
// The chip's registers.
#[cfg(panel_test)]
pub use panel_board::platform_register;
#[cfg(platform_test)]
pub use platform::platform_register;
#[cfg(sdboot_test)]
pub use sdboot::platform_register;
#[cfg(boot_sim)]
pub use sim::{boot_sim_settle, platform_dma_address, platform_register};
// The reset, the card, and the application's and a staged updater's starts.
#[cfg(all(boot_sim, not(lifecycle_test)))]
pub use boot::{boot_sim_call_app, boot_sim_call_updater, boot_sim_reset, sd_probe, sd_stage2};
#[cfg(lifecycle_test)]
pub use loader_lifecycle::{
    boot_sim_call_app, boot_sim_call_updater, boot_sim_reset, sd_probe, sd_stage2,
};
// USB, the MIDI update's protocol, and where a RAM app is.
#[cfg(midi_mode)]
pub use boot_midi::{app_ram, mu_run, usb_midi_poll, usb_midi_send, usb_midi_start, usb_midi_stop};
#[cfg(all(mu_test_app_ram, not(boot_sim)))]
pub use midi_update::app_ram;

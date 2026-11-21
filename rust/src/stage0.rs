// SPDX-License-Identifier: MIT
//! Stage 0: still src/stage0.c, declared here for the Rust that calls it.
unsafe extern "C" {
    /// Restarts the chip in its ROM's USB download mode (§4.2).
    pub safe fn rom_download() -> !;
}

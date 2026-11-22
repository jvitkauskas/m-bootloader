// SPDX-License-Identifier: MIT
//! Keys and LEDs: docs/SPEC.md §8. Stage 0 owns these and stage 1 calls them through the linker, so they keep their C
//! names and calling convention.
pub const KEY_PREV: u32 = 1;
pub const KEY_NEXT: u32 = 2;
pub const KEY_SELECT: u32 = 4;
pub const KEY_RECOVERY: u32 = 8;

// Stage 1 takes these from stage 0. So does a test that stands in for them (`import_board`).
#[cfg(any(stage1, import_board))]
unsafe extern "C" {
    pub safe fn board_init(smk: bool);
    pub safe fn board_keys() -> u32;
    pub safe fn board_hold(key: u32, ms: u32) -> bool;
    pub safe fn board_wait(ms: u32, leds: bool);
    pub safe fn board_restore();
}

#[cfg(not(any(stage1, import_board)))]
pub use own::*;

#[cfg(not(any(stage1, import_board)))]
mod own {
    use crate::Global;
    use crate::hw::{Reg, bit, gpio, sync_cpu};
    use crate::platform::{gpio_input, gpio_level, gpio_output, gpio_read, hal_delay_us};

    const PHY: Reg = Reg::at::<0x16a0c>();
    /// The registers of a port that are saved and put back.
    const OFFSETS: [u32; 8] = [0, 8, 12, 16, 20, 24, 28, 32];
    const PORTS: [u32; 2] = [0, 7];

    struct Board {
        saved: [[u32; 8]; 2],
        phy: u32,
        smk: bool,
        saved_valid: bool,
        phy_active: bool,
        led_phase: u32,
    }

    static BOARD: Global<Board> = Global::new(Board {
        saved: [[0; 8]; 2],
        phy: 0,
        smk: false,
        saved_valid: false,
        phy_active: false,
        led_phase: 0,
    });

    /// Safety, for every use: these entries call nothing that comes back into this module.
    fn board() -> &'static mut Board {
        unsafe { BOARD.get() }
    }

    impl Board {
        fn shift(&self, word: u32) {
            for b in 0..if self.smk { 4 } else { 2 } {
                let byte = (word >> if self.smk { b * 8 } else { (1 - b) * 8 }) as u8;
                for n in (0..8).rev() {
                    gpio_level(0, 4, byte & 1 << n != 0);
                    gpio_level(0, 3, true);
                    sync_cpu();
                    gpio_level(0, 3, false);
                }
            }
            gpio_level(0, 1, true);
            sync_cpu();
            gpio_level(0, 1, false);
        }

        fn dark(&self) {
            if self.smk {
                if self.phy_active {
                    PHY.set(bit(17) | bit(18));
                }
            } else {
                gpio_level(0, 9, false);
                gpio_level(0, 10, false);
                gpio_level(7, 6, false);
                gpio_level(7, 9, false);
            }
        }

        fn keys(&self) -> u32 {
            const SMK_WORDS: [u32; 4] = [0xfffe_ffff, 0xefff_ffff, 0xfeff_ffff, 0xf7ff_ffff];
            const FM_WORDS: [u32; 4] = [0xf7fe, 0xeffd, 0xfbff, 0xff7f];
            let mut keys = 0;
            self.dark();
            for i in 0..4 {
                self.shift(if self.smk { SMK_WORDS[i] } else { FM_WORDS[i] });
                hal_delay_us(10);
                let (port, pin) = if self.smk {
                    (7, 8)
                } else {
                    (0, if i < 2 { 8 } else { 6 })
                };
                if !gpio_read(port, pin) {
                    keys |= 1 << i;
                }
            }
            self.shift(u32::MAX);
            keys
        }

        fn wait(&mut self, ms: u32, leds: bool) {
            for _ in 0..ms {
                self.dark();
                if leds {
                    let phase = self.led_phase;
                    self.led_phase = phase.wrapping_add(1);
                    if self.smk {
                        if !self.phy_active {
                            PHY.set(0x1000f | bit(17) | bit(18));
                            self.phy_active = true;
                        }
                        let group = phase & 1 != 0;
                        self.shift(!if group { bit(0) | bit(12) } else { bit(0) });
                        PHY.clear(bit(if group { 18 } else { 17 }));
                    } else {
                        let p = phase % 3;
                        self.shift(match p {
                            0 => 0xf7fe,
                            1 => 0xeffd,
                            _ => 0xfbff,
                        });
                        let (port, pin) = if p == 2 { (0, 10) } else { (7, 9) };
                        gpio_level(port, pin, true);
                    }
                }
                hal_delay_us(1000);
            }
            self.dark();
            self.shift(u32::MAX);
        }
    }

    #[unsafe(no_mangle)]
    pub extern "C" fn board_init(smk: bool) {
        let b = board();
        b.smk = smk;
        b.led_phase = 0;
        b.phy_active = false;
        for (saved, port) in b.saved.iter_mut().zip(PORTS) {
            for (slot, offset) in saved.iter_mut().zip(OFFSETS) {
                *slot = gpio(port, offset).read();
            }
        }
        b.phy = PHY.read();
        b.saved_valid = true;
        gpio_output(0, 1, false);
        gpio_output(0, 3, false);
        gpio_output(0, 4, false);
        if smk {
            gpio_output(0, 0, false);
            gpio_input(7, 8, true, false);
        } else {
            // PA2 also enables the FM-1 key matrix; what was saved preserves its state.
            gpio_output(0, 2, false);
            gpio_input(0, 6, true, false);
            gpio_input(0, 8, true, false);
            gpio_output(0, 9, false);
            gpio_output(0, 10, false);
            gpio_output(7, 6, false);
            gpio_output(7, 9, false);
        }
        b.dark();
        b.shift(u32::MAX);
    }

    #[unsafe(no_mangle)]
    pub extern "C" fn board_keys() -> u32 {
        board().keys()
    }

    #[unsafe(no_mangle)]
    pub extern "C" fn board_wait(ms: u32, leds: bool) {
        board().wait(ms, leds)
    }

    #[unsafe(no_mangle)]
    pub extern "C" fn board_hold(key: u32, ms: u32) -> bool {
        let b = board();
        if b.keys() & key == 0 {
            return false;
        }
        let (mut missing, mut elapsed) = (0, 0);
        while elapsed < ms {
            b.wait(20, false);
            if b.keys() & key != 0 {
                missing = 0;
            } else {
                missing += 1;
                if missing >= 2 {
                    return false;
                }
            }
            elapsed += 20;
        }
        true
    }

    #[unsafe(no_mangle)]
    pub extern "C" fn board_restore() {
        let b = board();
        if !b.saved_valid {
            return;
        }
        b.dark();
        b.shift(u32::MAX);
        for (saved, port) in b.saved.iter().zip(PORTS) {
            for (&value, offset) in saved.iter().zip(OFFSETS) {
                gpio(port, offset).write(value);
            }
        }
        if b.phy_active {
            PHY.write(b.phy);
        }
        b.phy_active = false;
        b.saved_valid = false;
    }
}

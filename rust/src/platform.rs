// SPDX-License-Identifier: MIT
//! Clock, power, pins, delays and the SPI flash. Stage 0 owns what comes first here and stage 1 calls it through the
//! linker (docs/SPEC.md §4.4), so these keep their C names and calling convention.
#[cfg(not(stage0))]
use crate::Global;
use crate::hw::{Reg, bit, gpio, sync_cpu};

/// What stage 0 and stage 1 share: in stage 0's data.
mod state {
    use crate::Global;

    #[cfg(not(stage1))]
    mod own {
        use crate::Global;
        #[cfg_attr(stage0, unsafe(export_name = "failed"))]
        pub static FAILED: Global<bool> = Global::new(false);
        #[cfg_attr(stage0, unsafe(export_name = "dma_active"))]
        pub static DMA_ACTIVE: Global<bool> = Global::new(false);
        #[cfg_attr(stage0, unsafe(export_name = "timer_mhz"))]
        pub static TIMER_MHZ: Global<u32> = Global::new(24);
    }

    #[cfg(stage1)]
    mod own {
        use crate::Global;
        unsafe extern "C" {
            #[link_name = "failed"]
            pub safe static FAILED: Global<bool>;
            #[link_name = "dma_active"]
            pub safe static DMA_ACTIVE: Global<bool>;
            #[link_name = "timer_mhz"]
            pub safe static TIMER_MHZ: Global<u32>;
        }
    }

    pub fn failed() -> &'static Global<bool> {
        &own::FAILED
    }

    pub fn dma_active() -> &'static Global<bool> {
        &own::DMA_ACTIVE
    }

    pub fn timer_mhz() -> &'static Global<u32> {
        &own::TIMER_MHZ
    }
}

use state::{dma_active, failed, timer_mhz};

// Stage 1 takes these from stage 0. So does a test that stands in for them (`import_early`).
#[cfg(any(stage1, import_early))]
unsafe extern "C" {
    pub safe fn wait_mask(reg: Reg, mask: u32, value: bool) -> bool;
    pub safe fn settle(n: u32);
    pub safe fn hal_fault() -> bool;
    pub safe fn hal_fault_clear();
    pub safe fn gpio_level(port: u32, pin: u32, high: bool);
    pub safe fn gpio_output(port: u32, pin: u32, high: bool);
    pub safe fn gpio_input(port: u32, pin: u32, up: bool, down: bool);
    pub safe fn gpio_read(port: u32, pin: u32) -> bool;
    pub safe fn hal_delay_us(usec: u32) -> bool;
    pub safe fn p33_read(address: u32) -> u8;
    pub safe fn p33_write(address: u32, value: u8);
    pub safe fn p33_or(address: u32, value: u8);
    pub safe fn p33_and(address: u32, value: u8);
    pub safe fn hal_early();
    pub safe fn hal_clock_a() -> bool;
    pub safe fn hal_reset() -> !;
}

#[cfg(not(any(stage1, import_early)))]
pub use early::*;
#[cfg(not(stage0))]
pub use flash::*;

fn wait(reg: Reg, mask: u32, value: bool) -> bool {
    wait_mask(reg, mask, value)
}

#[cfg(not(any(stage1, import_early)))]
mod early {
    use super::*;

    const TIMER: Reg = Reg::at::<0x10400>();
    const P33_CON: Reg = Reg::at::<0x13e08>();
    const P33_BUF: Reg = Reg::at::<0x13e0c>();
    const CLK_CON0: Reg = Reg::at::<0x1000c>();
    const CLK_CON1: Reg = Reg::at::<0x10008>();
    const CLK_DIV: Reg = Reg::at::<0x10014>();
    const PLL_CON0: Reg = Reg::at::<0x119a0>();
    const PLL_CON1: Reg = Reg::at::<0x119a4>();
    const PLL_CON2: Reg = Reg::at::<0x119a8>();
    const PLL_CON3: Reg = Reg::at::<0x119ac>();
    #[cfg(not(boot_sim))]
    const POWER: Reg = Reg::at::<0x10000>();

    #[unsafe(no_mangle)]
    pub extern "C" fn wait_mask(reg: Reg, mask: u32, value: bool) -> bool {
        let mut n = 3_000_000;
        while n != 0 {
            if (reg.read() & mask != 0) == value {
                return true;
            }
            n -= 1;
        }
        failed().set(true);
        false
    }

    #[unsafe(no_mangle)]
    pub extern "C" fn hal_fault() -> bool {
        failed().read()
    }

    #[unsafe(no_mangle)]
    pub extern "C" fn hal_fault_clear() {
        failed().set(false);
    }

    #[unsafe(no_mangle)]
    pub extern "C" fn gpio_level(port: u32, pin: u32, high: bool) {
        if high {
            gpio(port, 0).set(bit(pin));
        } else {
            gpio(port, 0).clear(bit(pin));
        }
    }

    #[unsafe(no_mangle)]
    pub extern "C" fn gpio_output(port: u32, pin: u32, high: bool) {
        gpio_level(port, pin, high);
        gpio(port, 8).clear(bit(pin));
    }

    #[unsafe(no_mangle)]
    pub extern "C" fn gpio_input(port: u32, pin: u32, up: bool, down: bool) {
        gpio(port, 8).set(bit(pin));
        gpio(port, 0xc).set(bit(pin));
        if up {
            gpio(port, 0x10).set(bit(pin));
        } else {
            gpio(port, 0x10).clear(bit(pin));
        }
        if down {
            gpio(port, 0x14).set(bit(pin));
        } else {
            gpio(port, 0x14).clear(bit(pin));
        }
    }

    #[unsafe(no_mangle)]
    pub extern "C" fn gpio_read(port: u32, pin: u32) -> bool {
        gpio(port, 4).read() & bit(pin) != 0
    }

    /// A short wait that needs no timer: `n` times round a loop of one `nop`.
    #[unsafe(no_mangle)]
    pub extern "C" fn settle(n: u32) {
        #[cfg(boot_sim)]
        {
            unsafe extern "C" {
                safe fn boot_sim_settle(iterations: u32);
            }
            boot_sim_settle(n);
        }
        for _ in 0..n {
            #[cfg(target_arch = "pi32v2")]
            unsafe {
                core::arch::asm!("nop", options(nostack, preserves_flags))
            }
        }
    }

    #[unsafe(no_mangle)]
    pub extern "C" fn hal_delay_us(mut us: u32) -> bool {
        while us != 0 {
            let part = us.min(100_000);
            us -= part;
            TIMER.write(bit(14));
            TIMER.add(4).write(0);
            TIMER.add(8).write(timer_mhz().read() * part);
            TIMER.write(bit(0));
            let ok = wait(TIMER, bit(15), true);
            TIMER.write(bit(14));
            if !ok {
                return false;
            }
        }
        true
    }

    fn p33_byte(b: u8) -> u8 {
        P33_CON.set(12);
        P33_BUF.write(b as u32);
        P33_CON.set(16);
        sync_cpu();
        if !wait(P33_CON, 2, false) {
            return 0xff;
        }
        P33_BUF.read() as u8
    }

    fn p33_access(a: u32, op: u32, b: u8) -> u8 {
        P33_CON.set(1);
        p33_byte((op | a >> 8) as u8);
        p33_byte(a as u8);
        let b = p33_byte(b);
        P33_CON.clear(1);
        b
    }

    #[unsafe(no_mangle)]
    pub extern "C" fn p33_read(a: u32) -> u8 {
        p33_access(a, 0x80, 0x5e)
    }

    #[unsafe(no_mangle)]
    pub extern "C" fn p33_write(a: u32, b: u8) {
        p33_access(a, 0, b);
    }

    #[unsafe(no_mangle)]
    pub extern "C" fn p33_or(a: u32, b: u8) {
        p33_access(a, 0x20, b);
    }

    #[unsafe(no_mangle)]
    pub extern "C" fn p33_and(a: u32, b: u8) {
        p33_access(a, 0x40, b);
    }

    #[unsafe(no_mangle)]
    pub extern "C" fn hal_early() {
        failed().set(false);
        dma_active().set(false);
        p33_and(0x80, 0xef);
    }

    #[unsafe(no_mangle)]
    pub extern "C" fn hal_clock_a() -> bool {
        // §3.2: ROM's startup clock is slow. Register loops use 10x the tested memory-counter counts; do not lengthen
        // these or use Timer 0 here.
        CLK_CON0.clear(bit(1));
        settle(20);
        CLK_CON0.clear(bit(8));
        settle(20);
        CLK_DIV.modify(|v| v & !255 | 6);
        PLL_CON0.write(0x4540_0200);
        PLL_CON1.write(0x3f50_3026);
        PLL_CON0.set(1);
        settle(100);
        PLL_CON0.set(2);
        let v = PLL_CON2.read() & !3;
        PLL_CON2.write(v);
        const M0: u32 = 31 << 2 | bit(7) | 3 << 8 | bit(10) | 3 << 22 | 3 << 24 | 3 << 26;
        PLL_CON2.write(v & !M0 | 10 << 2 | 2 << 8 | 1 << 22 | 1 << 24 | 2 << 26);
        const M1: u32 = 0xfff | 7 << 12 | 15 << 15 | 3 << 20 | 3 << 22 | 7 << 24;
        PLL_CON3.modify(|v| v & !M1 | 268 | 3 << 12 | 1 << 20 | 1 << 22 | 7 << 24);
        PLL_CON2.set(1);
        settle(100);
        PLL_CON2.set(2);
        settle(900);
        CLK_CON0.set(bit(6) | bit(7));
        CLK_CON1.modify(|v| v & !0x30700 | 0x10200);
        CLK_CON0.set(bit(8));
        sync_cpu();
        timer_mhz().set(60);
        !failed().read()
    }

    #[unsafe(no_mangle)]
    pub extern "C" fn hal_reset() -> ! {
        #[cfg(boot_sim)]
        {
            unsafe extern "C" {
                safe fn boot_sim_reset() -> !;
            }
            boot_sim_reset();
        }
        #[cfg(not(boot_sim))]
        {
            POWER.set(bit(4));
            sync_cpu();
            loop {
                #[cfg(target_arch = "pi32v2")]
                unsafe {
                    core::arch::asm!("idle", options(nostack, preserves_flags))
                }
            }
        }
    }
}

#[cfg(not(stage0))]
mod flash {
    use super::*;
    use core::ffi::c_void;

    const SPI_CON: Reg = Reg::at::<0x11c00>();
    const SPI_BAUD: Reg = SPI_CON.add(4);
    const SPI_BUF: Reg = SPI_CON.add(8);
    const SPI_ADR: Reg = SPI_CON.add(12);
    const SPI_CNT: Reg = SPI_CON.add(16);
    const CLK_CON1: Reg = Reg::at::<0x10008>();
    const CLK_DIV: Reg = Reg::at::<0x10014>();
    const EFUSE_CON: Reg = Reg::at::<0x10200>();
    const DMA_MUX: Reg = Reg::at::<0x13000>();
    const SFC_CON: Reg = Reg::at::<0x40200>();
    const SFC_BAUD: Reg = Reg::at::<0x40204>();
    const SFC_BASE: Reg = Reg::at::<0x4020c>();
    const ENC_CON: Reg = Reg::at::<0x40300>();
    const ENC_KEY: Reg = Reg::at::<0x40304>();
    const IO_MUX0: Reg = Reg::at::<0x5101c>();
    const IO_MUX1: Reg = Reg::at::<0x51020>();
    const CACHE_CON: Reg = Reg::at::<0x1eee008>();
    const CACHE_TAGS: Reg = Reg::at::<0x1f0_0000>();

    static READ_WIDTH: Global<u32> = Global::new(1);
    static CAPACITY: Global<u32> = Global::new(0);

    #[unsafe(no_mangle)]
    pub extern "C" fn hal_clock_b() -> u32 {
        let mut mhz = 0;
        if CLK_DIV.read() & 255 == 0x80 && CLK_CON1.read() & 0x30700 == 0 {
            CLK_CON1.set(bit(8));
            sync_cpu();
            CLK_DIV.modify(|v| v & !255 | 0x40);
            sync_cpu();
            mhz = 96;
        } else if CLK_DIV.read() & 255 == 0x40 && CLK_CON1.read() & 0x30700 == 0x100 {
            mhz = 96;
        }
        timer_mhz().set(48);
        mhz
    }

    /// For code started after the clock switch (a staged updater): LSB MHz.
    #[unsafe(no_mangle)]
    pub extern "C" fn hal_timer_clock(mhz: u32) {
        timer_mhz().set(mhz);
    }

    #[unsafe(no_mangle)]
    pub extern "C" fn hal_efuse(bank: u32, row: u32) -> u32 {
        let count = if EFUSE_CON.read() & 255 != 0 { 4 } else { 3 };
        let mut v = 0;
        p33_write(0xb0, (bank << 6) as u8);
        for j in 0..count {
            p33_write(0xb1, (row << 6 | j << 2 | 2) as u8);
            if !hal_delay_us(50) {
                break;
            }
            v |= (p33_read(0xb2) as u32) << (8 * j);
            p33_write(0xb1, 0);
        }
        v
    }

    fn cs(high: bool) {
        gpio_level(3, 0, high);
        settle(40);
    }

    fn spi_byte(v: u8, rx: bool) -> u8 {
        if failed().read() {
            return 0xff;
        }
        if rx {
            SPI_CON.set(bit(12));
        } else {
            SPI_CON.clear(bit(12));
        }
        SPI_BUF.write(v as u32);
        sync_cpu();
        let ok = wait(SPI_CON, bit(15), true);
        let r = SPI_BUF.read() as u8;
        SPI_CON.set(bit(14));
        if ok { r } else { 0xff }
    }

    fn spi_single() {
        SPI_CON.modify(|v| v & !(3 << 10 | bit(12)) | bit(3));
    }

    fn command(op: u8) {
        spi_single();
        cs(false);
        spi_byte(op, false);
    }

    fn address24(a: u32) {
        spi_byte((a >> 16) as u8, false);
        spi_byte((a >> 8) as u8, false);
        spi_byte(a as u8, false);
    }

    fn status(op: u8) -> u8 {
        command(op);
        let v = spi_byte(255, true);
        cs(true);
        v
    }

    fn ready() -> bool {
        for _ in 0..30000 {
            if failed().read() {
                return false;
            }
            if status(5) & 1 == 0 {
                return !failed().read();
            }
            if !hal_delay_us(100) {
                return false;
            }
        }
        failed().set(true);
        false
    }

    fn wren() {
        command(6);
        cs(true);
    }

    fn write_status(s1: u8, s2: u8, combined: bool) -> bool {
        wren();
        command(1);
        spi_byte(s1, false);
        if combined {
            spi_byte(s2, false);
        }
        cs(true);
        if !ready() {
            return false;
        }
        if !combined {
            wren();
            command(0x31);
            spi_byte(s2, false);
            cs(true);
        }
        ready()
    }

    fn protected() -> bool {
        status(5) & 0x7c != 0 || status(0x35) & 0x40 != 0
    }

    fn unprotect() -> bool {
        let (a, b) = (status(5), status(0x35));
        if a & 0x7c != 0 || b & 0x40 != 0 {
            if !write_status(a & !0x7f, b & !0x40, true) {
                return false;
            }
            if protected() && !write_status(a & !0x7f, b & !0x40, false) {
                return false;
            }
            if protected() {
                failed().set(true);
                return false;
            }
        }
        !failed().read()
    }

    #[repr(align(4))]
    struct Line([u8; 32]);

    #[unsafe(no_mangle)]
    pub extern "C" fn flash_init(width: u32, baud: u32) -> bool {
        SFC_CON.write(0);
        DMA_MUX.write(0);
        IO_MUX1.clear(bit(1));
        IO_MUX0.clear(bit(5));
        gpio_output(3, 0, true);
        gpio_output(3, 4, false);
        gpio_output(3, 5, false);
        gpio(3, 0x1c).set(bit(4));
        gpio_input(3, 1, false, false);
        gpio_output(3, 2, true);
        gpio_output(3, 3, true);
        SPI_CON.write(0x28);
        SPI_BAUD.write(baud);
        SPI_CON.set(bit(14) | bit(0));
        cs(true);
        // Exit any continuous-read state inherited across reset/reinstallation.
        command(0xff);
        spi_byte(0xff, false);
        cs(true);
        READ_WIDTH.set(1);
        dma_active().set(false);
        let e = flash_jedec() & 255;
        CAPACITY.set(if (16..=24).contains(&e) { 1 << e } else { 0 });
        if CAPACITY.read() == 0 {
            failed().set(true);
            return false;
        }
        if width > 1 {
            let (mut single, mut wide) = (Line([0; 32]), Line([0; 32]));
            if !read(0, &mut single.0) {
                return false;
            }
            READ_WIDTH.set(if width == 4 && flash_quad() { 4 } else { 2 });
            while READ_WIDTH.read() > 1 {
                if !read(0, &mut wide.0) {
                    return false;
                }
                if single.0 == wide.0 {
                    break;
                }
                READ_WIDTH.set(READ_WIDTH.read() / 2);
            }
        }
        !failed().read()
    }

    #[unsafe(no_mangle)]
    pub extern "C" fn flash_quad() -> bool {
        let (a, b) = (status(5), status(0x35));
        if b & 2 != 0 {
            return !failed().read();
        }
        if !write_status(a & !3, b | 2, true) {
            return false;
        }
        let mut i = 0;
        while status(0x35) & 2 == 0 && i < 3 {
            if !write_status(a & !3, b | 2, false) {
                return false;
            }
            i += 1;
        }
        if status(0x35) & 2 == 0 {
            return false;
        }
        READ_WIDTH.set(4);
        !failed().read()
    }

    #[unsafe(no_mangle)]
    pub extern "C" fn flash_jedec() -> u32 {
        command(0x9f);
        let mut id = 0;
        for _ in 0..3 {
            id = id << 8 | spi_byte(255, true) as u32;
        }
        cs(true);
        id
    }

    #[unsafe(no_mangle)]
    pub extern "C" fn flash_capacity() -> u32 {
        CAPACITY.read()
    }

    #[unsafe(no_mangle)]
    pub extern "C" fn flash_read_width() -> u32 {
        READ_WIDTH.read()
    }

    #[unsafe(no_mangle)]
    pub extern "C" fn flash_uid(out: &mut [u8; 8]) {
        command(0x4b);
        for _ in 0..4 {
            spi_byte(255, false);
        }
        for b in out {
            *b = spi_byte(255, true);
        }
        cs(true);
    }

    use crate::hw::dma_address;

    fn dma_receive(out: *mut u8, n: u32) {
        DMA_MUX.write(2);
        SPI_CON.set(bit(12) | bit(14));
        SPI_ADR.write(dma_address(out));
        sync_cpu();
        SPI_CNT.write(n);
    }

    fn dma_wait() -> bool {
        let ok = wait(SPI_CON, bit(15), true);
        SPI_CON.set(bit(14));
        DMA_MUX.write(0);
        ok && !failed().read()
    }

    /// # Safety
    /// `out` has room for `n` bytes, and stays valid until `flash_dma_finish`.
    #[unsafe(no_mangle)]
    pub unsafe extern "C" fn flash_dma_start(a: u32, out: *mut c_void, n: u32) -> bool {
        let (capacity, width) = (CAPACITY.read(), READ_WIDTH.read());
        if failed().read()
            || dma_active().read()
            || n == 0
            || n > 65535
            || a > capacity
            || n > capacity - a
        {
            return false;
        }
        command(match width {
            4 => 0x6b,
            2 => 0x3b,
            _ => 3,
        });
        address24(a);
        if width > 1 {
            let mut dummy = core::mem::MaybeUninit::<u32>::uninit();
            SPI_CON.modify(|v| {
                v & !(bit(3) | 3 << 10) | (if width == 4 { 2 } else { 1 }) << 10 | bit(12)
            });
            // §3.4: eight dummy clocks are a separate wide DMA receive.
            dma_receive(dummy.as_mut_ptr().cast(), width);
            if !dma_wait() {
                cs(true);
                spi_single();
                return false;
            }
        }
        dma_receive(out.cast(), n);
        dma_active().set(true);
        true
    }

    #[unsafe(no_mangle)]
    pub extern "C" fn flash_dma_finish() -> bool {
        if !dma_active().read() {
            return true;
        }
        let ok = dma_wait();
        cs(true);
        spi_single();
        dma_active().set(false);
        ok
    }

    /// # Safety
    /// `out` has room for `n` bytes, or `n` is 0. `ctx` is not used: the flash is the chip's.
    #[cfg(any(not(target_arch = "pi32v2"), loader_a, loader_b))]
    #[unsafe(no_mangle)]
    pub unsafe extern "C" fn flash_read(
        _ctx: *mut c_void,
        a: u32,
        out: *mut c_void,
        n: u32,
    ) -> bool {
        // (Nothing is made of `out` when there is nothing to read: C may pass a null pointer then.)
        read(
            a,
            if n == 0 {
                &mut []
            } else {
                unsafe { core::slice::from_raw_parts_mut(out.cast(), n as usize) }
            },
        )
    }

    pub fn read(mut a: u32, out: &mut [u8]) -> bool {
        let capacity = CAPACITY.read();
        if a > capacity || out.len() as u32 > capacity - a {
            return false;
        }
        for part in out.chunks_mut(4096) {
            // Safety: the read is finished before `part` is used again.
            if !unsafe { flash_dma_start(a, part.as_mut_ptr().cast(), part.len() as u32) }
                || !flash_dma_finish()
            {
                return false;
            }
            a += part.len() as u32;
        }
        !failed().read()
    }

    /// The chip's own flash, for the formats' code.
    pub struct Chip;

    impl crate::formats::Flash for Chip {
        fn capacity(&self) -> u32 {
            CAPACITY.read()
        }

        fn read_inside(&self, address: u32, dst: &mut [u8]) -> bool {
            read(address, dst)
        }
    }

    impl crate::formats::Nor for Chip {
        fn program(&self, address: u32, src: &[u8]) -> bool {
            program(address, src)
        }

        fn erase(&self, address: u32, size: u32) -> bool {
            flash_erase(core::ptr::null_mut(), address, size)
        }
    }

    /// # Safety
    /// `data` has `n` bytes, or `n` is 0.
    #[cfg(any(not(target_arch = "pi32v2"), loader_a, loader_b))]
    #[unsafe(no_mangle)]
    pub unsafe extern "C" fn flash_program(
        _ctx: *mut c_void,
        a: u32,
        data: *const c_void,
        n: u32,
    ) -> bool {
        program(
            a,
            if n == 0 {
                &[]
            } else {
                unsafe { core::slice::from_raw_parts(data.cast(), n as usize) }
            },
        )
    }

    pub fn program(mut a: u32, mut data: &[u8]) -> bool {
        let capacity = CAPACITY.read();
        if a > capacity || data.len() as u32 > capacity - a || !unprotect() {
            return false;
        }
        while !data.is_empty() {
            let (page, rest) = data.split_at((256 - (a & 255) as usize).min(data.len()));
            wren();
            command(2);
            address24(a);
            for &b in page {
                spi_byte(b, false);
            }
            cs(true);
            if !ready() {
                return false;
            }
            a += page.len() as u32;
            data = rest;
        }
        !failed().read()
    }

    #[unsafe(no_mangle)]
    pub extern "C" fn flash_erase(_ctx: *mut c_void, a: u32, n: u32) -> bool {
        let capacity = CAPACITY.read();
        if (n != 4096 && n != 65536)
            || a & (n - 1) != 0
            || a > capacity
            || n > capacity - a
            || !unprotect()
        {
            return false;
        }
        wren();
        command(if n == 4096 { 0x20 } else { 0xd8 });
        address24(a);
        cs(true);
        ready()
    }

    #[unsafe(no_mangle)]
    pub extern "C" fn flash_release() {
        flash_dma_finish();
        SPI_CON.write(0);
        DMA_MUX.write(0);
        ENC_CON.write(0);
    }

    #[unsafe(no_mangle)]
    pub extern "C" fn hal_xip(base: u32, key: u16, width: u32, baud: u32) -> bool {
        flash_release();
        IO_MUX0.clear(bit(16));
        SFC_CON.write(0xf0_0000);
        SFC_CON.write(0);
        SFC_BAUD.write(baud);
        let mode: u32 = match width {
            4 => 0x0028_0381,
            2 => 0x0028_0281,
            _ => 0x0020_0089,
        };
        SFC_CON.write(mode & !0xf0_0000 | 0x90_0000);
        SFC_BASE.write(base);
        ENC_KEY.write(key as u32);
        CACHE_CON.clear(bit(8) | bit(9));
        // The cache's tags (§3.6).
        for (from, to) in [
            (0, 0x4000),
            (0x8000, 0xa000),
            (0xa000, 0xa200),
            (0xb000, 0xb200),
        ] {
            let mut a = from;
            while a < to {
                CACHE_TAGS.add(a).write(0);
                a += 4;
            }
        }
        CACHE_CON.set(bit(8) | bit(9));
        crate::hw::irq_off();
        if !wait(CACHE_CON, bit(14), true) {
            return false;
        }
        ENC_CON.write(1);
        CACHE_CON.set(bit(8) | bit(9));
        sync_cpu();
        true
    }
}

// SPDX-License-Identifier: MIT
//! A way out of the middle of a run, for a test that stands in for a reset, a started application or a power cut:
//! the code under test calls something that does not come back.
//!
//! Where only Rust's own functions lie between, the way out is a panic that `unwind` catches. A panic may not pass
//! a function that has C's calling convention, which the bootloader's entries and stage 0's functions do: there
//! the code runs on a thread of its own (`run`), and leaving stops that thread where it is, for good.
use std::panic::{AssertUnwindSafe, catch_unwind, resume_unwind};
use std::sync::Mutex;
use std::sync::mpsc::{Sender, channel};
use std::thread;

enum End<T> {
    Returned(thread::Result<T>),
    Left(i32),
}

/// The way out of the run that is going on: `leave` takes it.
static OUT: Mutex<Option<Box<dyn FnOnce(i32) + Send>>> = Mutex::new(None);

/// Runs `f` to its end, which is `Ok` with what it returned, or until it calls `leave`, which is `Err` with the
/// code that was given. One run at a time. What `f` borrows it must own or find in a static, as after `leave` its
/// thread stays, stopped, with everything it had.
pub fn run<T: Send + 'static>(f: impl FnOnce() -> T + Send + 'static) -> Result<T, i32> {
    let (tx, rx) = channel();
    let out: Sender<End<T>> = tx.clone();
    let before = OUT
        .lock()
        .unwrap()
        .replace(Box::new(move |code| out.send(End::Left(code)).unwrap()));
    assert!(before.is_none(), "a run inside a run");
    thread::spawn(move || {
        let result = catch_unwind(AssertUnwindSafe(f));
        // It came back: there is no way out any more.
        *OUT.lock().unwrap() = None;
        tx.send(End::Returned(result)).unwrap();
    });
    match rx.recv().unwrap() {
        End::Returned(Ok(value)) => Ok(value),
        // A check failed on the run's thread: it fails here too.
        End::Returned(Err(panic)) => resume_unwind(panic),
        End::Left(code) => Err(code),
    }
}

/// What `leave` throws to `unwind`.
struct Left(i32);

/// As `run`, where no function with C's calling convention lies between `f` and its `leave`: on this thread, which
/// `leave` unwinds, so nothing of the run is left behind.
pub fn unwind<T>(f: impl FnOnce() -> T) -> Result<T, i32> {
    let before = OUT
        .lock()
        .unwrap()
        .replace(Box::new(|code| resume_unwind(Box::new(Left(code)))));
    assert!(before.is_none(), "a run inside a run");
    let result = catch_unwind(AssertUnwindSafe(f));
    // It came back, or left: there is no way out any more.
    *OUT.lock().unwrap() = None;
    match result {
        Ok(value) => Ok(value),
        Err(panic) => match panic.downcast::<Left>() {
            Ok(left) => Err(left.0),
            // A check failed in the run: it fails here too.
            Err(panic) => resume_unwind(panic),
        },
    }
}

/// Ends the run that this is called from: its `run` or `unwind` returns `Err(code)`, and nothing after this call
/// happens.
pub fn leave(code: i32) -> ! {
    let out = OUT.lock().unwrap().take().expect("leave outside a run");
    out(code);
    loop {
        thread::park();
    }
}

#[test]
fn escape() {
    assert!(run(|| 7) == Ok(7));
    assert!(run(|| -> u32 { leave(3) }) == Err(3));
    // Out of a function with C's calling convention, and again after the last one.
    extern "C" fn deep(code: i32) {
        leave(code)
    }
    assert!(run(|| deep(5)) == Err(5));
    assert!(run(|| deep(6)) == Err(6));
    // By unwinding, with what the run borrows, and then on a thread again.
    let mut steps = 0;
    assert!(unwind(|| steps += 1) == Ok(()));
    let left = unwind(|| {
        steps += 1;
        leave(4)
    });
    assert!(left == Err::<(), _>(4) && steps == 2);
    assert!(run(|| deep(8)) == Err(8));
}

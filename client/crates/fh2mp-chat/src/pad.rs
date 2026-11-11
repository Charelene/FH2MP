//! The pad and keys that drive the Guide (plan phase 3, Guide v0). On Windows a thread reads the first connected pad through XInput every
//! 33 ms, with `XInputGetStateEx` (xinput1_4 ordinal 100) so the Guide button is seen; holding it for one second opens or closes the Guide.
//! Home also toggles it while the game or the Guide has the focus. While the Guide is open the thread turns the pad into navigation events
//! (a held direction repeats after 400 ms, then every 80 ms). Other systems get no events yet.

use std::sync::mpsc::Sender;

/// What the Guide gets from the pad and the keyboard.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PadEvent {
    /// The Guide button was held for a second, or Home was pressed: open or close.
    Toggle,
    Up,
    Down,
    Left,
    Right,
    A,
    B,
    X,
    Y,
    Lb,
    Rb,
    Start,
}

pub const GUIDE_HOLD_MS: u64 = 1000;
const REPEAT_DELAY_MS: u64 = 400;
const REPEAT_EVERY_MS: u64 = 80;

/// XInput button bits.
pub mod bits {
    pub const UP: u16 = 0x0001;
    pub const DOWN: u16 = 0x0002;
    pub const LEFT: u16 = 0x0004;
    pub const RIGHT: u16 = 0x0008;
    pub const START: u16 = 0x0010;
    pub const LB: u16 = 0x0100;
    pub const RB: u16 = 0x0200;
    pub const GUIDE: u16 = 0x0400;
    pub const A: u16 = 0x1000;
    pub const B: u16 = 0x2000;
    pub const X: u16 = 0x4000;
    pub const Y: u16 = 0x8000;
}

/// Turns readings (buttons, left stick, time) into events; pure, so it is tested without a pad.
#[derive(Default)]
pub struct Reader {
    prev: u16,
    guide_since: Option<u64>,
    guide_fired: bool,
    dir: Option<PadEvent>,
    next_at: u64,
}

fn direction(buttons: u16, lx: i16, ly: i16) -> Option<PadEvent> {
    use bits::*;
    for (b, e) in [
        (UP, PadEvent::Up),
        (DOWN, PadEvent::Down),
        (LEFT, PadEvent::Left),
        (RIGHT, PadEvent::Right),
    ] {
        if buttons & b != 0 {
            return Some(e);
        }
    }
    let (x, y) = (lx as i32, ly as i32);
    if x.abs().max(y.abs()) < 18000 {
        return None;
    }
    Some(if x.abs() >= y.abs() {
        if x > 0 { PadEvent::Right } else { PadEvent::Left }
    } else if y > 0 {
        PadEvent::Up
    } else {
        PadEvent::Down
    })
}

impl Reader {
    /// One reading at `now_ms`; `open` says whether the Guide is showing (navigation events only then).
    pub fn feed(&mut self, buttons: u16, lx: i16, ly: i16, now_ms: u64, open: bool) -> Vec<PadEvent> {
        use bits::*;
        let mut out = Vec::new();
        if buttons & GUIDE != 0 {
            let since = *self.guide_since.get_or_insert(now_ms);
            if !self.guide_fired && now_ms - since >= GUIDE_HOLD_MS {
                self.guide_fired = true;
                out.push(PadEvent::Toggle);
            }
        } else {
            if open && self.guide_since.is_some() && !self.guide_fired {
                out.push(PadEvent::Toggle); // a short press closes an open Guide
            }
            self.guide_since = None;
            self.guide_fired = false;
        }
        if open {
            let pressed = buttons & !self.prev;
            for (b, e) in [
                (A, PadEvent::A),
                (B, PadEvent::B),
                (X, PadEvent::X),
                (Y, PadEvent::Y),
                (LB, PadEvent::Lb),
                (RB, PadEvent::Rb),
                (START, PadEvent::Start),
            ] {
                if pressed & b != 0 {
                    out.push(e);
                }
            }
            let d = direction(buttons, lx, ly);
            if d != self.dir {
                self.dir = d;
                if let Some(d) = d {
                    out.push(d);
                    self.next_at = now_ms + REPEAT_DELAY_MS;
                }
            } else if let Some(d) = d
                && now_ms >= self.next_at
            {
                out.push(d);
                self.next_at = now_ms + REPEAT_EVERY_MS;
            }
        } else {
            self.dir = None;
        }
        self.prev = buttons;
        out
    }
}

/// Starts the reader thread; events go to `tx`, and `wake` is called after each batch (egui's request_repaint). `open` reads whether the
/// Guide is showing.
pub fn start(tx: Sender<PadEvent>, wake: impl Fn() + Send + 'static, open: impl Fn() -> bool + Send + 'static) {
    #[cfg(windows)]
    win::start(tx, wake, open);
    #[cfg(not(windows))]
    {
        let _ = (tx, wake, open);
        tracing::info!("guide: no pad reader on this system yet");
    }
}

/// Puts this program's top-level window titled `title` at (x, y) with size (w, h) in screen pixels and on top, when it is not there already.
/// Screen pixels, not egui points: with the game on a monitor whose scaling differs from the one the window waits on, egui's points were
/// scaled twice and the Guide came out too big and shifted (2026-11-11, a 4K monitor left of the primary one).
pub fn place_window(title: &str, x: i32, y: i32, w: i32, h: i32) -> bool {
    #[cfg(windows)]
    {
        win::place_window(title, x, y, w, h)
    }
    #[cfg(not(windows))]
    {
        let _ = (title, x, y, w, h);
        false
    }
}

/// This program's window titled `title`: (x, y, w, h) in screen pixels (for the Guide's size log).
pub fn own_window_rect(title: &str) -> Option<(i32, i32, i32, i32)> {
    #[cfg(windows)]
    {
        win::own_window_rect(title)
    }
    #[cfg(not(windows))]
    {
        let _ = title;
        None
    }
}

/// The game window's client area in screen pixels (x, y, w, h): the largest visible top-level window of azimuth.exe.
pub fn game_window() -> Option<(i32, i32, i32, i32)> {
    #[cfg(windows)]
    {
        win::game_window()
    }
    #[cfg(not(windows))]
    {
        None
    }
}

#[cfg(windows)]
mod win {
    use super::*;
    use std::time::{Duration, Instant};
    use windows_sys::Win32::Foundation::{CloseHandle, HWND, LPARAM, POINT, RECT};
    use windows_sys::Win32::Graphics::Gdi::ClientToScreen;
    use windows_sys::Win32::System::LibraryLoader::{GetProcAddress, LoadLibraryW};
    use windows_sys::Win32::System::Threading::{OpenProcess, PROCESS_QUERY_LIMITED_INFORMATION, QueryFullProcessImageNameW};
    use windows_sys::Win32::UI::Input::KeyboardAndMouse::{GetAsyncKeyState, VK_HOME};
    use windows_sys::Win32::UI::WindowsAndMessaging::{
        EnumWindows, GetClientRect, GetForegroundWindow, GetWindowRect, GetWindowTextW, GetWindowThreadProcessId, HWND_TOPMOST, IsWindowVisible,
        SWP_NOACTIVATE, SWP_NOOWNERZORDER, SetWindowPos,
    };

    #[repr(C)]
    #[derive(Default)]
    struct XState {
        packet: u32,
        buttons: u16,
        lt: u8,
        rt: u8,
        lx: i16,
        ly: i16,
        rx: i16,
        ry: i16,
    }
    type GetState = unsafe extern "system" fn(u32, *mut XState) -> u32;

    fn wide(s: &str) -> Vec<u16> {
        s.encode_utf16().chain(std::iter::once(0)).collect()
    }

    fn load() -> Option<(GetState, bool)> {
        for dll in ["xinput1_4.dll", "xinput1_3.dll"] {
            let name = wide(dll);
            let m = unsafe { LoadLibraryW(name.as_ptr()) };
            if m.is_null() {
                continue;
            }
            // ordinal 100: XInputGetStateEx, which also reports the Guide button
            if let Some(f) = unsafe { GetProcAddress(m, 100usize as *const u8) } {
                return Some((unsafe { std::mem::transmute::<unsafe extern "system" fn() -> isize, GetState>(f) }, true));
            }
            if let Some(f) = unsafe { GetProcAddress(m, c"XInputGetState".as_ptr() as *const u8) } {
                return Some((unsafe { std::mem::transmute::<unsafe extern "system" fn() -> isize, GetState>(f) }, false));
            }
        }
        None
    }

    fn process_is_azimuth(pid: u32) -> bool {
        let h = unsafe { OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, 0, pid) };
        if h.is_null() {
            return false;
        }
        let mut buf = [0u16; 520];
        let mut n = buf.len() as u32;
        let ok = unsafe { QueryFullProcessImageNameW(h, 0, buf.as_mut_ptr(), &mut n) } != 0;
        unsafe { CloseHandle(h) };
        ok && String::from_utf16_lossy(&buf[..n as usize])
            .to_ascii_lowercase()
            .ends_with("\\azimuth.exe")
    }

    struct Found {
        best: Option<(i32, i32, i32, i32)>,
    }

    unsafe extern "system" fn each(hwnd: HWND, lp: LPARAM) -> i32 {
        let found = unsafe { &mut *(lp as *mut Found) };
        if unsafe { IsWindowVisible(hwnd) } == 0 {
            return 1;
        }
        let mut pid = 0u32;
        unsafe { GetWindowThreadProcessId(hwnd, &mut pid) };
        if pid == 0 || !process_is_azimuth(pid) {
            return 1;
        }
        let mut r = RECT {
            left: 0,
            top: 0,
            right: 0,
            bottom: 0,
        };
        if unsafe { GetClientRect(hwnd, &mut r) } == 0 {
            return 1;
        }
        let mut p = POINT { x: 0, y: 0 };
        unsafe { ClientToScreen(hwnd, &mut p) };
        let (w, h) = (r.right - r.left, r.bottom - r.top);
        if w * h > found.best.map(|b| b.2 * b.3).unwrap_or(0) {
            found.best = Some((p.x, p.y, w, h));
        }
        1
    }

    struct Own {
        title: Vec<u16>,
        hwnd: HWND,
    }

    unsafe extern "system" fn own(hwnd: HWND, lp: LPARAM) -> i32 {
        let o = unsafe { &mut *(lp as *mut Own) };
        let mut pid = 0u32;
        unsafe { GetWindowThreadProcessId(hwnd, &mut pid) };
        if pid != std::process::id() {
            return 1;
        }
        let mut buf = [0u16; 128];
        let n = unsafe { GetWindowTextW(hwnd, buf.as_mut_ptr(), buf.len() as i32) };
        if n > 0 && buf[..n as usize] == o.title[..] {
            o.hwnd = hwnd;
            return 0;
        }
        1
    }

    pub fn own_window_rect(title: &str) -> Option<(i32, i32, i32, i32)> {
        let mut o = Own {
            title: title.encode_utf16().collect(),
            hwnd: std::ptr::null_mut(),
        };
        unsafe { EnumWindows(Some(own), &mut o as *mut Own as LPARAM) };
        if o.hwnd.is_null() {
            return None;
        }
        let mut r = RECT {
            left: 0,
            top: 0,
            right: 0,
            bottom: 0,
        };
        unsafe { GetWindowRect(o.hwnd, &mut r) };
        Some((r.left, r.top, r.right - r.left, r.bottom - r.top))
    }

    pub fn place_window(title: &str, x: i32, y: i32, w: i32, h: i32) -> bool {
        let mut o = Own {
            title: title.encode_utf16().collect(),
            hwnd: std::ptr::null_mut(),
        };
        unsafe { EnumWindows(Some(own), &mut o as *mut Own as LPARAM) };
        if o.hwnd.is_null() {
            return false;
        }
        let mut r = RECT {
            left: 0,
            top: 0,
            right: 0,
            bottom: 0,
        };
        unsafe { GetWindowRect(o.hwnd, &mut r) };
        if (r.left, r.top, r.right - r.left, r.bottom - r.top) == (x, y, w, h) {
            return true;
        }
        unsafe { SetWindowPos(o.hwnd, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE | SWP_NOOWNERZORDER) != 0 }
    }

    pub fn game_window() -> Option<(i32, i32, i32, i32)> {
        let mut found = Found { best: None };
        unsafe { EnumWindows(Some(each), &mut found as *mut Found as LPARAM) };
        found.best
    }

    /// Whether the game or this program has the focus, and the name of what has it (for the log).
    fn game_or_guide_in_front() -> (bool, String) {
        let hwnd = unsafe { GetForegroundWindow() };
        if hwnd.is_null() {
            return (false, "nothing".into());
        }
        let mut pid = 0u32;
        unsafe { GetWindowThreadProcessId(hwnd, &mut pid) };
        if pid == std::process::id() {
            return (true, "the Guide".into());
        }
        let name = image_name(pid);
        (name.to_ascii_lowercase().ends_with(r"\azimuth.exe"), name)
    }

    fn image_name(pid: u32) -> String {
        let h = unsafe { OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, 0, pid) };
        if h.is_null() {
            return format!("pid {pid}");
        }
        let mut buf = [0u16; 520];
        let mut n = buf.len() as u32;
        let ok = unsafe { QueryFullProcessImageNameW(h, 0, buf.as_mut_ptr(), &mut n) } != 0;
        unsafe { CloseHandle(h) };
        if ok {
            String::from_utf16_lossy(&buf[..n as usize])
        } else {
            format!("pid {pid}")
        }
    }

    pub fn start(tx: Sender<PadEvent>, wake: impl Fn() + Send + 'static, open: impl Fn() -> bool + Send + 'static) {
        let Some((get, ex)) = load() else {
            tracing::warn!("guide: no XInput DLL: the pad cannot open the Guide (Home still does)");
            return;
        };
        tracing::info!(
            guide_button = ex,
            "guide: pad reader started (hold the Guide button 1 s, or press Home in the game)"
        );
        std::thread::Builder::new()
            .name("guide-pad".into())
            .spawn(move || {
                let t0 = Instant::now();
                let mut reader = Reader::default();
                let (mut slot, mut next_scan, mut home_was) = (None::<u32>, Duration::ZERO, false);
                let (mut guide_seen, mut home_logged) = (false, 0u32);
                loop {
                    std::thread::sleep(Duration::from_millis(33));
                    let now = t0.elapsed();
                    let mut events = Vec::new();
                    let home = unsafe { GetAsyncKeyState(VK_HOME as i32) } as u16 & 0x8000 != 0;
                    if home && !home_was {
                        let (ok, front) = game_or_guide_in_front();
                        if ok {
                            events.push(PadEvent::Toggle);
                        } else if home_logged < 5 {
                            home_logged += 1;
                            tracing::info!(front = %front, "guide: Home pressed, but neither the game nor the Guide is in front: ignored");
                        }
                    }
                    home_was = home;
                    let mut st = XState::default();
                    if let Some(s) = slot
                        && unsafe { get(s, &mut st) } != 0
                    {
                        slot = None;
                    }
                    if slot.is_none() && now >= next_scan {
                        next_scan = now + Duration::from_secs(2);
                        slot = (0..4).find(|&i| unsafe { get(i, &mut st) } == 0);
                        if let Some(s) = slot {
                            tracing::info!(slot = s, "guide: pad found in XInput slot");
                        }
                    }
                    if slot.is_some() && st.buttons & bits::GUIDE != 0 && !guide_seen {
                        guide_seen = true;
                        tracing::info!("guide: the pad's Guide button is seen (hold it for a second)");
                    }
                    if slot.is_some() {
                        events.extend(reader.feed(st.buttons, st.lx, st.ly, now.as_millis() as u64, open()));
                    }
                    if !events.is_empty() {
                        for e in events {
                            if tx.send(e).is_err() {
                                return;
                            }
                        }
                        wake();
                    }
                }
            })
            .expect("guide pad thread");
    }
}

#[cfg(test)]
mod tests {
    use super::bits::*;
    use super::*;

    #[test]
    fn guide_hold_toggles_once() {
        let mut r = Reader::default();
        let mut got = Vec::new();
        for t in (0..1500).step_by(33) {
            got.extend(r.feed(GUIDE, 0, 0, t, false));
        }
        assert_eq!(got, vec![PadEvent::Toggle]);
        assert!(r.feed(0, 0, 0, 1600, true).is_empty()); // releasing after the hold does not close it again
        let mut close = Vec::new();
        close.extend(r.feed(GUIDE, 0, 0, 1700, true));
        close.extend(r.feed(0, 0, 0, 1800, true)); // a short press closes an open Guide
        assert_eq!(close, vec![PadEvent::Toggle]);
    }

    #[test]
    fn navigation_only_while_open_with_repeat() {
        let mut r = Reader::default();
        assert!(r.feed(A | DOWN, 0, 0, 0, false).is_empty());
        let mut r = Reader::default();
        let mut got = Vec::new();
        for t in (0..=1000).step_by(16) {
            got.extend(r.feed(DOWN, 0, 0, t, true));
        }
        assert_eq!(got.first(), Some(&PadEvent::Down));
        assert!(got.len() >= 7 && got.len() <= 10, "{} repeats", got.len());
        assert_eq!(r.feed(A, 0, 0, 1100, true), vec![PadEvent::A]);
        assert!(r.feed(A, 0, 0, 1120, true).is_empty());
        assert_eq!(r.feed(0, 30000, 0, 1200, true), vec![PadEvent::Right]);
    }
}

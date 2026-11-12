//! Chat link v1 wire format between the shim (in the game) and this core: loopback TCP frames, laid out in
//! `shim/tests/vectors/chat_link.txt`. The C side is `shim/src/chat_link_wire.h`; both are tested against those vectors.

/// The protocol version both sides speak.
pub const VERSION: u16 = 1;
/// Largest type + payload of one frame.
pub const MAX_FRAME: usize = 65536;
/// Largest OPEN_PAGE argument.
pub const MAX_ARG: usize = 512;

/// One message.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Msg {
    Hello {
        version: u16,
        secret: [u8; 32],
        pid: u32,
        hwnd: u64,
        xuid: u64,
    },
    Welcome {
        version: u16,
        flags: u8,
    },
    Pad {
        buttons: u16,
        lt: u8,
        rt: u8,
        lx: i16,
        ly: i16,
        rx: i16,
        ry: i16,
    },
    Guide {
        cause: u8,
    },
    GuideState {
        open: bool,
    },
    Window {
        x: i32,
        y: i32,
        w: i32,
        h: i32,
        flags: u8,
    },
    GameState {
        online: bool,
        mode: u8,
        players: u16,
    },
    OpenPage {
        page: u8,
        arg: String,
    },
    Bye {
        reason: u8,
    },
    /// A type this build does not know (skipped by its length).
    Unknown {
        kind: u16,
    },
}

/// What [`frame`] found at the start of a buffer.
#[derive(Debug, PartialEq, Eq)]
pub enum Framed {
    /// A whole frame: the message and how many bytes it took.
    Msg(Msg, usize),
    /// More bytes are needed.
    Partial,
    /// An impossible length or a malformed known message: the link must close.
    Bad,
}

impl Msg {
    fn kind(&self) -> u16 {
        match self {
            Msg::Hello { .. } => 1,
            Msg::Welcome { .. } => 2,
            Msg::Pad { .. } => 3,
            Msg::Guide { .. } => 4,
            Msg::GuideState { .. } => 5,
            Msg::Window { .. } => 6,
            Msg::GameState { .. } => 7,
            Msg::OpenPage { .. } => 8,
            Msg::Bye { .. } => 9,
            Msg::Unknown { kind } => *kind,
        }
    }

    /// The whole frame; None for an argument longer than [`MAX_ARG`] or an Unknown message.
    pub fn encode(&self) -> Option<Vec<u8>> {
        let mut p = Vec::new();
        match self {
            Msg::Hello {
                version,
                secret,
                pid,
                hwnd,
                xuid,
            } => {
                p.extend_from_slice(&version.to_le_bytes());
                p.extend_from_slice(secret);
                p.extend_from_slice(&pid.to_le_bytes());
                p.extend_from_slice(&hwnd.to_le_bytes());
                p.extend_from_slice(&xuid.to_le_bytes());
            }
            Msg::Welcome { version, flags } => {
                p.extend_from_slice(&version.to_le_bytes());
                p.push(*flags);
            }
            Msg::Pad {
                buttons,
                lt,
                rt,
                lx,
                ly,
                rx,
                ry,
            } => {
                p.extend_from_slice(&buttons.to_le_bytes());
                p.push(*lt);
                p.push(*rt);
                for v in [lx, ly, rx, ry] {
                    p.extend_from_slice(&v.to_le_bytes());
                }
            }
            Msg::Guide { cause } => p.push(*cause),
            Msg::GuideState { open } => p.push(*open as u8),
            Msg::Window { x, y, w, h, flags } => {
                for v in [x, y, w, h] {
                    p.extend_from_slice(&v.to_le_bytes());
                }
                p.push(*flags);
            }
            Msg::GameState { online, mode, players } => {
                p.push(*online as u8);
                p.push(*mode);
                p.extend_from_slice(&players.to_le_bytes());
            }
            Msg::OpenPage { page, arg } => {
                if arg.len() > MAX_ARG {
                    return None;
                }
                p.push(*page);
                p.extend_from_slice(&(arg.len() as u16).to_le_bytes());
                p.extend_from_slice(arg.as_bytes());
            }
            Msg::Bye { reason } => p.push(*reason),
            Msg::Unknown { .. } => return None,
        }
        let mut out = Vec::with_capacity(p.len() + 6);
        out.extend_from_slice(&((p.len() + 2) as u32).to_le_bytes());
        out.extend_from_slice(&self.kind().to_le_bytes());
        out.extend_from_slice(&p);
        Some(out)
    }
}

fn u16_at(p: &[u8], i: usize) -> u16 {
    u16::from_le_bytes([p[i], p[i + 1]])
}

fn u32_at(p: &[u8], i: usize) -> u32 {
    u32::from_le_bytes(p[i..i + 4].try_into().unwrap())
}

fn u64_at(p: &[u8], i: usize) -> u64 {
    u64::from_le_bytes(p[i..i + 8].try_into().unwrap())
}

fn decode(kind: u16, p: &[u8]) -> Option<Msg> {
    let sized = |n: usize| (p.len() == n).then_some(());
    Some(match kind {
        1 => {
            sized(54)?;
            Msg::Hello {
                version: u16_at(p, 0),
                secret: p[2..34].try_into().unwrap(),
                pid: u32_at(p, 34),
                hwnd: u64_at(p, 38),
                xuid: u64_at(p, 46),
            }
        }
        2 => {
            sized(3)?;
            Msg::Welcome {
                version: u16_at(p, 0),
                flags: p[2],
            }
        }
        3 => {
            sized(12)?;
            Msg::Pad {
                buttons: u16_at(p, 0),
                lt: p[2],
                rt: p[3],
                lx: u16_at(p, 4) as i16,
                ly: u16_at(p, 6) as i16,
                rx: u16_at(p, 8) as i16,
                ry: u16_at(p, 10) as i16,
            }
        }
        4 => {
            sized(1)?;
            Msg::Guide { cause: p[0] }
        }
        5 => {
            sized(1)?;
            Msg::GuideState { open: p[0] != 0 }
        }
        6 => {
            sized(17)?;
            Msg::Window {
                x: u32_at(p, 0) as i32,
                y: u32_at(p, 4) as i32,
                w: u32_at(p, 8) as i32,
                h: u32_at(p, 12) as i32,
                flags: p[16],
            }
        }
        7 => {
            sized(4)?;
            Msg::GameState {
                online: p[0] != 0,
                mode: p[1],
                players: u16_at(p, 2),
            }
        }
        8 => {
            if p.len() < 3 {
                return None;
            }
            let n = u16_at(p, 1) as usize;
            if n > MAX_ARG || p.len() != 3 + n {
                return None;
            }
            Msg::OpenPage {
                page: p[0],
                arg: String::from_utf8(p[3..].to_vec()).ok()?,
            }
        }
        9 => {
            sized(1)?;
            Msg::Bye { reason: p[0] }
        }
        k => Msg::Unknown { kind: k },
    })
}

/// The first frame in `buf`.
pub fn frame(buf: &[u8]) -> Framed {
    if buf.len() < 4 {
        return Framed::Partial;
    }
    let n = u32_at(buf, 0) as usize;
    if !(2..=MAX_FRAME).contains(&n) {
        return Framed::Bad;
    }
    if buf.len() < 4 + n {
        return Framed::Partial;
    }
    match decode(u16_at(buf, 4), &buf[6..4 + n]) {
        Some(m) => Framed::Msg(m, 4 + n),
        None => Framed::Bad,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn vectors() -> Vec<(String, Vec<u8>)> {
        let path = concat!(env!("CARGO_MANIFEST_DIR"), "/../../../shim/tests/vectors/chat_link.txt");
        let text = std::fs::read_to_string(path).expect("chat_link.txt");
        text.lines()
            .filter(|l| !l.starts_with('#') && !l.trim().is_empty())
            .map(|l| {
                let (name, hex) = l.split_once(' ').unwrap();
                let bytes = (0..hex.len())
                    .step_by(2)
                    .map(|i| u8::from_str_radix(&hex[i..i + 2], 16).unwrap())
                    .collect();
                (name.to_string(), bytes)
            })
            .collect()
    }

    fn expected(name: &str) -> Msg {
        let mut secret = [0u8; 32];
        for (i, b) in secret.iter_mut().enumerate() {
            *b = i as u8;
        }
        match name {
            "hello" => Msg::Hello {
                version: 1,
                secret,
                pid: 4242,
                hwnd: 0x1a0b2c,
                xuid: 0x0009_0000_27d5_236b,
            },
            "welcome" => Msg::Welcome { version: 1, flags: 1 },
            "pad" => Msg::Pad {
                buttons: 0x1401,
                lt: 0,
                rt: 255,
                lx: -32768,
                ly: 32767,
                rx: 100,
                ry: -100,
            },
            "guide" => Msg::Guide { cause: 1 },
            "guide_state" => Msg::GuideState { open: true },
            "window" => Msg::Window {
                x: -1920,
                y: 0,
                w: 1920,
                h: 1080,
                flags: 1,
            },
            "game_state" => Msg::GameState {
                online: true,
                mode: 1,
                players: 11,
            },
            "open_page" => Msg::OpenPage {
                page: 1,
                arg: "freeroam:7741dc1d".into(),
            },
            "bye" => Msg::Bye { reason: 2 },
            other => panic!("no expected value for vector {other}"),
        }
    }

    #[test]
    fn matches_the_shared_vectors() {
        let v = vectors();
        assert_eq!(v.len(), 9);
        for (name, bytes) in v {
            let want = expected(&name);
            assert_eq!(want.encode().as_deref(), Some(&bytes[..]), "{name} encodes differently");
            assert_eq!(frame(&bytes), Framed::Msg(want, bytes.len()), "{name} decodes differently");
        }
    }

    #[test]
    fn framing() {
        let g = Msg::Guide { cause: 1 }.encode().unwrap();
        let w = Msg::Welcome { version: 1, flags: 0 }.encode().unwrap();
        let both = [g.clone(), w.clone()].concat();
        assert_eq!(frame(&both[..3]), Framed::Partial);
        assert_eq!(frame(&both[..g.len() - 1]), Framed::Partial);
        assert_eq!(frame(&both), Framed::Msg(Msg::Guide { cause: 1 }, g.len()));
        assert_eq!(frame(&both[g.len()..]), Framed::Msg(Msg::Welcome { version: 1, flags: 0 }, w.len()));
        assert_eq!(frame(&[1, 0, 0, 0, 0, 0]), Framed::Bad); // length 1
        assert_eq!(frame(&[1, 0, 1, 0, 0, 0]), Framed::Bad); // length 65537
        assert_eq!(frame(&[3, 0, 0, 0, 77, 0, 9]), Framed::Msg(Msg::Unknown { kind: 77 }, 7));
        assert_eq!(frame(&[4, 0, 0, 0, 3, 0, 1, 2]), Framed::Bad); // a PAD of 2 bytes
        assert!(
            Msg::OpenPage {
                page: 1,
                arg: "x".repeat(MAX_ARG + 1)
            }
            .encode()
            .is_none()
        );
    }
}

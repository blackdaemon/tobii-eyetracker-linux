//! cargo run --example gaze [seconds]
use std::cell::Cell;
use std::rc::Rc;
use std::time::{Duration, Instant};

use tobii_stream_engine::Api;

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let secs: u64 = std::env::args().nth(1).and_then(|s| s.parse().ok()).unwrap_or(5);

    let api = Api::new()?;
    let mut dev = api.connect_first()?;
    let info = dev.info()?;
    println!("{} {} firmware {}", info.model, info.serial_number, info.firmware_version);

    // Closures are 'static: share state through Rc<Cell<..>> (or channels).
    let eye_dist = Rc::new(Cell::new(0.0f32));
    let d = eye_dist.clone();
    dev.subscribe_eye_position(move |e| {
        if let Some(p) = e.left_mm.or(e.right_mm) {
            d.set(p[2]);
        }
    })?;
    let e = eye_dist.clone();
    dev.subscribe_gaze(move |g| {
        println!("{:>12} gaze {:.4} {:.4}  (eyes at {:.0} mm)", g.timestamp_us, g.x, g.y, e.get())
    })?;

    let end = Instant::now() + Duration::from_secs(secs);
    while Instant::now() < end {
        dev.process()?;
    }
    Ok(())
}

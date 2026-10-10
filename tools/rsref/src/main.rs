//! rsref: reference + benchmark tool for the Rust side (png crate git, image-rs, fdeflate).
//!   rsref apng in.png out.raw               png crate raw APNG frames (IDENTITY), concatenated
//!   rsref decode MODE in.png out.raw         MODE = raw (png IDENTITY) | expand (png EXPAND) | imagers (image-rs)
//!   rsref encode SPEC in.png out.png         SPEC = PRESET[:FILTER[:DEFLATE]] (see encode_png)
//!   rsref bench-decode MODE REPS files...    CSV: tool,op,param,file,rep,width,height,ns,bytes
//!   rsref bench-encode PRESET REPS files...  source PNG decoded (IDENTITY) untimed, then encoded with same format
//!   rsref bench-deflate LEVEL REPS files...  fdeflate raw zlib compress (LEVEL 1..9 | rle | ultrafast), bytes = out
//!   rsref bench-inflate - REPS orig:stream... fdeflate zlib decompress, verified
//! Timing per (file, rep): construction + header + allocation + work; 1 untimed warm-up rep.
use std::io::Cursor;
use std::time::Instant;

fn decode_png(data: &[u8], mode: &str) -> (u32, u32, Vec<u8>) {
    if mode == "imagers" {
        use image::ImageDecoder;
        let mut dec = image::codecs::png::PngDecoder::new(Cursor::new(data));
        let layout = dec.prepare_image().unwrap();
        let mut buf = vec![0u8; usize::try_from(layout.total_bytes()).unwrap()];
        dec.read_image(&mut buf).unwrap();
        let (w, h) = layout.layout.dimensions();
        return (w, h, buf);
    }
    let mut dec = png::Decoder::new(Cursor::new(data));
    dec.set_transformations(if mode == "expand" { png::Transformations::EXPAND } else { png::Transformations::IDENTITY });
    let mut rd = dec.read_info().unwrap();
    let mut buf = vec![0u8; rd.output_buffer_size().unwrap()];
    let fi = rd.next_frame(&mut buf).unwrap();
    buf.truncate(fi.buffer_size());
    (fi.width, fi.height, buf)
}

struct Src { w: u32, h: u32, ct: png::ColorType, bd: png::BitDepth, px: Vec<u8>, pal: Option<Vec<u8>>, trns: Option<Vec<u8>> }
fn load_src(data: &[u8]) -> Src {
    let dec = png::Decoder::new(Cursor::new(data));
    let mut rd = dec.read_info().unwrap();
    let mut px = vec![0u8; rd.output_buffer_size().unwrap()];
    let fi = rd.next_frame(&mut px).unwrap(); px.truncate(fi.buffer_size());
    let info = rd.info();
    Src { w: fi.width, h: fi.height, ct: info.color_type, bd: info.bit_depth, px,
          pal: info.palette.as_ref().map(|p| p.to_vec()), trns: info.trns.as_ref().map(|t| t.to_vec()) }
}
/// PRESET[:FILTER[:DEFLATE]]  PRESET = fastest|fast|balanced|high,
/// FILTER = none|sub|up|avg|paeth|adaptive|minentropy, DEFLATE = ultrafast|1..9
fn encode_png(s: &Src, spec: &str) -> Vec<u8> {
    let mut it = spec.split(':');
    let preset = it.next().unwrap_or("balanced");
    let mut dest = Vec::new();
    let cl = match preset { "fastest" => png::Compression::Fastest, "fast" => png::Compression::Fast,
                            "high" => png::Compression::High, "none" => png::Compression::NoCompression,
                            _ => png::Compression::Balanced };
    let mut enc = png::Encoder::new(&mut dest, s.w, s.h);
    enc.set_color(s.ct); enc.set_depth(s.bd); enc.set_compression(cl);
    if let Some(f) = it.next() {
        if !f.is_empty() {
            enc.set_filter(match f { "none" => png::Filter::NoFilter, "sub" => png::Filter::Sub, "up" => png::Filter::Up,
                "avg" => png::Filter::Avg, "paeth" => png::Filter::Paeth, "minentropy" => png::Filter::MinEntropy,
                _ => png::Filter::Adaptive });
        }
    }
    if let Some(d) = it.next() {
        enc.set_deflate_compression(if d == "ultrafast" { png::DeflateCompression::FdeflateUltraFast }
                                    else { png::DeflateCompression::Level(d.parse().unwrap()) });
    }
    if let Some(p) = &s.pal { enc.set_palette(p.clone()); }
    if let Some(t) = &s.trns { enc.set_trns(t.clone()); }
    enc.write_header().unwrap().write_image_data(&s.px).unwrap();
    dest
}

fn main() {
    let a: Vec<String> = std::env::args().skip(1).collect();
    let cmd = a[0].as_str();
    let tool = if cfg!(feature = "zlibrs") { "png-zlibrs" } else { "png" };
    match cmd {
        "apng" => {
            // png crate raw APNG frames (IDENTITY), concatenated; prints one line per frame: "w h x y num den dispose blend"
            let d = std::fs::read(&a[1]).unwrap();
            let mut out = Vec::new();
            match png::Decoder::new(Cursor::new(&d)).read_info() {
                Ok(mut rd) => {
                    let n = rd.info().animation_control().map(|c| c.num_frames).unwrap_or(1);
                    let skip_default = rd.info().animation_control().is_some() && rd.info().frame_control().is_none();
                    let mut buf = vec![0u8; rd.output_buffer_size().unwrap()];
                    if skip_default { if rd.next_frame(&mut buf).is_err() { println!("error"); return; } }
                    for _ in 0..n {
                        match rd.next_frame(&mut buf) {
                            Ok(fi) => {
                                out.extend_from_slice(&buf[..fi.buffer_size()]);
                                match rd.info().frame_control() {
                                    Some(fc) => println!("{} {} {} {} {} {} {} {}", fc.width, fc.height, fc.x_offset, fc.y_offset,
                                                         fc.delay_num, fc.delay_den, fc.dispose_op as u8, fc.blend_op as u8),
                                    None => println!("{} {} 0 0 0 0 0 0", fi.width, fi.height),
                                }
                            }
                            Err(e) => { println!("error {e}"); break; }
                        }
                    }
                }
                Err(e) => println!("error {e}"),
            }
            std::fs::write(&a[2], &out).unwrap();
        }
        "decode" => { let d = std::fs::read(&a[2]).unwrap(); let (w, h, px) = decode_png(&d, &a[1]); std::fs::write(&a[3], &px).unwrap(); println!("{w} {h}"); }
        "encode" => { let d = std::fs::read(&a[2]).unwrap(); let s = load_src(&d); std::fs::write(&a[3], encode_png(&s, &a[1])).unwrap(); }
        "bench-decode" | "bench-encode" | "bench-deflate" | "bench-inflate" => {
            let param = a[1].as_str(); let reps: usize = a[2].parse().unwrap();
            for file in &a[3..] {
                if cmd == "bench-inflate" {
                    let (o, s) = file.split_once(':').unwrap();
                    let orig = std::fs::read(o).unwrap(); let st = std::fs::read(s).unwrap();
                    for r in 0..=reps {
                        let t = Instant::now();
                        let mut out = vec![0u8; orig.len()];
                        let mut d = fdeflate::Decompressor::new();
                        let (_, n) = d.read(&st, &mut out, 0).expect("inflate"); assert!(d.is_done()); out.truncate(n);
                        let ns = t.elapsed().as_nanos();
                        assert!(out == orig, "verify {s}");
                        if r > 0 { println!("fdeflate,inflate,-,{o},{r},0,0,{ns},{}", out.len()); }
                    }
                    continue;
                }
                let data = std::fs::read(file).unwrap();
                let src = if cmd == "bench-encode" { Some(load_src(&data)) } else { None };
                for r in 0..=reps {
                    let (w, h, bytes, ns);
                    match cmd {
                        "bench-decode" => { let t = Instant::now(); let (pw, ph, px) = decode_png(&data, param); ns = t.elapsed().as_nanos(); w = pw; h = ph; bytes = px.len(); }
                        "bench-encode" => { let s = src.as_ref().unwrap(); let t = Instant::now(); let out = encode_png(s, param); ns = t.elapsed().as_nanos(); w = s.w; h = s.h; bytes = out.len();
                                            if r == reps { if let Ok(dir) = std::env::var("DUMP_DIR") { std::fs::write(format!("{dir}/{}.{tool}-{}.png", file.rsplit('/').next().unwrap(), param.replace(':', "_")), &out).unwrap(); } } }
                        _ => {
                            let t = Instant::now();
                            let out = match param {
                                "ultrafast" => { let mut c = fdeflate::UltraFastCompressor::new(Vec::new()).unwrap(); c.write_data(&data).unwrap(); c.finish().unwrap() }
                                "rle" => { let mut c = fdeflate::Compressor::new_rle(Vec::new(), true).unwrap(); c.write_data(&data).unwrap(); c.finish().unwrap() }
                                #[cfg(feature = "zlibrs")]
                                l if l.starts_with("zrs") => { use std::io::Write; let mut e = flate2::write::ZlibEncoder::new(Vec::new(), flate2::Compression::new(l[3..].parse().unwrap())); e.write_all(&data).unwrap(); e.finish().unwrap() }
                                l => { let mut c = fdeflate::Compressor::new(Vec::new(), l.parse().unwrap(), true).unwrap(); c.write_data(&data).unwrap(); c.finish().unwrap() }
                            };
                            ns = t.elapsed().as_nanos(); w = 0; h = 0; bytes = out.len();
                            if r == reps { let back = fdeflate::decompress_to_vec(&out).unwrap(); assert!(back == data);
                                           if let Ok(dir) = std::env::var("DUMP_DIR") { std::fs::write(format!("{dir}/{}.{param}.zlib", file.rsplit('/').next().unwrap()), &out).unwrap(); } }
                        }
                    }
                    if r > 0 { println!("{},{cmd},{param},{file},{r},{w},{h},{ns},{bytes}", if cmd.ends_with("flate") { "fdeflate" } else { tool }); }
                }
            }
        }
        _ => { eprintln!("unknown command"); std::process::exit(2); }
    }
}

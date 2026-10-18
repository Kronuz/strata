// Many small indexes (strata's docs/perf/small-indexes.md, M1).
//   many build ROOT COUNT DOCS < corpus   COUNT keyed indexes of DOCS documents each (the
//                                         corpus's first lines, cycled), one writer thread
//                                         each, on every core
//   many open ROOT COUNT                  opens them all, a reader each, and reports each
//                                         open's time and, per index, the resident bytes,
//                                         descriptors, and mappings it adds (/proc/self)
use std::env;
use std::io::BufRead;
use std::path::Path;
use std::time::Instant;
use tantivy::schema::{Schema, Value, FAST, STORED, STRING, TEXT};
use tantivy::{Index, IndexReader, ReloadPolicy, TantivyDocument, Term};

fn schema() -> Schema {
    let mut b = Schema::builder();
    b.add_text_field("id", STRING | STORED);
    b.add_text_field("text", TEXT);
    b.add_u64_field("sort_field", FAST);
    b.build()
}

fn build(root: &Path, count: usize, docs: usize) -> tantivy::Result<()> {
    let want = docs.max((count * docs).min(200_000));
    let lines: Vec<String> = std::io::stdin()
        .lock()
        .lines()
        .map_while(Result::ok)
        .filter(|l| !l.trim().is_empty())
        .take(want)
        .collect();
    assert!(lines.len() >= docs, "only {} lines", lines.len());
    let schema = schema();
    let id = schema.get_field("id").unwrap();
    let threads = std::thread::available_parallelism().map_or(1, |n| n.get());
    std::thread::scope(|s| -> tantivy::Result<()> {
        let mut handles = Vec::new();
        for k in 0..threads {
            let (lines, schema) = (&lines, &schema);
            handles.push(s.spawn(move || -> tantivy::Result<()> {
                let mut i = k;
                while i < count {
                    let dir = root.join(i.to_string());
                    std::fs::create_dir_all(&dir)?;
                    let index = Index::create_in_dir(&dir, schema.clone())?;
                    let mut w = index.writer_with_num_threads(1, 15_000_000)?;
                    for d in 0..docs {
                        let doc = TantivyDocument::parse_json(schema, &lines[(i * docs + d) % lines.len()])?;
                        if let Some(key) = doc.get_first(id).and_then(|v| v.as_str()) {
                            w.delete_term(Term::from_field_text(id, key));
                        }
                        w.add_document(doc)?;
                    }
                    w.commit()?;
                    w.wait_merging_threads()?;
                    i += threads;
                }
                Ok(())
            }));
        }
        for h in handles {
            h.join().expect("builder thread")?;
        }
        Ok(())
    })?;
    eprintln!("many build: {} indexes of {} documents", count, docs);
    Ok(())
}

#[derive(Default, Clone, Copy)]
struct Holdings {
    rss: i64,
    anon: i64,
    file: i64,
    fds: i64,
    maps: i64,
}

fn sample() -> Holdings {
    let mut h = Holdings::default();
    if let Ok(status) = std::fs::read_to_string("/proc/self/status") {
        for line in status.lines() {
            let value = |name: &str| line.strip_prefix(name).and_then(|v| v.trim().split(' ').next().and_then(|n| n.parse().ok()));
            if let Some(v) = value("VmRSS:") { h.rss = v; }
            if let Some(v) = value("RssAnon:") { h.anon = v; }
            if let Some(v) = value("RssFile:") { h.file = v; }
        }
    }
    h.fds = std::fs::read_dir("/proc/self/fd").map_or(-1, |d| d.count() as i64);
    h.maps = std::fs::read_to_string("/proc/self/maps").map_or(-1, |m| m.lines().count() as i64);
    h
}

fn open(root: &Path, count: usize) -> tantivy::Result<()> {
    let before = sample();
    let mut open: Vec<(Index, IndexReader)> = Vec::with_capacity(count);
    let mut us: Vec<f64> = Vec::with_capacity(count);
    let start = Instant::now();
    for i in 0..count {
        let t0 = Instant::now();
        let index = Index::open_in_dir(root.join(i.to_string()))?;
        let reader = index.reader_builder().reload_policy(ReloadPolicy::Manual).try_into()?;
        let _ = reader.searcher();
        open.push((index, reader));
        us.push(t0.elapsed().as_secs_f64() * 1e6);
    }
    let total = start.elapsed().as_secs_f64();
    let after = sample();
    us.sort_by(|a, b| a.partial_cmp(b).unwrap());
    let at = |q: f64| us[((q * us.len() as f64) as usize).min(us.len() - 1)];
    let mean = us.iter().sum::<f64>() / us.len() as f64;
    let per = |a: i64, b: i64, scale: f64| (b - a) as f64 * scale / count as f64;
    let segments: usize = open.iter().map(|(_, r)| r.searcher().segment_readers().len()).sum();
    println!("open-many: {} indexes, {} segments, {:.3} s", count, segments, total);
    println!("open us: mean {:.1} p50 {:.1} p99 {:.1} max {:.1}", mean, at(0.5), at(0.99), us[us.len() - 1]);
    println!(
        "per index: rss {:.0} B, anon {:.0} B, file {:.0} B, fds {:.2}, maps {:.2}",
        per(before.rss, after.rss, 1024.0),
        per(before.anon, after.anon, 1024.0),
        per(before.file, after.file, 1024.0),
        per(before.fds, after.fds, 1.0),
        per(before.maps, after.maps, 1.0)
    );
    println!("process: rss {} kB, anon {} kB, file {} kB, fds {}, maps {}", after.rss, after.anon, after.file, after.fds, after.maps);
    Ok(())
}

fn main() -> tantivy::Result<()> {
    let args: Vec<String> = env::args().collect();
    let root = Path::new(&args[2]);
    match args[1].as_str() {
        "build" => build(root, args[3].parse().unwrap(), args[4].parse().unwrap()),
        "open" => open(root, args[3].parse().unwrap()),
        _ => panic!("usage: many build ROOT COUNT DOCS < corpus | many open ROOT COUNT"),
    }
}

import java.io.BufferedReader;
import java.io.IOException;
import java.io.InputStreamReader;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.concurrent.atomic.AtomicReference;

import org.apache.lucene.analysis.CharArraySet;
import org.apache.lucene.analysis.standard.StandardAnalyzer;
import org.apache.lucene.document.Document;
import org.apache.lucene.document.Field;
import org.apache.lucene.document.NumericDocValuesField;
import org.apache.lucene.document.StringField;
import org.apache.lucene.document.TextField;
import org.apache.lucene.index.DirectoryReader;
import org.apache.lucene.index.IndexWriter;
import org.apache.lucene.index.IndexWriterConfig;
import org.apache.lucene.index.IndexWriterConfig.OpenMode;
import org.apache.lucene.index.SerialMergeScheduler;
import org.apache.lucene.index.Term;
import org.apache.lucene.search.IndexSearcher;
import org.apache.lucene.store.Directory;
import org.apache.lucene.store.FSDirectory;

import com.eclipsesource.json.Json;
import com.eclipsesource.json.JsonObject;

// Many small indexes (strata's docs/perf/small-indexes.md, M1).
//   Many build ROOT COUNT DOCS [THREADS] < corpus
//                                         COUNT keyed indexes of DOCS documents each (the
//                                         corpus's first lines, cycled), one writer thread
//                                         each, THREADS at once (default: one a core);
//                                         LUCENE_RAM_MB each (Lucene's 16 MB by default)
//   Many open ROOT COUNT                  opens them all, a reader each, and reports each
//                                         open's time and, per index, the resident bytes,
//                                         descriptors, and mappings it adds (/proc/self)
//   Many open-writers ROOT COUNT          the same with a writer each, its reader from the
//                                         writer: an index open to write, as strata's
public class Many {
	public static void main(String[] args) throws Exception {
		final Path root = Paths.get(args[1]);
		if (args[0].equals("build")) {
			build(root, Integer.parseInt(args[2]), Integer.parseInt(args[3]), args.length > 4 ? Integer.parseInt(args[4]) : 0);
		} else {
			open(root, Integer.parseInt(args[2]), args[0].equals("open-writers"));
		}
	}

	static void build(Path root, int count, int docs, int builders) throws Exception {
		final int want = Math.max(docs, (int) Math.min((long) count * docs, 200_000L));
		final List<String> lines = new ArrayList<>();
		try (BufferedReader in = new BufferedReader(new InputStreamReader(System.in))) {
			for (String line; lines.size() < want && (line = in.readLine()) != null;) {
				if (!line.isBlank()) { lines.add(line); }
			}
		}
		final int threads = builders > 0 ? builders : Runtime.getRuntime().availableProcessors();
		final double ramMb = System.getenv("LUCENE_RAM_MB") != null ? Double.parseDouble(System.getenv("LUCENE_RAM_MB")) : IndexWriterConfig.DEFAULT_RAM_BUFFER_SIZE_MB;
		final AtomicReference<Exception> error = new AtomicReference<>();
		final Thread[] pool = new Thread[threads];
		for (int k = 0; k < threads; ++k) {
			final int first = k;
			pool[k] = new Thread(() -> {
				try {
					final Document document = new Document();
					final StringField idField = new StringField("id", "", Field.Store.YES);
					final TextField textField = new TextField("text", "", Field.Store.NO);
					final NumericDocValuesField sortField = new NumericDocValuesField("sort_field", 0);
					document.add(idField);
					document.add(textField);
					document.add(sortField);
					for (int i = first; i < count; i += threads) {
						final Path dir = root.resolve(Integer.toString(i));
						Files.createDirectories(dir);
						final IndexWriterConfig config = new IndexWriterConfig(new StandardAnalyzer(CharArraySet.EMPTY_SET))
								.setOpenMode(OpenMode.CREATE).setMergeScheduler(new SerialMergeScheduler())
								.setRAMBufferSizeMB(ramMb).setRAMPerThreadHardLimitMB((int) Math.min(Math.max(ramMb, 1), 2047));
						try (Directory d = FSDirectory.open(dir); IndexWriter w = new IndexWriter(d, config)) {
							for (int n = 0; n < docs; ++n) {
								final JsonObject doc = Json.parse(lines.get((int) (((long) i * docs + n) % lines.size()))).asObject();
								final String id = doc.get("id").asString();
								idField.setStringValue(id);
								textField.setStringValue(doc.get("text").asString());
								sortField.setLongValue(doc.getLong("sort_field", 0L));
								w.updateDocument(new Term("id", id), document);
							}
							w.forceMerge(1, true);
						}
					}
				} catch (Exception e) {
					error.compareAndSet(null, e);
				}
			});
			pool[k].start();
		}
		for (Thread t : pool) { t.join(); }
		if (error.get() != null) { throw error.get(); }
		System.err.println("Many build: " + count + " indexes of " + docs + " documents");
	}

	static long[] sample() throws IOException {
		final long[] h = {-1, -1, -1, -1, -1};  // rss, anon, file (kB), fds, maps
		for (String line : Files.readAllLines(Paths.get("/proc/self/status"))) {
			final String[] parts = line.trim().split("\\s+");
			if (line.startsWith("VmRSS:")) { h[0] = Long.parseLong(parts[1]); }
			if (line.startsWith("RssAnon:")) { h[1] = Long.parseLong(parts[1]); }
			if (line.startsWith("RssFile:")) { h[2] = Long.parseLong(parts[1]); }
		}
		try (var s = Files.list(Paths.get("/proc/self/fd"))) { h[3] = s.count(); }
		h[4] = Files.readAllLines(Paths.get("/proc/self/maps")).size();
		return h;
	}

	static void open(Path root, int count, boolean writers) throws Exception {
		System.gc();
		final long[] before = sample();
		final long heapBefore = Runtime.getRuntime().totalMemory() - Runtime.getRuntime().freeMemory();
		final List<DirectoryReader> open = new ArrayList<>(count);
		final List<IndexSearcher> searchers = new ArrayList<>(count);
		final List<IndexWriter> held = new ArrayList<>(writers ? count : 0);
		final double[] us = new double[count];
		final long start = System.nanoTime();
		for (int i = 0; i < count; ++i) {
			final long t0 = System.nanoTime();
			final Directory dir = FSDirectory.open(root.resolve(Integer.toString(i)));
			DirectoryReader reader;
			if (writers) {
				final IndexWriter w = new IndexWriter(dir, new IndexWriterConfig(new StandardAnalyzer(CharArraySet.EMPTY_SET))
						.setOpenMode(OpenMode.APPEND).setMergeScheduler(new SerialMergeScheduler()));
				held.add(w);
				reader = DirectoryReader.open(w);
			} else {
				reader = DirectoryReader.open(dir);
			}
			open.add(reader);
			searchers.add(new IndexSearcher(reader));
			us[i] = (System.nanoTime() - t0) / 1e3;
		}
		final double total = (System.nanoTime() - start) / 1e9;
		System.gc();
		final long heapAfter = Runtime.getRuntime().totalMemory() - Runtime.getRuntime().freeMemory();
		final long[] after = sample();
		Arrays.sort(us);
		double mean = 0;
		for (double v : us) { mean += v; }
		mean /= count;
		long segments = 0;
		for (DirectoryReader r : open) { segments += r.leaves().size(); }
		System.out.printf("open-many: %d indexes, %d segments, %.3f s%n", count, segments, total);
		System.out.printf("open us: mean %.1f p50 %.1f p99 %.1f max %.1f%n", mean, us[count / 2], us[Math.min(count - 1, (int) (0.99 * count))], us[count - 1]);
		System.out.printf("per index: rss %.0f B, anon %.0f B, file %.0f B, fds %.2f, maps %.2f, java heap %.0f B%n", (after[0] - before[0]) * 1024.0 / count,
				(after[1] - before[1]) * 1024.0 / count, (after[2] - before[2]) * 1024.0 / count, (double) (after[3] - before[3]) / count,
				(double) (after[4] - before[4]) / count, (double) (heapAfter - heapBefore) / count);
		System.out.printf("process: rss %d kB, anon %d kB, file %d kB, fds %d, maps %d%n", after[0], after[1], after[2], after[3], after[4]);
	}
}

// SPDX-License-Identifier: GPL-3.0-only
//
// ROM-1's converter in the browser: .sf2 files to a bank (region A for the firmware, region B for the data
// area), with tools/conv/conv.c compiled to WebAssembly (web/conv/build.sh). The same code as the command
// line's rom1conv, so the same files give the same bank.
//
// A font is never read whole (some are a gigabyte): its chunks are found by walking the file, its preset
// tables read, then only the sample data the plan uses, a sample at a time.
//
//   const cv = await rom1Converter(wasmBytes);
//   const f = await cv.addFont(source);   source: a File (or Blob with a name), or {name, size, read(off, len)}
//   cv.presets(f)                         [{name, bank, prog, bytes, on}]   (bytes: 16-bit, at its own rate)
//   cv.select(f, p, on)
//   cv.plan(roomA, roomB)                 {ok, over, planned, levels, bytes, error}  (levels, bytes: the samples
//                                         and their bytes at each quality, LEVELS)
//   await cv.build(name, progress)        {a, b, id, report}  (Uint8Arrays; report: the converter's JSON)
//   cv.reset()

// the quality levels the converter steps down through to fit (tools/conv/conv.c LVL_CAP)
export const LEVELS = ["16-bit", "4-bit", "4-bit, 32 kHz", "4-bit, 22 kHz", "4-bit, 16 kHz", "4-bit, 11 kHz", "4-bit, 8 kHz"];

export async function rom1Converter(wasm) {
  let mem = null;
  const env = { emscripten_notify_memory_growth: () => { mem = null; } };
  const any = new Proxy({}, { get: () => () => 0 });
  const { instance } = await WebAssembly.instantiate(wasm, { env, wasi_snapshot_preview1: any });
  const x = instance.exports;
  x._initialize?.();
  const heap = () => (mem && mem.buffer === x.memory.buffer ? mem : (mem = new Uint8Array(x.memory.buffer)));
  const put = (bytes) => { const p = x.web_alloc(bytes.length); heap().set(bytes, p); return p; };
  const cstr = (s) => put(new TextEncoder().encode(s + "\0"));
  const str = (p) => { const h = heap(); let e = p; while (h[e]) e++; return new TextDecoder().decode(h.subarray(p, e)); };
  const u32 = (p, i) => new DataView(x.memory.buffer).getUint32(p + 4 * i, true);
  let fonts = [];

  const reader = (src) => (src.read ? src : {
    name: src.name || "font", size: src.size,
    read: async (off, len) => new Uint8Array(await src.slice(off, off + len).arrayBuffer()),
  });

  // where the preset tables (the pdta list) and the sample data (smpl) are
  async function walk(r) {
    const head = await r.read(0, 12), id = (b, o) => String.fromCharCode(b[o], b[o + 1], b[o + 2], b[o + 3]);
    const le = (b, o) => (b[o] | b[o + 1] << 8 | b[o + 2] << 16 | b[o + 3] << 24) >>> 0;
    if (head.length < 12 || id(head, 0) !== "RIFF" || id(head, 8) !== "sfbk") throw new Error("not an .sf2 file");
    const out = { pdta: 0, pdtaN: 0, smpl: 0, smplN: 0 };
    for (let o = 12; o + 8 <= r.size;) {
      const h = await r.read(o, 12);
      let sz = le(h, 4);
      if (sz > r.size - o - 8) sz = r.size - o - 8;
      if (id(h, 0) === "LIST" && sz >= 4) {
        if (id(h, 8) === "pdta") { out.pdta = o + 12; out.pdtaN = sz - 4; }
        else if (id(h, 8) === "sdta") {
          for (let q = o + 12, e = o + 8 + sz; q + 8 <= e;) {
            const g = await r.read(q, 8);
            let s2 = le(g, 4);
            if (s2 > e - q - 8) s2 = e - q - 8;
            if (id(g, 0) === "smpl") { out.smpl = q + 8; out.smplN = s2; }
            q += 8 + s2 + (s2 & 1);
          }
        }
      }
      o += 8 + sz + (sz & 1);
    }
    if (!out.pdta) throw new Error("not an .sf2 file: no preset tables");
    return out;
  }

  return {
    async addFont(source) {
      const r = reader(source), w = await walk(r);
      const pd = await r.read(w.pdta, w.pdtaN);
      const p = put(pd), n = cstr(r.name.replace(/\.sf2$/i, ""));
      const f = x.web_font_add(p, pd.length, w.smplN, n);
      x.web_free(p);
      x.web_free(n);
      if (f < 0) throw new Error(str(x.web_error()));
      fonts[f] = { r, smpl: w.smpl };
      return f;
    },
    fonts: () => x.web_fonts(),
    fontName: (f) => str(x.web_font_name(f)),
    presets(f) {
      const out = [];
      for (let p = 0; p < x.web_presets(f); p++)
        out.push({ name: str(x.web_preset_name(f, p)), bank: x.web_preset_bank(f, p), prog: x.web_preset_prog(f, p),
                   bytes: x.web_preset_bytes(f, p), on: !!x.web_selected(f, p) });
      return out;
    },
    select: (f, p, on) => x.web_select(f, p, on ? 1 : 0),
    plan(roomA, roomB) {
      const r = x.web_plan(roomA >>> 0, roomB >>> 0), levels = [], bytes = [];
      for (let l = 0; l < LEVELS.length; l++) { levels.push(x.web_plan_level(l)); bytes.push(x.web_plan_level_bytes()); }
      return { ok: r === 0, over: x.web_over(), planned: x.web_planned(), levels, bytes, error: r ? str(x.web_error()) : "" };
    },
    async build(name, progress = () => {}) {
      const n = x.web_needs();
      for (let i = 0; i < n; i++) {
        const q = x.web_need(i), f = u32(q, 0), off = u32(q, 1), bytes = u32(q, 2);
        const data = await fonts[f].r.read(fonts[f].smpl + off, bytes);
        const p = put(data), rc = x.web_give(i, p, data.length);
        x.web_free(p);
        if (rc) throw new Error(str(x.web_error()));
        progress((i + 1) / n);
      }
      const nm = cstr(name), id = x.web_bank_id(nm), rc = x.web_build(id, nm);
      x.web_free(nm);
      if (rc) throw new Error(str(x.web_error()));
      const pa = x.web_out_a(), pb = x.web_out_b();
      const a = heap().slice(pa, pa + x.web_out_len(0)), b = pb ? heap().slice(pb, pb + x.web_out_len(1)) : new Uint8Array(0);
      return { a, b, id, report: JSON.parse(str(x.web_report())) };
    },
    reset() { x.web_reset(); fonts = []; },
  };
}

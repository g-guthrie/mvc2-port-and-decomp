import fs from 'node:fs';
const [wasmPath, iso, nframes] = process.argv.slice(2);
const fd = fs.openSync(iso, 'r'), size = fs.fstatSync(fd).size;
let mem;
const u8 = () => new Uint8Array(mem.buffer), dv = () => new DataView(mem.buffer);
const wasi = {
  fd_write(f, iov, n, pn) { let t = 0; for (let i = 0; i < n; i++) { const p = dv().getUint32(iov + 8 * i, true), l = dv().getUint32(iov + 8 * i + 4, true); process.stderr.write(u8().subarray(p, p + l)); t += l; } dv().setUint32(pn, t, true); return 0; },
  fd_read() { return 8; }, fd_close() { return 0; }, fd_seek() { return 8; },
  clock_time_get(id, prec, out) { dv().setBigUint64(out, 0n, true); return 0; },
  environ_sizes_get(a, b) { dv().setUint32(a, 0, true); dv().setUint32(b, 0, true); return 0; },
  environ_get() { return 0; }, proc_exit(c) { process.exit(c); },
};
const env = {
  emscripten_asm_const_int() { return 0; },
  page_disc_read(off, n, dst) { return fs.readSync(fd, u8(), dst, n, off); },
  __syscall_getdents64() { return -1; }, __syscall_unlinkat() { return -1; }, __syscall_rmdir() { return -1; },
};
const { instance } = await WebAssembly.instantiate(fs.readFileSync(wasmPath), { env, wasi_snapshot_preview1: wasi });
const x = instance.exports; mem = x.memory;
x._initialize();
if (x.mvc2g_boot(size)) { console.error('boot failed'); process.exit(1); }
for (let i = 0; i < +nframes; i++) { x.mvc2g_frame(); console.log(i, (x.mvc2g_state_hash() >>> 0).toString(16).padStart(8, '0'), x.mvc2g_game_state().toString(16).padStart(6, '0')); }

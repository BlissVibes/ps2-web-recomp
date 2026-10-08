struct Params {
    v: array<vec4<u32>, 16>,
}

@group(0) @binding(0) var<storage, read_write> vram: array<atomic<u32>>;
@group(0) @binding(1) var<storage, read> tables: array<u32>;
@group(0) @binding(2) var<uniform> params: Params;
@group(0) @binding(3) var<storage, read> data: array<u32>;
@group(0) @binding(4) var<storage, read_write> clut: array<u32>;
@group(0) @binding(5) var<storage, read_write> scratch: array<u32>;

const PSMCT32: u32 = 0x00u;
const PSMCT24: u32 = 0x01u;
const PSMCT16: u32 = 0x02u;
const PSMCT16S: u32 = 0x0Au;
const PSMT8: u32 = 0x13u;
const PSMT4: u32 = 0x14u;
const PSMT8H: u32 = 0x1Bu;
const PSMT4HL: u32 = 0x24u;
const PSMT4HH: u32 = 0x2Cu;
const PSMZ32: u32 = 0x30u;
const PSMZ24: u32 = 0x31u;
const PSMZ16: u32 = 0x32u;
const PSMZ16S: u32 = 0x3Au;

const T_OFF32: u32 = 0u;
const T_OFFZ32: u32 = 2048u;
const T_OFF16: u32 = 4096u;
const T_OFF16S: u32 = 8192u;
const T_OFFZ16: u32 = 12288u;
const T_OFFZ16S: u32 = 16384u;
const T_OFF8: u32 = 20480u;
const T_OFF4: u32 = 28672u;

const CLUT_ENTRIES: u32 = 512u;

fn prm(i: u32) -> u32 {
    return params.v[i / 4u][i % 4u];
}

struct Loc {
    word: u32,
    shift: u32,
    mask: u32,
}

fn page_addr(bp: u32, bw: u32, x: u32, y: u32, pw: u32, ph: u32) -> u32 {
    let per_row = max(1u, bw * 64u / pw);
    return bp * 256u + ((x / pw) + (y / ph) * per_row) * 8192u;
}

fn locate(psm: u32, bp: u32, bw: u32, x: u32, y: u32) -> Loc {
    var a = 0u;
    var shift = 0u;
    var mask = 0xFFFFFFFFu;
    switch psm {
        case 0x30u {
            a = page_addr(bp, bw, x, y, 64u, 32u) + tables[T_OFFZ32 + (y & 31u) * 64u + (x & 63u)];
        }
        case 0x31u {
            a = page_addr(bp, bw, x, y, 64u, 32u) + tables[T_OFFZ32 + (y & 31u) * 64u + (x & 63u)];
            mask = 0xFFFFFFu;
        }
        case 0x01u {
            a = page_addr(bp, bw, x, y, 64u, 32u) + tables[T_OFF32 + (y & 31u) * 64u + (x & 63u)];
            mask = 0xFFFFFFu;
        }
        case 0x02u {
            a = page_addr(bp, bw, x, y, 64u, 64u) + tables[T_OFF16 + (y & 63u) * 64u + (x & 63u)];
            shift = (a & 2u) * 8u;
            mask = 0xFFFFu;
        }
        case 0x0Au {
            a = page_addr(bp, bw, x, y, 64u, 64u) + tables[T_OFF16S + (y & 63u) * 64u + (x & 63u)];
            shift = (a & 2u) * 8u;
            mask = 0xFFFFu;
        }
        case 0x32u {
            a = page_addr(bp, bw, x, y, 64u, 64u) + tables[T_OFFZ16 + (y & 63u) * 64u + (x & 63u)];
            shift = (a & 2u) * 8u;
            mask = 0xFFFFu;
        }
        case 0x3Au {
            a = page_addr(bp, bw, x, y, 64u, 64u) + tables[T_OFFZ16S + (y & 63u) * 64u + (x & 63u)];
            shift = (a & 2u) * 8u;
            mask = 0xFFFFu;
        }
        case 0x13u {
            a = page_addr(bp, bw, x, y, 128u, 64u) + tables[T_OFF8 + (y & 63u) * 128u + (x & 127u)];
            shift = (a & 3u) * 8u;
            mask = 0xFFu;
        }
        case 0x14u {
            let n = tables[T_OFF4 + (y & 127u) * 128u + (x & 127u)];
            a = page_addr(bp, bw, x, y, 128u, 128u) + n / 2u;
            shift = (a & 3u) * 8u + (n & 1u) * 4u;
            mask = 0xFu;
        }
        case 0x1Bu {
            a = page_addr(bp, bw, x, y, 64u, 32u) + tables[T_OFF32 + (y & 31u) * 64u + (x & 63u)];
            shift = 24u;
            mask = 0xFFu;
        }
        case 0x24u {
            a = page_addr(bp, bw, x, y, 64u, 32u) + tables[T_OFF32 + (y & 31u) * 64u + (x & 63u)];
            shift = 24u;
            mask = 0xFu;
        }
        case 0x2Cu {
            a = page_addr(bp, bw, x, y, 64u, 32u) + tables[T_OFF32 + (y & 31u) * 64u + (x & 63u)];
            shift = 28u;
            mask = 0xFu;
        }
        default {
            a = page_addr(bp, bw, x, y, 64u, 32u) + tables[T_OFF32 + (y & 31u) * 64u + (x & 63u)];
        }
    }
    return Loc((a & 0x3FFFFFu) >> 2u, shift, mask);
}

fn read_pixel(psm: u32, bp: u32, bw: u32, x: u32, y: u32) -> u32 {
    let l = locate(psm, bp, bw, x, y);
    return (atomicLoad(&vram[l.word]) >> l.shift) & l.mask;
}

fn write_pixel(psm: u32, bp: u32, bw: u32, x: u32, y: u32, value: u32) {
    let l = locate(psm, bp, bw, x, y);
    if (l.mask == 0xFFFFFFFFu) {
        atomicStore(&vram[l.word], value);
        return;
    }
    atomicAnd(&vram[l.word], ~(l.mask << l.shift));
    atomicOr(&vram[l.word], (value & l.mask) << l.shift);
}

fn bits_at(offset: u32, bit: u32, count: u32) -> u32 {
    let w = offset + bit / 32u;
    let s = bit % 32u;
    var v = data[w] >> s;
    if (s + count > 32u) {
        v = v | (data[w + 1u] << (32u - s));
    }
    if (count == 32u) {
        return v;
    }
    return v & ((1u << count) - 1u);
}

@compute @workgroup_size(64)
fn upload(@builtin(global_invocation_id) gid: vec3<u32>) {
    let i = gid.x + gid.y * 64u * 1024u;
    if (i >= prm(2u)) {
        return;
    }
    let records = prm(0u);
    var lo = 0u;
    var hi = prm(1u);
    while (hi - lo > 1u) {
        let mid = (lo + hi) / 2u;
        if (data[records + mid * 12u + 11u] <= i) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    let r = records + lo * 12u;
    let k = i - data[r + 11u];
    let rrw = data[r + 5u];
    let p = data[r + 6u] + k;
    let x = (data[r + 3u] + p % rrw) & 0x7FFu;
    let y = (data[r + 4u] + p / rrw) & 0x7FFu;
    let bpp = data[r + 10u];
    let v = bits_at(data[r + 8u], data[r + 9u] + k * bpp, bpp);
    write_pixel(data[r + 2u], data[r], data[r + 1u], x, y, v);
}

@compute @workgroup_size(64)
fn copy_read(@builtin(global_invocation_id) gid: vec3<u32>) {
    let i = gid.x + gid.y * 64u * 1024u;
    let w = prm(5u);
    if (i >= w * prm(6u)) {
        return;
    }
    let x = (prm(3u) + i % w) & 0x7FFu;
    let y = (prm(4u) + i / w) & 0x7FFu;
    scratch[i] = read_pixel(prm(2u), prm(0u), prm(1u), x, y);
}

@compute @workgroup_size(64)
fn copy_write(@builtin(global_invocation_id) gid: vec3<u32>) {
    let i = gid.x + gid.y * 64u * 1024u;
    let w = prm(5u);
    let h = prm(6u);
    if (i >= w * h) {
        return;
    }
    let dir = prm(7u);
    let x = i % w;
    let y = i / w;
    var sx = x;
    var sy = y;
    if ((dir & 1u) != 0u) {
        sx = w - 1u - x;
    }
    if ((dir & 2u) != 0u) {
        sy = h - 1u - y;
    }
    write_pixel(prm(2u), prm(0u), prm(1u), (prm(3u) + x) & 0x7FFu, (prm(4u) + y) & 0x7FFu, scratch[sy * w + sx]);
}

@compute @workgroup_size(256)
fn load_clut(@builtin(local_invocation_index) k: u32) {
    let count = prm(1u);
    for (var n = 0u; n < count; n = n + 1u) {
        let r = prm(0u) + n * 12u;
        let src = data[r] * CLUT_ENTRIES;
        let dst = data[r + 1u] * CLUT_ENTRIES;
        clut[dst + k] = clut[src + k];
        clut[dst + k + 256u] = clut[src + k + 256u];
        storageBarrier();
        let cbp = data[r + 2u];
        let cpsm = data[r + 3u];
        let csm = data[r + 4u];
        let csa = data[r + 5u];
        let eight = data[r + 6u] != 0u;
        var entries = 16u;
        if (eight) {
            entries = 256u;
        }
        if (k < entries) {
            if (csm != 0u) {
                clut[dst + ((csa * 16u + k) & 511u)] = read_pixel(cpsm, cbp, data[r + 7u], data[r + 8u] * 16u + k, data[r + 9u]);
            } else {
                var x = k & 7u;
                var y = k >> 3u;
                var slot = ((csa & 15u) * 16u + k) & 255u;
                if (eight) {
                    let j = (k & ~0x18u) | ((k & 0x08u) << 1u) | ((k & 0x10u) >> 1u);
                    x = j & 15u;
                    y = j >> 4u;
                    slot = k;
                }
                clut[dst + slot] = read_pixel(cpsm, cbp, 1u, x, y);
            }
        }
        storageBarrier();
    }
}

@compute @workgroup_size(8, 8)
fn display(@builtin(global_invocation_id) gid: vec3<u32>) {
    let w = prm(5u);
    let h = prm(6u);
    if (gid.x >= w || gid.y >= h) {
        return;
    }
    let psm = prm(2u);
    var sy = gid.y;
    if (prm(7u) != 0u) {
        sy = gid.y / 2u;
    }
    var c = read_pixel(psm, prm(0u), prm(1u), prm(3u) + gid.x, prm(4u) + sy);
    if (psm == PSMCT16 || psm == PSMCT16S) {
        c = ((c & 0x1Fu) << 3u) | ((c & 0x3E0u) << 6u) | ((c & 0x7C00u) << 9u);
    }
    scratch[prm(8u) + gid.y * w + gid.x] = c | 0xFF000000u;
}

struct Setup {
    flags: u32,
    fbp: u32,
    fbw: u32,
    fpsm: u32,
    fbmsk: u32,
    zbp: u32,
    zpsm: u32,
    modes: u32,
    aref: u32,
    fix: u32,
    scax0: i32,
    scax1: i32,
    scay0: i32,
    scay1: i32,
    tbp: u32,
    tbw: u32,
    tpsm: u32,
    tw: u32,
    th: u32,
    cpsm: u32,
    csa: u32,
    minu: u32,
    maxu: u32,
    minv: u32,
    maxv: u32,
    ta0: u32,
    ta1: u32,
    fogcol: u32,
    clut: u32,
    zmax: u32,
}

const F_IIP: u32 = 1u;
const F_TME: u32 = 2u;
const F_FGE: u32 = 4u;
const F_ABE: u32 = 8u;
const F_FST: u32 = 16u;
const F_ZMSK: u32 = 32u;
const F_ATE: u32 = 64u;
const F_DATE: u32 = 128u;
const F_DATM: u32 = 256u;
const F_ZTE: u32 = 512u;
const F_PABE: u32 = 1024u;
const F_COLCLAMP: u32 = 2048u;
const F_FBA: u32 = 4096u;
const F_TCC: u32 = 8192u;
const F_BILINEAR: u32 = 16384u;
const F_AEM: u32 = 32768u;

fn load_setup(at: u32) -> Setup {
    var s: Setup;
    s.flags = data[at];
    s.fbp = data[at + 1u];
    s.fbw = data[at + 2u];
    s.fpsm = data[at + 3u];
    s.fbmsk = data[at + 4u];
    s.zbp = data[at + 5u];
    s.zpsm = data[at + 6u];
    s.modes = data[at + 7u];
    s.aref = data[at + 8u];
    s.fix = data[at + 9u];
    s.scax0 = i32(data[at + 10u] & 0xFFFFu);
    s.scax1 = i32(data[at + 10u] >> 16u);
    s.scay0 = i32(data[at + 11u] & 0xFFFFu);
    s.scay1 = i32(data[at + 11u] >> 16u);
    s.tbp = data[at + 12u];
    s.tbw = data[at + 13u];
    s.tpsm = data[at + 14u];
    s.tw = data[at + 15u] & 0xFFu;
    s.th = data[at + 15u] >> 8u;
    s.cpsm = data[at + 16u];
    s.csa = data[at + 17u];
    s.minu = data[at + 18u] & 0xFFFFu;
    s.maxu = data[at + 18u] >> 16u;
    s.minv = data[at + 19u] & 0xFFFFu;
    s.maxv = data[at + 19u] >> 16u;
    s.ta0 = data[at + 20u] & 0xFFu;
    s.ta1 = (data[at + 20u] >> 8u) & 0xFFu;
    s.fogcol = data[at + 21u];
    s.clut = data[at + 22u];
    s.zmax = data[at + 23u];
    return s;
}

fn has(s: Setup, f: u32) -> bool {
    return (s.flags & f) != 0u;
}

fn mode(s: Setup, shift: u32, bits: u32) -> u32 {
    return (s.modes >> shift) & ((1u << bits) - 1u);
}

fn clamp255(v: i32) -> i32 {
    return clamp(v, 0, 255);
}

fn expand16(c: u32, s: Setup) -> u32 {
    let r = (c & 0x1Fu) << 3u;
    let g = ((c >> 5u) & 0x1Fu) << 3u;
    let b = ((c >> 10u) & 0x1Fu) << 3u;
    var a = s.ta0;
    if ((c & 0x8000u) != 0u) {
        a = s.ta1;
    } else if (has(s, F_AEM) && (c & 0x7FFFu) == 0u) {
        a = 0u;
    }
    return r | (g << 8u) | (b << 16u) | (a << 24u);
}

fn wrap_coord(v: i32, size: i32, m: u32, mn: u32, mx: u32) -> i32 {
    switch m {
        case 0u {
            return v & (size - 1);
        }
        case 1u {
            return clamp(v, 0, size - 1);
        }
        case 2u {
            return clamp(v, i32(mn), i32(mx));
        }
        default {
            return i32((u32(v) & mn) | mx);
        }
    }
}

fn fetch(s: Setup, xi: i32, yi: i32) -> u32 {
    let x = wrap_coord(xi, 1 << s.tw, mode(s, 24u, 2u), s.minu, s.maxu);
    let y = wrap_coord(yi, 1 << s.th, mode(s, 26u, 2u), s.minv, s.maxv);
    let raw = read_pixel(s.tpsm, s.tbp, s.tbw, u32(x), u32(y));
    switch s.tpsm {
        case 0x00u, 0x30u {
            return raw;
        }
        case 0x01u, 0x31u {
            var a = s.ta0;
            if (has(s, F_AEM) && (raw & 0xFFFFFFu) == 0u) {
                a = 0u;
            }
            return (raw & 0xFFFFFFu) | (a << 24u);
        }
        case 0x02u, 0x0Au, 0x32u, 0x3Au {
            return expand16(raw, s);
        }
        case 0x13u, 0x1Bu {
            let e = clut[s.clut * CLUT_ENTRIES + (raw & 255u)];
            if (s.cpsm == PSMCT32 || s.cpsm == PSMCT24) {
                return e;
            }
            return expand16(e, s);
        }
        case 0x14u, 0x24u, 0x2Cu {
            let e = clut[s.clut * CLUT_ENTRIES + (((s.csa & 15u) * 16u + (raw & 15u)) & 511u)];
            if (s.cpsm == PSMCT32 || s.cpsm == PSMCT24) {
                return e;
            }
            return expand16(e, s);
        }
        default {
            return raw;
        }
    }
}

fn sample(s: Setup, uin: f32, vin: f32) -> u32 {
    if (!has(s, F_BILINEAR)) {
        return fetch(s, i32(floor(uin)), i32(floor(vin)));
    }
    let u = uin - 0.5;
    let v = vin - 0.5;
    let x0 = i32(floor(u));
    let y0 = i32(floor(v));
    let fx = u - f32(x0);
    let fy = v - f32(y0);
    let c00 = fetch(s, x0, y0);
    let c10 = fetch(s, x0 + 1, y0);
    let c01 = fetch(s, x0, y0 + 1);
    let c11 = fetch(s, x0 + 1, y0 + 1);
    var out = 0u;
    for (var ch = 0u; ch < 32u; ch = ch + 8u) {
        let a = f32((c00 >> ch) & 0xFFu);
        let b = f32((c10 >> ch) & 0xFFu);
        let c = f32((c01 >> ch) & 0xFFu);
        let d = f32((c11 >> ch) & 0xFFu);
        let top = a + (b - a) * fx;
        let bottom = c + (d - c) * fx;
        out = out | (u32(clamp(top + (bottom - top) * fy + 0.5, 0.0, 255.0)) << ch);
    }
    return out;
}

fn read_frame(s: Setup, x: u32, y: u32) -> u32 {
    let raw = read_pixel(s.fpsm, s.fbp, s.fbw, x, y);
    switch s.fpsm {
        case 0x01u {
            return raw | 0x80000000u;
        }
        case 0x02u, 0x0Au {
            var a = 0u;
            if ((raw & 0x8000u) != 0u) {
                a = 0x80000000u;
            }
            return ((raw & 0x1Fu) << 3u) | (((raw >> 5u) & 0x1Fu) << 11u) | (((raw >> 10u) & 0x1Fu) << 19u) | a;
        }
        default {
            return raw;
        }
    }
}

fn pack16(v: u32) -> u32 {
    var a = 0u;
    if ((v & 0x80000000u) != 0u) {
        a = 0x8000u;
    }
    return ((v >> 3u) & 0x1Fu) | (((v >> 11u) & 0x1Fu) << 5u) | (((v >> 19u) & 0x1Fu) << 10u) | a;
}

fn write_frame(s: Setup, x: u32, y: u32, c: u32, mask: u32) {
    switch s.fpsm {
        case 0x02u, 0x0Au {
            let old = read_pixel(s.fpsm, s.fbp, s.fbw, x, y);
            var m16 = pack16(mask);
            if ((mask & 0x80000000u) != 0u) {
                m16 = m16 | 0x8000u;
            }
            write_pixel(s.fpsm, s.fbp, s.fbw, x, y, (pack16(c) & ~m16) | (old & m16));
        }
        case 0x01u {
            let old = read_pixel(PSMCT32, s.fbp, s.fbw, x, y);
            let m = mask | 0xFF000000u;
            write_pixel(PSMCT32, s.fbp, s.fbw, x, y, (c & ~m) | (old & m));
        }
        default {
            var v = c;
            if (mask != 0u) {
                let old = read_pixel(s.fpsm, s.fbp, s.fbw, x, y);
                v = (c & ~mask) | (old & mask);
            }
            write_pixel(s.fpsm, s.fbp, s.fbw, x, y, v);
        }
    }
}

struct Attr {
    r: f32,
    g: f32,
    b: f32,
    a: f32,
    s: f32,
    t: f32,
    q: f32,
    f: f32,
    z: u32,
}

fn plot(s: Setup, x: i32, y: i32, at: Attr) {
    if (x < s.scax0 || x > s.scax1 || y < s.scay0 || y > s.scay1) {
        return;
    }
    let scanmsk = mode(s, 28u, 2u);
    if (scanmsk == 2u && (y & 1) == 0) {
        return;
    }
    if (scanmsk == 3u && (y & 1) != 0) {
        return;
    }
    let ux = u32(x);
    let uy = u32(y);
    let z = min(at.z, s.zmax);

    var r = i32(at.r);
    var g = i32(at.g);
    var b = i32(at.b);
    var a = i32(at.a);
    if (has(s, F_TME)) {
        var u: f32;
        var v: f32;
        if (has(s, F_FST)) {
            u = at.s / 16.0;
            v = at.t / 16.0;
        } else {
            var q = at.q;
            if (q == 0.0) {
                q = 1.0;
            }
            u = at.s / q * f32(1u << s.tw);
            v = at.t / q * f32(1u << s.th);
        }
        let t = sample(s, u, v);
        let tr = i32(t & 0xFFu);
        let tg = i32((t >> 8u) & 0xFFu);
        let tb = i32((t >> 16u) & 0xFFu);
        let ta = i32(t >> 24u);
        let tcc = has(s, F_TCC);
        switch mode(s, 20u, 2u) {
            case 0u {
                r = clamp255((tr * r) >> 7u);
                g = clamp255((tg * g) >> 7u);
                b = clamp255((tb * b) >> 7u);
                if (tcc) {
                    a = clamp255((ta * a) >> 7u);
                }
            }
            case 1u {
                r = tr;
                g = tg;
                b = tb;
                if (tcc) {
                    a = ta;
                }
            }
            case 2u {
                r = clamp255(((tr * r) >> 7u) + a);
                g = clamp255(((tg * g) >> 7u) + a);
                b = clamp255(((tb * b) >> 7u) + a);
                if (tcc) {
                    a = clamp255(ta + a);
                }
            }
            default {
                r = clamp255(((tr * r) >> 7u) + a);
                g = clamp255(((tg * g) >> 7u) + a);
                b = clamp255(((tb * b) >> 7u) + a);
                if (tcc) {
                    a = ta;
                }
            }
        }
    }
    if (has(s, F_FGE)) {
        let f = i32(at.f);
        let fr = i32(s.fogcol & 0xFFu);
        let fg = i32((s.fogcol >> 8u) & 0xFFu);
        let fb = i32((s.fogcol >> 16u) & 0xFFu);
        r = (f * r + (255 - f) * fr) >> 8u;
        g = (f * g + (255 - f) * fg) >> 8u;
        b = (f * b + (255 - f) * fb) >> 8u;
    }

    var write_fb = true;
    var write_z = !has(s, F_ZMSK);
    var write_alpha = true;
    if (has(s, F_ATE)) {
        let av = u32(a);
        var ok = false;
        switch mode(s, 0u, 3u) {
            case 0u {
                ok = false;
            }
            case 1u {
                ok = true;
            }
            case 2u {
                ok = av < s.aref;
            }
            case 3u {
                ok = av <= s.aref;
            }
            case 4u {
                ok = av == s.aref;
            }
            case 5u {
                ok = av >= s.aref;
            }
            case 6u {
                ok = av > s.aref;
            }
            default {
                ok = av != s.aref;
            }
        }
        if (!ok) {
            switch mode(s, 4u, 2u) {
                case 0u {
                    return;
                }
                case 1u {
                    write_z = false;
                }
                case 2u {
                    write_fb = false;
                }
                default {
                    write_z = false;
                    write_alpha = false;
                }
            }
        }
    }

    var dst = 0u;
    var have_dst = false;
    if (has(s, F_DATE)) {
        dst = read_frame(s, ux, uy);
        have_dst = true;
        if (((dst & 0x80000000u) != 0u) != has(s, F_DATM)) {
            return;
        }
    }

    let zte = has(s, F_ZTE);
    if (zte) {
        let old = read_pixel(s.zpsm, s.zbp, s.fbw, ux, uy) & s.zmax;
        var ok = false;
        switch mode(s, 8u, 2u) {
            case 0u {
                ok = false;
            }
            case 1u {
                ok = true;
            }
            case 2u {
                ok = z >= old;
            }
            default {
                ok = z > old;
            }
        }
        if (!ok) {
            return;
        }
    }

    if (write_fb) {
        let colclamp = has(s, F_COLCLAMP);
        if (has(s, F_ABE) && (!has(s, F_PABE) || (a & 0x80) != 0)) {
            if (!have_dst) {
                dst = read_frame(s, ux, uy);
            }
            let cd = vec3<i32>(i32(dst & 0xFFu), i32((dst >> 8u) & 0xFFu), i32((dst >> 16u) & 0xFFu));
            let cs = vec3<i32>(r, g, b);
            let ad = i32(dst >> 24u);
            let ba = mode(s, 12u, 2u);
            let bb = mode(s, 14u, 2u);
            let bc = mode(s, 16u, 2u);
            let bd = mode(s, 18u, 2u);
            var cc = i32(s.fix);
            if (bc == 0u) {
                cc = a;
            } else if (bc == 1u) {
                cc = ad;
            }
            var av = vec3<i32>(0);
            var bv = vec3<i32>(0);
            var dv = vec3<i32>(0);
            if (ba == 0u) {
                av = cs;
            } else if (ba == 1u) {
                av = cd;
            }
            if (bb == 0u) {
                bv = cs;
            } else if (bb == 1u) {
                bv = cd;
            }
            if (bd == 0u) {
                dv = cs;
            } else if (bd == 1u) {
                dv = cd;
            }
            var o = (((av - bv) * cc) >> vec3<u32>(7u)) + dv;
            if (colclamp) {
                o = clamp(o, vec3<i32>(0), vec3<i32>(255));
            } else {
                o = o & vec3<i32>(0xFF);
            }
            r = o.x;
            g = o.y;
            b = o.z;
        } else if (!colclamp) {
            r = r & 0xFF;
            g = g & 0xFF;
            b = b & 0xFF;
        }
        var out_a = u32(a & 0xFF);
        if (has(s, F_FBA)) {
            out_a = out_a | 0x80u;
        }
        var mask = s.fbmsk;
        if (!write_alpha) {
            mask = mask | 0xFF000000u;
        }
        write_frame(s, ux, uy, u32(r) | (u32(g) << 8u) | (u32(b) << 16u) | (out_a << 24u), mask);
    }
    if (write_z && zte) {
        write_pixel(s.zpsm, s.zbp, s.fbw, ux, uy, z);
    }
}

const PRIM_WORDS: u32 = 40u;
const SETUP_WORDS: u32 = 24u;

fn fat(i: u32) -> f32 {
    return bitcast<f32>(data[i]);
}

fn vertex_attr(p: u32, n: u32) -> vec4<f32> {
    return vec4<f32>(fat(p + 12u + n * 8u), fat(p + 13u + n * 8u), fat(p + 14u + n * 8u), fat(p + 15u + n * 8u));
}

fn vertex_tex(p: u32, n: u32) -> vec4<f32> {
    return vec4<f32>(fat(p + 16u + n * 8u), fat(p + 17u + n * 8u), fat(p + 18u + n * 8u), fat(p + 19u + n * 8u));
}

fn z_at(p: u32, d: f32) -> u32 {
    let z0 = data[p + 36u];
    let step = floor(d);
    if (step >= 0.0) {
        return z0 + u32(min(step, f32(0xFFFFFFFFu - z0)));
    }
    return z0 - u32(min(-step, f32(z0)));
}

fn shade(p: u32, s: Setup, x: i32, y: i32) {
    let kind = data[p + 1u];
    let px = f32(x);
    let py = f32(y);
    var at: Attr;
    switch kind {
        case 0u {
            let x0 = fat(p + 4u);
            let y0 = fat(p + 5u);
            let x1 = fat(p + 6u);
            let y1 = fat(p + 7u);
            let x2 = fat(p + 8u);
            let y2 = fat(p + 9u);
            let inv = fat(p + 10u);
            let w0 = ((x1 - px) * (y2 - py) - (x2 - px) * (y1 - py)) * inv;
            let w1 = ((x2 - px) * (y0 - py) - (x0 - px) * (y2 - py)) * inv;
            let w2 = 1.0 - w0 - w1;
            if (w0 < 0.0 || w1 < 0.0 || w2 < 0.0) {
                return;
            }
            let c = vertex_attr(p, 0u) * w0 + vertex_attr(p, 1u) * w1 + vertex_attr(p, 2u) * w2;
            let t = vertex_tex(p, 0u) * w0 + vertex_tex(p, 1u) * w1 + vertex_tex(p, 2u) * w2;
            at = Attr(c.x, c.y, c.z, c.w, t.x, t.y, t.z, t.w, z_at(p, fat(p + 37u) * w1 + fat(p + 38u) * w2));
        }
        case 1u {
            let x0 = fat(p + 4u);
            let y0 = fat(p + 5u);
            let dx = fat(p + 6u);
            let dy = fat(p + 7u);
            let a = vertex_tex(p, 0u);
            let b = vertex_tex(p, 1u);
            var tx = 0.0;
            var ty = 0.0;
            if (dx != 0.0) {
                tx = (px - x0) / dx;
            }
            if (dy != 0.0) {
                ty = (py - y0) / dy;
            }
            let c = vertex_attr(p, 1u);
            at = Attr(c.x, c.y, c.z, c.w, a.x + (b.x - a.x) * tx, a.y + (b.y - a.y) * ty, b.z, b.w, data[p + 36u]);
        }
        case 2u {
            let x0 = fat(p + 4u);
            let y0 = fat(p + 5u);
            let x1 = fat(p + 6u);
            let y1 = fat(p + 7u);
            let steps = i32(data[p + 8u]);
            let count = max(steps, 1);
            var guess = 0;
            if (abs(x1 - x0) >= abs(y1 - y0)) {
                if (x1 != x0) {
                    guess = i32(round((px - x0) / (x1 - x0) * f32(steps)));
                }
            } else {
                guess = i32(round((py - y0) / (y1 - y0) * f32(steps)));
            }
            var hit = -1;
            for (var k = guess - 1; k <= guess + 1; k = k + 1) {
                if (k < 0 || k >= count || hit >= 0) {
                    continue;
                }
                var t = 0.0;
                if (steps != 0) {
                    t = f32(k) / f32(steps);
                }
                if (i32(round(x0 + (x1 - x0) * t)) == x && i32(round(y0 + (y1 - y0) * t)) == y) {
                    hit = k;
                }
            }
            if (hit < 0) {
                return;
            }
            var t = 0.0;
            if (steps != 0) {
                t = f32(hit) / f32(steps);
            }
            let c = vertex_attr(p, 0u) * (1.0 - t) + vertex_attr(p, 1u) * t;
            let tex = vertex_tex(p, 0u) * (1.0 - t) + vertex_tex(p, 1u) * t;
            at = Attr(c.x, c.y, c.z, c.w, tex.x, tex.y, tex.z, tex.w, z_at(p, fat(p + 37u) * t));
        }
        default {
            if (x != i32(data[p + 4u]) || y != i32(data[p + 5u])) {
                return;
            }
            let c = vertex_attr(p, 0u);
            let t = vertex_tex(p, 0u);
            at = Attr(c.x, c.y, c.z, c.w, t.x, t.y, t.z, t.w, data[p + 36u]);
        }
    }
    plot(s, x, y, at);
}

@compute @workgroup_size(16, 16)
fn raster(@builtin(workgroup_id) wid: vec3<u32>, @builtin(local_invocation_id) lid: vec3<u32>) {
    let tile = prm(0u) + (wid.x + wid.y * 1024u) * 4u;
    if (wid.x + wid.y * 1024u >= prm(3u)) {
        return;
    }
    let origin = data[tile];
    let x = i32((origin & 0xFFFFu) * 16u + lid.x);
    let y = i32((origin >> 16u) * 16u + lid.y);
    let list = data[tile + 1u];
    let count = data[tile + 2u];
    let prims = prm(1u);
    let setups = prm(2u);
    var last_setup = 0xFFFFFFFFu;
    var s: Setup;
    for (var i = 0u; i < count; i = i + 1u) {
        let p = prims + data[list + i] * PRIM_WORDS;
        let lo = data[p + 2u];
        let hi = data[p + 3u];
        if (x < i32(lo & 0xFFFFu) || y < i32(lo >> 16u) || x > i32(hi & 0xFFFFu) || y > i32(hi >> 16u)) {
            continue;
        }
        let si = data[p];
        if (si != last_setup) {
            s = load_setup(setups + si * SETUP_WORDS);
            last_setup = si;
        }
        shade(p, s, x, y);
    }
}

struct Blit {
    width: u32,
    height: u32,
    pad0: u32,
    pad1: u32,
}

@group(0) @binding(0) var<storage, read> pixels: array<u32>;
@group(0) @binding(1) var<uniform> blit: Blit;

struct VertexOut {
    @builtin(position) position: vec4<f32>,
    @location(0) uv: vec2<f32>,
}

@vertex
fn vs(@builtin(vertex_index) i: u32) -> VertexOut {
    let p = vec2<f32>(f32((i << 1u) & 2u), f32(i & 2u));
    var o: VertexOut;
    o.position = vec4<f32>(p * 2.0 - 1.0, 0.0, 1.0);
    o.uv = vec2<f32>(p.x, 1.0 - p.y);
    return o;
}

fn texel(x: i32, y: i32) -> vec4<f32> {
    let cx = u32(clamp(x, 0, i32(blit.width) - 1));
    let cy = u32(clamp(y, 0, i32(blit.height) - 1));
    return unpack4x8unorm(pixels[cy * blit.width + cx]);
}

@fragment
fn fs(in: VertexOut) -> @location(0) vec4<f32> {
    let p = in.uv * vec2<f32>(f32(blit.width), f32(blit.height)) - 0.5;
    let base = floor(p);
    let f = p - base;
    let x = i32(base.x);
    let y = i32(base.y);
    let top = mix(texel(x, y), texel(x + 1, y), f.x);
    let bottom = mix(texel(x, y + 1), texel(x + 1, y + 1), f.x);
    return vec4<f32>(mix(top, bottom, f.y).rgb, 1.0);
}

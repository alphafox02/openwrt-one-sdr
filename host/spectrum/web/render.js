// SPDX-License-Identifier: MIT
// WebGL2 drawing of the spectrum and waterfall, and the 2D overlay (grid,
// labels, cursor) above them.
//
// The spectrum is drawn per screen column from values the page computes
// (the maximum of the bins under each column), so the shader only turns
// levels into a trace with a gradient fill. The waterfall keeps its rows in
// a ring texture, each with its own frequency range, so zooming, panning and
// retuning redraw the whole history in place.

export const NO_DATA = -1e30;
// Frequencies reach the GPU as float32 offsets from a reference near the rows shown: a float32
// carries 24 bits, so an offset of 50 MHz still resolves 4 Hz where 5 GHz would round to 512 Hz.
const REBASE_HZ = 50e6;

// Waterfall palettes, weakest to strongest.
export const PALETTES = {
    sdrsharp: ['000000', '000020', '000030', '000050', '000091', '1E90FF', 'FFFF00', 'FE6D16', 'FF0000',
               'C60000', '9F0000', '750000', '4A0000'],
    classic: ['000000', '000000', '000050', '1E90FF', 'ADD8E6', 'FFFFFF'],
    turbo: ['30123B', '4145AB', '4675ED', '39A2FC', '1BCFD4', '24ECA6', '61FC6C', 'A4FC3B', 'D1E834',
            'F3C63A', 'FE9B2D', 'F36315', 'D93806', 'B11901', '7A0402'],
    viridis: ['440154', '482878', '3E4989', '31688E', '26828E', '1F9E89', '35B779', '6DCD59', 'B4DE2C', 'FDE725'],
    gray: ['000000', 'FFFFFF'],
};
// The spectrum's fill, bottom to top (SDR#'s spectrum gradient).
const FILL = ['000000', '000050', '1E90FF', 'ADD8E6', 'FFFFFF'];

const VERTEX = `#version 300 es
void main() {
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}`;

const SPECTRUM = `#version 300 es
precision highp float;
uniform highp sampler2D columns;  // row 0: trace, row 1: peak hold (dB per column)
uniform sampler2D fill;
uniform vec2 origin, size;
uniform float top, bottom, fillAlpha, lineWidth;
uniform bool showFill, showPeak;
out vec4 color;

float level(int x, int row) {
    return texelFetch(columns, ivec2(clamp(x, 0, int(size.x) - 1), row), 0).r;
}
float height(float db) { return (db - bottom) / (top - bottom) * size.y; }

// Coverage of the trace through this column, joined halfway to its neighbours.
float trace(int x, float y, int row) {
    float v = level(x, row);
    if (v < -1e29) return 0.0;
    float h = height(v), low = h, high = h;
    float left = level(x - 1, row), right = level(x + 1, row);
    if (left > -1e29) { float m = 0.5 * (h + height(left)); low = min(low, m); high = max(high, m); }
    if (right > -1e29) { float m = 0.5 * (h + height(right)); low = min(low, m); high = max(high, m); }
    float d = y < low ? low - y : (y > high ? y - high : 0.0);
    return 1.0 - smoothstep(0.5 * lineWidth - 0.5, 0.5 * lineWidth + 0.5, d);
}

void main() {
    vec2 p = gl_FragCoord.xy - origin;
    int x = int(p.x);
    vec3 c = vec3(0.0);
    float v = level(x, 0);
    if (showFill && v > -1e29 && p.y < height(v))
        c = mix(c, texture(fill, vec2(clamp(p.y / size.y, 0.0, 1.0), 0.5)).rgb, fillAlpha);
    if (showPeak) c = mix(c, vec3(1.0, 0.72, 0.2), 0.9 * trace(x, p.y, 1));
    c = mix(c, vec3(0.92, 0.97, 1.0), trace(x, p.y, 0));
    color = vec4(c, 1.0);
}`;

const WATERFALL = `#version 300 es
precision highp float;
uniform highp sampler2D rows;   // dB, one row per frame
uniform highp sampler2D meta;   // per row: start, stop (Hz from the reference), bins, valid
uniform sampler2D palette;
uniform vec2 origin, size;
uniform float viewStart, viewSpan, rowHeight, textureWidth, top, bottom, contrast;
uniform int head, filled, textureHeight;
out vec4 color;

void main() {
    vec2 p = gl_FragCoord.xy - origin;
    int age = int((size.y - p.y) / rowHeight);
    color = vec4(0.0, 0.0, 0.0, 1.0);
    if (age >= filled) return;
    int ring = (head - age + textureHeight) % textureHeight;
    vec4 m = texelFetch(meta, ivec2(0, ring), 0);
    float f = viewStart + p.x / size.x * viewSpan;
    float u = (f - m.x) / (m.y - m.x);
    if (m.w < 0.5 || u < 0.0 || u > 1.0) return;
    float x = clamp(u * m.z, 0.5, m.z - 0.5) / textureWidth;
    float db = texture(rows, vec2(x, (float(ring) + 0.5) / float(textureHeight))).r;
    float t = clamp((db - bottom) / (top - bottom) + contrast, 0.0, 1.0);
    color = vec4(texture(palette, vec2(t, 0.5)).rgb, 1.0);
}`;

function hexColor(hex) {
    return [parseInt(hex.slice(0, 2), 16), parseInt(hex.slice(2, 4), 16), parseInt(hex.slice(4, 6), 16)];
}

// 256 RGBA entries interpolated between evenly spaced stops.
function gradient(stops) {
    const out = new Uint8Array(256 * 4);
    const colors = stops.map(hexColor);
    for (let i = 0; i < 256; i++) {
        const t = (i / 255) * (colors.length - 1), k = Math.min(Math.floor(t), colors.length - 2), f = t - k;
        for (let c = 0; c < 3; c++) out[4 * i + c] = Math.round(colors[k][c] * (1 - f) + colors[k + 1][c] * f);
        out[4 * i + 3] = 255;
    }
    return out;
}

export function paletteCss(name) {
    const stops = PALETTES[name] || PALETTES.sdrsharp;
    return `linear-gradient(to right, ${stops.map((s) => '#' + s).join(', ')})`;
}

export class Renderer {
    constructor(canvas, overlay) {
        this.canvas = canvas;
        this.overlay = overlay;
        this.context2d = overlay.getContext('2d');
        this.gl = canvas.getContext('webgl2', {antialias: false, alpha: false, preserveDrawingBuffer: false});
        if (!this.gl) throw new Error('This page needs WebGL 2.');
        this.width = this.height = 1;
        this.rowTimes = [];
        this.setup();
    }

    // (Re)creates every GPU object; also after a lost context comes back.
    setup() {
        const gl = this.gl;
        this.maxTexture = gl.getParameter(gl.MAX_TEXTURE_SIZE);
        this.textureWidth = Math.min(4096, this.maxTexture);
        this.textureHeight = Math.min(1024, this.maxTexture);
        this.spectrumProgram = this.program(SPECTRUM);
        this.waterfallProgram = this.program(WATERFALL);
        gl.bindVertexArray(gl.createVertexArray());

        const texture = (unit) => {
            const t = gl.createTexture();
            gl.activeTexture(gl.TEXTURE0 + unit);
            gl.bindTexture(gl.TEXTURE_2D, t);
            gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
            gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
            gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
            gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
            return t;
        };
        // Units: 0 columns, 1 fill, 2 rows, 3 meta, 4 palette.
        this.columnsTexture = texture(0);
        this.columnsWidth = 0;
        this.fillTexture = texture(1);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
        gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA8, 256, 1, 0, gl.RGBA, gl.UNSIGNED_BYTE, gradient(FILL));
        this.rowsTexture = texture(2);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
        gl.texStorage2D(gl.TEXTURE_2D, 1, gl.R16F, this.textureWidth, this.textureHeight);
        this.metaTexture = texture(3);
        gl.texStorage2D(gl.TEXTURE_2D, 1, gl.RGBA32F, 1, this.textureHeight);
        this.paletteTexture = texture(4);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
        this.setPalette(this.paletteName || 'sdrsharp');

        this.head = 0;
        this.filled = 0;
        this.rowTimes = new Array(this.textureHeight).fill(0);
        this.rowMeta = new Float64Array(4 * this.textureHeight);  // start, stop (Hz), bins, valid
        this.reference = 0;
    }

    // Moves the reference to `hz` and rewrites every row's range relative to it.
    rebase(hz) {
        const gl = this.gl, meta = new Float32Array(this.rowMeta.length);
        this.reference = Math.round(hz / 1e6) * 1e6;
        for (let i = 0; i < meta.length; i += 4) {
            meta[i] = this.rowMeta[i] - this.reference;
            meta[i + 1] = this.rowMeta[i + 1] - this.reference;
            meta[i + 2] = this.rowMeta[i + 2];
            meta[i + 3] = this.rowMeta[i + 3];
        }
        gl.activeTexture(gl.TEXTURE3);
        gl.bindTexture(gl.TEXTURE_2D, this.metaTexture);
        gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, 1, this.textureHeight, gl.RGBA, gl.FLOAT, meta);
    }

    program(fragment) {
        const gl = this.gl;
        const compile = (type, source) => {
            const s = gl.createShader(type);
            gl.shaderSource(s, source);
            gl.compileShader(s);
            if (!gl.getShaderParameter(s, gl.COMPILE_STATUS)) throw new Error(gl.getShaderInfoLog(s));
            return s;
        };
        const p = gl.createProgram();
        gl.attachShader(p, compile(gl.VERTEX_SHADER, VERTEX));
        gl.attachShader(p, compile(gl.FRAGMENT_SHADER, fragment));
        gl.linkProgram(p);
        if (!gl.getProgramParameter(p, gl.LINK_STATUS)) throw new Error(gl.getProgramInfoLog(p));
        p.uniforms = {};
        const n = gl.getProgramParameter(p, gl.ACTIVE_UNIFORMS);
        for (let i = 0; i < n; i++) {
            const name = gl.getActiveUniform(p, i).name;
            p.uniforms[name] = gl.getUniformLocation(p, name);
        }
        return p;
    }

    setPalette(name) {
        const gl = this.gl;
        this.paletteName = PALETTES[name] ? name : 'sdrsharp';
        gl.activeTexture(gl.TEXTURE4);
        gl.bindTexture(gl.TEXTURE_2D, this.paletteTexture);
        gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA8, 256, 1, 0, gl.RGBA, gl.UNSIGNED_BYTE,
                      gradient(PALETTES[this.paletteName]));
    }

    // Canvas size in CSS pixels and the device pixel ratio.
    resize(width, height, ratio) {
        this.ratio = ratio;
        this.width = width;
        this.height = height;
        for (const c of [this.canvas, this.overlay]) {
            c.width = Math.max(1, Math.round(width * ratio));
            c.height = Math.max(1, Math.round(height * ratio));
        }
    }

    // Adds a waterfall row: dB values over [start, stop) Hz.
    pushRow(db, start, stop, time) {
        const gl = this.gl, bins = Math.min(db.length, this.textureWidth);
        this.head = (this.head + 1) % this.textureHeight;
        this.filled = Math.min(this.filled + 1, this.textureHeight);
        this.rowTimes[this.head] = time;
        gl.activeTexture(gl.TEXTURE2);
        gl.bindTexture(gl.TEXTURE_2D, this.rowsTexture);
        gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, this.head, bins, 1, gl.RED, gl.FLOAT,
                         db.length > bins ? db.subarray(0, bins) : db);
        this.rowMeta.set([start, stop, bins, 1], 4 * this.head);
        if (Math.abs((start + stop) / 2 - this.reference) > REBASE_HZ) {
            this.rebase((start + stop) / 2);
            return;
        }
        gl.activeTexture(gl.TEXTURE3);
        gl.bindTexture(gl.TEXTURE_2D, this.metaTexture);
        gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, this.head, 1, 1, gl.RGBA, gl.FLOAT,
                         new Float32Array([start - this.reference, stop - this.reference, bins, 1]));
    }

    // The time of the row `age` rows back, or 0.
    rowTime(age) {
        if (age < 0 || age >= this.filled) return 0;
        return this.rowTimes[(this.head - age + this.textureHeight) % this.textureHeight];
    }

    clearWaterfall() {
        const gl = this.gl;
        gl.activeTexture(gl.TEXTURE3);
        gl.bindTexture(gl.TEXTURE_2D, this.metaTexture);
        gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, 1, this.textureHeight, gl.RGBA, gl.FLOAT,
                         new Float32Array(4 * this.textureHeight));
        this.rowMeta.fill(0);
        this.filled = 0;
    }

    // One value per device-pixel column of the spectrum: the trace, and the
    // peak-hold trace (or null).
    setColumns(trace, peak) {
        const gl = this.gl, w = trace.length;
        gl.activeTexture(gl.TEXTURE0);
        gl.bindTexture(gl.TEXTURE_2D, this.columnsTexture);
        if (w !== this.columnsWidth) {
            gl.texImage2D(gl.TEXTURE_2D, 0, gl.R32F, w, 2, 0, gl.RED, gl.FLOAT, null);
            this.columnsWidth = w;
        }
        gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, w, 1, gl.RED, gl.FLOAT, trace);
        if (peak) gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 1, w, 1, gl.RED, gl.FLOAT, peak);
    }

    // layout: CSS pixel heights {spectrum, axis, waterfall}; view: {start, stop} Hz;
    // levels: {top, bottom} dB, contrast; fill, peak: booleans.
    draw(layout, view, s) {
        const gl = this.gl, r = this.ratio, W = this.canvas.width, H = this.canvas.height;
        const spectrumH = Math.round(layout.spectrum * r), waterfallH = H - Math.round((layout.spectrum + layout.axis) * r);
        gl.disable(gl.SCISSOR_TEST);
        gl.viewport(0, 0, W, H);
        gl.clearColor(0.03, 0.035, 0.045, 1);
        gl.clear(gl.COLOR_BUFFER_BIT);

        if (spectrumH > 0 && this.columnsWidth === W) {
            const p = this.spectrumProgram, u = p.uniforms;
            gl.useProgram(p);
            gl.viewport(0, H - spectrumH, W, spectrumH);
            gl.uniform1i(u.columns, 0);
            gl.uniform1i(u.fill, 1);
            gl.uniform2f(u.origin, 0, H - spectrumH);
            gl.uniform2f(u.size, W, spectrumH);
            gl.uniform1f(u.top, s.top);
            gl.uniform1f(u.bottom, s.bottom);
            gl.uniform1f(u.fillAlpha, 180 / 255);
            gl.uniform1f(u.lineWidth, 1.25 * r);
            gl.uniform1i(u.showFill, s.fill ? 1 : 0);
            gl.uniform1i(u.showPeak, s.peak ? 1 : 0);
            gl.drawArrays(gl.TRIANGLES, 0, 3);
        }
        if (waterfallH > 0) {
            const p = this.waterfallProgram, u = p.uniforms;
            gl.useProgram(p);
            gl.viewport(0, 0, W, waterfallH);
            gl.uniform1i(u.rows, 2);
            gl.uniform1i(u.meta, 3);
            gl.uniform1i(u.palette, 4);
            gl.uniform2f(u.origin, 0, 0);
            gl.uniform2f(u.size, W, waterfallH);
            gl.uniform1f(u.viewStart, view.start - this.reference);
            gl.uniform1f(u.viewSpan, view.stop - view.start);
            gl.uniform1f(u.rowHeight, Math.max(1, r));
            gl.uniform1f(u.textureWidth, this.textureWidth);
            gl.uniform1f(u.top, s.top);
            gl.uniform1f(u.bottom, s.bottom);
            gl.uniform1f(u.contrast, s.contrast);
            gl.uniform1i(u.head, this.head);
            gl.uniform1i(u.filled, this.filled);
            gl.uniform1i(u.textureHeight, this.textureHeight);
            gl.drawArrays(gl.TRIANGLES, 0, 3);
        }
    }
}

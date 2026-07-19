#!/usr/bin/env node

/**
 * Decode a Draco-compressed Archie GLB and sample its real triangle surfaces.
 *
 * The private GLB never enters the repository. Only deterministic, quantized
 * vertices are emitted for the firmware. Sampling triangles by surface area
 * preserves the silhouette better than selecting mesh vertices (whose density
 * depends on the modeling/export pipeline).
 */

import { createHash } from 'node:crypto';
import { readFile, writeFile } from 'node:fs/promises';
import { NodeIO } from '@gltf-transform/core';
import { ALL_EXTENSIONS } from '@gltf-transform/extensions';
import draco3d from 'draco3dgltf';

function usage() {
  console.error(
    'usage: node generate_archie_pointcloud_from_glb.mjs <source.glb> <output.hpp> ' +
      '[--points 5200] [--preview preview.svg]'
  );
  process.exit(2);
}

const args = process.argv.slice(2);
if (args.length < 2) usage();
const sourcePath = args[0];
const outputPath = args[1];
let pointCount = 5200;
let previewPath = '';
for (let i = 2; i < args.length; i += 2) {
  if (args[i] === '--points') pointCount = Number.parseInt(args[i + 1], 10);
  else if (args[i] === '--preview') previewPath = args[i + 1];
  else usage();
}
if (!Number.isFinite(pointCount) || pointCount < 100) usage();

const sourceBytes = await readFile(sourcePath);
const digest = createHash('sha256').update(sourceBytes).digest();
let rngState = digest.readUInt32LE(0) || 0x4e565331;
function random() {
  // xorshift32: deterministic across Node versions and platforms.
  rngState ^= rngState << 13;
  rngState ^= rngState >>> 17;
  rngState ^= rngState << 5;
  return (rngState >>> 0) / 0x100000000;
}

const io = new NodeIO()
  .registerExtensions(ALL_EXTENSIONS)
  .registerDependencies({
    'draco3d.decoder': await draco3d.createDecoderModule(),
  });
const document = await io.read(sourcePath);

function transformPoint(matrix, x, y, z) {
  return [
    matrix[0] * x + matrix[4] * y + matrix[8] * z + matrix[12],
    matrix[1] * x + matrix[5] * y + matrix[9] * z + matrix[13],
    matrix[2] * x + matrix[6] * y + matrix[10] * z + matrix[14],
  ];
}

const triangles = [];
let cumulativeArea = 0;
for (const node of document.getRoot().listNodes()) {
  const mesh = node.getMesh();
  if (!mesh) continue;
  const matrix = node.getWorldMatrix();
  for (const primitive of mesh.listPrimitives()) {
    const position = primitive.getAttribute('POSITION');
    const indices = primitive.getIndices();
    if (!position || !indices || primitive.getMode() !== 4) continue;
    const p = [0, 0, 0];
    const vertices = new Array(position.getCount());
    for (let i = 0; i < position.getCount(); ++i) {
      position.getElement(i, p);
      vertices[i] = transformPoint(matrix, p[0], p[1], p[2]);
    }
    const indexArray = indices.getArray();
    for (let i = 0; i + 2 < indexArray.length; i += 3) {
      const a = vertices[indexArray[i]];
      const b = vertices[indexArray[i + 1]];
      const c = vertices[indexArray[i + 2]];
      const abx = b[0] - a[0], aby = b[1] - a[1], abz = b[2] - a[2];
      const acx = c[0] - a[0], acy = c[1] - a[1], acz = c[2] - a[2];
      const cx = aby * acz - abz * acy;
      const cy = abz * acx - abx * acz;
      const cz = abx * acy - aby * acx;
      const area = 0.5 * Math.hypot(cx, cy, cz);
      if (area < 1e-12) continue;
      cumulativeArea += area;
      triangles.push({ a, b, c, end: cumulativeArea });
    }
  }
}
if (!triangles.length) throw new Error('GLB contains no indexed triangle surfaces');

function triangleForArea(target) {
  let lo = 0, hi = triangles.length - 1;
  while (lo < hi) {
    const mid = (lo + hi) >>> 1;
    if (target <= triangles[mid].end) hi = mid;
    else lo = mid + 1;
  }
  return triangles[lo];
}

const sampled = [];
for (let i = 0; i < pointCount; ++i) {
  const tri = triangleForArea(random() * cumulativeArea);
  const root = Math.sqrt(random());
  const wa = 1 - root;
  const wb = root * (1 - random());
  const wc = 1 - wa - wb;
  sampled.push([
    tri.a[0] * wa + tri.b[0] * wb + tri.c[0] * wc,
    tri.a[1] * wa + tri.b[1] * wb + tri.c[1] * wc,
    tri.a[2] * wa + tri.b[2] * wb + tri.c[2] * wc,
  ]);
}

const min = [Infinity, Infinity, Infinity];
const max = [-Infinity, -Infinity, -Infinity];
for (const point of sampled) {
  for (let axis = 0; axis < 3; ++axis) {
    min[axis] = Math.min(min[axis], point[axis]);
    max[axis] = Math.max(max[axis], point[axis]);
  }
}
const center = min.map((value, axis) => (value + max[axis]) / 2);
// glTF is Y-up. One common scale preserves the real model proportions, with
// the tallest dimension fitting model-space -512..512.
const scale = 1024 / Math.max(max[1] - min[1], max[0] - min[0]);

const quantized = sampled.map((point, i) => {
  const x = Math.round((point[0] - center[0]) * scale);
  const y = Math.round(-(point[1] - center[1]) * scale);
  const z = Math.round((point[2] - center[2]) * scale);
  // Mix real depth with a deterministic shimmer rank.
  const depth = (point[2] - min[2]) / Math.max(1e-9, max[2] - min[2]);
  const glow = Math.max(48, Math.min(255, Math.round(92 + depth * 118 + random() * 45)));
  return [x, y, z, glow, i];
});
quantized.sort((a, b) => a[1] - b[1] || a[0] - b[0] || a[4] - b[4]);

const hpp = [
  '#pragma once',
  '',
  '#include <cstddef>',
  '#include <cstdint>',
  '',
  '// Generated from the supplied Draco GLB by tools/generate_archie_pointcloud_from_glb.mjs.',
  '// The source model and textures are not embedded; these are quantized surface samples.',
  'struct ArchiePointVertex {',
  '    int16_t x;',
  '    int16_t y;',
  '    int16_t z;',
  '    uint8_t glow;',
  '};',
  '',
  'inline constexpr ArchiePointVertex kArchiePointVertices[] = {',
];
for (const [x, y, z, glow] of quantized) hpp.push(`    {${x}, ${y}, ${z}, ${glow}},`);
hpp.push(
  '};',
  '',
  'inline constexpr std::size_t kArchiePointVertexCount =',
  '    sizeof(kArchiePointVertices) / sizeof(kArchiePointVertices[0]);',
  ''
);
await writeFile(outputPath, hpp.join('\n'), 'utf8');

if (previewPath) {
  const width = 720, height = 720;
  const dots = quantized
    .map(([x, y, , glow]) => {
      const px = width / 2 + x * 0.62;
      const py = height / 2 + y * 0.62;
      const alpha = (0.28 + glow / 355).toFixed(2);
      const radius = glow > 205 ? 1.55 : 1.0;
      return `<circle cx="${px.toFixed(1)}" cy="${py.toFixed(1)}" r="${radius}" fill="#e7c9c0" opacity="${alpha}"/>`;
    })
    .join('');
  const svg = `<svg xmlns="http://www.w3.org/2000/svg" width="${width}" height="${height}" viewBox="0 0 ${width} ${height}"><rect width="100%" height="100%" fill="#070605"/>${dots}</svg>`;
  await writeFile(previewPath, svg, 'utf8');
}

console.log(
  `decoded ${triangles.length} triangles; sampled ${quantized.length} particles; ` +
    `bounds ${min.map((v, i) => `${v.toFixed(4)}..${max[i].toFixed(4)}`).join(', ')}`
);

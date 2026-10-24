#!/usr/bin/env node
// SPDX-License-Identifier: GPL-3.0-only
/* Regenerates the mock device's engine tables in web/editor.html from the firmware's own (build/host/desc.json, written by
 * tests/run_tests.sh from tests/descdump.c): every engine's titles, EDIT descriptors and factory presets, between
 *   /*ENG-BEGIN*​/ and /*ENG-END*​/ (the mock device) and ENGINE_PAGES between /*EPAGES-BEGIN*​/ and /*EPAGES-END*​/ (the EDIT pages the editor shows for each engine)
 * The test "editor: mock tables == firmware" (web/test_web.mjs) fails when the two drift apart: run
 *   sh tests/run_tests.sh      (once, for build/host/desc.json)
 *   node web/sync_mock.mjs     (this)
 * The removed engines (ENG_GONE) are the same reserved entry. */
import { readFileSync, writeFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const HERE = dirname(fileURLToPath(import.meta.url));
const DESC = process.env.FELUCCA_DESC || join(HERE, "../build/host/desc.json");
const HTML = join(HERE, "editor.html");
const FMT = ["INT", "PCT", "BIPCT", "TIME", "LFOHZ", "CUTOFF", "DB", "SEMI", "ENUM", "BPM", "NOTE", "ONOFF", "OCT", "STEPS", "REVDEC", "DTIME"];
const fw = JSON.parse(readFileSync(DESC, "utf8"));

const q = (s) => JSON.stringify(s);
const desc = (d) => {
  const f = FMT[d.fmt] ? `F.${FMT[d.fmt]}` : String(d.fmt);
  const extra = d.unit ? `, ${d.names ? JSON.stringify(d.names) : "null"}, ${q(d.unit)}` : d.names ? `, ${JSON.stringify(d.names)}` : "";
  return `D(${q(d.label)}, ${f}, ${d.min}, ${d.max}, ${d.def}${extra})`;
};
const isGone = (e) => e.name === "-" && e.edit.every((d) => d.label === "-" && d.min === -128 && d.max === 127 && d.def === 0 && !d.names);   /* (ENG_GONE; engine 1, DIGITAL retired, has its own) */
const lines = [];
fw.ENG.forEach((e, i) => {
  if (isGone(e)) { lines.push(`    GONE(),   /* ${i}: removed (engines.c ENG_GONE): its values load as they were and play silence */`); return; }
  lines.push(i === 1 ? `    /* 1: reserved (DIGITAL, retired: src/fm4_convert.c ENG_FM4_GONE; its sounds load as FM6) */` : `    /* ${i} ${e.name} (src/eng_*.c) */`);
  lines.push(`    { name: ${q(e.name)}, titles: ${JSON.stringify(e.titles)}, edit: [`);
  e.edit.forEach((d, k) => lines.push(`        ${desc(d)}${k < e.edit.length - 1 ? "," : "],"}`));
  const td = (fw.TDESC && fw.TDESC[i]) || [];
  if (td.length) {                                   /* the operator slots (61..80) under this engine's names (engine_t tdesc) */
    lines.push("      tdesc: {");
    td.forEach(([id, d], k) => lines.push(`        ${id}: ${desc(d)}${k < td.length - 1 ? "," : ""}`));
    lines.push("      },");
  }
  lines.push("      presets: [");
  e.presets.forEach((p, k) => lines.push(`        P(${q(p.name)}, ${JSON.stringify(p.e)}, ${JSON.stringify(p.env)}, ${p.mono}, ${p.pat})${k < e.presets.length - 1 ? "," : "] },"}`));
  if (!e.presets.length) lines[lines.length - 1] = "      presets: [] },";
});
const block = `/*ENG-BEGIN*/\n${lines.join("\n")}\n    /*ENG-END*/`;

/* the EDIT pages of each live engine (params.c PAGES): [title, parameter ids] */
const pages = [];
fw.EPAGES.forEach((pp, i) => { if (pp.length) pages.push(`  ${i}: ${JSON.stringify(pp)},   /* ${fw.ENG[i].name} */`); });
const pblock = `/*EPAGES-BEGIN*/\nconst ENGINE_PAGES = {\n${pages.join("\n")}\n};\n/*EPAGES-END*/`;

let html = readFileSync(HTML, "utf8");
const a = html.indexOf("/*ENG-BEGIN*/"), b = html.indexOf("/*ENG-END*/");
if (a < 0 || b < 0) { console.error("sync_mock: no /*ENG-BEGIN*/ .. /*ENG-END*/ in editor.html"); process.exit(1); }
html = html.slice(0, a) + block + html.slice(b + "/*ENG-END*/".length);
const pa = html.indexOf("/*EPAGES-BEGIN*/"), pb = html.indexOf("/*EPAGES-END*/");
if (pa < 0 || pb < 0) { console.error("sync_mock: no /*EPAGES-BEGIN*/ .. /*EPAGES-END*/ in editor.html"); process.exit(1); }
html = html.slice(0, pa) + pblock + html.slice(pb + "/*EPAGES-END*/".length);
writeFileSync(HTML, html);
console.log(`sync_mock: ${fw.ENG.length} engines (${fw.ENG.filter((e) => !isGone(e)).length} live) written to editor.html`);

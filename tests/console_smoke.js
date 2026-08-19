/*
 * console_smoke.js -- Render every tab of the console, headlessly.
 *
 * The console is one file of hand-written DOM building driven by whatever rcd
 * serves, and a TypeError anywhere in render() empties the whole page rather
 * than spoiling one row: `sheet` is cleared first and the exception stops the
 * loop that refills it. That failure mode has now happened twice, both times
 * from a value that was fine on the tab that was looked at and wrong on the
 * others, so it is worth being able to draw all of them without a browser.
 *
 * The shim is deliberately thin. It is not a DOM; it is enough of one to let
 * the page build its tree and to notice when building it throws.
 *
 *   node tests/console_smoke.js [path/to/index.html]
 */
"use strict";

const fs = require("fs");
const path = require("path");
const vm = require("vm");

const page = process.argv[2] || path.join(__dirname, "..", "rhd", "index.html");
const html = fs.readFileSync(page, "utf8");

const script = [...html.matchAll(/<script[^>]*>([\s\S]*?)<\/script>/g)].map(m => m[1]).join("\n");
if (!script.trim()) fail("no <script> found in " + page);

function fail(msg) {
	console.error("FAIL " + msg);
	process.exit(1);
}

/* The page builds its tree from inside an async boot(), so a TypeError there
 * surfaces as a rejected promise rather than as a throw this file can wrap. */
process.on("uncaughtException", e => fail("the page threw: " + ((e && e.stack) || e)));
process.on("unhandledRejection", e => fail("the page left a promise rejected: " +
					   ((e && e.stack) || e)));

/* ── the thinnest DOM that can hold a tree ── */

class El {
	constructor(tag) {
		this.tagName = String(tag).toUpperCase();
		this.children = [];
		this.attrs = {};
		this.dataset = {};
		this.style = {};
		this.classes = new Set();
		this._text = "";
		this.hidden = false;
		this.disabled = false;
		this.handlers = {};
		const self = this;
		this.classList = {
			add: (...c) => c.forEach(x => self.classes.add(x)),
			remove: (...c) => c.forEach(x => self.classes.delete(x)),
			toggle: (c, on) => (on === undefined ? (self.classes.has(c) ? self.classes.delete(c)
									       : self.classes.add(c))
					   : on ? self.classes.add(c) : self.classes.delete(c)),
			contains: c => self.classes.has(c),
		};
	}
	get className() { return [...this.classes].join(" "); }
	set className(v) { this.classes = new Set(String(v).split(/\s+/).filter(Boolean)); }
	get textContent() {
		return this.children.length ? this.children.map(c => c.textContent).join("") : this._text;
	}
	set textContent(v) { this._text = String(v); this.children = []; }
	get firstChild() { return this.children[0] || null; }
	get offsetWidth() { return 0; }
	append(...nodes) {
		nodes.forEach(n => this.children.push(typeof n === "string" ? new Text(n) : n));
	}
	appendChild(n) { this.append(n); return n; }
	prepend(...nodes) {
		this.children.unshift(...nodes.map(n => (typeof n === "string" ? new Text(n) : n)));
	}
	get lastChild() { return this.children[this.children.length - 1] || null; }
	insertBefore(n) { this.append(n); return n; }
	replaceChildren(...nodes) { this.children = []; this.append(...nodes); }
	setAttribute(k, v) { this.attrs[k] = String(v); }
	getAttribute(k) { return this.attrs[k] === undefined ? null : this.attrs[k]; }
	removeAttribute(k) { delete this.attrs[k]; }
	addEventListener(ev, fn) { (this.handlers[ev] = this.handlers[ev] || []).push(fn); }
	remove() {}
	focus() {}
	blur() {}
	/* Depth-first walk; enough for the ".chip" and ".tab" lookups the page does. */
	querySelectorAll(sel) {
		const want = sel.replace(/^\./, "");
		const byClass = sel.startsWith(".");
		const out = [];
		const walk = e => e.children.forEach(c => {
			if (c instanceof El) {
				if (byClass ? c.classes.has(want) : c.tagName === sel.toUpperCase())
					out.push(c);
				walk(c);
			}
		});
		walk(this);
		return out;
	}
	querySelector(sel) { return this.querySelectorAll(sel)[0] || null; }
}

class Text {
	constructor(t) { this._text = String(t); }
	get textContent() { return this._text; }
	set textContent(v) { this._text = String(v); }
}

const nodes = {};
const document = {
	activeElement: null,
	createElement: t => new El(t),
	createTextNode: t => new Text(t),
	getElementById: id => (nodes[id] = nodes[id] || new El("div")),
	querySelector: sel => root.querySelector(sel),
	querySelectorAll: sel => root.querySelectorAll(sel),
	addEventListener: () => {},
};
const root = new El("body");
/* Everything the page looks up by id lives under one root so the ".tab"
 * sweep in paintTabs finds the buttons the page appended to #tabs. */
const ids = [...html.matchAll(/getElementById\("([a-zA-Z0-9_]+)"\)/g)].map(m => m[1]);
[...new Set(ids)].forEach(id => { nodes[id] = new El("div"); root.append(nodes[id]); });

/* ── the camera, as far as the page can tell ── */

let served = 0;
function reply(body) {
	served++;
	const cmd = body.cmd;
	if (cmd === "hello")
		return {api: 1, status: "ok", daemon: "rcd", build: "smoke",
			daemons: {rvd: {installed: true, running: true, impact: "pipeline"},
				  rsd: {installed: true, running: true, impact: "stream"},
				  rad: {installed: true, running: true, impact: "service"},
				  rod: {installed: true, running: true, impact: "service"},
				  ric: {installed: true, running: true, impact: "service"},
				  rmr: {installed: true, running: true, impact: "service"},
				  rhd: {installed: true, running: true, impact: "stream"},
				  rwd: {installed: true, running: true, impact: "stream"}}};
	if (cmd === "schema") return SCHEMA;
	if (cmd === "get") return {api: 1, status: "ok", values: valuesFor(body.section)};
	if (cmd === "pending") return {api: 1, status: "ok", stale: []};
	if (cmd === "state") return {api: 1, status: "ok", up: {}, daemons_up: 6};
	return {api: 1, status: "ok"};
}

const SCHEMA = JSON.parse(fs.readFileSync(path.join(__dirname, "console_schema.json"), "utf8"));

/*
 * Half of every section is configured and half is not, so both branches of the
 * reset control are drawn on every tab rather than whichever the bench camera
 * happened to have.
 */
function valuesFor(section) {
	const out = [];
	SCHEMA.keys.filter(k => k.section === section).forEach((k, i) => {
		const o = {section: k.section, key: k.key};
		if (k.type === "credential") { o.set = true; out.push(o); return; }
		o.value = k.type === "bool" ? true
			: k.type === "enum" ? (k.choices || ["x"])[0]
			: k.type === "host" ? "camera.local"
			: k.type === "ipv4" ? "192.168.1.50"
			: k.labels ? k.labels[0]
			: (k.min || 0);
		o.source = k.section === "system" || k.section === "network" ? "system" : "daemon";
		if (i % 2) o.configured = false;
		out.push(o);
	});
	return out;
}

const sandbox = {
	document, console,
	window: {addEventListener: () => {}, location: {}},
	location: {protocol: "http:", host: "cam", hostname: "cam", port: "8080", origin: "http://cam:8080"},
	navigator: {userAgent: "smoke"},
	setTimeout: (fn) => { void fn; return 0; },      /* no timers: one pass, then stop */
	clearTimeout: () => {},
	setInterval: () => 0,
	clearInterval: () => {},
	requestAnimationFrame: () => 0,
	AbortController: function () { this.signal = {}; this.abort = () => {}; },
	fetch: async (url, opt) => {
		const body = opt && opt.body ? JSON.parse(opt.body) : {};
		return {ok: true, status: 200, json: async () => reply(body), text: async () => ""};
	},
	Image: function () {},
	EventSource: function () { this.addEventListener = () => {}; this.close = () => {}; },
	URL: URL, URLSearchParams: URLSearchParams, JSON, Math, Date, Set, Map, Promise,
	parseInt, parseFloat, isNaN, encodeURIComponent, decodeURIComponent, btoa: s => s,
};
sandbox.globalThis = sandbox;
sandbox.window.document = document;

/*
 * `const` and `let` at the top level of a script are lexical, not properties of
 * the global object, so the page's own names are invisible from out here. The
 * epilogue runs in that same scope and hands out the few this needs.
 */
const probe_epilogue = `
;globalThis.__probe = {
  TABS: TABS, byId: byId, render: render, refreshBar: refreshBar,
  getKeys: function () { return KEYS; },
  canReset: canReset, toggleReset: toggleReset,
  setActive: function (v) { active = v; },
};
`;

const ctx = vm.createContext(sandbox);
try {
	vm.runInContext(script + probe_epilogue, ctx, {filename: "index.html"});
} catch (e) {
	fail("the page threw while loading: " + e.stack);
}

/* boot() is async and the page calls it itself; let its promise settle. */
(async () => {
	for (let i = 0; i < 50; i++)
		await new Promise(r => setTimeout(r, 1));

	const p = sandbox.__probe;
	if (!p || !p.TABS || !p.TABS.length) fail("no TABS in the page");
	if (!p.getKeys().length) fail("the page kept no keys from the schema it was served");

	const sheet = nodes.sheet;
	let drawn = 0, undos = 0;
	p.TABS.forEach(t => {
		p.setActive(t.id);
		try {
			p.render();
		} catch (e) {
			fail("render() threw on tab '" + t.id + "': " + e.stack);
		}
		const groups = sheet.querySelectorAll(".group");
		if (!groups.length)
			fail("tab '" + t.id + "' drew no groups at all");
		drawn += groups.length;
		undos += sheet.querySelectorAll(".undo").length;
	});

	/* The control the last bug lived in: a group holding exactly one section
	 * used to hand a section name where a key id was expected. */
	if (!undos) fail("no reset control was drawn on any tab");

	/*
	 * And the same tabs with a reset staged on them. A staged reset draws a
	 * row differently -- dimmed, relabelled, its readback replaced -- which
	 * is a second path through every widget the first pass just drew.
	 */
	let staged = 0;
	p.TABS.forEach(t => {
		p.setActive(t.id);
		p.render();
		const id = Object.keys(p.byId).find(i => p.canReset(i));
		if (!id) return;
		p.toggleReset(id, true);
		staged++;
		try {
			p.render();
			p.refreshBar();
		} catch (e) {
			fail("render() threw with a reset staged on '" + id + "': " + e.stack);
		}
		p.toggleReset(id, false);
	});
	if (!staged) fail("no key could be staged for reset");

	console.log("ok  " + p.TABS.length + " tabs, " + drawn + " groups, " + undos +
		    " reset controls, " + staged + " redrawn with a reset staged, " + served +
		    " requests served");
})();

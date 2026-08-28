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
 *   node tests/console_smoke.js [path/to/console.html]
 */
"use strict";

const fs = require("fs");
const path = require("path");
const vm = require("vm");

const page = process.argv[2] || path.join(__dirname, "..", "rhd", "console.html");
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
	contains(n) {
		if (!n) return false;
		if (n === this) return true;
		return this.children.some(c => c instanceof El && c.contains(n));
	}
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
const sent = [];          /* every request body, so a test can read the last one */
/* The value the camera insists on for the next `set`, or null to take it. */
let refuseNext = null;
function reply(body) {
	served++;
	sent.push(body);
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
	/* The camera's answer echoed back, which is what the page writes into
	 * the control -- an ISP quantises, and the value that comes back is
	 * the one in force. */
	if (cmd === "set") {
		const e = (body.edits || [])[0] || {};
		/* A live command the daemon would not take: rcd writes the file
		 * instead and reports the value still in force. */
		if (refuseNext !== null)
			return {api: 1, status: "ok",
				results: [{section: e.section, key: e.key, value: refuseNext,
					   applied: "saved", note: "refused while running"}]};
		return {api: 1, status: "ok",
			results: [{section: e.section, key: e.key, value: e.value,
				   applied: "live"}]};
	}
	if (cmd === "state") return {api: 1, status: "ok", up: {}, daemons_up: 6,
				    ir: {mode: "auto", state: "day"}, image: ISP_STATE};
	return {api: 1, status: "ok"};
}

const SCHEMA = JSON.parse(fs.readFileSync(path.join(__dirname, "console_schema.json"), "utf8"));

/*
 * [image] as an Infinity6C answers for it, copied from an SSC377QE running
 * the imx335 tuning. The numbers are the point: none of these ranges is the
 * 0-255 the schema carries, brightness is a module the tuning ships switched
 * off, and three knobs are following the tuning's own curve rather than any
 * value of ours. A page that drew its own bounds instead would look right on
 * this reply and be wrong on every value it sent.
 *
 * temper carries caps and no value, which is a camera saying it has no reading
 * for the knob -- Ingenic has no getter for the denoise strengths at all, and
 * withholds any knob the running rvd has not written, since IMP's readback is
 * a cache that dies with the process. The page has to draw that from the caps
 * rather than from a number nobody sent.
 */
const ISP_STATE = {
	brightness: 50, contrast: 65, sharpness: 40, ae_comp: 0,
	drc_strength: 128, defog_strength: 52, hflip: 0, vflip: 0,
	auto: ",brightness,sharpness,drc_strength,",
	settable: ",brightness,contrast,sharpness,temper,hflip,vflip,ae_comp," +
		  "drc_strength,defog_strength,",
	caps: {
		brightness: {min: 0, max: 100, neutral: 50, auto: true, enabled: false},
		contrast: {min: 0, max: 100, neutral: 50, auto: true, enabled: true},
		sharpness: {min: 0, max: 127, neutral: 40, auto: true, enabled: true},
		temper: {min: 0, max: 7, neutral: 1, auto: false, enabled: true},
		ae_comp: {min: -20, max: 20, neutral: 0, auto: false, enabled: true},
		drc_strength: {min: 0, max: 255, neutral: 128, auto: true, enabled: true},
		defog_strength: {min: 0, max: 255, neutral: 128, auto: true, enabled: true},
	},
};

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
		/*
		 * rvd stops writing an [image] key the config never named, so
		 * the ordinary state of a knob is absent from the file and
		 * following the tuning -- and one of them carries the word,
		 * which is what a knob handed back on purpose looks like.
		 */
		if (section === "image" && ISP_STATE.caps[k.key]) {
			if (k.key === "defog_strength") { o.value = "auto"; o.source = "daemon"; }
			else if (k.key === "contrast") { o.value = 65; o.source = "daemon"; }
			else o.set = false;
			out.push(o);
			return;
		}
		o.value = k.type === "bool" ? true
			: k.type === "enum" ? (k.choices || ["x"])[0]
			: k.type === "host" ? "camera.local"
			: k.type === "ipv4" ? "192.168.1.50"
			: k.labels ? k.labels[0]
			: (k.min || 0);
		o.source = k.section === "device" || k.section === "network" ? "system" : "daemon";
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
  ACT_STATE: ACT_STATE,
  V: V,
};
`;

const ctx = vm.createContext(sandbox);
try {
	vm.runInContext(script + probe_epilogue, ctx, {filename: "console.html"});
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
	const settle = async () => { for (let i = 0; i < 20; i++) await new Promise(r => setTimeout(r, 1)); };
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

	/*
	 * Actions, which nothing above touches: the day/night override shipped
	 * broken because the page named the verb `name` where rcd reads
	 * `action`, and every button on the page failed the same way with
	 * nothing on screen to show it.
	 */
	p.setActive("night");
	p.render();
	const seg = sheet.querySelectorAll(".seg").find(e => e.dataset.act === "ircut-mode");
	if (!seg) fail("the day/night tab drew no override control");
	const night = seg.querySelectorAll("button").find(b => b.textContent === "night");
	if (!night) fail("the override control has no 'night' choice");

	await night.handlers.click[0]();
	const req = sent[sent.length - 1];
	if (req.cmd !== "action" || req.action !== "ircut-mode")
		fail("clicking 'night' sent " + JSON.stringify(req) + ", not an ircut-mode action");
	if (req.value !== "night")
		fail("the ircut-mode action carried value " + JSON.stringify(req.value));
	if (night.getAttribute("aria-pressed") !== "true")
		fail("'night' did not latch after the camera took it");

	/* And the camera's own answer wins over the click: an override moved by
	 * raptorctl or MQTT has to reach this page too. */
	if (p.ACT_STATE["ircut-mode"] !== "auto")
		fail("the page did not read the mode out of `state` (got " +
		     p.ACT_STATE["ircut-mode"] + ")");

	/*
	 * Rate control: the first named value on the live tier, and it sits in
	 * the encoder matrix, whose cells carry no row id to find it by. Both
	 * halves are worth pinning -- that the mode reaches rcd as this
	 * section's own key, with no channel of the page's invention, and that
	 * a refusal puts the control back. A select that keeps showing the mode
	 * the camera would not take is a control that lies about the camera.
	 */
	p.setActive("streams");
	p.render();
	const rcRow = sheet.querySelectorAll("tr").find(
		r => r.querySelectorAll(".id").some(i => i.textContent === "*.rc_mode"));
	if (!rcRow) fail("the encoder matrix drew no rate-control row");
	const rcCells = rcRow.querySelectorAll(".cell");
	const rcSel = rcCells.map(c => c.querySelector("select"));
	if (rcSel.length !== 2 || !rcSel[0] || !rcSel[1])
		fail("rate control was not drawn for both encoders");
	if (!rcSel[0].querySelectorAll("option").some(o => o.value === "capped_vbr"))
		fail("the rate-control choices did not come from the schema");
	/* Six mode names mean nothing on their own, and a matrix cell has no
	   room to say what they are: the row label is the only place left. */
	if (!rcRow.querySelectorAll(".help").length)
		fail("the rate-control row drew six bare mode names and no explanation");

	rcSel[0].value = "vbr";
	rcSel[0].handlers.change[0]();
	await settle();
	let rc = sent[sent.length - 1];
	if (rc.cmd !== "set" || rc.edits[0].section !== "stream0" ||
	    rc.edits[0].key !== "rc_mode" || rc.edits[0].value !== "vbr")
		fail("choosing a rate control sent " + JSON.stringify(rc));
	if (rc.edits[0].channel !== undefined)
		fail("the page invented a channel; the section is what carries it");

	/* And the refusal. rcd answers with the value in force, which after a
	 * refused live command is the one the camera still has. */
	refuseNext = "cbr";
	rcSel[0].value = "fixqp";
	rcSel[0].handlers.change[0]();
	await settle();
	refuseNext = null;
	if (rcSel[0].value !== "cbr")
		fail("a refused mode left the control showing " + rcSel[0].value);

	/*
	 * The image knobs, which have no fixed scale to draw from. Their range
	 * is the hardware's and arrives in `state`; the schema's bounds are the
	 * widest any platform accepts and are wrong for every one of them here.
	 * A page that ignored caps would look plausible and send values the
	 * camera refuses -- or, worse, take them: 140 on a 0-100 knob.
	 */
	p.setActive("image");
	p.render();
	const rowFor = key => sheet.querySelectorAll(".row").find(r => r.dataset.id === "image." + key);
	const sliderIn = row => row.querySelectorAll("input").find(i => i.type === "range");

	const contrast = rowFor("contrast");
	if (!contrast) fail("the image tab drew no contrast row");
	if (sliderIn(contrast).max !== 100)
		fail("contrast was drawn 0-" + sliderIn(contrast).max +
		     ", not the 0-100 the camera published");

	const ae = rowFor("ae_comp");
	if (Number(sliderIn(ae).min) >= 0)
		fail("exposure compensation was drawn from " + sliderIn(ae).min +
		     ", so the whole darker half is unreachable");

	/* A module the tuning ships switched off. Setting a value switches it
	 * on, so the control is not dead -- but using it leaves the tuning
	 * behind in a way the number alone does not show, and the page has to
	 * say so while it is still true. */
	if (!rowFor("brightness").querySelectorAll(".modoff").length)
		fail("brightness is switched off in the tuning and the page did not say so");
	if (rowFor("contrast").querySelectorAll(".modoff").length)
		fail("contrast is switched on and the page said otherwise");

	/* Auto is a state of the camera, not a memory of what was clicked: the
	 * knobs it names are following the tuning whoever set them that way. */
	const autoBtn = key => rowFor(key).querySelector(".autobtn");
	if (autoBtn("brightness").getAttribute("aria-pressed") !== "true")
		fail("brightness is in the camera's auto list and the page drew it as chosen");
	if (autoBtn("contrast").getAttribute("aria-pressed") !== "false")
		fail("contrast carries a value of 65 and the page drew it as auto");
	if (autoBtn("defog_strength").getAttribute("aria-pressed") !== "true")
		fail("defog is configured as the word auto and the page drew it as a number");
	/* 3DNR is a VPE level with no tuning curve behind it, so there is
	 * nothing to hand back and nothing to offer. */
	if (rowFor("temper").querySelector(".autobtn"))
		fail("temporal denoise has no auto mode on this camera and the page offered one");

	/*
	 * And a knob the camera sends caps but no value for has to land on the
	 * neutral those caps carry. Drawing 0 there -- the number a missing
	 * field reads as -- would put the control somewhere the camera never
	 * said, under a row already reading "not set" and "tuning 1".
	 */
	if (Number(sliderIn(rowFor("temper")).value) !== 1)
		fail("a knob the camera sent no value for drew " +
		     sliderIn(rowFor("temper")).value + " instead of the tuning's neutral");

	/* And the knob on auto still shows where the picture is -- the tuner's
	 * value, read from the camera rather than left blank. */
	if (Number(sliderIn(rowFor("brightness")).value) !== 50)
		fail("a knob on auto drew " + sliderIn(rowFor("brightness")).value +
		     " instead of the value the camera reports");

	autoBtn("contrast").handlers.click[0]();
	await settle();
	let set = sent[sent.length - 1];
	if (set.cmd !== "set" || set.edits[0].key !== "contrast" || set.edits[0].value !== "auto")
		fail("pressing auto sent " + JSON.stringify(set) + ", not the word auto");
	if (p.V["image.contrast"] !== "auto")
		fail("the page did not keep the camera's answer of auto");

	/* Pressing it again takes the knob back, pinned where the tuning had
	 * it -- a number, because that is what leaving auto means. */
	autoBtn("brightness").handlers.click[0]();
	await settle();
	set = sent[sent.length - 1];
	if (set.edits[0].key !== "brightness" || set.edits[0].value !== 50)
		fail("releasing auto sent " + JSON.stringify(set.edits[0]) +
		     ", not the value the knob was sitting at");

	/*
	 * And a number past the end of the camera's range never leaves the
	 * page. Sent, it would be refused by the ISP, fall back to the file
	 * and stage a pipeline restart to enact something the silicon cannot
	 * do -- 200 on a knob whose ceiling is 127.
	 */
	const shp = rowFor("sharpness").querySelectorAll("input").find(i => i.type === "number");
	shp.value = 200;
	shp.handlers.change[0]();
	await settle();
	set = sent[sent.length - 1];
	if (set.edits[0].key !== "sharpness" || set.edits[0].value !== 127)
		fail("sharpness 200 was sent as " + JSON.stringify(set.edits[0].value) +
		     ", where the camera's ceiling is 127");

	console.log("ok  " + p.TABS.length + " tabs, " + drawn + " groups, " + undos +
		    " reset controls, " + staged + " redrawn with a reset staged, " +
		    "day/night override wired, image knobs on the camera's own " +
		    "ranges, " + served + " requests served");
})();

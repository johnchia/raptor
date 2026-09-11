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
		/* A real input always has a string here, and a page reading
		 * .length off it is entitled to assume so. */
		this.value = "";
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
	/*
	 * Depth-first walk over ".class", "tag", and "tag[attr=value]" -- the
	 * last because the page finds a slider by input[type=range], and a
	 * shim that matched nothing there answered "no such control" to a
	 * lookup the real DOM always satisfies.
	 */
	querySelectorAll(sel) {
		const want = sel.replace(/^\./, "");
		const byClass = sel.startsWith(".");
		const attr = /^([a-z]+)\[([a-z]+)=([a-z]+)\]$/i.exec(sel);
		const hit = c => byClass ? c.classes.has(want)
			     : attr ? c.tagName === attr[1].toUpperCase() &&
				      String(c[attr[2]]) === attr[3]
				    : c.tagName === sel.toUpperCase();
		const out = [];
		const walk = e => e.children.forEach(c => {
			if (c instanceof El) {
				if (hit(c))
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

/* Whether this camera has been claimed, and what a claim carried. The console
 * run below is a claimed camera -- that is the ordinary case and the one the
 * whole page is about -- and the claim card is exercised afterwards by
 * flipping this and asking the page to draw it. */
let CLAIMABLE = false;
let claimed_with = null;
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
	/*
	 * Readings, not just modes. The sidebar had no fixture at all, so the
	 * status box was drawn on every run and asserted on never.
	 *
	 * 7680 of 61440 kB is an eighth left -- the middle band, so the class
	 * the page picks is neither of the two easy ones. Gain 1080 is 1.05x
	 * to the multiplier and the raw figure the ircut thresholds are
	 * written in, which is the pair the Gain line has to show.
	 */
	if (cmd === "state") return {api: 1, status: "ok", up: {}, daemons_up: 6,
				    uptime: 3 * 3600 + 20 * 60,
				    mem: {total_kb: 61440, avail_kb: 7680,
					  avail_from: "MemAvailable"},
				    stream0: {resolution: "2688x1520", fps: 25},
				    ir: {mode: "auto", state: "day",
					 total_gain: 1080, ae_luma: 47},
				    image: ISP_STATE};
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
		/* Ten bits, as HiSilicon publishes it -- wider than the span a
		 * slider is drawn for, and still a knob with an auto. */
		drc_strength: {min: 0, max: 1023, neutral: 418, auto: true, enabled: true},
		defog_strength: {min: 0, max: 255, neutral: 128, auto: true, enabled: true},
	},
};

/*
 * Half of every section is configured and half is not, so both branches of the
 * reset control are drawn on every tab rather than whichever the bench camera
 * happened to have.
 */
/*
 * The overlay as a camera in the field has it: elements named by whoever set
 * the camera up, two drawn in places of their own, one switched off, and one
 * with no text in it. Each of the four differs in what a place dropdown may
 * offer, and a uniform fixture -- every element drawn, all of them top_left --
 * is the one arrangement that hides that.
 */
const OSD_ELEMENTS = {
	"osd.timestamp": {template: "%time%", position: "top_left", align: "left", visible: true},
	"osd.uptime": {template: "%uptime%", position: "top_right", align: "right", visible: true},
	"osd.camera": {template: "Camera", position: "bottom_left", align: "left", visible: false},
	"osd.blank": {position: "center", align: "left", visible: true},
};

function valuesFor(section) {
	const out = [];

	/*
	 * The pattern stands for every element the config has, so one request
	 * answers with the list and its values together -- and each value
	 * under the name of the section it came from, not the pattern's.
	 */
	if (section === "osd.*") {
		Object.keys(OSD_ELEMENTS).forEach(sec => {
			SCHEMA.repeat_keys.forEach(k => {
				const o = {section: sec, key: k.key};
				const e = OSD_ELEMENTS[sec];

				if (e[k.key] === undefined) o.set = false;
				else o.value = e[k.key];
				out.push(o);
			});
		});
		return out;
	}

	SCHEMA.keys.filter(k => k.section === section).forEach((k, i) => {
		const o = {section: k.section, key: k.key};
		if (k.type === "credential") { o.set = true; out.push(o); return; }
		/* A secret backed by a store: never its value, and the one bit
		 * that says whether a form should offer "set" or "change". */
		if (k.type === "password") { o.configured = false; out.push(o); return; }
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
		/*
		 * The overlay's font size is the one key that may be written
		 * on either of two scales, and this camera has it on the
		 * second -- which is the reading a page drawing only the pixel
		 * track puts somewhere the camera never said.
		 */
		if (k.section === "osd" && k.key === "font_size") {
			o.value = "3.6%";
			o.source = "daemon";
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
		/* The claim route is not rcd's envelope and is answered here
		 * rather than by reply(): the page asks it before anything
		 * else, without a credential, and what it answers decides
		 * whether there is a console to draw at all. CLAIMABLE is set
		 * per run below. */
		if (String(url).indexOf("/api/v1/claim") === 0) {
			const st = {status: "ok", claimed: !CLAIMABLE, claimable: CLAIMABLE};
			if (opt && opt.method === "POST") claimed_with = JSON.parse(opt.body);
			return {ok: true, status: 200, json: async () => st, text: async () => ""};
		}
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
  resetsLive: resetsLive, commitReset: commitReset,
  RESET: RESET, dirty: dirty, UNSET: UNSET,
  setActive: function (v) { active = v; },
  ACT_STATE: ACT_STATE,
  V: V,
  claimGate: claimGate, drawClaim: drawClaim,
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
	 * A reset its owner can enact is sent, not staged.
	 *
	 * The two are told apart by rcd, per key, and getting it wrong is not a
	 * cosmetic difference: staging one makes the operator confirm "stops
	 * capture" for a reset that restarts nothing, and then leaves the knob
	 * alone until an Apply that has nothing to apply.
	 */
	let live_reset = 0, staged_reset = 0;
	{
		/* Driven through the button rather than by calling the sender,
		 * because which of the two a click reaches is the thing under
		 * test. */
		/* The row can be on any tab, so the tab is found rather than
		 * assumed -- and a key with no reset control drawn is a
		 * different failure than one whose button does the wrong
		 * thing. */
		const undoOf = id => {
			for (const t of p.TABS) {
				p.setActive(t.id);
				p.render();
				const row = sheet.querySelectorAll(".row")
						 .find(r => r.dataset.id === id);
				const b = row && row.querySelectorAll(".undo")[0];
				if (b) return b;
			}
			fail("no reset control drawn for " + id);
		};
		const press = async id => {
			const b = undoOf(id);
			for (const fn of (b.handlers.click || [])) await fn();
			await settle();
		};

		/* contrast, not brightness: the fixture leaves brightness unset,
		 * so "the value was cleared" would pass without clearing
		 * anything. */
		const isp = "image.contrast";

		if (!p.resetsLive(isp))
			fail(isp + " should reset live -- is resets_live reaching the page?");

		/* Whatever the fixture offers that cannot be put back live: the
		 * contrast is the point, not the particular key. */
		let orient = null;
		p.TABS.forEach(t => {
			p.setActive(t.id);
			p.render();
			sheet.querySelectorAll(".row").forEach(r => {
				const id = r.dataset.id;
				if (orient || !id || p.resetsLive(id)) return;
				if (r.querySelectorAll(".undo").length) orient = id;
			});
		});
		if (!orient) fail("no staged-reset key in the fixture to contrast with");

		const before = served;
		await press(isp);
		if (served === before)
			fail("resetting " + isp + " sent nothing");
		if (p.RESET.has(isp) || p.dirty.has(isp))
			fail("resetting " + isp + " staged it as well as sending it");
		if (!p.UNSET.has(isp))
			fail("after a live reset " + isp + " should read as unset");
		if (p.V[isp] !== undefined)
			fail("after a live reset " + isp + " kept its old value");
		live_reset++;

		/* And the other kind still waits for Apply, sending nothing now. */
		const before2 = served;
		await press(orient);
		if (served !== before2)
			fail("staging " + orient + " sent a request it should have held");
		if (!p.RESET.has(orient))
			fail(orient + " was not staged");
		p.toggleReset(orient, false);
		staged_reset++;
	}

	/*
	 * And when the owner cannot be asked -- rvd stopped, or the command
	 * refused -- rcd falls back to the file and says so with the same
	 * `applied` a set uses. The page has to follow it there, because that
	 * is the case where the restart really is owed.
	 */
	{
		const isp = "image.brightness";

		refuseNext = 65;
		await p.commitReset(isp);
		await settle();
		refuseNext = null;

		if (!p.RESET.has(isp) || !p.dirty.has(isp))
			fail("a refused live reset must fall back to staging");
		p.toggleReset(isp, false);
	}

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
	/* And the offer does not depend on how wide the range happens to be.
	 * DRC is ten bits here, which is over the span the slider is drawn
	 * for; the number widget the page would otherwise fall to has no
	 * button, so the knob would lose its auto for having more resolution. */
	if (!autoBtn("drc_strength"))
		fail("drc is ten bits and the page dropped its auto with the slider");
	if (!sliderIn(rowFor("drc_strength")))
		fail("drc has an auto to offer and the page did not draw it as a slider");

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

	/*
	 * The status sidebar. Every line here is a reading an operator uses to
	 * answer a question the page cannot answer for them -- why the camera
	 * has not gone to night, whether the leak they are chasing is real --
	 * so the assertions are about what each line has to carry, not about
	 * its wording.
	 */
	const stats = nodes.status.querySelectorAll(".stat");
	const statAt = name => stats.find(d => d.querySelector(".k").textContent === name);
	const statVal = name => (statAt(name) ? statAt(name).querySelector(".v").textContent : null);

	const mem = statAt("Memory");
	if (!mem) fail("the sidebar drew no memory line");
	/* Before uptime, where it was asked for: a monitor read at a glance
	 * wants to be with the other health lines, not after the inventory. */
	if (stats.indexOf(mem) > stats.indexOf(statAt("Uptime")))
		fail("the memory line was drawn after uptime, not before it");
	if (!/7\.5 MB free \(13%\)/.test(statVal("Memory")))
		fail("memory read " + JSON.stringify(statVal("Memory")) +
		     ", not the free figure and the share of the total");
	/* An eighth left is the middle band. Drawing it green would be the
	 * failure that matters -- a monitor that only ever reads good. */
	const memCls = mem.querySelector(".v").className;
	if (!/\bbad\b/.test(memCls))
		fail("an eighth of memory left was drawn as " + JSON.stringify(memCls));

	/* The multiplier is what a person reads; the raw figure is what the
	 * ircut thresholds are written in, so a page showing only one of them
	 * makes the operator convert in their head to use the other tab. */
	if (statVal("Gain") !== "1.05\u00d7 (1080)")
		fail("gain read " + JSON.stringify(statVal("Gain")) +
		     ", not the multiplier and the raw figure together");

	if (statVal("AE luma") !== "47")
		fail("the sidebar drew no AE luma, which is half of the night rule");

	/*
	 * Rotation is four degrees, not a range: a labelled integer cannot
	 * name 0, 90, 180 and 270, so the schema sends it as an enum and the
	 * page has to draw the four rather than a slider from 0 to 270.
	 */
	p.setActive("image");
	p.render();
	await settle();
	const rot = rowFor("rotate");
	if (!rot) fail("the image tab drew no rotation row");
	if (sliderIn(rot)) fail("rotation was drawn as a slider over its degrees");
	if (rot.querySelectorAll(".seg").length !== 1)
		fail("rotation has four settings and the page did not draw them as one choice");
	if (!/270/.test(rot.textContent))
		fail("the rotation control did not offer every angle the camera takes");

	/*
	 * The overlay's font size: one setting on two scales.
	 *
	 * A size in pixels is a size on one picture, so the same key also
	 * takes a percentage of the height -- and the camera here is
	 * configured with one. The row has to come up on that scale: a slider
	 * running 8 to 96 cannot show 3.6, and the position it would land on
	 * instead is one nobody chose.
	 */
	p.setActive("overlay");
	p.render();
	await settle();
	const fs = sheet.querySelectorAll(".row").find(r => r.dataset.id === "osd.font_size");
	if (!fs) fail("the overlay tab drew no font size row");
	const unit = fs.querySelectorAll("button").find(b => /px|%/.test(b.textContent));
	if (!unit) fail("the font size row offered no choice of unit");
	if (unit.getAttribute("aria-pressed") !== "true")
		fail("the camera's size is a percentage and the row drew it as pixels");
	const fsl = sliderIn(fs);
	if (Number(fsl.max) !== 10 || Number(fsl.value) !== 3.6)
		fail("the percentage scale drew " + fsl.value + " of " + fsl.max +
		     ", not the camera's own value on its own range");

	/*
	 * And switching units keeps the text the size it is now, converted
	 * through the picture the camera says it is encoding -- 1520 lines
	 * here, which is the reading and not the configured 1080 that
	 * [stream0] does not carry. A conversion through the wrong height is
	 * the whole failure this scale exists to avoid.
	 */
	unit.handlers.click[0]();
	await settle();
	/* Staged rather than sent: the overlay reads its size when it starts,
	   so this key is on the restart tier and waits for Apply. */
	if (p.V["osd.font_size"] !== 55)
		fail("switching to pixels staged " + JSON.stringify(p.V["osd.font_size"]) +
		     ", not 3.6% of the 1520 lines the camera is encoding");
	if (Number(sliderIn(fs).max) !== 96)
		fail("the row stayed on the percentage scale after switching to pixels");

	unit.handlers.click[0]();
	await settle();
	if (p.V["osd.font_size"] !== "3.6%")
		fail("switching back staged " + JSON.stringify(p.V["osd.font_size"]) +
		     ", not a percentage of the picture");

	/*
	 * One element to a place.
	 *
	 * The camera draws one element in a place, so a dropdown offering a
	 * place another element is drawn in is offering a choice that ends in
	 * an element nobody can see -- which is how the clock disappeared off
	 * the camera this came from. timestamp is drawn top_left and uptime
	 * top_right; camera is switched off, which holds nothing, and blank
	 * has no text in it, which holds nothing either.
	 */
	const placeSel = name => {
		const row = sheet.querySelectorAll(".row")
				 .find(r => r.dataset.id === "osd." + name + ".position");
		if (!row) fail("the overlay tab drew no position row for " + name);
		return row.querySelectorAll("select")[0];
	};
	const optFor = (sel, place) => sel.querySelectorAll("option")
					  .find(o => o.value === place);

	const up = placeSel("uptime");
	if (!optFor(up, "top_left").disabled)
		fail("uptime was offered top_left, where timestamp is drawn");
	if (!/timestamp/.test(optFor(up, "top_left").textContent))
		fail("the place timestamp holds was disabled without saying whose it is");
	if (optFor(up, "top_right").disabled)
		fail("uptime could not choose the place it is already drawn in");
	if (optFor(up, "bottom_left").disabled)
		fail("uptime was refused bottom_left, which only a hidden element names");
	if (optFor(up, "center").disabled)
		fail("uptime was refused a place nothing is drawn in");

	const blank = placeSel("blank");
	if (!optFor(blank, "top_left").disabled || !optFor(blank, "top_right").disabled)
		fail("an element with no text was offered places that are already drawn in");

	/*
	 * And it follows the page. Switching timestamp off gives its place up,
	 * which uptime may then take -- without a reload, because the operator
	 * making room is the operator about to use it.
	 */
	const visStamp = sheet.querySelectorAll(".row")
			      .find(r => r.dataset.id === "osd.timestamp.visible")
			      .querySelectorAll("input")[0];
	visStamp.checked = false;
	visStamp.handlers.change[0]();
	await settle();
	if (optFor(placeSel("uptime"), "top_left").disabled)
		fail("timestamp was switched off and still held its place");

	/*
	 * A camera whose config already puts two elements in one place -- hand
	 * written, or written by a page that did not know better -- still has
	 * to show each of them where it is. A field that offered uptime every
	 * place except the one it is drawn in would make moving the other
	 * element the only way to see its own setting.
	 */
	p.V["osd.timestamp.visible"] = true;
	p.V["osd.uptime.position"] = "top_left";
	p.render();
	await settle();
	if (optFor(placeSel("timestamp"), "top_left").disabled ||
	    optFor(placeSel("uptime"), "top_left").disabled)
		fail("an element already drawn in a place was not shown that place");
	if (!optFor(placeSel("blank"), "top_left").disabled)
		fail("an element with no text was offered a place two elements are in");

	/*
	 * The elements are the camera's own, named as its config names them
	 * and listed in the order the file lists them -- which is the order
	 * rod settles two of them wanting one place in, so a page showing any
	 * other order would be describing a different camera.
	 */
	/* The heading carries the group's own controls too, so the name is
	   taken from the front of it rather than the whole. */
	const names = sheet.querySelectorAll("h3").map(h => h.textContent)
			   .map(t => (t.match(/^(timestamp|uptime|camera|blank)/) || [])[1])
			   .filter(Boolean);
	if (names.join(" ") !== "timestamp uptime camera blank")
		fail("the overlay listed its elements as " + JSON.stringify(names.join(" ")));
	/* And the pattern is not one of them: it describes the shape of an
	   element, and a page that rendered it would offer a form headed
	   "osd.*" and write whatever was typed into a section nothing draws. */
	if (p.getKeys().some(k => k.section === "osd.*"))
		fail("the pattern was taken for a section and given keys of its own");

	/*
	 * The password key is a settings row like any other and must be drawn
	 * as one -- typed twice, committed by a button. A password that saved
	 * as you left the field is one you had a single chance to type right.
	 */
	p.setActive("system");
	p.render();
	await settle();
	const pw = sheet.querySelectorAll(".row").find(r => r.dataset.id === "device.root_password");
	if (!pw) fail("the system tab drew no row for the root password");
	const pwFields = pw.querySelectorAll("input").filter(i => i.type === "password");
	if (pwFields.length !== 2)
		fail("the password row drew " + pwFields.length + " password fields, not two");
	const pwGo = pw.querySelectorAll("button")[0];
	if (!pwGo || !pwGo.disabled)
		fail("the password row offered its button before anything had been typed");
	pwFields[0].value = "a good long password";
	pwFields[1].value = "a good long passwerd";
	pwFields[0].handlers.input[0]();
	if (!pwGo.disabled) fail("the password row accepted two fields that do not match");
	pwFields[1].value = "a good long password";
	pwFields[1].handlers.input[0]();
	if (pwGo.disabled) fail("the password row refused two fields that do match");

	/*
	 * And the gate in front of all of it. A camera nobody has claimed has
	 * no credential to authenticate the console with, so the page must ask
	 * before it asks for anything else and draw a way in rather than a
	 * fault.
	 */
	CLAIMABLE = true;
	if (await p.claimGate() !== true)
		fail("the page did not notice a camera that has not been claimed");
	const card = sheet.querySelectorAll(".banner")[0];
	if (!card) fail("the claim gate drew no card");
	if (!/claim/i.test(card.textContent)) fail("the claim card does not say what it is for");
	const fields = sheet.querySelectorAll("input").filter(i => i.type === "password");
	if (fields.length !== 2)
		fail("the claim card drew " + fields.length + " password fields, not two");
	const go = sheet.querySelectorAll("button")[0];
	if (!go || !go.disabled) fail("the claim card offered its button before anything was typed");

	fields[0].value = "short";
	fields[1].value = "short";
	fields[0].handlers.input[0]();
	if (!go.disabled) fail("the claim card accepted a password shorter than rcd's minimum");

	fields[0].value = fields[1].value = "a good long password";
	fields[1].handlers.input[0]();
	if (go.disabled) fail("the claim card refused a password it should have taken");

	await go.handlers.click[0]();
	await settle();
	if (!claimed_with || claimed_with.password !== "a good long password")
		fail("the claim card did not send the password to the claim route");
	if (!/claimed/i.test(sheet.textContent))
		fail("the claim card did not say the camera had been claimed");

	console.log("ok  " + p.TABS.length + " tabs, " + drawn + " groups, " + undos +
		    " reset controls, " + staged + " redrawn with a reset staged, " +
		    "day/night override wired, image knobs on the camera's own " +
		    "ranges, " + live_reset + " reset live and " + staged_reset +
		    " staged, " + served + " requests served, claim card drawn");
})();

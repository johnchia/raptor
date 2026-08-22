/*
 * portal_smoke.js -- Run the setup page without a browser.
 *
 * Two properties are worth this much machinery, and neither can be checked by
 * reading the file.
 *
 * The passphrase must hash to what the access point hashed it to. The page
 * carries its own PBKDF2-HMAC-SHA1 because crypto.subtle does not exist
 * outside a secure context and the portal deliberately is not one, so the
 * derivation is ours to get right -- and getting it wrong produces a camera
 * that reports the passphrase as wrong, which is indistinguishable from the
 * operator having mistyped it. The three vectors below are 802.11i's own.
 *
 * And the plaintext must not leave the device. That is the entire reason for
 * deriving here, so it is asserted against the bytes actually sent rather
 * than against the code that builds them.
 *
 *   node tests/portal_smoke.js [path/to/portal.html]
 */
"use strict";

const fs = require("fs");
const path = require("path");
const vm = require("vm");

const page = process.argv[2] || path.join(__dirname, "..", "rhd", "portal.html");
const html = fs.readFileSync(page, "utf8");

function fail(msg) {
	console.error("FAIL " + msg);
	process.exit(1);
}

const script = [...html.matchAll(/<script[^>]*>([\s\S]*?)<\/script>/g)].map((m) => m[1]).join("\n");
if (!script.trim()) fail("no <script> found in " + page);

process.on("uncaughtException", (e) => fail("the page threw: " + ((e && e.stack) || e)));
process.on("unhandledRejection", (e) =>
	fail("the page left a promise rejected: " + ((e && e.stack) || e)));

/* ── enough of a DOM for one form ──
 * Smaller than the console's shim on purpose: this page builds no tree worth
 * walking, and the only container that matters is a <select> whose options
 * have to behave like options. */

class El {
	constructor(tag) {
		this.tagName = String(tag).toUpperCase();
		this.options = [];
		this.dataset = {};
		this.handlers = {};
		this.value = "";
		this.textContent = "";
		this.placeholder = "";
		this.type = "text";
		this.disabled = false;
		this.selected = false;
		const classes = new Set();
		this.classes = classes;
		this.classList = {
			add: (...c) => c.forEach((x) => classes.add(x)),
			remove: (...c) => c.forEach((x) => classes.delete(x)),
			toggle: (c, on) =>
				on === undefined
					? classes.has(c) ? classes.delete(c) : classes.add(c)
					: on ? classes.add(c) : classes.delete(c),
			contains: (c) => classes.has(c),
		};
	}
	set innerHTML(v) {
		if (v === "") this.options = [];
	}
	get innerHTML() {
		return "";
	}
	get selectedOptions() {
		const hit = this.options.find((o) => o.value === this.value);
		return hit ? [hit] : [];
	}
	appendChild(n) {
		this.options.push(n);
		return n;
	}
	addEventListener(ev, fn) {
		(this.handlers[ev] = this.handlers[ev] || []).push(fn);
	}
	async fire(ev, arg) {
		for (const fn of this.handlers[ev] || []) await fn(arg || {preventDefault() {}});
	}
}

function Option(text, value) {
	const o = new El("option");
	o.textContent = String(text);
	o.value = value === undefined ? String(text) : String(value);
	return o;
}

const nodes = {};
const document = {
	createElement: (t) => new El(t),
	getElementById: (id) => (nodes[id] = nodes[id] || new El("div")),
	addEventListener: () => {},
};
/* The page reaches for its fields through a one-character helper, so there
 * are no getElementById literals to scan for -- every node is created on
 * first ask. Only the two that have to behave like a <select> are named. */
document.getElementById("ssid").tagName = "SELECT";
document.getElementById("tz").tagName = "SELECT";

/* ── the camera, as far as the page can tell ── */

const sent = [];
let scanReply = {
	api: 1,
	status: "ok",
	networks: [
		{ssid: "Hedgerow", signal: -48, secure: true},
		{ssid: "Hedgerow Guest", signal: -61, secure: false},
		{ssid: "café ☕", signal: -77, secure: true},
	],
};

function reply(body) {
	sent.push(body);
	if (body.cmd === "action" && body.action === "wifi-scan") return scanReply;
	if (body.cmd === "get")
		return {api: 1, status: "ok", section: "system", key: "hostname", value: "raptor-1a2b"};
	if (body.cmd === "schema")
		return {
			api: 1,
			status: "ok",
			keys: [{section: "system", key: "timezone", type: "enum",
				choices: ["UTC", "Europe/London", "America/New_York"]}],
		};
	return {api: 1, status: "ok"};
}

/*
 * Timers: a zero delay runs, anything longer does not.
 *
 * The page yields with setTimeout(0) so the button can repaint before it
 * spends a few tens of milliseconds hashing, and it re-arms the scan poll at
 * 2500 ms. Honouring the first and dropping the second is what lets this
 * finish rather than polling a camera that is not there.
 */
const sandbox = {
	document,
	console,
	window: {addEventListener: () => {}, scrollTo: () => {}},
	location: {protocol: "http:", host: "172.16.0.1", hostname: "172.16.0.1"},
	navigator: {userAgent: "smoke"},
	Option,
	TextEncoder,
	setTimeout: (fn, ms) => {
		if (!ms) queueMicrotask(fn);
		return 0;
	},
	clearTimeout: () => {},
	fetch: async (url, opt) => {
		const body = opt && opt.body ? JSON.parse(opt.body) : {};
		sandbox.__lastBody = opt ? opt.body : "";
		return {ok: true, status: 200, json: async () => reply(body)};
	},
	JSON, Math, Date, Set, Map, Promise, Array, Uint8Array, Int32Array, DataView,
	ArrayBuffer, Error, parseInt, parseFloat, isNaN, String, Number, Object,
};
sandbox.globalThis = sandbox;
sandbox.window.document = document;

/* The page's own names are lexical, so an epilogue in the same scope is the
 * only way to reach them. */
const epilogue = `
;globalThis.__probe = {derive: derive, scan: scan, api: api};
`;

vm.createContext(sandbox);
try {
	vm.runInContext(script + epilogue, sandbox, {filename: path.basename(page)});
} catch (e) {
	fail("the page did not load: " + ((e && e.stack) || e));
}

const probe = sandbox.__probe;
if (!probe || typeof probe.derive !== "function") fail("the page exposed no derive()");

/* ── checks ── */

/* Every field, whether or not the page has asked for it yet: the page reaches
 * for most of them only from inside a handler. */
const el = (id) => document.getElementById(id);

let checks = 0;
function ok(what, cond) {
	checks++;
	if (!cond) fail(what);
}
function eq(what, got, want) {
	checks++;
	if (got !== want) fail(what + "\n  got:  " + got + "\n  want: " + want);
}

/*
 * The first two are IEEE 802.11i-2004 Annex H.4, which is also what
 * wpa_passphrase(8) prints. The rest come from a reference PBKDF2 --
 * `hashlib.pbkdf2_hmac("sha1", ...)` -- because what matters is agreeing with
 * every other implementation, not only with the two cases the standard chose
 * to print.
 */
eq("PSK for 'password' on 'IEEE'", probe.derive("password", "IEEE"),
   "f42c6fc52df0ebef9ebb4b90b38a5f902e83fe1b135a70e23aed762e9710a12e");
eq("PSK for 'ThisIsAPassword' on 'ThisIsASSID'",
   probe.derive("ThisIsAPassword", "ThisIsASSID"),
   "0dc0d6eb90555ed6419756b9a15ec3e3209b63df707dd508d14581f8982721af");

/* Both fields at their limit: 64 characters is the longest passphrase the
 * form takes, and 32 the longest SSID there is. */
eq("PSK for 64 a's on 32 Z's",
   probe.derive("a".repeat(64), "Z".repeat(32)),
   "4fd16ee24bd1d8f9e7ebd86cbd802d0b3acfd23cb08de414da4e1690e474b857");

/*
 * And a name outside ASCII, which is the one thing a JavaScript
 * implementation gets wrong for free: strings are UTF-16, and hashing them a
 * code unit at a time produces a key that is wrong on exactly the networks
 * whose owners did not name them in English.
 */
eq("PSK for a UTF-8 SSID", probe.derive("secret12", "café"),
   "01162f546f6cfb1f2290a24bf2d31e2aaeb7d0d1715eb4e52ce193bd98320661");
ok("and it differs from its ASCII neighbour",
   probe.derive("secret12", "café") !== probe.derive("secret12", "cafe"));

async function main() {
	/* The page kicked a scan on load; let it land. */
	await new Promise((r) => setImmediate(r));

	const sel = el("ssid");
	eq("every network is offered, plus the unlisted option", sel.options.length, 4);
	eq("strongest first", sel.options[0].value, "Hedgerow");
	ok("the open one is marked open", sel.options[1].dataset.secure === "0");
	ok("an unlisted network can still be typed",
	   sel.options[3].value.indexOf("other") >= 0);

	/* Nothing in the page may be a control byte: it is served as HTML and
	 * read by things that are not browsers. The sentinel above is the one
	 * place that could have introduced one. */
	ok("the page has no NUL in it", html.indexOf("\u0000") < 0);

	/* -- a passphrase that is too short never reaches the camera -- */
	sel.value = "Hedgerow";
	el("psk").value = "short";
	const before = sent.length;
	await el("form").fire("submit");
	eq("a short passphrase is refused here", sent.length, before);
	ok("and it says so", el("err").textContent.indexOf("8 to 63") >= 0);

	/* -- the real thing -- */
	const secret = "correct horse battery staple";
	el("psk").value = secret;
	el("name").value = "front-door";
	el("tz").value = "Europe/London";
	await el("form").fire("submit");

	const set = sent.find((r) => r.cmd === "set");
	ok("the credentials were stored", !!set);
	const psk = set.edits.find((e) => e.key === "psk");
	const ssid = set.edits.find((e) => e.key === "ssid");
	eq("the network name is sent as typed", ssid.value, "Hedgerow");
	ok("the passphrase is sent pre-derived", /^[0-9a-f]{64}$/.test(psk.value));
	eq("and it is the right one", psk.value, probe.derive(secret, "Hedgerow"));

	/* The property the derivation exists for, checked against the bytes
	 * rather than against the code that built them. */
	ok("the plaintext passphrase is nowhere in the request",
	   JSON.stringify(sent).indexOf(secret) < 0);

	ok("the name came along", set.edits.some((e) => e.key === "hostname" && e.value === "front-door"));
	ok("so did the zone",
	   set.edits.some((e) => e.key === "timezone" && e.value === "Europe/London"));

	ok("and the apply was sent", sent.some((r) => r.cmd === "apply"));
	ok("the form gave way to the outcome", el("form").classList.contains("hidden"));
	ok("which names the network", el("donessid").textContent === "Hedgerow");

	/* -- an open network -- */
	sent.length = 0;
	el("done").classList.add("hidden");
	el("form").classList.remove("hidden");
	sel.value = "Hedgerow Guest";
	el("psk").value = "";
	await el("form").fire("submit");
	const open = sent.find((r) => r.cmd === "set");
	ok("an open network is accepted with no passphrase", !!open);
	eq("and stores an empty one", open.edits.find((e) => e.key === "psk").value, "");

	console.log("portal_smoke: " + checks + " checks passed");
}

main();

/* Scratch helper: render the camera's own web page in headless Chrome (via
   CDP) and screenshot specific panels, so the dock's layout can be compared
   against the real page. Authenticates with the web UI's `account` session
   cookie, which is enough on its own (no per-request Digest needed).
   Usage: node _camshot.mjs <out-dir> */
import { spawn } from "node:child_process";
import fs from "node:fs";
import path from "node:path";

const CHROME = "C:/Program Files/Google/Chrome/Application/chrome.exe";
const PORT = 9333;
const COOKIE = "account=JrEkAUU^=iD_q^lz";
const HOST = "192.168.10.121";
const OUT = process.argv[2] || ".";
const PROFILE = path.join(process.env.TEMP || ".", "cdpprof-zc");

const chrome = spawn(
	CHROME,
	[
		"--headless=new",
		"--disable-gpu",
		"--no-first-run",
		"--no-default-browser-check",
		"--ignore-certificate-errors",
		"--disable-features=OptimizationGuideModelDownloading",
		`--remote-debugging-port=${PORT}`,
		`--user-data-dir=${PROFILE}`,
		"--window-size=1800,1100",
		"about:blank",
	],
	{ stdio: "ignore" }
);

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

async function devtoolsUrl() {
	for (let i = 0; i < 60; i++) {
		try {
			const r = await fetch(`http://127.0.0.1:${PORT}/json/version`);
			const j = await r.json();
			return j.webSocketDebuggerUrl;
		} catch {
			await sleep(250);
		}
	}
	throw new Error("devtools never came up");
}

class Cdp {
	constructor(url) {
		this.ws = new WebSocket(url);
		this.id = 0;
		this.pending = new Map();
		this.events = [];
		this.ws.addEventListener("message", (e) => {
			const msg = JSON.parse(e.data);
			if (msg.id && this.pending.has(msg.id)) {
				const { resolve, reject } = this.pending.get(msg.id);
				this.pending.delete(msg.id);
				msg.error ? reject(new Error(JSON.stringify(msg.error))) : resolve(msg.result);
			} else if (msg.method) {
				this.events.push(msg);
			}
		});
	}
	ready() {
		return new Promise((res, rej) => {
			this.ws.addEventListener("open", res, { once: true });
			this.ws.addEventListener("error", rej, { once: true });
		});
	}
	send(method, params = {}, sessionId) {
		const id = ++this.id;
		return new Promise((resolve, reject) => {
			this.pending.set(id, { resolve, reject });
			this.ws.send(JSON.stringify({ id, method, params, sessionId }));
		});
	}
}

async function main() {
	const wsUrl = await devtoolsUrl();
	const cdp = new Cdp(wsUrl);
	await cdp.ready();

	// Attach to the first page target.
	let targetId = null;
	for (let i = 0; i < 40 && !targetId; i++) {
		const list = await (await fetch(`http://127.0.0.1:${PORT}/json/list`)).json();
		const page = list.find((t) => t.type === "page");
		if (page) targetId = page.id;
		else await sleep(200);
	}
	if (!targetId) throw new Error("no page target");
	const { sessionId } = await cdp.send("Target.attachToTarget", {
		targetId,
		flatten: true,
	});
	const S = (m, p) => cdp.send(m, p, sessionId);

	await S("Page.enable");
	await S("Network.enable");
	await S("Runtime.enable");
	await S("Emulation.setDeviceMetricsOverride", {
		width: 1800,
		height: 1100,
		deviceScaleFactor: 2,
		mobile: false,
	});
	await S("Network.setCookie", {
		name: "account",
		value: COOKIE.slice("account=".length),
		url: `https://${HOST}/`,
		path: "/",
		secure: true,
	});
	await S("Page.navigate", { url: `https://${HOST}/www/index.html` });
	await sleep(6000);

	// Reveal the auto-framing panel (the page hides it until its own logic
	// decides the camera supports it; jQuery is loaded by then).
	const reveal = await S("Runtime.evaluate", {
		expression: `(() => {
			const out = {jq: typeof $, tabs: !!document.querySelector('#tabs-li-aframe'),
				af: !!document.querySelector('#auto_frame'),
				afHtml: (document.querySelector('#auto_frame')||{outerHTML:''}).outerHTML.length,
				ctrl: !!document.querySelector('#aframe_start_pos'),
				ids: [...document.querySelectorAll('#auto_frame [id]')].map(e=>e.id)};
			try {
				if (window.$) {
					$('#main_ui').show();
					$('#tabs-li-aframe').show();
					$('#auto_frame').show();
					$('#tabs-live').show();
					$('.aframe_tips_container').show();
				}
			} catch (e) { out.err = String(e); }
			return JSON.stringify(out);
		})()`,
		returnByValue: true,
	});
	console.log("reveal:", reveal.result.value);
	await sleep(1500);

	async function shootElement(sel, name, pad = 10) {
		const box = await S("Runtime.evaluate", {
			expression: `(() => {
				const el = document.querySelector(${JSON.stringify(sel)});
				if (!el) return "null";
				const r = el.getBoundingClientRect();
				return JSON.stringify({x: r.x, y: r.y, w: r.width, h: r.height});
			})()`,
			returnByValue: true,
		});
		if (box.result.value === "null" || !box.result.value) {
			console.log(`skip ${name} (${sel} not found)`);
			return;
		}
		const r = JSON.parse(box.result.value);
		const res = await S("Page.captureScreenshot", {
			format: "png",
			captureBeyondViewport: true,
			clip: {
				x: Math.max(0, r.x - pad),
				y: Math.max(0, r.y - pad),
				width: Math.max(20, r.w + pad * 2),
				height: Math.max(20, r.h + pad * 2),
				scale: 2,
			},
		});
		fs.writeFileSync(path.join(OUT, name), Buffer.from(res.data, "base64"));
		console.log(`wrote ${name} ${Math.round(r.w)}x${Math.round(r.h)}`);
	}

	await shootElement("#auto_frame", "web_framing.png");
	await shootElement(".aframe_ctrl", "web_framing_ctrl.png");
	// The whole live tab, to see the pad/canvas against the control strip.
	await shootElement("#tabs-live", "web_live.png");

	// And the PTZ page, for the joystick + zoom arrangement.
	await S("Runtime.evaluate", {
		expression: `(() => {
			if (!window.$) return "nojq";
			const a = document.querySelector('a[href="#tabs-settings"]');
			if (a) a.click();
			const b = document.querySelector('a[href="#tabs-settings-pt"]');
			if (b) b.click();
			return "ok";
		})()`,
		returnByValue: true,
	});
	await sleep(1200);
	await shootElement("#tabs-settings", "web_settings.png");
	const ids = await S("Runtime.evaluate", {
		expression: `JSON.stringify([...document.querySelectorAll('[id]')].map(e=>e.id).filter(i=>/pt|joy|zoom|speed|ctrl/i.test(i)).slice(0,80))`,
		returnByValue: true,
	});
	console.log("pt-ish ids:", ids.result.value);

	cdp.ws.close();
	chrome.kill();
}

main().catch((e) => {
	console.error("FAILED:", e);
	try {
		chrome.kill();
	} catch {}
	process.exit(1);
});

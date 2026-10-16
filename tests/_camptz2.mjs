/* Scratch helper: screenshot the camera web UI's Pan Tilt settings page and
   report where its joystick / zoom widgets sit. */
import { spawn } from "node:child_process";
import fs from "node:fs";
import path from "node:path";

const CHROME = "C:/Program Files/Google/Chrome/Application/chrome.exe";
const PORT = 9335;
const HOST = "192.168.10.121";
const OUT = process.argv[2] || ".";
const PROFILE = path.join(process.env.TEMP || ".", "cdpprof-zc3");

const chrome = spawn(CHROME, [
	"--headless=new", "--disable-gpu", "--no-first-run", "--no-default-browser-check",
	"--ignore-certificate-errors", `--remote-debugging-port=${PORT}`,
	`--user-data-dir=${PROFILE}`, "--window-size=1800,1200", "about:blank",
], { stdio: "ignore" });

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

class Cdp {
	constructor(url) {
		this.ws = new WebSocket(url); this.id = 0; this.pending = new Map();
		this.ws.addEventListener("message", (e) => {
			const m = JSON.parse(e.data);
			if (m.id && this.pending.has(m.id)) {
				const { resolve, reject } = this.pending.get(m.id);
				this.pending.delete(m.id);
				m.error ? reject(new Error(JSON.stringify(m.error))) : resolve(m.result);
			}
		});
	}
	ready() { return new Promise((res, rej) => { this.ws.addEventListener("open", res, { once: true }); this.ws.addEventListener("error", rej, { once: true }); }); }
	send(method, params = {}, sessionId) {
		const id = ++this.id;
		return new Promise((resolve, reject) => { this.pending.set(id, { resolve, reject }); this.ws.send(JSON.stringify({ id, method, params, sessionId })); });
	}
}

async function main() {
	let wsUrl = null;
	for (let i = 0; i < 60 && !wsUrl; i++) {
		try { wsUrl = (await (await fetch(`http://127.0.0.1:${PORT}/json/version`)).json()).webSocketDebuggerUrl; }
		catch { await sleep(250); }
	}
	const cdp = new Cdp(wsUrl); await cdp.ready();
	let targetId = null;
	for (let i = 0; i < 40 && !targetId; i++) {
		const list = await (await fetch(`http://127.0.0.1:${PORT}/json/list`)).json();
		const p = list.find((t) => t.type === "page"); if (p) targetId = p.id; else await sleep(200);
	}
	const { sessionId } = await cdp.send("Target.attachToTarget", { targetId, flatten: true });
	const S = (m, p) => cdp.send(m, p, sessionId);
	const ev = async (expr) => (await S("Runtime.evaluate", { expression: expr, returnByValue: true })).result.value;

	await S("Page.enable"); await S("Network.enable"); await S("Runtime.enable");
	await S("Emulation.setDeviceMetricsOverride", { width: 1800, height: 1200, deviceScaleFactor: 2, mobile: false });
	await S("Network.setCookie", { name: "account", value: "JrEkAUU^=iD_q^lz", url: `https://${HOST}/`, path: "/", secure: true });
	await S("Page.navigate", { url: `https://${HOST}/www/index.html` });
	await sleep(6000);

	// Where does the joystick live, and is it visible?
	console.log("joystick geometry:", await ev(`(() => {
		const ids = ['pt-ctrl','pt-ctrl-joystick','pt-ctrl-joystick-bg','pt-ctrl-joystick-center','pt_speed','pt-speed-ctrl','lens_zoom_pos','tab-ctrl-af-zoom-pos','tab-ctrl-focus-zoom-speed'];
		const o = {};
		for (const id of ids) {
			const e = document.getElementById(id);
			if (!e) { o[id] = 'missing'; continue; }
			const r = e.getBoundingClientRect();
			let vis = true, p = e;
			while (p) { const d = getComputedStyle(p).display; if (d === 'none') { vis = false; break; } p = p.parentElement; }
			o[id] = (vis ? 'visible' : 'hidden') + ' ' + Math.round(r.x) + ',' + Math.round(r.y) + ' ' + Math.round(r.width) + 'x' + Math.round(r.height);
		}
		return JSON.stringify(o, null, 1);
	})()`));

	// Open Settings -> Pan Tilt and force the panel visible.
	await ev(`(async () => {
		const a = document.querySelector('a[href="#tabs-settings"]'); if (a) a.click();
		await new Promise(r => setTimeout(r, 800));
		if (window.$) { $('#tabs-settings-pt').show(); $('a[href="#tabs-settings-pt"]').parent().show(); }
		const b = document.querySelector('a[href="#tabs-settings-pt"]'); if (b) b.click();
		await new Promise(r => setTimeout(r, 800));
		if (window.$) $('#tabs-settings-pt').show();
		return 1;
	})()`);
	await sleep(2000);

	const info = await ev(`(() => {
		const el = document.querySelector('#tabs-settings-pt');
		if (!el) return 'none';
		const r = el.getBoundingClientRect();
		return JSON.stringify({x:r.x,y:r.y,w:r.width,h:r.height});
	})()`);
	console.log("pt panel:", info);

	const shoot = async (sel, name, pad = 8) => {
		const box = await ev(`(() => { const el=document.querySelector(${JSON.stringify(sel)}); if(!el) return 'null';
			const r=el.getBoundingClientRect(); return JSON.stringify({x:r.x,y:r.y,w:r.width,h:r.height}); })()`);
		if (!box || box === "null") { console.log("skip", name); return; }
		const r = JSON.parse(box);
		if (r.w < 5 || r.h < 5) { console.log("tiny, skip", name, box); return; }
		const res = await S("Page.captureScreenshot", {
			format: "png", captureBeyondViewport: true,
			clip: { x: Math.max(0, r.x - pad), y: Math.max(0, r.y - pad), width: r.w + pad * 2, height: r.h + pad * 2, scale: 2 },
		});
		fs.writeFileSync(path.join(OUT, name), Buffer.from(res.data, "base64"));
		console.log("wrote", name, Math.round(r.w), "x", Math.round(r.h));
	};

	await shoot("#tabs-settings-pt", "web_pt_page.png");
	await shoot("#pt-ctrl", "web_pt_joystick.png");

	// The zoom widget markup, in full.
	console.log("zoom widget:", await ev(`(() => {
		const a = document.getElementById('tab-ctrl-af-zoom-pos');
		const b = document.getElementById('tab-ctrl-focus-zoom-speed');
		return JSON.stringify({af:(a?a.outerHTML:'').slice(0,1800), fz:(b?b.outerHTML:'').slice(0,900)});
	})()`));

	cdp.ws.close(); chrome.kill();
}

main().catch((e) => { console.error("FAILED:", e); try { chrome.kill(); } catch {} process.exit(1); });

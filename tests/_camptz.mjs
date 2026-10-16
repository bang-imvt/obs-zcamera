/* Scratch helper: dump the camera web UI's PTZ page DOM and screenshot it. */
import { spawn } from "node:child_process";
import fs from "node:fs";
import path from "node:path";

const CHROME = "C:/Program Files/Google/Chrome/Application/chrome.exe";
const PORT = 9334;
const HOST = "192.168.10.121";
const OUT = process.argv[2] || ".";
const PROFILE = path.join(process.env.TEMP || ".", "cdpprof-zc2");

const chrome = spawn(
	CHROME,
	[
		"--headless=new",
		"--disable-gpu",
		"--no-first-run",
		"--no-default-browser-check",
		"--ignore-certificate-errors",
		`--remote-debugging-port=${PORT}`,
		`--user-data-dir=${PROFILE}`,
		"--window-size=1800,1200",
		"about:blank",
	],
	{ stdio: "ignore" }
);

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

class Cdp {
	constructor(url) {
		this.ws = new WebSocket(url);
		this.id = 0;
		this.pending = new Map();
		this.ws.addEventListener("message", (e) => {
			const m = JSON.parse(e.data);
			if (m.id && this.pending.has(m.id)) {
				const { resolve, reject } = this.pending.get(m.id);
				this.pending.delete(m.id);
				m.error ? reject(new Error(JSON.stringify(m.error))) : resolve(m.result);
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
	let wsUrl = null;
	for (let i = 0; i < 60 && !wsUrl; i++) {
		try {
			wsUrl = (await (await fetch(`http://127.0.0.1:${PORT}/json/version`)).json())
				.webSocketDebuggerUrl;
		} catch {
			await sleep(250);
		}
	}
	const cdp = new Cdp(wsUrl);
	await cdp.ready();
	let targetId = null;
	for (let i = 0; i < 40 && !targetId; i++) {
		const list = await (await fetch(`http://127.0.0.1:${PORT}/json/list`)).json();
		const p = list.find((t) => t.type === "page");
		if (p) targetId = p.id;
		else await sleep(200);
	}
	const { sessionId } = await cdp.send("Target.attachToTarget", { targetId, flatten: true });
	const S = (m, p) => cdp.send(m, p, sessionId);

	await S("Page.enable");
	await S("Network.enable");
	await S("Runtime.enable");
	await S("Emulation.setDeviceMetricsOverride", {
		width: 1800, height: 1200, deviceScaleFactor: 2, mobile: false,
	});
	await S("Network.setCookie", {
		name: "account", value: "JrEkAUU^=iD_q^lz", url: `https://${HOST}/`, path: "/", secure: true,
	});
	await S("Page.navigate", { url: `https://${HOST}/www/index.html` });
	await sleep(6000);

	const ev = async (expr) => {
		const r = await S("Runtime.evaluate", { expression: expr, returnByValue: true });
		return r.result.value;
	};

	console.log("settings tabs:", await ev(
		`JSON.stringify([...document.querySelectorAll('#settings_tabs a, #settings_tabs li a, .settings_tab a, a[href^="#tabs-settings-"]')].map(a=>a.getAttribute('href')+'|'+a.textContent.trim()))`
	));
	console.log("pt-ish ids:", await ev(
		`JSON.stringify([...document.querySelectorAll('[id]')].map(e=>e.id).filter(i=>/pt|joy|zoom|speed|focus|ctrl/i.test(i)).slice(0,120))`
	));

	// Open Settings -> the PTZ page.
	await ev(`(() => {
		const a = document.querySelector('a[href="#tabs-settings"]'); if (a) a.click();
		const b = document.querySelector('a[href="#tabs-settings-pt"]'); if (b) b.click();
		return 1;
	})()`);
	await sleep(1500);

	const info = await ev(`(() => {
		const el = document.querySelector('#tabs-settings-pt') || document.querySelector('#tabs-settings');
		if (!el) return "none";
		el.style.display = 'block';
		const r = el.getBoundingClientRect();
		return JSON.stringify({sel: el.id, x: r.x, y: r.y, w: r.width, h: r.height});
	})()`);
	console.log("pt panel:", info);

	if (info && info !== "none") {
		const r = JSON.parse(info);
		const res = await S("Page.captureScreenshot", {
			format: "png", captureBeyondViewport: true,
			clip: { x: Math.max(0, r.x), y: Math.max(0, r.y), width: Math.max(50, r.w), height: Math.max(50, r.h), scale: 2 },
		});
		fs.writeFileSync(path.join(OUT, "web_ptz.png"), Buffer.from(res.data, "base64"));
		console.log("wrote web_ptz.png", Math.round(r.w), Math.round(r.h));
	}

	// The joystick/zoom markup itself, wherever it lives in the Live tab.
	console.log("zoom markup:", await ev(
		`(() => {const e=[...document.querySelectorAll('[id*=zoom],[class*=zoom],[id*=joy],[class*=joy]')].slice(0,12);
		 return JSON.stringify(e.map(x=>({t:x.tagName,id:x.id,c:x.className,h:(x.outerHTML||'').slice(0,240)})));})()`
	));

	cdp.ws.close();
	chrome.kill();
}

main().catch((e) => { console.error("FAILED:", e); try { chrome.kill(); } catch {} process.exit(1); });

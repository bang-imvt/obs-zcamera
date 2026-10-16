/* Scratch helper: screenshot the camera web UI's Auto Framing Settings page. */
import { spawn } from "node:child_process";
import fs from "node:fs";
import path from "node:path";

const CHROME = "C:/Program Files/Google/Chrome/Application/chrome.exe";
const PORT = 9336;
const HOST = "192.168.10.121";
const OUT = process.argv[2] || ".";
const PROFILE = path.join(process.env.TEMP || ".", "cdpprof-zc4");

const chrome = spawn(CHROME, [
	"--headless=new", "--disable-gpu", "--no-first-run", "--no-default-browser-check",
	"--ignore-certificate-errors", `--remote-debugging-port=${PORT}`,
	`--user-data-dir=${PROFILE}`, "--window-size=1800,1400", "about:blank",
], { stdio: "ignore" });
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

class Cdp {
	constructor(url) {
		this.ws = new WebSocket(url); this.id = 0; this.pending = new Map();
		this.ws.addEventListener("message", (e) => {
			const m = JSON.parse(e.data);
			if (m.id && this.pending.has(m.id)) {
				const { resolve, reject } = this.pending.get(m.id); this.pending.delete(m.id);
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
	await S("Emulation.setDeviceMetricsOverride", { width: 1800, height: 1400, deviceScaleFactor: 2, mobile: false });
	await S("Network.setCookie", { name: "account", value: "JrEkAUU^=iD_q^lz", url: `https://${HOST}/`, path: "/", secure: true });
	await S("Page.navigate", { url: `https://${HOST}/www/index.html` });
	await sleep(6000);

	await S("Runtime.evaluate", {
		expression: `(async () => {
			const a = document.querySelector('a[href="#tabs-settings"]'); if (a) a.click();
			await new Promise(r=>setTimeout(r,900));
			const b = document.querySelector('a[href="#tabs-settings-framing"]'); if (b) b.click();
			await new Promise(r=>setTimeout(r,900));
			if (window.$) { $('#tabs-li-setting-aframe').show(); $('#tabs-settings-framing').show(); }
			return 1;
		})()`, awaitPromise: true, returnByValue: true,
	});
	await sleep(2500);

	const info = await ev(`(() => { const el=document.querySelector('#tabs-settings-framing'); if(!el) return 'none';
		const r=el.getBoundingClientRect(); el.style.display='block';
		return JSON.stringify({x:r.x,y:r.y,w:r.width,h:el.getBoundingClientRect().height,
			html: el.innerHTML.length, text: el.innerText.replace(/\\n+/g,' | ').slice(0,600)}); })()`);
	console.log("settings-framing:", info);

	const r = JSON.parse(info);
	const res = await S("Page.captureScreenshot", {
		format: "png", captureBeyondViewport: true,
		clip: { x: Math.max(0, r.x - 8), y: Math.max(0, r.y - 8), width: Math.max(50, r.w + 16), height: Math.max(50, r.h + 16), scale: 2 },
	});
	fs.writeFileSync(path.join(OUT, "web_framing_settings.png"), Buffer.from(res.data, "base64"));
	console.log("wrote web_framing_settings.png", Math.round(r.w), Math.round(r.h));

	cdp.ws.close(); chrome.kill();
}
main().catch((e) => { console.error("FAILED:", e); try { chrome.kill(); } catch {} process.exit(1); });

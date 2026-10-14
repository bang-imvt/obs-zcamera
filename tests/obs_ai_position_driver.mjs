/* Pin the drawn box to the camera's own numbers.

   The overlay is drawn from normalized coordinates the camera reports, so the
   red pixels it produces must land in the same normalized place. A direct
   websocket client reads what the camera is reporting right now, a screenshot
   reads what OBS rendered, and the difference between the overlay-on and
   overlay-off masks is the drawn box alone (the camera's own red OSD is in
   both).

   Run:  node verify_position.mjs --ip 192.168.10.106 --out .
*/

import { writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { createHash } from 'node:crypto';
import { parseBmp } from './obs_bmp_pixels.mjs';

const args = { ip: '192.168.10.106', port: 4455, out: '.' };
for (let i = 2; i < process.argv.length; i++) {
	const a = process.argv[i];
	if (a === '--ip') args.ip = process.argv[++i];
	else if (a === '--port') args.port = parseInt(process.argv[++i], 10);
	else if (a === '--out') args.out = process.argv[++i];
	else throw new Error(`unknown argument ${a}`);
}
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const info = (s) => console.log(`  [info] ${s}`);

/* The same red predicate countRed() uses, per pixel. */
function redMask(buf)
{
	const b = parseBmp(buf);
	if (!b)
		return null;
	const m = new Uint8Array(b.width * b.height);
	for (let row = 0; row < b.height; row++) {
		const y = b.bottomUp ? b.height - 1 - row : row;
		const base = b.dataOffset + row * b.stride;
		for (let x = 0; x < b.width; x++) {
			const p = base + x * b.bytes;
			if (p + b.bytes > b.buf.length)
				break;
			const bl = b.buf[p], gr = b.buf[p + 1], rr = b.buf[p + 2];
			if (rr > 140 && gr < 90 && bl < 90 && rr - gr > 60 && rr - bl > 60)
				m[y * b.width + x] = 1;
		}
	}
	return { m, width: b.width, height: b.height };
}

function bboxOf(mask)
{
	let minX = 1e9, minY = 1e9, maxX = -1, maxY = -1, n = 0;
	for (let y = 0; y < mask.height; y++)
		for (let x = 0; x < mask.width; x++)
			if (mask.m[y * mask.width + x]) {
				n++;
				if (x < minX) minX = x;
				if (x > maxX) maxX = x;
				if (y < minY) minY = y;
				if (y > maxY) maxY = y;
			}
	if (maxX < 0)
		return null;
	return {
		count: n,
		x0: minX / mask.width, y0: minY / mask.height,
		x1: (maxX + 1) / mask.width, y1: (maxY + 1) / mask.height,
	};
}

/* What the camera is reporting over its notification socket right now. */
function readCameraBoxes(host, ms)
{
	return new Promise((resolve) => {
		const ws = new WebSocket(`ws://${host}:81/`);
		let last = null;
		ws.addEventListener('message', (ev) => {
			let o;
			try { o = JSON.parse(ev.data); } catch { return; }
			if (o.what !== 'AI' || !o.num)
				return;
			const boxes = [];
			for (let i = 0; i + 7 < o.objs.length; i += o.cols)
				boxes.push({ x: o.objs[i + 4], y: o.objs[i + 5], width: o.objs[i + 6], height: o.objs[i + 7] });
			last = boxes;
		});
		ws.addEventListener('error', () => resolve(last));
		setTimeout(() => { try { ws.close(); } catch {} resolve(last); }, ms);
	});
}

class Ws {
	constructor(url) { this.url = url; this.id = 1; this.p = new Map(); }
	connect() {
		return new Promise((res, rej) => {
			const ws = new WebSocket(this.url);
			this.ws = ws;
			ws.addEventListener('message', (ev) => {
				let m; try { m = JSON.parse(ev.data); } catch { return; }
				const d = m.d || {};
				if (m.op === 0) {
					const payload = { rpcVersion: 1, eventSubscriptions: 0 };
					if (d.authentication) {
						const sha = (s) => createHash('sha256').update(s).digest('base64');
						payload.authentication = sha(sha('' + d.authentication.salt) + d.authentication.challenge);
					}
					ws.send(JSON.stringify({ op: 1, d: payload }));
				} else if (m.op === 2) res();
				else if (m.op === 7) {
					const q = this.p.get(d.requestId);
					if (!q) return;
					this.p.delete(d.requestId);
					d.requestStatus?.result ? q.res(d.responseData || {}) :
						q.rej(new Error(`${d.requestType} (${d.requestStatus?.code}: ${d.requestStatus?.comment})`));
				}
			});
			ws.addEventListener('error', () => rej(new Error('ws error')));
		});
	}
	req(t, d = {}, timeoutMs = 45000) {
		const id = String(this.id++);
		return new Promise((res, rej) => {
			const to = setTimeout(() => { this.p.delete(id); rej(new Error(`${t} timed out`)); }, timeoutMs);
			this.p.set(id, { res: (v) => { clearTimeout(to); res(v); }, rej: (e) => { clearTimeout(to); rej(e); } });
			this.ws.send(JSON.stringify({ op: 6, d: { requestType: t, requestId: id, requestData: d } }));
		});
	}
}

const SCENE = 'ZC Autotest';
const SRC = `ZC Position ${Date.now()}`;
const W = 1280, H = 720;

async function shot(ws, name)
{
	const r = await ws.req('GetSourceScreenshot', {
		sourceName: name, imageFormat: 'bmp', imageWidth: W, imageHeight: H,
		imageCompressionQuality: -1,
	});
	return Buffer.from(r.imageData.replace(/^data:image\/\w+;base64,/, ''), 'base64');
}

async function main()
{
	const ws = new Ws(`ws://127.0.0.1:${args.port}`);
	await ws.connect();
	await ws.req('GetVersion');

	await ws.req('CreateInput', {
		sceneName: SCENE, inputName: SRC, inputKind: 'zcamera_source',
		sceneItemEnabled: true,
		inputSettings: { zc_source_ip: args.ip, zc_decode_backend: 0, zc_ai_overlay: true },
	});
	await ws.req('SetCurrentProgramScene', { sceneName: SCENE });
	info(`source '${SRC}' created with the overlay on`);
	await sleep(9000);

	/* What the camera says, then what OBS drew, as close in time as possible. */
	const reported = await readCameraBoxes(args.ip, 2500);
	info(`camera reports ${reported?.length ?? 0} box(es): ` +
	       (reported || []).map((b) => `x${b.x.toFixed(3)} y${b.y.toFixed(3)} w${b.width.toFixed(3)} h${b.height.toFixed(3)}`).join(', '));

	const on = await shot(ws, SRC);
	writeFileSync(join(args.out, 'pos_on.bmp'), on);
	await ws.req('SetInputSettings', {
		inputName: SRC, inputKind: 'zcamera_source',
		inputSettings: { zc_ai_overlay: false },
	});
	await sleep(3000);
	const off = await shot(ws, SRC);
	writeFileSync(join(args.out, 'pos_off.bmp'), off);

	const onM = redMask(on), offM = redMask(off);
	const drawn = { width: onM.width, height: onM.height, m: new Uint8Array(onM.width * onM.height) };
	for (let i = 0; i < drawn.m.length; i++)
		drawn.m[i] = onM.m[i] && !offM.m[i] ? 1 : 0;

	const drawnBox = bboxOf(drawn);
	const allBox = bboxOf(onM);
	info(`red with overlay on : ${allBox?.count} px, x${allBox?.x0.toFixed(3)} y${allBox?.y0.toFixed(3)} x${allBox?.x1.toFixed(3)} y${allBox?.y1.toFixed(3)}`);
	info(`red without overlay : ${bboxOf(offM)?.count} px (the camera's own OSD)`);
	info(`drawn only          : ${drawnBox?.count} px, x${drawnBox?.x0.toFixed(3)} y${drawnBox?.y0.toFixed(3)} x${drawnBox?.x1.toFixed(3)} y${drawnBox?.y1.toFixed(3)}`);

	let pass = true;
	if (!reported?.length || !drawnBox) {
		info('FAIL: nothing to compare');
		pass = false;
	} else {
		const rep = {
			x0: Math.min(...reported.map((b) => b.x)),
			y0: Math.min(...reported.map((b) => b.y)),
			x1: Math.max(...reported.map((b) => b.x + b.width)),
			y1: Math.max(...reported.map((b) => b.y + b.height)),
		};
		info(`camera union box    : x${rep.x0.toFixed(3)} y${rep.y0.toFixed(3)} x${rep.x1.toFixed(3)} y${rep.y1.toFixed(3)}`);
		const d = ['x0', 'y0', 'x1', 'y1'].map((k) => Math.abs(drawnBox[k] - rep[k]));
		const worst = Math.max(...d);
		info(`largest edge mismatch: ${worst.toFixed(3)} normalized`);
		if (worst > 0.05) { info('FAIL: the drawn box does not match the camera coordinates'); pass = false; }
		else info('PASS: the drawn box lands where the camera reports the person');
	}
	writeFileSync(join(args.out, 'position.json'), JSON.stringify({ reported, drawnBox, allBox }, null, 2));
	process.exit(pass ? 0 : 1);
}

main().catch((e) => { console.log(`  [error] ${e.message}`); process.exit(2); });

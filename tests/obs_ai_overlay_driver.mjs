/* Does the AI overlay draw into the preview render, and stay out of the mix?

   One source, three renders of the same camera:
     preview screenshot, overlay ON   -> camera's own red + the drawn boxes
     preview screenshot, overlay OFF  -> camera's own red only   (best effort)
     a frame of the recording         -> the canvas mix: must have no boxes

   The screenshot size is deliberately NOT the canvas base size: the plugin
   treats an off-screen target drawn at exactly the base size as the canvas mix
   and suppresses the overlay there by design, so a base-size screenshot would
   prove nothing about the preview path.

   Run:  node overlay_test.mjs [--ip 192.168.10.106] [--port 4455] [--out .]
                               [--record-seconds 6]
*/

import { writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { createHash } from 'node:crypto';
import { countRed, frameVariance } from './obs_bmp_pixels.mjs';

/* The camera serves a self-signed certificate, and on current firmware the
   notification socket on port 81 is wss (a plain ws dial is refused outright,
   which the plugin handles by flipping the scheme and retrying). Node's global
   WebSocket takes its TLS settings from the process rather than per socket, so
   the waitForDetection() listener below only reaches the camera with this set;
   without it the listener errors out, the driver reports "the camera is
   reporting nothing", and every box assertion is skipped as inconclusive
   instead of being checked. */
process.env.NODE_TLS_REJECT_UNAUTHORIZED = '0';

const args = { ip: '192.168.10.106', port: 4455, out: '.', recordSeconds: 6 };
for (let i = 2; i < process.argv.length; i++) {
	const a = process.argv[i];
	if (a === '--ip') args.ip = process.argv[++i];
	else if (a === '--port') args.port = parseInt(process.argv[++i], 10);
	else if (a === '--out') args.out = process.argv[++i];
	else if (a === '--record-seconds') args.recordSeconds = parseFloat(process.argv[++i]);
	else throw new Error(`unknown argument ${a}`);
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const info = (s) => console.log(`  [info] ${s}`);

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
						payload.authentication =
							sha(sha('' + d.authentication.salt) + d.authentication.challenge);
					}
					ws.send(JSON.stringify({ op: 1, d: payload }));
				} else if (m.op === 2) res();
				else if (m.op === 7) {
					const q = this.p.get(d.requestId);
					if (!q) return;
					this.p.delete(d.requestId);
					d.requestStatus?.result
						? q.res(d.responseData || {})
						: q.rej(new Error(
							`${d.requestType} (${d.requestStatus?.code}: ${d.requestStatus?.comment})`));
				}
			});
			ws.addEventListener('error', () => rej(new Error(`ws error on ${this.url}`)));
		});
	}
	req(t, d = {}, timeoutMs = 20000) {
		const id = String(this.id++);
		return new Promise((res, rej) => {
			const to = setTimeout(() => { this.p.delete(id); rej(new Error(`${t} timed out`)); }, timeoutMs);
			this.p.set(id, { res: (v) => { clearTimeout(to); res(v); }, rej: (e) => { clearTimeout(to); rej(e); } });
			this.ws.send(JSON.stringify({ op: 6, d: { requestType: t, requestId: id, requestData: d } }));
		});
	}
}

/* OBS renders on its graphics thread; on a loaded box a request can stall, so
   retry before treating a failure as a result. */
async function shot(ws, name, w, h, attempts = 4)
{
	let last = null;
	for (let i = 0; i < attempts; i++) {
		try {
			const r = await ws.req('GetSourceScreenshot', {
				sourceName: name,
				imageFormat: 'bmp',
				imageWidth: w,
				imageHeight: h,
				imageCompressionQuality: -1,
			}, 45000);
			return Buffer.from(r.imageData.replace(/^data:image\/\w+;base64,/, ''), 'base64');
		} catch (e) {
			last = e;
			info(`screenshot attempt ${i + 1}/${attempts}: ${e.message}`);
			await sleep(1000);
		}
	}
	throw last;
}

/* The boxes the camera is reporting on its own notification socket, resolved as
   soon as one arrives so the screenshot lands in the same moment. Null means
   the camera sent no AI event with a box in it within the timeout.

   The dial mirrors the plugin's ZcWsNotifier: port 81, wss first and ws as the
   fallback, because a plain ws dial to port 81 is refused on current firmware
   while wss on the same port answers 101. */
function waitForDetection(host, ms)
{
	return new Promise((resolve) => {
		let last = null, sock = null, budget = null;
		const done = () => {
			clearTimeout(budget);
			clearInterval(poll);
			try { sock?.close(); } catch { /* gone */ }
			resolve(last);
		};
		const attach = (url, isFallback) => {
			let s;
			try { s = new WebSocket(url); } catch { return resolve(last); }
			sock = s;
			s.addEventListener('message', (ev) => {
				let o;
				try { o = JSON.parse(ev.data); } catch { return; }
				if (o.what === 'AI' && o.num > 0) {
					const cols = o.cols || 8;
					last = Array.from({ length: o.num }, (_, k) => ({
						x: o.objs[k * cols + 4], y: o.objs[k * cols + 5],
						width: o.objs[k * cols + 6], height: o.objs[k * cols + 7],
					}));
				}
			});
			s.addEventListener('error', () => {
				/* Never reached the upgrade: try the other scheme once, the
				   same way the plugin does. */
				if (!isFallback) return attach(`ws://${host}:81/`, true);
				resolve(last);
			});
		};
		budget = setTimeout(done, ms);
		const poll = setInterval(() => { if (last !== null) done(); }, 300);
		attach(`wss://${host}:81/`, false);
	});
}

const SCENE = 'ZC Autotest';
/* Unique per run: a source left in the collection from an earlier run cannot be
   removed reliably over websocket, and CreateInput would then refuse the name. */
const SRC = `ZC Overlay ${Date.now()}`;
const W = 1280, H = 720;

async function main()
{
	const ws = new Ws(`ws://127.0.0.1:${args.port}`);
	await ws.connect();
	const ready = Date.now() + 60000;
	for (;;) {
		try { const v = await ws.req('GetVersion'); info(`OBS ${v.obsVersion} / obs-websocket ${v.obsWebSocketVersion}`); break; }
		catch (e) {
			if (!/\(207/.test(e.message) || Date.now() > ready) throw e;
			await sleep(1000);
		}
	}
	info(`camera ${args.ip}, screenshots at ${W}x${H} (not the canvas base size)`);

	try { await ws.req('RemoveInput', { sourceName: SRC }); } catch { /* none */ }
	await ws.req('CreateInput', {
		sceneName: SCENE,
		inputName: SRC,
		inputKind: 'zcamera_source',
		sceneItemEnabled: true,
		inputSettings: {
			zc_source_ip: args.ip,
			zc_decode_backend: 0,
			zc_ai_overlay: true,
		},
	});
	info('source created with the overlay ON');

	/* The recording only carries what the active scene holds, so make sure the
	   scene the source was added to is the live one. */
	for (const [req, field] of [['SetCurrentProgramScene', 'sceneName'], ['SetCurrentScene', 'sceneName']]) {
		try { await ws.req(req, { [field]: SCENE }); info(`active scene set to '${SCENE}' via ${req}`); break; }
		catch (e) { info(`${req}: ${e.message}`); }
	}

	/* Give the pipeline and the notification feed time to come up, then wait
	   for a picture that is not black. */
	await sleep(6000);
	let variance = 0;
	const deadline = Date.now() + 60000;
	for (;;) {
		const probe = await shot(ws, SRC, W, H);
		variance = frameVariance(probe) ?? 0;
		if (variance > 8)
			break;
		if (Date.now() > deadline)
			break;
		await sleep(3000);
	}

	/* An empty room correctly draws nothing, and a screenshot taken then would
	   read as "the overlay is broken" when nothing is wrong. Wait until the
	   camera itself reports someone, then shoot straight away. */
	const detected = await waitForDetection(args.ip, 45000);
	info(`camera is reporting ` + (!detected
		? 'nothing (no AI event within 45s)'
		: `${detected.length} box(es): ` + detected
			.map((b) => `x${b.x.toFixed(3)} y${b.y.toFixed(3)} w${b.width.toFixed(3)} h${b.height.toFixed(3)}`)
			.join(', ')));

	const on = await shot(ws, SRC, W, H);
	writeFileSync(join(args.out, 'on.bmp'), on);
	const onRed = countRed(on);
	info(`preview, overlay ON : ${onRed.count} red px, box ${JSON.stringify(onRed.box)}, variance ${variance.toFixed(1)}`);

	/* Same source, overlay off: the camera's own red, with nothing drawn.
	   OBS 31's obs-websocket names this SetInputSettings (inputName), the older
	   spelling is SetSourceSettings (sourceName); try both. */
	let offRed = null;
	const variants = [
		['SetInputSettings', 'inputName', 'inputKind', 'inputSettings'],
		['SetSourceSettings', 'sourceName', 'sourceType', 'sourceSettings'],
	];
	for (const [req, nameField, kindField, settingsField] of variants) {
		try {
			await ws.req(req, {
				[nameField]: SRC,
				[kindField]: 'zcamera_source',
				[settingsField]: { zc_ai_overlay: false },
			});
			info(`overlay switched off via ${req}`);
			break;
		} catch (e) {
			info(`${req}: ${e.message}`);
		}
	}
	if (!offRed) {
		try {
			await sleep(4000);
			const off = await shot(ws, SRC, W, H);
			writeFileSync(join(args.out, 'off.bmp'), off);
			offRed = countRed(off);
			info(`preview, overlay OFF: ${offRed.count} red px`);
		} catch (e) {
			info(`no overlay-off control: ${e.message}`);
		}
	}

	/* The recording is the canvas mix: it must carry no boxes. */
	let recording = '';
	let started = false;
	for (const start of ['StartRecord', 'StartRecording']) {
		try { await ws.req(start); started = true; info(`recording via ${start}`); break; }
		catch (e) { info(`${start}: ${e.message}`); }
	}
	if (started) {
		try {
			await sleep(args.recordSeconds * 1000);
			for (const stop of ['StopRecord', 'StopRecording']) {
				try { await ws.req(stop); break; } catch (e) { info(`${stop}: ${e.message}`); }
			}
			for (let i = 0; i < 30; i++) {
				let st = null;
				for (const stName of ['GetRecordStatus', 'GetRecordingStatus']) {
					try { st = await ws.req(stName); break; } catch { /* try the other */ }
				}
				if (st && !st.outputActive && st.outputFile) { recording = st.outputFile; break; }
				await sleep(500);
			}
		} catch (e) {
			info(`recording failed: ${e.message}`);
		}
	}
	info(`recording: ${recording || '(none)'}`);

	writeFileSync(join(args.out, 'result.json'), JSON.stringify({
		recording, onRed, offRed, variance, detected,
	}, null, 2));
	console.log(`RECORDING ${recording}`);
	process.exit(0);
}

main().catch((e) => { console.log(`  [error] ${e.message}`); process.exit(2); });

/*
   obs-websocket driver for the obs-zcamera OBS integration test.

   Injects a zcamera_source into a running OBS through obs-websocket 5.x (the
   copy OBS 28+ ships) and verifies the zero-copy render path inside the real
   application: the source is created like a user would create it, OBS renders
   it, and the rendered frame is read back as a BMP and checked for real pixels
   and motion. Nothing is clicked and no scene collection has to be prepared
   beforehand, so the test exercises the plugin's own create/update/render
   path rather than a hand-written config file.

   Uses Node's built-in global WebSocket (Node >= 21), so there is no npm
   dependency.

   Usage: node obs_ws_driver.mjs [--ip <addr>] [--backend <n>] [--port 4455]
                                [--out <dir>] [--ready-timeout 25]
                                [--record-seconds 5]
*/

import { createHash } from 'node:crypto';
import { mkdirSync, statSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';

/* ------------------------------------------------------------------ args */

function parseArgs(argv)
{
	const out = {
		ip: '192.168.10.174',
		backend: 0,
		port: 4455,
		out: '.',
		readyTimeout: 25,
		recordSeconds: 5,
	};

	for (let i = 0; i < argv.length; i++) {
		const a = argv[i];
		const next = () => argv[++i];
		if (a === '--ip') out.ip = next();
		else if (a === '--backend') out.backend = parseInt(next(), 10);
		else if (a === '--port') out.port = parseInt(next(), 10);
		else if (a === '--out') out.out = next();
		else if (a === '--ready-timeout') out.readyTimeout = parseFloat(next());
		else if (a === '--record-seconds') out.recordSeconds = parseFloat(next());
		else throw new Error(`unknown argument: ${a}`);
	}
	return out;
}

const args = parseArgs(process.argv.slice(2));
mkdirSync(args.out, { recursive: true });

/* --------------------------------------------------------------- reporting */

const checks = [];

function check(name, ok, detail)
{
	checks.push({ name, ok: !!ok, detail: String(detail ?? '') });
	console.log(`  [${ok ? 'PASS' : 'FAIL'}] ${name}` +
		    (detail !== undefined && detail !== '' ? ` — ${detail}` : ''));
}

function info(line)
{
	console.log(`  [info] ${line}`);
}

/* ------------------------------------------------------------- ws protocol */

/* obs-websocket 5.x opcodes: 0 Hello, 1 Identify, 2 Identified, 5 Event,
   6 Request, 7 RequestResponse. The server is expected to be reachable on
   loopback without authentication; the identify path still signs the
   challenge so an auth-enabled OBS config works too. */
class ObsWs {
	constructor(url)
	{
		this.url = url;
		this.nextId = 1;
		this.pending = new Map();
		this.identified = false;
	}

	connect(timeoutMs)
	{
		return new Promise((resolve, reject) => {
			const ws = new WebSocket(this.url);
			this.ws = ws;
			const timer = setTimeout(
				() => reject(new Error(`timed out connecting to ${this.url}`)),
				timeoutMs);

			ws.addEventListener('message', (ev) => {
				let msg;
				try {
					msg = JSON.parse(ev.data);
				} catch {
					return;
				}
				const d = msg.d || {};
				if (msg.op === 0) {
					ws.send(JSON.stringify({
						op: 1,
						d: this.identifyPayload(d),
					}));
				} else if (msg.op === 2) {
					this.identified = true;
					clearTimeout(timer);
					resolve();
				} else if (msg.op === 7) {
					const p = this.pending.get(d.requestId);
					if (!p)
						return;
					this.pending.delete(d.requestId);
					if (d.requestStatus && d.requestStatus.result)
						p.resolve(d.responseData || {});
					else
						p.reject(new Error(
							`${d.requestType} failed ` +
							`(${d.requestStatus?.code}: ` +
							`${d.requestStatus?.comment})`));
				}
			});
			ws.addEventListener('error', () => {
				clearTimeout(timer);
				reject(new Error(`websocket error on ${this.url}`));
			});
			ws.addEventListener('close', () => {
				if (!this.identified) {
					clearTimeout(timer);
					reject(new Error('websocket closed before Identify'));
				}
			});
		});
	}

	identifyPayload(hello)
	{
		const payload = { rpcVersion: 1, eventSubscriptions: 0 };
		const auth = hello.authentication;
		if (auth) {
			const sha = (s) => createHash('sha256').update(s)
						  .digest('base64');
			const secret = sha('' + auth.salt);
			payload.authentication =
				sha(secret + auth.challenge);
		}
		return payload;
	}

	request(requestType, requestData = {}, timeoutMs = 10000)
	{
		const requestId = String(this.nextId++);
		return new Promise((resolve, reject) => {
			const timer = setTimeout(() => {
				this.pending.delete(requestId);
				reject(new Error(`${requestType} timed out`));
			}, timeoutMs);
			this.pending.set(requestId, {
				resolve: (v) => {
					clearTimeout(timer);
					resolve(v);
				},
				reject: (e) => {
					clearTimeout(timer);
					reject(e);
				},
			});
			this.ws.send(JSON.stringify({
				op: 6,
				d: { requestType, requestId, requestData },
			}));
		});
	}

	close()
	{
		try {
			this.ws.close();
		} catch { /* already gone */ }
	}
}

/* ------------------------------------------------------------------- bmp */

/* obs-websocket encodes BMP through OBS's own stage-surface encoder: a
   standard BITMAPFILEHEADER plus BITMAPINFOHEADER, 24 or 32 bpp, rows padded
   to 4 bytes. Only the header fields needed to walk the pixels are read. */
function parseBmp(buf)
{
	if (buf.length < 54 || buf.readUInt16LE(0) !== 0x4d42)
		return null;
	const dataOffset = buf.readUInt32LE(10);
	const headerSize = buf.readUInt32LE(14);
	const width = buf.readInt32LE(18);
	const height = Math.abs(buf.readInt32LE(22));
	const bpp = buf.readUInt16LE(28);
	if (headerSize < 40 || width <= 0 || height <= 0)
		return null;
	if (bpp !== 24 && bpp !== 32)
		return null;
	const stride = Math.floor((width * (bpp / 8) + 3) / 4) * 4;
	if (dataOffset + stride * height > buf.length)
		return null;
	return { width, height, bpp, dataOffset, pixels: buf };
}

/* Return the interesting statistics of one frame. A working camera frame is
   bright (a large max), varied (a large per-channel spread) and not uniform;
   a blank or never-rendered texture is all zeros. */
function frameStats(bmp)
{
	const { width, height, bpp, dataOffset, pixels } = bmp;
	const stride = Math.floor((width * (bpp / 8) + 3) / 4) * 4;
	const bytes = bpp / 8;

	let min = 255;
	let max = 0;
	let sum = 0;
	let sumSq = 0;
	let bright = 0;
	let n = 0;

	for (let y = 0; y < height; y++) {
		const row = dataOffset + y * stride;
		for (let x = 0; x < width; x++) {
			const p = row + x * bytes;
			const b = pixels[p];
			const g = pixels[p + 1];
			const r = pixels[p + 2];
			const lum = (r * 77 + g * 150 + b * 29) >> 8;
			if (lum < min)
				min = lum;
			if (lum > max)
				max = lum;
			sum += lum;
			sumSq += lum * lum;
			if (lum > 20)
				bright++;
			n++;
		}
	}

	if (n === 0)
		return { width, height, bpp, min: 0, max: 0, mean: 0, stddev: 0,
			 brightFraction: 0, bytes: pixels.length };

	const mean = sum / n;
	const variance = Math.max(0, sumSq / n - mean * mean);
	return {
		width,
		height,
		bpp,
		min,
		max,
		mean: +mean.toFixed(1),
		stddev: +Math.sqrt(variance).toFixed(1),
		brightFraction: +(bright / n).toFixed(3),
		bytes: pixels.length,
	};
}

/* Fraction of pixels whose luma actually changed between two frames. */
function frameDiff(a, b)
{
	if (a.width !== b.width || a.height !== b.height)
		return 1;
	const pa = a.data;
	const pb = b.data;
	let changed = 0;
	let n = 0;
	for (let y = 0; y < a.height; y++) {
		const rowA = a.dataOffset + y * a.stride;
		const rowB = b.dataOffset + y * b.stride;
		for (let x = 0; x < a.width; x++) {
			const ia = rowA + x * a.bytes;
			const ib = rowB + x * a.bytes;
			const la = (pa[ia + 2] * 77 + pa[ia + 1] * 150 +
				    pa[ia] * 29) >> 8;
			const lb = (pb[ib + 2] * 77 + pb[ib + 1] * 150 +
				    pb[ib] * 29) >> 8;
			if (Math.abs(la - lb) > 2)
				changed++;
			n++;
		}
	}
	return +(changed / n).toFixed(4);
}

function bmpForDiff(buf)
{
	const b = parseBmp(buf);
	if (!b)
		return null;
	const bytes = b.bpp / 8;
	return {
		width: b.width,
		height: b.height,
		dataOffset: b.dataOffset,
		bytes,
		stride: Math.floor((b.width * bytes + 3) / 4) * 4,
		data: b.pixels,
	};
}

/* ------------------------------------------------------------------ main */

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

/* imageWidth/imageHeight are left out on purpose: obs-websocket then uses the
   source's own size, and it rejects any explicit value below 8.

   obs-websocket renders the source on OBS's graphics thread, so the request
   only completes when that thread gets to it. On a loaded machine (this repo is
   tested on a shared box with an antivirus driver in the render path) a single
   request can take tens of seconds or be dropped entirely, which says nothing
   about the source under test - so retry before reporting a failure. */
async function screenshot(ws, sourceName, timeoutMs = 45000, attempts = 3)
{
	let lastErr = null;
	for (let i = 0; i < attempts; i++) {
		try {
			const r = await ws.request('GetSourceScreenshot', {
				sourceName,
				imageFormat: 'bmp',
				imageCompressionQuality: -1,
			}, timeoutMs);
			return Buffer.from(
				r.imageData.replace(/^data:image\/\w+;base64,/, ''),
				'base64');
		} catch (e) {
			lastErr = e;
			info(`screenshot attempt ${i + 1}/${attempts} failed: ${e.message}`);
			if (i + 1 < attempts)
				await sleep(1000);
		}
	}
	throw lastErr;
}

async function main()
{
	const sourceName = 'ZC Autotest Camera';
	const sceneName = 'ZC Autotest';
	const ws = new ObsWs(`ws://127.0.0.1:${args.port}`);

	await ws.connect(10000);

	/* The websocket server accepts connections before the OBS frontend has
	   finished loading and answers code 207 ("OBS is not ready to perform the
	   request") until then, so retry until it is up. */
	const readyDeadline = Date.now() + 60000;
	let ver;
	for (;;) {
		try {
			ver = await ws.request('GetVersion');
			break;
		} catch (e) {
			if (!/\(207/.test(e.message) || Date.now() > readyDeadline)
				throw e;
			await sleep(1000);
		}
	}
	info(`OBS ${ver.obsVersion} / obs-websocket ${ver.obsWebSocketVersion}`);
	info(`camera=${args.ip} backend=${args.backend}`);

	/* The plugin must have been loaded for the source kind to exist; this is
	   the first thing that fails if the DLL was not deployed. */
	const kinds = await ws.request('GetInputKindList', { unversioned: false });
	check('OBS loaded the obs-zcamera plugin (zcamera_source is registered)',
	      (kinds.inputKinds || []).includes('zcamera_source'),
	      (kinds.inputKinds || []).filter((k) => k.includes('zcamera'))
	      .join(',') || 'not registered');

	/* Build the scene through the same API the UI uses. */
	try {
		await ws.request('CreateScene', { sceneName });
	} catch { /* already exists from an earlier run */ }
	await ws.request('SetCurrentProgramScene', { sceneName });

	try {
		await ws.request('RemoveInput', { inputName: sourceName });
	} catch { /* not present */ }

	await ws.request('CreateInput', {
		sceneName,
		inputName: sourceName,
		inputKind: 'zcamera_source',
		inputSettings: {
			zc_source_ip: args.ip,
			zc_decode_backend: args.backend,
		},
		sceneItemEnabled: true,
	});
	check('created the zcamera source through obs-websocket', true, sourceName);

	/* OBS merges the settings given to obs_source_create() into the source's
	   data object and only then calls the plugin's get_defaults(). A
	   get_defaults() that writes its keys outright (obs_data_set_*) rather than
	   as defaults (obs_data_set_default_*) therefore wipes them, and the source
	   comes up with no camera IP and never renders. Assert the create-time
	   settings survive before the explicit update below can mask it. */
	const created = await ws.request('GetInputSettings', {
		inputName: sourceName,
	});
	check('the settings passed to CreateInput survive the plugin defaults',
	      created.inputSettings?.zc_source_ip === args.ip,
	      `zc_source_ip='${created.inputSettings?.zc_source_ip}'`);

	/* CreateInput's settings can be lost by an old get_defaults, so configure
	   the source the way the properties dialog does as well: an explicit
	   update. */
	await ws.request('SetInputSettings', {
		inputName: sourceName,
		inputSettings: {
			zc_source_ip: args.ip,
			zc_decode_backend: args.backend,
		},
		overlay: true,
	});
	const applied = await ws.request('GetInputSettings', {
		inputName: sourceName,
	});
	check('the injected camera settings reached the plugin',
	      applied.inputSettings?.zc_source_ip === args.ip,
	      `zc_source_ip='${applied.inputSettings?.zc_source_ip}' ` +
	      `zc_decode_backend=${applied.inputSettings?.zc_decode_backend}`);

	/* The source must expose its real size once the decoder is up: that alone
	   already proves a frame was decoded and imported (base size comes from
	   the decoder, not from settings). */
	const deadline = Date.now() + args.readyTimeout * 1000;
	let active = null;
	let width = 0;
	let height = 0;
	let lastErr = '';
	while (Date.now() < deadline) {
		try {
			active = await ws.request('GetSourceActive', { sourceName });
		} catch (e) { /* source not ready yet */
			lastErr = String(e.message);
		}
		try {
			/* One attempt here: the loop below is the retry, and it
			   respects the ready deadline. */
			const raw = await screenshot(ws, sourceName, 20000, 1);
			const b = parseBmp(raw);
			if (b && b.width > 0 && b.height > 0) {
				width = b.width;
				height = b.height;
				break;
			}
			lastErr = `screenshot was not a usable BMP (${raw.length} bytes)`;
		} catch (e) { /* no frame rendered yet */
			lastErr = String(e.message);
		}
		await sleep(500);
	}

	check('source reports itself active in OBS',
	      !!(active && (active.videoActive || active.videoShowing)),
	      JSON.stringify(active));
	check('source renders at a real video size', width > 0 && height > 0,
	      `${width}x${height}` + (lastErr ? ` (last error: ${lastErr})` : ''));

	/* Sample the rendered output a few times: real pixels, and a live feed
	   rather than one frozen frame. The source reports its size from a
	   placeholder texture as soon as the decoder is opened, so the first
	   screenshots can still be blank — those are kept aside and we wait for a
	   decoded picture instead of asserting on whatever came first. */
	const samples = [];
	let blankFirst = null;
	const frameDeadline = Date.now() + args.readyTimeout * 1000;
	while (Date.now() < frameDeadline) {
		let raw = null;
		try {
			raw = await screenshot(ws, sourceName, 45000, 2);
		} catch (e) {
			/* Keep trying until the deadline: a request that is lost to a
			   busy graphics thread is not a verdict on the source. If no
			   frame ever arrives the checks below fail on the empty
			   sample list, which is the honest outcome. */
			info(`frame ${samples.length}: screenshot failed: ${e.message}`);
			await sleep(1000);
			continue;
		}
		const path = join(args.out, `zc_frame_${samples.length}.bmp`);
		writeFileSync(path, raw);
		const stats = frameStats(parseBmp(raw) || {
			width: 0,
			height: 0,
			bpp: 0,
			dataOffset: 0,
			pixels: Buffer.alloc(0),
		});
		info(`frame ${samples.length}: ${stats.width}x${stats.height} ` +
		     `luma min=${stats.min} max=${stats.max} mean=${stats.mean} ` +
		     `stddev=${stats.stddev} bright=${stats.brightFraction}`);
		if (samples.length === 0 && stats.max < 32) {
			blankFirst = { raw, stats, path };
			await sleep(1000);
			continue;
		}
		samples.push({ raw, stats, path });
		if (samples.length === 3)
			break;
		await sleep(1200);
	}
	/* Never produced a picture: keep the blank frame so the checks below fail
	   with its statistics rather than on a missing sample. */
	if (samples.length === 0 && blankFirst)
		samples.push(blankFirst);

	/* Every sampled frame must show the picture: a source that only draws on
	   the render that happens to hit a new frame comes back blank in between,
	   because OBS renders (and screenshots) faster than the camera produces
	   frames. */
	const blankSample = samples.find(
		(s) => s.stats.max < 32 || s.stats.brightFraction < 0.05);
	check('every rendered frame shows the picture',
	      samples.length >= 3 && !blankSample,
	      blankSample
		      ? `blank frame ${samples.indexOf(blankSample)} of ` +
			`${samples.length}: luma max=${blankSample.stats.max} ` +
			`bright=${blankSample.stats.brightFraction}`
		      : `${samples.length} frames, luma max=` +
			samples.map((s) => s.stats.max).join('/'));

	const diffs = [];
	for (let i = 1; i < samples.length; i++) {
		const a = bmpForDiff(samples[0].raw);
		const b = bmpForDiff(samples[i].raw);
		diffs.push(a && b ? frameDiff(a, b) : 0);
	}
	const moved = diffs.some((d) => d > 0.01);
	check('the feed is live, not one frozen frame', moved,
	      `changed-pixel fractions: ${diffs.map((d) => (d * 100).toFixed(1) + '%').join(', ')}`);

	/* Recording proves the whole chain end to end through OBS's encoder. */
	let recorded = '';
	if (args.recordSeconds > 0) {
		/* Both calls wait for OBS to actually start/stop the output, and
		   starting the encoder is slow on a busy GPU, so they get generous
		   timeouts of their own. */
		let started = false;
		try {
			await ws.request('StartRecord', {}, 90000);
			started = true;
		} catch (e) {
			info(`StartRecord failed: ${e.message}`);
		}
		let stopped = null;
		if (started) {
			await sleep(args.recordSeconds * 1000);
			for (let i = 0; i < 3 && !stopped; i++) {
				try {
					stopped = await ws.request('StopRecord', {}, 90000);
				} catch (e) {
					info(`StopRecord attempt ${i + 1}/3 failed: ${e.message}`);
					await sleep(2000);
				}
			}
		}
		recorded = (stopped && stopped.outputPath) || '';
		/* StopRecord reports the path OBS would write to, which it hands
		   back even when the output produced nothing. Wait for the file and
		   assert on its size, otherwise a run that recorded no video at all
		   would be reported as a clip. */
		let clipBytes = 0;
		for (let i = 0; i < 15 && recorded && clipBytes === 0; i++) {
			try {
				clipBytes = statSync(recorded).size;
			} catch { /* not on disk yet */ }
			if (clipBytes === 0)
				await sleep(1000);
		}
		check('OBS recorded a clip while the source was rendering',
		      clipBytes > 0,
		      recorded
			      ? `${recorded} (${clipBytes} bytes)`
			      : (started ? 'no output path' : 'recording never started'));
	}

	/* ---- legacy profile recovery (dock-named source, empty address) ----
	   The dock names its sources "ZCamera <ip>" and passes zc_source_ip at
	   create time. A scene collection saved before zc_source_getdefaults()
	   stopped clobbering create-time settings holds that source with
	   zc_source_ip = "", and it used to stay black on every later OBS launch
	   (the operator had to re-add it). It must now recover its address from
	   its own name and render, with no dock interaction. */
	const legacyName = `ZCamera ${args.ip}`;
	/* The camera serves a single SSP stream per client: a second source
	   pointed at the same address connects at TCP level but is never sent an
	   SSP_T_INIT, so it stays black forever (verified with three raw TCP
	   connections to port 9999 — only the first receives data). This case is
	   about legacy address recovery, not about two concurrent streams, so the
	   main source is taken down first to free the camera's only session. */
	try {
		await ws.request('RemoveInput', { inputName: sourceName });
	} catch { /* not present */ }
	try {
		await ws.request('RemoveInput', { inputName: legacyName });
	} catch { /* not present */ }
	try {
		await ws.request('CreateInput', {
			sceneName,
			inputName: legacyName,
			inputKind: 'zcamera_source',
			inputSettings: { zc_source_ip: '' },
			sceneItemEnabled: true,
		});
		const legacySettings = await ws.request('GetInputSettings', {
			inputName: legacyName,
		});
		check('the legacy source really starts with an empty address',
		      legacySettings.inputSettings?.zc_source_ip === '',
		      `zc_source_ip='${legacySettings.inputSettings?.zc_source_ip}'`);

		const legacyDeadline = Date.now() + args.readyTimeout * 1000;
		let legacyActive = null;
		let legacyStats = null;
		let legacyErr = '';
		while (Date.now() < legacyDeadline) {
			try {
				legacyActive = await ws.request('GetSourceActive', {
					sourceName: legacyName,
				});
			} catch (e) {
				legacyErr = String(e.message);
			}
			try {
				const raw = await screenshot(ws, legacyName, 20000, 1);
				const b = parseBmp(raw);
				if (b && b.width > 0) {
					const stats = frameStats(b);
					info(`legacy frame: ${stats.width}x${stats.height} ` +
					     `luma min=${stats.min} max=${stats.max} ` +
					     `mean=${stats.mean}`);
					if (stats.max >= 32) {
						legacyStats = stats;
						break;
					}
				}
				legacyErr = 'no decoded picture yet';
			} catch (e) {
				legacyErr = String(e.message);
			}
			await sleep(1000);
		}
		check('a source saved with an empty address recovers it from its name',
		      !!legacyStats,
		      legacyStats
			      ? `${legacyStats.width}x${legacyStats.height} ` +
				`luma max=${legacyStats.max}`
			      : `no frame rendered (${legacyErr})`);
		check('the recovered legacy source is active in OBS',
		      !!(legacyActive &&
			 (legacyActive.videoActive || legacyActive.videoShowing)),
		      JSON.stringify(legacyActive));
	} catch (e) {
		check('the legacy empty-address case ran', false, String(e.message));
	} finally {
		try {
			await ws.request('RemoveInput', { inputName: legacyName });
		} catch { /* fine */ }
	}

	try {
		await ws.request('RemoveInput', { inputName: sourceName });
	} catch { /* fine */ }
	try {
		await ws.request('Quit', {}, 5000);
		console.log('asked OBS to quit through obs-websocket');
	} catch (e) {
		/* obs-websocket 5.5.6 has no Quit request (verified against its own
		   GetVersion.availableRequests), so this normally answers 204 and the
		   harness stops OBS itself. Keep trying anyway: if a future version
		   adds it, this is the graceful path. */
		console.log(`obs-websocket did not accept Quit (${e.message}); ` +
			    'the harness will stop OBS itself');
	}
	ws.close();

	const failed = checks.filter((c) => !c.ok);
	writeFileSync(join(args.out, 'zc_obs_result.json'),
		      JSON.stringify({
			      ip: args.ip,
			      backend: args.backend,
			      recorded,
			      samples: samples.map((s) => s.stats),
			      diffs,
			      checks,
		      }, null, 2));
	console.log(`=== ${checks.length - failed.length} passed, ${failed.length} failed ===`);
	process.exit(failed.length ? 1 : 0);
}

main().catch((e) => {
	console.error(`driver error: ${e.stack || e.message}`);
	process.exit(2);
});
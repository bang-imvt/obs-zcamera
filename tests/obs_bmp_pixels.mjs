/* Shared BMP red-pixel counting for the overlay test.
   OBS's BMP (obs-websocket) and ffmpeg's BMP are both BITMAPINFOHEADER
   bitmaps, 24 or 32 bpp, rows padded to 4 bytes, bottom-up. */

export function parseBmp(buf)
{
	if (buf.length < 54 || buf.readUInt16LE(0) !== 0x4d42)
		return null;
	const dataOffset = buf.readUInt32LE(10);
	const width = buf.readInt32LE(18);
	const height = buf.readInt32LE(22);
	const bpp = buf.readUInt16LE(28);
	if (width <= 0 || height === 0 || (bpp !== 24 && bpp !== 32))
		return null;
	return {
		width,
		height: Math.abs(height),
		bottomUp: height > 0,
		bytes: bpp / 8,
		stride: Math.floor((width * (bpp / 8) + 3) / 4) * 4,
		dataOffset,
		buf,
	};
}

/* Count pixels that read as "the red of a drawn box": strong red, weak green
   and blue. Returns the count plus the bounding box of those pixels in
   normalized image coordinates, which is what tells us the boxes landed where
   the camera reported them rather than somewhere else in the frame. */
export function countRed(buf)
{
	const b = parseBmp(buf);
	if (!b)
		return null;
	let n = 0;
	let minX = b.width, minY = b.height, maxX = -1, maxY = -1;
	for (let row = 0; row < b.height; row++) {
		const y = b.bottomUp ? b.height - 1 - row : row;
		const base = b.dataOffset + row * b.stride;
		for (let x = 0; x < b.width; x++) {
			const p = base + x * b.bytes;
			if (p + b.bytes > b.buf.length)
				break;
			const bl = b.buf[p], gr = b.buf[p + 1], rr = b.buf[p + 2];
			if (rr > 140 && gr < 90 && bl < 90 &&
			    rr - gr > 60 && rr - bl > 60) {
				n++;
				if (x < minX) minX = x;
				if (x > maxX) maxX = x;
				if (y < minY) minY = y;
				if (y > maxY) maxY = y;
			}
		}
	}
	const box = maxX < 0
	    ? null
	    : {
		    x0: +(minX / b.width).toFixed(3),
		    y0: +(minY / b.height).toFixed(3),
		    x1: +((maxX + 1) / b.width).toFixed(3),
		    y1: +((maxY + 1) / b.height).toFixed(3),
	    };
	return { count: n, width: b.width, height: b.height, box };
}

/* How much the frame varies — a black or frozen frame has almost none, so this
   is how the test knows the camera picture actually arrived. */
export function frameVariance(buf)
{
	const b = parseBmp(buf);
	if (!b)
		return null;
	let sum = 0, sum2 = 0, n = 0;
	for (let row = 0; row < b.height; row += 4) {
		const base = b.dataOffset + row * b.stride;
		for (let x = 0; x < b.width; x += 4) {
			const p = base + x * b.bytes;
			if (p + 2 >= b.buf.length)
				break;
			const v = (b.buf[p] + b.buf[p + 1] + b.buf[p + 2]) / 3;
			sum += v;
			sum2 += v * v;
			n++;
		}
	}
	if (!n)
		return null;
	const mean = sum / n;
	return Math.sqrt(Math.max(0, sum2 / n - mean * mean));
}

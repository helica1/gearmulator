/* Machinedrum remote panel: renders the machine's LCD and LEDs from the state stream and sends
   button, encoder and encoder-push events back over a WebSocket. Multitouch: every pointer is
   tracked on its own, so FUNCTION + trig chords work with two fingers. */
(function () {
	'use strict';

	const TRACK_NAMES = ['BD', 'SD', 'HT', 'MT', 'LT', 'CP', 'RS', 'CB', 'CH', 'OH', 'RC', 'CC', 'M1', 'M2', 'M3', 'M4'];
	const LED_BANK_FIRST = 0x20;
	const STATE_HEADER = 2;
	const VRAM_SIZE = 2 * 8 * 64;
	const LETTERS = 'ABCDEFGH';

	let socket = null;
	let reconnectTimer = null;
	let recordLit = false;		// grid recording: trig keys edit steps, so no auto track select then

	// ---- build the repeated parts of the panel ----

	function build() {
		const sound = document.getElementById('soundSelect');
		for (let row = 0; row < 2; ++row) {
			const r = document.createElement('div'); r.className = 'row';
			for (let i = 0; i < 8; ++i) {
				const t = row * 8 + i;
				const cell = document.createElement('div'); cell.className = 'cell';
				const led = document.createElement('div'); led.className = 'led'; led.id = 'drumLed' + t;
				const name = document.createElement('div'); name.className = 'name'; name.textContent = TRACK_NAMES[t];
				name.dataset.track = String(t);
				cell.appendChild(led); cell.appendChild(name); r.appendChild(cell);
			}
			sound.appendChild(r);
		}

		const data = document.getElementById('dataEntry');
		for (let row = 0; row < 2; ++row) {
			const r = document.createElement('div'); r.className = 'row';
			for (let i = 0; i < 4; ++i) {
				const k = row * 4 + i;
				const cell = document.createElement('div'); cell.className = 'cell';
				const knob = document.createElement('div'); knob.className = 'knob'; knob.id = 'enc' + LETTERS[k];
				knob.dataset.encoder = 'DataEntry' + LETTERS[k];
				const cap = document.createElement('div'); cap.className = 'cap'; knob.appendChild(cap);
				const caption = document.createElement('div'); caption.className = 'caption'; caption.textContent = LETTERS[k];
				cell.appendChild(knob); cell.appendChild(caption); r.appendChild(cell);
			}
			data.appendChild(r);
		}

		const strip = document.getElementById('stepStrip');
		for (let i = 0; i < 16; ++i) {
			const cell = document.createElement('div'); cell.className = 'cell';
			const led = document.createElement('div'); led.className = 'led'; led.id = 'stepLed' + i;
			const trig = document.createElement('div'); trig.className = 'trig'; trig.dataset.control = 'Trigger' + (i + 1);
			trig.dataset.track = String(i);
			const legend = document.createElement('div'); legend.className = 'legend' + ((i % 4) === 0 ? ' primary' : '');
			const num = document.createElement('div'); num.className = 'num'; num.textContent = String(i + 1);
			const name = document.createElement('div'); name.className = 'name'; name.textContent = TRACK_NAMES[i];
			legend.appendChild(num); legend.appendChild(name);
			cell.appendChild(led); cell.appendChild(trig); cell.appendChild(legend); strip.appendChild(cell);
		}
	}

	// ---- scaling to the screen ----

	let stageScale = 1;

	function layout() {
		const stage = document.getElementById('stage');
		const w = window.innerWidth, h = window.innerHeight;
		stageScale = Math.min(w / 1100, h / 570);
		const x = Math.floor((w - 1100 * stageScale) / 2), y = Math.floor((h - 570 * stageScale) / 2);
		stage.style.transform = 'translate(' + x + 'px,' + y + 'px) scale(' + stageScale + ')';
	}

	// ---- connection ----

	function send(msg) {
		if (socket && socket.readyState === WebSocket.OPEN) socket.send(msg);
	}

	function connect() {
		const proto = location.protocol === 'https:' ? 'wss://' : 'ws://';
		socket = new WebSocket(proto + location.host + '/ws');
		socket.binaryType = 'arraybuffer';
		socket.onopen = function () {
			document.getElementById('overlay').classList.add('hidden');
			send('hello');
		};
		socket.onmessage = function (ev) {
			if (ev.data instanceof ArrayBuffer) applyState(new Uint8Array(ev.data));
		};
		socket.onclose = function () {
			document.getElementById('overlay').classList.remove('hidden');
			document.getElementById('overlayText').textContent = 'Connection lost, retrying...';
			releaseAll();
			clearTimeout(reconnectTimer);
			reconnectTimer = setTimeout(connect, 1000);
		};
		socket.onerror = function () { socket.close(); };
	}

	// ---- state rendering ----

	const lcdCanvas = document.getElementById('lcd');
	const lcdCtx = lcdCanvas.getContext('2d');
	const lcdImage = lcdCtx.createImageData(128, 64);
	let lastVram = null;

	function drawLcd(vram) {
		const px = lcdImage.data;
		for (let y = 0; y < 64; ++y) {
			const page = y >> 3, bit = y & 7;
			for (let x = 0; x < 128; ++x) {
				const half = (x >> 6) & 1, col = x & 63;
				const on = ((vram[half * 512 + page * 64 + col] >> bit) & 1) !== 0;
				const o = (y * 128 + x) * 4;
				if (on) { px[o] = 0xf4; px[o + 1] = 0xa0; px[o + 2] = 0x6c; px[o + 3] = 255; }
				else { px[o] = 0x3a; px[o + 1] = 0x14; px[o + 2] = 0x0e; px[o + 3] = 255; }
			}
		}
		lcdCtx.putImageData(lcdImage, 0, 0);
	}

	function setLit(id, lit) {
		const el = document.getElementById(id);
		if (el) el.classList.toggle('lit', lit);
	}

	function applyState(bytes) {
		if (bytes.length < STATE_HEADER + VRAM_SIZE + 14 || bytes[0] !== 0x53) return;
		const vram = bytes.subarray(STATE_HEADER, STATE_HEADER + VRAM_SIZE);
		let changed = !lastVram;
		if (!changed) for (let i = 0; i < VRAM_SIZE; ++i) if (vram[i] !== lastVram[i]) { changed = true; break; }
		if (changed) { drawLcd(vram); lastVram = new Uint8Array(vram); }

		const leds = bytes.subarray(STATE_HEADER + VRAM_SIZE);
		const bank = function (cmd) { return leds[cmd - LED_BANK_FIRST]; };
		const lit = function (cmd, bit) { return ((bank(cmd) >> bit) & 1) === 0; };	// active low

		for (let i = 0; i < 16; ++i) {
			setLit('stepLed' + i, lit(i < 8 ? 0x20 : 0x21, i & 7));
			setLit('drumLed' + i, lit(i < 8 ? 0x24 : 0x25, i & 7));
		}
		// status bank 0x22
		setLit('mdPatternPage0', lit(0x22, 0));
		setLit('mdPatternPage1', lit(0x22, 1));
		setLit('mdPatternPage2', lit(0x22, 2));
		setLit('stPattern', lit(0x22, 3));
		setLit('stSong', lit(0x22, 4));
		setLit('stRoute', lit(0x22, 5));
		setLit('stFx', lit(0x22, 6));
		setLit('stSynth', lit(0x22, 7));
		// mode bank 0x23
		setLit('ledClassic', lit(0x23, 0));
		setLit('ledExtended', lit(0x23, 1));
		setLit('ledBankAD', lit(0x23, 2));
		setLit('ledBankEH', lit(0x23, 3));
		recordLit = lit(0x23, 4);
		setLit('ledRecord', recordLit);
		setLit('ledTempo', lit(0x23, 5));
		setLit('mdPatternPage3', lit(0x23, 6));
	}

	// ---- input ----

	const activeButtons = new Map();	// pointerId -> element

	function functionHeld() {
		for (const el of activeButtons.values()) if (el.dataset.control === 'Function') return true;
		return false;
	}

	function buttonDown(el) {
		el.classList.add('down');
		send('b ' + el.dataset.control + ' 1');
		// playing a trig key selects that sound, unless it is a chord or a grid edit
		if (el.dataset.track !== undefined && !functionHeld() && !recordLit)
			send('t ' + el.dataset.track);
	}
	function buttonUp(el) {
		el.classList.remove('down');
		send('b ' + el.dataset.control + ' 0');
	}

	function releaseAll() {
		activeButtons.forEach(function (el) { el.classList.remove('down'); });
		activeButtons.clear();
		encoders.forEach(function (st) { if (st.held) { st.held = false; st.el.classList.remove('held'); } });
		encoders.clear();
		xyFingers.forEach(function (f) { if (f.marker) f.marker.remove(); });
		xyFingers.clear();
	}

	function bindButtons() {
		document.querySelectorAll('[data-control]').forEach(function (el) {
			el.addEventListener('pointerdown', function (ev) {
				ev.preventDefault();
				el.setPointerCapture(ev.pointerId);
				activeButtons.set(ev.pointerId, el);
				buttonDown(el);
			});
			const up = function (ev) {
				if (!activeButtons.has(ev.pointerId)) return;
				activeButtons.delete(ev.pointerId);
				buttonUp(el);
			};
			el.addEventListener('pointerup', up);
			el.addEventListener('pointercancel', up);
			el.addEventListener('lostpointercapture', up);
		});
		// the sound selection names select their track directly
		document.querySelectorAll('.soundSelect .name').forEach(function (el) {
			el.addEventListener('pointerdown', function (ev) {
				ev.preventDefault();
				send('t ' + el.dataset.track);
			});
		});
	}

	// encoders: drag vertically (or horizontally) to turn, tap to push, hold still then drag to turn while pushed.
	// The sound selection wheel is turned around its centre like the real jog wheel, one track per 24 degrees.
	const encoders = new Map();	// pointerId -> state
	const STEP_PIXELS = 3;
	const WHEEL_DEGREES = 24;
	const TAP_MS = 250, HOLD_MS = 350, MOVE_DEAD = 4;

	function bindEncoders() {
		document.querySelectorAll('[data-encoder]').forEach(function (el) {
			const cap = el.querySelector('.cap');
			const wheel = el.classList.contains('wheel');
			let wheelAngle = 0;

			const angleOf = function (ev) {
				const r = el.getBoundingClientRect();
				return Math.atan2(ev.clientY - (r.top + r.height / 2), ev.clientX - (r.left + r.width / 2)) * 180 / Math.PI;
			};

			el.addEventListener('pointerdown', function (ev) {
				ev.preventDefault();
				el.setPointerCapture(ev.pointerId);
				const st = { el: el, x: ev.clientX, y: ev.clientY, acc: 0, moved: false, held: false, t: Date.now(), holdTimer: null,
					angle: wheel ? angleOf(ev) : 0 };
				st.holdTimer = setTimeout(function () {
					if (!st.moved && encoders.get(ev.pointerId) === st) {
						st.held = true; el.classList.add('held');
						send('p ' + el.dataset.encoder + ' 1');
					}
				}, HOLD_MS);
				encoders.set(ev.pointerId, st);
			});
			el.addEventListener('pointermove', function (ev) {
				const st = encoders.get(ev.pointerId);
				if (!st) return;
				let steps = 0;
				if (wheel) {
					const a = angleOf(ev);
					let d = a - st.angle;
					if (d > 180) d -= 360; else if (d < -180) d += 360;
					st.angle = a;
					st.acc += d;
					if (!st.moved && Math.abs(st.acc) > 6) { st.moved = true; if (!st.held) clearTimeout(st.holdTimer); }
					if (!st.moved) return;
					steps = Math.trunc(st.acc / WHEEL_DEGREES);
					if (steps !== 0) {
						st.acc -= steps * WHEEL_DEGREES;
						wheelAngle += steps * WHEEL_DEGREES;
						cap.style.transform = 'rotate(' + wheelAngle + 'deg)';
					}
				} else {
					const dx = ev.clientX - st.x, dy = ev.clientY - st.y;
					st.x = ev.clientX; st.y = ev.clientY;
					st.acc += (Math.abs(dy) >= Math.abs(dx) ? -dy : dx);		// up or right = clockwise
					if (!st.moved && Math.abs(st.acc) > MOVE_DEAD) { st.moved = true; if (!st.held) clearTimeout(st.holdTimer); }
					if (!st.moved) return;
					steps = Math.trunc(st.acc / STEP_PIXELS);
					if (steps !== 0) st.acc -= steps * STEP_PIXELS;
				}
				if (steps !== 0) send('e ' + el.dataset.encoder + ' ' + steps);
			});
			const up = function (ev) {
				const st = encoders.get(ev.pointerId);
				if (!st) return;
				encoders.delete(ev.pointerId);
				clearTimeout(st.holdTimer);
				if (st.held) {
					st.held = false; el.classList.remove('held');
					send('p ' + el.dataset.encoder + ' 0');
				} else if (!st.moved && Date.now() - st.t < TAP_MS) {
					send('p ' + el.dataset.encoder + ' 1');
					setTimeout(function () { send('p ' + el.dataset.encoder + ' 0'); }, 60);
				}
			};
			el.addEventListener('pointerup', up);
			el.addEventListener('pointercancel', up);
		});
	}

	// ---- four-finger XY mode: finger n moves encoder A+n left/right and E+n up/down ----

	const xyFingers = new Map();	// pointerId -> { slot, x, y, accX, accY, marker }
	let xyActive = false;

	function bindXy() {
		const toggle = document.getElementById('xyToggle');
		const area = document.getElementById('xy');
		toggle.addEventListener('pointerdown', function (ev) {
			ev.preventDefault(); ev.stopPropagation();
			xyActive = !xyActive;
			toggle.classList.toggle('on', xyActive);
			area.classList.toggle('hidden', !xyActive);
			if (!xyActive) { xyFingers.forEach(function (f) { if (f.marker) f.marker.remove(); }); xyFingers.clear(); }
		});

		const freeSlot = function () {
			const used = new Set(); xyFingers.forEach(function (f) { used.add(f.slot); });
			for (let s = 0; s < 4; ++s) if (!used.has(s)) return s;
			return -1;
		};
		const toPanel = function (ev) {
			const r = area.getBoundingClientRect();
			return { x: (ev.clientX - r.left) / stageScale, y: (ev.clientY - r.top) / stageScale };
		};

		area.addEventListener('pointerdown', function (ev) {
			ev.preventDefault();
			const slot = freeSlot();
			if (slot < 0) return;
			area.setPointerCapture(ev.pointerId);
			const p = toPanel(ev);
			const marker = document.createElement('div'); marker.className = 'xyFinger';
			marker.textContent = LETTERS[slot] + '/' + LETTERS[slot + 4];
			marker.style.left = p.x + 'px'; marker.style.top = p.y + 'px';
			area.appendChild(marker);
			xyFingers.set(ev.pointerId, { slot: slot, x: ev.clientX, y: ev.clientY, accX: 0, accY: 0, marker: marker });
		});
		area.addEventListener('pointermove', function (ev) {
			const f = xyFingers.get(ev.pointerId);
			if (!f) return;
			f.accX += ev.clientX - f.x; f.accY -= ev.clientY - f.y;	// up = increase
			f.x = ev.clientX; f.y = ev.clientY;
			const sx = Math.trunc(f.accX / STEP_PIXELS), sy = Math.trunc(f.accY / STEP_PIXELS);
			if (sx !== 0) { f.accX -= sx * STEP_PIXELS; send('e DataEntry' + LETTERS[f.slot] + ' ' + sx); }
			if (sy !== 0) { f.accY -= sy * STEP_PIXELS; send('e DataEntry' + LETTERS[f.slot + 4] + ' ' + sy); }
			const p = toPanel(ev);
			f.marker.style.left = p.x + 'px'; f.marker.style.top = p.y + 'px';
		});
		const up = function (ev) {
			const f = xyFingers.get(ev.pointerId);
			if (!f) return;
			xyFingers.delete(ev.pointerId);
			if (f.marker) f.marker.remove();
		};
		area.addEventListener('pointerup', up);
		area.addEventListener('pointercancel', up);
	}

	// ---- go ----

	build();
	bindButtons();
	bindEncoders();
	bindXy();
	layout();
	window.addEventListener('resize', layout);
	window.addEventListener('orientationchange', function () { setTimeout(layout, 100); });
	document.addEventListener('contextmenu', function (ev) { ev.preventDefault(); });
	document.addEventListener('gesturestart', function (ev) { ev.preventDefault(); });
	connect();
})();

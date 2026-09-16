/* Machinedrum / Monomachine remote panel: renders the machine's LCD and LEDs from the state stream
   and sends button, encoder and encoder-push events back over a WebSocket. Multitouch: every
   pointer is tracked on its own, so FUNCTION + trig chords work with two fingers.
   The page says which machine it draws via <body data-model="md|mm">. */
(function () {
	'use strict';

	const MODEL = document.body.dataset.model === 'mm' ? 'mm' : 'md';
	const IS_MM = MODEL === 'mm';
	const MODEL_BYTE = IS_MM ? 1 : 0;				// md::MachineModel
	const OTHER_PAGE = IS_MM ? 'index.html' : 'index-mm.html';
	const DEFAULT_PORT = { md: 8790, mm: 8792 };
	const TRACK_NAMES = ['BD', 'SD', 'HT', 'MT', 'LT', 'CP', 'RS', 'CB', 'CH', 'OH', 'RC', 'CC', 'M1', 'M2', 'M3', 'M4'];
	const LED_BANK_FIRST = 0x20;
	const STATE_HEADER = 2;
	const VRAM_SIZE = 2 * 8 * 64;
	const LETTERS = 'ABCDEFGH';
	const LCD_ON = IS_MM ? [0x1a, 0x2b, 0x1e] : [0xf4, 0xa0, 0x6c];
	const LCD_OFF = IS_MM ? [0xb9, 0xc8, 0xb2] : [0x3a, 0x14, 0x0e];

	let socket = null;
	let reconnectTimer = null;
	let recordLit = false;		// grid recording: trig keys edit steps, so no auto track select then
	let currentPage = -1;		// Monomachine: lit EDIT page LED

	// ---- build the repeated parts of the panel ----

	function build() {
		const sound = document.getElementById('soundSelect');
		if (sound) {
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
		}

		const tracks = document.getElementById('trackStack');
		if (tracks) {
			for (let t = 0; t < 6; ++t) {
				const row = document.createElement('div'); row.className = 'trackRow';
				const rule = document.createElement('div'); rule.className = 'trackRule';
				const led = document.createElement('div'); led.className = 'led'; led.id = 'drumLed' + t;
				const label = document.createElement('div'); label.className = 'trackLabel'; label.innerHTML = 'TRACK<br>' + (t + 1);
				const key = document.createElement('div'); key.className = 'key light'; key.dataset.control = 'Track' + (t + 1);
				const tab = document.createElement('div'); tab.className = 'trackTab'; tab.innerHTML = 'M<br>U<br>T<br>E';
				row.appendChild(rule); row.appendChild(led); row.appendChild(label); row.appendChild(key); row.appendChild(tab);
				tracks.appendChild(row);
			}
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
			if (!IS_MM) trig.dataset.track = String(i);		// Machinedrum: one sound per trig key
			const legend = document.createElement('div'); legend.className = 'legend' + ((i % 4) === 0 ? ' primary' : '');
			if (IS_MM) {
				legend.textContent = String(i + 1);
			} else {
				const num = document.createElement('div'); num.className = 'num'; num.textContent = String(i + 1);
				const name = document.createElement('div'); name.className = 'name'; name.textContent = TRACK_NAMES[i];
				legend.appendChild(num); legend.appendChild(name);
			}
			cell.appendChild(led); cell.appendChild(trig); cell.appendChild(legend); strip.appendChild(cell);
		}
	}

	// ---- scaling to the screen ----

	let stageScale = 1;

	function layout() {
		const stage = document.getElementById('stage');
		const w = window.innerWidth, h = window.innerHeight;
		const stageHeight = document.getElementById('rack') ? 766 : 570;
		stageScale = Math.min(w / 1100, h / stageHeight);
		const x = Math.floor((w - 1100 * stageScale) / 2), y = Math.floor((h - stageHeight * stageScale) / 2);
		stage.style.transform = 'translate(' + x + 'px,' + y + 'px) scale(' + stageScale + ')';
	}

	// ---- connection ----

	function send(msg) {
		if (socket && socket.readyState === WebSocket.OPEN) socket.send(msg);
	}
	window.remotePanelSend = send;

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
			else if (typeof ev.data === 'string' && ev.data.charAt(0) === 'M' && window.remotePanelMachineInfo)
				window.remotePanelMachineInfo(ev.data.substring(2));
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
				const c = on ? LCD_ON : LCD_OFF;
				const o = (y * 128 + x) * 4;
				px[o] = c[0]; px[o + 1] = c[1]; px[o + 2] = c[2]; px[o + 3] = 255;
			}
		}
		lcdCtx.putImageData(lcdImage, 0, 0);
	}

	function setLit(id, lit) {
		const el = document.getElementById(id);
		if (el) el.classList.toggle('lit', lit);
	}

	function setColor(id, green, red) {
		const el = document.getElementById(id);
		if (!el) return;
		el.classList.toggle('green', green && !red);
		el.classList.toggle('red', red && !green);
		el.classList.toggle('yellow', green && red);
	}

	let modelChecked = false;

	function applyState(bytes) {
		if (bytes.length < STATE_HEADER + VRAM_SIZE + 14 || bytes[0] !== 0x53) return;
		if (!modelChecked) {
			modelChecked = true;
			// opened the wrong page for this machine, e.g. index.html on the Monomachine port
			if (bytes[1] !== MODEL_BYTE) { location.replace(OTHER_PAGE); return; }
		}
		const vram = bytes.subarray(STATE_HEADER, STATE_HEADER + VRAM_SIZE);
		let changed = !lastVram;
		if (!changed) for (let i = 0; i < VRAM_SIZE; ++i) if (vram[i] !== lastVram[i]) { changed = true; break; }
		if (changed) { drawLcd(vram); lastVram = new Uint8Array(vram); }

		const leds = bytes.subarray(STATE_HEADER + VRAM_SIZE);
		const bank = function (cmd) { return leds[cmd - LED_BANK_FIRST]; };
		const lit = function (cmd, bit) { return ((bank(cmd) >> bit) & 1) === 0; };	// active low

		if (IS_MM) {
			// four bicolour step LEDs per bank: even bit green, odd bit red
			for (let i = 0; i < 16; ++i) {
				const b = 0x20 + (i >> 2), g = (i & 3) * 2;
				setColor('stepLed' + i, lit(b, g), lit(b, g + 1));
			}
			const tracks = [[0x25, 0], [0x25, 2], [0x24, 0], [0x24, 2], [0x24, 4], [0x24, 6]];
			for (let t = 0; t < 6; ++t) setColor('drumLed' + t, lit(tracks[t][0], tracks[t][1]), lit(tracks[t][0], tracks[t][1] + 1));
			currentPage = -1;
			for (let p = 0; p < 7; ++p) {
				const on = p < 4 ? lit(0x25, 4 + p) : lit(0x26, p - 4);
				setLit('mmPageLed' + p, on);
				if (on && currentPage < 0) currentPage = p;
			}
			setLit('mmBankGroupAD', lit(0x26, 3));
			setLit('mmBankGroupEH', lit(0x26, 4));
			setLit('stPattern', lit(0x26, 5));
			setLit('stSong', lit(0x26, 6));
			setLit('mmTempoLed', lit(0x26, 7));
			recordLit = lit(0x27, 0);
			setLit('mmRecordLed', recordLit);
			setLit('mmTrigAmp', lit(0x27, 1));
			setLit('mmTrigFilter', lit(0x27, 2));
			setLit('mmTrigLfo', lit(0x27, 3));
			for (let p = 0; p < 4; ++p) setLit('mmTrackPage' + p, lit(0x27, 4 + p));
			return;
		}

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
		// Machinedrum: playing a trig key selects that sound, unless it is a chord or a grid edit
		if (el.dataset.track !== undefined && !functionHeld() && !recordLit)
			send('t ' + el.dataset.track);
	}
	function buttonUp(el) {
		el.classList.remove('down');
		send('b ' + el.dataset.control + ' 0');
	}

	// lets go of everything, telling the machine when the connection is still up
	function releaseAll() {
		activeButtons.forEach(function (el) { buttonUp(el); });
		activeButtons.clear();
		encoders.forEach(function (st) { if (st.held) { st.held = false; st.el.classList.remove('held'); send('p ' + st.el.dataset.encoder + ' 0'); } });
		encoders.clear();
		xyFingers.forEach(function (f) { if (f.marker) f.marker.remove(); });
		xyFingers.clear();
		updateXyZones();
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
		// the Machinedrum sound selection names select their track directly
		document.querySelectorAll('.soundSelect .name').forEach(function (el) {
			el.addEventListener('pointerdown', function (ev) {
				ev.preventDefault();
				send('t ' + el.dataset.track);
			});
		});
		// the Monomachine EDIT page names step the page arrows until that page is lit
		document.querySelectorAll('.pageSelect').forEach(function (el) {
			el.addEventListener('pointerdown', function (ev) {
				ev.preventDefault();
				const target = parseInt(el.dataset.page, 10);
				if (currentPage < 0 || target === currentPage) return;
				const control = target > currentPage ? 'DataPageForward' : 'DataPageBackward';
				const steps = Math.abs(target - currentPage);
				for (let i = 0; i < steps; ++i) {
					setTimeout(function () { send('b ' + control + ' 1'); }, i * 160);
					setTimeout(function () { send('b ' + control + ' 0'); }, i * 160 + 60);
				}
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

	// ---- XY pad: four equal zones AE, BF, CG, DH from left to right. A finger controls the pair of
	// the zone it lands in (left/right = top-row encoder, up/down = bottom-row encoder) and keeps
	// that pair while it moves, even across zone borders. ----

	const xyFingers = new Map();	// pointerId -> { slot, x, y, accX, accY, marker }
	const xyArea = document.getElementById('xy');
	const xyZones = [];

	function updateXyZones() {
		const active = [0, 0, 0, 0];
		xyFingers.forEach(function (f) { ++active[f.slot]; });
		xyZones.forEach(function (zone, i) { zone.classList.toggle('active', active[i] > 0); });
	}

	function bindXy() {
		const area = xyArea;
		const hint = area.querySelector('.xyHint');
		if (hint) hint.remove();
		for (let i = 0; i < 4; ++i) {
			const zone = document.createElement('div');
			zone.className = 'xyZone';
			zone.style.left = (i * 25) + '%';
			const label = document.createElement('div');
			label.className = 'xyZoneLabel';
			label.textContent = LETTERS[i] + LETTERS[i + 4];
			zone.appendChild(label);
			area.appendChild(zone);
			xyZones.push(zone);
		}

		const toPanel = function (ev) {
			const r = area.getBoundingClientRect();
			return { x: (ev.clientX - r.left) / stageScale, y: (ev.clientY - r.top) / stageScale };
		};
		const zoneOf = function (ev) {
			const r = area.getBoundingClientRect();
			const u = r.width > 0 ? (ev.clientX - r.left) / r.width : 0;
			return Math.max(0, Math.min(3, Math.floor(u * 4)));
		};

		area.addEventListener('pointerdown', function (ev) {
			ev.preventDefault();
			const slot = zoneOf(ev);
			try { area.setPointerCapture(ev.pointerId); } catch (e) { }
			const p = toPanel(ev);
			const marker = document.createElement('div'); marker.className = 'xyFinger';
			marker.textContent = LETTERS[slot] + LETTERS[slot + 4];
			marker.style.left = p.x + 'px'; marker.style.top = p.y + 'px';
			area.appendChild(marker);
			xyFingers.set(ev.pointerId, { slot: slot, x: ev.clientX, y: ev.clientY, accX: 0, accY: 0, marker: marker });
			updateXyZones();
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
			updateXyZones();
		};
		area.addEventListener('pointerup', up);
		area.addEventListener('pointercancel', up);
	}

	// ---- switching between the two machines: a button, or a four-finger swipe outside the XY pad ----

	function otherMachineUrl() {
		const other = IS_MM ? 'md' : 'mm';
		let port = DEFAULT_PORT[other];
		try { const saved = localStorage.getItem('port.' + other); if (saved) port = parseInt(saved, 10); } catch (e) { }
		return location.protocol + '//' + location.hostname + ':' + port + '/';
	}

	let switching = false;

	function switchMachine() {
		if (switching) return;
		switching = true;
		const button = document.getElementById('switchMachine');
		if (button) button.classList.add('busy');
		const url = otherMachineUrl();
		// only leave when the other machine answers, a dead page on a tablet is a nuisance
		const ctrl = new AbortController();
		const timer = setTimeout(function () { ctrl.abort(); }, 1500);
		fetch(url + 'panel.css', { mode: 'no-cors', cache: 'no-store', signal: ctrl.signal }).then(function () {
			clearTimeout(timer);
			releaseAll();
			location.href = url;
		}).catch(function () {
			clearTimeout(timer);
			switching = false;
			if (button) { button.classList.remove('busy'); button.textContent = 'no ' + (IS_MM ? 'MD' : 'MM'); setTimeout(function () { button.innerHTML = (IS_MM ? 'MD' : 'MM') + ' &rarr;'; }, 1500); }
		});
	}

	function bindSwitch() {
		try { localStorage.setItem('port.' + MODEL, location.port || (location.protocol === 'https:' ? '443' : '80')); } catch (e) { }
		const button = document.getElementById('switchMachine');
		if (button) button.addEventListener('pointerdown', function (ev) { ev.preventDefault(); ev.stopPropagation(); switchMachine(); });

		// four fingers swiping sideways, none of them on the XY pad
		let swipe = null;
		document.addEventListener('touchstart', function (ev) {
			if (ev.touches.length > 1) ev.preventDefault();
			if (ev.touches.length === 4) {
				let onPad = false, sx = 0, sy = 0;
				for (let i = 0; i < 4; ++i) {
					const t = ev.touches[i];
					if (xyArea && xyArea.contains(t.target)) onPad = true;
					sx += t.clientX; sy += t.clientY;
				}
				swipe = onPad ? null : { x: sx / 4, y: sy / 4 };
			} else if (ev.touches.length > 4) swipe = null;
		}, { passive: false });
		document.addEventListener('touchmove', function (ev) {
			ev.preventDefault();
			if (!swipe || ev.touches.length !== 4) return;
			let sx = 0, sy = 0;
			for (let i = 0; i < 4; ++i) { sx += ev.touches[i].clientX; sy += ev.touches[i].clientY; }
			const dx = sx / 4 - swipe.x, dy = sy / 4 - swipe.y;
			if (Math.abs(dx) > 100 && Math.abs(dy) < 80) { swipe = null; switchMachine(); }
		}, { passive: false });
		document.addEventListener('touchend', function (ev) { if (ev.touches.length < 4) swipe = null; });
		document.addEventListener('touchcancel', function () { swipe = null; });
	}

	// ---- go ----

	build();
	bindButtons();
	bindEncoders();
	bindXy();
	bindSwitch();
	layout();
	window.addEventListener('resize', layout);
	window.addEventListener('orientationchange', function () { setTimeout(layout, 100); });
	document.addEventListener('contextmenu', function (ev) { ev.preventDefault(); });
	document.addEventListener('gesturestart', function (ev) { ev.preventDefault(); });
	connect();
})();

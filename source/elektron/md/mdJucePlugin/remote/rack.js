/* Machine rack for the remote panel: family tabs and machine tiles below the faceplate.
   The plugin pushes the catalogue and state as "M <json>"; a confirmed tile sends "m <id>". */
(function () {
	'use strict';

	const root = document.getElementById('rack');
	if (!root) return;

	let info = null;
	let family = null;
	let lastAssigned = -1;
	let pending = null;
	const storageKey = 'rackFamily.' + (document.body.dataset.model || 'md');

	function el(tag, cls, text) {
		const e = document.createElement(tag);
		if (cls) e.className = cls;
		if (text !== undefined) e.textContent = text;
		return e;
	}

	// ---- static structure ----
	const header = el('div', 'rackHeader');
	const title = el('div', 'rackTitle');
	title.appendChild(el('span', 'rackDot'));
	title.appendChild(document.createTextNode('MACHINES'));
	const track = el('div', 'rackTrack', 'TRACK --');
	const tabs = el('div', 'rackTabs');
	header.appendChild(title); header.appendChild(track); header.appendChild(tabs);
	const accent = el('div', 'rackAccent');
	const grid = el('div', 'rackGrid');
	const hint = el('div', 'rackHint', 'Connecting...');
	root.appendChild(header); root.appendChild(accent); root.appendChild(grid); root.appendChild(hint);

	const modal = el('div', 'rackModal hidden');
	const card = el('div', 'rackCard');
	const cardCode = el('div', 'rackCardCode');
	const cardTitle = el('div', 'rackCardTitle');
	const cardSub = el('div', 'rackCardSub');
	const cardDesc = el('div', 'rackCardDesc');
	const cardButtons = el('div', 'rackCardButtons');
	const cancel = el('div', 'rackButton cancel', 'Cancel');
	const load = el('div', 'rackButton load', 'Load');
	cardButtons.appendChild(cancel); cardButtons.appendChild(load);
	card.appendChild(cardCode); card.appendChild(cardTitle); card.appendChild(cardSub); card.appendChild(cardDesc); card.appendChild(cardButtons);
	modal.appendChild(card);
	document.body.appendChild(modal);

	const toast = el('div', 'rackToast hidden');
	document.body.appendChild(toast);
	let toastTimer = null;

	function tap(element, handler) {
		let armed = false;
		element.addEventListener('pointerdown', function (ev) { ev.preventDefault(); ev.stopPropagation(); armed = true; element.classList.add('pressed'); });
		element.addEventListener('pointerup', function (ev) { ev.preventDefault(); ev.stopPropagation(); element.classList.remove('pressed'); if (armed) handler(); armed = false; });
		element.addEventListener('pointercancel', function () { armed = false; element.classList.remove('pressed'); });
		element.addEventListener('pointerleave', function () { armed = false; element.classList.remove('pressed'); });
	}

	tap(cancel, function () { closeModal(); });
	tap(load, function () {
		if (pending) {
			window.remotePanelSend('m ' + pending.machine.id);
			lastAssigned = pending.machine.id;
			showToast(pending.machine.name + ' loaded on ' + trackLabel(), pending.family.color);
			renderTiles();
		}
		closeModal();
	});
	modal.addEventListener('pointerdown', function (ev) { if (ev.target === modal) { ev.preventDefault(); closeModal(); } });

	function closeModal() {
		pending = null;
		modal.classList.add('hidden');
	}

	function showToast(text, color) {
		toast.textContent = text;
		toast.style.setProperty('--accent', color || '#ff9d3a');
		toast.classList.remove('hidden');
		clearTimeout(toastTimer);
		toastTimer = setTimeout(function () { toast.classList.add('hidden'); }, 2200);
	}

	function trackLabel() {
		if (!info || info.track < 0) return 'the current track';
		return info.model === 'md' ? 'track ' + (info.track + 1) + ' · ' + info.trackName : 'track ' + (info.track + 1);
	}

	function openModal(fam, machine) {
		if (!info || info.track < 0) {
			showToast('The current track is not known yet', '#ff5f5f');
			return;
		}
		pending = { family: fam, machine: machine };
		card.style.setProperty('--accent', fam.color);
		cardCode.textContent = machine.code;
		cardTitle.textContent = 'Load ' + machine.name;
		cardSub.textContent = 'on ' + trackLabel();
		let desc = machine.desc || fam.title;
		if (fam.name === 'ROM' && info.slots) {
			const name = info.slots[machine.slot];
			desc = name ? 'Plays sample slot R' + String(machine.slot + 1).padStart(2, '0') + ': ' + name : 'Sample slot R' + String(machine.slot + 1).padStart(2, '0') + ' is empty';
		}
		cardDesc.textContent = desc + '. The track’s current machine is replaced.';
		modal.classList.remove('hidden');
	}

	function renderTabs() {
		tabs.textContent = '';
		info.families.forEach(function (fam) {
			const tab = el('div', 'rackTab' + (family === fam.name ? ' active' : ''), fam.name);
			tab.style.setProperty('--accent', fam.color);
			tap(tab, function () {
				family = fam.name;
				try { localStorage.setItem(storageKey, family); } catch (e) { }
				renderTabs();
				renderTiles();
			});
			tabs.appendChild(tab);
		});
	}

	function renderTiles() {
		const fam = info.families.find(function (f) { return f.name === family; }) || info.families[0];
		root.style.setProperty('--accent', fam.color);
		grid.textContent = '';
		const rom = fam.name === 'ROM';
		grid.classList.toggle('compact', rom);
		hint.textContent = fam.title + ' · tap a machine to load it on ' + trackLabel();
		if (fam.machines.length === 0) {
			grid.appendChild(el('div', 'rackEmpty', 'These machines need a community OS such as Machinedrum X.13 (plugin settings, Firmware).'));
			return;
		}
		fam.machines.forEach(function (machine) {
			const tile = el('div', 'rackTile');
			tile.style.setProperty('--accent', fam.color);
			let label = machine.name;
			let empty = false;
			if (rom) {
				const slotName = info.slots && info.slots.length ? info.slots[machine.slot] : null;
				empty = slotName === '';
				label = slotName === null ? '' : (empty ? 'empty' : slotName);
			}
			tile.classList.toggle('empty', empty);
			tile.classList.toggle('assigned', machine.id === lastAssigned);
			tile.appendChild(el('div', 'rackTileCode', machine.code));
			tile.appendChild(el('div', 'rackTileName', label));
			tap(tile, function () { openModal(fam, machine); });
			grid.appendChild(tile);
		});
	}

	window.remotePanelMachineInfo = function (json) {
		let next;
		try { next = JSON.parse(json); } catch (e) { return; }
		const firstTime = !info;
		const trackChanged = !info || info.track !== next.track;
		info = next;
		if (firstTime) {
			try { family = localStorage.getItem(storageKey); } catch (e) { family = null; }
			if (!info.families.some(function (f) { return f.name === family; }))
				family = info.families[Math.min(1, info.families.length - 1)].name;
		}
		track.textContent = info.track < 0 ? 'TRACK --' : (info.model === 'md' ? 'TRACK ' + (info.track + 1) + ' · ' + info.trackName : 'TRACK ' + (info.track + 1));
		if (trackChanged && !firstTime) {
			track.classList.remove('flash');
			void track.offsetWidth;
			track.classList.add('flash');
			lastAssigned = -1;
		}
		renderTabs();
		renderTiles();
	};
})();

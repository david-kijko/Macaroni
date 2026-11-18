// SPDX-License-Identifier: GPL-3.0-or-later
import { BackendClient } from '../api/BackendClient.js';

export function openRoom(context) {
    if (document.querySelector('dialog.macaroni-room')) return;
    if (document.pointerLockElement) document.exitPointerLock();
    const previousFocus = /** @type {HTMLElement} */ (document.activeElement);
    const dialog = document.createElement('dialog');
    dialog.className = 'macaroni-room';
    dialog.setAttribute('aria-label', 'Room chat');
    const frame = document.createElement('iframe');
    frame.title = 'Macaroni room chat';
    dialog.append(frame);
    (document.fullscreenElement || document.body).append(dialog);
    dialog.showModal();
    // The composer has its own document so stream keyboard listeners cannot
    // forward typed messages to the host, even while gaming input is active.
    const doc = frame.contentDocument;
    const styles = doc.createElement('link');
    styles.rel = 'stylesheet';
    styles.href = new URL('/css/macaroni.css', document.baseURI).href;
    doc.head.append(styles);
    doc.body.className = 'room-body';
    doc.body.innerHTML = `<header><img src="/assets/logo.png" alt="" width="32" height="32">
        <h1>Room chat</h1><button type="button" aria-label="Close chat">×</button></header>
        <p class="room-people" aria-live="polite"></p>
        <div class="room-control" hidden><button type="button" class="control-action"></button>
        <label hidden>Give control <select></select></label><span aria-live="polite"></span></div>
        <ol aria-label="Messages" role="log"></ol>
        <p class="room-status" role="status"></p>
        <form><label for="room-text">Message</label>
        <div><input id="room-text" maxlength="1000" autocomplete="off" placeholder="Write a message…">
        <button type="submit">Send</button></div></form>`;
    const people = doc.querySelector('.room-people');
    const status = doc.querySelector('.room-status');
    const list = doc.querySelector('ol');
    const input = doc.querySelector('input');
    const send = /** @type {HTMLButtonElement} */ (doc.querySelector('[type=submit]'));
    const path = context.token ? '/api/share/player/chat' : '/api/share/chat';
    let timer,
        latest = 0,
        painted = '',
        stopped = false;
    function close() {
        dialog.close();
    }
    dialog.addEventListener(
        'close',
        () => {
            stopped = true;
            clearTimeout(timer);
            dialog.remove();
            previousFocus?.focus?.();
        },
        { once: true },
    );
    doc.querySelector('header button').addEventListener('click', close);
    doc.addEventListener('keydown', (e) => {
        if (e.key === 'Escape') {
            e.preventDefault();
            close();
        }
    });
    function render(data) {
        people.textContent = 'In chat · ' + data.participants.join(', ');
        const next = JSON.stringify(data.messages);
        if (next === painted) return;
        painted = next;
        const atEnd = list.scrollHeight - list.scrollTop - list.clientHeight < 32;
        list.replaceChildren(
            ...data.messages.map((m) => {
                const li = doc.createElement('li');
                const name = doc.createElement('strong');
                name.textContent = m.name;
                const time = doc.createElement('time');
                time.dateTime = new Date(m.time).toISOString();
                time.textContent = new Date(m.time).toLocaleTimeString([], {
                    hour: '2-digit',
                    minute: '2-digit',
                });
                const text = doc.createElement('p');
                text.textContent = m.text;
                li.append(name, time, text);
                return li;
            }),
        );
        if (atEnd) list.scrollTop = list.scrollHeight;
    }
    async function update(text) {
        const turn = ++latest;
        try {
            const data = await BackendClient.post(
                path,
                text === undefined ? context : { ...context, text },
                { timeoutMs: 6500 },
            );
            if (!stopped && turn === latest) {
                render(data);
                status.textContent = data.messages.length ? '' : 'No messages yet.';
            }
            return true;
        } catch (e) {
            if (stopped || turn !== latest) return false;
            const code = e.statusCode;
            status.textContent =
                code === 403
                    ? 'This invitation has ended or needs its PIN.'
                    : code === 409
                      ? 'Chat opens when the first guest joins.'
                      : code === 429
                        ? 'Wait a moment before sending again.'
                        : 'Chat is unavailable. Check the connection.';
            if (code === 403 || code === 409) {
                list.replaceChildren();
                painted = '';
                people.textContent = '';
                if (code === 403) {
                    stopped = true;
                    input.disabled = send.disabled = true;
                }
            }
            return false;
        }
    }
    const controls = doc.querySelector('.room-control');
    const take = /** @type {HTMLButtonElement} */ (controls.querySelector('button'));
    const transfer = controls.querySelector('select');
    const controlStatus = controls.querySelector('span');
    let controlBusy = false,
        controlSeq = 0,
        holding = false;
    async function control(action, to = '') {
        if (!context.token || stopped || controlBusy) return;
        const seq = ++controlSeq;
        if (action !== 'status') controlBusy = true;
        try {
            const state = await BackendClient.post(
                '/api/share/player/control',
                { token: context.token, action, to },
                { timeoutMs: 6500 },
            );
            if (seq !== controlSeq || stopped) return;
            controls.hidden = !state.available;
            if (!state.available) return;
            holding = state.holding;
            take.disabled = !holding && (!state.allowed || state.pending);
            take.textContent = holding ? 'Release control' : 'Request control';
            controlStatus.textContent = holding
                ? 'You have control'
                : state.pending
                  ? 'Control requested'
                  : state.allowed
                    ? 'Viewing'
                    : 'Guest requests are locked';
            transfer.parentElement.hidden = !holding || !state.others.length;
            const options = JSON.stringify(state.others);
            if (transfer.dataset.options !== options) {
                transfer.dataset.options = options;
                transfer.replaceChildren(
                    new Option('Choose a participant', ''),
                    ...state.others.map(
                        (m) =>
                            new Option(
                                m.name + (m.requested ? ' · Requests control' : ''),
                                m.opening,
                            ),
                    ),
                );
            }
        } catch (err) {
            if (seq === controlSeq && !stopped && action !== 'status')
                controlStatus.textContent = err.responseBody?.error || 'Control is unavailable.';
        } finally {
            if (action !== 'status') controlBusy = false;
        }
    }
    take.onclick = () => control(holding ? 'release' : 'request');
    transfer.onchange = () => {
        const picked = transfer.value;
        transfer.value = '';
        if (picked) void control('give', picked);
    };
    async function poll() {
        if (stopped) return;
        if (!send.disabled) await Promise.all([update(), control('status')]);
        if (!stopped) timer = setTimeout(poll, 2000);
    }
    doc.querySelector('form').onsubmit = async (e) => {
        e.preventDefault();
        const draft = input.value;
        if (send.disabled || !draft.trim()) return;
        send.disabled = true;
        if ((await update(draft)) && input.value === draft) input.value = '';
        send.disabled = stopped;
        input.focus();
    };
    input.focus();
    void poll();
}

// SPDX-License-Identifier: GPL-3.0-or-later
import { BackendClient } from '../api/BackendClient.js';
import { openRoom } from './RoomChat.js';

function element(tag, text, cls) {
    const el = document.createElement(tag);
    if (text) el.textContent = text;
    if (cls) el.className = cls;
    return el;
}
function button(text, action) {
    const el = element('button', text, 'btn btn-secondary');
    el.type = 'button';
    el.addEventListener('click', action);
    return el;
}
export function guestLanding(container, invite) {
    container.replaceChildren();
    const page = element('main', '', 'player-page');
    const card = element('section', '', 'player-card');
    const mark = element('img');
    mark.src = '/assets/logo.png';
    mark.alt = '';
    mark.width = mark.height = 42;
    const title = element('h1', 'Join Macaroni');
    const form = element('form', '', 'hands-join');
    const label = element('label', 'Your name');
    const input = element('input');
    input.required = true;
    input.maxLength = 60;
    input.autocomplete = 'nickname';
    label.append(input);
    const join = element('button', 'Join room', 'btn btn-open');
    const status = element('p');
    status.setAttribute('role', 'status');
    form.append(label, join, status);
    card.append(mark, title, form);
    page.append(card);
    container.append(page);
    form.onsubmit = async (event) => {
        event.preventDefault();
        if (join.disabled) return;
        join.disabled = true;
        try {
            const { token } = await BackendClient.post('/api/share/player/claim', {
                invite,
                name: input.value,
            });
            location.replace('/p/' + encodeURIComponent(token) + '?room=chat');
        } catch (err) {
            status.textContent = err.responseBody?.error || 'Could not join this room.';
            join.disabled = false;
        }
    };
    input.focus();
}

export class HandsInvite {
    constructor(ctx) {
        this.ctx = ctx;
        this.state = {};
        this.busy = false;
    }
    async open() {
        const el = (this.el = document.createElement('dialog'));
        el.className = 'hands-invite';
        el.setAttribute('aria-label', 'Invite participants');
        (document.fullscreenElement || document.body).append(el);
        el.innerHTML = `<header><img src="/assets/logo.png" alt="" width="30" height="30"><h2>Invite participants</h2>
        <button type="button" aria-label="Close">×</button></header>
        <section class="invite-content"><label class="invite-source">Share <select></select></label>
        <button class="btn btn-open invite-start">Create invitation</button>
        <section class="invite-live" hidden><p class="invite-help"></p>
        <label>Guest link<input class="invite-url" readonly></label>
        <div class="invite-actions"><button class="btn btn-secondary copy">Copy link</button>
        <button class="btn btn-secondary chat">Room chat</button></div>
        <label class="control-gate"><input type="checkbox"> Guests can control</label>
        <h3>Participants</h3><ul></ul><button class="btn btn-secondary end">End sharing</button></section>
        <p role="status"></p></section>`;
        const find = (sel) => el.querySelector(sel);
        this.status = find('[role=status]');
        this.select = find('select');
        this.members = find('ul');
        find('header button').onclick = () => this.close();
        el.addEventListener('close', () => this.close(), { once: true });
        el.showModal();
        if (!this.ctx.streaming) {
            let apps = [];
            try {
                apps = (await BackendClient.getAppList(this.ctx.hostUuid)).apps || [];
            } catch {
                /* Screen capture may still need permission. */
            }
            for (const app of [...apps, { id: -2, name: 'Chat only' }]) {
                this.select.add(new Option(app.name, String(app.id)));
            }
        }
        find('.invite-start').onclick = () =>
            this.action('open', { app: this.ctx.streaming ? -1 : Number(this.select.value) });
        find('.copy').onclick = async () => {
            const input = find('.invite-url');
            try {
                await navigator.clipboard.writeText(input.value);
                this.status.textContent = 'Invitation copied';
            } catch {
                input.focus();
                input.select();
                this.status.textContent = 'Copy the selected link.';
            }
        };
        find('.chat').onclick = () => openRoom({ host: this.ctx.hostUuid, app: this.state.app });
        find('.end').onclick = () => this.action('end');
        find('.control-gate input').onchange = (e) =>
            this.action('allow', { value: e.target.checked });
        const refresh = async () => {
            if (!this.el) return;
            if (!this.busy) await this.action('status');
            if (this.el) this.timer = setTimeout(refresh, 2000);
        };
        await refresh();
        return true;
    }
    async action(action, data = {}) {
        // Polling never disables the controls. A user action supersedes an
        // older poll; an in-flight owner write is never repeated.
        if (this.busy) return;
        const seq = (this.seq = (this.seq || 0) + 1);
        if (action !== 'status') this.busy = true;
        try {
            const state = await BackendClient.post('/api/share/room', {
                host: this.ctx.hostUuid,
                action,
                invitation: this.state.invitation,
                ...data,
            });
            if (!this.el || this.seq !== seq) return;
            this.state = state;
            this.paint();
            if (action !== 'status')
                this.status.textContent = action === 'end' ? 'Sharing ended.' : '';
        } catch (err) {
            if (this.el && this.seq === seq) {
                this.paint();
                this.status.textContent = err.responseBody?.error || 'Could not reach the host.';
            }
        } finally {
            if (action !== 'status') this.busy = false;
        }
    }
    paint() {
        const s = this.state,
            find = (sel) => this.el.querySelector(sel);
        find('.invite-source').hidden = !!s.active || !!this.ctx.streaming;
        find('.invite-start').hidden = !!s.active;
        find('.invite-live').hidden = !s.active;
        if (!s.active) {
            find('.invite-url').value = '';
            this.members.replaceChildren();
            return;
        }
        const address = new URL('/p', s.origin || location.origin);
        address.hash = 'invite=' + s.invitation;
        find('.invite-url').value = address.href;
        const local = ['localhost', '127.0.0.1', '[::1]'].includes(address.hostname);
        find('.invite-help').textContent =
            'Guests join with their name. Expires at ' +
            new Date(s.expires * 1000).toLocaleTimeString([], {
                hour: '2-digit',
                minute: '2-digit',
            }) +
            '.' +
            (local
                ? ' This address opens on this computer. Set a guest HTTPS origin to invite another device.'
                : '');
        find('.control-gate').hidden = s.app === -2;
        find('.control-gate input').checked = s.allow_control;
        const signature = JSON.stringify([s.members, s.allow_control, s.app]);
        if (this.signature === signature && this.members.childElementCount) return;
        this.signature = signature;
        const expanded = new Set(
            Array.from(this.members.querySelectorAll('details[open]'), (el) => el.dataset.opening),
        );
        const rows = s.members.map((m) => {
            const li = element('li'),
                avatar = element('img');
            avatar.src = '/assets/logo.png';
            avatar.width = avatar.height = 28;
            avatar.alt = '';
            const name = element(
                'span',
                m.name + (m.control ? ' · Has control' : m.requested ? ' · Requests control' : ''),
            );
            const opts = { slot: m.slot, opening: m.opening };
            const menu = element('details'),
                summary = element('summary', 'Manage');
            menu.dataset.opening = m.opening;
            menu.open = expanded.has(m.opening);
            summary.setAttribute('aria-label', 'Manage ' + m.name);
            menu.append(summary);
            if (s.app !== -2) {
                const control = button(m.control ? 'Revoke control' : 'Grant control', () =>
                    this.action('control', { ...opts, value: !m.control }),
                );

                menu.append(control);
            }
            menu.append(button('Remove', () => this.action('remove', opts)));
            li.append(avatar, name, menu);
            return li;
        });
        this.members.replaceChildren(...(rows.length ? rows : [element('li', 'No guests yet.')]));
    }
    close() {
        if (!this.el) return;
        clearTimeout(this.timer);
        const el = this.el;
        this.el = null;
        el.remove();
        this.onClose?.();
    }
}

import fs from 'node:fs';
import path from 'node:path';
export class MembershipStore {
  constructor(filename) {
    this.filename = filename;
    this.entries = fs.existsSync(filename) ? JSON.parse(fs.readFileSync(filename, 'utf8')) : [];
    if (!Array.isArray(this.entries) || this.entries.some(entry => !/^(?:camera-)?[A-F0-9]{12}$/.test(entry.room) || !/^\d{1,64}$/.test(entry.client))) throw Error('Invalid saved signaling membership');
  }
  save() {
    fs.mkdirSync(path.dirname(this.filename), { recursive: true });
    const temporary = this.filename + '.tmp';
    fs.writeFileSync(temporary, JSON.stringify(this.entries), { mode: 0o600 });
    fs.renameSync(temporary, this.filename);
  }
  add(room, client) {
    if (!/^(?:camera-)?[A-F0-9]{12}$/.test(room) || !/^\d{1,64}$/.test(client)) throw Error('Invalid signaling membership');
    if (!this.entries.some(entry => entry.room === room && entry.client === client)) { this.entries.push({ room, client }); this.save(); }
  }
  remove(room, client) { this.entries = this.entries.filter(entry => entry.room !== room || entry.client !== client); this.save(); }
  async release(entry, request) {
    // Keep the journal entry unless BOTH room and collider cleanup succeed.
    await request('POST', `https://webrtc.espressif.com/leave/${entry.room}/${entry.client}`);
    await request('DELETE', `https://webrtc.espressif.com:8089/${entry.room}/${entry.client}`);
    this.remove(entry.room, entry.client);
  }
}

/* SPDX-License-Identifier: Apache-2.0 */
/* Vertex Script function for the existing MTeam/client/RSS deployment.
 * Keep cron: 17 17,47 * * * *. Requires the host mt-api shim and
 * /vertex/data/mt-selection-policy.js. Account credentials stay in Vertex.
 */
async () => {
  const fs = require('fs');
  const moment = require('moment');
  const { requestMT, downloadTorrent } = require('../libs/mt-api');
  const util = require('../libs/util');
  const category = 'mt-upload';
  const selectionPolicy = require('/vertex/data/mt-selection-policy');
  const client = global.runningClient.a4190001;
  const task = global.runningRss.a4190002;
  const site = global.runningSite.MTeam;
  if (global.mtSelectionBusy) return;
  global.mtSelectionBusy = true;
  try {
    if (!client?.status || !client.cookie || !task || !site) throw new Error('MT selector: downloader/site not ready');
    task._downloadTorrent = downloadTorrent;
    await client.getMaindata();
    const torrents = client.maindata.torrents;
    const own = torrents.filter(t => t.category === category);
    const active = own.filter(t => t.progress < 1 && !/paused|stopped/i.test(t.state));
    const today = moment().startOf('day').unix();
    const dailyBudget = 512 * 1024 ** 3 - own.filter(t => t.addedTime >= today).reduce((sum, t) => sum + t.size, 0);
    const reserved = own.filter(t => t.progress < 1).reduce((sum, t) => sum + Math.max(0, t.size - t.completed), 0);
    let budget = Math.min(dailyBudget, client.maindata.freeSpaceOnDisk - 1024 ** 4 - reserved);
    const hourlySlots = Math.max(0, task.addCountPerHour - task.addCount);
    const slots = Math.max(0, Math.min(6 - active.length, hourlySlots, 1));
    if (!slots || budget < 20 * 1024 ** 3) {
      fs.writeFileSync('/vertex/data/mt-selection-plan.json', JSON.stringify({ time: new Date(), reason: 'concurrency/daily-size/free-space limit', slots, budget }, null, 2)); return;
    }
    const schedulePath = '/vertex/data/mt-selection-schedule.json';
    let lastScan = 0;
    try { lastScan = JSON.parse(fs.readFileSync(schedulePath)).lastScan || 0; } catch (error) { if (error.code !== 'ENOENT') throw error; }
    // Cron callbacks may differ by a few milliseconds between half-hour ticks.
    if (Date.now() - lastScan < 30 * 60000 - 1000) return;
    fs.writeFileSync(schedulePath, JSON.stringify({ lastScan: Date.now() }));
    const raw = new Map();
    for (const sortField of ['CREATED_DATE', 'LEECHERS']) {
      const data = await requestMT(site.cookie, '/api/torrent/search', { mode: 'normal', categories: [], discount: 'FREE', pageNumber: 1, pageSize: 200, sortField, sortDirection: 'DESC' }, 'json');
      for (const torrent of data.data || []) raw.set(String(torrent.id), torrent);
    }
    const parsed = [...raw.values()].map(t => ({
      id: String(t.id), hash: 'fakehash-mteam-' + t.id, name: t.name, size: Number(t.size),
      pubTime: moment(t.status.createdDate || t.createdDate).unix(), link: 'https://kp.m-team.cc/detail/' + t.id,
      seeders: Number(t.status.seeders), leechers: Number(t.status.leechers),
      free: ['FREE', '_2X_FREE'].includes(t.status.discount),
      discount: t.status.discount,
      completedOnTracker: Number(t.status.timesCompleted || 0),
      freeUntil: t.status.discountEndTime ? moment(t.status.discountEndTime).unix() : 0,
      category: String(t.category), description: ''
    }));
    fs.writeFileSync('/vertex/data/mt-latest-candidates.json', JSON.stringify({ time: new Date(), torrents: parsed }), { mode: 0o600 });
    const candidates = [];
    for (const torrent of parsed) {
      if (torrents.some(t => Number(t.size) === torrent.size)) continue;
      const fits = task.acceptRules.filter(rule => task._fitRule(rule, torrent));
      if (!fits.length) continue;
      const record = await util.getRecord('SELECT id FROM torrents WHERE hash = ? AND rss_id = ?', [torrent.hash, task.id]);
      if (record) continue;
      const demandScore = selectionPolicy.score(torrent);
      candidates.push({ ...torrent, rule: fits[0].alias, priority: fits[0].priority, demandScore });
    }
    candidates.sort((a, b) => b.demandScore - a.demandScore || b.priority - a.priority);
    const selected = [];
    for (const torrent of candidates) {
      if (selected.length >= slots) break;
      if (torrent.size <= budget) { selected.push(torrent); budget -= torrent.size; }
    }
    const plan = { time: new Date(), sortFields: ['CREATED_DATE', 'LEECHERS'], scanned: parsed.length, eligible: candidates.length, selected: selected.map(t => ({ id: t.id, name: t.name, size: t.size, seeders: t.seeders, leechers: t.leechers, rule: t.rule, demandScore: t.demandScore })), dryrun: fs.existsSync('/vertex/data/mt-selection-dryrun') };
    fs.writeFileSync('/vertex/data/mt-selection-plan.json', JSON.stringify(plan, null, 2));
    if (plan.dryrun) return;
    for (const torrent of selected) {
      torrent.url = await site.getDownloadLink(torrent.link);
      if (typeof torrent.url !== 'string' || !torrent.url.startsWith('https://')) throw new Error('MT returned no download token');
      task.lastRssTime = moment().unix();
      await task.rss([torrent]);
      await client.getMaindata();
      const added = client.maindata.torrents.find(t => t.category === category && Number(t.size) === torrent.size);
      if (added?.state === 'queuedDL') await util.requestPromise({ url: client.clientUrl + '/api/v2/torrents/setForceStart', method: 'POST', headers: { cookie: client.cookie }, form: { hashes: added.hash, value: 'true' } });
    }
    await client.getMaindata();
    const queued = client.maindata.torrents.filter(t => t.category === category && !/paused|stopped/i.test(t.state));
    if (queued.length) await util.requestPromise({ url: client.clientUrl + '/api/v2/torrents/setForceStart', method: 'POST', headers: { cookie: client.cookie }, form: { hashes: queued.map(t => t.hash).join('|'), value: 'true' } });
  } finally { global.mtSelectionBusy = false; }
}

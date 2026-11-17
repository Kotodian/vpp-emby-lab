/* SPDX-License-Identifier: Apache-2.0 */
const assert = require('assert');
const policy = require('./vertex-mt-selection-policy');
const GiB = 1024 ** 3;
const now = 1800000000;
const base = {free: true, category: '405', size: 25 * GiB,
  pubTime: now - 4 * 3600, freeUntil: now + 86400, seeders: 200, leechers: 140};
assert(policy.fits(base, 'main', now));
// A three-hour free window can safely admit this 52 GiB candidate; the old
// blanket six-hour rule rejected it regardless of available download time.
assert(policy.fits({...base, size: 52 * GiB, freeUntil: now + 3 * 3600,
  seeders: 227, leechers: 121}, 'main', now));
for (const candidate of [
  {...base, size: 9 * GiB, seeders: 1, leechers: 500},
  {...base, size: 50 * GiB, seeders: 1, pubTime: now - 2 * 3600},
  {...base, free: false}, {...base, free: 'false'},
  {...base, freeUntil: now + 3600},
  {...base, size: 250 * GiB, freeUntil: now + 3 * 3600},
  {...base, seeders: 0}, {...base, leechers: 0}, {...base, category: '410'},
  {...base, pubTime: now - 72 * 3600}, {...base, size: 301 * GiB},
]) {
  for (const kind of ['main', 'scarce', 'jumbo'])
    assert.strictEqual(policy.fits(candidate, kind, now), false);
}
assert(policy.fits({...base, size: 50 * GiB, pubTime: now - 1800,
  seeders: 1, leechers: 50}, 'scarce', now));
assert(policy.fits({...base, size: 250 * GiB, seeders: 100, leechers: 100}, 'jumbo', now));
const doro = {...base, size: 25.43 * GiB, pubTime: now - 6.2 * 3600, seeders: 254, leechers: 188};
const spriggan = {...base, size: 22.5 * GiB, pubTime: now - 2.5 * 3600, seeders: 207, leechers: 136};
assert(policy.score(doro, now) > policy.score(spriggan, now));
assert.strictEqual(policy.score({...doro, seeders: '254', leechers: '188'}, now), policy.score(doro, now));
const before = policy.score({...base, pubTime: now - 5.999 * 3600}, now);
const after = policy.score({...base, pubTime: now - 6.001 * 3600}, now);
assert(before / after < 1.001); // No abrupt threefold drop at six hours.
console.log('PASS: MT qualification, expiry budgets and continuous size-aware ranking');

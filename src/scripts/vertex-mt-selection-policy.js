/* SPDX-License-Identifier: Apache-2.0 */
/* Qualification does not guarantee upload: measure each admitted trial.
 * Native RSS rules can call fits(t, "main" | "scarce" | "jumbo").
 * Retain the one-hour expiry pause guard with this download-time budget.
 */
const GiB = 1024 ** 3;
const MiB = 1024 ** 2;
const categories = new Set([
  '401', '419', '420', '421', '439', '403', '402', '435', '438',
  '404', '405', '453', '407', '434', '406', '423', '448'
]);

function minimumFreeSeconds(size) {
  return Math.max(2 * 3600, size / (20 * MiB) * 1.5 + 3600);
}

function fits(t, kind, now = Date.now() / 1000) {
  const seeders = Number(t.seeders);
  const leechers = Number(t.leechers);
  const size = Number(t.size);
  const age = now - Number(t.pubTime);
  const end = Number(t.freeUntil || 0);
  if (t.free !== true || !categories.has(String(t.category)) ||
      ![seeders, leechers, size, age, end].every(Number.isFinite)) return false;
  if (size < 20 * GiB || size > 300 * GiB || age < 0 || age > 48 * 3600 ||
      seeders < 1 || leechers < 1) return false;
  // A lone source only qualifies for a recently published large package.
  if (seeders === 1 && (size < 40 * GiB || age > 3600)) return false;
  if (end && end - now < minimumFreeSeconds(size)) return false;
  if (kind === 'scarce')
    return size <= 180 * GiB && seeders <= 30 && leechers >= 15 && leechers / seeders >= 0.5;
  if (kind === 'jumbo')
    return size >= 80 * GiB && seeders <= 1000 && leechers >= 100 && leechers / seeders >= 0.3;
  return size <= 180 * GiB && seeders <= 500 && leechers >= 20 && leechers / seeders >= 0.3;
}

function score(t, now = Date.now() / 1000) {
  const ageHours = Math.max(0, (now - Number(t.pubTime)) / 3600);
  const seeders = Number(t.seeders);
  const leechers = Number(t.leechers);
  return leechers ** 2 / Math.max(1, seeders + leechers) *
    Math.sqrt(Number(t.size) / GiB) / Math.sqrt(1 + ageHours / 6);
}

module.exports = { fits, score, minimumFreeSeconds };

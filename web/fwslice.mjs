// SPDX-License-Identifier: Unlicense
// Split a merged full-flash image into the byte ranges ESP Web Tools should
// write, leaving the nvs partition untouched so a re-flash keeps the fleet
// config. Offsets are the partitions.csv layout: nvs @0x9000, size 0x7000.
// A merged image (esptool merge_bin) pads the nvs gap with 0xFF, so flashing it
// whole at offset 0 erases the shade store — split around nvs to preserve it.

export const NVS = { start: 0x9000, end: 0x10000 };

/**
 * Byte ranges {from,to,offset} of a merged image to flash, skipping the nvs
 * region [NVS.start, NVS.end). Each range is written at `offset`; the gap is
 * never touched, so esptool leaves those flash sectors (the fleet) as-is.
 */
export function flashRanges(len, nvs = NVS) {
  const ranges = [{ from: 0, to: Math.min(nvs.start, len), offset: 0 }];
  if (len > nvs.end) ranges.push({ from: nvs.end, to: len, offset: nvs.end });
  return ranges;
}

if (typeof process !== "undefined" && import.meta.url === `file://${process.argv[1]}`) {
  const assert = (c, m) => { if (!c) throw new Error(m); };
  const rs = flashRanges(0x200000);
  assert(rs.length === 2, "two parts around nvs");
  assert(rs[0].offset === 0 && rs[0].to === NVS.start, "part 0 stops at nvs start");
  assert(rs[1].offset === NVS.end && rs[1].from === NVS.end, "part 1 resumes after nvs");
  const covers = (o) => rs.some((x) => o >= x.offset && o < x.offset + (x.to - x.from));
  for (let o = NVS.start; o < NVS.end; o += 0x1000) assert(!covers(o), "nvs untouched @0x" + o.toString(16));
  assert(covers(0) && covers(0x8000) && covers(NVS.end) && covers(0x1FFFFF), "everything else covered");
  console.log("fwslice ok");
}

// Drives scene.html in Chrome one frame at a time and saves each as a JPEG.
// Frames are rendered in order because touch marks carry state between them.
//
//   node render.mjs http://127.0.0.1:8765/scene.html OUTDIR [all | 20,160,300]
import { chromium } from "playwright-core";
import fs from "node:fs";

const [, , url, outDir, which = "all"] = process.argv;
fs.mkdirSync(outDir, { recursive: true });
const browser = await chromium.launch({
  executablePath: process.env.CHROME ||
    "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
  // Without the GPU flags headless Chrome falls back to software GL.
  args: ["--use-angle=metal", "--enable-gpu", "--ignore-gpu-blocklist"],
});
const page = await browser.newPage({ viewport: { width: 1080, height: 1350 }, deviceScaleFactor: 1 });
page.on("pageerror", (e) => console.log("page error:", e.message));
await page.goto(url);
await page.waitForFunction(() => window.ready === true, null, { timeout: 60000 });
const n = await page.evaluate(() => window.frameCount);
const frames = which === "all" ? [...Array(n).keys()] : which.split(",").map(Number);
for (let i = 0; i <= Math.max(...frames); i++) {
  const data = await page.evaluate((i) => window.renderFrame(i), i);
  if (frames.includes(i)) {
    fs.writeFileSync(`${outDir}/r${String(i).padStart(5, "0")}.jpg`, Buffer.from(data.split(",")[1], "base64"));
  }
}
console.log(`rendered ${frames.length} of ${n} frames`);
await browser.close();

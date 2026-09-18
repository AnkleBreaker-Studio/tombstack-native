import assert from "node:assert/strict";
import { mkdtempSync, readFileSync, writeFileSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { execFileSync } from "node:child_process";

const token = process.env.TOMBSTACK_WIRE_TOKEN;
if (!token || !process.argv[2]) throw new Error("Set TOMBSTACK_WIRE_TOKEN for the test tenant and pass the compiled wire probe path");
const base = process.env.TOMBSTACK_BASE_URL ?? "https://tombstack.com";
const root = mkdtempSync(join(tmpdir(), "tombstack-native-live-"));
try {
  const buildVersion = `native-wire-audit-${Date.now()}`;
  const bodyPath = join(root, "body.json");
  const responsePath = join(root, "response.json");
  writeFileSync(bodyPath, JSON.stringify({
    occurredAtIso: new Date().toISOString(), buildVersion, os: "windows", arch: "x64",
    signature: buildVersion, stackHint: "Native SDK signed POST and log audit", kind: "exception", log: true,
  }));
  execFileSync(resolve(process.argv[2]), [`${base}/api/v1/ingest/crashes`, root], {
    env: { ...process.env, TOMBSTACK_WIRE_BODY: bodyPath, TOMBSTACK_WIRE_RESPONSE: responsePath },
    stdio: "pipe", windowsHide: true, timeout: 30000,
  });
  const ack = JSON.parse(readFileSync(responsePath, "utf8"));
  assert.ok(ack.success && ack.data.crashId && ack.data.logUpload, "signed ingest did not return a log descriptor");
  const read = async path => {
    const response = await fetch(`${base}/api/v1/read/${path}`, { headers: { Authorization: `Bearer ${token}` } });
    assert.equal(response.status, 200);
    return response.json();
  };
  let passed = false;
  for (let attempt = 0; attempt < 10 && !passed; attempt++) {
    const list = await read("crashes?days=1");
    const crash = list.data.crashes.find(row => row.crashId === ack.data.crashId);
    if (crash) {
      const detail = await read(`signatures/${encodeURIComponent(crash.signature)}`);
      const occurrence = detail.data.recent.find(row => row.logS3Key === ack.data.logUpload.key);
      if (occurrence?.logUrl) {
        const response = await fetch(occurrence.logUrl);
        if (response.ok) {
          assert.deepEqual(Buffer.from(await response.arrayBuffer()), readFileSync(join(root, "session.log")));
          passed = true;
          break;
        }
      }
    }
    await new Promise(done => setTimeout(done, 500));
  }
  assert.ok(passed, "native log did not round-trip through storage and authenticated read-back");
  console.log("PASS native Worker -> signed ingest -> multipart storage POST -> identical log read-back");
} finally {
  assert.equal(dirname(resolve(root)), resolve(tmpdir()));
  rmSync(root, { recursive: true, force: true });
}

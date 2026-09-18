import assert from "node:assert/strict";
import { createHmac } from "node:crypto";
import { mkdtemp, rm } from "node:fs/promises";
import { createServer } from "node:http";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { spawn } from "node:child_process";

const fields = { key: "logs/audit.log", Policy: "signed-policy+/=", "X-Amz-Credential": "audit/credential" };
const errors = [];
const counts = new Map();
const logs = [];
const server = createServer(async (req, res) => {
  try {
    const parts = [];
    for await (const part of req) parts.push(part);
    const body = Buffer.concat(parts);
    counts.set(req.url, (counts.get(req.url) ?? 0) + 1);
    if (["/post", "/legacy", "/invalid"].includes(req.url)) {
      assert.equal(req.headers.authorization, "Bearer audit-public-token");
      assert.equal(req.headers["x-tombstone-signature"], undefined);
      const signature = /^t=(\d+),v1=([a-f0-9]{64})$/.exec(req.headers["x-tombstack-signature"] ?? "");
      assert.ok(signature, "missing current signature header");
      const expected = createHmac("sha256", "audit-public-token").update(`${signature[1]}.`).update(body).digest("hex");
      assert.equal(signature[2], expected);
      const legacy = req.url === "/legacy";
      res.setHeader("Content-Type", "application/json");
      res.end(JSON.stringify({ success: true, data: { logUpload: {
        url: `${base}/${legacy ? "put-storage" : "post-storage"}`,
        method: legacy ? "PUT" : "POST", fields: req.url === "/invalid" ? {} : fields,
      } } }));
    } else {
      assert.equal(req.headers.authorization, undefined, "ingest token leaked to storage");
      assert.equal(req.headers["x-tombstack-signature"], undefined);
      if (req.url === "/post-storage") {
        assert.equal(req.method, "POST");
        const form = await new Response(body, { headers: { "Content-Type": req.headers["content-type"] } }).formData();
        assert.deepEqual([...form.keys()], [...Object.keys(fields), "file"]);
        for (const [key, value] of Object.entries(fields)) assert.equal(form.get(key), value);
        const file = form.get("file");
        assert.equal(file.name, "session.log");
        assert.equal(file.type, "text/plain");
        logs.push(Buffer.from(await file.arrayBuffer()));
        if (counts.get(req.url) === 1) {
          res.writeHead(503, { "Retry-After": "0" });
          res.end();
          return;
        }
      } else {
        assert.equal(req.url, "/put-storage");
        assert.equal(req.method, "PUT");
        logs.push(body);
      }
      res.writeHead(204);
      res.end();
    }
  } catch (error) {
    errors.push(error);
    res.writeHead(400);
    res.end();
  }
});
await new Promise(resolve => server.listen(0, "127.0.0.1", resolve));
const base = `http://127.0.0.1:${server.address().port}`;
const scratch = await mkdtemp(join(tmpdir(), "tombstack-wire-"));
try {
  for (const route of ["post", "legacy", "invalid"]) {
    await new Promise((resolve, reject) => {
      const env = { ...process.env };
      delete env.TOMBSTACK_WIRE_TOKEN;
      delete env.TOMBSTACK_WIRE_BODY;
      delete env.TOMBSTACK_WIRE_RESPONSE;
      const child = spawn(process.argv[2], [`${base}/${route}`, join(scratch, route)], { env, stdio: "inherit" });
      child.on("error", reject);
      child.on("exit", code => code === 0 ? resolve() : reject(new Error(`probe exited ${code}`)));
    });
  }
  assert.deepEqual(errors, []);
  assert.equal(counts.get("/post-storage"), 2, "POST retry must preserve its policy fields");
  assert.equal(counts.get("/put-storage"), 1, "legacy PUT remains compatible");
  assert.equal(logs.length, 3);
  assert.ok(logs[0].includes(Buffer.from("native wire audit\0UTF-8: \u6f22")));
  for (const log of logs) assert.deepEqual(log, logs[0]);
  console.log("PASS: signed ingest, multipart log POST, retry, binary bytes, no credential leak, legacy PUT, invalid descriptor");
} finally {
  server.closeAllConnections();
  await new Promise(resolve => server.close(resolve));
  assert.equal(dirname(resolve(scratch)), resolve(tmpdir()));
  await rm(scratch, { recursive: true, force: true });
}

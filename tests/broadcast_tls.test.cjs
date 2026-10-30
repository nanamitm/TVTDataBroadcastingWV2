const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const http = require('node:http');
const tls = require('node:tls');
const { spawn, spawnSync } = require('node:child_process');
const dir = path.resolve(__dirname, '../work/broadcast-tls-' + process.pid);
fs.mkdirSync(dir, { recursive: true });
const openssl = path.join(process.env.ProgramFiles ?? 'C:/Program Files', 'Git/usr/bin/openssl.exe');
const executable = path.resolve(process.argv[2] ?? 'work/safety-tests/broadcast-certificates.exe');
function ssl(...args) {
  const result = spawnSync(openssl, args, { cwd: dir, encoding: 'utf8' });
  if (result.status !== 0) throw new Error(result.error?.message ?? result.stderr);
}
const listen = server => new Promise((resolve, reject) => { server.once('error', reject); server.listen(0, '127.0.0.1', resolve); });
const close = server => new Promise(resolve => server.close(resolve));
function native(url, expected) {
  return new Promise((resolve, reject) => {
    const process = spawn(executable, ['--tls', path.join(dir, 'root.cer'), url, expected], { windowsHide: true });
    let text = '';
    process.stdout.on('data', chunk => text += chunk);
    process.stderr.on('data', chunk => text += chunk);
    process.on('error', reject);
    process.on('exit', code => code === 0 ? resolve(text) : reject(new Error(text + '\nNative exit=' + code)));
  });
}
async function serverFor(name, response) {
  let received = '';
  const sockets = new Set();
  const server = tls.createServer({ key: fs.readFileSync(path.join(dir, name === 'foreign' ? 'foreign.key' : 'leaf.key')),
    cert: fs.readFileSync(path.join(dir, name + '.pem')) }, socket => {
    sockets.add(socket); socket.on('error', () => {}); socket.on('close', () => sockets.delete(socket));
    socket.on('data', chunk => {
      received += chunk.toString();
      if (received.includes('\r\n\r\n')) socket.end(response ?? 'HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n');
    });
  });
  server.on('tlsClientError', () => {});
  await listen(server);
  return { url: `https://localhost:${server.address().port}/fixture`, bytes: () => received,
    close: async () => { for (const socket of sockets) socket.destroy(); await close(server); } };
}
async function main() {
  const crlServer = http.createServer((req, res) => {
    if (req.url !== '/crl') { res.writeHead(404); res.end(); return; }
    res.writeHead(200, { 'Content-Type': 'application/pkix-crl' });
    res.end(fs.readFileSync(path.join(dir, 'root.crl')));
  });
  await listen(crlServer);
  try {
    fs.writeFileSync(path.join(dir, 'index'), '');
    fs.writeFileSync(path.join(dir, 'serial'), '1000\n'); fs.writeFileSync(path.join(dir, 'crlnumber'), '1000\n');
    const extensions = (dns, usage) => `basicConstraints=critical,CA:FALSE\nkeyUsage=critical,digitalSignature,keyEncipherment\nextendedKeyUsage=${usage}\nsubjectAltName=DNS:${dns}\ncrlDistributionPoints=URI:http://127.0.0.1:${crlServer.address().port}/crl\n`;
    fs.writeFileSync(path.join(dir, 'ca.cnf'), `[ca]\ndefault_ca=ca_default\n[ca_default]\ndir="${dir.replace(/\\/g, '/')}"\ndatabase=$dir/index\nserial=$dir/serial\ncrlnumber=$dir/crlnumber\nnew_certs_dir=$dir\ncertificate=$dir/root.pem\nprivate_key=$dir/root.key\ndefault_md=sha256\ndefault_days=30\ndefault_crl_days=1\npolicy=policy\nunique_subject=no\n[policy]\ncommonName=supplied\n[server]\n${extensions('localhost', 'serverAuth')}\n[wrong_name]\n${extensions('other.invalid', 'serverAuth')}\n[wrong_usage]\n${extensions('localhost', 'clientAuth')}\n[no_crl]\n${extensions('localhost', 'serverAuth').replace(/^crlDistributionPoints=.*\n/m, '')}`);
    ssl('req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-keyout', 'root.key', '-out', 'root.pem', '-days', '3650', '-subj', '/CN=Broadcast Fixture Root', '-addext', 'basicConstraints=critical,CA:TRUE', '-addext', 'keyUsage=critical,keyCertSign,cRLSign');
    ssl('x509', '-in', 'root.pem', '-outform', 'DER', '-out', 'root.cer');
    ssl('req', '-new', '-newkey', 'rsa:2048', '-nodes', '-keyout', 'leaf.key', '-out', 'leaf.csr', '-subj', '/CN=localhost');
    for (const [name, extension] of [['valid', 'server'], ['uncheckable', 'no_crl'], ['revoked', 'server'], ['expired', 'server'], ['wrong-name', 'wrong_name'], ['wrong-usage', 'wrong_usage']]) {
      ssl('ca', '-batch', '-config', 'ca.cnf', '-extensions', extension, '-in', 'leaf.csr', '-out', name + '.pem',
        ...(name === 'expired' ? ['-startdate', '20000101000000Z', '-enddate', '20000102000000Z'] : []));
    }
    ssl('ca', '-batch', '-config', 'ca.cnf', '-revoke', 'revoked.pem', '-crl_reason', 'keyCompromise');
    ssl('ca', '-gencrl', '-config', 'ca.cnf', '-out', 'root-crl.pem');
    ssl('crl', '-in', 'root-crl.pem', '-outform', 'DER', '-out', 'root.crl');
    ssl('req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-keyout', 'foreign.key', '-out', 'foreign.pem', '-days', '30', '-subj', '/CN=localhost', '-addext', 'subjectAltName=DNS:localhost');
    // Before a valid CRL is cached, lack of revocation information must fail.
    for (const name of ['uncheckable', 'valid', 'revoked', 'expired', 'wrong-name', 'wrong-usage', 'foreign']) {
      const fixture = await serverFor(name);
      try {
        const result = await native(fixture.url, name === 'valid' ? 'success' : 'failure');
        if (name === 'valid') assert.ok(fixture.bytes().includes('fixture-secret'), 'approved TLS server receives POST');
        else assert.equal(fixture.bytes().length, 0, `${name}: invalid certificate receives no HTTP bytes`);
        console.log(name + ': ' + result.trim());
      } finally { await fixture.close(); }
    }
    let httpRequests = 0;
    const plain = http.createServer((request, response) => { httpRequests++; request.resume(); response.writeHead(200); response.end(); });
    await listen(plain);
    try {
      const url = `http://localhost:${plain.address().port}/fixture`;
      await native(url, 'success');
      assert.equal(httpRequests, 1, 'private roots do not break ordinary HTTP BML requests');
      const downgrade = await serverFor('valid', `HTTP/1.1 307 Temporary Redirect\r\nLocation: ${url}\r\nContent-Length: 0\r\nConnection: close\r\n\r\n`);
      try {
        await native(downgrade.url, 'failure');
        assert.equal(httpRequests, 1, 'TLS cannot redirect POST to plaintext');
      } finally { await downgrade.close(); }
    } finally { await close(plain); }
    const destination = await serverFor('foreign');
    const redirect = await serverFor('valid', `HTTP/1.1 307 Temporary Redirect\r\nLocation: ${destination.url}\r\nContent-Length: 0\r\nConnection: close\r\n\r\n`);
    try {
      await native(redirect.url, 'failure');
      assert.ok(redirect.bytes().includes('fixture-secret'));
      assert.equal(destination.bytes().length, 0, 'redirect destination is verified before resending POST');
    } finally { await redirect.close(); await destination.close(); }
    console.log('TLS validation and redirect tests passed; invalid peers received zero HTTP bytes');
  } finally { await close(crlServer); }
}
main().catch(error => { console.error(error); process.exitCode = 1; });

// 独立 JS 参考实现：直接照抄门户下发的函数体，用来生成 Python 实现的测试向量。
// 运行：node js_reference.js
'use strict';
const crypto = require('crypto');

// ---- 以下三个函数原样来自 /static/js/jquery.srun.portal.js?v=2.00.20220802 ----
function xEncode(str, key) {
    if (str == "") {
        return "";
    }
    var v = s(str, true),
        k = s(key, false);
    if (k.length < 4) {
        k.length = 4;
    }
    var n = v.length - 1,
        z = v[n],
        y = v[0],
        c = 0x86014019 | 0x183639A0,
        m, e, p,
        q = Math.floor(6 + 52 / (n + 1)),
        d = 0;
    while (0 < q--) {
        d = d + c & (0x8CE0D9BF | 0x731F2640);
        e = d >>> 2 & 3;
        for (p = 0; p < n; p++) {
            y = v[p + 1];
            m = z >>> 5 ^ y << 2;
            m += (y >>> 3 ^ z << 4) ^ (d ^ y);
            m += k[(p & 3) ^ e] ^ z;
            z = v[p] = v[p] + m & (0xEFB8D130 | 0x10472ECF);
        }
        y = v[0];
        m = z >>> 5 ^ y << 2;
        m += (y >>> 3 ^ z << 4) ^ (d ^ y);
        m += k[(p & 3) ^ e] ^ z;
        z = v[n] = v[n] + m & (0xBB390742 | 0x44C6F8BD);
    }
    return l(v, false);
}
function s(a, b) {
    var c = a.length, v = [];
    for (var i = 0; i < c; i += 4) {
        v[i >> 2] = a.charCodeAt(i) | a.charCodeAt(i + 1) << 8 | a.charCodeAt(i + 2) << 16 | a.charCodeAt(i + 3) << 24;
    }
    if (b) {
        v[v.length] = c;
    }
    return v;
}
function l(a, b) {
    var d = a.length, c = (d - 1) << 2;
    if (b) {
        var m = a[d - 1];
        if ((m < c - 3) || (m > c))
            return null;
        c = m;
    }
    for (var i = 0; i < d; i++) {
        a[i] = String.fromCharCode(a[i] & 0xff, a[i] >>> 8 & 0xff, a[i] >>> 16 & 0xff, a[i] >>> 24 & 0xff);
    }
    if (b) {
        return a.join('').substring(0, c);
    } else {
        return a.join('');
    }
}
// -----------------------------------------------------------------------------

const CUSTOM_ALPHA = 'LVoJPiCN2R8G90yg+hmFHuacZ1OWMnrsSTXkYpUq/3dlbfKwv6xztjI7DeBE45QA';
const STD_ALPHA = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
const MAP = {};
for (let i = 0; i < STD_ALPHA.length; i++) MAP[STD_ALPHA[i]] = CUSTOM_ALPHA[i];

function srunBase64(text) {
    const std = Buffer.from(text, 'latin1').toString('base64');
    return std.split('').map(ch => MAP[ch] || ch).join('');
}

function hmacMd5(password, token) {
    return crypto.createHmac('md5', Buffer.from(token, 'utf8')).update(Buffer.from(password, 'utf8')).digest('hex');
}

function makeInfo(username, password, ip, acId, token) {
    const payload = { username, password, ip, acid: String(acId), enc_ver: 'srun_bx1' };
    return '{SRBX1}' + srunBase64(xEncode(JSON.stringify(payload), token));
}

function buildLoginParams(token, username, password, acId, ip) {
    const info = makeInfo(username, password, ip, acId, token);
    const hmd5 = hmacMd5(password, token);
    let chkstr = token + username;
    chkstr += token + hmd5;
    chkstr += token + String(acId);
    chkstr += token + ip;
    chkstr += token + 200;
    chkstr += token + 1;
    chkstr += token + info;
    return {
        action: 'login',
        username: username,
        password: '{MD5}' + hmd5,
        ac_id: String(acId),
        ip: ip,
        chksum: crypto.createHash('sha1').update(chkstr, 'utf8').digest('hex'),
        info: info,
        n: '200',
        type: '1',
        os: 'Windows 10',
        name: 'Windows',
        double_stack: '0',
        ignore: '2',
    };
}

const tokens = [
    '2494f3d9bdb1af7dfea63939126a202694282f6ebbea0c8cf8a7ab2cd7ff8fca',
    '0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef',
];
const cases = [
    { username: '2024000001', password: 'TestP@ss!234', ac_id: '43', ip: '10.0.0.2' },
    { username: '2024000002', password: 'simple-password', ac_id: '43', ip: '10.0.0.99' },
];
const xencodeVectors = [
    ['', 'token'],
    ['{}', 'token'],
    ['{"username":"2024000001","password":"TestP@ss!234","ip":"10.0.0.2","acid":"43","enc_ver":"srun_bx1"}', '2494f3d9bdb1af7dfea63939126a202694282f6ebbea0c8cf8a7ab2cd7ff8fca'],
    ['The quick brown fox jumps over the lazy dog', 'key-0123456789'],
    ['abc', 'k'],
];
const base64Vectors = ['', '\x00\x01\x02', 'hello world', '\xff\xfe\xfd\xfc', xencodeVectors[2][0].slice(0, 50)];

const out = {
    xencode: xencodeVectors.map(([str, key]) => ({
        text: str, key, expected: xEncode(str, key),
    })),
    base64: base64Vectors.map(text => ({
        bytes: Buffer.from(text, 'latin1').toString('hex'),
        expected: srunBase64(text),
    })),
    hmac: [
        { password: 'password', token: 'token' },
        { password: 'TestP@ss!234', token: tokens[0] },
    ].map(c => ({ ...c, expected: hmacMd5(c.password, c.token) })),
    login: [],
};
for (const token of tokens) {
    for (const c of cases) {
        out.login.push({
            token,
            username: c.username,
            password: c.password,
            ac_id: c.ac_id,
            ip: c.ip,
            params: buildLoginParams(token, c.username, c.password, c.ac_id, c.ip),
        });
    }
}
console.log(JSON.stringify(out, null, 2));

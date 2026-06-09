import express from 'express';
import cors from 'cors';
import ExcelJS from 'exceljs';
import multer from 'multer';
import crypto from 'crypto';
import { readFileSync, writeFileSync, existsSync } from 'fs';
import { fileURLToPath } from 'url';
import { dirname, join } from 'path';

const __filename = fileURLToPath(import.meta.url);
const __dirname = dirname(__filename);

const app = express();
const PORT = 3002;

app.use(cors());
app.use(express.json());
app.use(express.urlencoded({ extended: true }));

// ========== Auth ==========
const AUTH_USER = 'Altoll';
const AUTH_PASS = 'CHe0cG1u8lim';
const activeSessions = new Set();

function generateToken() {
  return crypto.randomBytes(32).toString('hex');
}

function parseCookies(req) {
  const cookies = {};
  (req.headers.cookie || '').split(';').forEach(c => {
    const [k, v] = c.trim().split('=');
    if (k) cookies[k] = v;
  });
  return cookies;
}

function requireAuth(req, res, next) {
  const cookies = parseCookies(req);
  if (cookies.token && activeSessions.has(cookies.token)) {
    return next();
  }
  res.redirect('/login');
}

// Login page
app.get('/login', (req, res) => {
  const cookies = parseCookies(req);
  if (cookies.token && activeSessions.has(cookies.token)) {
    return res.redirect('/');
  }
  res.send(`<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>登录 - 设备编号生成器</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:'Segoe UI','Microsoft YaHei',sans-serif;background:linear-gradient(135deg,#667eea 0%,#764ba2 100%);min-height:100vh;display:flex;align-items:center;justify-content:center}
.card{background:#fff;padding:50px 40px;border-radius:20px;box-shadow:0 20px 60px rgba(0,0,0,.15);max-width:420px;width:90%}
h2{text-align:center;color:#2c3e50;margin-bottom:30px;font-size:22px}
.form-group{margin-bottom:20px}
label{display:block;font-size:13px;color:#666;margin-bottom:6px;font-weight:600}
input{width:100%;padding:14px 16px;border:2px solid #e1e8ed;border-radius:12px;font-size:15px;outline:none;transition:border-color .3s}
input:focus{border-color:#667eea}
.btn{width:100%;padding:14px;background:linear-gradient(45deg,#667eea,#764ba2);color:#fff;border:none;border-radius:12px;font-size:16px;font-weight:600;cursor:pointer;transition:transform .2s,box-shadow .2s}
.btn:hover{transform:translateY(-2px);box-shadow:0 8px 25px rgba(102,126,234,.4)}
.btn:active{transform:translateY(0)}
.msg{margin-bottom:16px;padding:12px;border-radius:10px;font-size:14px;display:none;text-align:center}
.msg.err{display:block;background:#f8d7da;color:#721c24;border:1px solid #f5c6cb}
</style>
</head>
<body>
<div class="card">
  <h2>设备编号生成器</h2>
  <div id="msgBox" class="msg"></div>
  <div class="form-group">
    <label>账号</label>
    <input type="text" id="username" placeholder="请输入账号" autofocus />
  </div>
  <div class="form-group">
    <label>密码</label>
    <input type="password" id="password" placeholder="请输入密码" />
  </div>
  <button class="btn" onclick="doLogin()">登 录</button>
</div>
<script>
async function doLogin(){
  const u=document.getElementById('username').value.trim();
  const p=document.getElementById('password').value;
  if(!u||!p){showErr('请输入账号和密码');return;}
  try{
    const r=await fetch('/api/login',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({username:u,password:p})});
    const d=await r.json();
    if(d.ok){window.location.href='/';}else{showErr(d.error||'登录失败');}
  }catch(e){showErr('网络错误');}
}
function showErr(t){const b=document.getElementById('msgBox');b.textContent=t;b.className='msg err';}
document.getElementById('password').addEventListener('keydown',function(e){if(e.key==='Enter')doLogin();});
document.getElementById('username').addEventListener('keydown',function(e){if(e.key==='Enter')document.getElementById('password').focus();});
</script>
</body>
</html>`);
});

// Login API
app.post('/api/login', (req, res) => {
  const { username, password } = req.body || {};
  if (username === AUTH_USER && password === AUTH_PASS) {
    const token = generateToken();
    activeSessions.add(token);
    res.setHeader('Set-Cookie', `token=${token}; Path=/; HttpOnly; Max-Age=86400`);
    res.json({ ok: true });
  } else {
    res.status(401).json({ error: '账号或密码错误' });
  }
});

// Logout
app.get('/logout', (req, res) => {
  const cookies = parseCookies(req);
  if (cookies.token) activeSessions.delete(cookies.token);
  res.setHeader('Set-Cookie', 'token=; Path=/; HttpOnly; Max-Age=0');
  res.redirect('/login');
});

// ========== JSON file storage ==========
const DATA_FILE_LOCAL = join(__dirname, 'codes.json');
const DATA_FILE_PARENT = join(__dirname, '..', 'codes.json');
const DATA_FILE = existsSync(DATA_FILE_LOCAL)
  ? DATA_FILE_LOCAL
  : (existsSync(DATA_FILE_PARENT) ? DATA_FILE_PARENT : DATA_FILE_LOCAL);
let store = { nextId: 1, records: [] };

function loadData() {
  try {
    if (existsSync(DATA_FILE)) {
      store = JSON.parse(readFileSync(DATA_FILE, 'utf-8'));
    }
  } catch (e) {
    console.log('Load data error, using empty store');
  }
}

function saveData() {
  writeFileSync(DATA_FILE, JSON.stringify(store, null, 2), 'utf-8');
}

loadData();

// ========== 4-digit code generator ==========
const ALPHABET = '0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ';

const CODE_LENGTH = 16;
const LEGACY_CODE_LENGTH = 4;

function isValidCodeLength(len) {
  return len === CODE_LENGTH || len === LEGACY_CODE_LENGTH;
}

function generateCode() {
  let code = '';
  for (let i = 0; i < CODE_LENGTH; i++) {
    code += ALPHABET[Math.floor(Math.random() * ALPHABET.length)];
  }
  return code;
}

function codeExists(code) {
  return store.records.some(r => r.code === code);
}

function generateUniqueCode() {
  for (let i = 0; i < 10000; i++) {
    const code = generateCode();
    if (!codeExists(code)) return code;
  }
  return null;
}

// API auth middleware (returns 401 JSON instead of redirect)
function requireApiAuth(req, res, next) {
  const cookies = parseCookies(req);
  if (cookies.token && activeSessions.has(cookies.token)) {
    return next();
  }
  res.status(401).json({ error: '未登录，请先登录' });
}

// ========== API (all protected) ==========
app.use('/api/generate', requireApiAuth);
app.use('/api/save', requireApiAuth);
app.use('/api/search', requireApiAuth);
app.use('/api/delete', requireApiAuth);
app.use('/api/update', requireApiAuth);
app.use('/api/export', requireApiAuth);
app.use('/api/import', requireApiAuth);

app.get('/api/generate', (req, res) => {
  const code = generateUniqueCode();
  if (!code) return res.status(500).json({ error: 'pool exhausted' });
  res.json({ code });
});

app.post('/api/save', (req, res) => {
  const { customer, code } = req.body || {};
  if (!customer || !customer.trim()) return res.status(400).json({ error: 'missing customer' });
  const trimmed = String(code || '').trim();
  if (!trimmed || !isValidCodeLength(trimmed.length)) return res.status(400).json({ error: 'invalid code' });
  const upper = trimmed.toUpperCase();
  if (codeExists(upper)) return res.status(409).json({ error: 'code already used' });
  const record = {
    id: store.nextId++,
    customer_name: customer.trim(),
    code: upper,
    created_at: new Date().toLocaleString('zh-CN', { timeZone: 'Asia/Shanghai' })
  };
  store.records.push(record);
  saveData();
  res.json({ ok: true });
});

app.get('/api/search', (req, res) => {
  const q = (req.query.q || '').trim().toLowerCase();
  let list = store.records;
  if (q) {
    list = list.filter(r => r.customer_name.toLowerCase().includes(q) || r.code.toLowerCase().includes(q));
  }
  list = [...list].reverse();
  res.json({ list, total: store.records.length });
});

app.post('/api/delete', (req, res) => {
  const { id } = req.body || {};
  if (!id) return res.status(400).json({ error: 'missing id' });
  const idx = store.records.findIndex(r => r.id === id);
  if (idx === -1) return res.status(404).json({ error: 'not found' });
  store.records.splice(idx, 1);
  saveData();
  res.json({ ok: true });
});

// Update existing record
app.put('/api/update', (req, res) => {
  const { id, customer, code } = req.body || {};
  if (!id) return res.status(400).json({ error: 'missing id' });
  if (!customer || !customer.trim()) return res.status(400).json({ error: 'missing customer' });
  
  const trimmed = String(code || '').trim();
  if (!trimmed || !isValidCodeLength(trimmed.length)) return res.status(400).json({ error: 'invalid code' });
  const upper = trimmed.toUpperCase();
  
  const idx = store.records.findIndex(r => r.id === id);
  if (idx === -1) return res.status(404).json({ error: 'record not found' });
  
  // Check if code already exists (excluding current record)
  const existingRecord = store.records.find(r => r.code === upper && r.id !== id);
  if (existingRecord) return res.status(409).json({ error: 'code already used' });
  
  // Update record
  store.records[idx].customer_name = customer.trim();
  store.records[idx].code = upper;
  saveData();
  res.json({ ok: true });
});

// Export as .xlsx Excel
app.get('/api/export', async (req, res) => {
  const q = (req.query.q || '').trim().toLowerCase();
  let list = store.records;
  if (q) {
    list = list.filter(r => r.customer_name.toLowerCase().includes(q) || r.code.toLowerCase().includes(q));
  }
  list = [...list].reverse();

  const wb = new ExcelJS.Workbook();
  const ws = wb.addWorksheet('device-codes');
  ws.columns = [
    { header: 'ID', key: 'id', width: 10 },
    { header: '客户名', key: 'customer_name', width: 30 },
    { header: '随机码', key: 'code', width: 20 },
    { header: '生成时间', key: 'created_at', width: 22 },
  ];
  ws.getRow(1).font = { bold: true };
  list.forEach(r => ws.addRow(r));

  res.setHeader('Content-Type', 'application/vnd.openxmlformats-officedocument.spreadsheetml.sheet');
  res.setHeader('Content-Disposition', 'attachment; filename=device-codes.xlsx');
  await wb.xlsx.write(res);
  res.end();
});

// Import from .xlsx Excel
const upload = multer({ storage: multer.memoryStorage() });
app.post('/api/import', upload.single('file'), async (req, res) => {
  if (!req.file) return res.status(400).json({ error: 'no file' });
  try {
    const wb = new ExcelJS.Workbook();
    await wb.xlsx.load(req.file.buffer);
    const ws = wb.worksheets[0];
    if (!ws) return res.status(400).json({ error: 'empty workbook' });

    let imported = 0;
    let skipped = 0;
    let errors = [];

    ws.eachRow((row, rowNum) => {
      if (rowNum === 1) return; // skip header
      const customerName = String(row.getCell(2).value || '').trim();
      const code = String(row.getCell(3).value || '').trim().toUpperCase();
      if (!customerName || !code || !isValidCodeLength(code.length)) {
        skipped++;
        return;
      }
      if (codeExists(code)) {
        skipped++;
        errors.push(code + ' already exists');
        return;
      }
      store.records.push({
        id: store.nextId++,
        customer_name: customerName,
        code: code,
        created_at: new Date().toLocaleString('zh-CN', { timeZone: 'Asia/Shanghai' })
      });
      imported++;
    });

    saveData();
    res.json({ ok: true, imported, skipped, errors: errors.slice(0, 10) });
  } catch (e) {
    res.status(500).json({ error: 'parse failed: ' + e.message });
  }
});

// ========== Home page ==========
app.get('/', requireAuth, (req, res) => {
  res.send(`<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>设备编号生成</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:'Segoe UI','Microsoft YaHei',sans-serif;background:#f0f2f5;min-height:100vh;display:flex;justify-content:center;align-items:flex-start;padding:40px 16px}
.card{background:#fff;border-radius:16px;box-shadow:0 4px 24px rgba(0,0,0,.08);padding:32px;max-width:640px;width:100%}
h2{font-size:22px;color:#1a1a2e;margin-bottom:24px;text-align:center}
label{display:block;font-size:13px;color:#666;margin-bottom:6px;font-weight:600}
input{width:100%;padding:12px 14px;border:1.5px solid #ddd;border-radius:10px;font-size:15px;outline:none;transition:border .2s}
input:focus{border-color:#4361ee}
input[readonly]{background:#f7f8fa;color:#333}
.row{display:flex;gap:14px;margin-bottom:18px}
.row>div{flex:1}
.btns{display:flex;gap:10px;margin-top:6px;flex-wrap:wrap}
.btn{padding:12px 20px;border:none;border-radius:10px;font-size:14px;font-weight:600;cursor:pointer;transition:all .15s}
.btn-gen{background:#4361ee;color:#fff}
.btn-gen:hover{background:#3a56d4}
.btn-save{background:#2ec4b6;color:#fff}
.btn-save:hover{background:#28b0a3}
.btn-admin{background:#f8f9fa;color:#333;border:1.5px solid #ddd}
.btn-admin:hover{background:#e9ecef}
.preview{margin-top:20px;padding:16px;background:#f0f4ff;border:1.5px solid #d0d9ff;border-radius:12px}
.preview-label{font-size:12px;color:#888;margin-bottom:6px}
.preview-text{font-size:20px;font-weight:700;color:#1a1a2e;word-break:break-all}
.msg{margin-top:14px;padding:12px;border-radius:10px;font-size:14px;display:none}
.msg.ok{display:block;background:#d4edda;color:#155724;border:1px solid #c3e6cb}
.msg.err{display:block;background:#f8d7da;color:#721c24;border:1px solid #f5c6cb}
.stats{text-align:center;margin-top:16px;font-size:12px;color:#aaa}
</style>
</head>
<body>
<div class="card">
  <h2>客户设备编号生成</h2>
  <div id="msgBox" class="msg"></div>
  <div class="row">
    <div>
      <label>客户名称（可修改）</label>
      <input id="customer" value="客户xxxxx" placeholder="输入客户名称"/>
    </div>
    <div>
      <label>随机码（16位）（只读）</label>
      <input id="code" readonly placeholder="点击生成"/>
    </div>
  </div>
  <div class="btns">
    <button class="btn btn-gen" onclick="gen()">生成随机码</button>
    <button class="btn btn-save" onclick="save()">保存</button>
    <button class="btn btn-admin" onclick="location.href='/admin'">后台管理</button>
  </div>
  <div class="preview">
    <div class="preview-label">命名预览</div>
    <div class="preview-text" id="previewText"></div>
  </div>
  <div class="stats" id="stats"></div>
</div>
<script>
function updatePreview(){
  const c=document.getElementById('customer').value||'';
  const code=document.getElementById('code').value||'';
  document.getElementById('previewText').textContent=c+'设备编号'+code;
}
function showMsg(text,type){
  const box=document.getElementById('msgBox');
  box.textContent=text;
  box.className='msg '+(type==='ok'?'ok':'err');
  setTimeout(()=>{box.style.display='none';box.className='msg'},3000);
}
async function gen(){
  try{
    const r=await fetch('/api/generate');
    const d=await r.json();
    if(d.code){document.getElementById('code').value=d.code;updatePreview();}
    else showMsg(d.error||'生成失败','err');
  }catch(e){showMsg('网络错误','err');}
}
async function save(){
  const customer=document.getElementById('customer').value.trim();
  const code=document.getElementById('code').value.trim();
  if(!customer){showMsg('请输入客户名称','err');return;}
  if(!code||!(code.length===4||code.length===16)){showMsg('请先点击"生成随机码"','err');return;}
  try{
    const r=await fetch('/api/save',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({customer,code})});
    const d=await r.json();
    if(d.ok){showMsg('保存成功！','ok');document.getElementById('code').value='';updatePreview();loadStats();}
    else showMsg(d.error||'保存失败','err');
  }catch(e){showMsg('网络错误','err');}
}
async function loadStats(){
  try{const r=await fetch('/api/search?q=');const d=await r.json();document.getElementById('stats').textContent='已使用 '+d.total+' 个编号（16位，理论容量 36^16）';}catch(e){}
}
document.getElementById('customer').addEventListener('input',updatePreview);
updatePreview();
loadStats();
</script>
</body>
</html>`);
});

// ========== Admin page ==========
app.get('/admin', requireAuth, (req, res) => {
  res.send(`<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>后台管理 - 设备编号</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:'Segoe UI','Microsoft YaHei',sans-serif;background:#f0f2f5;padding:24px 16px}
.wrap{max-width:1000px;margin:0 auto}
h2{font-size:22px;color:#1a1a2e;margin-bottom:20px;text-align:center}
.top{display:flex;gap:10px;flex-wrap:wrap;margin-bottom:18px;align-items:center}
.top input{flex:1;min-width:200px;padding:12px 14px;border:1.5px solid #ddd;border-radius:10px;font-size:14px;outline:none}
.top input:focus{border-color:#4361ee}
.btn{padding:12px 18px;border:none;border-radius:10px;font-size:13px;font-weight:600;cursor:pointer;transition:all .15s;text-decoration:none;display:inline-block}
.btn-search{background:#4361ee;color:#fff}
.btn-search:hover{background:#3a56d4}
.btn-export{background:#2ec4b6;color:#fff}
.btn-export:hover{background:#28b0a3}
.btn-import{background:#f39c12;color:#fff}
.btn-import:hover{background:#e67e22}
.file-input{display:none}
.btn-back{background:#f8f9fa;color:#333;border:1.5px solid #ddd}
.btn-back:hover{background:#e9ecef}
.btn-del{background:#e74c3c;color:#fff;padding:6px 14px;font-size:12px;border-radius:8px}
.btn-del:hover{background:#c0392b}
.btn-edit{background:#3498db;color:#fff;padding:6px 12px;font-size:12px;border-radius:8px;margin-right:4px}
.btn-edit:hover{background:#2980b9}
.btn-save{background:#27ae60;color:#fff;padding:6px 12px;font-size:12px;border-radius:8px;margin-right:4px}
.btn-save:hover{background:#229954}
.btn-cancel{background:#95a5a6;color:#fff;padding:6px 12px;font-size:12px;border-radius:8px;margin-right:4px}
.btn-cancel:hover{background:#7f8c8d}
.btn-secondary{background:#95a5a6;color:#fff}
.edit-mode{width:100%;padding:4px 8px;border:1px solid #ddd;border-radius:4px;font-size:13px}
.view-mode{display:inline}
.edit-mode{display:none}
.msg{margin-bottom:14px;padding:12px;border-radius:10px;font-size:14px;display:none}
.msg.ok{display:block;background:#d4edda;color:#155724;border:1px solid #c3e6cb}
table{width:100%;border-collapse:collapse;background:#fff;border-radius:12px;overflow:hidden;box-shadow:0 2px 12px rgba(0,0,0,.06)}
th{background:#34495e;color:#fff;padding:14px 12px;text-align:left;font-size:13px}
td{padding:14px 12px;border-bottom:1px solid #f0f0f0;font-size:14px}
tr:hover td{background:#f8f9fa}
.empty{text-align:center;color:#aaa;padding:40px;font-style:italic}
.stats{text-align:center;margin-top:14px;font-size:12px;color:#aaa}
</style>
</head>
<body>
<div class="wrap">
  <h2>后台管理（查询 / 删除 / 导出）</h2>
  <div id="msgBox" class="msg"></div>
  <div class="top">
    <input id="searchInput" placeholder="按客户名或随机码搜索..." onkeydown="if(event.key==='Enter')doSearch()"/>
    <button class="btn btn-search" onclick="doSearch()">搜索</button>
    <button class="btn btn-export" onclick="doExport()">导出Excel</button>
    <button class="btn btn-import" onclick="document.getElementById('fileInput').click()">导入Excel</button>
    <input type="file" id="fileInput" class="file-input" accept=".xlsx" onchange="doImport(this)"/>
    <a class="btn btn-back" href="/">返回前台</a>
  </div>
  <table>
    <thead><tr><th style="width:70px">ID</th><th>客户名</th><th style="width:180px">随机码</th><th style="width:180px">生成时间</th><th style="width:120px">操作</th></tr></thead>
    <tbody id="tbody"><tr><td colspan="5" class="empty">加载中...</td></tr></tbody>
  </table>
  <div class="stats" id="stats"></div>
</div>
<script>
let currentQ='';
function showMsg(text){
  const box=document.getElementById('msgBox');
  box.textContent=text;box.className='msg ok';
  setTimeout(()=>{box.style.display='none';box.className='msg'},3000);
}
async function doSearch(){
  currentQ=document.getElementById('searchInput').value.trim();
  try{
    const r=await fetch('/api/search?q='+encodeURIComponent(currentQ));
    const d=await r.json();
    const tbody=document.getElementById('tbody');
    if(d.list.length===0){
      tbody.innerHTML='<tr><td colspan="5" class="empty">暂无数据</td></tr>';
    }else{
      tbody.innerHTML=d.list.map(item=>'<tr id="row-'+item.id+'"><td>'+item.id+'</td><td><span class="view-mode" id="customer-view-'+item.id+'">'+esc(item.customer_name)+'</span><input class="edit-mode" style="display:none" id="customer-edit-'+item.id+'" value="'+esc(item.customer_name)+'"></td><td><span class="view-mode" id="code-view-'+item.id+'"><b>'+esc(item.code)+'</b></span><input class="edit-mode" style="display:none" id="code-edit-'+item.id+'" value="'+esc(item.code)+'"></td><td>'+esc(item.created_at)+'</td><td><button class="btn btn-primary btn-edit" onclick="startEdit('+item.id+')" id="edit-btn-'+item.id+'">编辑</button><button class="btn btn-success btn-save" style="display:none" onclick="saveEdit('+item.id+')" id="save-btn-'+item.id+'">保存</button><button class="btn btn-secondary btn-cancel" style="display:none" onclick="cancelEdit('+item.id+')" id="cancel-btn-'+item.id+'">取消</button><button class="btn btn-del" onclick="doDel('+item.id+')">删除</button></td></tr>').join('');
    }
    document.getElementById('stats').textContent='当前显示 '+d.list.length+' 条，已使用 '+d.total+' 个编号（16位，理论容量 36^16）';
  }catch(e){document.getElementById('tbody').innerHTML='<tr><td colspan="5" class="empty">加载失败</td></tr>';}
}
async function doDel(id){
  if(!confirm('确认删除？删除后该号码将释放回号池，可再次生成。'))return;
  try{
    const r=await fetch('/api/delete',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({id})});
    const d=await r.json();
    if(d.ok){showMsg('删除成功，号码已释放回号池');doSearch();}
  }catch(e){}
}
function doExport(){
  window.location.href='/api/export?q='+encodeURIComponent(currentQ);
}
async function doImport(input){
  if(!input.files||!input.files[0])return;
  const fd=new FormData();
  fd.append('file',input.files[0]);
  try{
    const r=await fetch('/api/import',{method:'POST',body:fd});
    const d=await r.json();
    if(d.ok){
      let msg='导入成功！新增 '+d.imported+' 条';
      if(d.skipped>0) msg+='，跳过 '+d.skipped+' 条（重复或无效）';
      showMsg(msg);
      doSearch();
    }else{
      showMsg('导入失败: '+(d.error||'unknown'));
    }
  }catch(e){showMsg('导入失败: 网络错误');}
  input.value='';
}
function esc(s){return String(s||'').replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;');}
function startEdit(id){
  // Hide view mode, show edit mode
  document.getElementById('customer-view-'+id).style.display='none';
  document.getElementById('customer-edit-'+id).style.display='inline';
  document.getElementById('code-view-'+id).style.display='none';
  document.getElementById('code-edit-'+id).style.display='inline';
  
  // Hide edit button, show save/cancel buttons
  document.getElementById('edit-btn-'+id).style.display='none';
  document.getElementById('save-btn-'+id).style.display='inline';
  document.getElementById('cancel-btn-'+id).style.display='inline';
}
function cancelEdit(id){
  // Show view mode, hide edit mode
  document.getElementById('customer-view-'+id).style.display='inline';
  document.getElementById('customer-edit-'+id).style.display='none';
  document.getElementById('code-view-'+id).style.display='inline';
  document.getElementById('code-edit-'+id).style.display='none';
  
  // Show edit button, hide save/cancel buttons
  document.getElementById('edit-btn-'+id).style.display='inline';
  document.getElementById('save-btn-'+id).style.display='none';
  document.getElementById('cancel-btn-'+id).style.display='none';
  
  // Reset input values
  const originalCustomer = document.getElementById('customer-view-'+id).textContent;
  const originalCode = document.getElementById('code-view-'+id).textContent;
  document.getElementById('customer-edit-'+id).value = originalCustomer;
  document.getElementById('code-edit-'+id).value = originalCode;
}
async function saveEdit(id){
  const customer = document.getElementById('customer-edit-'+id).value.trim();
  const code = document.getElementById('code-edit-'+id).value.trim();
  
  if(!customer){showMsg('请输入客户名称');return;}
  if(!code||!(code.length===4||code.length===16)){showMsg('随机码长度必须为4位或16位');return;}
  
  try{
    const r = await fetch('/api/update',{method:'PUT',headers:{'Content-Type':'application/json'},body:JSON.stringify({id,customer,code})});
    const d = await r.json();
    if(d.ok){
      showMsg('修改成功！');
      // Update view mode with new values
      document.getElementById('customer-view-'+id).textContent = customer;
      document.getElementById('code-view-'+id).innerHTML = '<b>'+esc(code)+'</b>';
      cancelEdit(id);
    }else{
      showMsg(d.error||'修改失败');
    }
  }catch(e){
    showMsg('网络错误');
  }
}
doSearch();
</script>
</body>
</html>`);
});

app.listen(PORT, () => {
  console.log('============================================');
  console.log('  Device Code Generator started');
  console.log('  Home:  http://localhost:' + PORT + '/');
  console.log('  Admin: http://localhost:' + PORT + '/admin');
  console.log('============================================');
});

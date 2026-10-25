const fs = require('node:fs');
const vm = require('node:vm');
const assert = require('node:assert/strict');
const path = require('node:path');
const source = fs.readFileSync(path.join(__dirname,'../TVTDataBroadcastingWV2/TVTDataBroadcastingWV2.cpp'),'utf8');
const htmlSource = source.slice(source.indexOf('static const wchar_t kMomentumHtml[]'), source.indexOf('void CDataBroadcastingWV2::CreateMomentumWebViewController'));
const html = [...htmlSource.matchAll(/LR"HTML\(([\s\S]*?)\)HTML"/g)].map(m=>m[1]).join('');
const script = html.match(/<script>([\s\S]*?)<\/script>/)[1];
new vm.Script(script);
// 接続状況の行はTVTestのパネル色に追従させる。固定色に戻すと暗いパネルで読めない。
assert.match(html,/--sys:#[0-9a-f]{6}/,'接続状況の色は:rootに既定値を持つ');
assert.match(html,/\.le\.system \.lx\{color:var\(--sys\)\}/,'接続状況の色はテーマ変数を使う');
assert.match(script,/setProperty\('--sys',m\.sys\)/,'テーマ通知で接続状況の色を更新する');
const themeFn=source.slice(source.indexOf('void CDataBroadcastingWV2::SendMomentumTheme'),source.indexOf('void CDataBroadcastingWV2::SendMomentumChannels'));
assert.match(themeFn,/"sys"/,'テーマ通知に接続状況の色を載せる');
class Element {
  constructor(tag) { this.tag=tag; this.children=[]; this.dataset={}; this.style={}; this.className=''; this.classList={toggle:(name,on)=>{assert.equal(typeof on,'boolean','NG判定は常にbooleanを渡す');const c=new Set(this.className.split(' ')); on?c.add(name):c.delete(name);this.className=[...c].join(' ');}}; }
  appendChild(e) { if(e.tag==='fragment')this.children.push(...e.children);else this.children.push(e); }
  removeChild(e) { this.children.splice(this.children.indexOf(e),1); }
  get childElementCount(){return this.children.length;}
  get firstChild(){return this.children[0];}
  get scrollHeight(){return this.children.length*20;}
}
const logEl=new Element('div');logEl.hidden=false;
const context={document:{createElement:t=>new Element(t),createDocumentFragment:()=>new Element('fragment')},logEl,ngUsers:new Set(['blocked']),atBottom:true};
vm.createContext(context);
vm.runInContext(script.slice(script.indexOf('function applyNg('),script.indexOf("logEl.addEventListener('contextmenu'")),context);
context.logAdd([{system:true,date:1792970000,text:'コメントサーバーに接続しました。'},{userId:'blocked',date:1792970001,text:'<img src=x onerror=alert(1)>'}]);
assert.equal(logEl.children[0].className,'le system');
assert.equal(logEl.children[0].children.length,2);
assert.equal(logEl.children[0].children[0].textContent.length,8);
assert.equal(logEl.children[0].children[1].textContent,'コメントサーバーに接続しました。');
assert.equal(logEl.children[0].dataset.u,undefined);
assert.match(logEl.children[1].className,/ng/);
assert.equal(logEl.children[1].children[2].textContent,'<img src=x onerror=alert(1)>');
context.logAdd([{date:1792970002,text:'IDなしコメント'}]);
for(const users of [[],['blocked'],[],['blocked']]) {
  context.setNgUsers(users);
  assert.equal(logEl.children[0].className,'le system','接続ログをNG扱いしない');
  assert.equal(logEl.children[2].className,'le','IDなしコメントをNG扱いしない');
  assert.equal(logEl.children[1].className.includes('ng'),users.includes('blocked'),'ユーザーNGの追加と解除');
}
logEl.children[0].dataset.nb='1';
context.setNgUsers(['blocked']);
assert.equal(logEl.children[0].className,'le system','システム行をすべてのNG判定から除外');
context.atBottom=false;logEl.scrollTop=17;
context.logAdd(Array.from({length:510},(_,i)=>({system:i%2===0,date:1792970002+i,text:String(i)})));
assert.equal(logEl.childElementCount,500);
assert.equal(logEl.firstChild.children.at(-1).textContent,'10');
assert.equal(logEl.scrollTop,17);
context.atBottom=true;context.logAdd([{system:true,text:'再接続',date:1792971000}]);
assert.equal(logEl.scrollTop,logEl.scrollHeight);
console.log('Panel script syntax and log rendering checks passed');

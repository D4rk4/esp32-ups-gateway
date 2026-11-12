#!/usr/bin/env node
/* Actual production-JS contract checks; no browser and no network process. */
'use strict';
const fs=require('fs'),vm=require('vm'),assert=require('assert/strict'),path=require('path');
const root=path.resolve(__dirname,'..');
const html=fs.readFileSync(path.join(root,'web/index.html'),'utf8');
const source=html.match(/<script id="gateway-script">([\s\S]*?)<\/script>/)[1].replace(/\nboot\(\);\s*$/,'\n');
class Element{constructor(id=''){this.id=id;this.innerHTML='';this.textContent='';this.hidden=false;this.value='';this.disabled=false;this.dataset={};this.style={};this.listeners={};this.classList={add(){},remove(){},toggle(){}};this.elements={namedItem(){return null;}};this.isConnected=true;}addEventListener(type,fn){this.listeners[type]=fn;}querySelectorAll(){return [];}querySelector(){return null;}setAttribute(key,value){this[key]=value;}appendChild(){}remove(){}focus(){}reset(){}click(){}}
const elements=new Map();const get=id=>{if(!elements.has(id))elements.set(id,new Element(id));return elements.get(id);};
const document={getElementById:get,querySelectorAll:()=>[],querySelector:()=>new Element(),addEventListener(){},createElement:tag=>new Element(tag),body:{style:{},appendChild(){},insertBefore(){},firstChild:null},head:{appendChild(){}},hidden:false,activeElement:null};
let requests=[],handler=async()=>({ok:true}),testNow=null,captureTimers=false,timerSequence=0;const capturedTimers=new Map();class TestDate extends Date{static now(){return testNow===null?Date.now():testNow;}}const scheduleTimeout=(fn,ms)=>{if(!captureTimers)return setTimeout(fn,ms);const id='captured-timer-'+(++timerSequence);capturedTimers.set(id,{fn,ms});return id;};const cancelTimeout=id=>{if(typeof id==='string')capturedTimers.delete(id);else clearTimeout(id);};
const context=vm.createContext({document,window:{addEventListener(){}},location:{hash:''},history:{replaceState(){}},console,TextEncoder,URL,Blob,Headers,Response,AbortController,Date:TestDate,Number,String,Math,JSON,Set,Promise,Error,parseInt,encodeURIComponent,decodeURIComponent,setInterval(){},setTimeout:scheduleTimeout,clearTimeout:cancelTimeout,fetch:async(url,init)=>{requests.push({url,init});const data=await handler(url,init);return data instanceof Response?data:new Response(JSON.stringify(data),{headers:{'content-type':'application/json'}});}});
vm.runInContext(source,context,{filename:'production-ui.js'});
const run=code=>vm.runInContext(code,context);
(async()=>{
 assert(!html.includes('__ASSET_'),'Every decorative reference asset embedded');
 assert(!/localStorage|sessionStorage|Authorization\s*:/.test(source),'Credentials must not be stored or sent via Basic');
 assert(!html.includes('TEST FIXTURE'),'Production HTML must contain no fixture label/data');
 const ids=[...html.split('<script id="gateway-script">')[0].matchAll(/\bid="([^"]+)"/g)].map(m=>m[1]);assert.equal(ids.length,new Set(ids).size,'Static element IDs unique');
 const required=['home','alarms','status','control','serial','schedules','events','data','security','network','notifications','general','about'];for(const id of required)assert(html.includes('id="page-'+id+'"'),'Page '+id);
 assert(source.includes("X-Session-Activity"),'Idle session activity protocol used');
 run("state.session={role:'admin',permissions:{command:true,configure:true},csrf:'secret-csrf',drivers:[{id:'megatec_q1',capabilities:63}],display:{temperature_unit:'C',date_format:'YYYY-MM-DD'}};state.status={thresholds:{alarm_load_high:90,alarm_delay_s:5},ups:{connected:true,status_valid:true,age_ms:120,stale_ms:10000,driver:'megatec_q1'},control:{enabled:true,power_enabled:true}};state.statusGood=true;state.lastStatusAt=Date.now();");
 run("renderLoadMeter({load_percent:37.5,output_mode:'online'},true)");let gauge=get('overview-load').innerHTML;
 assert.equal((gauge.match(/gauge-cell /g)||[]).length,20);assert.equal((gauge.match(/gauge-cell on/g)||[]).length,8);assert(gauge.includes('apc-gauge normal'));assert(gauge.includes('37.5'));assert(gauge.includes('90%'));
 run("renderLoadMeter({load_percent:80,output_mode:'online'},true)");assert(get('overview-load').innerHTML.includes('apc-gauge attention'));
 run("renderLoadMeter({load_percent:96,output_mode:'online'},true)");assert(get('overview-load').innerHTML.includes('apc-gauge over'));
 run("renderLoadMeter({load_percent:37.5,output_mode:'online'},false)");gauge=get('overview-load').innerHTML;assert(gauge.includes('apc-gauge unavailable'));assert(!gauge.includes('gauge-cell on'));assert(!gauge.includes('aria-valuenow'));assert(!gauge.includes('37.5'));
 run("renderLoadMeter({load_percent:null,output_mode:'online'},true)");gauge=get('overview-load').innerHTML;assert(!gauge.includes('gauge-cell on'));assert(gauge.includes('Не сообщается драйвером'));

 // Local expiry must repaint current measurements and command gates even while
 // the next status request is still in flight; no additional request is needed.
 testNow=1700000000000;captureTimers=true;requests=[];
 run("state.status.ups={connected:true,status_valid:true,age_ms:100,stale_ms:700,driver:'megatec_q1',load_percent:37.5,output_mode:'online'};state.lastStatusAt=Date.now();state.statusGood=true;renderCore();");
 assert.equal(run('fresh()'),true);let deadline=run('state.freshnessTimer');assert.equal(capturedTimers.get(deadline).ms,600);assert(get('overview-load').innerHTML.includes('gauge-cell on'));
 testNow+=600;run('state.polling=true');const expiry=capturedTimers.get(deadline);capturedTimers.delete(deadline);expiry.fn();
 assert.equal(run('fresh()'),false);assert(get('overview-load').innerHTML.includes('apc-gauge unavailable'));assert(!get('overview-load').innerHTML.includes('gauge-cell on'));assert(get('command-options').innerHTML.includes('Нет свежего состояния ИБП'));assert.equal(get('review-command').disabled,true);assert.equal(requests.length,0);assert.equal(run('state.statusGood'),true);
 run("state.polling=false;state.status.ups.stale_ms=10000;state.status.ups.age_ms=1000;state.lastStatusAt=Date.now();renderCore();");deadline=run('state.freshnessTimer');assert.equal(capturedTimers.get(deadline).ms,9000);testNow+=8999;assert.equal(run('fresh()'),true);testNow++;capturedTimers.get(deadline).fn();assert.equal(run('fresh()'),false);assert(!get('overview-load').innerHTML.includes('gauge-cell on'));
 run("state.status.ups.stale_ms=300000;state.status.ups.age_ms=0;state.lastStatusAt=Date.now()-15000");assert.equal(run('fresh()'),false,'15 second maximum client cache cap');
 run("state.lastStatusAt=Date.now()+1");assert.equal(run('fresh()'),false,'Backward clock jump fails closed');
 run("state.lastStatusAt=Date.now();state.status.ups.age_ms=null");assert.equal(run('fresh()'),false,'Unknown sample age fails closed');run("state.status.ups.age_ms=-1");assert.equal(run('fresh()'),false,'Negative sample age fails closed');
 run("state.status.ups.age_ms=0;state.status.ups.stale_ms=null");assert.equal(run('fresh()'),false,'Malformed stale limit fails closed');run("state.status.ups.stale_ms=699");assert.equal(run('fresh()'),false);run("delete state.status.ups.stale_ms");assert.equal(run('fresh()'),true,'Absent legacy fixture field uses minimum 700 ms bound');testNow+=700;assert.equal(run('fresh()'),false);
 // Receipt timestamp is captured when the status request resolves, not after
 // slower alarm/time requests settle.
 let releaseTail;const tail=new Promise(resolve=>{releaseTail=resolve;});const receivedAt=testNow;
 handler=async url=>{if(url==='/api/status')return {thresholds:{alarm_load_high:90,alarm_delay_s:5},ups:{connected:true,status_valid:true,age_ms:100,stale_ms:700,driver:'megatec_q1',load_percent:37.5},control:{enabled:true,power_enabled:true}};await tail;return url==='/api/alarms'?{alarms:[],critical:0,warning:0}:{valid:false,timezone:'UTC0',ntp_enabled:true};};
 const refreshing=run('refreshCore()');await new Promise(resolve=>setImmediate(resolve));testNow+=600;releaseTail();await refreshing;
 assert.equal(run('state.lastStatusAt'),receivedAt);assert.equal(run('fresh()'),false);assert(!get('overview-load').innerHTML.includes('gauge-cell on'));
 run('clearTimeout(state.freshnessTimer);state.freshnessTimer=null');capturedTimers.clear();captureTimers=false;testNow=null;
 run("state.status.ups.age_ms=120;state.status.ups.stale_ms=10000;state.lastStatusAt=Date.now();state.statusGood=true;state.polling=false;");
 assert.equal(run("formatMetric('battery_charge',null,true).text"),'—');assert.equal(run("formatMetric('input_voltage',230,false).text"),'—');assert.equal(run("formatMetric('load_percent',0,true).text"),'0.0');
 run("state.session.display.temperature_unit='F'");assert.equal(run("formatMetric('ups_temperature',0,true).text"),'32.0');run("state.session.display.temperature_unit='C'");
 assert.equal(run("commandReason(commands[5])"),'');run("state.session.permissions.command=false");assert(run("commandReason(commands[5])").includes('чтение'));run("state.session.permissions.command=true;state.status.control.power_enabled=false");assert(run("commandReason(commands[5])").includes('питания'));run("state.status.control.power_enabled=true;state.statusGood=false");assert(run("commandReason(commands[0])").includes('свежего'));run("state.statusGood=true;state.session.drivers[0].capabilities=1");assert(run("commandReason(commands[1])").includes('драйвером'));run("state.session.drivers[0].capabilities=63");
 assert.equal(run("bytes('я'.repeat(16))"),32);assert.throws(()=>run("bounded('я'.repeat(17),1,32,'Name')"),/байт/);assert.throws(()=>run("bounded('abc\\n',0,96,'Contact')"),/управляющих/);assert.throws(()=>run("integer('1.5',1,200,'Threshold')"),/целое/);assert.throws(()=>run("ipv4('192.168.001.1','IP')"),/IPv4/);assert.throws(()=>run("unicast('127.0.0.1','IP')"),/unicast/);assert.throws(()=>run("host('bad..host','Host')"),/hostname/);assert.equal(run("host('pool.ntp.org','NTP')"),'pool.ntp.org');

 run("state.time={min_utc:1577836800,max_utc:2147483647};");
 assert.equal(run("parseUtcInput('2038-01-19T03:14:07','UTC')"),2147483647);assert.throws(()=>run("parseUtcInput('2038-01-19T03:14:08','UTC')"),/допустимо/);assert.throws(()=>run("parseUtcInput('2026-02-30T12:00:00','UTC')"),/календарная/);
 run("state.config={controls_enabled:false,power_controls_enabled:false,driver:'megatec_q1',timezone:'UTC0',schedules:[{id:1,enabled:true,name:'Power',command:'output_off',delay_minutes:1,restore_minutes:3,mode:'weekly',at_utc:0,days_mask:2,minute_of_day:540}]};");
 assert.equal(run("configPowerConsent({location:'changed'})"),false);assert.equal(run("configPowerConsent({controls_enabled:true})"),true);assert.equal(run("configPowerConsent({power_controls_enabled:true})"),true);assert.equal(run("configPowerConsent({driver:'another_driver'})"),true);assert.equal(run("configPowerConsent({timezone:'MSK-3'})"),true);assert.equal(run("configPowerConsent({schedules:state.config.schedules})"),false);assert.equal(run("configPowerConsent({schedules:[Object.assign({},state.config.schedules[0],{delay_minutes:2})]})"),true);
 requests=[];handler=async(url)=>url==='/api/config'?{ok:true,reboot_required:true,schedules:[],controls_enabled:true,power_controls_enabled:true}:{};
 let review=await run("savePatch({controls_enabled:true,power_controls_enabled:true})");assert.equal(review.review_required,true);assert.equal(requests.length,0,'No power activation request before user review');
 get('config-power').value='WRONG';await get('dialog-confirm').onclick();assert.equal(requests.length,0,'Incorrect typed proof never sends request');assert(get('dialog-error').textContent.includes('POWER'));
 get('config-power').value='POWER';await get('dialog-confirm').onclick();assert.equal(requests.length,2);assert.equal(JSON.parse(requests[0].init.body).confirm,'POWER');assert.equal(run('state.pending'),true);
 requests=[];handler=async()=>({ok:true});run("state.activityPending=true");await run("api('/api/status')");await run("api('/api/alarms')");assert.equal(requests[0].init.headers['X-Session-Activity'],'1');assert.equal(requests[1].init.headers['X-Session-Activity'],undefined);await run("api('/api/config',{method:'POST',body:{location:'x'}})");assert.equal(requests[2].init.headers['X-CSRF-Token'],'secret-csrf');assert.equal(requests[2].init.credentials,'same-origin');assert.deepEqual(JSON.parse(requests[2].init.body),{location:'x'});
 handler=async()=>new Response(JSON.stringify({error:'Session expired'}),{status:401});await assert.rejects(run("api('/api/status')"),/Session expired/);assert.equal(run('state.session'),null);assert.equal(get('app-shell').hidden,true);assert.equal(get('login-shell').hidden,false);
 // Verify the offline server itself refuses all native-network fallthroughs,
 // honors roles/CSRF, preserves blank secrets and keeps saved/active distinct.
 const fixtureContext=vm.createContext({window:{},document,URL,Headers,Response,TextEncoder,Date,Number,String,Math,JSON,Set,Promise,Error,console});
 vm.runInContext(fs.readFileSync(path.join(root,'tests/ui_fixture_api.js'),'utf8'),fixtureContext,{filename:'ui_fixture_api.js'});
 const fetchFixture=fixtureContext.window.fetch;
 let r=await fetchFixture('https://example.org/unrelated');assert.equal(r.status,403);
 r=await fetchFixture('/api/session');assert.equal((await r.json()).authenticated,false);
 r=await fetchFixture('/api/login',{method:'POST',body:JSON.stringify({username:'viewer',password:'fixture-password-2026'})});let session=await r.json();assert.equal(session.role,'viewer');assert.equal(session.permissions.command,false);
 r=await fetchFixture('/api/config');assert.equal(r.status,403);
 r=await fetchFixture('/api/command',{method:'POST',headers:{'X-CSRF-Token':session.csrf},body:JSON.stringify({command:'test_short',confirm:'EXECUTE'})});assert.equal(r.status,403);
 r=await fetchFixture('/api/login',{method:'POST',body:JSON.stringify({username:'admin',password:'fixture-password-2026'})});session=await r.json();
 r=await fetchFixture('/api/config',{method:'POST',headers:{'X-CSRF-Token':'wrong'},body:'{}'});assert.equal(r.status,403);
 r=await fetchFixture('/api/config',{method:'POST',headers:{'X-CSRF-Token':session.csrf},body:JSON.stringify({location:'Changed',web_password:''})});assert.equal(r.status,200);
 r=await fetchFixture('/api/config');let config=await r.json();assert.equal(config.location,'Changed');assert.equal(config.web_password,'');assert.equal(config.web_password_set,true);assert.equal(fixtureContext.window.__UPS_FIXTURE__.active.location,'Стенд проверки · синтетические данные');
 r=await fetchFixture('/api/config/export');assert.equal((await r.json()).web_password,'fixture-password-2026');
 fixtureContext.window.__UPS_FIXTURE__.mode='lost';r=await fetchFixture('/api/status');const stale=await r.json();assert.equal(stale.ups.stale_ms,10000);assert.equal(stale.ups.connected,false);assert.equal(stale.ups.load_percent,null);assert.equal(stale.ups.battery_charge,null);
 r=await fetchFixture('/api/command',{method:'POST',headers:{'X-CSRF-Token':session.csrf},body:JSON.stringify({command:'test_short',confirm:'EXECUTE'})});assert.equal(r.status,409);
 console.log('PASS: actual production JS syntax/structure, APC segmented load normal/amber/high/stale/null, active stale deadline repaint and cache-age/clock guards, no fabricated metrics, role/capability/freshness gates, UTF-8/numeric/network/target-clock validation, typed POWER review and arming/retiming guards, activity/CSRF/section-patch/session-expiry contract; offline fixture auth/permissions/secret preservation/saved-active separation/no-network.');
})().catch(error=>{console.error(error);process.exitCode=1;});

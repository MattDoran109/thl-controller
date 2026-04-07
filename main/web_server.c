// ============================================================
//  web_server.c — HTTP server + embedded HTML dashboard
// ============================================================

#include "web_server.h"
#include "sensor_manager.h"
#include "controller.h"
#include "relay.h"
#include "config.h"
#include "sd_logger.h"
#include "wifi_manager.h"
#include "notifier.h"
#include "alert_manager.h"
#include "display.h"

#include "light_pwm.h"
#include "esp_http_server.h"
#include "esp_system.h"
#include "esp_log.h"
#include "cJSON.h"
#include "nvs.h"
#include "esp_heap_caps.h"
#include <string.h>
#include <stdlib.h>
#include <time.h>

static const char *TAG = "web";
static httpd_handle_t s_server = NULL;
extern volatile uint32_t g_ctrl_cycles;  // diagnostic: control task cycle counter
// Cached at server start; only device_name/hostname read by status handler.
// Avoids an NVS flash read on every 10-second poll.
static wifi_config_nvs_t s_device_cfg;

// ============================================================
// Embedded HTML dashboard
// ============================================================
// ============================================================
// Embedded HTML — /test hardware test page
// ============================================================
static const char HTML_TEST[] =
"<!DOCTYPE html><html lang='en'><head>"
"<meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
"<link rel='icon' type='image/png' href='data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAACAAAAAgCAYAAABzenr0AAACfUlEQVR4AeyUbUhTYRTH/89cdW9GC4wINMKwwOyTixRMEoJilZAwhEAQY1MosXatjCJUpMh0W4aVeY0ShCAEP/Qy6tPCgoy5jwUl0YuBRJ8Mt1uxnc4iYbi7tc2gkHu5f87zcp5zfvd/4THhHz8GgOGA4YDhwP/pAPVihHpwjWMXedDMqqFLKKUrWP23L059B0xog0A/onjMmgFhI8/r8QN3GOoWy8VAu6gD0mKBdAGEgk/iBF6KUxjnOCpacVmcxDGO+xnoAoCPyIENq/CQYTrZrSq6yyu8kemrC5CqCEO9+Q11GoRqzn3O7tjwAT6G6fr1uzwo4vW03owB4quyK3MM42O1YTnsDOJnhzaxzpIbjxjoBsd21tH4c/HjRIDxiS14GqhPW88mnbFcURqoEWWBAlEe+MLyr7H6R3cU335XXeReWVfYVYzxyQPxjefHiQDzO4uOFiBSQfTdTpHw4aTVEgEqy15j5/bhtFVhVWO5I0Hr2NBEybT6YtvawYmSKnew2N44VVB4cHpdePfnvFeotN7Xo0gE0MtKstbTM5N706vZBt1adziijUKgiiDemoQ471CkvU5FbnK65E6nS7qapAQyBhjo0zar3pB9yKNdtORY7kWIyoWJfJZ8yeZQ5HONyooxhyJNJWu4cF0X4LonlD/Q+3Wr6glXqt5vdtWtHVfdoT7Voz0wR+mMINOGKMi3bFba16jI7U6X7K+tFZGFxdOZ6wKYSXTnCHOzAPYIovUkxPuoiYbD5rlDDpfcwF/o5cZPGjqElk6TVDm6AM5WuY51xMGWOhSpP2Zrkys32NKSN5uqWDZ7ugDZFMr2jAFgOGA4sPQd+NP98BMAAP//Dttx7gAAAAZJREFUAwAno/lBlBw0IgAAAABJRU5ErkJggg=='>"
"<title>Test &mdash; Al Wall Controller</title>"
"<style>"
"*{box-sizing:border-box}"
"body{font-family:Arial,sans-serif;background:#1a1a2e;color:#eee;margin:0;padding:14px}"
"h1{color:#e94560;margin:0 0 12px;font-size:1.4em}"
".card{background:#16213e;border-radius:10px;padding:14px;margin-bottom:12px}"
"h3{color:#aaa;font-size:.85em;text-transform:uppercase;letter-spacing:.5px;margin:0 0 12px}"
".row{display:flex;align-items:center;gap:10px;margin-bottom:10px}"
".row label{min-width:120px;font-size:.9em;color:#aaa}"
".vl{min-width:44px;text-align:right;font-weight:bold;color:#e94560}"
"input[type=range]{flex:1;accent-color:#e94560}"
".btns{display:flex;gap:8px;flex-wrap:wrap;margin-top:4px}"
".btn{padding:9px 16px;border-radius:8px;border:2px solid #555;background:#16213e;"
"     color:#888;cursor:pointer;font-size:13px;font-weight:bold;min-width:56px}"
".btn.on{border-color:#4caf50;background:#162a1c;color:#4caf50}"
".btn-apply{background:#e94560;color:#fff;border:none;border-radius:8px;"
"           padding:10px 20px;cursor:pointer;font-size:14px;font-weight:bold}"
".note{font-size:11px;color:#666;margin-top:8px}"
".back{display:inline-block;color:#aaa;text-decoration:none;margin-bottom:14px;font-size:.9em}"
".tm-on{border-color:#f59e0b!important;background:#2a1a00!important;color:#f59e0b!important}"
"#tmbanner{display:none;background:#92400e;color:#fef3c7;text-align:center;"
           "padding:6px;font-size:13px;font-weight:bold;border-radius:8px;margin-bottom:12px}"
"</style></head><body>"
"<a class='back' href='/'>&#8592; Dashboard</a>"
"<h1>&#128295; Hardware Test</h1>"
"<div id='tmbanner'>&#9888; TEST MODE ACTIVE &mdash; schedules suspended</div>"
"<div class='card'>"
"<h3>Test Mode</h3>"
"<div class='btns'>"
"<button class='btn' id='tmbtn' onclick='toggleTestMode()'>Enable Test Mode</button>"
"</div>"
"<p class='note'>While active: all schedules and hysteresis are suspended. "
"Disabling clears all manual overrides and returns to normal control.</p>"
"</div>"
"<div class='card'><h3>Light Brightness</h3>"
"<div class='row'><label>Brightness</label>"
"<input type='range' id='sl' min='0' max='100' value='100' oninput='updSl(this.value)'>"
"<span class='vl' id='slv'>100%</span></div>"
"<div class='row'><label>Current PWM</label><span id='cur'>--</span></div>"
"<div class='btns'>"
"<button class='btn-apply' onclick='applyBright()'>Apply</button>"
"<button class='btn' onclick='holdBright()'>Hold</button>"
"<button class='btn' onclick='qset(0)'>Off</button>"
"<button class='btn' onclick='qset(25)'>25%</button>"
"<button class='btn' onclick='qset(50)'>50%</button>"
"<button class='btn' onclick='qset(75)'>75%</button>"
"<button class='btn' onclick='qset(100)'>Full</button>"
"</div>"
"<p class='note'>Override holds until the next scheduled on/off transition.</p>"
"</div>"
"<div class='card'>"
"<h3>Colour Temperature</h3>"
"<div class='btns' id='ctbtns'>"
"<button class='btn' id='ct0' onclick='setCT(0)'>&#10052; Full (all rows)</button>"
"<button class='btn' id='ct1' onclick='setCT(1)'>&#9728; Med (rows 1,3,5)</button>"
"<button class='btn' id='ct2' onclick='setCT(2)'>&#127844; Low (rows 2,4)</button>"
"</div>"
"<p class='note' id='ctnote'>--</p>"
"</div>"
"<div class='card'><h3>Panel Temperature</h3>"  
"<div class='row'><label>Cabinet temp</label><span class='vl' id='ptmp'>--</span><span style='font-size:.85em;color:#888;margin-left:4px'>&deg;C</span></div>"  
"</div>"  
"<div class='card'><h3>Relays</h3><div id='rels'><span style='color:#666'>Loading...</span></div>"
"<p class='note'>Forces relay on or off. Auto-control resumes at next controller cycle.</p></div>"
"<script>"
"async function load(){"
"try{const d=await(await fetch('/api/status')).json();"
"var tm=d.test_mode||false;"
"document.getElementById('tmbanner').style.display=tm?'block':'none';"
"var tmbtn=document.getElementById('tmbtn');"
"tmbtn.textContent=tm?'Disable Test Mode':'Enable Test Mode';"
"if(tm)tmbtn.classList.add('tm-on');else tmbtn.classList.remove('tm-on');"
"document.getElementById('cur').textContent=(d.light_brightness_pct!=null?d.light_brightness_pct:'--')+'%';"  
"if(document.getElementById('ptmp'))document.getElementById('ptmp').textContent=(d.panel_temp_c!=null?d.panel_temp_c.toFixed(1):'--');"
"var ct=d.light_colour_temp!=null?d.light_colour_temp:0;"
"var ctNames=['Full (all rows)','Med (rows 1,3,5)','Low (rows 2,4)'];"
"[0,1,2].forEach(function(i){var b=document.getElementById('ct'+i);"
"if(b)b.className='btn'+(i===ct?' on':'');});"
"if(document.getElementById('ctnote'))document.getElementById('ctnote').textContent='Active: '+ctNames[ct]+' — persisted to NVS';"
"var relNames=['Heater','Humidifier','Fan','Panel Fan','Light'];"
"let h='';d.relays.forEach(function(r,i){"
"var on_cls=r.state?' on':'',off_cls=r.state?'':' on';"
"var lbl=r.name&&r.name.length?r.name:relNames[i]||('Relay '+i);"
"h+='<div class=\"row\"><span style=\"min-width:110px;flex-shrink:0;color:#eee;font-size:.9em\">'+lbl+'</span><div class=\"btns\">';"
"h+='<button class=\"btn'+on_cls+'\" onclick=\"setR('+i+',true)\">ON</button>';"
"h+='<button class=\"btn'+off_cls+'\" onclick=\"setR('+i+',false)\">OFF</button>';"
"h+='<span style=\"font-size:.75em;color:#666;margin-left:6px\">'+(r.manual?'manual':'auto')+'</span>';"
"h+='</div></div>';});"
"document.getElementById('rels').innerHTML=h;}"
"catch(e){console.warn(e);}}"
"function updSl(v){document.getElementById('slv').textContent=v+'%';}"
"async function applyBright(){await fetch('/api/light',{method:'POST',"
"headers:{'Content-Type':'application/json'},"
"body:JSON.stringify({pct:parseInt(document.getElementById('sl').value)})});load();}""function holdBright(){"  
"var cur=parseInt(document.getElementById('cur').textContent)||0;"  
"document.getElementById('sl').value=cur;updSl(cur);applyBright();}"  "function qset(v){document.getElementById('sl').value=v;updSl(v);applyBright();}"
"async function toggleTestMode(){"
"var active=document.getElementById('tmbtn').classList.contains('tm-on');"
"await fetch('/api/testmode',{method:'POST',"
"headers:{'Content-Type':'application/json'},"
"body:JSON.stringify({enabled:!active})});load();}"
"async function setCT(mode){await fetch('/api/setpoints',{method:'POST',"
"headers:{'Content-Type':'application/json'},"
"body:JSON.stringify({light_colour_temp:mode})});load();}"
"async function setR(id,on){await fetch('/api/relay',{method:'POST',"
"headers:{'Content-Type':'application/json'},body:JSON.stringify({relay:id,on:on})});load();}"
"load();setInterval(load,5000);"
"</script></body></html>";

// ============================================================
// Embedded HTML dashboard
// ============================================================
static const char HTML_DASHBOARD[] =
"<!DOCTYPE html>"
"<html lang='en'><head>"
"<meta charset='UTF-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>Al Wall Controller</title>"
"<style>"
"*{box-sizing:border-box}"
"body{font-family:Arial,sans-serif;background:#1a1a2e;color:#eee;margin:0;padding:14px}"
"h1{color:#e94560;margin:0 0 2px;font-size:1.4em}"
".topbar{display:flex;justify-content:space-between;align-items:baseline;margin-bottom:12px}"
".topbar .right{font-size:12px;color:#888}"
".grid{display:grid;grid-template-columns:repeat(4,1fr);gap:8px;margin-bottom:12px}"
".card{background:#16213e;border-radius:10px;padding:10px;text-align:center}"
".card .val{font-size:1.8em;font-weight:bold;color:#e94560}"
".card.warn .val{color:#f90}"
".card .lbl{font-size:11px;color:#aaa;margin-top:3px}"
".devices{display:grid;grid-template-columns:repeat(2,1fr);gap:10px}"
".device{background:#16213e;border-radius:10px;padding:16px;cursor:pointer;"
         "border:2px solid #333;transition:border-color .2s,background .2s;user-select:none}"
".device:active{opacity:.85}"
".device.on{border-color:#4caf50;background:#162a1c}"
".device.warn{border-color:#f90}"
".device.man{border-style:dashed}"
".device .d-head{display:flex;justify-content:space-between;align-items:center}"
".device .d-icon{font-size:1.6em}"
".device .d-state{font-size:.85em;font-weight:bold;padding:2px 8px;border-radius:12px;background:#333}"
".device.on .d-state{background:#16a34a;color:#fff}"
".device.man .d-state{background:#d97706;color:#fff}"
".device .d-name{font-size:1em;font-weight:bold;margin:8px 0 4px}"
".device .d-sp{font-size:11px;color:#888;margin-top:-2px}"
".ovl{display:none;position:fixed;inset:0;background:rgba(0,0,0,.75);z-index:100;align-items:flex-end}"
".ovl.open{display:flex}"
".modal{background:#0f1b35;width:100%;border-radius:16px 16px 0 0;padding:18px;max-height:90vh;overflow-y:auto}"
".mhdr{display:flex;justify-content:space-between;align-items:center;margin-bottom:16px}"
".mhdr h2{margin:0;color:#e94560;font-size:1.1em}"
".mhdr .x{cursor:pointer;color:#aaa;font-size:1.4em;line-height:1;padding:0 4px}"
".sec h4{margin:0 0 8px;font-size:11px;color:#aaa;text-transform:uppercase;letter-spacing:.5px}"
".sec{margin-bottom:14px}"
".tog{display:flex;gap:8px;margin-bottom:14px}"
".tog button{flex:1;padding:11px;border-radius:8px;border:2px solid #555;"
            "background:#16213e;color:#888;cursor:pointer;font-size:14px;font-weight:bold}"
".tog button.act{border-color:#4caf50;background:#162a1c;color:#4caf50}"
".tog button.ovr{border:2px solid #555;background:#16213e;color:#ccc}"
".tog button.ovr.act{border-color:#d97706;background:#2a1a00;color:#f59e0b}"
".sp-row{display:flex;flex-wrap:wrap;gap:12px}"
".sp-item{display:flex;flex-direction:column;gap:5px}"
".sp-item label{font-size:11px;color:#aaa}"
".sp-ctrl{display:flex;align-items:center;gap:6px}"
".sp-ctrl .pm{background:#0f3460;border:1px solid #555;color:#eee;border-radius:6px;"
             "width:34px;height:38px;font-size:20px;cursor:pointer;padding:0;line-height:1}"
"input.sp{background:#0f3460;border:1px solid #555;border-radius:6px;"
          "color:#eee;padding:7px 6px;width:74px;font-size:15px;text-align:center}"
".btn-save{background:#e94560;color:#fff;border:none;border-radius:8px;"
           "padding:12px;cursor:pointer;font-size:15px;font-weight:bold;width:100%;margin-top:4px}"
".btn-save:active{background:#c73652}"
".toast{font-size:12px;text-align:center;height:18px;margin-top:6px;color:#4caf50}"
"#testbanner{display:none;background:#92400e;color:#fef3c7;text-align:center;"
             "padding:6px;font-size:13px;font-weight:bold;letter-spacing:.5px}"
".toast.err{color:#f44}"
"</style></head><body>"

"<div id='testbanner'>&#9888; TEST MODE ACTIVE &mdash; schedules suspended</div>"
"<div class='topbar'>"
"<h1>&#127807; Al Wall Controller</h1>"
"<div class='right'><a href='/charts' style='color:#888;text-decoration:none;margin-right:10px'>&#128200;</a><a href='/logs' style='color:#888;text-decoration:none;margin-right:10px'>&#x2699;</a><a href='/test' style='color:#888;text-decoration:none;margin-right:10px'>&#128295;</a><span id='devtime'>--:--</span> &bull; <span id='sdst' title='SD card'>SD:?</span> &bull; <span id='door' style='display:none;color:#f59e0b;font-weight:bold'>DOOR OPEN</span><span id='door-sep' style='display:none'> &bull; </span><span id='health'>...</span></div>"
"</div>"

"<div class='grid'>"
"<div class='card'><div class='val' id='temp'>--</div><div class='lbl'>Temp &#176;C</div></div>"
"<div class='card'><div class='val' id='hum'>--</div><div class='lbl'>Humidity %</div></div>"
"<div class='card'><div class='val' id='co2'>--</div><div class='lbl'>CO&#8322; ppm</div></div>"
"<div class='card' id='lvl-card'><div class='val' id='lvl'>OK</div><div class='lbl'>Water</div></div>"
"</div>"

"<div class='devices'>"
"<div class='device' id='d0' onclick='openModal(0)'>"
"<div class='d-head'><span class='d-icon'>&#128293;</span><span class='d-state' id='ds0'>OFF</span></div>"
"<div class='d-name'>Heater</div><div class='d-sp' id='dsp0'></div></div>"
"<div class='device' id='d1' onclick='openModal(1)'>"
"<div class='d-head'><span class='d-icon'>&#128167;</span><span class='d-state' id='ds1'>OFF</span></div>"
"<div class='d-name'>Humidifier</div><div class='d-sp' id='dsp1'></div></div>"
"<div class='device' id='d2' onclick='openModal(2)'>"
"<div class='d-head'><span class='d-icon'>&#128168;</span><span class='d-state' id='ds2'>OFF</span></div>"
"<div class='d-name'>Fan / CO&#8322;</div><div class='d-sp' id='dsp2'></div></div>"
"<div class='device' id='d3' onclick='openModal(3)'>"
"<div class='d-head'><span class='d-icon'>&#128161;</span><span class='d-state' id='ds3'>OFF</span></div>"
"<div class='d-name'>Light</div><div class='d-sp' id='dsp3'></div></div>"
"</div>"

"<div class='ovl' id='ovl' onclick='ovlClick(event)'>"
"<div class='modal'>"
"<div class='mhdr'><h2 id='mtitle'>Settings</h2><span class='x' onclick='closeModal()'>&#x2715;</span></div>"
"<div id='mbody'></div>"
"<button class='btn-save' onclick='saveModal()'>Save</button>"
"<div class='toast' id='mtoast'></div>"
"</div></div>"

"<script>"
"var D=null,mid=-1;"
"var rmap=[0,1,2,4];"
"function p2(n){return String(n).padStart(2,'0');}"

"async function fetch_s(){"
"try{"
"const r=await fetch('/api/status');"
"D=await r.json();"
"updateUI();"
"}catch(e){console.warn(e);}"
"}"

"function updateUI(){"
"if(!D)return;"
"document.getElementById('temp').textContent=D.temperature_c!=null?D.temperature_c.toFixed(1):'--';"
"document.getElementById('hum').textContent=D.humidity_pct!=null?D.humidity_pct.toFixed(1):'--';"
"document.getElementById('co2').textContent=D.co2_ppm>0?D.co2_ppm:'--';"
"document.getElementById('devtime').textContent=D.device_time||'--:--';"
"document.getElementById('health').textContent=D.sensors_healthy?'Sensors OK':'&#9888; Fault';"
"if(D.device_name){document.title=D.device_name;var h1=document.querySelector('h1');if(h1)h1.textContent=D.device_name;}"
"var dorel=document.getElementById('door'),dorsep=document.getElementById('door-sep');"
"if(dorel){var dopen=D.door_open;dorel.style.display=dopen?'inline':'none';"
"if(dorsep)dorsep.style.display=dopen?'inline':'none';}"
"var sdel=document.getElementById('sdst');if(sdel){sdel.textContent=D.sd_mounted?'SD:OK':'SD:!';"
"sdel.style.color=D.sd_mounted?'#4caf50':'#f90';"
"sdel.title='SD card'+(D.sd_mounted?'':' error: '+(D.sd_err||'?'));}"

"const lw=D.level_low;"
"document.getElementById('lvl').textContent=lw?'LOW':'OK';"
"document.getElementById('lvl-card').className='card'+(lw?' warn':'');"
"const icons=['d0','d1','d2','d3'];"
"icons.forEach(function(id,i){"
"const r=D.relays[rmap[i]];"
"const el=document.getElementById(id);"
"el.className='device'+(r.state?' on':'')+(i===1&&lw?' warn':'')+(r.manual?' man':'');"
"document.getElementById('ds'+i).textContent=(r.manual?'M ':'')+( r.state?'ON':'OFF');"
"});"
"document.getElementById('testbanner').style.display=D.test_mode?'block':'none';"
"const sp=D.setpoints;"
"const dsp=[sp.temp_setpoint.toFixed(1)+'\u00b0C \u00b1'+sp.temp_hysteresis.toFixed(1),"
"sp.hum_setpoint.toFixed(0)+'% \u00b1'+sp.hum_hysteresis.toFixed(0),"
"sp.co2_threshold+' ppm',"
"p2(sp.light_on_hour)+':'+p2(sp.light_on_min)+'-'+p2(sp.light_off_hour)+':'+p2(sp.light_off_min)];"
"dsp.forEach((s,i)=>{const e=document.getElementById('dsp'+i);if(e)e.textContent=s;});"
"}"

"function spCtrl(id,step,lbl){"
"return \"<div class='sp-item'><label>\"+lbl+\"</label><div class='sp-ctrl'>\""
"+\"<button class='pm' onclick='adj(\\\"\"+id+\"\\\",\"+(-step)+\")'>&#8722;</button>\""
"+\"<input class='sp' type='number' id='\"+id+\"' step='\"+step+\"'>\""
"+\"<button class='pm' onclick='adj(\\\"\"+id+\"\\\",\"+step+\")'>+</button>\""
"+\"</div></div>\";}"

"function modalHTML(id){"
"const sp=D.setpoints;"
"if(id===0)return \"\""
"+\"<div class='sec'><h4>Temperature</h4><div class='sp-row'>\""
"+spCtrl('sp_temp',0.5,'Target &#176;C')+spCtrl('sp_th',0.5,'Hysteresis &#176;C')"
"+\"</div></div>\";"
"if(id===1)return \"\""
"+\"<div class='sec'><h4>Humidity</h4><div class='sp-row'>\""
"+spCtrl('sp_hum',1,'Target %RH')+spCtrl('sp_hh',0.5,'Hysteresis %')"
"+\"</div></div>\";"
"if(id===2)return \"\""
"+\"<div class='sec'><h4>CO&#8322; Override</h4><div class='sp-row'>\""
"+spCtrl('sp_co2',50,'On above (ppm)')+spCtrl('sp_co2h',50,'Off hysteresis')"
"+\"</div></div>\""
"+\"<div class='sec'><h4>Timed Schedule</h4><div class='sp-row'>\""
"+spCtrl('sp_fan_on',1,'On for (min)')+spCtrl('sp_fan_per',5,'Every (min)')"
"+\"</div>\""
"+\"<div class='tog'><label style='margin-right:8px'>Schedule</label>\""
"+\"<input type='checkbox' id='sp_fan_en'></div>\""
"+\"</div>\";"
"if(id===3){"
"return \"\""
"+\"<div class='sec'><h4>Schedule</h4><div class='sp-row'>\""
"+\"<div class='sp-item'><label>ON time</label><input class='sp' type='text' id='sp_lon' placeholder='HH:MM' maxlength='5'></div>\""
"+\"<div class='sp-item'><label>OFF time</label><input class='sp' type='text' id='sp_loff' placeholder='HH:MM' maxlength='5'></div>\""
"+\"</div></div>\""
"+\"<div class='sec'><h4>Ramp (min, 0&nbsp;=&nbsp;instant)</h4><div class='sp-row'>\""
"+spCtrl('sp_rise',5,'Sunrise (min)')+spCtrl('sp_set_m',5,'Sunset (min)')"
"+\"</div></div>\""
"+\"<div class='sec'><h4>Colour Temperature</h4>\""
"+\"<div class='tog'>\""
"+\"<button id='ctb0' class='ovr' onclick='selCT(0)'>&#10052; Full</button>\""
"+\"<button id='ctb1' class='ovr' onclick='selCT(1)'>&#9728; Neutral</button>\""
"+\"<button id='ctb2' class='ovr' onclick='selCT(2)'>&#127844; Warm</button>\""
"+\"</div></div>\";}"
"return '';}"

"function populateModal(id){"
"const sp=D.setpoints;"
"if(id===0){document.getElementById('sp_temp').value=sp.temp_setpoint;"
"document.getElementById('sp_th').value=sp.temp_hysteresis;}"
"else if(id===1){document.getElementById('sp_hum').value=sp.hum_setpoint;"
"document.getElementById('sp_hh').value=sp.hum_hysteresis;}"
"else if(id===2){document.getElementById('sp_co2').value=sp.co2_threshold;"
"document.getElementById('sp_co2h').value=sp.co2_hysteresis;"
"document.getElementById('sp_fan_on').value=sp.fan_sched_on_min;"
"document.getElementById('sp_fan_per').value=sp.fan_sched_period_min;"
"document.getElementById('sp_fan_en').checked=sp.fan_sched_enabled;}"
"else if(id===3){document.getElementById('sp_lon').value=p2(sp.light_on_hour)+':'+p2(sp.light_on_min);"
"document.getElementById('sp_loff').value=p2(sp.light_off_hour)+':'+p2(sp.light_off_min);"
"document.getElementById('sp_rise').value=sp.light_sunrise_min!=null?sp.light_sunrise_min:0;"
"document.getElementById('sp_set_m').value=sp.light_sunset_min!=null?sp.light_sunset_min:0;"
"selCT(sp.light_colour_temp!=null?sp.light_colour_temp:0);}}"

"function openModal(id){"
"if(!D)return;"
"mid=id;"
"const titles=['Heater','Humidifier','Fan / CO\u20822','Light'];"
"document.getElementById('mtitle').textContent=titles[id];"
"document.getElementById('mbody').innerHTML=modalHTML(id);"
"document.getElementById('mtoast').textContent='';"
"populateModal(id);"
"document.getElementById('ovl').classList.add('open');}"

"function closeModal(){"
"document.getElementById('ovl').classList.remove('open');"
"mid=-1;}"

"function ovlClick(e){if(e.target===document.getElementById('ovl'))closeModal();}"

"function adj(id,d){"
"const el=document.getElementById(id);"
"el.value=Math.round((parseFloat(el.value||0)+d)*100)/100;}"

"function selCT(n){"
"[0,1,2].forEach(function(i){var b=document.getElementById('ctb'+i);"
"if(b)b.className='ovr'+(i===n?' act':'');});}"

"async function saveModal(){"
"if(!D||mid<0)return;"
"const sp=Object.assign({},D.setpoints);"
"const id=mid;"
"if(id===0){sp.temp_setpoint=parseFloat(document.getElementById('sp_temp').value||sp.temp_setpoint);"
"sp.temp_hysteresis=parseFloat(document.getElementById('sp_th').value||sp.temp_hysteresis);}"
"else if(id===1){sp.hum_setpoint=parseFloat(document.getElementById('sp_hum').value||sp.hum_setpoint);"
"sp.hum_hysteresis=parseFloat(document.getElementById('sp_hh').value||sp.hum_hysteresis);}"
"else if(id===2){sp.co2_threshold=parseInt(document.getElementById('sp_co2').value||sp.co2_threshold);"
"sp.co2_hysteresis=parseInt(document.getElementById('sp_co2h').value||sp.co2_hysteresis);"
"sp.fan_sched_on_min=parseInt(document.getElementById('sp_fan_on').value||sp.fan_sched_on_min);"
"sp.fan_sched_period_min=parseInt(document.getElementById('sp_fan_per').value||sp.fan_sched_period_min);"
"sp.fan_sched_enabled=document.getElementById('sp_fan_en').checked;}"
"else if(id===3){const lon=document.getElementById('sp_lon').value.split(':');"
"const loff=document.getElementById('sp_loff').value.split(':');"
"sp.light_on_hour=parseInt(lon[0])||0;sp.light_on_min=parseInt(lon[1])||0;"
"sp.light_off_hour=parseInt(loff[0])||0;sp.light_off_min=parseInt(loff[1])||0;"
"sp.light_sunrise_min=parseInt(document.getElementById('sp_rise').value)||0;"
"sp.light_sunset_min=parseInt(document.getElementById('sp_set_m').value)||0;"
"var ctact=[0,1,2].findIndex(function(i){var b=document.getElementById('ctb'+i);return b&&b.classList.contains('act');});"
"if(ctact>=0)sp.light_colour_temp=ctact;}"
"const r=await fetch('/api/setpoints',{method:'POST',"
"headers:{'Content-Type':'application/json'},body:JSON.stringify(sp)});"
"const t=document.getElementById('mtoast');"
"if(r.ok){closeModal();await fetch_s();}"
"else{t.textContent='Error '+r.status;t.className='toast err';}}"

"fetch_s();"
"setInterval(fetch_s,10000);"
"</script></body></html>";

// ============================================================
// Embedded HTML — /logs  settings + log viewer page
// ============================================================
static const char HTML_LOGS[] =
"<!DOCTYPE html><html lang='en'><head>"
"<meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>Settings &amp; Logs &mdash; Al Wall Controller</title>"
"<style>"
"*{box-sizing:border-box}"
"body{font-family:Arial,sans-serif;background:#1a1a2e;color:#eee;margin:0;padding:14px}"
"h1{color:#e94560;margin:0 0 12px;font-size:1.4em}"
".card{background:#16213e;border-radius:10px;padding:14px;margin-bottom:12px}"
"h3{color:#aaa;font-size:.85em;text-transform:uppercase;letter-spacing:.5px;margin:0 0 12px}"
".sp-row{display:flex;flex-wrap:wrap;gap:12px;margin-bottom:14px}"
".sp-item{display:flex;flex-direction:column;gap:5px}"
".sp-item label{font-size:11px;color:#aaa}"
".sp-ctrl{display:flex;align-items:center;gap:6px}"
".sp-ctrl .pm{background:#0f3460;border:1px solid #555;color:#eee;border-radius:6px;"
"             width:34px;height:38px;font-size:20px;cursor:pointer;padding:0;line-height:1}"
"input.sp{background:#0f3460;border:1px solid #555;border-radius:6px;"
"         color:#eee;padding:7px 6px;width:74px;font-size:15px;text-align:center}"
".btn-save{background:#e94560;color:#fff;border:none;border-radius:8px;"
"          padding:12px;cursor:pointer;font-size:15px;font-weight:bold;width:100%;margin-top:4px}"
".btn-save:active{background:#c73652}"
".toast{font-size:12px;text-align:center;height:18px;margin-top:6px;color:#4caf50}"
".toast.err{color:#f44}"
".back{display:inline-block;color:#aaa;text-decoration:none;margin-bottom:14px;font-size:.9em}"
".placeholder{color:#555;font-style:italic;text-align:center;padding:24px 0}"
"</style></head><body>"
"<a class='back' href='/'>&#8592; Dashboard</a>"
"<h1>&#x2699; Settings &amp; Logs</h1>"
"<div class='card'>"
"<h3>Cabinet Fan</h3>"
"<div style='margin-bottom:12px;font-size:.9em'>Current panel temp: "
"<span id='ptmp_s' style='font-weight:bold;color:#e94560'>--</span> &deg;C</div>"
"<div class='sp-row'>"
"<div class='sp-item'><label>Fan ON above (&deg;C)</label>"
"<div class='sp-ctrl'>"
"<button class='pm' onclick='adj(\"sp_pfan_max\",-1)'>&#8722;</button>"
"<input class='sp' type='number' id='sp_pfan_max' step='1'>"
"<button class='pm' onclick='adj(\"sp_pfan_max\",1)'>+</button>"
"</div></div>"
"<div class='sp-item'><label>Hysteresis (&deg;C)</label>"
"<div class='sp-ctrl'>"
"<button class='pm' onclick='adj(\"sp_pfan_hyst\",-1)'>&#8722;</button>"
"<input class='sp' type='number' id='sp_pfan_hyst' step='1'>"
"<button class='pm' onclick='adj(\"sp_pfan_hyst\",1)'>+</button>"
"</div></div>"
"</div>"
"<button class='btn-save' onclick='save()'>Save</button>"
"<div class='toast' id='toast'></div>"
"</div>"
"<div class='card'>"  
"<h3>Recent Readings</h3>"
"<div id='log_status' style='font-size:11px;color:#888;margin-bottom:6px'>Checking&#8230;</div>"
"<div style='overflow-x:auto'>"
"<table id='ltbl' style='width:100%;border-collapse:collapse;font-size:12px'>"
"<thead><tr style='color:#aaa'>"
"<th style='text-align:left;padding:4px 6px;white-space:nowrap'>Time</th>"
"<th style='padding:4px'>&#176;C</th>"
"<th style='padding:4px'>%RH</th>"
"<th style='padding:4px'>CO&#8322;</th>"
"<th style='padding:4px'>Heat</th>"
"<th style='padding:4px'>Hum</th>"
"<th style='padding:4px'>Fan</th>"
"<th style='padding:4px'>Light</th>"
"<th style='padding:4px'>Panel&#176;C</th>"
"</tr></thead>"
"<tbody id='ltbody'><tr><td colspan='9' class='placeholder'>Loading&#8230;</td></tr></tbody>"
"</table></div>"
"<button class='btn-save' style='margin-top:10px' onclick='loadLogs()'>&#8635; Refresh</button>"
"</div>"
"<div class='card'>"
"<h3>Log Files</h3>"
"<div id='flist' class='placeholder'>Loading&#8230;</div>"
"<div style='margin-top:10px;display:flex;align-items:center;gap:10px;flex-wrap:wrap'>"
"<button class='btn-save' style='width:auto;padding:10px 18px' onclick='remountSd()'>&#128190; Remount SD</button>"
"<span id='sd_remount_status' style='font-size:12px;color:#888'></span>"
"</div>"
"</div>"

// WiFi & Device card
"<div class='card'>"
"<h3>WiFi &amp; Device</h3>"
"<div style='margin-bottom:10px;font-size:.85em' id='w_status'>Loading&#8230;</div>"
"<div class='sp-row'>"
"<div class='sp-item'><label>Device Name</label>"
"<input class='sp' type='text' id='w_dname' maxlength='32' style='width:200px'></div>"
"<div class='sp-item'><label>Hostname</label>"
"<input class='sp' type='text' id='w_host' maxlength='32' style='width:180px'></div>"
"</div>"
"<div style='font-size:10px;color:#666;margin:-8px 0 10px'>hostname sets mDNS &amp; http://&lt;hostname&gt;.local</div>"
"<div class='sp-row'>"
"<div class='sp-item'><label>Primary SSID</label>"
"<input class='sp' type='text' id='w_ssid' maxlength='32' style='width:200px'></div>"
"<div class='sp-item'><label>Primary Password</label>"
"<input class='sp' type='password' id='w_pass' maxlength='64' style='width:180px' placeholder='(unchanged)'></div>"
"</div>"
"<div class='sp-row'>"
"<div class='sp-item'><label>Fallback SSID (optional)</label>"
"<input class='sp' type='text' id='w_ssid2' maxlength='32' style='width:200px'></div>"
"<div class='sp-item'><label>Fallback Password</label>"
"<input class='sp' type='password' id='w_pass2' maxlength='64' style='width:180px' placeholder='(unchanged)'></div>"
"</div>"
"<div class='sp-row'>"
"<div class='sp-item'><label>Timezone</label>"
"<select id='w_tz' style='background:#0f3460;border:1px solid #555;border-radius:6px;"
"color:#eee;padding:8px;font-size:14px'>"
"<option value='GMT0BST,M3.5.0/1,M10.5.0'>UK (GMT/BST)</option>"
"<option value='CET-1CEST,M3.5.0,M10.5.0/3'>Central Europe (CET/CEST)</option>"
"<option value='EET-2EEST,M3.5.0/3,M10.5.0/4'>Eastern Europe (EET/EEST)</option>"
"<option value='EST5EDT,M3.2.0,M11.1.0'>US Eastern (EST/EDT)</option>"
"<option value='CST6CDT,M3.2.0,M11.1.0'>US Central (CST/CDT)</option>"
"<option value='PST8PDT,M3.2.0,M11.1.0'>US Pacific (PST/PDT)</option>"
"<option value='AEST-10AEDT,M10.1.0,M4.1.0/3'>Australia Eastern (AEST/AEDT)</option>"
"<option value='UTC0'>UTC</option>"
"</select></div>"
"</div>"
"<button class='btn-save' onclick='saveWifi()'>Save &mdash; device will reboot</button>"
"<div class='toast' id='w_toast'></div>"
"</div>"

// Notifications card
"<div class='card'>"
"<h3>Push Notifications (ntfy.sh)</h3>"
"<p style='font-size:.85em;color:#aaa;margin:0 0 10px'>"
"Enter your <a href='https://ntfy.sh' style='color:#4caf50'>ntfy.sh</a> topic name to receive alerts on your phone. "
"Install the free ntfy app and subscribe to the same topic.</p>"
"<div class='sp-row'>"
"<div class='sp-item'><label>Topic name</label>"
"<input class='sp' type='text' id='n_topic' maxlength='63' style='width:220px' placeholder='e.g. my-growroom-abc'></div>"
"</div>"
"<div style='display:flex;gap:8px;flex-wrap:wrap;align-items:center'>"
"<button class='btn-save' onclick='saveNotifier()'>Save</button>"
"<button class='btn-save' style='background:#1565c0' onclick='testNotifier()'>Send Test</button>"
"</div>"
"<div class='toast' id='n_toast'></div>"
"</div>"

// Alert Conditions card
"<div class='card'>"
"<h3>Alert Conditions</h3>"
"<p style='font-size:.85em;color:#aaa;margin:0 0 10px'>ntfy topic must be configured above. "
"Repeat intervals are in minutes except Door (seconds).</p>"

"<h4 style='color:#e94560;margin:6px 0 6px;font-size:.8em;text-transform:uppercase'>Temperature</h4>"
"<div class='sp-row'>"
"<div class='sp-item'><label>Enabled</label><input type='checkbox' id='al_t_en'></div>"
"<div class='sp-item'><label>Low (&deg;C)</label><input class='sp' type='number' id='al_t_lo' step='0.5' style='width:70px'></div>"
"<div class='sp-item'><label>High (&deg;C)</label><input class='sp' type='number' id='al_t_hi' step='0.5' style='width:70px'></div>"
"<div class='sp-item'><label>Repeat (min)</label><input class='sp' type='number' id='al_t_rep' min='1' style='width:70px'></div>"
"</div>"

"<h4 style='color:#e94560;margin:6px 0 6px;font-size:.8em;text-transform:uppercase'>Humidity</h4>"
"<div class='sp-row'>"
"<div class='sp-item'><label>Enabled</label><input type='checkbox' id='al_h_en'></div>"
"<div class='sp-item'><label>Low (%)</label><input class='sp' type='number' id='al_h_lo' step='1' style='width:70px'></div>"
"<div class='sp-item'><label>High (%)</label><input class='sp' type='number' id='al_h_hi' step='1' style='width:70px'></div>"
"<div class='sp-item'><label>Repeat (min)</label><input class='sp' type='number' id='al_h_rep' min='1' style='width:70px'></div>"
"</div>"

"<h4 style='color:#e94560;margin:6px 0 6px;font-size:.8em;text-transform:uppercase'>CO&#8322;</h4>"
"<div class='sp-row'>"
"<div class='sp-item'><label>Enabled</label><input type='checkbox' id='al_c_en'></div>"
"<div class='sp-item'><label>Low (ppm)</label><input class='sp' type='number' id='al_c_lo' step='50' style='width:80px'></div>"
"<div class='sp-item'><label>High (ppm)</label><input class='sp' type='number' id='al_c_hi' step='50' style='width:80px'></div>"
"<div class='sp-item'><label>Repeat (min)</label><input class='sp' type='number' id='al_c_rep' min='1' style='width:70px'></div>"
"</div>"

"<h4 style='color:#e94560;margin:6px 0 6px;font-size:.8em;text-transform:uppercase'>Water Level</h4>"
"<div class='sp-row'>"
"<div class='sp-item'><label>Enabled</label><input type='checkbox' id='al_w_en'></div>"
"<div class='sp-item'><label>Repeat (min)</label><input class='sp' type='number' id='al_w_rep' min='1' style='width:80px'></div>"
"</div>"

"<h4 style='color:#e94560;margin:6px 0 6px;font-size:.8em;text-transform:uppercase'>Door Open</h4>"
"<div class='sp-row'>"
"<div class='sp-item'><label>Enabled</label><input type='checkbox' id='al_d_en'></div>"
"<div class='sp-item'><label>Alert after (s)</label><input class='sp' type='number' id='al_d_dur' min='5' style='width:80px'></div>"
"<div class='sp-item'><label>Repeat (s)</label><input class='sp' type='number' id='al_d_rep' min='10' style='width:80px'></div>"
"</div>"

"<h4 style='color:#e94560;margin:6px 0 6px;font-size:.8em;text-transform:uppercase'>SD Card Failure</h4>"
"<div class='sp-row'>"
"<div class='sp-item'><label>Enabled</label><input type='checkbox' id='al_sd_en'></div>"
"<div class='sp-item'><label>Repeat (min)</label><input class='sp' type='number' id='al_sd_rep' min='1' style='width:80px'></div>"
"</div>"

"<h4 style='color:#e94560;margin:6px 0 6px;font-size:.8em;text-transform:uppercase'>WiFi Failure</h4>"
"<div class='sp-row'>"
"<div class='sp-item'><label>Enabled</label><input type='checkbox' id='al_wf_en'></div>"
"<div class='sp-item'><label>Repeat (min)</label><input class='sp' type='number' id='al_wf_rep' min='1' style='width:80px'></div>"
"</div>"

"<h4 style='color:#e94560;margin:6px 0 6px;font-size:.8em;text-transform:uppercase'>Equipment Failure</h4>"
"<p style='font-size:.82em;color:#888;margin:0 0 8px'>Alert when a relay has been ON but the environment has not improved after the set time.</p>"
"<div style='font-size:.8em;color:#ccc;margin:4px 0 2px'>Heater (can&apos;t reach temp setpoint)</div>"
"<div class='sp-row'>"
"<div class='sp-item'><label>Enabled</label><input type='checkbox' id='al_tf_en'></div>"
"<div class='sp-item'><label>Alert after (min)</label><input class='sp' type='number' id='al_tf_min' min='5' style='width:80px'></div>"
"<div class='sp-item'><label>Repeat (min)</label><input class='sp' type='number' id='al_tf_rep' min='1' style='width:70px'></div>"
"</div>"
"<div style='font-size:.8em;color:#ccc;margin:4px 0 2px'>Humidifier (can&apos;t reach humidity setpoint)</div>"
"<div class='sp-row'>"
"<div class='sp-item'><label>Enabled</label><input type='checkbox' id='al_hf_en'></div>"
"<div class='sp-item'><label>Alert after (min)</label><input class='sp' type='number' id='al_hf_min' min='5' style='width:80px'></div>"
"<div class='sp-item'><label>Repeat (min)</label><input class='sp' type='number' id='al_hf_rep' min='1' style='width:70px'></div>"
"</div>"
"<div style='font-size:.8em;color:#ccc;margin:4px 0 2px'>Fan (CO&#8322; not clearing)</div>"
"<div class='sp-row'>"
"<div class='sp-item'><label>Enabled</label><input type='checkbox' id='al_cf_en'></div>"
"<div class='sp-item'><label>Alert after (min)</label><input class='sp' type='number' id='al_cf_min' min='5' style='width:80px'></div>"
"<div class='sp-item'><label>Repeat (min)</label><input class='sp' type='number' id='al_cf_rep' min='1' style='width:70px'></div>"
"</div>"

"<h4 style='color:#e94560;margin:6px 0 6px;font-size:.8em;text-transform:uppercase'>Power Fault / Reboot</h4>"
"<div class='sp-row'>"
"<div class='sp-item'><label>Notify on restart</label><input type='checkbox' id='al_rb_en'></div>"
"</div>"

"<button class='btn-save' onclick='saveAlerts()'>Save Alerts</button>"
"<div class='toast' id='al_toast'></div>"
"</div>"

// Factory Reset card
"<div class='card' style='border:2px solid #7f1d1d'>"
"<h3 style='color:#f87171'>Factory Reset</h3>"
"<p style='font-size:.85em;color:#aaa;margin:0 0 12px'>Erases ALL settings (WiFi credentials, setpoints) and reboots. "
"The device will start in setup AP mode &mdash; connect to <strong>thlcontroller</strong> with password: setup1234 and browse to <strong>192.168.4.1</strong>.</p>"
"<button class='btn-save' style='background:#7f1d1d' onclick='factoryReset()'>Factory Reset</button>"
"<div class='toast' id='fr_toast'></div>"
"</div>"

"<script>"
"var D=null;"
"async function load(){"
"try{"
"const r=await fetch('/api/status');"
"D=await r.json();"
"const sp=D.setpoints;"
"document.getElementById('sp_pfan_max').value=sp.panel_temp_max_c;"
"document.getElementById('sp_pfan_hyst').value=sp.panel_temp_hyst_c;"
"if(D.panel_temp_c!=null)document.getElementById('ptmp_s').textContent=D.panel_temp_c.toFixed(1);"
"var sdCol=D.sd_mounted?'#4caf50':'#f87171';"
"var ntpOk=D.device_time&&D.device_time!='00:00';"
"var lastT=D.last_log_t&&D.last_log_t>1000?new Date(D.last_log_t*1000).toLocaleTimeString():'none';"
"document.getElementById('log_status').innerHTML="
"'SD: <span style=\"color:'+sdCol+'\">'+(D.sd_mounted?'mounted':'UNMOUNTED &mdash; '+D.sd_err)+'</span>'"
"+'&nbsp;&nbsp;Ring: '+(D.ring_count||0)+' entries'"
"+'&nbsp;&nbsp;NTP: <span style=\"color:'+(ntpOk?'#4caf50':'#f87171')+'\">'+(ntpOk?D.device_time:'NOT SYNCED')+'</span>'"
"+'&nbsp;&nbsp;Last log: '+(D.last_log_t>1000?lastT:'&mdash;')"
"+'&nbsp;&nbsp;Ctrl: <span style=\"color:'+(D.ctrl_cycles>0?'#4caf50':'#f87171')+'\">'+(D.ctrl_cycles||0)+' cycles</span>'"
"+(D.door_pin_raw!==undefined?'&nbsp;&nbsp;GPB0: <span style=\"color:'+(D.door_pin_raw?'#4caf50':'#f59e0b')+'\">'+(D.door_pin_raw?'HIGH':'LOW')+'</span>':'');"
"}catch(e){"
"document.getElementById('log_status').textContent='Status check failed: '+e.message;"
"}}"
"function adj(id,d){"
"const el=document.getElementById(id);"
"el.value=Math.round((parseFloat(el.value||0)+d)*100)/100;}"
"async function save(){"
"if(!D)return;"
"const sp=Object.assign({},D.setpoints);"
"sp.panel_temp_max_c=parseFloat(document.getElementById('sp_pfan_max').value||sp.panel_temp_max_c);"
"sp.panel_temp_hyst_c=parseFloat(document.getElementById('sp_pfan_hyst').value||sp.panel_temp_hyst_c);"
"const r=await fetch('/api/setpoints',{method:'POST',"
"headers:{'Content-Type':'application/json'},body:JSON.stringify(sp)});"
"const t=document.getElementById('toast');"
"if(r.ok){t.textContent='Saved';t.className='toast';setTimeout(function(){t.textContent='';},2000);}"
"else{t.textContent='Error '+r.status;t.className='toast err';}}"
"async function loadLogs(){"
"try{"
"const r=await fetch('/api/log/recent');"
"const rows=await r.json();"
"const tb=document.getElementById('ltbody');"
"if(!rows.length){tb.innerHTML=\"<tr><td colspan='9' class='placeholder'>No entries yet</td></tr>\";return;}"
"var dot=function(v){return v?'\u25cf':'\u25cb';};"
"var clr=function(v){return v?'#4caf50':'#555';};"
"var h='';"
"for(var i=rows.length-1;i>=0;i--){"
"var e=rows[i];"
"h+=\"<tr style='border-top:1px solid #0f0f1a'>\";"  
"h+=\"<td style='padding:3px 6px;font-size:11px;white-space:nowrap'>\"+e.ts.replace('T',' ')+\"</td>\";"
"h+=\"<td style='text-align:center'>\"+e.temp.toFixed(1)+\"</td>\";"
"h+=\"<td style='text-align:center'>\"+e.hum.toFixed(1)+\"</td>\";"
"h+=\"<td style='text-align:center'>\"+e.co2+\"</td>\";"
"h+=\"<td style='text-align:center;color:\"+clr(e.heater)+\"'>\"+dot(e.heater)+\"</td>\";"
"h+=\"<td style='text-align:center;color:\"+clr(e.humidifier)+\"'>\"+dot(e.humidifier)+\"</td>\";"
"h+=\"<td style='text-align:center;color:\"+clr(e.fan)+\"'>\"+dot(e.fan)+\"</td>\";"
"h+=\"<td style='text-align:center;color:\"+clr(e.light)+\"'>\"+dot(e.light)+\"</td>\";"
"h+=\"<td style='text-align:center'>\"+e.ptmp.toFixed(1)+\"</td>\";"
"h+='</tr>';"
"}"
"tb.innerHTML=h;"
"}catch(ex){"
"const tb2=document.getElementById('ltbody');"
"if(tb2)tb2.innerHTML=\"<tr><td colspan='9' style='color:#f87171;padding:6px'>Error loading data: \"+ex.message+\"</td></tr>\";"
"}}"
"async function loadFiles(){"
"try{"
"const r=await fetch('/api/log/files');"
"const files=await r.json();"
"const el=document.getElementById('flist');"
"if(!files.length){el.innerHTML=\"<span style='color:#555;font-style:italic'>No log files on SD card</span>\";return;}"
"var h='';"
"files.slice().reverse().forEach(function(f){"
"h+=\"<div style='margin-bottom:6px'>\";"
"h+=\"<a href='/api/log/file?name=\"+f.name+\"' style='color:#4caf50'>\"+f.name+\"</a>\";"
"h+=\" <span style='color:#888;font-size:11px'>\"+(f.size/1024).toFixed(1)+\" KB</span>\";"
"h+='</div>';"
"});"
"el.innerHTML=h;"
"}catch(ex){"
"const el2=document.getElementById('flist');"
"if(el2)el2.innerHTML=\"<span style='color:#f87171'>Error: \"+ex.message+\"</span>\";"
"}}"
"async function remountSd(){"
"var el=document.getElementById('sd_remount_status');"
"el.textContent='Remounting\u2026';el.style.color='#888';"
"try{"
"const r=await fetch('/api/remount_sd',{method:'POST'});"
"const j=await r.json();"
"el.textContent=j.ok?'Mounted OK':('Failed: '+(j.err||'?'));"
"el.style.color=j.ok?'#4caf50':'#f44';"
"if(j.ok){await loadLogs();await loadFiles();}"
"}catch(e){el.textContent='Error';el.style.color='#f44';}}"
"(async function(){"
"  await load();"
"  await loadLogs();"
"  await loadFiles();"
"  await loadWifi();"
"  await loadNotifier();"
"  await loadAlerts();"
"  setInterval(load,10000);"
"  setInterval(function(){loadLogs();loadFiles();},60000);"
"})();"

// ---- WiFi & Device card scripts appended to HTML_LOGS ----
"async function loadWifi(){"
"try{"
"if(D){document.getElementById('w_dname').value=D.device_name||'';"
"document.getElementById('w_host').value=D.hostname||'';}"
"const r2=await fetch('/api/wifi');const W=await r2.json();"
"document.getElementById('w_ssid').value=W.ssid||'';"
"document.getElementById('w_ssid2').value=W.ssid2||'';"
"if(D){"
"var modes=['Connected (primary)','Connected (fallback)','Setup AP (offline)','Offline'];"
"var mode=D.wifi_mode!=null?D.wifi_mode:3;"
"var sc=mode===0||mode===1?'#4caf50':mode===2?'#f59e0b':'#f44';"
"var stext=modes[mode]+(D.ip_address&&D.ip_address!='0.0.0.0'?' \u00b7 '+D.ip_address:'');"
"document.getElementById('w_status').innerHTML='<span style=\"color:'+sc+'\">'+stext+'</span>';}"
"var tzSel=document.getElementById('w_tz');"
"if(tzSel&&W.timezone){for(var i=0;i<tzSel.options.length;i++){if(tzSel.options[i].value===W.timezone){tzSel.selectedIndex=i;break;}}}"
"}catch(e){console.warn(e);}}"
"async function saveWifi(){"
"var payload={ssid:document.getElementById('w_ssid').value,"
"password:document.getElementById('w_pass').value,"
"ssid2:document.getElementById('w_ssid2').value,"
"password2:document.getElementById('w_pass2').value,"
"hostname:document.getElementById('w_host').value,"
"device_name:document.getElementById('w_dname').value,"
"timezone:document.getElementById('w_tz').value};"
"var r=await fetch('/api/wifi',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(payload)});"
"var t=document.getElementById('w_toast');"
"if(r.ok){t.textContent='Saved \u2014 rebooting\u2026';t.className='toast';"
"setTimeout(function(){location.reload();},6000);}"
"else{t.textContent='Error '+r.status;t.className='toast err';}}"
"async function factoryReset(){"
"if(!confirm('Erase ALL settings and reboot? The device will enter WiFi setup mode.'))return;"
"await fetch('/api/factory_reset',{method:'POST'});"
"document.getElementById('fr_toast').textContent='Resetting\u2026 reconnect to thlcontroller';"
"}"
"loadWifi();"
"async function loadNotifier(){"
"try{const r=await fetch('/api/notifier');const j=await r.json();"
"document.getElementById('n_topic').value=j.topic||'';}"
"catch(e){console.warn(e);}}"
"async function saveNotifier(){"
"var t=document.getElementById('n_toast');"
"var topic=document.getElementById('n_topic').value.trim();"
"var r=await fetch('/api/notifier',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({topic:topic})});"
"if(r.ok){t.textContent='Saved';t.className='toast';setTimeout(function(){t.textContent='';},2000);}"
"else{t.textContent='Error '+r.status;t.className='toast err';}}"
"async function testNotifier(){"
"var t=document.getElementById('n_toast');"
"t.textContent='Sending…';t.className='toast';"
"var r=await fetch('/api/notifier/test',{method:'POST'});"
"if(r.ok){t.textContent='Queued \u2014 check ntfy app';setTimeout(function(){t.textContent='';},4000);}"
"else{t.textContent='Error '+r.status;t.className='toast err';}}"
"async function loadAlerts(){"
"try{const r=await fetch('/api/alerts');const j=await r.json();"
"document.getElementById('al_t_en').checked=j.temp.en;"
"document.getElementById('al_t_lo').value=j.temp.lo;"
"document.getElementById('al_t_hi').value=j.temp.hi;"
"document.getElementById('al_t_rep').value=Math.round(j.temp.rep/60);"
"document.getElementById('al_h_en').checked=j.hum.en;"
"document.getElementById('al_h_lo').value=j.hum.lo;"
"document.getElementById('al_h_hi').value=j.hum.hi;"
"document.getElementById('al_h_rep').value=Math.round(j.hum.rep/60);"
"document.getElementById('al_c_en').checked=j.co2.en;"
"document.getElementById('al_c_lo').value=j.co2.lo;"
"document.getElementById('al_c_hi').value=j.co2.hi;"
"document.getElementById('al_c_rep').value=Math.round(j.co2.rep/60);"
"document.getElementById('al_w_en').checked=j.water.en;"
"document.getElementById('al_w_rep').value=Math.round(j.water.rep/60);"
"document.getElementById('al_d_en').checked=j.door.en;"
"document.getElementById('al_d_dur').value=j.door.dur;"
"document.getElementById('al_d_rep').value=j.door.rep;"
"document.getElementById('al_sd_en').checked=j.sd.en;"
"document.getElementById('al_sd_rep').value=Math.round(j.sd.rep/60);"
"document.getElementById('al_wf_en').checked=j.wifi.en;"
"document.getElementById('al_wf_rep').value=Math.round(j.wifi.rep/60);"
"document.getElementById('al_rb_en').checked=j.reboot.en;"
"document.getElementById('al_tf_en').checked=j.temp.fail_en;"
"document.getElementById('al_tf_min').value=j.temp.fail_min;"
"document.getElementById('al_tf_rep').value=Math.round(j.temp.fail_rep/60);"
"document.getElementById('al_hf_en').checked=j.hum.fail_en;"
"document.getElementById('al_hf_min').value=j.hum.fail_min;"
"document.getElementById('al_hf_rep').value=Math.round(j.hum.fail_rep/60);"
"document.getElementById('al_cf_en').checked=j.co2.fail_en;"
"document.getElementById('al_cf_min').value=j.co2.fail_min;"
"document.getElementById('al_cf_rep').value=Math.round(j.co2.fail_rep/60);"
"}catch(e){console.warn(e);}}"
"async function saveAlerts(){"
"var t=document.getElementById('al_toast');"
"var p={"
"temp:{en:document.getElementById('al_t_en').checked,"
"lo:parseFloat(document.getElementById('al_t_lo').value),"
"hi:parseFloat(document.getElementById('al_t_hi').value),"
"rep:parseInt(document.getElementById('al_t_rep').value)*60,"
"fail_en:document.getElementById('al_tf_en').checked,"
"fail_min:parseInt(document.getElementById('al_tf_min').value),"
"fail_rep:parseInt(document.getElementById('al_tf_rep').value)*60},"
"hum:{en:document.getElementById('al_h_en').checked,"
"lo:parseFloat(document.getElementById('al_h_lo').value),"
"hi:parseFloat(document.getElementById('al_h_hi').value),"
"rep:parseInt(document.getElementById('al_h_rep').value)*60,"
"fail_en:document.getElementById('al_hf_en').checked,"
"fail_min:parseInt(document.getElementById('al_hf_min').value),"
"fail_rep:parseInt(document.getElementById('al_hf_rep').value)*60},"
"co2:{en:document.getElementById('al_c_en').checked,"
"lo:parseInt(document.getElementById('al_c_lo').value),"
"hi:parseInt(document.getElementById('al_c_hi').value),"
"rep:parseInt(document.getElementById('al_c_rep').value)*60,"
"fail_en:document.getElementById('al_cf_en').checked,"
"fail_min:parseInt(document.getElementById('al_cf_min').value),"
"fail_rep:parseInt(document.getElementById('al_cf_rep').value)*60},"
"water:{en:document.getElementById('al_w_en').checked,"
"rep:parseInt(document.getElementById('al_w_rep').value)*60},"
"door:{en:document.getElementById('al_d_en').checked,"
"dur:parseInt(document.getElementById('al_d_dur').value),"
"rep:parseInt(document.getElementById('al_d_rep').value)},"
"sd:{en:document.getElementById('al_sd_en').checked,"
"rep:parseInt(document.getElementById('al_sd_rep').value)*60},"
"wifi:{en:document.getElementById('al_wf_en').checked,"
"rep:parseInt(document.getElementById('al_wf_rep').value)*60},"
"reboot:{en:document.getElementById('al_rb_en').checked}};"
"var r=await fetch('/api/alerts',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(p)});"
"if(r.ok){t.textContent='Saved';t.className='toast';setTimeout(function(){t.textContent='';},2000);}"
"else{t.textContent='Error '+r.status;t.className='toast err';}}"
"</script></body></html>";

// ============================================================
// Helper: send a const HTML string in 2 KB chunks so a single
// large send() call never stalls on the ~5 KB TCP send buffer.
// ============================================================
static esp_err_t resp_html_chunked(httpd_req_t *req, const char *html, size_t len)
{
    const size_t CHUNK = 2048;
    const char  *p = html;
    while (len > 0) {
        size_t n = (len < CHUNK) ? len : CHUNK;
        if (httpd_resp_send_chunk(req, p, (ssize_t)n) != ESP_OK) {
            httpd_resp_send_chunk(req, NULL, 0);
            return ESP_FAIL;
        }
        p   += n;
        len -= n;
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

// ============================================================
// GET /
// ============================================================
static esp_err_t handler_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Connection", "close");
    return resp_html_chunked(req, HTML_DASHBOARD, sizeof(HTML_DASHBOARD) - 1);
}

// ============================================================
// GET /api/status  → JSON
// ============================================================
static esp_err_t handler_api_status(httpd_req_t *req)
{
    sensor_data_t sd;
    sensor_manager_get(&sd);

    relay_status_t rs[RELAY_COUNT];
    relay_get_all_status(rs);

    controller_setpoints_t sp;
    controller_get_setpoints(&sp);

    cJSON *root = cJSON_CreateObject();

    // Sensor readings
    if (sd.temp_rh_valid) {
        cJSON_AddNumberToObject(root, "temperature_c", (double)sd.temperature_c);
        cJSON_AddNumberToObject(root, "humidity_pct",  (double)sd.humidity_pct);
    } else {
        cJSON_AddNullToObject(root, "temperature_c");
        cJSON_AddNullToObject(root, "humidity_pct");
    }
    cJSON_AddNumberToObject(root, "co2_ppm", sd.co2_valid ? sd.co2_ppm : 0);
    cJSON_AddBoolToObject(root, "sensors_healthy", sensor_manager_is_healthy());
    cJSON_AddBoolToObject(root, "level_low", sd.level_low);
    cJSON_AddBoolToObject(root, "door_open", controller_is_door_open());
    cJSON_AddNumberToObject(root, "light_brightness_pct", light_pwm_get_brightness(0));
    cJSON_AddNumberToObject(root, "light_colour_temp",    light_pwm_get_colour_temp());
    if (sd.panel_temp_valid)
        cJSON_AddNumberToObject(root, "panel_temp_c", (double)sd.panel_temp_c);
    else
        cJSON_AddNullToObject(root, "panel_temp_c");

    // Relay states + manual override flags
    bool manual[RELAY_COUNT];
    controller_get_manual_states(manual);
    cJSON *relays = cJSON_AddArrayToObject(root, "relays");
    for (int i = 0; i < RELAY_COUNT; i++) {
        cJSON *r = cJSON_CreateObject();
        cJSON_AddStringToObject(r, "name",   rs[i].name);
        cJSON_AddBoolToObject(r,   "state",  rs[i].state);
        cJSON_AddBoolToObject(r,   "locked", rs[i].locked_off);
        cJSON_AddBoolToObject(r,   "manual", manual[i]);
        cJSON_AddItemToArray(relays, r);
    }

    // Current setpoints
    cJSON *sps = cJSON_AddObjectToObject(root, "setpoints");
    cJSON_AddNumberToObject(sps, "temp_setpoint",      (double)sp.temp_setpoint);
    cJSON_AddNumberToObject(sps, "temp_hysteresis",    (double)sp.temp_hysteresis);
    cJSON_AddNumberToObject(sps, "hum_setpoint",       (double)sp.hum_setpoint);
    cJSON_AddNumberToObject(sps, "hum_hysteresis",     (double)sp.hum_hysteresis);
    cJSON_AddNumberToObject(sps, "co2_threshold",      sp.co2_threshold);
    cJSON_AddNumberToObject(sps, "co2_hysteresis",     sp.co2_hysteresis);
    cJSON_AddNumberToObject(sps, "fan_sched_on_min",   sp.fan_sched_on_min);
    cJSON_AddNumberToObject(sps, "fan_sched_period_min", sp.fan_sched_period_min);
    cJSON_AddBoolToObject(sps,   "fan_sched_enabled",  sp.fan_sched_enabled);
    cJSON_AddNumberToObject(sps, "light_on_hour",      sp.light_on_hour);
    cJSON_AddNumberToObject(sps, "light_on_min",       sp.light_on_min);
    cJSON_AddNumberToObject(sps, "light_off_hour",     sp.light_off_hour);
    cJSON_AddNumberToObject(sps, "light_off_min",      sp.light_off_min);
    cJSON_AddNumberToObject(sps, "light_sunrise_min",  sp.light_sunrise_min);
    cJSON_AddNumberToObject(sps, "light_sunset_min",   sp.light_sunset_min);
    cJSON_AddNumberToObject(sps, "panel_temp_max_c",   (double)sp.panel_temp_max_c);
    cJSON_AddNumberToObject(sps, "panel_temp_hyst_c",  (double)sp.panel_temp_hyst_c);

    cJSON_AddBoolToObject(root, "test_mode", controller_get_test_mode());
    cJSON_AddBoolToObject(root, "sd_mounted", sd_logger_is_mounted());
    cJSON_AddStringToObject(root, "sd_err", sd_logger_mount_error());
    cJSON_AddNumberToObject(root, "ring_count", sd_logger_recent_count());
    cJSON_AddNumberToObject(root, "last_log_t", (double)sd_logger_last_log_time());
    cJSON_AddNumberToObject(root, "ctrl_cycles", (double)g_ctrl_cycles);
    cJSON_AddBoolToObject(root, "door_pin_raw", controller_get_door_pin_raw());

    // WiFi / device identity (use cached copy — avoids NVS read on every poll)
    cJSON_AddStringToObject(root, "device_name", s_device_cfg.device_name);
    cJSON_AddStringToObject(root, "hostname",    s_device_cfg.hostname);
    cJSON_AddNumberToObject(root, "wifi_mode",   (int)wifi_manager_get_mode());
    char ip_str[16];
    wifi_manager_get_ip(ip_str, sizeof(ip_str));
    cJSON_AddStringToObject(root, "ip_address",  ip_str);

    // Current device time (HH:MM) so the web UI can show it
    time_t now_t = time(NULL);
    struct tm now_tm;
    localtime_r(&now_t, &now_tm);
    char dev_time[6];
    snprintf(dev_time, sizeof(dev_time), "%02d:%02d", now_tm.tm_hour, now_tm.tm_min);
    cJSON_AddStringToObject(root, "device_time", dev_time);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
    free(json_str);
    return ESP_OK;
}

// ============================================================
// POST /api/setpoints  ← JSON body
// ============================================================
static esp_err_t handler_api_setpoints(httpd_req_t *req)
{
    char buf[512] = {0};
    int  received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No body");
        return ESP_FAIL;
    }

    cJSON *json = cJSON_Parse(buf);
    if (!json) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    controller_setpoints_t sp;
    controller_get_setpoints(&sp);

#define JSON_GET_FLOAT(key, field) \
    { cJSON *item = cJSON_GetObjectItem(json, key); \
      if (item && cJSON_IsNumber(item)) sp.field = (float)item->valuedouble; }
#define JSON_GET_INT(key, field) \
    { cJSON *item = cJSON_GetObjectItem(json, key); \
      if (item && cJSON_IsNumber(item)) sp.field = (int)item->valueint; }

    JSON_GET_FLOAT("temp_setpoint",   temp_setpoint)
    JSON_GET_FLOAT("temp_hysteresis", temp_hysteresis)
    JSON_GET_FLOAT("hum_setpoint",    hum_setpoint)
    JSON_GET_FLOAT("hum_hysteresis",  hum_hysteresis)
    JSON_GET_INT("co2_threshold",       co2_threshold)
    JSON_GET_INT("co2_hysteresis",      co2_hysteresis)
    JSON_GET_INT("fan_sched_on_min",    fan_sched_on_min)
    JSON_GET_INT("fan_sched_period_min",fan_sched_period_min)
    { cJSON *item = cJSON_GetObjectItem(json, "fan_sched_enabled");
      if (item && cJSON_IsBool(item)) sp.fan_sched_enabled = cJSON_IsTrue(item); }
    JSON_GET_INT("light_on_hour",       light_on_hour)
    JSON_GET_INT("light_on_min",        light_on_min)
    JSON_GET_INT("light_off_hour",      light_off_hour)
    JSON_GET_INT("light_off_min",       light_off_min)
    JSON_GET_INT("light_colour_temp",   light_colour_temp)
    JSON_GET_INT("light_sunrise_min",   light_sunrise_min)
    JSON_GET_INT("light_sunset_min",    light_sunset_min)
    JSON_GET_FLOAT("panel_temp_max_c",  panel_temp_max_c)
    JSON_GET_FLOAT("panel_temp_hyst_c", panel_temp_hyst_c)

    cJSON_Delete(json);
    controller_set_setpoints(&sp);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

// ============================================================
// POST /api/relay  ← { "relay": 0-3, "on": true/false }
// ============================================================
static esp_err_t handler_api_relay(httpd_req_t *req)
{
    char buf[128] = {0};
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No body");
        return ESP_FAIL;
    }

    cJSON *json = cJSON_Parse(buf);
    if (!json) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *relay_j = cJSON_GetObjectItem(json, "relay");
    cJSON *on_j    = cJSON_GetObjectItem(json, "on");

    if (!cJSON_IsNumber(relay_j) || !cJSON_IsBool(on_j)) {
        cJSON_Delete(json);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing relay or on");
        return ESP_FAIL;
    }

    int  relay_id = relay_j->valueint;
    bool on       = cJSON_IsTrue(on_j);
    cJSON_Delete(json);

    if (relay_id < 0 || relay_id >= RELAY_COUNT) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid relay id");
        return ESP_FAIL;
    }

    // Only toggle if the requested state differs from the current state.
    // This lets the test page send explicit on/off without double-toggling.
    if (relay_get((relay_id_t)relay_id) != on)
        controller_set_manual_toggle((relay_id_t)relay_id);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

// ============================================================
// ============================================================
// GET /api/diag  → human-readable JSON showing controller decisions
// Open in browser: http://mushroom-motherload.local/api/diag
// ============================================================
static esp_err_t handler_api_diag(httpd_req_t *req)
{
    sensor_data_t sd;
    sensor_manager_get(&sd);
    float hum = sensor_manager_effective_humidity(&sd);

    controller_setpoints_t sp;
    controller_get_setpoints(&sp);

    relay_status_t rs[RELAY_COUNT];
    relay_get_all_status(rs);

    bool manual[RELAY_COUNT];
    controller_get_manual_states(manual);

    cJSON *root = cJSON_CreateObject();

    // ── Sensors ─────────────────────────────────────────────
    cJSON *sens = cJSON_AddObjectToObject(root, "sensors");
    cJSON_AddBoolToObject  (sens, "sht31_valid",     sd.temp_rh_valid);
    cJSON_AddNumberToObject(sens, "sht31_temp_c",    (double)sd.temperature_c);
    cJSON_AddNumberToObject(sens, "sht31_humidity",  (double)sd.humidity_pct);
    cJSON_AddBoolToObject  (sens, "scd41_valid",     sd.temp_rh2_valid);
    cJSON_AddNumberToObject(sens, "scd41_humidity",  (double)sd.humidity2_pct);
    cJSON_AddBoolToObject  (sens, "co2_valid",       sd.co2_valid);
    cJSON_AddNumberToObject(sens, "co2_ppm",         sd.co2_ppm);
    cJSON_AddNumberToObject(sens, "effective_rh",    (double)hum);
    cJSON_AddBoolToObject  (sens, "level_low",       sd.level_low);

    // ── Heater decision ─────────────────────────────────────
    {
        float lower = sp.temp_setpoint - sp.temp_hysteresis;
        float upper = sp.temp_setpoint + sp.temp_hysteresis;
        const char *why;
        if (!sd.temp_rh_valid)            why = "no sensor";
        else if (sd.temperature_c <= lower) why = "below lower threshold - SHOULD BE ON";
        else if (sd.temperature_c >= upper) why = "above upper threshold - SHOULD BE OFF";
        else                                why = "in dead band - holding current state";

        cJSON *h = cJSON_AddObjectToObject(root, "heater");
        cJSON_AddBoolToObject  (h, "state",           rs[RELAY_HEATER].state);
        cJSON_AddBoolToObject  (h, "manual",          manual[RELAY_HEATER]);
        cJSON_AddNumberToObject(h, "setpoint",        (double)sp.temp_setpoint);
        cJSON_AddNumberToObject(h, "hysteresis",      (double)sp.temp_hysteresis);
        cJSON_AddNumberToObject(h, "lower_threshold", (double)lower);
        cJSON_AddNumberToObject(h, "upper_threshold", (double)upper);
        cJSON_AddStringToObject(h, "decision",        why);
    }

    // ── Humidifier decision ──────────────────────────────────
    {
        float lower = sp.hum_setpoint - sp.hum_hysteresis;
        float upper = sp.hum_setpoint + sp.hum_hysteresis;
        const char *why;
        if (!sd.temp_rh_valid)    why = "no sensor";
        else if (rs[RELAY_HUMIDIFIER].locked_off) why = "locked off (water low)";
        else if (rs[RELAY_FAN].state)             why = "fan interlock - held off";
        else if (hum <= lower)    why = "below lower threshold - SHOULD BE ON";
        else if (hum >= upper)    why = "above upper threshold - SHOULD BE OFF";
        else                      why = "in dead band - holding current state";

        cJSON *h = cJSON_AddObjectToObject(root, "humidifier");
        cJSON_AddBoolToObject  (h, "state",           rs[RELAY_HUMIDIFIER].state);
        cJSON_AddBoolToObject  (h, "manual",          manual[RELAY_HUMIDIFIER]);
        cJSON_AddNumberToObject(h, "setpoint",        (double)sp.hum_setpoint);
        cJSON_AddNumberToObject(h, "hysteresis",      (double)sp.hum_hysteresis);
        cJSON_AddNumberToObject(h, "lower_threshold", (double)lower);
        cJSON_AddNumberToObject(h, "upper_threshold", (double)upper);
        cJSON_AddStringToObject(h, "decision",        why);
    }

    // ── Fan decision ─────────────────────────────────────────
    {
        const char *why;
        if (sd.co2_valid && sd.co2_ppm >= sp.co2_threshold)
            why = "CO2 above threshold - SHOULD BE ON";
        else if (sp.fan_sched_enabled)
            why = "CO2 ok - following timed schedule";
        else
            why = "CO2 ok - schedule disabled";

        cJSON *f = cJSON_AddObjectToObject(root, "fan");
        cJSON_AddBoolToObject  (f, "state",          rs[RELAY_FAN].state);
        cJSON_AddBoolToObject  (f, "manual",         manual[RELAY_FAN]);
        cJSON_AddBoolToObject  (f, "co2_valid",      sd.co2_valid);
        cJSON_AddNumberToObject(f, "co2_threshold",  sp.co2_threshold);
        cJSON_AddBoolToObject  (f, "sched_enabled",  sp.fan_sched_enabled);
        cJSON_AddStringToObject(f, "decision",       why);
    }

    // ── Light ────────────────────────────────────────────────
    cJSON *l = cJSON_AddObjectToObject(root, "light");
    cJSON_AddBoolToObject(l, "state",  rs[RELAY_LIGHT].state);
    cJSON_AddBoolToObject(l, "manual", manual[RELAY_LIGHT]);

    char *json_str = cJSON_Print(root);   // pretty-printed
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
    free(json_str);
    return ESP_OK;
}

// ── /api/light ─────────────────────────────────────────────
// POST { "pct": 0-100 }  — force light brightness, overrides schedule.
static esp_err_t handler_api_light(httpd_req_t *req)
{
    char buf[64] = {0};
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No body");
        return ESP_FAIL;
    }
    cJSON *json = cJSON_Parse(buf);
    if (!json) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }
    cJSON *pct_j = cJSON_GetObjectItem(json, "pct");
    if (!cJSON_IsNumber(pct_j)) {
        cJSON_Delete(json);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing pct");
        return ESP_FAIL;
    }
    int pct = pct_j->valueint;
    cJSON_Delete(json);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    controller_set_light_brightness((uint8_t)pct);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

// ── /logs ─────────────────────────────────────────────────
static esp_err_t handler_logs(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Connection", "close");
    return resp_html_chunked(req, HTML_LOGS, sizeof(HTML_LOGS) - 1);
}

// ── /test ──────────────────────────────────────────────────
static esp_err_t handler_test(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Connection", "close");
    return resp_html_chunked(req, HTML_TEST, sizeof(HTML_TEST) - 1);
}

// ── /api/reset ───────────────────────────────────────────────
static esp_err_t handler_api_reset(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_sendstr(req, "{\"ok\":true,\"message\":\"Rebooting...\"}");
    vTaskDelay(pdMS_TO_TICKS(300));  // allow response to flush before restart
    esp_restart();
    return ESP_OK;
}

// ── /api/wifi  GET ────────────────────────────────────────────
// Returns current WiFi config (passwords redacted)
static esp_err_t handler_api_wifi_get(httpd_req_t *req)
{
    wifi_config_nvs_t wcfg;
    wifi_manager_load(&wcfg);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "ssid",        wcfg.ssid);
    cJSON_AddStringToObject(root, "ssid2",       wcfg.ssid2);
    cJSON_AddStringToObject(root, "hostname",    wcfg.hostname);
    cJSON_AddStringToObject(root, "device_name", wcfg.device_name);
    cJSON_AddStringToObject(root, "timezone",    wcfg.timezone);
    // passwords deliberately omitted from GET response
    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_sendstr(req, s);
    free(s);
    return ESP_OK;
}

// ── /api/wifi  POST ───────────────────────────────────────────
// Accepts { ssid, password, ssid2, password2, hostname, device_name, timezone }
// Saves to NVS then reboots.
static esp_err_t handler_api_wifi_post(httpd_req_t *req)
{
    char buf[512] = {0};
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No body");
        return ESP_FAIL;
    }
    cJSON *json = cJSON_Parse(buf);
    if (!json) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    wifi_config_nvs_t wcfg;
    wifi_manager_load(&wcfg);  // start from current values

#define JSON_STR(key, dst, maxlen) \
    { cJSON *_j = cJSON_GetObjectItem(json, key); \
      if (_j && cJSON_IsString(_j) && _j->valuestring) \
          strlcpy((dst), _j->valuestring, (maxlen)); }
// Passwords: only overwrite if field is non-empty (blank = keep existing)
#define JSON_PASS(key, dst, maxlen) \
    { cJSON *_j = cJSON_GetObjectItem(json, key); \
      if (_j && cJSON_IsString(_j) && _j->valuestring && _j->valuestring[0]) \
          strlcpy((dst), _j->valuestring, (maxlen)); }

    JSON_STR("ssid",        wcfg.ssid,        sizeof(wcfg.ssid))
    JSON_PASS("password",   wcfg.pass,        sizeof(wcfg.pass))
    JSON_STR("ssid2",       wcfg.ssid2,       sizeof(wcfg.ssid2))
    JSON_PASS("password2",  wcfg.pass2,       sizeof(wcfg.pass2))
    JSON_STR("hostname",    wcfg.hostname,    sizeof(wcfg.hostname))
    JSON_STR("device_name", wcfg.device_name, sizeof(wcfg.device_name))
    JSON_STR("timezone",    wcfg.timezone,    sizeof(wcfg.timezone))
#undef JSON_STR
#undef JSON_PASS

    cJSON_Delete(json);

    // Sanitise hostname: lowercase alphanumeric + hyphens only
    for (char *p = wcfg.hostname; *p; p++) {
        if (*p >= 'A' && *p <= 'Z') { *p += 32; continue; }
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-'))
            *p = '-';
    }
    // Ensure hostname isn't empty
    if (wcfg.hostname[0] == '\0')
        strlcpy(wcfg.hostname, WIFI_HOSTNAME, sizeof(wcfg.hostname));

    wifi_manager_save(&wcfg);
    display_set_device_name(wcfg.device_name[0] ? wcfg.device_name : DEVICE_NAME_DEFAULT);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

// ── /api/notifier  GET — return current topic ─────────────────
static esp_err_t handler_api_notifier_get(httpd_req_t *req)
{
    char topic[NOTIFIER_TOPIC_MAX] = {0};
    notifier_get_topic(topic, sizeof(topic));
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "topic", topic);
    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_sendstr(req, s);
    free(s);
    return ESP_OK;
}

// ── /api/notifier  POST — save topic ─────────────────────────
static esp_err_t handler_api_notifier_post(httpd_req_t *req)
{
    char buf[128] = {0};
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No body");
        return ESP_FAIL;
    }
    cJSON *json = cJSON_Parse(buf);
    if (!json) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }
    cJSON *jtopic = cJSON_GetObjectItem(json, "topic");
    if (jtopic && cJSON_IsString(jtopic))
        notifier_set_topic(jtopic->valuestring);
    cJSON_Delete(json);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

// ── /api/notifier/test  POST — send test notification ───────
static esp_err_t handler_api_notifier_test(httpd_req_t *req)
{
    // Drain any request body.
    char drain[32];
    httpd_req_recv(req, drain, sizeof(drain));

    // Fire-and-forget: TLS POST runs in a one-shot background task so the
    // httpd task is never blocked (blocking here causes EAGAIN on all pages).
    notifier_send("Al Wall Controller",
                  "Test notification — your controller is connected!");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

// ── /api/alerts  GET — return current alert config ───────────
static esp_err_t handler_api_alerts_get(httpd_req_t *req)
{
    alert_cfg_t cfg;
    alert_manager_get_cfg(&cfg);

    cJSON *root = cJSON_CreateObject();

    cJSON *jt = cJSON_CreateObject();
    cJSON_AddBoolToObject(jt, "en", cfg.temp_en);
    cJSON_AddNumberToObject(jt, "lo", (double)cfg.temp_lo);
    cJSON_AddNumberToObject(jt, "hi", (double)cfg.temp_hi);
    cJSON_AddNumberToObject(jt, "rep", cfg.temp_rep);
    cJSON_AddBoolToObject(jt, "fail_en", cfg.temp_fail_en);
    cJSON_AddNumberToObject(jt, "fail_min", cfg.temp_fail_min);
    cJSON_AddNumberToObject(jt, "fail_rep", cfg.temp_fail_rep);
    cJSON_AddItemToObject(root, "temp", jt);

    cJSON *jh = cJSON_CreateObject();
    cJSON_AddBoolToObject(jh, "en", cfg.hum_en);
    cJSON_AddNumberToObject(jh, "lo", (double)cfg.hum_lo);
    cJSON_AddNumberToObject(jh, "hi", (double)cfg.hum_hi);
    cJSON_AddNumberToObject(jh, "rep", cfg.hum_rep);
    cJSON_AddBoolToObject(jh, "fail_en", cfg.hum_fail_en);
    cJSON_AddNumberToObject(jh, "fail_min", cfg.hum_fail_min);
    cJSON_AddNumberToObject(jh, "fail_rep", cfg.hum_fail_rep);
    cJSON_AddItemToObject(root, "hum", jh);

    cJSON *jc = cJSON_CreateObject();
    cJSON_AddBoolToObject(jc, "en", cfg.co2_en);
    cJSON_AddNumberToObject(jc, "lo", cfg.co2_lo);
    cJSON_AddNumberToObject(jc, "hi", cfg.co2_hi);
    cJSON_AddNumberToObject(jc, "rep", cfg.co2_rep);
    cJSON_AddBoolToObject(jc, "fail_en", cfg.co2_fail_en);
    cJSON_AddNumberToObject(jc, "fail_min", cfg.co2_fail_min);
    cJSON_AddNumberToObject(jc, "fail_rep", cfg.co2_fail_rep);
    cJSON_AddItemToObject(root, "co2", jc);

    cJSON *jw = cJSON_CreateObject();
    cJSON_AddBoolToObject(jw, "en", cfg.water_en);
    cJSON_AddNumberToObject(jw, "rep", cfg.water_rep);
    cJSON_AddItemToObject(root, "water", jw);

    cJSON *jd = cJSON_CreateObject();
    cJSON_AddBoolToObject(jd, "en", cfg.door_en);
    cJSON_AddNumberToObject(jd, "dur", cfg.door_dur);
    cJSON_AddNumberToObject(jd, "rep", cfg.door_rep);
    cJSON_AddItemToObject(root, "door", jd);

    cJSON *jsd = cJSON_CreateObject();
    cJSON_AddBoolToObject(jsd, "en", cfg.sd_en);
    cJSON_AddNumberToObject(jsd, "rep", cfg.sd_rep);
    cJSON_AddItemToObject(root, "sd", jsd);

    cJSON *jwf = cJSON_CreateObject();
    cJSON_AddBoolToObject(jwf, "en", cfg.wifi_en);
    cJSON_AddNumberToObject(jwf, "rep", cfg.wifi_rep);
    cJSON_AddItemToObject(root, "wifi", jwf);

    cJSON *jrb = cJSON_CreateObject();
    cJSON_AddBoolToObject(jrb, "en", cfg.reboot_en);
    cJSON_AddItemToObject(root, "reboot", jrb);

    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_sendstr(req, s);
    free(s);
    return ESP_OK;
}

// ── /api/alerts  POST — save alert config ──────────────────
static esp_err_t handler_api_alerts_post(httpd_req_t *req)
{
    char buf[1024] = {0};
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No body");
        return ESP_FAIL;
    }
    cJSON *json = cJSON_Parse(buf);
    if (!json) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    alert_cfg_t cfg;
    alert_manager_get_cfg(&cfg);  // start from current values

    cJSON *j, *sub;
#define AL_BOOL(parent, field, key) \
    j = cJSON_GetObjectItem(parent, key); \
    if (j) cfg.field = cJSON_IsTrue(j) ? 1 : 0
#define AL_FLOAT(parent, field, key) \
    j = cJSON_GetObjectItem(parent, key); \
    if (j && cJSON_IsNumber(j)) cfg.field = (float)j->valuedouble
#define AL_U16(parent, field, key) \
    j = cJSON_GetObjectItem(parent, key); \
    if (j && cJSON_IsNumber(j)) cfg.field = (uint16_t)j->valueint
#define AL_U32(parent, field, key) \
    j = cJSON_GetObjectItem(parent, key); \
    if (j && cJSON_IsNumber(j)) cfg.field = (uint32_t)j->valueint

    sub = cJSON_GetObjectItem(json, "temp");
    if (sub) { AL_BOOL(sub,temp_en,"en"); AL_FLOAT(sub,temp_lo,"lo"); AL_FLOAT(sub,temp_hi,"hi"); AL_U32(sub,temp_rep,"rep");
               AL_BOOL(sub,temp_fail_en,"fail_en"); AL_U32(sub,temp_fail_min,"fail_min"); AL_U32(sub,temp_fail_rep,"fail_rep"); }

    sub = cJSON_GetObjectItem(json, "hum");
    if (sub) { AL_BOOL(sub,hum_en,"en"); AL_FLOAT(sub,hum_lo,"lo"); AL_FLOAT(sub,hum_hi,"hi"); AL_U32(sub,hum_rep,"rep");
               AL_BOOL(sub,hum_fail_en,"fail_en"); AL_U32(sub,hum_fail_min,"fail_min"); AL_U32(sub,hum_fail_rep,"fail_rep"); }

    sub = cJSON_GetObjectItem(json, "co2");
    if (sub) { AL_BOOL(sub,co2_en,"en"); AL_U16(sub,co2_lo,"lo"); AL_U16(sub,co2_hi,"hi"); AL_U32(sub,co2_rep,"rep");
               AL_BOOL(sub,co2_fail_en,"fail_en"); AL_U32(sub,co2_fail_min,"fail_min"); AL_U32(sub,co2_fail_rep,"fail_rep"); }

    sub = cJSON_GetObjectItem(json, "water");
    if (sub) { AL_BOOL(sub,water_en,"en"); AL_U32(sub,water_rep,"rep"); }

    sub = cJSON_GetObjectItem(json, "door");
    if (sub) { AL_BOOL(sub,door_en,"en"); AL_U32(sub,door_dur,"dur"); AL_U32(sub,door_rep,"rep"); }

    sub = cJSON_GetObjectItem(json, "sd");
    if (sub) { AL_BOOL(sub,sd_en,"en"); AL_U32(sub,sd_rep,"rep"); }

    sub = cJSON_GetObjectItem(json, "wifi");
    if (sub) { AL_BOOL(sub,wifi_en,"en"); AL_U32(sub,wifi_rep,"rep"); }

    sub = cJSON_GetObjectItem(json, "reboot");
    if (sub) { AL_BOOL(sub,reboot_en,"en"); }

#undef AL_BOOL
#undef AL_FLOAT
#undef AL_U16
#undef AL_U32

    cJSON_Delete(json);
    alert_manager_set_cfg(&cfg);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

// ── /api/factory_reset  POST ──────────────────────────────────
static esp_err_t handler_api_factory_reset(httpd_req_t *req)
{
    // Erase both NVS namespaces
    wifi_manager_erase();
    nvs_handle_t h;
    if (nvs_open("ctrl", NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

// ── /api/testmode ─────────────────────────────────────────────
// POST { "enabled": true|false }
static esp_err_t handler_api_testmode(httpd_req_t *req)
{
    char buf[64] = {0};
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No body");
        return ESP_FAIL;
    }
    cJSON *json = cJSON_Parse(buf);
    if (!json) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }
    cJSON *en = cJSON_GetObjectItem(json, "enabled");
    if (!cJSON_IsBool(en)) {
        cJSON_Delete(json);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing enabled");
        return ESP_FAIL;
    }
    controller_set_test_mode(cJSON_IsTrue(en));
    cJSON_Delete(json);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

// ── /charts ─────────────────────────────────────────────────
static esp_err_t handler_charts(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_sendstr_chunk(req,
        "<!DOCTYPE html><html lang='en'><head>"
        "<meta charset='UTF-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>Charts \u2014 Al Wall Controller</title>"
        "<script src='https://cdn.jsdelivr.net/npm/chart.js@4.4.3/dist/chart.umd.min.js'></script>"
        "<style>"
        "*{box-sizing:border-box}"
        "body{font-family:Arial,sans-serif;background:#1a1a2e;color:#eee;margin:0;padding:14px}"
        "h1{color:#e94560;margin:0 0 2px;font-size:1.4em}"
        ".topbar{display:flex;justify-content:space-between;align-items:baseline;margin-bottom:12px}"
        ".topbar .right{font-size:12px;color:#888}"
        ".topbar .right a{color:#888;text-decoration:none;margin-right:10px}"
        ".controls{display:flex;align-items:flex-end;gap:12px;margin-bottom:14px;flex-wrap:wrap}"
        ".controls label{font-size:12px;color:#aaa}"
        "select,input[type=date]{background:#16213e;border:1px solid #555;border-radius:6px;"
            "color:#eee;padding:6px 10px;font-size:14px;cursor:pointer}"
        ".btn{background:#e94560;color:#fff;border:none;border-radius:6px;"
            "padding:7px 16px;font-size:13px;font-weight:bold;cursor:pointer}"
        ".btn:active{background:#c73652}"
        ".btn.sec{background:#16213e;border:1px solid #555;color:#eee}"
        ".chart-wrap{background:#16213e;border-radius:10px;padding:14px;margin-bottom:12px}"
        ".chart-wrap h3{margin:0 0 8px;font-size:.9em;color:#aaa;text-transform:uppercase;letter-spacing:.5px}"
        ".status{font-size:12px;color:#888;margin-bottom:10px;min-height:16px}"
        "</style></head><body>"
    );
    httpd_resp_sendstr_chunk(req,
        "<div class='topbar'>"
        "<h1>&#128200; Charts</h1>"
        "<div class='right'><a href='/'>&#127807; Dashboard</a><a href='/logs'>&#x2699; Settings</a></div>"
        "</div>"
        "<div class='controls'>"
        "<div><label>View</label><br>"
        "<select id='mode' onchange='modeChange()'>"
        "<option value='live'>Live \u2014 last hour</option>"
        "<option value='hist'>Historical \u2014 choose date</option>"
        "</select></div>"
        "<div id='date-wrap' style='display:none'><label>Date</label><br>"
        "<select id='date-sel' onchange='loadHist()'><option>Loading\u2026</option></select></div>"
        "<button class='btn sec' onclick='refresh()'>&#8635; Refresh</button>"
        "</div>"
        "<div class='status' id='status'>Loading&#8230;</div>"
        "<div class='chart-wrap'><h3>Temperature \u00b0C</h3><canvas id='ct' height='90'></canvas></div>"
        "<div class='chart-wrap'><h3>Humidity %RH</h3><canvas id='ch' height='90'></canvas></div>"
        "<div class='chart-wrap'><h3>CO&#8322; ppm</h3><canvas id='cc' height='90'></canvas></div>"
    );
    httpd_resp_sendstr_chunk(req,
        "<script>"
        "var charts={};"
        "function mkChart(id,color,yLabel){"
            "var ctx=document.getElementById(id).getContext('2d');"
            "return new Chart(ctx,{type:'line',data:{labels:[],datasets:[{data:[],"
                "borderColor:color,backgroundColor:color+'22',borderWidth:1.5,"
                "pointRadius:0,tension:0.2,fill:true}]},"
            "options:{animation:false,responsive:true,plugins:{legend:{display:false},"
                "tooltip:{callbacks:{label:function(c){return c.parsed.y!=null?c.parsed.y.toFixed(1)+' '+yLabel:'';}}}},"
            "scales:{x:{ticks:{color:'#aaa',maxTicksLimit:12,maxRotation:0},grid:{color:'#ffffff0e'}},"
                "y:{ticks:{color:'#aaa'},grid:{color:'#ffffff0e'}}}}});"
        "}"
        "function setChart(chart,labels,data){"
            "chart.data.labels=labels;"
            "chart.data.datasets[0].data=data;"
            "chart.update('none');"
        "}"
        "window.onload=function(){"
            "charts.t=mkChart('ct','#f97316','\u00b0C');"
            "charts.h=mkChart('ch','#0ea5e9','%');"
            "charts.c=mkChart('cc','#10b981','ppm');"
            "loadFiles();"
            "loadLive();"
        "};"
        "function modeChange(){"
            "var m=document.getElementById('mode').value;"
            "document.getElementById('date-wrap').style.display=m==='hist'?'block':'none';"
            "if(m==='live')loadLive();else loadHist();"
        "}"
        "function refresh(){"
            "var m=document.getElementById('mode').value;"
            "if(m==='live')loadLive();else loadHist();"
        "}"
        "async function loadFiles(){"
            "try{"
                "const r=await fetch('/api/log/files');"
                "const files=await r.json();"
                "var sel=document.getElementById('date-sel');"
                "sel.innerHTML='';"
                "if(!files.length){sel.innerHTML='<option>No files</option>';return;}"
                "files.slice().reverse().forEach(function(f){"
                    "var o=document.createElement('option');"
                    "o.value=f.name;o.textContent=f.name.replace('.csv','');"
                    "sel.appendChild(o);"
                "});"
            "}catch(e){console.warn(e);}"
        "}"
        "async function loadLive(){"
            "document.getElementById('status').textContent='Loading live data\u2026';"
            "try{"
                "const r=await fetch('/api/log/recent');"
                "const rows=await r.json();"
                "if(!rows.length){document.getElementById('status').textContent='No data yet';return;}"
                "var lbls=rows.map(function(e){return e.ts.replace('T',' ').substring(5,16);});"
                "setChart(charts.t,lbls,rows.map(function(e){return e.temp;}));"
                "setChart(charts.h,lbls,rows.map(function(e){return e.hum;}));"
                "setChart(charts.c,lbls,rows.map(function(e){return e.co2;}));"
                "var last=rows[rows.length-1];"
                "document.getElementById('status').textContent="
                    "'Live \u2014 '+rows.length+' readings \u00b7 last: '+last.ts.replace('T',' ');"
            "}catch(e){"
                "document.getElementById('status').textContent='Error: '+e;"
            "}"
        "}"
        "async function loadHist(){"
            "var name=document.getElementById('date-sel').value;"
            "if(!name||name==='No files')return;"
            "document.getElementById('status').textContent='Loading '+name+'\u2026';"
            "try{"
                "const r=await fetch('/api/log/file?name='+encodeURIComponent(name));"
                "const txt=await r.text();"
                "var lines=txt.trim().split('\\n');"
                "if(lines.length<2){document.getElementById('status').textContent='Empty file';return;}"
                "var hdr=lines[0].split(',');"
                "var ci={ts:hdr.indexOf('time'),t:hdr.indexOf('temp_c'),"
                        "h:hdr.indexOf('hum_pct'),c:hdr.indexOf('co2_ppm')};"
                "var lbls=[],td=[],hd=[],cd=[];"
                "for(var i=1;i<lines.length;i++){"
                    "var cols=lines[i].split(',');"
                    "if(cols.length<4)continue;"
                    "lbls.push(cols[ci.ts].replace('T',' ').substring(5,16));"
                    "td.push(parseFloat(cols[ci.t]));"
                    "hd.push(parseFloat(cols[ci.h]));"
                    "cd.push(parseFloat(cols[ci.c]));"
                "}"
                "setChart(charts.t,lbls,td);"
                "setChart(charts.h,lbls,hd);"
                "setChart(charts.c,lbls,cd);"
                "document.getElementById('status').textContent="
                    "name.replace('.csv','')+' \u2014 '+td.length+' readings';"
            "}catch(e){"
                "document.getElementById('status').textContent='Error: '+e;"
            "}"
        "}"
        "</script></body></html>"
    );
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

// ── /api/remount_sd ───────────────────────────────────────────
static esp_err_t handler_api_remount_sd(httpd_req_t *req)
{
    esp_err_t ret = sd_logger_remount();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Connection", "close");
    if (ret == ESP_OK) {
        httpd_resp_sendstr(req, "{\"ok\":true}");
    } else {
        char buf[80];
        snprintf(buf, sizeof(buf), "{\"ok\":false,\"err\":\"%s\"}", esp_err_to_name(ret));
        httpd_resp_sendstr(req, buf);
    }
    return ESP_OK;
}

// ── /api/log/recent ────────────────────────────────────────
static esp_err_t handler_api_log_recent(httpd_req_t *req)
{
    char *buf = heap_caps_malloc(64 * 1024, MALLOC_CAP_SPIRAM);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }
    sd_logger_recent_json(buf, 64 * 1024);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_sendstr(req, buf);
    free(buf);
    return ESP_OK;
}

// ── /api/log/ls ──────────────────────────────────────────────
static esp_err_t handler_api_log_ls(httpd_req_t *req)
{
    char *buf = malloc(4096);
    if (!buf) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM"); return ESP_FAIL; }
    sd_logger_ls_json(buf, 4096);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_sendstr(req, buf);
    free(buf);
    return ESP_OK;
}

// ── /api/log/files ────────────────────────────────────────
static esp_err_t handler_api_log_files(httpd_req_t *req)
{
    char buf[2048];
    sd_logger_files_json(buf, sizeof(buf));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

// ── /api/log/file?name=YYYY-MM-DD.csv ─────────────────────
static esp_err_t handler_api_log_file(httpd_req_t *req)
{
    char qstr[64] = {0};
    if (httpd_req_get_url_query_str(req, qstr, sizeof(qstr)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing query");
        return ESP_FAIL;
    }
    char name[32] = {0};
    if (httpd_query_key_value(qstr, "name", name, sizeof(name)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing name param");
        return ESP_FAIL;
    }
    // Security: only allow [A-Za-z0-9._-]; block path traversal
    for (int i = 0; name[i]; i++) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_')) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid name");
            return ESP_FAIL;
        }
    }
    if (strstr(name, "..")) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid name");
        return ESP_FAIL;
    }
    char fpath[80];
    snprintf(fpath, sizeof(fpath), SD_LOG_DIR "/%s", name);
    FILE *f = fopen(fpath, "r");
    if (!f) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
        return ESP_FAIL;
    }
    char disp[64];
    snprintf(disp, sizeof(disp), "attachment; filename=\"%s\"", name);
    httpd_resp_set_type(req, "text/csv");
    httpd_resp_set_hdr(req, "Content-Disposition", disp);
    httpd_resp_set_hdr(req, "Connection", "close");
    char chunk[2048];
    size_t n;
    esp_err_t send_err = ESP_OK;
    while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) {
        send_err = httpd_resp_send_chunk(req, chunk, (ssize_t)n);
        if (send_err != ESP_OK) break;
    }
    fclose(f);
    if (send_err == ESP_OK) {
        /* Normal end — send chunked-transfer terminator */
        httpd_resp_send_chunk(req, NULL, 0);
        return ESP_OK;
    }
    /* Client disconnected or send timed out mid-transfer. Do NOT send the
       terminator (connection is already broken) and return FAIL so the httpd
       server closes the socket cleanly rather than trying to reuse it. */
    return ESP_FAIL;
}

// ============================================================
// Start / stop
// ============================================================
esp_err_t web_server_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port        = WEB_SERVER_PORT;
    cfg.lru_purge_enable   = true;
    cfg.recv_wait_timeout  = 2;      // close idle keep-alive sockets faster
    cfg.send_wait_timeout  = 5;      // don't block >5 s waiting for TCP window
    cfg.max_uri_handlers   = 24;
    cfg.stack_size         = 16384; // handlers malloc 64KB JSON + cJSON + SD I/O
    cfg.uri_match_fn       = httpd_uri_match_wildcard;

    wifi_manager_load(&s_device_cfg);  // prime the device identity cache

    if (httpd_start(&s_server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return ESP_FAIL;
    }

    static const httpd_uri_t uri_root = {
        .uri = "/", .method = HTTP_GET, .handler = handler_root
    };
    static const httpd_uri_t uri_status = {
        .uri = "/api/status", .method = HTTP_GET, .handler = handler_api_status
    };
    static const httpd_uri_t uri_setpoints = {
        .uri = "/api/setpoints", .method = HTTP_POST, .handler = handler_api_setpoints
    };
    static const httpd_uri_t uri_relay = {
        .uri = "/api/relay", .method = HTTP_POST, .handler = handler_api_relay
    };
    static const httpd_uri_t uri_diag = {
        .uri = "/api/diag", .method = HTTP_GET, .handler = handler_api_diag
    };
    static const httpd_uri_t uri_reset = {
        .uri = "/api/reset", .method = HTTP_POST, .handler = handler_api_reset
    };
    static const httpd_uri_t uri_testmode = {
        .uri = "/api/testmode", .method = HTTP_POST, .handler = handler_api_testmode
    };
    static const httpd_uri_t uri_light = {
        .uri = "/api/light", .method = HTTP_POST, .handler = handler_api_light
    };
    static const httpd_uri_t uri_test = {
        .uri = "/test", .method = HTTP_GET, .handler = handler_test
    };
    static const httpd_uri_t uri_remount_sd = {
        .uri = "/api/remount_sd", .method = HTTP_POST, .handler = handler_api_remount_sd
    };
    static const httpd_uri_t uri_charts = {
        .uri = "/charts", .method = HTTP_GET, .handler = handler_charts
    };
    static const httpd_uri_t uri_logs = {
        .uri = "/logs", .method = HTTP_GET, .handler = handler_logs
    };
    static const httpd_uri_t uri_log_recent = {
        .uri = "/api/log/recent", .method = HTTP_GET, .handler = handler_api_log_recent
    };
    static const httpd_uri_t uri_log_files = {
        .uri = "/api/log/files", .method = HTTP_GET, .handler = handler_api_log_files
    };
    static const httpd_uri_t uri_log_ls = {
        .uri = "/api/log/ls", .method = HTTP_GET, .handler = handler_api_log_ls
    };
    static const httpd_uri_t uri_log_file = {
        .uri = "/api/log/file*", .method = HTTP_GET, .handler = handler_api_log_file
    };
    static const httpd_uri_t uri_wifi_get = {
        .uri = "/api/wifi", .method = HTTP_GET, .handler = handler_api_wifi_get
    };
    static const httpd_uri_t uri_wifi_post = {
        .uri = "/api/wifi", .method = HTTP_POST, .handler = handler_api_wifi_post
    };
    static const httpd_uri_t uri_factory_reset = {
        .uri = "/api/factory_reset", .method = HTTP_POST, .handler = handler_api_factory_reset
    };

    httpd_register_uri_handler(s_server, &uri_root);
    httpd_register_uri_handler(s_server, &uri_log_recent);
    httpd_register_uri_handler(s_server, &uri_log_files);
    httpd_register_uri_handler(s_server, &uri_log_ls);
    httpd_register_uri_handler(s_server, &uri_status);
    httpd_register_uri_handler(s_server, &uri_setpoints);
    httpd_register_uri_handler(s_server, &uri_relay);
    httpd_register_uri_handler(s_server, &uri_diag);
    httpd_register_uri_handler(s_server, &uri_reset);
    httpd_register_uri_handler(s_server, &uri_testmode);
    httpd_register_uri_handler(s_server, &uri_light);
    httpd_register_uri_handler(s_server, &uri_test);
    httpd_register_uri_handler(s_server, &uri_charts);
    httpd_register_uri_handler(s_server, &uri_remount_sd);
    httpd_register_uri_handler(s_server, &uri_logs);
    httpd_register_uri_handler(s_server, &uri_log_file);
    httpd_register_uri_handler(s_server, &uri_wifi_get);
    httpd_register_uri_handler(s_server, &uri_wifi_post);
    httpd_register_uri_handler(s_server, &uri_factory_reset);
    static const httpd_uri_t uri_notifier_get = {
        .uri = "/api/notifier", .method = HTTP_GET, .handler = handler_api_notifier_get
    };
    static const httpd_uri_t uri_notifier_post = {
        .uri = "/api/notifier", .method = HTTP_POST, .handler = handler_api_notifier_post
    };
    httpd_register_uri_handler(s_server, &uri_notifier_get);
    httpd_register_uri_handler(s_server, &uri_notifier_post);
    static const httpd_uri_t uri_notifier_test = {
        .uri = "/api/notifier/test", .method = HTTP_POST, .handler = handler_api_notifier_test
    };
    httpd_register_uri_handler(s_server, &uri_notifier_test);
    static const httpd_uri_t uri_alerts_get = {
        .uri = "/api/alerts", .method = HTTP_GET, .handler = handler_api_alerts_get
    };
    static const httpd_uri_t uri_alerts_post = {
        .uri = "/api/alerts", .method = HTTP_POST, .handler = handler_api_alerts_post
    };
    httpd_register_uri_handler(s_server, &uri_alerts_get);
    httpd_register_uri_handler(s_server, &uri_alerts_post);

    ESP_LOGI(TAG, "HTTP server started on port %d", WEB_SERVER_PORT);
    return ESP_OK;
}

void web_server_stop(void)
{
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }
}
